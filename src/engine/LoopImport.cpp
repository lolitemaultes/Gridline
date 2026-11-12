// Gridline - loop files into a project. See LoopImport.h.
#include "LoopImport.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "../core/MeterEdit.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "SlicexEngine.h"

namespace gl::loopimport {

namespace {

void setParam(PluginState& st, int k, double v) {
  if (k < 0) return;
  if (st.params.size() <= static_cast<size_t>(k)) st.params.resize(static_cast<size_t>(k) + 1, 0.0f);
  st.params[static_cast<size_t>(k)] = static_cast<float>(v);
}

// Joins the shortest slice into the one before it (the first into the next) until `max` remain.
int join(std::vector<loopfile::Slice>& v, size_t max) {
  int joined = 0;
  while (v.size() > max && v.size() > 1) {
    size_t k = 0;
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i].length < v[k].length) k = i;
    if (k == 0) {
      v[1].start = v[0].start;
      v[1].beat = v[0].beat;
      v[1].length += v[0].length;
      v[1].muted = v[0].muted && v[1].muted;
      v.erase(v.begin());
    } else {
      v[k - 1].length = v[k].start + v[k].length - v[k - 1].start;
      v[k - 1].muted = v[k - 1].muted && v[k].muted;
      v.erase(v.begin() + static_cast<std::ptrdiff_t>(k));
    }
    ++joined;
  }
  return joined;
}

}  // namespace

bool slicerFor(const std::string& path, const loopfile::Info& li, Sliced& out) {
  out = Sliced{};
  if (li.slices.empty() || li.frames <= 0) return false;
  out.slices = li.slices;
  out.joined = join(out.slices, kMaxSlicedKeys);
  const int n = static_cast<int>(out.slices.size());
  using namespace pp;
  PluginState st = defaultPluginState("gl.slicer");
  if (st.params.size() < static_cast<size_t>(SlCount)) st.params.resize(static_cast<size_t>(SlCount), 0.0f);
  // Keys from C5 up, lowered so every slice has one.
  const int base = std::min(60, 128 - n);
  out.keys.resize(static_cast<size_t>(n));
  for (int k = 0; k < n; ++k) out.keys[static_cast<size_t>(k)] = base + k;
  setParam(st, SlMode, SlModeManual);
  setParam(st, SlBaseNote, base);
  setParam(st, SlLoopBeats, std::clamp(li.beats * 4.0 / (li.sigDen > 0 ? li.sigDen : 4), 1.0, 64.0));
  // The slices play as recorded: no tempo fit, nothing held past the note.
  setParam(st, SlFitTempo, 0);
  const double frames = static_cast<double>(li.frames);
  for (int k = 0; k < kMaxSlices; ++k) setParam(st, slMarker(k), -1);
  for (int k = 0; k < n && k < kMaxSlices; ++k) {
    const loopfile::Slice& s = out.slices[static_cast<size_t>(k)];
    setParam(st, slMarker(k), static_cast<double>(s.start) / frames);
    setParam(st, slParam(k, SliceMute), s.muted ? 1 : 0);
    const int64_t next = k + 1 < n ? out.slices[static_cast<size_t>(k) + 1].start : li.frames;
    // A slice whose audio stops before the next one starts (a gated slice) ends there.
    if (s.length > 0 && s.start + s.length < next) setParam(st, slParam2(k, SliceEnd), static_cast<double>(s.start + s.length) / frames);
  }
  st.data = path;
  if (n > kMaxSlices) {
    // Past the 64 slice settings: the region engine, one region per slice on its own key.
    slicex::Model md = slicex::defaultModel();
    md.decks[0].regions.clear();
    for (int k = 0; k < n; ++k) {
      const loopfile::Slice& s = out.slices[static_cast<size_t>(k)];
      slicex::Region r;
      r.start = s.start;
      const int64_t next = k + 1 < n ? out.slices[static_cast<size_t>(k) + 1].start : li.frames;
      r.length = s.length > 0 && s.start + s.length < next ? s.length : -1;
      r.key = out.keys[static_cast<size_t>(k)];
      if (s.muted) r.rec[slicex::RVolume] = 0;
      md.decks[0].regions.push_back(r);
      if (k < kMaxSlices) setParam(st, slParam2(k, SliceKey), r.key);
    }
    st.data = path + "\n" + slicex::writeText(md);
    setParam(st, SlEngine, 1);
  }
  out.state = st;
  return true;
}

Tick beatTicks(const loopfile::Info& li, double beat, int ppq) {
  const double quarters = beat * 4.0 / (li.sigDen > 0 ? li.sigDen : 4);
  return static_cast<Tick>(std::llround(quarters * ppq));
}

Tick loopTicks(const loopfile::Info& li, int ppq) { return li.beats > 0 ? beatTicks(li, li.beats, ppq) : 0; }

std::vector<Note> sliceNotes(const Sliced& sl, const loopfile::Info& li, int ppq) {
  std::vector<Note> v;
  const Tick total = loopTicks(li, ppq);
  for (size_t k = 0; k < sl.slices.size(); ++k) {
    const loopfile::Slice& s = sl.slices[k];
    if (s.muted) continue;
    const Tick at = std::max<Tick>(0, beatTicks(li, s.beat, ppq));
    Tick end = k + 1 < sl.slices.size() ? beatTicks(li, sl.slices[k + 1].beat, ppq) : (total > 0 ? total : at + ppq);
    if (end <= at) end = at + 1;
    v.push_back(Note(at, end - at, sl.keys[k], 100));
  }
  return v;
}

Result addSlicedLoop(Song& s, const std::string& path, const loopfile::Info& li, int track, Tick start) {
  Result r;
  Sliced sl;
  if (!slicerFor(path, li, sl)) return r;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = std::filesystem::u8path(path).stem().u8string();
  ch.color = 0x8A9BA8;
  ch.plugin = sl.state;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Pattern p;
  p.id = s.allocateId();
  p.name = ch.name;
  p.color = 0x8A9BA8;
  p.notes[ch.id] = sliceNotes(sl, li, s.ppq);
  const Tick len = std::max<Tick>(1, loopTicks(li, s.ppq));
  if (track >= 0) {  // K-193: placed in the playlist: the meter of its section, as new patterns get
    int steps = 16;
    meter::newPatternMeter(s, std::max<Tick>(0, start), p.meterNum, p.meterDen, steps);
  }
  const Tick bar = std::max<Tick>(1, s.patternBarTicks(p));
  p.bars = static_cast<int>(std::clamp<Tick>((len + bar - 1) / bar, 1, kMaxPatternBars));
  s.patterns.push_back(p);
  r.channel = ch.id, r.pattern = p.id, r.slices = static_cast<int>(sl.slices.size()), r.joined = sl.joined;
  if (track >= 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = p.id;
    c.track = std::clamp(track, 0, kMaxPlaylistTracks - 1);
    c.start = std::max<Tick>(0, start);
    c.length = len;
    if (static_cast<int>(s.tracks.size()) <= c.track) s.tracks.resize(static_cast<size_t>(c.track) + 1);
    s.playlist.push_back(c);
    r.clip = c.id;
  }
  return r;
}

void followTempo(PluginState& sampler, double bpm) {
  if (!(bpm > 0)) return;
  setParam(sampler, pp::SmpFitTempo, std::clamp(bpm, 1.0, 1000.0));
  setParam(sampler, pp::SmpStretchMode, 1);  // stretch, the pitch kept
}

}  // namespace gl::loopimport
