// Gridline: SFZ instruments. See SfzMap.h. The opcode list follows the public SFZ format
// documentation (names only); the reading and every behaviour here are Gridline's own.
#include "SfzMap.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "SfzText.h"

namespace fs = std::filesystem;

namespace gl::sfz {

// ======================================================================= registry
namespace {
#include "SfzOpcodes.inc"  // kOpcodes[]: {pattern, family, support, note}

// Digit runs become '#'; in patterns the placeholders N, X, Y, NN, NNN do.
std::string normalName(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      o += '#';
    } else {
      o += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i++])));
    }
  }
  return o;
}
std::string normalPattern(const char* p) {
  std::string o;
  for (size_t i = 0; p[i];) {
    const char c = p[i];
    if (c == 'N' || c == 'X' || c == 'Y') {
      while (p[i] == 'N' || p[i] == 'X' || p[i] == 'Y') ++i;
      o += '#';
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      while (std::isdigit(static_cast<unsigned char>(p[i]))) ++i;
      o += '#';
    } else {
      o += c, ++i;
    }
  }
  return o;
}
struct Registry {
  std::map<std::string, const OpcodeInfo*> exact;
  std::vector<std::pair<std::string, const OpcodeInfo*>> prefix, suffix;  // "hint_*", "*_mod", "varNN_*"
  std::vector<OpcodeInfo> all;
  Registry() {
    for (const auto& o : kOpcodes) all.push_back(o);
    for (const auto& o : all) {
      std::string p = o.pattern;
      if (!p.empty() && p.back() == '*') prefix.emplace_back(normalPattern(p.substr(0, p.size() - 1).c_str()), &o);
      else if (!p.empty() && p[0] == '*') suffix.emplace_back(p.substr(1), &o);
      else exact.emplace(normalPattern(o.pattern), &o);
    }
  }
};
const Registry& registry() {
  static const Registry r;
  return r;
}
}  // namespace

const std::vector<OpcodeInfo>& opcodeTable() { return registry().all; }

const OpcodeInfo* findOpcode(const std::string& name) {
  const Registry& r = registry();
  const std::string n = normalName(name);
  if (auto it = r.exact.find(n); it != r.exact.end()) return it->second;
  for (const auto& [p, o] : r.prefix)
    if (n.rfind(p, 0) == 0) return o;
  for (const auto& [s, o] : r.suffix)
    if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0) return o;
  return nullptr;
}

bool knownHeader(const std::string& h) {
  static const char* const k[] = {"control", "global", "master", "group", "region", "curve", "effect", "sample", "midi"};
  for (const char* x : k)
    if (h == x) return true;
  return false;
}

// ======================================================================= values
namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
float F(const std::string& v) { return static_cast<float>(std::strtod(v.c_str(), nullptr)); }
int I(const std::string& v) { return static_cast<int>(std::lround(std::strtod(v.c_str(), nullptr))); }
int64_t L64(const std::string& v) { return static_cast<int64_t>(std::llround(std::strtod(v.c_str(), nullptr))); }
bool B(const std::string& v) {
  const std::string s = lower(v);
  return s == "on" || s == "1" || s == "true" || s == "yes";
}
// Numbers in a name: "lfo2_eq3freq_oncc74" -> {2, 3, 74}.
std::vector<int> numbersIn(const std::string& s) {
  std::vector<int> n;
  for (size_t i = 0; i < s.size();)
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
      int v = 0;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
      n.push_back(v);
    } else {
      ++i;
    }
  return n;
}
}  // namespace

int noteNumber(const std::string& vIn, bool* ok) {
  const std::string v = lower(vIn);
  if (ok) *ok = true;
  if (v.empty()) {
    if (ok) *ok = false;
    return 60;
  }
  if (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-') return I(v);
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  if (v[0] < 'a' || v[0] > 'g') {
    if (ok) *ok = false;
    return 60;
  }
  int n = base[v[0] - 'a'];
  size_t i = 1;
  if (i < v.size() && (v[i] == '#' || v[i] == 's')) ++n, ++i;
  else if (i < v.size() && v[i] == 'b' && i + 1 < v.size() && (std::isdigit(static_cast<unsigned char>(v[i + 1])) || v[i + 1] == '-')) --n, ++i;
  const int oct = i < v.size() ? std::atoi(v.c_str() + i) : 4;
  return n + (oct + 1) * 12;
}

int filterTypeOf(const std::string& nameIn) {
  static const char* const k[] = {"lpf_1p", "hpf_1p", "lpf_2p", "hpf_2p", "bpf_2p", "brf_2p", "bpf_1p", "brf_1p", "apf_1p",
                                  "lpf_2p_sv", "hpf_2p_sv", "bpf_2p_sv", "brf_2p_sv", "lpf_4p", "hpf_4p", "lpf_6p", "hpf_6p",
                                  "pkf_2p", "lsh", "hsh", "peq", "comb", "pink", "bpk_2p"};
  const std::string n = lower(nameIn);
  for (int i = 0; i < static_cast<int>(sizeof(k) / sizeof(k[0])); ++i)
    if (n == k[i]) return i;
  return -1;
}
const char* filterTypeName(int t) {
  static const char* const k[] = {"lpf_1p", "hpf_1p", "lpf_2p", "hpf_2p", "bpf_2p", "brf_2p", "bpf_1p", "brf_1p", "apf_1p",
                                  "lpf_2p_sv", "hpf_2p_sv", "bpf_2p_sv", "brf_2p_sv", "lpf_4p", "hpf_4p", "lpf_6p", "hpf_6p",
                                  "pkf_2p", "lsh", "hsh", "peq", "comb", "pink", "bpk_2p"};
  return t >= 0 && t < static_cast<int>(sizeof(k) / sizeof(k[0])) ? k[t] : "off";
}

float curveAt(const Instrument& ins, int c, float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  if (c >= 0 && c < 256 && ins.curves[static_cast<size_t>(c)].used) {
    const auto& v = ins.curves[static_cast<size_t>(c)].v;
    const float p = x * 127.0f;
    const int i = std::min(126, static_cast<int>(p));
    return v[static_cast<size_t>(i)] + (v[static_cast<size_t>(i + 1)] - v[static_cast<size_t>(i)]) * (p - static_cast<float>(i));
  }
  switch (c) {  // the format's default curves
    case 1: return x * 2.0f - 1.0f;                           // bipolar
    case 2: return 1.0f - x;                                  // inverted
    case 3: return 1.0f - x * 2.0f;                           // bipolar inverted
    case 4: return x * x;                                     // concave
    case 5: return std::sqrt(x);                              // convex
    case 6: return std::sqrt(1.0f - x);                       // convex inverted
    default: return x;
  }
}

std::string reportLine(const Report& r) {
  if (r.unknown.empty() && r.unknownHeaders.empty() && r.reported.empty() && r.unhandled.empty()) return {};
  std::string names;
  int n = 0;
  std::map<std::string, int> all = r.unknown;
  for (const auto& [k, c] : r.reported) all[k] += c;
  for (const auto& [k, c] : r.unhandled) all[k] += c;
  for (const auto& [k, c] : all) {
    (void)c;
    if (n < 8) names += (n ? ", " : "") + k;
    ++n;
  }
  for (const auto& [k, c] : r.unknownHeaders) {
    (void)c;
    if (n < 8) names += std::string(n ? ", " : "") + "<" + k + ">";
    ++n;
  }
  if (n > 8) names += ", ...";
  return std::to_string(n) + (n == 1 ? " setting" : " settings") + " Gridline does not play: " + names;
}

// ======================================================================= reading
namespace {

struct Op {
  std::string name, value;
};
enum class Hd { None, Control, Global, Master, Group, Region, Curve, Effect, Sample, Midi };

struct Ctx {
  Instrument* ins = nullptr;
  std::string defaultPath;
  int noteOffset = 0, octaveOffset = 0, swNoteOffset = 0, swOctaveOffset = 0;
  fs::path base;
};

int keyOf(const Ctx& c, const std::string& v) {
  bool ok = true;
  const int n = noteNumber(v, &ok);
  return n + c.noteOffset + 12 * c.octaveOffset;
}
int swKeyOf(const Ctx& c, const std::string& v) {
  bool ok = true;
  const int n = noteNumber(v, &ok);
  return n + c.swNoteOffset + 12 * c.swOctaveOffset;
}

// The values of the RIFF 'smpl' chunk of a WAV file: root key and the first loop.
struct WavMeta {
  int root = -1;
  int64_t loopStart = -1, loopEnd = -1;
};
WavMeta wavMeta(const std::string& path) {
  WavMeta m;
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) return m;
  char hdr[12];
  if (!f.read(hdr, 12) || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return m;
  for (int guard = 0; guard < 256; ++guard) {
    char ch[8];
    if (!f.read(ch, 8)) break;
    const uint32_t len = static_cast<uint32_t>(static_cast<unsigned char>(ch[4])) | static_cast<uint32_t>(static_cast<unsigned char>(ch[5])) << 8 |
                         static_cast<uint32_t>(static_cast<unsigned char>(ch[6])) << 16 | static_cast<uint32_t>(static_cast<unsigned char>(ch[7])) << 24;
    if (std::memcmp(ch, "smpl", 4) == 0 && len >= 36) {
      std::vector<unsigned char> b(std::min<uint32_t>(len, 4096));
      if (!f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()))) break;
      auto u32 = [&](size_t at) { return at + 4 <= b.size() ? static_cast<uint32_t>(b[at]) | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<uint32_t>(b[at + 3]) << 24 : 0u; };
      m.root = static_cast<int>(u32(12));
      if (u32(28) > 0 && b.size() >= 36 + 24) {
        m.loopStart = u32(36 + 8);
        m.loopEnd = static_cast<int64_t>(u32(36 + 12)) + 1;
      }
      break;
    }
    f.seekg(static_cast<std::streamoff>(len + (len & 1)), std::ios::cur);
  }
  return m;
}

// ---- modulation names
// The target a name base stands for ("cutoff2" -> Cutoff index 1).
bool targetOf(const std::string& b, Target& t, int& idx) {
  idx = 0;
  if (b == "volume" || b == "gain") return t = Target::Volume, true;
  if (b == "amplitude") return t = Target::Amplitude, true;
  if (b == "pan") return t = Target::Pan, true;
  if (b == "width") return t = Target::Width, true;
  if (b == "position") return t = Target::Position, true;
  if (b == "pitch" || b == "tune") return t = Target::Pitch, true;
  if (b == "cutoff") return t = Target::Cutoff, true;
  if (b == "cutoff2") return t = Target::Cutoff, idx = 1, true;
  if (b == "resonance") return t = Target::Resonance, true;
  if (b == "resonance2") return t = Target::Resonance, idx = 1, true;
  if (b == "fil_gain") return t = Target::FilGain, true;
  if (b == "fil2_gain") return t = Target::FilGain, idx = 1, true;
  if (b == "offset") return t = Target::Offset, true;
  if (b == "delay") return t = Target::Delay, true;
  if (b == "delay_samples") return t = Target::Delay, idx = 1, true;
  if (b == "delay_beats") return t = Target::Delay, idx = 2, true;
  if (b == "loop_start") return t = Target::LoopStart, true;
  if (b == "loop_length") return t = Target::LoopLength, true;
  if (b == "noise_level" || b == "noiselevel") return t = Target::NoiseLevel, true;
  if (b == "noise_step" || b == "noisestep") return t = Target::NoiseStep, true;
  if (b == "noise_tone" || b == "noisetone") return t = Target::NoiseTone, true;
  if (b == "decim") return t = Target::Decim, true;
  if (b == "bitred") return t = Target::Bitred, true;
  if (b == "rectify") return t = Target::Rectify, true;
  if (b == "ringmod") return t = Target::RingMod, true;
  if (b == "drive" || b == "driveshape") return t = Target::Drive, true;
  if (b == "amp_veltrack") return t = Target::AmpegStage, idx = 100, true;  // velocity tracking amount
  if (b == "oscillator_detune") return t = Target::Oscillator, true;
  if (b == "oscillator_mod_depth" || b == "oscillator_mod") return t = Target::Oscillator, idx = 1, true;
  if (b.size() >= 5 && b.rfind("eq", 0) == 0 && std::isdigit(static_cast<unsigned char>(b[2]))) {
    const int band = b[2] - '1';
    if (band < 0 || band > 2) return false;
    std::string rest = b.substr(3);
    if (!rest.empty() && rest[0] == '_') rest.erase(0, 1);
    idx = band;
    if (rest == "freq") return t = Target::EqFreq, true;
    if (rest == "bw") return t = Target::EqBw, true;
    if (rest == "gain") return t = Target::EqGain, true;
  }
  return false;
}
int egStageOf(const std::string& s) {
  static const char* const k[] = {"delay", "start", "attack", "hold", "decay", "sustain", "release", "depth"};
  for (int i = 0; i < 8; ++i)
    if (s == k[i]) return i;
  return -1;
}

// Splits "<base>_onccN" style names: kind 0 amount, 1 curve, 2 smooth, 3 step. cc < 0: not such a name.
bool ccSuffix(const std::string& n, std::string& base, int& cc, int& kind) {
  struct S {
    const char* s;
    int kind;
  };
  static const S k[] = {{"_curvecc", 1}, {"_smoothcc", 2}, {"_stepcc", 3}, {"_oncc", 0}, {"_cc", 0}, {"cc", 0}};
  size_t d = n.size();
  while (d > 0 && std::isdigit(static_cast<unsigned char>(n[d - 1]))) --d;
  if (d == n.size()) {
    if (n.size() > 8 && n.compare(n.size() - 8, 8, "_chanaft") == 0) return base = n.substr(0, n.size() - 8), cc = kCcChanAft, kind = 0, true;
    if (n.size() > 8 && n.compare(n.size() - 8, 8, "_polyaft") == 0) return base = n.substr(0, n.size() - 8), cc = kCcPolyAft, kind = 0, true;
    if (n.size() > 7 && n.compare(n.size() - 7, 7, "chanaft") == 0) return base = n.substr(0, n.size() - 7), cc = kCcChanAft, kind = 0, true;
    if (n.size() > 7 && n.compare(n.size() - 7, 7, "polyaft") == 0) return base = n.substr(0, n.size() - 7), cc = kCcPolyAft, kind = 0, true;
    return false;
  }
  const std::string head = n.substr(0, d);
  for (const auto& s : k) {
    const size_t L = std::strlen(s.s);
    if (head.size() > L && head.compare(head.size() - L, L, s.s) == 0) {
      base = head.substr(0, head.size() - L);
      cc = std::atoi(n.c_str() + d);
      kind = s.kind;
      return cc >= 0 && cc < kCcCount;
    }
  }
  return false;
}
void addCc(std::vector<CcMod>& v, Target t, int idx, int cc, int kind, float val) {
  CcMod* m = nullptr;
  for (auto& x : v)
    if (x.target == t && x.index == idx && x.cc == cc) m = &x;
  if (!m) {
    v.push_back({});
    m = &v.back();
    m->target = t, m->index = idx, m->cc = cc;
  }
  switch (kind) {
    case 0: m->depth = val; break;
    case 1: m->curve = static_cast<int>(val); break;
    case 2: m->smooth = val; break;
    default: m->step = val; break;
  }
}

FlexEg& egAt(Region& r, int n) {
  n = std::clamp(n, 1, 64);
  if (static_cast<int>(r.egs.size()) < n) r.egs.resize(static_cast<size_t>(n));
  return r.egs[static_cast<size_t>(n - 1)];
}
Lfo& lfoAt(Region& r, int n) {
  n = std::clamp(n, 1, 64);
  if (static_cast<int>(r.lfos.size()) < n) r.lfos.resize(static_cast<size_t>(n));
  Lfo& l = r.lfos[static_cast<size_t>(n - 1)];
  l.used = true;
  return l;
}
template <typename D>
void setDest(std::vector<D>& v, Target t, int idx, float depth) {
  for (auto& d : v)
    if (d.target == t && d.index == idx) return void(d.depth = depth);
  v.push_back({t, idx, depth});
}
int destIndex(const std::vector<FlexEg::Dest>& v, Target t, int idx) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].target == t && v[i].index == idx) return static_cast<int>(i);
  return -1;
}
int destIndex(const std::vector<Lfo::Dest>& v, Target t, int idx) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].target == t && v[i].index == idx) return static_cast<int>(i);
  return -1;
}

// Target names used after lfoN_ / egN_ / varNN_.
bool modTarget(const std::string& s, Target& t, int& idx) {
  if (targetOf(s, t, idx)) return true;
  // eqXfreq without underscore (lfoN_eq2freq)
  return false;
}

// ---- classic envelopes
bool classicEg(ClassicEg& eg, const std::string& rest, const std::string& n, const std::string& v) {
  // rest: "attack", "vel2decay", "attack_shape", "decay_zero", "dynamic", "attack_onccN" ...
  eg.used = true;
  const int st = egStageOf(rest);
  if (st >= 0) {
    float x = F(v);
    switch (st) {
      case 0: eg.delay = x; break;
      case 1: eg.start = x; break;
      case 2: eg.attack = x; break;
      case 3: eg.hold = x; break;
      case 4: eg.decay = x; break;
      case 5: eg.sustain = x; break;
      case 6: eg.release = x; break;
      default: eg.depth = x; break;
    }
    return true;
  }
  if (rest.rfind("vel2", 0) == 0) {
    const int s2 = egStageOf(rest.substr(4));
    const float x = F(v);
    switch (s2) {
      case 0: eg.vel2delay = x; return true;
      case 2: eg.vel2attack = x; return true;
      case 3: eg.vel2hold = x; return true;
      case 4: eg.vel2decay = x; return true;
      case 5: eg.vel2sustain = x; return true;
      case 6: eg.vel2release = x; return true;
      case 7: eg.vel2depth = x; return true;
      default: return false;
    }
  }
  if (rest == "attack_shape") return eg.attackShape = F(v), true;
  if (rest == "decay_shape") return eg.decayShape = F(v), true;
  if (rest == "release_shape") return eg.releaseShape = F(v), true;
  if (rest == "decay_zero") return eg.decayZero = B(v), true;
  if (rest == "release_zero") return eg.releaseZero = B(v), true;
  if (rest == "dynamic") return eg.dynamic = B(v), true;
  std::string base;
  int cc = 0, kind = 0;
  if (ccSuffix(rest, base, cc, kind)) {
    const int s2 = egStageOf(base);
    if (s2 < 0) return false;
    for (auto& c : eg.cc)
      if (c.stage == s2 && c.cc == cc) {
        if (kind == 0) c.amount = F(v);
        else if (kind == 1) c.curve = I(v);
        return true;
      }
    eg.cc.push_back({s2, cc, kind == 0 ? F(v) : 0.0f, kind == 1 ? I(v) : 0});
    return true;
  }
  (void)n;
  return false;
}

// ---- the region opcodes (and the inherited ones)
bool regionOp(Ctx& c, Region& r, const std::string& nameIn, const std::string& v) {
  const std::string n = lower(nameIn);
  const std::vector<int> num = numbersIn(n);
  const std::string nn = normalName(n);
  Target t;
  int ti = 0;
  // ------------------------------------------------ sound source
  if (n == "sample") {
    r.sample = v;
    const std::string lv = lower(v);
    if (!lv.empty() && lv[0] == '*') {
      if (lv == "*sine") r.builtin = Builtin::Sine;
      else if (lv == "*tri" || lv == "*triangle") r.builtin = Builtin::Triangle;
      else if (lv == "*square") r.builtin = Builtin::Square;
      else if (lv == "*saw") r.builtin = Builtin::Saw;
      else if (lv == "*noise") r.builtin = Builtin::Noise;
      else r.builtin = Builtin::Silence;
      r.file.clear();
    } else {
      r.builtin = Builtin::None;
      std::string p = c.defaultPath + v;
      std::replace(p.begin(), p.end(), '\\', '/');
      fs::path fp = fs::u8path(p);
      if (!fp.is_absolute()) fp = c.base / fp;
      r.file = fp.lexically_normal().u8string();
    }
    return true;
  }
  if (n == "offset") return r.offset = L64(v), true;
  if (n == "offset_random") return r.offsetRandom = L64(v), true;
  if (n == "offset_mode") return r.offsetPercent = lower(v) == "percent", true;
  if (n == "end") return r.end = L64(v), true;
  if (n == "count") return r.count = L64(v), true;
  if (n == "loop_mode" || n == "loopmode") {
    const std::string s = lower(v);
    r.loopMode = s == "no_loop" ? LoopMode::NoLoop : s == "one_shot" ? LoopMode::OneShot : s == "loop_continuous" ? LoopMode::Continuous
                 : s == "loop_sustain" ? LoopMode::Sustain : LoopMode::Unset;
    return true;
  }
  if (n == "loop_type" || n == "looptype") {
    const std::string s = lower(v);
    r.loopType = s == "backward" ? LoopType::Backward : s == "alternate" ? LoopType::Alternate : LoopType::Forward;
    return true;
  }
  if (n == "loop_start" || n == "loopstart") return r.loopStart = L64(v), true;
  if (n == "loop_end" || n == "loopend") return r.loopEnd = L64(v), true;
  if (n == "loop_crossfade") return r.loopCrossfade = F(v), true;
  if (n == "loop_tune" || n == "looptune") return r.loopTune = F(v), true;
  if (n == "loop_count" || n == "loopcount") return r.loopCount = I(v), true;
  if (n == "loop_end_offset") return r.loopEndOffset = I(v), true;
  if (n == "direction") return r.reverse = lower(v) == "reverse", true;
  if (nn == "reverse_locc#" || nn == "reverse_hicc#") {
    Range* g = nullptr;
    for (auto& x : r.reverseCc)
      if (x.cc == num[0]) g = &x;
    if (!g) r.reverseCc.push_back({num[0], 0, 127}), g = &r.reverseCc.back();
    (nn == "reverse_locc#" ? g->lo : g->hi) = F(v);
    return true;
  }
  if (n == "delay") return r.delay = F(v), true;
  if (n == "delay_random") return r.delayRandom = F(v), true;
  if (n == "delay_samples") return r.delaySamples = L64(v), true;
  if (n == "delay_beats") return r.delayBeats = F(v), true;
  if (n == "delay_beats_random") return r.delayBeatsRandom = F(v), true;
  if (n == "stop_beats") return r.stopBeats = F(v), true;
  if (n == "sync_beats") return r.syncBeats = F(v), true;
  if (n == "sync_offset") return r.syncOffset = F(v), true;
  if (n == "sample_fadeout") return r.sampleFadeout = F(v), true;
  if (n == "md5") return r.md5 = lower(v), true;
  if (n == "waveguide") return r.waveguide = B(v), true;
  if (nn == "sample_dyn_param#" || nn == "sample_dyn_param#_oncc#") return true;  // reported (see registry)
  if (n == "oscillator") return r.oscillator = B(v) || lower(v) == "auto", true;
  if (n == "oscillator_mode") return r.oscMode = I(v), true;
  if (n == "oscillator_multi") return r.oscMulti = std::clamp(I(v), 1, 9), true;
  if (n == "oscillator_detune") return r.oscDetune = F(v), true;
  if (n == "oscillator_mod_depth") return r.oscModDepth = F(v), true;
  if (n == "oscillator_phase") return r.oscPhase = F(v), true;
  if (n == "oscillator_quality") return r.oscQuality = I(v), true;
  if (n == "oscillator_table_size") return r.oscTableSize = I(v), true;
  if (n == "sample_quality") return r.sampleQuality = I(v), true;
  if (n == "load_mode" || n == "load_start" || n == "load_end" || n == "image") return true;
  // ------------------------------------------------ mapping
  if (n == "key") {
    const int k = keyOf(c, v);
    r.lokey = r.hikey = r.pitchKeycenter = k;
    return true;
  }
  if (n == "lokey") return r.lokey = keyOf(c, v), true;
  if (n == "hikey") return r.hikey = keyOf(c, v), true;
  if (n == "lovel") return r.lovel = I(v), true;
  if (n == "hivel") return r.hivel = I(v), true;
  if (n == "lochan") return r.lochan = I(v), true;
  if (n == "hichan") return r.hichan = I(v), true;
  if (n == "lobend") return r.lobend = I(v), true;
  if (n == "hibend") return r.hibend = I(v), true;
  if (n == "lochanaft") return r.lochanaft = I(v), true;
  if (n == "hichanaft") return r.hichanaft = I(v), true;
  if (n == "lopolyaft") return r.lopolyaft = I(v), true;
  if (n == "hipolyaft") return r.hipolyaft = I(v), true;
  if (n == "loprog") return r.loprog = I(v), true;
  if (n == "hiprog") return r.hiprog = I(v), true;
  if (n == "lobpm") return r.lobpm = F(v), true;
  if (n == "hibpm") return r.hibpm = F(v), true;
  if (n == "lorand") return r.lorand = F(v), true;
  if (n == "hirand") return r.hirand = F(v), true;
  if (n == "lotimer") return r.lotimer = F(v), true;
  if (n == "hitimer") return r.hitimer = F(v), true;
  if (n == "seq_length") return r.seqLength = std::max(1, I(v)), true;
  if (n == "seq_position") return r.seqPosition = std::max(1, I(v)), true;
  {
    // loccN hiccN lohdccN hihdccN on_/start_/stop_ variants
    struct RangeName {
      const char* lo;
      const char* hi;
      int which;  // 0 range, 1 on, 2 stop
      bool hd;
    };
    static const RangeName k[] = {{"locc#", "hicc#", 0, false},           {"lohdcc#", "hihdcc#", 0, true},
                                  {"on_locc#", "on_hicc#", 1, false},     {"on_lohdcc#", "on_hihdcc#", 1, true},
                                  {"start_locc#", "start_hicc#", 1, false}, {"start_lohdcc#", "start_hihdcc#", 1, true},
                                  {"stop_locc#", "stop_hicc#", 2, false}, {"stop_lohdcc#", "stop_hihdcc#", 2, true}};
    for (const auto& x : k)
      if (nn == x.lo || nn == x.hi) {
        auto& vec = x.which == 0 ? r.ccRange : x.which == 1 ? r.onCc : r.stopCc;
        Range* g = nullptr;
        for (auto& y : vec)
          if (y.cc == num.back()) g = &y;
        if (!g) {
          vec.push_back({num.back(), x.which == 1 ? -1.f : 0.f, 127.f});
          g = &vec.back();
        }
        const float val = x.hd ? F(v) * 127.0f : F(v);
        (nn == x.lo ? g->lo : g->hi) = val;
        if (x.which == 1 && nn == x.hi && g->lo < 0) g->lo = 0;
        return true;
      }
  }
  if (n == "trigger") {
    const std::string s = lower(v);
    r.trigger = s == "release" ? Trigger::Release : s == "release_key" ? Trigger::ReleaseKey : s == "first" ? Trigger::First
                : s == "legato" ? Trigger::Legato : Trigger::Attack;
    return true;
  }
  if (n == "rt_decay") return r.rtDecay = F(v), true;
  if (nn == "rt_decay#" || nn == "rt_decay#_time") {
    const size_t k = static_cast<size_t>(std::clamp(num[0], 1, 16) - 1);
    if (r.rtDecayStages.size() <= k) r.rtDecayStages.resize(k + 1, {0.f, 0.f});
    (nn == "rt_decay#" ? r.rtDecayStages[k].first : r.rtDecayStages[k].second) = F(v);
    return true;
  }
  if (n == "rt_dead") return r.rtDead = B(v), true;
  // keyswitches
  if (n == "sw_lokey") return r.swLokey = swKeyOf(c, v), true;
  if (n == "sw_hikey") return r.swHikey = swKeyOf(c, v), true;
  if (n == "sw_last") return r.swLast = swKeyOf(c, v), true;
  if (n == "sw_lolast") return r.swLolast = swKeyOf(c, v), true;
  if (n == "sw_hilast") return r.swHilast = swKeyOf(c, v), true;
  if (n == "sw_down") return r.swDown = swKeyOf(c, v), true;
  if (n == "sw_up") return r.swUp = swKeyOf(c, v), true;
  if (n == "sw_previous") return r.swPrevious = swKeyOf(c, v), true;
  if (n == "sw_vel") return r.swVelPrevious = lower(v) == "previous", true;
  if (n == "sw_default") return r.swDefault = swKeyOf(c, v), true;
  if (n == "sw_label") return r.swLabel = v, true;
  // pedals
  if (n == "sustain_cc") return r.sustainCc = I(v), true;
  if (n == "sustain_lo") return r.sustainLo = F(v) / 127.0f, true;
  if (n == "sustain_sw") return r.sustainSw = B(v), true;
  if (n == "sostenuto_cc") return r.sostenutoCc = I(v), true;
  if (n == "sostenuto_lo") return r.sostenutoLo = F(v) / 127.0f, true;
  if (n == "sostenuto_sw") return r.sostenutoSw = B(v), true;
  // voices
  if (n == "group" || n == "polyphony_group") return r.group = L64(v), true;
  if (n == "off_by" || n == "offby") return r.offBy = L64(v), true;
  if (n == "off_mode") {
    const std::string s = lower(v);
    r.offMode = s == "normal" ? OffMode::Normal : s == "time" ? OffMode::Time : OffMode::Fast;
    return true;
  }
  if (n == "off_time") return r.offTime = F(v), true;
  if (n == "off_shape") return r.offShape = I(v), true;
  if (n == "off_curve") return r.offCurve = I(v), true;
  if (n == "polyphony") return r.polyphony = I(v), true;
  if (n == "note_polyphony") return r.notePolyphony = I(v), true;
  if (n == "polyphony_stealing") return r.polyphonyStealing = I(v), true;
  if (n == "note_selfmask") return r.noteSelfmask = B(v) || lower(v) == "mask", true;
  if (n == "output") return r.output = I(v), true;
  // ------------------------------------------------ amplifier
  if (n == "volume") return r.volume = F(v), true;
  if (n == "amplitude") return r.amplitude = F(v), true;
  if (n == "pan") return r.pan = F(v), true;
  if (n == "width") return r.width = F(v), true;
  if (n == "position") return r.position = F(v), true;
  if (n == "pan_random") return r.panRandom = F(v), true;
  if (n == "position_random") return r.positionRandom = F(v), true;
  if (n == "position_keycenter") return r.positionKeycenter = keyOf(c, v), true;
  if (n == "position_keytrack") return r.positionKeytrack = F(v), true;
  if (n == "position_veltrack") return r.positionVeltrack = F(v), true;
  if (n == "pan_keycenter") return r.panKeycenter = keyOf(c, v), true;
  if (n == "pan_keytrack") return r.panKeytrack = F(v), true;
  if (n == "pan_veltrack") return r.panVeltrack = F(v), true;
  if (n == "pan_law") return r.panLaw = lower(v) == "mma" ? 1 : 0, true;
  if (n == "amp_keycenter") return r.ampKeycenter = keyOf(c, v), true;
  if (n == "amp_keytrack") return r.ampKeytrack = F(v), true;
  if (n == "amp_veltrack") return r.ampVeltrack = F(v), true;
  if (n == "amp_veltrack_random") return r.ampVeltrackRandom = F(v), true;
  if (n == "amp_random" || n == "gain_random") return r.ampRandom = F(v), true;
  if (nn == "amp_velcurve_#") {
    r.velcurve.emplace_back(std::clamp(num[0], 0, 127), F(v));
    return true;
  }
  if (n == "phase") return r.phaseInvert = lower(v) == "invert", true;
  if (n == "global_volume") return r.globalVolume = F(v), true;
  if (n == "master_volume") return r.masterVolume = F(v), true;
  if (n == "group_volume") return r.groupVolume = F(v), true;
  if (n == "global_amplitude") return r.globalAmplitude = F(v), true;
  if (n == "master_amplitude") return r.masterAmplitude = F(v), true;
  if (n == "group_amplitude") return r.groupAmplitude = F(v), true;
  // crossfades
  if (n == "xfin_lokey") return r.xfinLokey = keyOf(c, v), true;
  if (n == "xfin_hikey") return r.xfinHikey = keyOf(c, v), true;
  if (n == "xfout_lokey") return r.xfoutLokey = keyOf(c, v), true;
  if (n == "xfout_hikey") return r.xfoutHikey = keyOf(c, v), true;
  if (n == "xfin_lovel") return r.xfinLovel = I(v), true;
  if (n == "xfin_hivel") return r.xfinHivel = I(v), true;
  if (n == "xfout_lovel") return r.xfoutLovel = I(v), true;
  if (n == "xfout_hivel") return r.xfoutHivel = I(v), true;
  if (nn == "xfin_locc#" || nn == "xfin_hicc#" || nn == "xfout_locc#" || nn == "xfout_hicc#") {
    const bool in = nn.rfind("xfin", 0) == 0;
    Region::XfCc* g = nullptr;
    for (auto& x : r.xfCc)
      if (x.cc == num[0] && x.in == in) g = &x;
    if (!g) r.xfCc.push_back({num[0], 0, 0, in}), g = &r.xfCc.back();
    (nn.find("_lo") != std::string::npos ? g->lo : g->hi) = F(v);
    return true;
  }
  if (n == "xf_keycurve") return r.xfKeyPower = lower(v) != "gain", true;
  if (n == "xf_velcurve") return r.xfVelPower = lower(v) != "gain", true;
  if (n == "xf_cccurve") return r.xfCcPower = lower(v) != "gain", true;
  // ------------------------------------------------ pitch
  if (n == "tune" || n == "pitch") return r.tune = F(v), true;
  if (n == "global_tune") return r.globalTune = F(v), true;
  if (n == "master_tune") return r.masterTune = F(v), true;
  if (n == "group_tune") return r.groupTune = F(v), true;
  if (n == "transpose") return r.transpose = I(v), true;
  if (n == "pitch_keycenter") {
    if (lower(v) == "sample") r.pitchKeycenter = -2;
    else r.pitchKeycenter = keyOf(c, v);
    return true;
  }
  if (n == "pitch_keytrack" || n == "tune_keytrack") return r.pitchKeytrack = F(v), true;
  if (n == "pitch_veltrack" || n == "tune_veltrack") return r.pitchVeltrack = F(v), true;
  if (n == "pitch_random" || n == "tune_random") return r.pitchRandom = F(v), true;
  if (n == "bend_up" || n == "bendup") return r.bendUp = F(v), true;
  if (n == "bend_down" || n == "benddown") return r.bendDown = F(v), true;
  if (n == "bend_step" || n == "bendstep") return r.bendStep = F(v), true;
  if (n == "bend_smooth") return r.bendSmooth = F(v), true;
  if (n == "bend_stepup") return r.bendStepUp = F(v), true;
  if (n == "bend_stepdown") return r.bendStepDown = F(v), true;
  // ------------------------------------------------ filters
  if (n.rfind("fil", 0) == 0 || n.rfind("cutoff", 0) == 0 || n.rfind("resonance", 0) == 0) {
    int fi = 0;
    std::string rest;
    if (n.rfind("fil2_", 0) == 0) fi = 1, rest = n.substr(5);
    else if (n.rfind("fil_", 0) == 0) rest = n.substr(4);
    else if (n == "filtype") rest = "type";
    else if (n == "cutoff") rest = "cutoff";
    else if (n == "cutoff2") fi = 1, rest = "cutoff";
    else if (n == "resonance") rest = "resonance";
    else if (n == "resonance2") fi = 1, rest = "resonance";
    else if (n == "cutoff_random") rest = "random";
    else if (n == "cutoff2_random") fi = 1, rest = "random";
    else if (n == "resonance_random") rest = "resonance_random";
    else if (n == "resonance2_random") fi = 1, rest = "resonance_random";
    Filter& f = r.filter[static_cast<size_t>(fi)];
    if (rest == "type") {
      f.type = filterTypeOf(v);
      if (f.type < 0) c.ins->report.warnings.push_back("filter type " + v + " plays as a two-pole low-pass");
      if (f.type < 0) f.type = 2;
      return true;
    }
    if (rest == "cutoff") {
      f.cutoff = F(v);
      if (f.type < 0) f.type = 2;
      return true;
    }
    if (rest == "resonance") return f.resonance = F(v), true;
    if (rest == "gain") return f.gain = F(v), true;
    if (rest == "keytrack") return f.keytrack = F(v), true;
    if (rest == "keycenter") return f.keycenter = keyOf(c, v), true;
    if (rest == "veltrack") return f.veltrack = F(v), true;
    if (rest == "random") return f.random = F(v), true;
    if (rest == "resonance_random") return f.resonanceRandom = F(v), true;
    if (rest == "mode") return f.mode = lower(v) == "hz" ? 1 : 0, true;
  }
  // EQ
  if (nn.rfind("eq#", 0) == 0 && !num.empty() && num[0] >= 1 && num[0] <= 3) {
    Eq& e = r.eq[static_cast<size_t>(num[0] - 1)];
    std::string rest = n.substr(3);
    if (!rest.empty() && rest[0] == '_') rest.erase(0, 1);
    e.used = true;
    if (rest == "freq") return e.freq = F(v), true;
    if (rest == "bw") return e.bw = F(v), true;
    if (rest == "gain") return e.gain = F(v), true;
    if (rest == "vel2freq") return e.vel2freq = F(v), true;
    if (rest == "vel2gain") return e.vel2gain = F(v), true;
    if (rest == "dynamic") return e.dynamic = B(v), true;
    if (rest == "type") {
      const std::string s = lower(v);
      e.type = s == "lshelf" ? 1 : s == "hshelf" ? 2 : 0;
      return true;
    }
    if (rest == "mode") return true;  // mapped: one EQ mode
  }
  // noise
  if (n == "noise_level") return r.noiseLevel = F(v), true;
  if (n == "noise_step") return r.noiseStep = F(v), true;
  if (n == "noise_tone") return r.noiseTone = F(v), true;
  if (n == "noise_stereo") return r.noiseStereo = B(v), true;
  if (n == "noise_filter") return r.noiseFilter = B(v) ? 1.f : 0.f, true;
  if (n == "decim") return r.decim = F(v), true;
  if (n == "bitred") return r.bitred = F(v), true;
  // ------------------------------------------------ envelopes
  {
    struct EgName {
      const char* p;
      ClassicEg Region::*eg;
      bool alias;  // amp_ fil_ pitch_: only the stage and vel2 names
    };
    static const EgName k[] = {{"ampeg_", &Region::ampeg, false}, {"fileg_", &Region::fileg, false}, {"pitcheg_", &Region::pitcheg, false},
                               {"amp_", &Region::ampeg, true},    {"fil_", &Region::fileg, true},    {"pitch_", &Region::pitcheg, true}};
    for (const auto& x : k) {
      const size_t L = std::strlen(x.p);
      if (n.compare(0, L, x.p) == 0) {
        const std::string rest = n.substr(L);
        if (x.alias && rest.rfind("vel2", 0) != 0 && egStageOf(rest) < 0) continue;
        if (classicEg(r.*(x.eg), rest, n, v)) return true;
      }
    }
  }
  // flex envelopes egN_
  if (nn.rfind("eg#_", 0) == 0) {
    FlexEg& e = egAt(r, num[0]);
    const std::string rest = n.substr(n.find('_') + 1);
    const std::string rn = normalName(rest);
    auto point = [&](int k) -> FlexEg::Point& {
      k = std::clamp(k, 0, 63);
      if (static_cast<int>(e.points.size()) <= k) e.points.resize(static_cast<size_t>(k + 1));
      return e.points[static_cast<size_t>(k)];
    };
    if (rest == "points") return point(std::max(0, I(v) - 1)), true;
    if (rn == "time#") return point(num[1]).time = F(v), true;
    if (rn == "level#") return point(num[1]).level = F(v), true;
    if (rn == "shape#") return point(num[1]).shape = F(v), true;
    if (rn == "curve#") return point(num[1]).shape = -1000.f - F(v), true;  // a <curve> for the segment
    if (rest == "sustain") return e.sustain = I(v), true;
    if (rest == "loop") return e.loop = I(v), true;
    if (rest == "loop_count") return e.loopCount = I(v), true;
    if (rest == "dynamic") return e.dynamic = B(v), true;
    if (rest == "ampeg") return e.ampeg = B(v), true;
    if (rn == "time#_oncc#" || rn == "level#_oncc#") {
      addCc(e.pointCc, Target::EgTime, num[1] * 2 + (rn[0] == 'l' ? 1 : 0), num[2], 0, F(v));
      return true;
    }
    if (rest == "pan_curve") return true;  // mapped: the pan law
    std::string base;
    int cc = 0, kind = 0;
    if (ccSuffix(rest, base, cc, kind)) {
      if (base == "pan_curve") return true;
      if (normalName(base) == "sample_dyn_param#") return true;
      std::string b = base;
      if (b.rfind("eq", 0) == 0 && b.size() > 3 && b[3] != '_') b.insert(3, "_");
      if (modTarget(b, t, ti)) {
        int d = destIndex(e.dest, t, ti);
        if (d < 0) e.dest.push_back({t, ti, 0}), d = static_cast<int>(e.dest.size()) - 1;
        addCc(e.depthCc, t, d, cc, kind, F(v));
        return true;
      }
    }
    if (rn == "freq_lfo#" || rn == "depth_lfo#" || rn == "depthadd_lfo#") {
      setDest(e.dest, rn[0] == 'f' ? Target::LfoFreq : rn[5] == 'a' ? Target::LfoDepthAdd : Target::LfoDepth, num[1], F(v));
      return true;
    }
    if (normalName(rest) == "sample_dyn_param#") return true;
    std::string b = rest;
    if (b.rfind("eq", 0) == 0 && b.size() > 3 && b[3] != '_') b.insert(3, "_");
    if (modTarget(b, t, ti)) return setDest(e.dest, t, ti, F(v)), true;
    return false;
  }
  // v1 LFOs
  {
    struct LfoName {
      const char* p;
      Lfo Region::*l;
      Target t;
    };
    static const LfoName k[] = {{"amplfo_", &Region::amplfo, Target::Volume}, {"fillfo_", &Region::fillfo, Target::Cutoff},
                                {"pitchlfo_", &Region::pitchlfo, Target::Pitch}};
    for (const auto& x : k) {
      const size_t L = std::strlen(x.p);
      if (n.compare(0, L, x.p) != 0) continue;
      Lfo& l = r.*(x.l);
      l.used = l.v1 = true;
      const std::string rest = n.substr(L);
      if (rest == "freq") return l.freq = F(v), true;
      if (rest == "delay") return l.delay = F(v), true;
      if (rest == "fade") return l.fade = F(v), true;
      if (rest == "depth") return setDest(l.dest, x.t, 0, F(v)), true;
      std::string base;
      int cc = 0, kind = 0;
      if (ccSuffix(rest, base, cc, kind)) {
        if (base == "freq") return addCc(l.cc, Target::LfoFreq, 0, cc, kind, F(v)), true;
        if (base == "depth") {
          if (destIndex(l.dest, x.t, 0) < 0) l.dest.push_back({x.t, 0, 0});
          addCc(l.cc, x.t, 0, cc, kind, F(v));
          return true;
        }
      }
      return false;
    }
  }
  // v2 LFOs lfoN_
  if (nn.rfind("lfo#_", 0) == 0) {
    Lfo& l = lfoAt(r, num[0]);
    const std::string rest = n.substr(n.find('_') + 1);
    const std::string rn = normalName(rest);
    if (rest == "freq") return l.freq = F(v), true;
    if (rest == "delay") return l.delay = F(v), true;
    if (rest == "fade") return l.fade = F(v), true;
    if (rest == "phase") return l.phase = F(v), true;
    if (rest == "count") return l.count = I(v), true;
    if (rest == "wave") return l.wave = I(v), true;
    if (rest == "steps") return l.steps.resize(static_cast<size_t>(std::clamp(I(v), 0, 128)), 0.f), true;
    if (rn == "step#") {
      const size_t k = static_cast<size_t>(std::clamp(num[1], 1, 128) - 1);
      if (l.steps.size() <= k) l.steps.resize(k + 1, 0.f);
      l.steps[k] = F(v) / 100.0f;
      return true;
    }
    if (rest == "smooth") return l.smooth = F(v), true;
    if (rest == "offset") return l.offset = F(v), true;
    if (rest == "ratio") return l.ratio = F(v), true;
    if (rest == "scale") return l.scale = F(v), true;
    auto sub = [&](std::vector<float>& vec, float def) -> float& {
      const size_t k = static_cast<size_t>(std::clamp(num[1], 2, 9) - 2);
      if (vec.size() <= k) vec.resize(k + 1, def);
      return vec[k];
    };
    if (rn == "wave#") {
      const size_t k = static_cast<size_t>(std::clamp(num[1], 2, 9) - 2);
      if (l.subWaves.size() <= k) l.subWaves.resize(k + 1, 1);
      l.subWaves[k] = I(v);
      return true;
    }
    if (rn == "offset#") return sub(l.subOffsets, 0.f) = F(v), true;
    if (rn == "ratio#") return sub(l.subRatios, 1.f) = F(v), true;
    if (rn == "scale#") return sub(l.subScales, 1.f) = F(v), true;
    if (rn == "freq_lfo#" || rn == "depth_lfo#" || rn == "depthadd_lfo#") {
      setDest(l.dest, rn[0] == 'f' ? Target::LfoFreq : rn[5] == 'a' ? Target::LfoDepthAdd : Target::LfoDepth, num[1], F(v));
      return true;
    }
    if (rn == "freq_lfo#_oncc#") return addCc(l.cc, Target::LfoFreq, num[1], num[2], 0, F(v)), true;
    if (normalName(rest).rfind("sample_dyn_param#", 0) == 0) return true;
    std::string base;
    int cc = 0, kind = 0;
    if (ccSuffix(rest, base, cc, kind)) {
      if (base == "freq") return addCc(l.cc, Target::LfoFreq, -1, cc, kind, F(v)), true;
      if (base == "delay" || base == "fade" || base == "phase" || base == "wave" || base == "smooth")
        return addCc(l.cc, Target::LfoDepthAdd, 1000 + egStageOf(base) + (base == "fade" ? 100 : base == "phase" ? 200 : base == "wave" ? 300 : base == "smooth" ? 400 : 0), cc, kind, F(v)), true;
      if (normalName(base) == "step#") return addCc(l.cc, Target::LfoDepthAdd, 2000 + numbersIn(base)[0], cc, kind, F(v)), true;
      std::string b = base;
      if (b.rfind("eq", 0) == 0 && b.size() > 3 && b[3] != '_') b.insert(3, "_");
      if (modTarget(b, t, ti)) {
        int d = destIndex(l.dest, t, ti);
        if (d < 0) l.dest.push_back({t, ti, 0}), d = static_cast<int>(l.dest.size()) - 1;
        addCc(l.cc, t, d, cc, kind, F(v));
        return true;
      }
      return false;
    }
    std::string b = rest;
    if (b.rfind("eq", 0) == 0 && b.size() > 3 && b[3] != '_') b.insert(3, "_");
    if (modTarget(b, t, ti)) return setDest(l.dest, t, ti, F(v)), true;
    return false;
  }
  // ARIA variables varNN_
  if (nn.rfind("var#_", 0) == 0) {
    const int idx = num[0];
    Region::Var* var = nullptr;
    for (auto& x : r.vars)
      if (x.index == idx) var = &x;
    if (!var) r.vars.push_back({idx, 0, {}, {}}), var = &r.vars.back();
    const std::string rest = n.substr(n.find('_') + 1);
    if (rest == "mod") return var->mod = lower(v) == "mult" ? 1 : 0, true;
    std::string base;
    int cc = 0, kind = 0;
    if (ccSuffix(rest, base, cc, kind) && base.empty()) {
      if (kind == 0) var->cc.emplace_back(cc, F(v));
      return true;
    }
    if (normalName(rest) == "curvecc#") return true;  // the variable's controller curve: linear
    if (normalName(rest) == "oncc#") return var->cc.emplace_back(num.back(), F(v)), true;
    std::string b = rest;
    if (b.rfind("eq", 0) == 0 && b.size() > 3 && b[3] != '_') b.insert(3, "_");
    if (modTarget(b, t, ti)) return var->dest.emplace_back(t, F(v)), true;
    return true;  // the rest of the variable targets mapped to their nearest Gridline target (see registry)
  }
  // effect sends
  if (nn == "effect#" && num[0] >= 1 && num[0] <= 4) return r.effectSend[static_cast<size_t>(num[0] - 1)] = F(v), true;
  // labels and hints
  if (n == "global_label") return r.globalLabel = v, true;
  if (n == "master_label") return r.masterLabel = v, true;
  if (n == "group_label") return r.groupLabel = v, true;
  if (n == "region_label") return r.regionLabel = v, true;
  if (n.rfind("hint_", 0) == 0) return c.ins->hints[n] = v, true;
  if (n.size() > 4 && n.compare(n.size() - 4, 4, "_mod") == 0) return true;  // ARIA: modulations add
  // ------------------------------------------------ controller modulations
  std::string base;
  int cc = 0, kind = 0;
  if (ccSuffix(n, base, cc, kind)) {
    if (targetOf(base, t, ti)) return addCc(r.cc, t, ti, cc, kind, F(v)), true;
    if (base == "filter_cutoff" || base == "filter_resonance") return true;  // effect opcodes outside <effect>
  }
  return false;
}

// Control header opcodes.
bool controlOp(Ctx& c, const std::string& nameIn, const std::string& v) {
  const std::string n = lower(nameIn);
  const std::vector<int> num = numbersIn(n);
  const std::string nn = normalName(n);
  if (n == "default_path") {
    std::string p = v;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (!p.empty() && p.back() != '/') p += '/';
    c.defaultPath = p;
    return true;
  }
  if (n == "note_offset") return c.noteOffset = I(v), true;
  if (n == "octave_offset") return c.octaveOffset = I(v), true;
  if (n == "sw_note_offset") return c.swNoteOffset = I(v), true;
  if (n == "sw_octave_offset") return c.swOctaveOffset = I(v), true;
  if (nn == "set_cc#" || nn == "set_realcc#") {
    if (num[0] < kCcCount) c.ins->setCc[static_cast<size_t>(num[0])] = std::clamp(F(v) / 127.0f, 0.f, 1.f);
    return true;
  }
  if (nn == "set_hdcc#") {
    if (num[0] < kCcCount) c.ins->setCc[static_cast<size_t>(num[0])] = std::clamp(F(v), 0.f, 1.f);
    return true;
  }
  if (nn == "label_cc#") return c.ins->ccLabels[num[0]] = v, true;
  if (n == "image") return c.ins->image = v, true;
  if (n.rfind("hint_", 0) == 0) return c.ins->hints[n] = v, true;
  if (n == "sample_quality") return c.ins->sampleQuality = I(v), true;
  if (n == "load_mode") return c.ins->loadMode = I(v), true;
  return false;
}

// Splits text into headers and opcodes. A value runs to the next "name=" on the line (sample
// names may hold spaces), a header or the line end.
void tokenize(const std::string& text, std::vector<std::pair<Hd, std::vector<Op>>>& blocks, Report& rep, std::vector<std::string>& headerNames) {
  auto isNameChar = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$' || ch == '#' || ch == '*'; };
  size_t i = 0;
  const size_t N = text.size();
  Hd cur = Hd::None;
  blocks.push_back({Hd::None, {}});
  headerNames.push_back("");
  while (i < N) {
    const char ch = text[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    if (ch == '/' && i + 1 < N && text[i + 1] == '*') {  // block comment
      const size_t e = text.find("*/", i + 2);
      i = e == std::string::npos ? N : e + 2;
      continue;
    }
    if (ch == '<') {
      const size_t e = text.find('>', i);
      if (e == std::string::npos) break;
      const std::string h = lower(text.substr(i + 1, e - i - 1));
      i = e + 1;
      cur = h == "control" ? Hd::Control : h == "global" ? Hd::Global : h == "master" ? Hd::Master : h == "group" ? Hd::Group
            : h == "region" ? Hd::Region : h == "curve" ? Hd::Curve : h == "effect" ? Hd::Effect : h == "sample" ? Hd::Sample
            : h == "midi" ? Hd::Midi : Hd::None;
      if (!knownHeader(h)) ++rep.unknownHeaders[h];
      blocks.push_back({cur, {}});
      headerNames.push_back(h);
      continue;
    }
    // name
    size_t s = i;
    while (i < N && isNameChar(text[i])) ++i;
    if (i == s || i >= N || text[i] != '=') {  // stray text: skip the word
      while (i < N && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '<') ++i;
      if (i == s) ++i;
      continue;
    }
    Op op;
    op.name = text.substr(s, i - s);
    ++i;  // '='
    // <sample> data= runs to the end of the block (embedded audio)
    size_t lineEnd = text.find_first_of("\r\n", i);
    if (lineEnd == std::string::npos) lineEnd = N;
    size_t e = lineEnd;
    for (size_t k = i; k < lineEnd; ++k) {
      if (text[k] == '<') {
        e = k;
        break;
      }
      if (std::isspace(static_cast<unsigned char>(text[k])) && k + 1 < lineEnd) {
        size_t q = k + 1;
        while (q < lineEnd && std::isspace(static_cast<unsigned char>(text[q]))) ++q;
        size_t w = q;
        while (w < lineEnd && isNameChar(text[w])) ++w;
        if (w > q && w < lineEnd && text[w] == '=') {
          e = k;
          break;
        }
        if (q < lineEnd && text[q] == '/' && q + 1 < lineEnd && text[q + 1] == '*') {
          e = k;
          break;
        }
      }
    }
    std::string val = text.substr(i, e - i);
    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
    op.value = val;
    blocks.back().second.push_back(std::move(op));
    i = e;
  }
}

}  // namespace

bool loadText(const std::string& text, const std::string& path, Instrument& out) {
  out = Instrument{};
  out.path = path;
  out.setCc.fill(-1.0f);
  for (int k = 0; k < 256; ++k) out.curves[static_cast<size_t>(k)].used = false;
  Ctx c;
  c.ins = &out;
  c.base = fs::u8path(path).parent_path();
  std::vector<std::pair<Hd, std::vector<Op>>> blocks;
  std::vector<std::string> headerNames;
  tokenize(text, blocks, out.report, headerNames);
  std::vector<Op> global, master, group;
  std::map<std::string, WavMeta> metas;
  // An opcode the reader did not take: unknown, or (a gap in this reader) known but not taken.
  auto known = [&](const Op& op) {
    if (const OpcodeInfo* o = findOpcode(op.name)) {
      if (o->support == Support::Reported) ++out.report.reported[op.name];
      else ++out.report.unhandled[op.name];
    } else {
      ++out.report.unknown[op.name];
    }
  };
  // Taken opcodes that have no Gridline counterpart are listed too.
  auto taken = [&](const Op& op) {
    if (const OpcodeInfo* o = findOpcode(op.name))
      if (o->support == Support::Reported) ++out.report.reported[op.name];
  };
  for (auto& blk : blocks) {
    const Hd hd = blk.first;
    const std::vector<Op>& ops = blk.second;
    switch (hd) {
      case Hd::Control:
        for (const auto& op : ops)
          if (!controlOp(c, op.name, op.value)) {
            Region scratch;
            if (!regionOp(c, scratch, op.name, op.value)) known(op);
            else taken(op);
          }
        break;
      case Hd::Global:
        global = ops, master.clear(), group.clear();
        break;
      case Hd::Master:
        master = ops, group.clear();
        ++out.report.masters;
        break;
      case Hd::Group:
        group = ops;
        ++out.report.groups;
        break;
      case Hd::Region: {
        Region r;
        auto apply = [&](const std::vector<Op>& v) {
          for (const auto& op : v) {
            if (!regionOp(c, r, op.name, op.value) && !controlOp(c, op.name, op.value)) known(op);
            else if (&v == &ops) taken(op);
          }
        };
        // header-level ARIA volumes and labels belong to their own level
        apply(global);
        apply(master);
        apply(group);
        apply(ops);
        if (r.sample.empty()) break;  // a region without a sample plays nothing
        if (r.builtin == Builtin::None) {
          auto it = metas.find(r.file);
          if (it == metas.end()) it = metas.emplace(r.file, wavMeta(r.file)).first;
          const WavMeta& m = it->second;
          if (r.pitchKeycenter == -2) r.pitchKeycenter = m.root >= 0 ? m.root : 60;
          if (r.loopMode == LoopMode::Unset) r.loopMode = m.loopStart >= 0 ? LoopMode::Continuous : LoopMode::NoLoop;
          if (r.loopStart < 0 && m.loopStart >= 0) r.loopStart = m.loopStart;
          if (r.loopEnd < 0 && m.loopEnd >= 0) r.loopEnd = m.loopEnd + r.loopEndOffset;
          std::error_code ec;
          if (!fs::is_regular_file(fs::u8path(r.file), ec)) {
            if (std::find(out.report.missingSamples.begin(), out.report.missingSamples.end(), r.file) == out.report.missingSamples.end())
              out.report.missingSamples.push_back(r.file);
          }
        } else {
          if (r.pitchKeycenter == -2) r.pitchKeycenter = 60;
          if (r.loopMode == LoopMode::Unset) r.loopMode = r.builtin == Builtin::Silence ? LoopMode::NoLoop : LoopMode::Continuous;
        }
        if (r.count > 0) r.loopMode = LoopMode::OneShot;
        if (r.swDefault >= 0 && out.swDefault < 0) out.swDefault = r.swDefault;
        if (r.swLast >= 0 || r.swLolast >= 0 || r.swDown >= 0 || r.swUp >= 0) {
          out.swLokey = std::min(out.swLokey, r.swLokey);
          out.swHikey = std::max(out.swHikey, r.swHikey);
          const int k = r.swLast >= 0 ? r.swLast : r.swDown >= 0 ? r.swDown : r.swLolast;
          if (!r.swLabel.empty() && k >= 0) out.swLabels[k] = r.swLabel;
        }
        std::sort(r.velcurve.begin(), r.velcurve.end());
        out.regions.push_back(std::move(r));
        break;
      }
      case Hd::Curve: {
        int idx = -1;
        std::array<float, 128> pts;
        pts.fill(-1.f);
        for (const auto& op : ops) {
          const std::string n = lower(op.name);
          if (n == "curve_index") idx = I(op.value);
          else if (n.size() >= 2 && n[0] == 'v' && std::isdigit(static_cast<unsigned char>(n[1]))) pts[static_cast<size_t>(std::clamp(std::atoi(n.c_str() + 1), 0, 127))] = F(op.value);
          else known(op);
        }
        if (idx < 0 || idx > 255) break;
        if (pts[0] < 0) pts[0] = 0;
        if (pts[127] < 0) pts[127] = 1;
        Curve& cv = out.curves[static_cast<size_t>(idx)];
        int last = 0;
        for (int k = 1; k < 128; ++k)
          if (pts[static_cast<size_t>(k)] >= 0) {
            for (int j = last; j <= k; ++j)
              cv.v[static_cast<size_t>(j)] = pts[static_cast<size_t>(last)] + (pts[static_cast<size_t>(k)] - pts[static_cast<size_t>(last)]) * static_cast<float>(j - last) / static_cast<float>(std::max(1, k - last));
            last = k;
          }
        cv.used = true;
        break;
      }
      case Hd::Effect: {
        EffectDef e;
        for (const auto& op : ops) {
          const std::string n = lower(op.name);
          if (n == "bus") e.bus = lower(op.value);
          else if (n == "type") e.type = lower(op.value);
          else if (n == "directtomain") e.directToMain = F(op.value);
          else if (n == "dsp_order") e.dspOrder = I(op.value);
          else if (normalName(n) == "fx#tomain" && !numbersIn(n).empty() && numbersIn(n)[0] >= 1 && numbersIn(n)[0] <= 4) e.fxToMain[numbersIn(n)[0] - 1] = F(op.value);
          else if (normalName(n) == "fx#tomix" && !numbersIn(n).empty() && numbersIn(n)[0] >= 1 && numbersIn(n)[0] <= 4) e.fxToMix[numbersIn(n)[0] - 1] = F(op.value);
          else {
            e.params[n] = op.value;
            if (!findOpcode(op.name)) ++out.report.unknown[op.name];
            else taken(op);
          }
        }
        out.effects.push_back(std::move(e));
        break;
      }
      case Hd::Sample:
        for (const auto& op : ops) taken(op);
        out.report.warnings.push_back("embedded <sample> data is not read; regions name their sample files");
        break;
      case Hd::Midi:
        for (const auto& op : ops) taken(op);
        if (!ops.empty()) ++out.report.reported["<midi>"];
        break;
      case Hd::None:
        for (const auto& op : ops) {  // opcodes before any header: as in <global>
          if (!controlOp(c, op.name, op.value)) global.push_back(op);
        }
        break;
    }
  }
  out.report.regions = static_cast<int>(out.regions.size());
  {
    std::vector<std::string> files;
    for (const auto& r : out.regions)
      if (!r.file.empty()) files.push_back(r.file);
    std::sort(files.begin(), files.end());
    out.report.samples = static_cast<int>(std::unique(files.begin(), files.end()) - files.begin());
  }
  (void)headerNames;
  return true;
}

bool load(const std::string& path, Instrument& out) {
  Expanded ex;
  if (!expand(path, ex)) {
    out = Instrument{};
    out.path = path;
    return false;
  }
  const bool ok = loadText(ex.text, path, out);
  out.report.included = ex.included;
  out.report.missingIncludes = ex.missing;
  if (ex.looped) out.report.warnings.push_back("an #include led back to a file already being read; it was skipped");
  return ok;
}

}  // namespace gl::sfz
