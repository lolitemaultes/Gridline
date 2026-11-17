// Gridline - "tracker" pack (K-120): the Tracker Sampler's instrument edits and built-in sounds.
// UI thread only (the audio thread reads what instrumentFor builds from them).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include "../../core/PluginInfo.h"
#include "../../core/PluginParams.h"
#include "../../engine/Sample.h"
#include "tracker.h"
#include "tracker_session.h"

namespace gl::packs::trackerpack {

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMiddleC = 261.6255653005986;

std::string oneLine(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  std::replace(s.begin(), s.end(), '\r', ' ');
  return s;
}

// One or more cycles summed from harmonics, scaled to a peak of `peak`.
void normalise(std::vector<float>& v, float peak) {
  float m = 0;
  for (float x : v) m = std::max(m, std::abs(x));
  if (m > 0)
    for (float& x : v) x *= peak / m;
}

std::vector<float> additive(int frames, int cycles, int harmonics, const std::function<double(int)>& amp) {
  std::vector<float> v(static_cast<size_t>(frames), 0.0f);
  for (int h = 1; h <= harmonics; ++h) {
    const double a = amp(h);
    if (a == 0) continue;
    const double w = 2.0 * kPi * h * cycles / frames;
    for (int i = 0; i < frames; ++i) v[static_cast<size_t>(i)] += static_cast<float>(a * std::sin(w * i));
  }
  return v;
}

trk::Envelope env(std::initializer_list<std::pair<int, int>> pts, int sustain) {
  trk::Envelope e;
  int i = 0;
  for (const auto& [x, y] : pts) {
    if (i >= trk::kEnvPoints) break;
    e.x[static_cast<size_t>(i)] = static_cast<int16_t>(x);
    e.y[static_cast<size_t>(i)] = static_cast<int16_t>(y);
    ++i;
  }
  e.count = static_cast<uint8_t>(i);
  e.flags = trk::kEnvOn;
  if (sustain >= 0) {
    e.flags |= trk::kEnvSustain;
    e.sustain = static_cast<uint8_t>(sustain);
  }
  return e;
}

// A looped sample: `frames` hold `cycles` periods of middle C.
void loopedSample(BuiltinSound& s, std::vector<float> v, double cycles) {
  normalise(v, 0.7f);
  s.frames = std::move(v);
  const auto n = static_cast<int32_t>(s.frames.size());
  s.sample.length = n;
  s.sample.loopStart = 0;
  s.sample.loopLength = n;
  s.sample.loop = trk::kLoopForward;
  tuneForRate(s.sample, kMiddleC * n / cycles);
}

BuiltinSound makeSound(int k) {
  BuiltinSound s;
  s.def = trk::InstrumentDef{};
  switch (k) {
    case 1: {  // a narrow pulse with a slow vibrato
      const double duty = 0.3;
      loopedSample(s, additive(128, 1, 40, [&](int h) { return std::sin(h * kPi * duty) / h * (1.0 - h / 44.0); }), 1);
      s.def.vol = env({{0, 64}, {6, 56}, {16, 48}, {40, 0}}, 2);
      s.def.vibType = 0;
      s.def.vibSweep = 48;
      s.def.vibDepth = 3;
      s.def.vibRate = 10;
      break;
    }
    case 2: {  // two saws a cycle apart over the loop: a slow chorus
      std::vector<float> a = additive(32768, 128, 60, [](int h) { return 1.0 / h * (1.0 - h / 64.0); });
      std::vector<float> b = additive(32768, 129, 60, [](int h) { return 1.0 / h * (1.0 - h / 64.0); });
      for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
      loopedSample(s, std::move(a), 128.5);
      s.def.vol = env({{0, 36}, {3, 64}, {10, 56}, {30, 0}}, 2);
      break;
    }
    case 3: {  // a struck tine: two modulated partials dying away, no loop
      const double rate = 44100.0;
      const int n = static_cast<int>(rate * 1.6);
      std::vector<float> v(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        const double t = i / rate;
        const double index = 2.4 * std::exp(-t / 0.18);
        const double body = std::sin(2 * kPi * kMiddleC * t + index * std::sin(2 * kPi * kMiddleC * 3.5 * t)) * std::exp(-t / 0.55);
        const double shine = 0.25 * std::sin(2 * kPi * kMiddleC * 4.0 * t) * std::exp(-t / 0.12);
        const double attack = std::min(1.0, t / 0.002);
        const double tail = i > n - 2000 ? (n - i) / 2000.0 : 1.0;
        v[static_cast<size_t>(i)] = static_cast<float>((body + shine) * attack * tail);
      }
      normalise(v, 0.7f);
      s.frames = std::move(v);
      s.sample.length = n;
      tuneForRate(s.sample, rate);
      s.def.vol = env({{0, 64}, {24, 0}}, 0);
      break;
    }
    case 4: {  // a round low wave: odd harmonics falling fast
      loopedSample(s, additive(128, 1, 14, [](int h) { return (h % 2 ? 1.0 : 0.35) / std::pow(h, 1.6); }), 1);
      s.def.vol = env({{0, 64}, {4, 52}, {12, 44}, {20, 0}}, 2);
      break;
    }
    case 5: {  // a hollow square with a slow swell
      loopedSample(s, additive(128, 1, 31, [](int h) { return h % 2 ? 1.0 / h * (1.0 - h / 36.0) : 0.0; }), 1);
      s.def.vol = env({{0, 0}, {14, 64}, {28, 54}, {70, 0}}, 2);
      s.def.vibType = 0;
      s.def.vibSweep = 96;
      s.def.vibDepth = 2;
      s.def.vibRate = 6;
      break;
    }
    default: break;
  }
  s.def.keymap.fill(0);
  s.def.samples[0] = s.sample;
  return s;
}

}  // namespace

const char* soundName(int sound) {
  switch (sound) {
    case 1: return "Glass Pulse";
    case 2: return "Velvet Saw";
    case 3: return "Tine Bell";
    case 4: return "Rubber Bass";
    case 5: return "Square Pad";
    default: return "None";
  }
}

const BuiltinSound* builtinSound(int sound) {
  if (sound < 1 || sound >= kSoundCount + 1) return nullptr;
  static std::mutex mu;
  static std::array<std::unique_ptr<BuiltinSound>, kSoundCount + 1> made;
  std::lock_guard<std::mutex> lk(mu);
  auto& p = made[static_cast<size_t>(sound)];
  if (!p) p = std::make_unique<BuiltinSound>(makeSound(sound));
  return p.get();
}

trk::InstrumentDef defaultDefinition() {
  trk::InstrumentDef d;
  d.vol = env({{0, 64}, {6, 0}}, 0);
  return d;
}

void tuneForRate(trk::SampleDef& s, double rate) {
  // The same rounding the instrument uses for a bare sample file.
  const double semis = 12.0 * std::log2(std::max(1.0, rate) / 8363.0);
  const int whole = static_cast<int>(std::floor(semis));
  s.relNote = static_cast<int8_t>(std::clamp(whole, -48, 71));
  s.finetune = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround((semis - whole) * 128.0)), -128, 127));
}

int rootNote(const trk::SampleDef& s, double rate) {
  const double semis = 12.0 * std::log2(std::max(1.0, rate) / 8363.0);
  return std::clamp(static_cast<int>(std::lround(49.0 + semis - s.relNote - s.finetune / 128.0)), 1, 96);
}

void setRootNote(trk::SampleDef& s, double rate, int note) {
  const double semis = 12.0 * std::log2(std::max(1.0, rate) / 8363.0);
  s.relNote = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(semis - (note - 49) - s.finetune / 128.0)), -48, 71));
}

Edit readEdit(const std::string& data, int sound) {
  Edit e;
  e.body = trk::decodeInstrument(data, e.text);
  if (!e.body) {
    // No instrument part: a bare sample file (on every key at its own rate) or nothing.
    const std::string first = data.substr(0, data.find('\n'));
    e.text = trk::InstrumentText{};
    const BuiltinSound* b = builtinSound(sound);
    e.text.def = b ? b->def : first.empty() ? defaultDefinition() : trk::InstrumentDef{};
    e.text.def.samples = {};  // no samples (fill() here trips a false GCC 12 -Wstringop-overflow)
    if (!first.empty()) {
      e.text.files.push_back(first);
      e.text.fileOf[0] = 0;
      if (SamplePtr sp = sampleCache().get(first); sp && sp->frames() > 0) {
        trk::SampleDef& sd = e.text.def.samples[0];
        sd.length = static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF));
        tuneForRate(sd, sp->sampleRate);
      }
    }
    return e;
  }
  std::istringstream lines(data);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("cut ", 0) == 0) {
      std::istringstream in(line.substr(4));
      long k = -1, a = 0, b = 0;
      if (!(in >> k >> a >> b) || k < 0 || k >= trk::kMaxSamples) continue;
      e.cuts.start[static_cast<size_t>(k)] = static_cast<int32_t>(std::clamp(a, 0L, 0x3FFFFFFFL));
      e.cuts.end[static_cast<size_t>(k)] = static_cast<int32_t>(std::clamp(b, 0L, 0x3FFFFFFFL));
    } else if (line.rfind("midi ", 0) == 0) {  // K-129
      std::istringstream in(line.substr(5));
      long on = 0, ch = 0, prg = 0, bend = 2, mute = 0;
      in >> on >> ch >> prg >> bend >> mute;
      e.midi.on = on != 0;
      e.midi.channel = static_cast<int>(std::clamp(ch, 0L, 15L));
      e.midi.program = static_cast<int>(std::clamp(prg, 0L, 127L));
      e.midi.bend = static_cast<int>(std::clamp(bend, 0L, 36L));
      e.midi.mute = mute != 0;
    } else if (line.rfind("bits ", 0) == 0) {  // K-129
      std::istringstream in(line.substr(5));
      long k = -1, bits = 16;
      if (!(in >> k >> bits) || k < 0 || k >= trk::kMaxSamples) continue;
      e.eightBit[static_cast<size_t>(k)] = bits == 8;
    }
  }
  e.text.def.mute = e.midi.on && e.midi.mute;  // R11-07: the own sound is muted only while notes go to MIDI
  return e;
}

std::string writeEdit(const Edit& e) {
  trk::InstrumentText t = e.text;
  t.name = oneLine(t.name);
  for (auto& n : t.sampleNames) n = oneLine(n);
  std::string o = trk::encodeInstrument(t);
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const auto i = static_cast<size_t>(k);
    if (e.cuts.start[i] == 0 && e.cuts.end[i] == 0) continue;
    if (!t.def.samples[i].present() && t.fileOf[i] < 0) continue;
    o += "cut " + std::to_string(k) + " " + std::to_string(e.cuts.start[i]) + " " + std::to_string(e.cuts.end[i]) + "\n";
  }
  // K-129: written only when they differ from the defaults (texts without them read the same).
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const auto i = static_cast<size_t>(k);
    if (e.eightBit[i] && (t.def.samples[i].present() || t.fileOf[i] >= 0)) o += "bits " + std::to_string(k) + " 8\n";
  }
  if (!e.midi.isDefault())
    o += "midi " + std::to_string(e.midi.on ? 1 : 0) + " " + std::to_string(e.midi.channel) + " " + std::to_string(e.midi.program) + " " +
         std::to_string(e.midi.bend) + " " + std::to_string(e.midi.mute ? 1 : 0) + "\n";
  return o;
}

void compactFiles(Edit& e) {
  std::vector<std::string> files;
  std::map<std::string, int> index;
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    int& f = e.text.fileOf[static_cast<size_t>(k)];
    if (f < 0 || f >= static_cast<int>(e.text.files.size()) || e.text.files[static_cast<size_t>(f)].empty()) {
      f = -1;
      continue;
    }
    const std::string& p = e.text.files[static_cast<size_t>(f)];
    auto it = index.find(p);
    if (it == index.end()) {
      it = index.emplace(p, static_cast<int>(files.size())).first;
      files.push_back(p);
    }
    f = it->second;
  }
  e.text.files = std::move(files);
}

int addSample(Edit& e, const std::string& path, int32_t frames, double rate, const std::string& name) {
  int slot = -1;
  bool any = false;
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const bool used = e.text.def.samples[static_cast<size_t>(k)].present() || e.text.fileOf[static_cast<size_t>(k)] >= 0;
    any = any || used;
    if (!used && slot < 0) slot = k;
  }
  if (slot < 0 || path.empty() || frames < 1) return -1;
  const auto i = static_cast<size_t>(slot);
  trk::SampleDef s;
  s.length = frames;
  tuneForRate(s, rate);
  e.text.def.samples[i] = s;
  e.text.fileOf[i] = static_cast<int>(e.text.files.size());
  e.text.files.push_back(path);
  e.text.sampleNames[i] = oneLine(name);
  e.cuts.start[i] = e.cuts.end[i] = 0;
  e.eightBit[i] = false;
  if (!any) e.text.def.keymap.fill(static_cast<uint8_t>(slot));
  compactFiles(e);
  return slot;
}

void removeSample(Edit& e, int slot) {
  if (slot < 0 || slot >= trk::kMaxSamples) return;
  const auto i = static_cast<size_t>(slot);
  e.text.def.samples[i] = trk::SampleDef{};
  e.text.def.samples[i].length = 0;
  e.text.fileOf[i] = -1;
  e.text.sampleNames[i].clear();
  e.cuts.start[i] = e.cuts.end[i] = 0;
  e.eightBit[i] = false;
  auto& map = e.text.def.keymap;
  auto live = [&](int n) {
    const int s = map[static_cast<size_t>(n)];
    return s != slot && e.text.def.samples[static_cast<size_t>(s)].present();
  };
  std::array<uint8_t, 96> next = map;
  for (int n = 0; n < 96; ++n) {
    if (map[static_cast<size_t>(n)] != slot) continue;
    int pick = -1;
    for (int d = 1; d < 96 && pick < 0; ++d) {
      if (n - d >= 0 && live(n - d)) pick = map[static_cast<size_t>(n - d)];
      else if (n + d < 96 && live(n + d)) pick = map[static_cast<size_t>(n + d)];
    }
    if (pick < 0)
      for (int k = 0; k < trk::kMaxSamples && pick < 0; ++k)
        if (k != slot && e.text.def.samples[static_cast<size_t>(k)].present()) pick = k;
    next[static_cast<size_t>(n)] = static_cast<uint8_t>(pick < 0 ? 0 : pick);
  }
  map = next;
  compactFiles(e);
}

void swapSamples(Edit& e, int a, int b) {
  if (a == b || a < 0 || b < 0 || a >= trk::kMaxSamples || b >= trk::kMaxSamples) return;
  const auto i = static_cast<size_t>(a), j = static_cast<size_t>(b);
  std::swap(e.text.def.samples[i], e.text.def.samples[j]);
  std::swap(e.text.fileOf[i], e.text.fileOf[j]);
  std::swap(e.text.sampleNames[i], e.text.sampleNames[j]);
  std::swap(e.cuts.start[i], e.cuts.start[j]);
  std::swap(e.cuts.end[i], e.cuts.end[j]);
  std::swap(e.eightBit[i], e.eightBit[j]);
  for (auto& m : e.text.def.keymap) m = static_cast<uint8_t>(m == a ? b : m == b ? a : m);
  compactFiles(e);
}

void mapKeys(Edit& e, int lo, int hi, int slot) {
  if (lo > hi) std::swap(lo, hi);
  lo = std::clamp(lo, 0, 95);
  hi = std::clamp(hi, 0, 95);
  for (int n = lo; n <= hi; ++n) e.text.def.keymap[static_cast<size_t>(n)] = static_cast<uint8_t>(std::clamp(slot, 0, trk::kMaxSamples - 1));
}

int insertEnvPoint(trk::Envelope& env, int x, int y) {
  if (env.count >= trk::kEnvPoints) return -1;
  int i = 0;
  while (i < env.count && env.x[static_cast<size_t>(i)] < x) ++i;
  if (i < env.count && env.x[static_cast<size_t>(i)] == x) return -1;
  if (i == 0 && env.count > 0) return -1;  // the first point stays at the start
  for (int k = env.count; k > i; --k) {
    env.x[static_cast<size_t>(k)] = env.x[static_cast<size_t>(k - 1)];
    env.y[static_cast<size_t>(k)] = env.y[static_cast<size_t>(k - 1)];
  }
  env.x[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(x, 0, 32767));
  env.y[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(y, 0, 64));
  const bool had = env.count > 0;
  ++env.count;
  if (had) {
    if (env.sustain >= i) ++env.sustain;
    if (env.loopStart >= i) ++env.loopStart;
    if (env.loopEnd >= i) ++env.loopEnd;
  }
  return i;
}

void deleteEnvPoint(trk::Envelope& env, int index) {
  if (index <= 0 || index >= env.count || env.count <= 2) return;
  for (int k = index; k + 1 < env.count; ++k) {
    env.x[static_cast<size_t>(k)] = env.x[static_cast<size_t>(k + 1)];
    env.y[static_cast<size_t>(k)] = env.y[static_cast<size_t>(k + 1)];
  }
  --env.count;
  env.x[env.count] = env.y[env.count] = 0;
  auto fix = [&](uint8_t& v) {
    if (v > index) --v;
    if (v >= env.count) v = static_cast<uint8_t>(env.count - 1);
  };
  fix(env.sustain);
  fix(env.loopStart);
  fix(env.loopEnd);
}

int addSampleFiles(Edit& e, const std::vector<std::string>& paths, int* added, bool* full) {
  if (added) *added = 0;
  if (full) *full = false;
  if (!e.body) {  // the first sample of a new instrument: a plain envelope, not the built-in sound's vibrato and shape
    const auto samples = e.text.def.samples;
    e.text.def = defaultDefinition();
    e.text.def.samples = samples;
  }
  int first = -1;
  for (const std::string& p : paths) {
    SamplePtr sp = sampleCache().get(p);
    if (!sp || sp->frames() == 0) continue;
    const int k = addSample(e, p, static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF)), sp->sampleRate,
                            fs::u8path(p).stem().u8string());
    if (k < 0) {
      if (full) *full = true;
      break;
    }
    if (first < 0) first = k;
    if (added) ++*added;
  }
  return first;
}

int duplicateSample(Edit& e, int from) {
  if (from < 0 || from >= trk::kMaxSamples) return -1;
  const auto f = static_cast<size_t>(from);
  if (!e.text.def.samples[f].present() && e.text.fileOf[f] < 0) return -1;
  int slot = -1;
  for (int k = 0; k < trk::kMaxSamples && slot < 0; ++k)
    if (!e.text.def.samples[static_cast<size_t>(k)].present() && e.text.fileOf[static_cast<size_t>(k)] < 0) slot = k;
  if (slot < 0) return -1;
  const auto i = static_cast<size_t>(slot);
  e.text.def.samples[i] = e.text.def.samples[f];
  e.text.fileOf[i] = e.text.fileOf[f];
  e.text.sampleNames[i] = e.text.sampleNames[f];
  e.cuts.start[i] = e.cuts.start[f];
  e.cuts.end[i] = e.cuts.end[f];
  e.eightBit[i] = e.eightBit[f];
  compactFiles(e);
  return slot;
}

// K-106 (the classic "Copy smp."): slot `sslot` of `src` (another instrument's edit, or this one)
// into slot `dslot` of `dst`: its file, settings, name, cuts and bit depth. The key map stays, as
// in the classic editor, except in an instrument that had no sample: every key plays it then.
// -1 when the source slot is empty or a slot is out of range.
int copySampleInto(Edit& dst, int dslot, const Edit& src, int sslot) {
  if (dslot < 0 || dslot >= trk::kMaxSamples || sslot < 0 || sslot >= trk::kMaxSamples) return -1;
  const auto i = static_cast<size_t>(dslot), j = static_cast<size_t>(sslot);
  const int f = src.text.fileOf[j];
  if (!src.text.def.samples[j].present() && f < 0) return -1;
  bool any = false;
  for (int k = 0; k < trk::kMaxSamples; ++k)
    any = any || dst.text.def.samples[static_cast<size_t>(k)].present() || dst.text.fileOf[static_cast<size_t>(k)] >= 0;
  // Read everything first: `src` may be `dst`.
  const trk::SampleDef sd = src.text.def.samples[j];
  const std::string path = f >= 0 && f < static_cast<int>(src.text.files.size()) ? src.text.files[static_cast<size_t>(f)] : std::string();
  const std::string name = src.text.sampleNames[j];
  const int32_t cs = src.cuts.start[j], ce = src.cuts.end[j];
  const bool eight = src.eightBit[j];
  dst.text.def.samples[i] = sd;
  if (path.empty()) {
    dst.text.fileOf[i] = -1;
  } else {
    dst.text.fileOf[i] = static_cast<int>(dst.text.files.size());
    dst.text.files.push_back(path);
  }
  dst.text.sampleNames[i] = name;
  dst.cuts.start[i] = cs;
  dst.cuts.end[i] = ce;
  dst.eightBit[i] = eight;
  if (!any) dst.text.def.keymap.fill(static_cast<uint8_t>(dslot));
  compactFiles(dst);
  return dslot;
}

// K-106 (the classic "Xchg smp."): two slots trade their samples; the key map stays, so the keys
// that played one now play the other.
void exchangeSampleData(Edit& e, int a, int b) {
  if (a == b || a < 0 || b < 0 || a >= trk::kMaxSamples || b >= trk::kMaxSamples) return;
  const auto i = static_cast<size_t>(a), j = static_cast<size_t>(b);
  std::swap(e.text.def.samples[i], e.text.def.samples[j]);
  std::swap(e.text.fileOf[i], e.text.fileOf[j]);
  std::swap(e.text.sampleNames[i], e.text.sampleNames[j]);
  std::swap(e.cuts.start[i], e.cuts.start[j]);
  std::swap(e.cuts.end[i], e.cuts.end[j]);
  std::swap(e.eightBit[i], e.eightBit[j]);
  compactFiles(e);
}

// ---- envelope slots ----

namespace {

trk::Envelope shape(std::initializer_list<std::pair<int, int>> pts, int sustain, int loopA = -1, int loopB = -1) {
  trk::Envelope e = env(pts, sustain);
  if (loopA >= 0 && loopB >= loopA) {
    e.flags |= trk::kEnvLoop;
    e.loopStart = static_cast<uint8_t>(loopA);
    e.loopEnd = static_cast<uint8_t>(loopB);
  }
  return e;
}

std::string envText(const trk::Envelope& e) {
  std::string o = std::to_string(e.flags) + " " + std::to_string(e.count) + " " + std::to_string(e.sustain) + " " + std::to_string(e.loopStart) + " " +
                  std::to_string(e.loopEnd);
  for (int i = 0; i < e.count && i < trk::kEnvPoints; ++i)
    o += " " + std::to_string(e.x[static_cast<size_t>(i)]) + ":" + std::to_string(e.y[static_cast<size_t>(i)]);
  return o;
}

// An envelope as envText wrote it; false when it is not one (points out of order, values out of range).
bool readEnvText(std::istringstream& in, trk::Envelope& out) {
  long flags = -1, count = -1, sus = 0, ls = 0, le = 0;
  if (!(in >> flags >> count >> sus >> ls >> le)) return false;
  if (flags < 0 || flags > 7 || count < 0 || count > trk::kEnvPoints) return false;
  trk::Envelope e;
  e.flags = static_cast<uint8_t>(flags);
  e.count = static_cast<uint8_t>(count);
  e.sustain = static_cast<uint8_t>(std::clamp(sus, 0L, static_cast<long>(trk::kEnvPoints - 1)));
  e.loopStart = static_cast<uint8_t>(std::clamp(ls, 0L, static_cast<long>(trk::kEnvPoints - 1)));
  e.loopEnd = static_cast<uint8_t>(std::clamp(le, 0L, static_cast<long>(trk::kEnvPoints - 1)));
  for (int i = 0; i < e.count; ++i) {
    std::string w;
    if (!(in >> w)) return false;
    const size_t c = w.find(':');
    if (c == std::string::npos) return false;
    const long x = std::atol(w.substr(0, c).c_str()), y = std::atol(w.substr(c + 1).c_str());
    if (x < 0 || x > 32767 || y < 0 || y > 64) return false;
    if (i > 0 && x <= e.x[static_cast<size_t>(i - 1)]) return false;
    e.x[static_cast<size_t>(i)] = static_cast<int16_t>(x);
    e.y[static_cast<size_t>(i)] = static_cast<int16_t>(y);
  }
  if (e.count == 0) e.flags = 0;
  out = e;
  return true;
}

}  // namespace

EnvSlots factoryEnvSlots() {
  EnvSlots s;
  // Volume: a pluck, a held tone with a short release, a slow swell, a pulsing loop, a decay to a
  // held level, a short stab.
  s.vol[0].env = shape({{0, 64}, {4, 40}, {16, 12}, {40, 0}}, -1);
  s.vol[1].env = shape({{0, 64}, {4, 64}, {10, 0}}, 1);
  s.vol[2].env = shape({{0, 0}, {24, 64}, {32, 56}, {64, 0}}, 2);
  s.vol[2].vibSweep = 64;
  s.vol[2].vibDepth = 2;
  s.vol[2].vibRate = 8;
  s.vol[3].env = shape({{0, 64}, {5, 30}, {10, 64}, {48, 0}}, -1, 0, 2);
  s.vol[4].env = shape({{0, 64}, {8, 48}, {20, 40}, {36, 0}}, 2);
  s.vol[4].fadeout = 512;
  s.vol[5].env = shape({{0, 64}, {3, 64}, {5, 0}}, -1);
  // Panning (32 = the centre): steady, left to right, a swing between the sides, right to left,
  // a slight wobble, from the left into the centre.
  s.pan[0].env = shape({{0, 32}, {16, 32}}, -1);
  s.pan[1].env = shape({{0, 8}, {48, 56}}, -1);
  s.pan[2].env = shape({{0, 16}, {12, 48}, {24, 16}}, -1, 0, 2);
  s.pan[3].env = shape({{0, 56}, {48, 8}}, -1);
  s.pan[4].env = shape({{0, 26}, {6, 38}, {12, 26}}, -1, 0, 2);
  s.pan[5].env = shape({{0, 4}, {20, 32}}, -1);
  return s;
}

EnvSlots loadEnvSlots(const std::string& path) {
  EnvSlots s = factoryEnvSlots();
  std::ifstream f(fs::u8path(path));
  if (!f) return s;
  std::string line;
  if (!std::getline(f, line) || line.rfind("gl-tracker-env-slots", 0) != 0) return s;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string kind;
    long k = -1;
    if (!(in >> kind >> k) || k < 0 || k >= kEnvSlots) continue;
    if (kind == "v") {
      long fade = 0, vt = 0, vs = 0, vd = 0, vr = 0;
      if (!(in >> fade >> vt >> vs >> vd >> vr)) continue;
      EnvSlot e;
      if (!readEnvText(in, e.env)) continue;
      e.fadeout = static_cast<uint16_t>(std::clamp(fade, 0L, 4095L));
      e.vibType = static_cast<uint8_t>(std::clamp(vt, 0L, 3L));
      e.vibSweep = static_cast<uint8_t>(std::clamp(vs, 0L, 255L));
      e.vibDepth = static_cast<uint8_t>(std::clamp(vd, 0L, 15L));
      e.vibRate = static_cast<uint8_t>(std::clamp(vr, 0L, 63L));
      s.vol[static_cast<size_t>(k)] = e;
    } else if (kind == "p") {
      EnvSlot e;
      if (readEnvText(in, e.env)) s.pan[static_cast<size_t>(k)] = e;
    }
  }
  return s;
}

bool saveEnvSlots(const std::string& path, const EnvSlots& s) {
  std::error_code ec;
  const fs::path p = fs::u8path(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::string o = "gl-tracker-env-slots 1\n";
  for (int k = 0; k < kEnvSlots; ++k) {
    const EnvSlot& v = s.vol[static_cast<size_t>(k)];
    o += "v " + std::to_string(k) + " " + std::to_string(v.fadeout) + " " + std::to_string(v.vibType) + " " + std::to_string(v.vibSweep) + " " +
         std::to_string(v.vibDepth) + " " + std::to_string(v.vibRate) + " " + envText(v.env) + "\n";
  }
  for (int k = 0; k < kEnvSlots; ++k) o += "p " + std::to_string(k) + " " + envText(s.pan[static_cast<size_t>(k)].env) + "\n";
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << o;
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

void applyEnvSlot(trk::InstrumentDef& d, const EnvSlot& s, bool pan) {
  if (pan) {
    d.pan = s.env;
    return;
  }
  d.vol = s.env;
  d.fadeout = s.fadeout;
  d.vibType = s.vibType;
  d.vibSweep = s.vibSweep;
  d.vibDepth = s.vibDepth;
  d.vibRate = s.vibRate;
}

EnvSlot takeEnvSlot(const trk::InstrumentDef& d, bool pan) {
  EnvSlot s;
  s.env = pan ? d.pan : d.vol;
  if (!pan) {
    s.fadeout = d.fadeout;
    s.vibType = d.vibType;
    s.vibSweep = d.vibSweep;
    s.vibDepth = d.vibDepth;
    s.vibRate = d.vibRate;
  }
  return s;
}

int auditionPitch(const Edit& e, int slot, double rate) {
  if (slot < 0 || slot >= trk::kMaxSamples) return 60;
  return rootNote(e.text.def.samples[static_cast<size_t>(slot)], rate) + trk::kPitchOffset;
}

namespace {
bool slotUsed(const Edit& e, int k) {
  const auto i = static_cast<size_t>(k);
  return e.text.def.samples[i].present() || e.text.fileOf[i] >= 0;
}
int soundOfState(const PluginState& st) {
  return st.params.size() > static_cast<size_t>(trk::PSound) ? std::clamp(static_cast<int>(std::lround(st.params[trk::PSound])), 0, kSoundCount) : 0;
}
}  // namespace

int replaceSample(Edit& e, int slot, const std::string& path, int32_t frames, double rate, const std::string& name) {
  if (path.empty() || frames < 1) return -1;
  bool any = false;
  for (int k = 0; k < trk::kMaxSamples; ++k) any = any || slotUsed(e, k);
  if (!any || slot < 0 || slot >= trk::kMaxSamples) return addSample(e, path, frames, rate, name);
  const auto i = static_cast<size_t>(slot);
  trk::SampleDef s;
  s.length = frames;
  tuneForRate(s, rate);
  e.text.def.samples[i] = s;
  e.text.fileOf[i] = static_cast<int>(e.text.files.size());
  e.text.files.push_back(path);
  e.text.sampleNames[i] = oneLine(name);
  e.cuts.start[i] = e.cuts.end[i] = 0;
  e.eightBit[i] = false;
  compactFiles(e);
  return slot;
}

SampleTrim trimSamples(Edit& e, bool unused, bool afterLoop, bool eightBits) {
  SampleTrim n;
  auto& def = e.text.def;
  if (unused) {
    // The classic trim: a sample no key plays goes, the others move up in their order (the key map
    // follows them).
    std::array<bool, trk::kMaxSamples> keyed{};
    for (uint8_t k : def.keymap) keyed[k % trk::kMaxSamples] = true;
    for (int k = 0; k < trk::kMaxSamples; ++k)
      if (slotUsed(e, k) && !keyed[static_cast<size_t>(k)]) removeSample(e, k), ++n.removed;
    if (n.removed > 0) {
      int to = 0;
      for (int k = 0; k < trk::kMaxSamples; ++k) {
        if (!slotUsed(e, k)) continue;
        if (k != to) swapSamples(e, k, to);
        ++to;
      }
    }
  }
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const auto i = static_cast<size_t>(k);
    const trk::SampleDef& sd = def.samples[i];
    if (!sd.present()) continue;
    if (afterLoop && sd.loop != trk::kLoopOff && sd.loopLength > 0) {
      // Loop points and the end point are frames of the file: the sample now ends where its loop does.
      const int32_t le = sd.loopStart + sd.loopLength;
      const int32_t end = e.cuts.end[i] > 0 ? std::min(e.cuts.end[i], sd.length) : sd.length;
      if (le > e.cuts.start[i] && le < end) e.cuts.end[i] = le, ++n.trimmed;
    }
    if (eightBits && !e.eightBit[i]) e.eightBit[i] = true, ++n.eightBit;
  }
  return n;
}

bool trimSampleState(PluginState& st, bool unused, bool afterLoop, bool eightBits, SampleTrim* counts) {
  if (st.type != trk::kInstrumentType || (!unused && !afterLoop && !eightBits)) return false;
  Edit e = readEdit(st.data, soundOfState(st));
  const SampleTrim n = trimSamples(e, unused, afterLoop, eightBits);
  if (counts) *counts = n;
  if (n.total() == 0) return false;
  const std::string data = writeEdit(e);
  if (data == st.data) return false;
  st.data = data;
  return true;
}

bool midiMuted(const PluginState& st) {
  if (st.type != trk::kInstrumentType || st.data.find("midi ") == std::string::npos) return false;
  return readEdit(st.data, soundOfState(st)).text.def.mute;
}

long long sampleBytes(const PluginState& st) {
  if (st.type != trk::kInstrumentType) return 0;
  const std::shared_ptr<const InstrumentAux> inst = instrumentFor(st);
  if (!inst) return 0;
  long long bytes = 0;
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const auto i = static_cast<size_t>(k);
    if (!inst->data[i]) continue;
    const long long frames = std::max(0, inst->text.def.samples[i].length);
    bytes += frames * (inst->dataR[i] ? 2 : 1) * (inst->eightBit[i] ? 1 : 2);
  }
  return bytes;
}

// ---- K-106 cp17: the editing session's pure edits (tracker_session.h). Each is the plugin
// editor's inline edit moved here as it was, so both front ends make exactly the same change. ----

int soundOf(const PluginState& st) { return soundOfState(st); }

bool hasSample(const Edit& e, int k) {
  const auto i = static_cast<size_t>(k);
  return e.text.def.samples[i].present() || e.text.fileOf[i] >= 0;
}

int sampleCountOf(const Edit& e) {
  int n = 0;
  for (int k = 0; k < trk::kMaxSamples; ++k) n += (e.text.def.samples[static_cast<size_t>(k)].present() || e.text.fileOf[static_cast<size_t>(k)] >= 0) ? 1 : 0;
  return n;
}

std::string editName(const Edit& e, int k) {
  const std::string& nm = e.text.sampleNames[static_cast<size_t>(k)];
  if (!nm.empty()) return nm;
  const int f = e.text.fileOf[static_cast<size_t>(k)];
  if (f >= 0 && f < static_cast<int>(e.text.files.size())) return fs::u8path(e.text.files[static_cast<size_t>(f)]).stem().u8string();
  return "sample";
}

void applySampleField(Edit& e, int k, SampleField f, long v) {
  trk::SampleDef& s = e.text.def.samples[static_cast<size_t>(k)];
  switch (f) {
    case SampleField::Volume: s.volume = static_cast<uint8_t>(std::clamp(v, 0L, 64L)); break;
    case SampleField::Pan: s.pan = static_cast<uint8_t>(std::clamp(v + 128, 0L, 255L)); break;
    case SampleField::Fine: s.finetune = static_cast<int8_t>(std::clamp(v, -128L, 127L)); break;
    case SampleField::RelNote: s.relNote = static_cast<int8_t>(std::clamp(v, -48L, 71L)); break;
  }
}

const char* sampleFieldLabel(SampleField f) {
  switch (f) {
    case SampleField::Volume: return "sample volume";
    case SampleField::Pan: return "sample pan";
    case SampleField::Fine: return "sample fine tune";
    case SampleField::RelNote: return "sample transpose";
  }
  return "sample";
}

int sampleFieldMerge(SampleField f) { return 1 + static_cast<int>(f); }

void applyLoopType(Edit& e, int k, uint8_t loop, int32_t s0, int32_t s1) {
  trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
  sd.loop = loop;
  if (loop != trk::kLoopOff && sd.loopLength < 1) {
    sd.loopStart = s0;
    sd.loopLength = s1 - s0;
  }
}

void applyMarker(Edit& e, int k, int marker, int32_t f, int32_t frames) {
  trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
  int32_t& cutA = e.cuts.start[static_cast<size_t>(k)];
  int32_t& cutB = e.cuts.end[static_cast<size_t>(k)];
  const int32_t s0 = std::clamp(cutA, 0, frames - 1);
  const int32_t s1 = cutB > 0 ? std::clamp(cutB, s0 + 1, frames) : frames;
  const int32_t ls = sd.loopStart, le = sd.loopStart + sd.loopLength;
  switch (marker) {
    case 0: cutA = std::clamp(f, 0, s1 - 1); break;
    case 1: {
      const int32_t v = std::clamp(f, s0 + 1, frames);
      cutB = v >= frames ? 0 : v;
      break;
    }
    case 2: {
      const int32_t v = std::clamp(f, s0, le - 1);
      sd.loopStart = v;
      sd.loopLength = le - v;
      break;
    }
    default: sd.loopLength = std::clamp(f, ls + 1, s1) - ls; break;
  }
}

const char* markerLabel(int marker) {
  static const char* labels[] = {"sample start", "sample end", "loop start", "loop end"};
  return labels[std::clamp(marker, 0, 3)];
}

void applyEnvFlag(Edit& e, bool pan, uint8_t flag) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  const bool on = (env.flags & flag) != 0;
  env.flags = static_cast<uint8_t>(on ? env.flags & ~flag : env.flags | flag);
  if (env.count == 0 && !on) {  // switched on with no points: a flat line to start from
    insertEnvPoint(env, 0, pan ? 32 : 64);
    insertEnvPoint(env, 16, pan ? 32 : 64);
  }
}

int applyEnvAdd(Edit& e, bool pan, int x, int y) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  const bool empty = env.count == 0;
  if (empty) insertEnvPoint(env, 0, y);  // the first point sits at the start
  const int i = insertEnvPoint(env, std::max(1, x), y);
  if (i >= 0 && empty) env.flags |= trk::kEnvOn;
  return i;
}

bool applyEnvMove(Edit& e, bool pan, int i, int x, int y) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  if (i < 0 || i >= env.count) return false;
  const int lo = i == 0 ? 0 : env.x[static_cast<size_t>(i - 1)] + 1;
  const int hi = i == 0 ? 0 : i + 1 < env.count ? env.x[static_cast<size_t>(i + 1)] - 1 : 32767;
  const int nx = std::clamp(x, lo, std::max(lo, hi));
  const int ny = std::clamp(y, 0, 64);
  if (nx == env.x[static_cast<size_t>(i)] && ny == env.y[static_cast<size_t>(i)]) return false;
  env.x[static_cast<size_t>(i)] = static_cast<int16_t>(nx);
  env.y[static_cast<size_t>(i)] = static_cast<int16_t>(ny);
  return true;
}

bool applyEnvDelete(Edit& e, bool pan, int i) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  if (i <= 0 || i >= env.count || env.count <= 2) return false;
  deleteEnvPoint(env, i);
  return true;
}

void applyEnvSustain(Edit& e, bool pan, int i) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  env.sustain = static_cast<uint8_t>(i);
  env.flags |= trk::kEnvSustain;
}

void applyEnvLoopStart(Edit& e, bool pan, int i) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  env.loopStart = static_cast<uint8_t>(i);
  if (env.loopEnd < i) env.loopEnd = static_cast<uint8_t>(i);
  env.flags |= trk::kEnvLoop;
}

void applyEnvLoopEnd(Edit& e, bool pan, int i) {
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  env.loopEnd = static_cast<uint8_t>(i);
  if (env.loopStart > i) env.loopStart = static_cast<uint8_t>(i);
  env.flags |= trk::kEnvLoop;
}

void applyFadeout(Edit& e, long v) { e.text.def.fadeout = static_cast<uint16_t>(std::clamp(v, 0L, 4095L)); }

void applyVibrato(Edit& e, VibField f, long v) {
  trk::InstrumentDef& d = e.text.def;
  switch (f) {
    case VibField::Sweep: d.vibSweep = static_cast<uint8_t>(std::clamp(v, 0L, 255L)); break;
    case VibField::Depth: d.vibDepth = static_cast<uint8_t>(std::clamp(v, 0L, 15L)); break;
    case VibField::Rate: d.vibRate = static_cast<uint8_t>(std::clamp(v, 0L, 63L)); break;
  }
}

const char* vibLabel(VibField f) { return f == VibField::Sweep ? "vibrato sweep" : f == VibField::Depth ? "vibrato depth" : "vibrato rate"; }
int vibMerge(VibField f) { return 31 + static_cast<int>(f); }

void applyMidi(Edit& e, const MidiSettings& m, bool syncMute) {
  e.midi = m;
  if (syncMute) e.text.def.mute = m.on && m.mute;
}

bool envPointAfter(const trk::Envelope& env, bool pan, int i, int* x, int* y) {
  if (env.count >= trk::kEnvPoints) {
    return false;
  }
  if (env.count == 0) {
    *x = 16;
    *y = pan ? 32 : 64;
    return true;
  }
  i = std::clamp(i, 0, env.count - 1);
  const auto at = static_cast<size_t>(i);
  if (i + 1 >= env.count) {
    *x = env.x[at] + 16;
    *y = env.y[at];
    return *x <= 32767;
  }
  const auto nx = static_cast<size_t>(i + 1);
  if (env.x[nx] - env.x[at] < 2) {
    return false;
  }
  *x = (env.x[at] + env.x[nx]) / 2;
  *y = (env.y[at] + env.y[nx] + 1) / 2;
  return true;
}

bool applyKeysAway(Edit& e, int slot) {
  if (slot < 0 || slot >= trk::kMaxSamples) {
    return false;
  }
  auto& map = e.text.def.keymap;
  auto live = [&](int n) {
    const int s = map[static_cast<size_t>(n)];
    return s != slot && e.text.def.samples[static_cast<size_t>(s)].present();
  };
  int other = -1;
  for (int k = 0; k < trk::kMaxSamples && other < 0; ++k) {
    if (k != slot && e.text.def.samples[static_cast<size_t>(k)].present()) {
      other = k;
    }
  }
  if (other < 0) {
    return false;
  }
  std::array<uint8_t, 96> next = map;
  bool any = false;
  for (int n = 0; n < 96; ++n) {
    if (map[static_cast<size_t>(n)] != slot) {
      continue;
    }
    int pick = -1;
    for (int d = 1; d < 96 && pick < 0; ++d) {
      if (n - d >= 0 && live(n - d)) {
        pick = map[static_cast<size_t>(n - d)];
      } else if (n + d < 96 && live(n + d)) {
        pick = map[static_cast<size_t>(n + d)];
      }
    }
    next[static_cast<size_t>(n)] = static_cast<uint8_t>(pick < 0 ? other : pick);
    any = true;
  }
  if (any) {
    map = next;
  }
  return any;
}

// ---- K-106 cp18 (K-316 plan 6): a plain Sampler becomes a Tracker Sampler ----
PluginState fromSamplerState(const PluginState& sampler, int32_t frames, double rate, const std::string& name) {
  PluginState out = defaultPluginState(trk::kInstrumentType);
  if (sampler.type != "gl.sampler" || sampler.data.empty() || frames < 1) return out;
  auto par = [&](int i, float d) { return i < static_cast<int>(sampler.params.size()) ? sampler.params[static_cast<size_t>(i)] : d; };
  Edit e = readEdit("", 0);
  if (addSample(e, sampler.data, frames, rate, name) != 0) return out;
  trk::InstrumentDef& d = e.text.def;
  const auto samples = d.samples;
  const auto keys = d.keymap;
  d = defaultDefinition();
  d.samples = samples;
  d.keymap = keys;
  e.body = true;
  trk::SampleDef& sd = d.samples[0];
  // The level and pan (the sampler's volume is a gain, 1 = full; the tracker's 64 is full).
  applySampleField(e, 0, SampleField::Volume, std::lround(std::clamp(par(pp::SmpVolume, 0.8f), 0.0f, 1.0f) * 64.0f));
  applySampleField(e, 0, SampleField::Pan, std::lround(std::clamp(par(pp::SmpPan, 0.0f), -1.0f, 1.0f) * 127.0f));
  // The root key and fine tune (cents to 1/128 semitone), the fine tune first: the root keeps it.
  applySampleField(e, 0, SampleField::Fine, std::lround(std::clamp(par(pp::SmpFine, 0.0f), -100.0f, 100.0f) * 1.28f));
  setRootNote(sd, rate, std::clamp(static_cast<int>(std::lround(par(pp::SmpRoot, 60.0f))) - trk::kPitchOffset, 1, 96));
  // Start and end (fractions of the file) become the sample's cuts; the loop its loop points.
  const int32_t c0 = std::clamp(static_cast<int32_t>(std::lround(std::clamp(par(pp::SmpStart, 0.0f), 0.0f, 1.0f) * frames)), 0, frames - 1);
  const float endF = std::clamp(par(pp::SmpEnd, 1.0f), 0.0f, 1.0f);
  int32_t c1 = endF >= 1.0f ? frames : std::clamp(static_cast<int32_t>(std::lround(endF * frames)), c0 + 1, frames);
  e.cuts.start[0] = c0;
  e.cuts.end[0] = c1 >= frames ? 0 : c1;
  if (par(pp::SmpLoop, 0.0f) >= 0.5f) {
    const int32_t ls = std::clamp(static_cast<int32_t>(std::lround(std::clamp(par(pp::SmpLoopStart, 0.0f), 0.0f, 1.0f) * frames)), c0, c1 - 1);
    const int32_t le = std::clamp(static_cast<int32_t>(std::lround(std::clamp(par(pp::SmpLoopEnd, 1.0f), 0.0f, 1.0f) * frames)), ls + 1, c1);
    sd.loop = par(pp::SmpLoopMode, 0.0f) >= 0.5f ? trk::kLoopPingPong : trk::kLoopForward;
    sd.loopStart = ls;
    sd.loopLength = le - ls;
  }
  // The amplitude envelope as a volume envelope (ticks of 1/50 s, the classic tracker's at 125 BPM):
  // attack up to full, decay to the sustain level (held while the key is down), release to silence.
  auto ticks = [](float sec) { return std::clamp(static_cast<int>(std::lround(sec * 50.0f)), 0, 8000); };
  const int at = ticks(par(pp::SmpAttack, 0.0005f)), dc = ticks(par(pp::SmpDecay, 10.0f)), rl = std::max(1, ticks(par(pp::SmpRelease, 0.02f)));
  const int sus = std::clamp(static_cast<int>(std::lround(std::clamp(par(pp::SmpSustain, 1.0f), 0.0f, 1.0f) * 64.0f)), 0, 64);
  trk::Envelope v;
  int n = 0;
  auto pt = [&](int x, int y) {
    v.x[static_cast<size_t>(n)] = static_cast<int16_t>(x);
    v.y[static_cast<size_t>(n)] = static_cast<int16_t>(y);
    ++n;
  };
  if (at >= 1) pt(0, 0);
  pt(at, 64);
  if (dc >= 1 && sus < 64) pt(at + dc, sus);
  v.sustain = static_cast<uint8_t>(n - 1);
  pt(at + (dc >= 1 && sus < 64 ? dc : 0) + rl, 0);
  v.count = static_cast<uint8_t>(n);
  v.flags = trk::kEnvOn | trk::kEnvSustain;
  d.vol = v;
  out.data = writeEdit(e);
  return out;
}

}  // namespace gl::packs::trackerpack
