// Gridline — MIDI file import. See MidiImport.h.
#include "MidiImport.h"
#include "Meter.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "NoteBend.h"
#include "Params.h"
#include "PatternAutomation.h"

namespace gl {

namespace {

constexpr Rgb kTrackColors[] = {0x5A8FD8, 0xD8775A, 0x6EB55E, 0xC9A23F, 0x9A6CD0, 0x4FB3B0, 0xD0607E, 0x8C9A5B};

int firstFreeTrack(const Song& s) {
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    bool used = false;
    for (const auto& c : s.playlist) used = used || c.track == t;
    if (!used) return t;
  }
  return 0;
}

std::string pluginOr(const std::string& type, const std::string& fallback) { return findPlugin(type) ? type : fallback; }

// K-25: pitch bend during a note becomes the note's bend line (in the file's bend range: RPN 0,
// General MIDI's 2 semitones until the file sets one). Overlapping notes on one MIDI channel all
// get the channel's bend, as they played it.
struct BendReader {
  std::vector<MidiCtl> bends[16];                          // per MIDI channel, by tick
  std::vector<std::pair<int64_t, double>> ranges[16];      // (tick, semitones) from RPN 0
  explicit BendReader(const MidiTrack& tr) {
    int msb[16], lsb[16];
    std::fill(std::begin(msb), std::end(msb), 127);
    std::fill(std::begin(lsb), std::end(lsb), 127);
    for (const auto& c : tr.ctls) {
      const int ch = c.channel & 15;
      if (c.controller == kMidiPitchBend) bends[ch].push_back(c);
      else if (c.controller == 101) msb[ch] = c.value;
      else if (c.controller == 100) lsb[ch] = c.value;
      else if (c.controller == 6 && msb[ch] == 0 && lsb[ch] == 0) ranges[ch].push_back({c.tick, static_cast<double>(std::max(1, c.value))});
    }
    for (auto& b : bends) std::stable_sort(b.begin(), b.end(), [](const MidiCtl& x, const MidiCtl& y) { return x.tick < y.tick; });
  }
  std::vector<BendPoint> lineFor(const MidiNote& mn, int division, int ppq) const {
    const int ch = mn.channel & 15;
    const auto& bs = bends[ch];
    if (bs.empty()) return {};
    double range = 2.0;
    for (const auto& r : ranges[ch])
      if (r.first <= mn.start) range = r.second;
    const double toSong = static_cast<double>(ppq) / std::max(1, division);
    auto semis = [&](int v) { return (static_cast<double>(std::clamp(v, 0, 16383)) - 8192.0) / 8192.0 * range; };
    std::vector<std::pair<double, double>> samples;
    double startValue = 0;
    auto it = std::upper_bound(bs.begin(), bs.end(), mn.start, [](int64_t t, const MidiCtl& c) { return t < c.tick; });
    if (it != bs.begin()) startValue = semis(std::prev(it)->value);
    samples.push_back({0.0, startValue});
    for (; it != bs.end() && it->tick < mn.start + mn.length; ++it) samples.push_back({static_cast<double>(it->tick - mn.start) * toSong, semis(it->value)});
    return bend::fromSamples(samples, 0.02, std::max(1.0, ppq / 8.0));  // denser than 1/32 note: a ramp
  }
};

}  // namespace

namespace {
// K-179: the imported pattern cut into one pattern per meter section, each in its section's meter
// with a clip at the section's start. Notes keep their song positions.
MidiImportResult& splitByMeter(Song& song, const Pattern& whole, Tick end, std::vector<Tick> starts, MidiImportResult& res) {
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  if (starts.empty() || starts.front() != 0) starts.insert(starts.begin(), 0);
  const MeterMap map(song);
  const int track = firstFreeTrack(song);
  if (static_cast<int>(song.tracks.size()) <= track) song.tracks.resize(static_cast<size_t>(track) + 1);
  for (size_t i = 0; i < starts.size(); ++i) {
    const Tick a = starts[i], b = i + 1 < starts.size() ? starts[i + 1] : std::max(end, a + 1);
    if (a >= end) break;
    if (static_cast<int>(song.patterns.size()) >= kMaxPatterns) break;
    Pattern q;
    q.id = song.allocateId();
    q.name = whole.name + (starts.size() > 1 ? " " + std::to_string(i + 1) : std::string());
    q.color = whole.color;
    Tick qEnd = 0;
    bool any = false;
    for (const auto& [ch, ns] : whole.notes) {
      std::vector<Note> out;
      for (Note n : ns)
        if (n.start >= a && n.start < b) {
          n.start -= a;
          qEnd = std::max(qEnd, n.start + n.length);
          out.push_back(n);
        }
      if (!out.empty()) q.notes[ch] = std::move(out), any = true;
    }
    for (const auto& lane : whole.automation) {
      std::vector<AutoPoint> pts;
      const AutoPoint* before = nullptr;
      for (const auto& pt : lane.points) {
        if (pt.pos < a) before = &pt;
        else if (pt.pos < b) pts.push_back(pt), pts.back().pos -= a;
      }
      if (before && (pts.empty() || pts.front().pos > 0)) {
        AutoPoint first = *before;
        first.pos = 0;
        pts.insert(pts.begin(), first);
      }
      if (!pts.empty()) setPatternLane(q, lane.owner, lane.index, std::move(pts)), any = true;
    }
    if (!any) continue;
    const MeterSeg& g = map.segAt(a);
    if (g.num != song.timeSigNum || g.den != song.timeSigDen) q.meterNum = g.num, q.meterDen = g.den;
    const Tick bar = std::max<Tick>(1, song.patternBarTicks(q));
    const Tick len = std::max(b - a, qEnd);
    q.bars = static_cast<int>(std::clamp<Tick>((len + bar - 1) / bar, 1, kMaxPatternBars));
    q.stepsPerBar = sixteenthStepsPerBar(song.patternMeterNum(q), song.patternMeterDen(q));
    song.patterns.push_back(q);
    if (res.pattern == kNoId) res.pattern = q.id;
    res.sections.push_back(q.id);
    Clip c;
    c.id = song.allocateId();
    c.pattern = q.id;
    c.track = track;
    c.start = a;
    if (i + 1 < starts.size()) c.length = b - a;
    song.playlist.push_back(c);
  }
  return res;
}
}  // namespace

MidiImportResult importMidiIntoSong(Song& song, const MidiFileData& midiIn, const std::string& name, const MidiImportOptions& o) {
  MidiImportResult res;
  MidiFileData midi = midiIn;
  if (midi.format == 0 || midi.tracks.size() == 1) splitTracksByChannel(midi);
  const int ppq = song.ppq;
  auto T = [&](int64_t t) { return midiToSongTicks(t, midi.division, ppq); };
  // ---- tempo / meter ----
  if (o.tempo) {
    auto desc = describeParam(song, songParam(SongParam::Tempo));
    const double first = midi.tempos.empty() ? midi.tempo : midi.tempos[0].bpm;
    if (desc && first > 0) song.tempo = desc->clamp(first);
    if (desc && midi.tempos.size() > 1) {
      AutomationClip a;
      a.id = song.allocateId();
      a.name = "Tempo (" + name + ")";
      a.owner = kSongId;
      a.index = static_cast<uint32_t>(SongParam::Tempo);
      for (const auto& t : midi.tempos) {
        AutoPoint p;
        p.pos = T(t.tick);
        p.value = static_cast<float>(desc->toNormalized(desc->clamp(t.bpm)));
        p.shape = AutoShape::Hold;
        if (!a.points.empty() && a.points.back().pos == p.pos)
          a.points.back() = p;
        else
          a.points.push_back(p);
      }
      res.tempoChanges = static_cast<int>(a.points.size()) - 1;
      song.automations.push_back(a);
      Clip c;
      c.id = song.allocateId();
      c.kind = ClipKind::Automation;
      c.source = a.id;
      c.track = firstFreeTrack(song);
      c.start = 0;
      c.length = a.points.back().pos + song.barTicks();
      song.playlist.push_back(c);
    }
  }
  std::vector<Tick> meterStarts;  // K-179: where the file's meter changes (song ticks)
  if (o.timeSignature && !midi.timeSigs.empty()) {
    const int num = std::clamp(midi.timeSigs[0].num, 1, 32), den = std::clamp(midi.timeSigs[0].den, 1, 32);
    for (auto& p : song.patterns)  // K-179: patterns already there keep their meter and length
      if (!p.ownMeter() && (p.meterNum != num || p.meterDen != den) && (song.timeSigNum != num || song.timeSigDen != den)) {
        p.meterNum = song.timeSigNum;
        p.meterDen = song.timeSigDen;
      }
    song.timeSigNum = num;
    song.timeSigDen = den;
    for (auto& p : song.patterns)
      if (p.meterNum == num && p.meterDen == den) p.meterNum = p.meterDen = 0;
    for (size_t k = 1; k < midi.timeSigs.size(); ++k) {
      Marker m;
      m.pos = T(midi.timeSigs[k].tick);
      m.timeSigNum = std::clamp(midi.timeSigs[k].num, 1, 32);
      m.timeSigDen = std::clamp(midi.timeSigs[k].den, 1, 32);
      song.markers.push_back(m);
      meterStarts.push_back(m.pos);
      ++res.meterChanges;
    }
  }
  if (o.markers)
    for (const auto& mk : midi.markers) {
      Marker m;
      m.pos = T(mk.tick);
      m.name = mk.text;
      song.markers.push_back(m);
    }
  std::stable_sort(song.markers.begin(), song.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });

  // ---- pattern + channels ----
  if (static_cast<int>(song.patterns.size()) >= kMaxPatterns) {
    res.error = "the project has the maximum number of patterns";
    return res;
  }
  Pattern pat;
  pat.id = song.allocateId();
  pat.name = name;
  pat.color = 0x6E9F4B;
  Tick maxEnd = 0;
  int n = 0;
  for (size_t ti = 0; ti < midi.tracks.size(); ++ti) {
    const MidiTrack& tr = midi.tracks[ti];
    if (!o.tracks.empty() && !o.tracks.count(static_cast<int>(ti))) continue;
    int mch = tr.channel >= 0 ? tr.channel : (tr.notes.empty() ? 0 : tr.notes[0].channel);
    if (!o.midiChannels.empty() && !o.midiChannels.count(mch)) continue;
    std::vector<Note> notes;
    const BendReader bends(tr);
    for (const auto& mn : tr.notes) {
      if (!o.midiChannels.empty() && !o.midiChannels.count(mn.channel)) continue;
      Note x(T(mn.start), std::max<Tick>(1, T(mn.start + mn.length) - T(mn.start)), std::clamp(mn.pitch, 0, 127),
             std::clamp(mn.velocity, 1, 127));
      x.bend = bends.lineFor(mn, midi.division, ppq);  // K-25
      notes.push_back(x);
      maxEnd = std::max(maxEnd, x.start + x.length);
    }
    // Controllers with a Gridline target: CC 7 → volume, CC 10 → pan.
    std::map<uint32_t, std::vector<std::pair<Tick, double>>> lanes;
    for (const auto& c : tr.ctls) {
      if (!o.midiChannels.empty() && !o.midiChannels.count(c.channel)) continue;
      if (o.controllers && c.controller == 7)
        lanes[static_cast<uint32_t>(ChannelParam::Volume)].push_back({T(c.tick), c.value / 127.0});
      else if (o.controllers && c.controller == 10)
        lanes[static_cast<uint32_t>(ChannelParam::Pan)].push_back({T(c.tick), std::clamp((c.value - 64) / 64.0, -1.0, 1.0)});
      else if (c.controller != kMidiPitchBend && c.controller != 101 && c.controller != 100 && c.controller != 6 && c.controller != 38)  // K-25: pitch bend (and its RPN 0 range) went into the notes' bend lines
        ++res.skippedControllers;
    }
    if (notes.empty() && lanes.empty()) continue;
    Channel ch;
    ch.id = song.allocateId();
    std::string nm = tr.name;
    if (nm.empty() && o.programNames && mch == 9) nm = "Drums";
    if (nm.empty() && o.programNames && tr.program >= 0) nm = gmProgramName(tr.program);
    if (nm.empty()) nm = name + " " + std::to_string(n + 1);
    ch.name = nm;
    ch.color = kTrackColors[static_cast<size_t>(n) % (sizeof kTrackColors / sizeof kTrackColors[0])];
    std::string type = o.instrument;
    if (mch == 9) type = pluginOr(o.drumInstrument, o.instrument);
    else if (tr.program >= 0 && tr.program < 8 && o.instrument == "gl.synth") type = pluginOr("gl.keys", o.instrument);
    ch.plugin = defaultPluginState(pluginOr(type, "gl.synth"));
    ch.mixerTrack = song.masterId();
    song.channels.push_back(ch);
    res.channels.push_back(ch.id);
    ++n;
    res.notes += static_cast<int>(notes.size());
    if (!notes.empty()) pat.notes[ch.id] = std::move(notes);
    for (auto& [index, pts] : lanes) {
      auto desc = describeParam(song, {ch.id, index});
      if (!desc) continue;
      std::vector<AutoPoint> points;
      for (const auto& [t, v] : pts) {
        AutoPoint p;
        p.pos = t;
        p.value = static_cast<float>(desc->toNormalized(desc->clamp(v)));
        p.shape = AutoShape::Hold;
        if (!points.empty() && points.back().pos == p.pos)
          points.back() = p;
        else
          points.push_back(p);
        maxEnd = std::max(maxEnd, t + 1);
      }
      setPatternLane(pat, ch.id, index, std::move(points));
      ++res.automationLanes;
    }
  }
  if (res.channels.empty()) {
    res.error = "the MIDI file has no notes on the selected tracks / channels";
    return res;
  }
  const Tick bar = std::max<Tick>(1, song.barTicks());
  pat.bars = static_cast<int>(std::max<Tick>(1, (maxEnd + bar - 1) / bar));
  pat.stepsPerBar = 16;
  if (o.splitAtMeterChanges && o.placeInPlaylist && !meterStarts.empty())
    return splitByMeter(song, pat, maxEnd, meterStarts, res);
  song.patterns.push_back(pat);
  res.pattern = pat.id;
  if (o.placeInPlaylist) {
    Clip c;
    c.id = song.allocateId();
    c.pattern = pat.id;
    c.track = firstFreeTrack(song);
    c.start = 0;
    if (static_cast<int>(song.tracks.size()) <= c.track) song.tracks.resize(static_cast<size_t>(c.track) + 1);
    song.playlist.push_back(c);
  }
  return res;
}

int insertMidiNotes(Song& song, ObjectId pattern, ObjectId channel, Tick at, const MidiFileData& midi, int track) {
  Pattern* p = song.pattern(pattern);
  if (!p || !song.channel(channel)) return 0;
  std::vector<Note>& dst = p->notes[channel];
  int added = 0;
  Tick maxEnd = 0;
  for (size_t ti = 0; ti < midi.tracks.size(); ++ti) {
    if (track >= 0 && static_cast<int>(ti) != track) continue;
    const BendReader bends(midi.tracks[ti]);
    for (const auto& mn : midi.tracks[ti].notes) {
      const Tick s = midiToSongTicks(mn.start, midi.division, song.ppq);
      Note x(at + s, std::max<Tick>(1, midiToSongTicks(mn.start + mn.length, midi.division, song.ppq) - s), std::clamp(mn.pitch, 0, 127),
             std::clamp(mn.velocity, 1, 127));
      x.bend = bends.lineFor(mn, midi.division, song.ppq);  // K-25
      dst.push_back(x);
      maxEnd = std::max(maxEnd, x.start + x.length);
      ++added;
    }
  }
  std::stable_sort(dst.begin(), dst.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  if (dst.empty()) p->notes.erase(channel);
  const Tick bar = std::max<Tick>(1, song.barTicks());
  p->bars = std::max(p->bars, static_cast<int>((maxEnd + bar - 1) / bar));
  return added;
}

}  // namespace gl
