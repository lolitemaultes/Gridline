// Gridline — render to sampled instrument (see SampledInstrument.h).
#include "SampledInstrument.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "../core/ChordTools.h"
#include "../core/Json.h"
#include "../core/Project.h"
#include "../core/WavWriter.h"
#include "../packs/samplers/dwave.h"
#include "Render.h"

namespace gl::sampled {

namespace fs = std::filesystem;

namespace {

constexpr int kTakesPerRender = 48;  // takes per offline render (bounded memory, progress, cancel)
constexpr double kGapSeconds = 0.25;  // silence between takes

}  // namespace

std::vector<int> sampledNotes(const Options& o) {
  std::vector<int> v;
  if (!o.notes.empty()) {
    for (int n : o.notes)
      if (n >= 0 && n <= 127) v.push_back(n);
  } else {
    const int lo = std::clamp(std::min(o.noteLo, o.noteHi), 0, 127), hi = std::clamp(std::max(o.noteLo, o.noteHi), 0, 127);
    const int step = std::clamp(o.step, 1, 24);
    for (int n = lo; n <= hi; n += step) v.push_back(n);
    if (v.back() != hi) v.push_back(hi);
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void keyRange(const std::vector<int>& roots, size_t i, int& lo, int& hi) {
  lo = 0, hi = 127;
  if (i >= roots.size()) return;
  if (i > 0) lo = (roots[i - 1] + roots[i]) / 2 + 1;
  if (i + 1 < roots.size()) hi = (roots[i] + roots[i + 1]) / 2;
}

void velocityLayer(int layers, int k, int& lo, int& hi) {
  layers = std::clamp(layers, 1, 8);
  k = std::clamp(k, 0, layers - 1);
  hi = static_cast<int>(std::lround(127.0 * (k + 1) / layers));
  lo = k == 0 ? 1 : static_cast<int>(std::lround(127.0 * k / layers)) + 1;
}

std::string noteLabel(int note) { return chord::noteName(((note % 12) + 12) % 12) + std::to_string(note / 12); }

Song sourceSong(const Song& song, ObjectId channel) {
  Song s = makeEmptySong();
  s.ppq = song.ppq;
  s.timeSigNum = 4, s.timeSigDen = 4;
  s.swing = 0, s.masterPitch = 0;
  const Channel* src = song.channel(channel);
  if (!src) return s;
  auto strip = [&](const Channel& c) {
    Channel x = c;
    x.id = s.allocateId();
    x.noteFx.clear();
    x.echo = ChannelEcho{};
    x.gain = 0.78f;  // unity on the fader law
    x.pan = 0, x.muted = false, x.pitch = 0, x.fine = 0;
    x.mixerTrack = s.masterId();
    x.porta = false;
    x.timeShift = 0, x.swingMix = 0;
    x.keyLo = 0, x.keyHi = 127;
    x.cutGroup = 0, x.cutBy = 0;
    x.auxOuts.clear();
    x.midiOutPort = -1, x.midiInPort = -1;
    x.layer.clear();
    x.group = -1;
    x.audioClip = false;
    return x;
  };
  s.channels.push_back(strip(*src));
  // A layer plays its children too: they come along (one level).
  for (ObjectId id : src->layer)
    if (const Channel* ch = song.channel(id); ch && id != channel) {
      Channel x = strip(*ch);
      s.channels[0].layer.push_back(x.id);
      s.channels.push_back(x);
    }
  s.channels[0].layerMode = src->layerMode;
  return s;
}

int64_t trimSilence(AudioBuffer& a, double thresholdDb) {
  const size_t n = a.frames;
  if (n == 0 || a.channels == 0) return 0;
  const float thr = static_cast<float>(std::pow(10.0, thresholdDb / 20.0));
  auto loud = [&](size_t i) {
    for (size_t c = 0; c < a.channels; ++c)
      if (std::fabs(a.at(c, i)) > thr) return true;
    return false;
  };
  size_t first = 0;
  while (first < n && !loud(first)) ++first;
  if (first >= n) {  // silent take: keep a short stretch of nothing
    a.resize(a.channels, std::min<size_t>(n, 64));
    return 0;
  }
  size_t last = n - 1;
  while (last > first && !loud(last)) --last;
  const size_t start = first > 16 ? first - 16 : 0;
  const size_t end = std::min(n, last + 1 + 256);
  AudioBuffer b(a.channels, end - start);
  for (size_t c = 0; c < a.channels; ++c) std::copy(a.plane(c) + start, a.plane(c) + end, b.plane(c));
  if (end < n) {  // fade into the cut
    const size_t f = std::min<size_t>(b.frames, 256);
    for (size_t c = 0; c < b.channels; ++c)
      for (size_t i = 0; i < f; ++i) b.at(c, b.frames - f + i) *= static_cast<float>(f - i) / static_cast<float>(f);
  }
  a = std::move(b);
  return static_cast<int64_t>(start);
}

bool findLoop(const AudioBuffer& a, int64_t from, int64_t to, int sampleRate, int& start, int& end) {
  const int64_t n = static_cast<int64_t>(a.frames);
  const int W = 256;
  from = std::max<int64_t>(from, W);
  to = std::min<int64_t>(to, n - W - 1);
  const int64_t minLen = std::max<int64_t>(64, sampleRate / 20);
  if (to - from < minLen + 2 || a.channels == 0) return false;
  auto mono = [&](int64_t i) {
    double v = 0;
    for (size_t c = 0; c < a.channels; ++c) v += a.at(c, static_cast<size_t>(i));
    return v / static_cast<double>(a.channels);
  };
  auto rising = [&](int64_t i) { return mono(i - 1) < 0 && mono(i) >= 0; };
  int64_t e = to;
  while (e > from + minLen && !rising(e)) --e;
  if (e <= from + minLen) return false;
  double energy = 0;
  for (int j = -W; j < W; ++j) energy += mono(e + j) * mono(e + j);
  if (energy < 1e-9) return false;
  std::vector<int64_t> cands;
  for (int64_t s = from; s <= e - minLen; ++s)
    if (rising(s)) cands.push_back(s);
  if (cands.empty()) return false;
  const size_t stride = std::max<size_t>(1, cands.size() / 1500);
  double best = 1e30;
  int64_t bestS = -1;
  for (size_t k = 0; k < cands.size(); k += stride) {
    const int64_t s = cands[k];
    double err = 0;
    for (int j = -W; j < W && err < best * energy; ++j) {
      const double d = mono(s + j) - mono(e + j);
      err += d * d;
    }
    err /= energy;
    if (err < best) best = err, bestS = s;
  }
  if (bestS < 0 || best > 0.05) return false;
  start = static_cast<int>(bestS);
  end = static_cast<int>(e);
  return true;
}

bool renderTakes(const Song& song, ObjectId channel, const Options& o, std::vector<Take>& out, std::string* err,
                 const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
  out.clear();
  if (!song.channel(channel)) {
    if (err) *err = "No such channel";
    return false;
  }
  const std::vector<int> notes = sampledNotes(o);
  if (notes.empty()) {
    if (err) *err = "No notes to sample";
    return false;
  }
  const int sr = std::clamp(o.sampleRate, 8000, 192000);
  Song src = sourceSong(song, channel);
  const ObjectId chId = src.channels[0].id;
  // Whole frames per tick (positions are exact) at a tempo near 125 BPM.
  const int fpt = std::max(1, static_cast<int>(std::lround(sr / 220.5)));
  src.tempo = sr * 60.0 / (static_cast<double>(src.ppq) * fpt);
  const Tick hold = std::max<Tick>(1, static_cast<Tick>(std::ceil(std::clamp(o.noteSeconds, 0.01, 60.0) * sr / fpt)));
  const Tick tail = static_cast<Tick>(std::ceil(std::clamp(o.tailSeconds, 0.0, 60.0) * sr / fpt));
  const Tick gap = static_cast<Tick>(std::ceil(kGapSeconds * sr / fpt));
  const Tick slot = hold + tail + gap;
  // Every take in order: note, layer, round robin.
  const int layers = std::clamp(o.velocityLayers, 1, 8), rrs = std::clamp(o.roundRobins, 1, 4);
  for (size_t i = 0; i < notes.size(); ++i)
    for (int l = 0; l < layers; ++l)
      for (int r = 0; r < rrs; ++r) {
        Take t;
        t.note = notes[i];
        t.roundRobin = r;
        keyRange(notes, i, t.keyLo, t.keyHi);
        velocityLayer(layers, l, t.velLo, t.velHi);
        t.velocity = t.velHi;
        out.push_back(std::move(t));
      }
  const ObjectId pat = src.patterns[0].id;
  const size_t total = out.size();
  for (size_t first = 0; first < total; first += kTakesPerRender) {
    if (cancel && cancel->load()) {
      if (err) *err = "Cancelled";
      out.clear();
      return false;
    }
    const size_t count = std::min<size_t>(kTakesPerRender, total - first);
    Song s = src;
    Pattern& p = *s.pattern(pat);
    std::vector<Note> ns;
    for (size_t k = 0; k < count; ++k) {
      const Take& t = out[first + k];
      ns.push_back(Note(static_cast<Tick>(k) * slot, hold, t.note, t.velocity));
    }
    p.notes[chId] = ns;
    const Tick len = static_cast<Tick>(count) * slot;
    p.bars = static_cast<int>(std::min<Tick>(kMaxPatternBars, (len + s.barTicks() - 1) / s.barTicks()));
    RenderOptions ro;
    ro.sampleRate = sr;
    ro.patternMode = true;
    ro.pattern = pat;
    ro.tail = 0;
    AudioBuffer buf = renderSong(s, ro);
    for (size_t k = 0; k < count; ++k) {
      Take& t = out[first + k];
      const size_t a = static_cast<size_t>(static_cast<Tick>(k) * slot * fpt);
      const size_t frames = static_cast<size_t>((hold + tail) * fpt);
      t.audio.resize(2, frames);
      for (size_t c = 0; c < 2; ++c) {
        const size_t sc = std::min<size_t>(c, buf.channels ? buf.channels - 1 : 0);
        for (size_t i = 0; i < frames && a + i < buf.frames; ++i) t.audio.at(c, i) = buf.at(sc, a + i);
      }
      t.holdFrames = hold * fpt;
      if (o.autoTrim) {
        t.trimmedStart = trimSilence(t.audio, o.trimDb);
        t.holdFrames = std::max<int64_t>(0, t.holdFrames - t.trimmedStart);
      }
      if (o.findLoops) {
        int ls = 0, le = 0;
        const int64_t h = std::min<int64_t>(t.holdFrames, static_cast<int64_t>(t.audio.frames));
        if (findLoop(t.audio, h * 3 / 10, h - sr / 50, sr, ls, le)) t.loopStart = ls, t.loopEnd = le;
      }
    }
    if (progress) progress(static_cast<double>(first + count) / static_cast<double>(total));
  }
  return true;
}

PluginState programState(const std::vector<Take>& takes, const Options& o) {
  using namespace packs::samplers::dw;
  DwProgram p;
  p.name = o.name;
  p.cc7 = 1.f;  // full channel volume: a zone at its own velocity plays the take as rendered
  const int rrs = std::clamp(o.roundRobins, 1, 4);
  if (rrs > 1) p.trig[1].type = 1;  // round robins: one take per note, in turn
  const double rel = std::clamp(o.tailSeconds, 0.02, 10.0);
  const float relKnob = static_cast<float>(std::pow((rel - 0.001) / 10.0, 0.25));
  for (const Take& t : takes) {
    DwZone z;
    z.root = t.note;
    z.lo = t.keyLo, z.hi = t.keyHi;
    z.vlo = t.velLo, z.vhi = t.velHi;
    z.trigGroup = rrs > 1 ? 1 : 0;
    // Velocity scales the voice (v / 127) and the centre pan halves each side:
    // the zone gain undoes both at the rendered velocity.
    z.gain = static_cast<float>(2.0 * 127.0 / std::max(1, t.velocity));
    z.pan = 0.5f;
    z.name = fs::path(t.file).stem().string();
    z.path = t.file;
    z.rate = static_cast<float>(o.sampleRate);
    if (t.loopEnd > t.loopStart) {
      z.loopMode = 3;  // sustained: loops while held, then plays on into the recorded release
      z.loopStart = t.loopStart, z.loopEnd = t.loopEnd;
    } else {
      z.loopMode = 0;
    }
    z.amp[0] = 0.f, z.amp[1] = 0.5f, z.amp[2] = 1.f, z.amp[3] = relKnob;
    p.zones.push_back(z);
  }
  PluginState st = defaultPluginState("gl.waveprogram");
  st.data = formatProgram(p);
  return st;
}

ObjectId applyToProject(Document& d, ObjectId channel, const PluginState& st, bool replace, const std::string& name) {
  const Channel* src = d.song().channel(channel);
  if (!src) return kNoId;
  if (replace) {
    d.editChannel(channel, "render to sampled instrument", [&](Channel& c) { c.plugin = st; });
    return channel;
  }
  const Channel copy = *src;
  const int index = d.song().channelIndex(channel);
  d.beginGroup("render to sampled instrument");
  const ObjectId id = d.addChannel(name.empty() ? copy.name + " (sampled)" : name, st.type, index + 1);
  d.editChannel(id, "", [&](Channel& c) {
    c.plugin = st;
    c.color = copy.color;
    c.gain = copy.gain;
    c.pan = copy.pan;
    c.pitch = copy.pitch;
    c.fine = copy.fine;
    c.mixerTrack = copy.mixerTrack;
    c.group = copy.group;
  });
  d.endGroup();
  return id;
}

std::string freshFolder(const std::string& parent, const std::string& name) {
  std::string base;
  for (char c : name) base += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) ? '-' : c;
  if (base.empty()) base = "Sampled";
  std::error_code ec;
  fs::path p = fs::path(parent) / base;
  for (int k = 2; fs::exists(p, ec) && k < 10000; ++k) p = fs::path(parent) / (base + " " + std::to_string(k));
  return p.string();
}

Result renderToSampler(const Song& song, ObjectId channel, const Options& o, const std::string& dir,
                       const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
  Result r;
  r.dir = dir;
  std::string err;
  if (!renderTakes(song, channel, o, r.takes, &err, [&](double f) { if (progress) progress(f * 0.95); }, cancel)) {
    r.cancelled = cancel && cancel->load();
    r.error = err;
    return r;
  }
  float peak = 0;
  for (const Take& t : r.takes) peak = std::max(peak, t.audio.peak());
  if (peak < 1e-5f) {
    r.error = "The instrument made no sound";
    return r;
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    r.error = "Could not create " + dir + ": " + ec.message();
    return r;
  }
  std::string stem;
  for (char c : o.name) stem += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) ? '-' : c;
  if (stem.empty()) stem = "Sampled";
  for (Take& t : r.takes) {
    std::string f = stem + "_" + noteLabel(t.note) + "_v" + std::to_string(t.velocity);
    if (std::clamp(o.roundRobins, 1, 4) > 1) f += "_rr" + std::to_string(t.roundRobin + 1);
    t.file = (fs::path(dir) / (f + ".wav")).string();
    if (!writeWav(t.file, t.audio, o.sampleRate, o.bits == 16 ? 16 : 24)) {
      r.error = "Could not write " + t.file;
      return r;
    }
  }
  r.state = programState(r.takes, o);
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-preset");
  j.object["type"] = Json::makeString(r.state.type);
  Json ps = Json::makeArray();
  for (float v : r.state.params) ps.array.push_back(Json::makeNumber(v));
  j.object["params"] = std::move(ps);
  j.object["data"] = Json::makeString(r.state.data);
  if (const Channel* c = song.channel(channel)) j.object["source"] = Json::makeString("Rendered from " + c->name);
  r.presetFile = (fs::path(dir) / (stem + ".json")).string();
  if (!writeFileAtomic(r.presetFile, jsonStringify(j) + "\n", &err)) r.error = err;
  if (progress) progress(1.0);
  return r;
}

}  // namespace gl::sampled
