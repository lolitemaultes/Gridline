// Gridline — FLP export: plugin state encoders for FlpGenOsc/Samplers/Simple/Synths/Sytrus/Harmor/Harmless/Gms.cpp.
// The reverse of the decoders in those files; see FlpEncode.h for the model.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include "FlpEncode.h"
#include "FlpGen.h"
#include "../PluginParams.h"
#include "../../packs/synths/matrixfm.h"
#include "../../packs/models/harmor.h"
#include "../../packs/models/models.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/samplers/samplers.h"
#include "../../packs/synths/gms.h"
#include "../../packs/synths/synths.h"

namespace gl::flp::enc {

namespace {

using namespace gl::pp;

// Parameter k of a state (NaN when absent).
float pv(const PluginState& s, int k) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : std::numeric_limits<float>::quiet_NaN();
}
// True when any of the params differ between a and b.
bool differ(const PluginState& a, const PluginState& b, std::initializer_list<int> ks) {
  for (int k : ks)
    if (!(pv(a, k) == pv(b, k))) return true;
  return false;
}
int32_t geti(const Bytes& b, size_t at) { return static_cast<int32_t>(getU32(b, at)); }

// Decoded value of param k of state b (NaN when it does not decode).
float decoded(const std::string& fl, const Bytes& b, int k, const EncodeContext& c) {
  PluginMapping m;
  return decodeState(fl, b, m, c) ? pv(m.state, k) : std::numeric_limits<float>::quiet_NaN();
}

// Bisection of the i32 at `at` (lo..hi, decoded value non-decreasing in it)
// for param k = want. Leaves the closest value written; true when exact.
bool bisect(const std::string& fl, Bytes& b, size_t at, int lo, int hi, int k, float want, const EncodeContext& c) {
  auto val = [&](int r) {
    putI32(b, at, r);
    return decoded(fl, b, k, c);
  };
  int best = lo;
  float bestErr = HUGE_VALF;
  auto consider = [&](int r, float v) {
    const float err = std::fabs(v - want);
    if (err < bestErr) bestErr = err, best = r;
    return v == want;
  };
  if (consider(lo, val(lo)) || consider(hi, val(hi))) {
    putI32(b, at, best);
    return true;
  }
  int a = lo, z = hi;
  while (z - a > 1) {
    const int mid = a + (z - a) / 2;
    const float v = val(mid);
    if (!std::isfinite(v)) break;
    if (consider(mid, v)) break;
    (v < want ? a : z) = mid;
  }
  putI32(b, at, best);
  return bestErr == 0.0f;
}
void pstr(ByteWriter& w, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 255);
  w.u8(static_cast<uint32_t>(n));
  w.bytes(s.data(), n);
}

// =================================================================== 3x Osc
// Layout (FlpGenOsc.cpp): dword version, 21 dwords in control-tag order
// (osc 1: pan, shape, coarse, fine, phase, detune; osc 2 / 3: mix then the
// same six; phase randomness), v12+: 4 switch bytes (invert 1..3, osc 3 AM),
// v13+: HQ byte, v14+: 3 pad bytes, a copy of the five channel envelope
// records (5 × 68 bytes) and of the channel filter levels (6 dwords:
// 0, 0, cutoff, resonance, 0, type). Early presets: 11 dwords, no version.
namespace osc {

constexpr size_t kEnv = 96, kRec = 68, kLev = 436, kSize14 = 460;
size_t rec(int target) { return kEnv + kRec * static_cast<size_t>(target); }
// Channel envelope record fields (dword index × 4).
enum : size_t { RFlag = 0, REnabled = 4, RPredelay = 8, RAttack = 12, RHold = 16, RDecay = 20, RSustain = 24, RRelease = 28, RAmount = 32,
                RLfoPredelay = 36, RLfoAttack = 40, RLfoAmount = 44, RLfoSpeed = 48, RLfoShape = 52, RTension = 56 };
constexpr size_t kLevCutoff = kLev + 8, kLevReso = kLev + 12, kLevType = kLev + 20;

// Offset of control tag i (0..20) in the state (npos when absent).
size_t tagOffset(const Bytes& b, int i) {
  if (b.size() == 44) {  // early 11-value layout
    static const int to[11] = {1, 2, 3, 6, 8, 9, 10, 13, 15, 16, 17};
    for (int j = 0; j < 11; ++j)
      if (to[j] == i) return static_cast<size_t>(j) * 4;
    return std::string::npos;
  }
  const size_t at = 4 + static_cast<size_t>(i) * 4;
  return at + 4 <= b.size() ? at : std::string::npos;
}

// The LFO the decoder reads: the first channel LFO in use in this order.
struct LfoCand {
  int rec, trio;
};
constexpr LfoCand kLfo[4] = {{1, 3}, {2, 2}, {4, 1}, {0, 4}};  // volume, mod X, pitch, panning

int activeLfo(const Bytes& b) {
  for (int k = 0; k < 4; ++k)
    if (geti(b, rec(kLfo[k].rec) + RLfoAmount) != 0) return k;
  return -1;
}

Bytes defaults() {
  ByteWriter w;
  w.i32(14);  // version FL 21+ writes
  // Control tags 0..20 at FL's defaults: osc 2 mix 64, one octave down; osc 3
  // mix 32, two octaves down; everything else centred / zero.
  const int p[21] = {0, 0, 0, 0, 0, 0, /*osc 2*/ 64, 0, 0, -12, 0, 0, 0, /*osc 3*/ 32, 0, 0, -24, 0, 0, 0, /*phase rand*/ 0};
  for (int v : p) w.i32(v);
  w.zeros(4);  // invert 1..3, osc 3 AM: off
  w.u8(0);     // HQ off
  w.zeros(3);  // pad
  for (int t = 0; t < 5; ++t) {  // channel envelope records, disabled, at the channel defaults
    w.i32(t == 1 ? 4 : 0);  // record flag word (4 on the volume record in every FL-written state)
    w.i32(0);               // enabled
    w.i32(100);             // predelay
    w.i32(20000), w.i32(20000), w.i32(30000);  // attack, hold, decay
    w.i32(50);                                 // sustain
    w.i32(20000);                              // release
    w.i32(0);                                  // amount
    w.i32(100), w.i32(20000);                  // LFO predelay, attack
    w.i32(0), w.i32(32950), w.i32(0);          // LFO amount, speed, shape
    w.i32(0), w.i32(0), w.i32(t == 1 ? -101 : 0);  // attack / decay / release tension
  }
  w.i32(0), w.i32(0);    // levels copy: unused words
  w.i32(256), w.i32(0);  // cutoff open, no resonance
  w.i32(0), w.i32(0);    // unused, filter type
  return w.b;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  auto tag = [&](int i, int gl, double lo, double hi) {
    const size_t at = tagOffset(b, i);
    if (at != std::string::npos) f.push_back(i32(gl, at, lo, hi));
  };
  const int shapeIdx[3] = {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape};
  const int coarseIdx[3] = {SynOsc1Coarse, SynOsc2Coarse, SynOsc3Coarse};
  const int fineIdx[3] = {SynOsc1Fine, SynOsc2Fine, SynOsc3Fine};
  const int levelIdx[3] = {SynOsc1Level, SynOsc2Level, SynOsc3Level};
  for (int k = 0; k < 3; ++k) {
    const int at = k == 0 ? 0 : 6 + (k - 1) * 7 + 1;
    if (k > 0) tag(at - 1, levelIdx[k], 0, 128);
    tag(at, SynOsc1Pan + k, -64, 64);
    tag(at + 1, shapeIdx[k], 0, 6);
    tag(at + 2, coarseIdx[k], -24, 24);
    tag(at + 3, fineIdx[k], -100, 100);
    tag(at + 4, SynOsc1Phase + k, -64, 64);
    tag(at + 5, SynOsc1Detune + k, -50, 50);
  }
  tag(20, SynPhaseRand, 0, 64);
  const int version = b.size() == 44 ? 0 : geti(b, 0);
  if (version >= 12 && b.size() >= 92) {
    for (int k = 0; k < 3; ++k) f.push_back(field(SynOsc1Invert + k, 88 + static_cast<size_t>(k), Type::U8, 0, 1));
    f.push_back(field(SynOsc3Mode, 91, Type::U8, 0, 1));
  }
  if (version >= 13 && b.size() >= 93) f.push_back(field(SynHQ, 92, Type::U8, 0, 1));
  if (b.size() >= kSize14) {
    const size_t v = rec(1), x = rec(2);
    f.push_back(i32(SynAttack, v + RAttack, 100, 65536));
    f.push_back(i32(SynDecay, v + RDecay, 100, 65536));
    f.push_back(i32(SynDecay, v + RHold, 100, 65536));  // decay time = hold / 4 + decay
    f.push_back(i32(SynSustain, v + RSustain, 0, 128));
    f.push_back(i32(SynRelease, v + RRelease, 100, 65536));
    f.push_back(i32(SynCutoff, kLevCutoff, 0, 256));
    f.push_back(i32(SynResonance, kLevReso, 0, 256));
    f.push_back(i32(SynFilterType, kLevType, 0, 7));
    f.push_back(i32(SynFilterEnv, x + RAmount, -128, 128));
    f.push_back(i32(SynFilterDecay, x + RDecay, 100, 65536));
    f.push_back(i32(SynFilterDecay, x + RHold, 100, 65536));
    const int l = activeLfo(b);
    if (l >= 0) {
      const size_t r = rec(kLfo[l].rec);
      f.push_back(i32(SynLfoShape, r + RLfoShape, 0, 2));
      f.push_back(i32(SynLfoRate, r + RLfoSpeed, 200, 65536));
      // (the pitch LFO goes to ±256: half depth per unit)
      f.push_back(i32(SynLfoAmount, r + RLfoAmount, 1, kLfo[l].rec == 4 ? 256 : 128));
    }
  }
  return f;
}

// Switches the channel envelope / filter / LFO copies on or off as the
// target needs (the field solver then sets their values).
void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  if (b.size() < kSize14) return;  // only v14+ carries the channel copies
  PluginMapping m;
  if (!decodeState("3x Osc", b, m, c)) return;
  const PluginState& cur = m.state;
  const float kOffA = 0.001f, kOffD = 1.0f, kOffS = 1.0f, kOffR = 0.008f;  // decoder's values without a volume envelope
  if (differ(g, cur, {SynAttack, SynDecay, SynSustain, SynRelease})) {
    const bool off = pv(g, SynAttack) == kOffA && pv(g, SynDecay) == kOffD && pv(g, SynSustain) == kOffS && pv(g, SynRelease) == kOffR;
    putI32(b, rec(1) + REnabled, off ? 0 : -1);
  }
  const size_t x = rec(2);
  const bool modX = geti(b, x + REnabled) != 0 && geti(b, x + RAmount) != 0;
  if (differ(g, cur, {SynFilterEnv, SynFilterDecay})) {
    const float want = pv(g, SynFilterEnv);
    if (want != 0.0f && !modX) putI32(b, x + REnabled, -1), putI32(b, x + RAmount, want > 0 ? 64 : -64);
    if (want == 0.0f && modX) putI32(b, x + RAmount, 0);
  }
  if (differ(g, cur, {SynCutoff, SynResonance, SynFilterType}) && pv(g, SynCutoff) == 20000.0f && pv(g, SynResonance) == 0.0f &&
      pv(g, SynFilterType) == 0.0f)
    putI32(b, kLevCutoff, 256), putI32(b, kLevReso, 0);
  if (!(pv(g, SynLfoTarget) == pv(cur, SynLfoTarget))) {
    const int want = static_cast<int>(pv(g, SynLfoTarget));
    for (const LfoCand& l : kLfo) {
      const size_t r = rec(l.rec) + RLfoAmount;
      if (l.trio == want) {
        if (geti(b, r) == 0) putI32(b, r, 64);
        break;
      }
      putI32(b, r, 0);
    }
  }
  // Decay times are hold / 4 + decay: try the usual holds (as stored, the
  // shortest, FL's default) with the decay solved for each.
  for (auto [t, k] : {std::pair<int, int>{1, SynDecay}, {2, SynFilterDecay}}) {
    const float want = pv(g, k);
    if (decoded("3x Osc", b, k, c) == want) continue;
    const size_t r = rec(t);
    const Bytes keep = b;
    bool hit = false;
    const int d0 = geti(b, r + RDecay);
    for (int h : {geti(b, r + RHold), 100, 20000})
      if (!hit) putI32(b, r + RHold, h), hit = bisect("3x Osc", b, r + RDecay, 100, 65536, k, want, c);
    for (int d : {d0, 100, 30000})  // or the hold solved for the usual decays
      if (!hit) putI32(b, r + RDecay, d), hit = bisect("3x Osc", b, r + RHold, 100, 65536, k, want, c);
    if (!hit) b = keep;
  }
}

Encoder encoder() {
  Encoder e;
  e.flName = "3x Osc";
  e.glType = "gl.synth";
  e.nativeRank = 0;  // the Trio Synth's FL counterpart
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace osc

// =================================================================== FL Keys
// Layout (FlpGenSimple.cpp): dword version 1, ANSI sample-set name (1-byte
// length), 15 dwords in control-tag order (decay, release, hardness,
// velocity → hardness, muffle, velocity → muffle, velocity sensitivity,
// stereo, tune, detune, stretch, treble, pan / tremolo, LFO rate, overdrive).
namespace keys {

size_t valuesAt(const Bytes& b) { return b.size() >= 5 ? 5 + static_cast<size_t>(b[4]) : 0; }

Bytes build(const std::string& set, const int (&p)[15]) {
  ByteWriter w;
  w.i32(1);  // version
  pstr(w, set);
  for (int v : p) w.i32(v);
  return w.b;
}

Bytes defaults() {
  // FL's default: the grand piano set; decay -1000, release 250, velocity →
  // hardness -1000, velocity → muffle 1000, velocity sensitivity -500, detune 123.
  const int p[15] = {-1000, 250, 0, -1000, 0, 1000, -500, 0, 0, 123, 0, 0, 0, 0, 0};
  return build("Grand Piano", p);
}

std::vector<Field> fields(const Bytes& b) {
  const size_t at = valuesAt(b);
  struct T {
    int tag, gl;
    double lo, hi;
  };
  // Tag 12 (pan / tremolo: sign = mode, size = depth) is written by write().
  static const T t[] = {{0, KyDecay, -1000, 1000},   {1, KyRelease, -1000, 1000},  {2, KyHardness, -1000, 1000}, {3, KyVelHardness, -1000, 1000},
                        {4, KyMuffle, 0, 1000},      {5, KyVelMuffle, 0, 2000},    {6, KyVelocity, -1000, 1000}, {7, KyStereo, 0, 2000},
                        {8, KyTune, -1000, 1000},    {9, KyDetune, 0, 500},        {10, KyStretch, -500, 500},   {11, KyTreble, -1000, 1000},
                        {13, KyTremRate, 0, 65536},  {14, KyDrive, 0, 1000}};
  std::vector<Field> f;
  for (const T& x : t) {
    const size_t o = at + static_cast<size_t>(x.tag) * 4;
    if (at && o + 4 <= b.size()) f.push_back(i32(x.gl, o, x.lo, x.hi));
  }
  return f;
}

void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  if (b.size() < 5 || !decodeState("FL Keys", b, m, c)) return;
  if (!(pv(g, KyModel) == pv(m.state, KyModel))) {
    // Sample set the decoder maps to the wanted model.
    const int model = static_cast<int>(pv(g, KyModel));
    const char* set = model == 4 ? "Grand Piano" : model == 0 ? "Rhodes" : model == 5 ? "mda Piano" : nullptr;
    if (set) {
      const size_t at = valuesAt(b);
      ByteWriter w;
      w.bytes(b.data(), 4);
      pstr(w, set);
      if (at <= b.size()) w.b.insert(w.b.end(), b.begin() + static_cast<std::ptrdiff_t>(at), b.end());
      b = w.b;
    }
  }
  if (differ(g, m.state, {KyModMode, KyTremDepth})) {
    const size_t o = valuesAt(b) + 12 * 4;
    const double depth = std::clamp(static_cast<double>(pv(g, KyTremDepth)), 0.0, 1.0);
    const long v = std::lround(depth * 1000.0);
    if (o + 4 <= b.size()) putI32(b, o, static_cast<int32_t>(pv(g, KyModMode) >= 0.5f ? -v : v));
  }
}

Encoder encoder() {
  Encoder e;
  e.flName = "FL Keys";
  e.glType = "gl.keys";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace keys

// ================================================================== MIDI Out
// Layout (FlpGenSimple.cpp): 8 dwords (version, MIDI channel, program, bank
// MSB, bank LSB, page, port, flags), 2 bytes, 72 controller value dwords from
// 0x22, controller records from 0x142 (21-byte header + two names) ended by
// 0xFF, then the page names (v7: + one dword).
namespace midiout {

constexpr size_t kValues = 0x22, kRecords = 0x142, kHeader = 21;
constexpr int kValueSlots = static_cast<int>((kRecords - kValues) / 4);

// Offsets of the controller records (at most `max`), and the terminator.
std::vector<size_t> records(const Bytes& b, size_t* end = nullptr) {
  std::vector<size_t> r;
  size_t at = kRecords;
  while (at + kHeader + 2 <= b.size() && b[at] != 0xFF) {
    r.push_back(at);
    size_t n = at + kHeader;
    n += 1 + b[n];
    if (n >= b.size()) break;
    n += 1 + b[n];
    at = n;
  }
  if (end) *end = at;
  return r;
}

Bytes defaults() {
  ByteWriter w;
  w.i32(7);                     // version FL 24+ writes
  w.i32(0);                     // MIDI channel 1
  w.i32(-1), w.i32(-1), w.i32(-1);  // program, bank MSB, bank LSB: none
  w.i32(0), w.i32(0);           // page, port
  w.i32(1);                     // flags
  w.zeros(2);
  w.zeros(static_cast<size_t>(kValueSlots) * 4);  // controller values
  w.u8(0xFF);                   // no controller records
  for (int k = 1; k <= 8; ++k) pstr(w, "Page " + std::to_string(k));
  w.i32(0);  // v7 trailer
  return w.b;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f{i32(MoChannel, 4, 0, 15), i32(MoProgram, 8, -1, 127), i32(MoBankMsb, 12, -1, 127), i32(MoBankLsb, 16, -1, 127),
                       i32(MoPort, 24, 0, 15)};
  if (geti(b, 0) < 4) return f;
  std::vector<size_t> r = records(b);
  for (size_t k = 0; k < r.size() && k < static_cast<size_t>(kMoKnobs); ++k) {
    const int kk = static_cast<int>(k);
    const size_t at = r[k];
    f.push_back(field(moKnob(kk, MoKnobNumber), at + 1, Type::U16, 0, 16383));
    f.push_back(i32(moKnob(kk, MoKnobMin), at + 9, 0, 16383));
    f.push_back(i32(moKnob(kk, MoKnobMax), at + 13, 0, 16383));
    f.push_back(i32(moKnob(kk, MoKnobChannel), at + 17, -1, 15));
    const size_t v = kValues + static_cast<size_t>(b[at]) * 4;
    if (v + 4 <= kRecords) f.push_back(i32(moKnob(kk, MoKnobValue), v, 0, 127));
  }
  return f;
}

// Adds controller records for knobs the state has none for (the decoder gives
// knobs without a record their defaults).
void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  if (geti(b, 0) < 4) return;
  size_t end = 0;
  std::vector<size_t> r = records(b, &end);
  if (end >= b.size() || b[end] != 0xFF) return;
  const PluginState def = defaultPluginState(g.type);
  int need = 0;
  for (int k = 0; k < kMoKnobs; ++k)
    for (int fld = 0; fld < MoKnobFields; ++fld)
      if (!(pv(g, moKnob(k, fld)) == pv(def, moKnob(k, fld)))) need = k + 1;
  if (static_cast<int>(r.size()) >= need) return;
  std::vector<bool> used(static_cast<size_t>(kValueSlots), false);
  for (size_t at : r)
    if (b[at] < kValueSlots) used[b[at]] = true;
  ByteWriter w;
  for (int k = static_cast<int>(r.size()); k < need; ++k) {
    int slot = k < kValueSlots && !used[static_cast<size_t>(k)] ? k : 0;
    while (slot < kValueSlots && used[static_cast<size_t>(slot)]) ++slot;
    if (slot >= kValueSlots) break;
    used[static_cast<size_t>(slot)] = true;
    const int number = static_cast<int>(std::clamp(pv(g, moKnob(k, MoKnobNumber)), 0.0f, 16383.0f));
    w.u8(static_cast<uint32_t>(slot));
    w.u16(static_cast<uint32_t>(number));
    w.u8(0);    // kind: CC
    w.i32(0);   // unknown dword (0 in FL's presets)
    w.u8(0);    // unknown byte (0 in FL's presets)
    w.i32(0), w.i32(127), w.i32(-1);  // min, max, channel (the plugin's); solved afterwards
    pstr(w, "Controller " + std::to_string(number));
    pstr(w, "CC " + std::to_string(number));
  }
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(end), w.b.begin(), w.b.end());
}

Encoder encoder() {
  Encoder e;
  e.flName = "MIDI Out";
  e.glType = "gl.midiout";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace midiout

// ===================================================== Envelope Controller
// Layout (FlpGenSimple.cpp): from 0x100, per articulator, FL envelope lists:
// dword point count N, float, float 1.0, float 0.5, dword 1, two dwords, then
// N points {double Δtime (beats), double level, float tension, dword flags}.
// The state around them (articulator blocks, LFOs, key mappings) is not
// decoded: no default state is built (FL loads its own defaults).
namespace envctl {

namespace md = gl::packs::modulation;
constexpr size_t kHead = 28, kPoint = 24;

struct List {
  size_t at;
  int points;  // valid points
};
// Same scan as the decoder (lists are recognised by their header).
std::vector<List> lists(const Bytes& b) {
  std::vector<List> out;
  for (size_t p = 0x100; p + kHead + kPoint * 2 <= b.size();) {
    const int n = geti(b, p);
    if (n < 2 || n > 64 || std::fabs(getF32(b, p + 8) - 1.0f) > 1e-6f || std::fabs(getF32(b, p + 12) - 0.5f) > 1e-6f || geti(b, p + 16) != 1) {
      p += 4;
      continue;
    }
    int ok = 0;
    for (int k = 0; k < n && p + kHead + static_cast<size_t>(k + 1) * kPoint <= b.size(); ++k) {
      const double dt = getF64(b, p + kHead + static_cast<size_t>(k) * kPoint), y = getF64(b, p + kHead + static_cast<size_t>(k) * kPoint + 8);
      if (!(dt >= 0 && dt < 1000 && y >= -0.001 && y <= 1.001)) break;
      ++ok;
    }
    if (ok < 2) {
      p += 4;
      continue;
    }
    out.push_back({p, ok});
    p += kHead + static_cast<size_t>(ok) * kPoint;
  }
  return out;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  const std::vector<List> ls = lists(b);
  for (int a = 0; a < md::kEnvArts && a < static_cast<int>(ls.size()); ++a)
    for (int k = 0; k < md::kEnvPoints && k < ls[static_cast<size_t>(a)].points; ++k) {
      const size_t at = ls[static_cast<size_t>(a)].at + kHead + static_cast<size_t>(k) * kPoint;
      f.push_back(field(md::envPointTime(a, k), at, Type::F64, 0, 100));  // beats
      f.push_back(field(md::envPointLevel(a, k), at + 8, Type::F64, 0, 1));
    }
  return f;
}

// Point counts: points are added (a copy of the last one) or removed at the
// end of a list whose header count equals its valid points.
void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  for (int a = 0; a < md::kEnvArts; ++a) {
    const std::vector<List> ls = lists(b);
    if (a >= static_cast<int>(ls.size())) break;
    const List l = ls[static_cast<size_t>(a)];
    const int want = static_cast<int>(pv(g, md::envParam(a, md::EaNumPoints)));
    const int have = std::max(2, std::min(l.points, md::kEnvPoints));
    if (want == have || want < 2 || want > md::kEnvPoints || geti(b, l.at) != l.points) continue;
    const size_t end = l.at + kHead + static_cast<size_t>(l.points) * kPoint;
    if (want < l.points) {
      b.erase(b.begin() + static_cast<std::ptrdiff_t>(l.at + kHead + static_cast<size_t>(want) * kPoint), b.begin() + static_cast<std::ptrdiff_t>(end));
    } else {
      const Bytes last(b.begin() + static_cast<std::ptrdiff_t>(end - kPoint), b.begin() + static_cast<std::ptrdiff_t>(end));
      for (int k = l.points; k < want; ++k) b.insert(b.begin() + static_cast<std::ptrdiff_t>(end), last.begin(), last.end());
    }
    putI32(b, l.at, want);
  }
}

Encoder encoder() {
  Encoder e;
  e.flName = "Fruity Envelope Controller";
  e.glType = "gl.envctl";
  e.nativeRank = 0;
  e.generator = true;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace envctl

// ------------------------------------------------- {u32 id, u64 size} records
struct Rec {
  uint32_t id = 0;
  Bytes data;
};
std::vector<Rec> parseRecs(const Bytes& b, size_t at, size_t* end = nullptr) {
  std::vector<Rec> out;
  while (at + 12 <= b.size()) {
    const uint32_t id = getU32(b, at);
    const uint64_t size = static_cast<uint64_t>(getU32(b, at + 4)) | (static_cast<uint64_t>(getU32(b, at + 8)) << 32);
    if (size > b.size() - at - 12) break;
    out.push_back({id, Bytes(b.begin() + static_cast<std::ptrdiff_t>(at + 12), b.begin() + static_cast<std::ptrdiff_t>(at + 12 + size))});
    at += 12 + static_cast<size_t>(size);
  }
  if (end) *end = at;
  return out;
}
void putRecs(ByteWriter& w, const std::vector<Rec>& rs) {
  for (const Rec& r : rs) {
    w.u32(r.id);
    w.u64(r.data.size());
    w.bytes(r.data);
  }
}
std::string cstr(const Bytes& d, size_t at = 0, size_t max = std::string::npos) {
  std::string s;
  for (size_t i = at; i < d.size() && i - at < max && d[i]; ++i) s += static_cast<char>(d[i]);
  return s;
}

// ===================================================================== FLEX
// Layout (FlpGenSamplers.cpp): dword version 1, records: 1 = preset identity
// (library name at 0, preset name at 0x401, ANSI), 2 = parameters (inner
// record 0: dword 2, dword count, count floats 0..1 in control-tag order).
// The decoder turns these into the closest Gridline instrument and keeps
// library, preset and the 45 control values in PluginState::data, which is
// what write() puts back.
namespace flex {

constexpr size_t kPresetAt = 0x401, kIdentity = 0x812;
constexpr int kControls = 45, kStored = 128;

Bytes defaults() {
  ByteWriter w;
  w.i32(1);  // version
  Rec ident{1, Bytes(kIdentity, 0)};  // no library / preset selected
  ByteWriter p;
  p.i32(2), p.i32(kStored);
  for (int k = 0; k < kStored; ++k) p.f32(k < kControls ? 0.5f : 0.0f);  // controls centred (offsets around the preset)
  ByteWriter inner;
  putRecs(inner, {{0, p.b}});
  putRecs(w, {ident, {2, inner.b}, {3, Bytes(56, 0)}, {4, Bytes(1, 0)}});  // records 3 / 4: not decoded, zero
  return w.b;
}

// Offset of control float 0 in the state (npos when absent) and their count.
size_t floatsAt(const Bytes& b, int* count = nullptr) {
  size_t at = 4;
  while (at + 12 <= b.size()) {
    const uint32_t id = getU32(b, at);
    const uint64_t size = static_cast<uint64_t>(getU32(b, at + 4)) | (static_cast<uint64_t>(getU32(b, at + 8)) << 32);
    if (size > b.size() - at - 12) break;
    if (id == 2) {
      size_t q = at + 12;
      const size_t end = q + static_cast<size_t>(size);
      while (q + 12 <= end) {
        const uint64_t qs = static_cast<uint64_t>(getU32(b, q + 4)) | (static_cast<uint64_t>(getU32(b, q + 8)) << 32);
        if (qs > end - q - 12) break;
        if (getU32(b, q) == 0 && qs >= 8) {
          if (count) *count = std::min<int>(geti(b, q + 16), static_cast<int>((qs - 8) / 4));
          return q + 20;
        }
        q += 12 + static_cast<size_t>(qs);
      }
    }
    at += 12 + static_cast<size_t>(size);
  }
  return std::string::npos;
}

struct Kept {
  bool ok = false;
  std::string pack, preset;
  std::vector<double> v;
};
Kept parseKept(const std::string& data) {
  Kept k;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    const std::string line = data.substr(pos, nl - pos);
    if (line == "flex 1") k.ok = true;
    else if (line.rfind("pack ", 0) == 0) k.pack = line.substr(5);
    else if (line.rfind("preset ", 0) == 0) k.preset = line.substr(7);
    else if (line.rfind("v", 0) == 0) {
      const char* s = line.c_str() + 1;
      char* e = nullptr;
      for (double x = std::strtod(s, &e); e != s; x = std::strtod(s, &e)) k.v.push_back(x), s = e;
    }
    pos = nl + 1;
  }
  return k;
}

std::string g6(double v) {
  char num[24];
  std::snprintf(num, sizeof num, "%.6g", v);
  return num;
}

void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  const Kept k = parseKept(g.data);
  if (!k.ok || b.size() < 4) return;
  std::vector<Rec> rs = parseRecs(b, 4);
  bool changed = false;
  for (Rec& r : rs) {
    if (r.id != 1) continue;
    const std::string lib = cstr(r.data, 0, 0x400), pre = r.data.size() > kPresetAt ? cstr(r.data, kPresetAt) : std::string();
    if (lib == k.pack && pre == k.preset) continue;
    Bytes d(std::max(r.data.size(), kPresetAt + k.preset.size() + 1), 0);
    std::copy_n(k.pack.begin(), std::min<size_t>(k.pack.size(), 0x3FF), d.begin());
    std::copy(k.preset.begin(), k.preset.end(), d.begin() + static_cast<std::ptrdiff_t>(kPresetAt));
    r.data = d;
    changed = true;
  }
  if (changed) {
    ByteWriter w;
    w.bytes(b.data(), 4);
    putRecs(w, rs);
    b = w.b;
  }
  int count = 0;
  const size_t at = floatsAt(b, &count);
  if (at == std::string::npos) return;
  for (int i = 0; i < count && i < kControls && i < static_cast<int>(k.v.size()); ++i) {
    const size_t o = at + static_cast<size_t>(i) * 4;
    const double cur = std::clamp(getF32(b, o), 0.0f, 1.0f);
    if (g6(cur) != g6(k.v[static_cast<size_t>(i)])) putF32(b, o, static_cast<float>(k.v[static_cast<size_t>(i)]));
  }
}

// Gridline parameters that follow one control (pitch, envelope, cutoff).
std::function<std::vector<Field>(const Bytes&)> fieldsFor(std::vector<std::pair<int, int>> map) {
  return [map](const Bytes& b) {
    std::vector<Field> f;
    int count = 0;
    const size_t at = floatsAt(b, &count);
    if (at == std::string::npos) return f;
    for (auto [gl, tag] : map)
      if (tag < count) f.push_back(field(gl, at + static_cast<size_t>(tag) * 4, Type::F32, 0, 1));
    return f;
  };
}

Encoder encoder(const std::string& type, std::vector<std::pair<int, int>> map) {
  Encoder e;
  e.flName = "FLEX";
  e.glType = type;
  e.nativeRank = -1;  // an approximation of FLEX content: only for FLEX imports
  e.accepts = [](const PluginState& s) { return s.data.rfind("flex 1", 0) == 0; };
  e.generator = true;
  e.defaults = defaults;
  e.fields = fieldsFor(std::move(map));
  e.write = write;
  return e;
}

}  // namespace flex

// =============================================================== DirectWave
// Layout (FlpGenSamplers.cpp): dword version, records; record 1 = a program
// (128), the first with zones is decoded. Zone = record 3 of the program with
// sub-records 500 (root, key / velocity range bytes, f32 volume at 9, f32 pan
// at 13), 501 name, 502 sample path, 503 sample info (loop mode / start / end
// dwords at 20 / 24 / 28). The default state (128 empty programs, ~370 KB of
// program / zone settings) is not built: native multisampler instances are
// not written as DirectWave.
namespace dw {

namespace smp = gl::packs::samplers;

// Volume / pan float whose decoded value is `want` (checked with the decoder's
// conversion via neighbours of the direct inverse).
float solveFloat(float guess, float want, const std::function<float(float)>& dec) {
  if (dec(guess) == want) return guess;
  float lo = guess, hi = guess;
  for (int i = 0; i < 64; ++i) {
    lo = std::nextafter(lo, -HUGE_VALF), hi = std::nextafter(hi, HUGE_VALF);
    if (dec(lo) == want) return lo;
    if (dec(hi) == want) return hi;
  }
  return guess;
}

void patchZone(Rec& zone, const smp::Zone& z) {
  std::vector<Rec> rs = parseRecs(zone.data, 0);
  for (Rec& r : rs) {
    Bytes& d = r.data;
    if (r.id == 500 && d.size() >= 17) {
      d[0] = static_cast<uint8_t>(std::clamp(z.root, 0, 127));
      d[1] = static_cast<uint8_t>(std::clamp(z.lo, 0, 127)), d[2] = static_cast<uint8_t>(std::clamp(z.hi, 0, 127));
      d[3] = static_cast<uint8_t>(std::clamp(z.vlo, 0, 127)), d[4] = static_cast<uint8_t>(std::clamp(z.vhi, 0, 127));
      auto decGain = [](float v) { return static_cast<float>(v > 0.001 ? std::clamp(20.0 * std::log10(static_cast<double>(v)), -60.0, 12.0) : -60.0); };
      auto decPan = [](float p) { return static_cast<float>(std::clamp((static_cast<double>(p) - 0.5) * 2.0, -1.0, 1.0)); };
      if (decGain(getF32(d, 9)) != z.gain)
        putF32(d, 9, z.gain <= -60.0f ? 0.0f : solveFloat(static_cast<float>(std::pow(10.0, z.gain / 20.0)), z.gain, decGain));
      if (decPan(getF32(d, 13)) != z.pan) putF32(d, 13, solveFloat(z.pan / 2.0f + 0.5f, z.pan, decPan));
    } else if (r.id == 501) {
      if (cstr(d) != z.name) d.assign(z.name.begin(), z.name.end());
    } else if (r.id == 503 && d.size() >= 32) {  // (502, the path, is set by the caller when it differs)
      const int mode = geti(d, 20);
      const bool on = mode > 0 && geti(d, 28) > geti(d, 24);
      if (z.loop == smp::LoopOff) {
        if (on) putI32(d, 20, 0);
      } else {
        putI32(d, 20, z.loop == smp::LoopBounce ? 3 : (on && mode != 3 ? mode : 2));
        putI32(d, 24, static_cast<int32_t>(z.loopStart)), putI32(d, 28, static_cast<int32_t>(z.loopEnd));
      }
    }
  }
  ByteWriter w;
  putRecs(w, rs);
  zone.data = w.b;
}

void setPath(Rec& zone, const std::string& path) {
  std::vector<Rec> rs = parseRecs(zone.data, 0);
  for (Rec& r : rs)
    if (r.id == 502) r.data.assign(path.begin(), path.end());
  ByteWriter w;
  putRecs(w, rs);
  zone.data = w.b;
}

std::string zonePath(const Rec& zone) {
  for (const Rec& r : parseRecs(zone.data, 0))
    if (r.id == 502) return cstr(r.data);
  return {};
}

void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  if (b.size() < 16 || !decodeState("DirectWave", b, m, c) || m.state.data == g.data) return;
  const smp::ZoneMap want = smp::parseZoneMap(g.data);
  const smp::ZoneMap have = smp::parseZoneMap(m.state.data);
  if (want.zones.empty()) return;
  std::vector<Rec> top = parseRecs(b, 4);
  for (Rec& prog : top) {
    if (prog.id != 1) continue;
    std::vector<Rec> pr = parseRecs(prog.data, 0);
    std::vector<size_t> zi;  // zone records the decoder reads (a sample path)
    for (size_t i = 0; i < pr.size(); ++i)
      if (pr[i].id == 3 && !zonePath(pr[i]).empty()) zi.push_back(i);
    if (zi.empty()) continue;
    // Zone count: extra zones copy the last one; surplus zones are removed.
    const Rec tmpl = pr[zi.back()];
    while (zi.size() > want.zones.size()) pr.erase(pr.begin() + static_cast<std::ptrdiff_t>(zi.back())), zi.pop_back();
    for (size_t n = zi.size(); n < want.zones.size(); ++n) {
      pr.insert(pr.begin() + static_cast<std::ptrdiff_t>(zi.back() + 1), tmpl);
      zi.push_back(zi.back() + 1);
    }
    for (size_t k = 0; k < zi.size(); ++k) {
      Rec& z = pr[zi[k]];
      patchZone(z, want.zones[k]);
      if (k >= have.zones.size() || have.zones[k].path != want.zones[k].path) setPath(z, flPath(want.zones[k].path, c));
    }
    ByteWriter w;
    putRecs(w, pr);
    prog.data = w.b;
    break;
  }
  ByteWriter w;
  w.bytes(b.data(), 4);
  putRecs(w, top);
  b = w.b;
}

Encoder encoder() {
  Encoder e;
  e.flName = "DirectWave";
  e.glType = "gl.multisampler";
  e.nativeRank = -1;
  e.generator = true;
  e.write = write;
  return e;
}

}  // namespace dw

// ====================================================================== GMS
// Layout (FlpGenGms.cpp): "SSLF", dword size, payload: magic ("4nyS"; older
// "3nyS" without the UI dwords), three 512-byte wave file paths, zeros to
// 0xE04, three UI dwords, 212 floats (0..1, one per control) from 0xE10, a
// 512-byte program name.
namespace gms {

namespace gm = gl::packs::synths::gms;
constexpr size_t kWave = 4, kWaveLen = 512, kFloats4 = 0xE10, kFloats3 = 0xE04, kName = 512;
constexpr int kCount = 212;

// Payload offset (after "SSLF" + size) and the floats' offset in the state.
size_t payloadAt(const Bytes& b) { return b.size() >= 8 && std::equal(b.begin(), b.begin() + 4, "SSLF") ? 8 : 0; }
size_t floatsAt(const Bytes& b) {
  const size_t p = payloadAt(b);
  if (b.size() < p + 4) return std::string::npos;
  return p + (std::equal(b.begin() + static_cast<std::ptrdiff_t>(p), b.begin() + static_cast<std::ptrdiff_t>(p + 4), "3nyS") ? kFloats3 : kFloats4);
}

Bytes defaults() {
  ByteWriter p;
  p.bytes("4nyS", 4);
  // Waves: FL's default trio (oscillator 1..3), by file name in the GMS wave folder.
  for (const char* w : {"Beast", "Sine", "Saw"}) {
    Bytes slot(kWaveLen, 0);
    const std::string s = std::string("Plugins\\Fruity\\Generators\\GMS\\Synth Waves\\") + w + ".wav";
    std::copy(s.begin(), s.end(), slot.begin());
    p.bytes(slot);
  }
  p.zeros(kFloats3 - p.size());
  p.i32(0), p.i32(0), p.i32(0);  // UI state
  float v[kCount] = {};
  // FL's default program, control by control (index = control tag).
  v[0] = 0.75f, v[1] = 0.5f, v[2] = 0.5f, v[3] = 0.4444f;     // level, pan, pitch, -
  v[4] = 0.06f, v[5] = 1.0f, v[6] = 1.0f, v[7] = 0.15f;       // amp A D S R
  v[8] = 0.4025f, v[12] = 1.0f;                               // amp level, cutoff
  v[22] = 1.0f, v[23] = 1.0f;                                 // envelope 1 / 2 decay
  v[41] = 0.1f, v[42] = 0.105f, v[43] = 0.715f, v[44] = 0.5f;  // unison voices / detune / stereo, octave
  v[45] = 0.16f, v[46] = 0.515f, v[47] = 1.0f;                // noise, unison phase / reset
  v[55] = 0.1625f, v[56] = 0.5f, v[57] = 0.33f, v[58] = 0.5f, v[59] = 0.5f, v[60] = 0.5f;  // oscillator pitches / detunes
  for (int i = 61; i <= 72; ++i) v[i] = 0.5f;
  for (int i : {81, 82, 83, 84, 89, 90, 91, 92}) v[i] = 1.0f;
  for (int k = 0; k < 10; ++k) v[117 + k] = 0.5f, v[127 + k] = 0.5f, v[137 + k] = 0.5567f, v[177 + k] = 0.5f;  // effect X / Y / rate / LFO
  v[120] = 0.1892f, v[124] = 0.1712f, v[130] = 0.045f, v[134] = 0.0f;
  v[150] = 1.0f, v[154] = 1.0f;                       // effects on: flanger, low pass… as FL's default program
  v[198] = 0.5f, v[199] = 1.0f, v[200] = 1.0f, v[201] = 0.5f, v[203] = 0.5f, v[205] = 0.1667f;  // velocity routing, bend range 2
  for (int i = 207; i <= 211; ++i) v[i] = 0.5f;       // EQ flat
  for (float x : v) p.f32(x);
  p.zeros(kName);  // program name: none
  ByteWriter w;
  w.bytes("SSLF", 4);
  w.u32(static_cast<uint32_t>(p.size()));
  w.bytes(p.b);
  return w.b;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  const size_t at = floatsAt(b);
  if (at == std::string::npos) return f;
  auto F = [&](int gl, int i, double hi = 1.0) {
    if (at + static_cast<size_t>(i + 1) * 4 <= b.size()) f.push_back(field(gl, at + static_cast<size_t>(i) * 4, Type::F32, 0, hi));
  };
  F(gm::GVolume, 0), F(gm::GPan, 1), F(gm::GPitch, 2), F(gm::GAmpA, 4), F(gm::GAmpD, 5), F(gm::GAmpS, 6), F(gm::GAmpR, 7);
  F(gm::GLevel, 8), F(gm::GSlideTime, 10), F(gm::GSlide, 11), F(gm::GCutoff, 12), F(gm::GReso, 13), F(gm::GKey, 14), F(gm::GFltType, 15);
  for (int k = 0; k < 2; ++k) {
    F(gm::eg(k, gm::EgAmt), 16 + k), F(gm::eg(k, gm::EgAtt), 20 + k), F(gm::eg(k, gm::EgDec), 22 + k), F(gm::eg(k, gm::EgDest), 24 + k);
    F(gm::lfo(k, gm::LfRetrig), 26 + k), F(gm::lfo(k, gm::LfAmt), 28 + k), F(gm::lfo(k, gm::LfShape), 32 + k);
    F(gm::lfo(k, gm::LfRate), 34 + k), F(gm::lfo(k, gm::LfDiv), 34 + k), F(gm::lfo(k, gm::LfSync), 36 + k), F(gm::lfo(k, gm::LfDest), 38 + k);
  }
  F(gm::GMono, 40), F(gm::GUniVoices, 41), F(gm::GUniDetune, 42), F(gm::GUniStereo, 43), F(gm::GOctave, 44), F(gm::GNoise, 45);
  F(gm::GUniPhase, 46), F(gm::GUniRetrig, 47), F(gm::GOsc2Mix, 48), F(gm::GOsc3Mix, 49), F(gm::GModAmt, 50), F(gm::GModKind, 51);
  F(gm::GOsc2Phase, 52), F(gm::GOsc2Retrig, 53), F(gm::GOsc2Invert, 54);
  for (int k = 0; k < 3; ++k) F(gm::osc(k, gm::OsSemi), 55 + 2 * k), F(gm::osc(k, gm::OsCents), 56 + 2 * k);
  if (at + 187 * 4 <= b.size())
    for (int k = 0; k < gm::kFxSlots; ++k) {
      F(gm::fx(k, gm::FfX), 117 + k), F(gm::fx(k, gm::FfY), 127 + k), F(gm::fx(k, gm::FfDiv), 137 + k), F(gm::fx(k, gm::FfOn), 147 + k);
      F(gm::fx(k, gm::FfShape), 157 + k), F(gm::fx(k, gm::FfX4), 167 + k), F(gm::fx(k, gm::FfLfo), 177 + k);
    }
  if (at + kCount * 4 <= b.size()) {
    F(gm::GVelAmt, 198), F(gm::GVelDest, 199, 21), F(gm::GVelVol, 200), F(gm::GBend, 205), F(gm::GEqOn, 206);
    for (int i = 0; i < gm::kEqBands; ++i) F(gm::GEq1 + i, 207 + i);
  }
  return f;
}

// Signs of the envelope / LFO amounts (separate invert switches) and the wave files.
void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  const size_t at = floatsAt(b);
  if (at == std::string::npos || at + 40 * 4 > b.size() || !decodeState("GMS", b, m, c)) return;
  for (int k = 0; k < 2; ++k)
    for (auto [gl, inv] : {std::pair<int, int>{gm::eg(k, gm::EgAmt), 18 + k}, {gm::lfo(k, gm::LfAmt), 30 + k}}) {
      const float want = pv(g, gl), cur = pv(m.state, gl);
      if (want != 0.0f && (want < 0.0f) != (cur < 0.0f)) putF32(b, at + static_cast<size_t>(inv) * 4, want < 0.0f ? 1.0f : 0.0f);
    }
  if (g.data == m.state.data) return;
  const gm::WaveFiles want = gm::parseWaveFiles(g.data), cur = gm::parseWaveFiles(m.state.data);
  const size_t p = payloadAt(b);
  for (int k = 0; k < gm::kOscs; ++k) {
    if (want.path[k] == cur.path[k] || want.path[k].empty() || want.path[k].size() >= kWaveLen) continue;
    const size_t o = p + kWave + static_cast<size_t>(k) * kWaveLen;
    std::fill(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + kWaveLen), 0);
    std::copy(want.path[k].begin(), want.path[k].end(), b.begin() + static_cast<std::ptrdiff_t>(o));
  }
}

Encoder encoder() {
  Encoder e;
  e.flName = "GMS";
  e.glType = "gl.gms";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace gms

// ================================================================= Harmless
// Layout (FlpGenHarmless.cpp): dword version, 100 dwords (control tags 0..99),
// option dwords (6 before v9, else 8: filter shape, resonance shape, phaser
// shape, compression type, timbre colour, quality, threading, custom table
// size N), N floats levels + N floats phases, v12: one flag byte.
namespace harmless {

namespace ad = gl::packs::models;
constexpr size_t kTags = 4, kOpts = 4 + 100 * 4;

struct T {
  int tag, gl;
  int lo, hi;
};
// Control tag → Gridline parameter, stored range (controls' Min..Max).
const std::vector<T>& table() {
  static const std::vector<T> t = [] {
    std::vector<T> v{{0, ad::AdPhaseRand, 0, 1000},    {1, ad::AdProtect, 0, 1024},       {2, ad::AdSub, 0, 2048},
                     {3, ad::AdSub3, 0, 2048},         {4, ad::AdSub4, 0, 2048},          {23, ad::AdMaskMix, 0, 1000},
                     {24, ad::AdMaskLfo, -1000, 1000}, {25, ad::AdTremolo, 0, 1000},      {26, ad::AdAttack, 0, 1000},
                     {27, ad::AdDecay, 0, 1000},       {27, ad::AdSustain, 0, 1000},      {28, ad::AdRelease, 0, 1000},
                     {29, ad::AdVelAtt, 0, 1},         {30, ad::AdVelRel, 0, 1},          {31, ad::AdPluck, 0, 1000},
                     {31, ad::AdPluckTime, 0, 1000},   {32, ad::AdVelPluck, 0, 1},        {33, ad::AdPluckAlt, 0, 1},
                     {34, ad::AdPluckMask, 0, 1},      {35, ad::AdPluckRel, 0, 1},        {36, ad::AdVolume, 0, 2000},
                     {37, ad::AdVelVol, 0, 1},         {38, ad::AdPitch, -2400, 2400},    {39, ad::AdPitch, -2400, 2400},
                     {40, ad::AdGrit, -1000, 1000},    {41, ad::AdVibrato, 0, 1000},      {42, ad::AdLfoPitch, -1000, 1000},
                     {45, ad::AdGlide, -1000, 1000},   {46, ad::AdVelPorta, 0, 1},        {47, ad::AdFEnvAdaptive, 0, 1},
                     {52, ad::AdFEnvAmt, -1000, 1000}, {48, ad::AdFEnvAtt, 0, 1000},      {49, ad::AdFEnvDec, 0, 1000},
                     {49, ad::AdFEnvSus, 0, 1000},     {50, ad::AdFEnvAttSlope, -1000, 1000}, {51, ad::AdFEnvDecSlope, -1000, 1000},
                     {53, ad::AdVelFEnv, 0, 1},        {54, ad::AdFltCutoff, 0, 8192},    {55, ad::AdFltScale, 0, 1},
                     {56, ad::AdFltWidth, 0, 1000},    {57, ad::AdFltKey, -2000, 2000},   {58, ad::AdLfoCutoff, -1000, 1000},
                     {59, ad::AdFltReso, 0, 1000},     {60, ad::AdResScale, 0, 1},        {61, ad::AdResOffset, -2400, 2400},
                     {62, ad::AdResOsc, 0, 2048},      {63, ad::AdResAdaptive, 0, 1},     {64, ad::AdResTrack, 0, 1},
                     {65, ad::AdUniOrder, 1, 9},       {66, ad::AdUniSpread, 0, 128},     {67, ad::AdUniDetune, 0, 256},
                     {68, ad::AdUniPhase, 0, 128},     {69, ad::AdUniVar, 0, 128},        {70, ad::AdUniType, 0, 2},
                     {71, ad::AdPhMix, 0, 1000},       {72, ad::AdPhScale, 0, 1},         {73, ad::AdPhDensity, 0, 10000},
                     {74, ad::AdPhWidthLfo, -1000, 1000}, {75, ad::AdPhOffset, 0, 999},   {76, ad::AdLfoPhaser, -1000, 1000},
                     {77, ad::AdPhSpeed, -8000, 8000}, {78, ad::AdPhKey, -2000, 2000},    {79, ad::AdHzMix, 0, 1000},
                     {80, ad::AdHzWidth, 0, 1000},     {81, ad::AdHzWidthLfo, -1000, 1000}, {82, ad::AdVelHarm, 0, 1},
                     {83, ad::AdHzStrength, 0, 1024},  {84, ad::AdHzType, 0, ad::AhCount - 1}, {85, ad::AdHzPos, 0, 3},
                     {86, ad::AdLfoShape, 0, 13},      {87, ad::AdLfoGlobal, 0, 1},       {88, ad::AdLfoAttack, 0, 1000},
                     {89, ad::AdLfoRate, -8000, 8000}, {90, ad::AdChType, 0, 9},          {91, ad::AdFx + ad::FxChorus, 0, 1000},
                     {92, ad::AdDelType, 0, 3},        {93, ad::AdFx + ad::FxDelay, 0, 1000}, {94, ad::AdFx + ad::FxDelayFeedback, 0, 900},
                     {95, ad::AdFx + ad::FxDelayTime, 0, 384}, {96, ad::AdRevType, 0, 6}, {97, ad::AdFx + ad::FxReverb, 0, 1000},
                     {98, ad::AdComp, 0, 1000},        {99, ad::AdVelUni, 0, 1}};
    for (int i = 0; i < 6; ++i) v.push_back({5 + i, ad::AdTeqBase + i, -1024, 1024});
    for (int k = 0; k < 12; ++k) v.push_back({11 + k, ad::AdMaskBase + k, 0, 1000});
    return v;
  }();
  return t;
}

int optCount(const Bytes& b) { return b.size() >= kOpts + 32 ? 8 : b.size() >= kOpts + 24 ? 6 : 0; }

Bytes defaults() {
  ByteWriter w;
  w.i32(12);  // version FL 21+ writes
  int p[100] = {};
  // FL's defaults (control tag: value).
  const int defs[][2] = {{1, 256},  {11, 1000}, {13, 1000}, {15, 1000}, {17, 1000}, {19, 1000}, {21, 1000}, {27, 1000}, {28, 250},
                         {31, 1000}, {36, 1000}, {37, 1},   {45, 400},  {47, 1},    {49, 500},  {51, -300}, {54, 8192}, {56, 500},
                         {57, 1000}, {60, 1},   {65, 1},    {66, 128},  {67, 128},  {68, 64},   {70, 2},    {72, 1},    {73, 5000},
                         {75, 500},  {77, 3000}, {78, 1000}, {80, 500}, {83, 256},  {85, 3},    {87, 1},    {89, 3000}, {91, 500},
                         {93, 500},  {94, 300}, {95, 144},  {97, 500},  {98, 500}};
  for (const auto& d : defs) p[d[0]] = d[1];
  for (int v : p) w.i32(v);
  // Options: filter shape 4, resonance / phaser shape 0, compression 0,
  // colour 0, quality 1, threading 0, no custom table.
  for (int v : {4, 0, 0, 0, 0, 1, 0, 0}) w.i32(v);
  w.u8(1);  // v12 flag byte (1 in every FL-written v12 state)
  return w.b;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  for (const T& t : table()) {
    const size_t at = kTags + static_cast<size_t>(t.tag) * 4;
    if (at + 4 <= b.size() && at < kOpts) f.push_back(i32(t.gl, at, t.lo, t.hi));
  }
  const int n = optCount(b);
  if (n >= 5) {
    f.push_back(i32(ad::AdFltType, kOpts, 0, 16));
    f.push_back(i32(ad::AdResType, kOpts + 4, 0, ad::ArCount - 1));
    f.push_back(i32(ad::AdPhType, kOpts + 8, 0, ad::ApCount - 1));
    f.push_back(i32(ad::AdCompType, kOpts + 12, 0, 4));
    f.push_back(i32(ad::AdSpectrumA, kOpts + 16, 0, 11));
    f.push_back(i32(ad::AdSpectrumB, kOpts + 16, 0, 11));
  }
  return f;
}

// Play mode (portamento + mono switches) and the custom harmonic table.
void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  if (b.size() < kOpts || !decodeState("Harmless", b, m, c)) return;
  if (!(pv(g, ad::AdPlayMode) == pv(m.state, ad::AdPlayMode))) {
    const int mode = static_cast<int>(pv(g, ad::AdPlayMode));
    if (mode != ad::ApAuto) {
      putI32(b, kTags + 43 * 4, mode == ad::ApPolyPorta || mode == ad::ApMonoPorta ? 1 : 0);
      putI32(b, kTags + 44 * 4, mode == ad::ApMono || mode == ad::ApMonoPorta ? 1 : 0);
    }
  }
  // Effect levels need their type combo off "none" (0) to be heard.
  for (auto [sel, gl] : {std::pair<int, int>{90, ad::AdFx + ad::FxChorus}, {92, ad::AdFx + ad::FxDelay}, {96, ad::AdFx + ad::FxReverb}})
    if (pv(g, gl) != 0.0f && geti(b, kTags + static_cast<size_t>(sel) * 4) == 0) putI32(b, kTags + static_cast<size_t>(sel) * 4, 1);
  if (g.data == m.state.data || optCount(b) != 8) return;
  ad::AdditiveData want;
  if (!g.data.empty() && !ad::parseAdditiveData(g.data, want)) return;
  const size_t tab = kOpts + 32;
  const size_t n0 = static_cast<size_t>(std::max(0, geti(b, kOpts + 28)));
  const size_t end0 = std::min(b.size(), tab + n0 * 8);
  ByteWriter w;
  for (size_t k = 0; k < want.level.size(); ++k) w.f32(want.level[k]);
  for (size_t k = 0; k < want.level.size(); ++k) w.f32(k < want.phase.size() ? want.phase[k] : 0.0f);
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(tab), b.begin() + static_cast<std::ptrdiff_t>(end0));
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(tab), w.b.begin(), w.b.end());
  putI32(b, kOpts + 28, static_cast<int32_t>(want.level.size()));
}

Encoder encoder() {
  Encoder e;
  e.flName = "Harmless";
  e.glType = "gl.additive";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = defaults;
  e.fields = fields;
  e.write = write;
  return e;
}

}  // namespace harmless

// =================================================================== Harmor
// Layout (FlpGenHarmor.cpp): u32 version, u32 section mask, per part (A, B):
// i32 size + part (386 tag dwords — 385 before v16 —, 12 unit-order dwords,
// 13-dword part record, 2 filter shapes, 2 resonance shapes (100 +), u8
// enabled, …, articulator / shaping curves, image), then the global block:
// 53 dwords (tags 10000.., 20000..), 6 fx-order dwords, 14 dwords (2 mono,
// 12 distortion, 13 compression), …
// Fields cover every parameter stored as one number; the curves, image and
// articulator smoothing (PluginState::data) are kept from the template: no
// default state is built (the format needs every curve of 43 articulators).
namespace harmor {

namespace hm = gl::packs::models::hm;

struct Layout {
  int ver = 0;
  size_t part[2] = {0, 0};  // offsets of the parts' tag dwords (0 = absent)
  size_t global = 0;        // offset of the global dwords (0 = absent)
};

Layout layout(const Bytes& b) {
  Layout l;
  if (b.size() < 12) return l;
  const uint32_t raw = getU32(b, 0);
  l.ver = raw >= 0x10000 ? static_cast<int>(raw >> 16) : static_cast<int>(raw);
  const int sections = geti(b, 4);
  const int parts = (sections & 3) == 3 ? 2 : (sections & 3) ? 1 : 0;
  size_t at = 8;
  for (int k = 0; k < parts; ++k) {
    if (at + 4 > b.size()) return Layout{};
    const int32_t size = geti(b, at);
    if (size < 0 || at + 4 + static_cast<size_t>(size) > b.size()) return Layout{};
    l.part[k] = at + 4;
    at += 4 + static_cast<size_t>(size);
  }
  if ((sections & 4) && at + 53 * 4 + 24 + 14 * 4 <= b.size()) l.global = at;
  return l;
}

struct T {
  int field, tag, lo, hi;
};
const std::vector<T>& partTable() {
  static const std::vector<T> t = {
      {hm::PfPhase, 0, 0, 1000},          {hm::PfPhaseRand, 1, -1000, 1000},   {hm::PfPredelay, 2, -384, 384},
      {hm::PfRelMul, 3, 0, 1000},         {hm::PfTimbreMix, 4, 0, 1000},       {hm::PfSub1, 5, 0, 2048},
      {hm::PfSub3, 6, 0, 2048},           {hm::PfSub4, 7, 0, 2048},            {hm::PfProtect, 8, 0, 1024},
      {hm::PfClip, 9, 0, 1024},           {hm::PfFxMix, 10, -1000, 1000},      {hm::PfVolume, 11, -2000, 2000},
      {hm::PfEnvVolume, 12, 0, 2000},     {hm::PfAutoGain, 13, 0, 1000},       {hm::PfVelAttTime, 14, 0, 1},
      {hm::PfVelAttLen, 15, 0, 1},        {hm::PfVelRelLen, 16, 0, 1},         {hm::PfTremolo, 17, 0, 1000},
      {hm::PfTremHz, 18, 0, 10000},       {hm::PfTremGap, 19, 0, 999},         {hm::PfBlurMix, 20, 0, 1000},
      {hm::PfTBlurAtt, 21, 0, 1600},      {hm::PfTBlurDec, 22, 0, 1600},       {hm::PfHBlur, 23, 0, 10000},
      {hm::PfHBlurLo, 24, 0, 1000},       {hm::PfHBlurHi, 25, 0, 1000},        {hm::PfPrism, 26, -1000, 1000},
      {hm::PfHzMix, 27, 0, 1000},         {hm::PfHzWidth, 28, 0, 1000},        {hm::PfHzStrength, 29, 0, 1024},
      {hm::PfHzShift, 30, -2, 7},         {hm::PfHzShiftStep, 31, -7, 8},      {hm::PfHzGap, 32, -1, 8},
      {hm::PfHzGapStep, 33, -7, 8},       {hm::PfUniOrder, 34, 1, 9},          {hm::PfUniType, 35, 0, 4},
      {hm::PfUniAlt, 36, 0, 1},           {hm::PfUniPan, 37, 0, 256},          {hm::PfUniPitch, 38, 0, 256},
      {hm::PfUniPhase, 39, -1000, 1000},  {hm::PfFreqMul, 40, 10000, 160000},  {hm::PfFreqDiv, 41, 1, 9},
      {hm::PfDetMul, 42, 10000, 160000},  {hm::PfDetDiv, 43, 1, 9},            {hm::PfPitchArt, 44, -6000, 6000},
      {hm::PfVibrato, 45, 0, 1000},       {hm::PfVibHz, 46, 0, 1000},          {hm::PfPortaLog, 47, 0, 1},
      {hm::PfPortaVar, 48, 0, 1},         {hm::PfPortaTime, 49, -384, 384},    {hm::PfPortaLimit, 50, 0, 1200},
      {hm::PfFMix, 74, -1000, 1000},      {hm::PfPluck, 75, 0, 1000},          {hm::PfPhMix, 76, 0, 1000},
      {hm::PfPhScale, 77, 0, 2},          {hm::PfPhWidth, 78, 0, 10000},       {hm::PfPhOfs, 79, 0, 999},
      {hm::PfPhMotion, 80, -8000, 8000},  {hm::PfPhKey, 81, -2000, 2000},      {hm::PfImgTimeSmooth, 82, -1000, 1000},
      {hm::PfImgTime, 83, 0, 1048575},    {hm::PfImgSpeed, 84, -20000, 20000}, {hm::PfImgSpeedOct, 85, -60000, 60000},
      {hm::PfImgSharp, 86, -1000, 1000},  {hm::PfImgGainScale, 88, 0, 1000},   {hm::PfImgGainMix, 89, 0, 1000},
      {hm::PfImgFreqMode, 90, 0, 3},      {hm::PfImgFreqScale, 92, -2000, 2000}, {hm::PfImgFormant, 93, -600, 600},
      {hm::PfImgFormantMix, 94, 0, 1000}};
  return t;
}
const std::vector<T>& filterTable() {  // per filter, tag + 12 × filter
  static const std::vector<T> t = {{hm::PfF1Adapt, 51, 0, 1},     {hm::PfF1Cut, 52, 0, 8192},     {hm::PfF1Width, 54, 0, 1000},
                                   {hm::PfF1Key, 55, -2000, 2000}, {hm::PfF1Env, 56, -1000, 1000}, {hm::PfF1Res, 57, 0, 1000},
                                   {hm::PfF1ResWidth, 59, 0, 1000}, {hm::PfF1ResOfs, 60, -2400, 2400}, {hm::PfF1ResAdapt, 61, 0, 1}};
  return t;
}
const std::vector<T>& globalTable() {  // field = Gridline param index
  static const std::vector<T> t = {
      {hm::GmModX, 10000, 0, 512},        {hm::GmModY, 10001, 0, 512},          {hm::GmModZ, 10002, 0, 512},
      {hm::GmLegatoLimit, 10003, 0, 2400}, {hm::GmLegato, 10004, 0, 1},         {hm::GmPorta, 10005, 0, 1},
      {hm::GmVelPorta, 10006, 0, 1},      {hm::GmStrumMode, 10007, 0, 3},       {hm::GmStrumTime, 10008, 0, 1000},
      {hm::GmStrumTension, 10009, -1000, 1000}, {hm::GmPartMix, 10010, 0, 1000}, {hm::GmVolume, 10011, 0, 2000},
      {hm::GmPostVolume, 10012, 0, 2000}, {hm::GmVelVol, 10013, 0, 1},          {hm::GmPitch, 10014, -2400, 2400},
      {hm::GmLfoAmt, 10015, -1000, 1000}, {hm::FxDistDrive, 20000, 0, 1000},    {hm::FxDistAsym, 20001, -500, 500},
      {hm::FxDistOut, 20002, 0, 2000},    {hm::FxDistMix, 20003, -1000, 1000},  {hm::FxDistTone, 20004, 0, 1100},
      {hm::FxRvOn, 20005, 0, 1},          {hm::FxRvLowCut, 20006, 0, 1000},     {hm::FxRvHighCut, 20007, 0, 1100},
      {hm::FxRvPredelay, 20008, -384, 384}, {hm::FxRvRoom, 20009, 0, 100},      {hm::FxRvDiffusion, 20010, 0, 100},
      {hm::FxRvDecay, 20011, 0, 900},     {hm::FxRvDamp, 20012, 0, 1100},       {hm::FxRvColor, 20013, 0, 4},
      {hm::FxRvMix, 20014, -1000, 1000},  {hm::FxChOrder, 20015, 0, 9},         {hm::FxChDepth, 20016, 0, 1024},
      {hm::FxChSpeed, 20017, 0, 1024},    {hm::FxChDelay, 20018, 0, 128},       {hm::FxChSpread, 20019, 0, 128},
      {hm::FxChCross, 20020, -128, 128},  {hm::FxChMix, 20021, -1000, 1000},    {hm::FxDlOn, 20022, 0, 1},
      {hm::FxDlMode, 20023, 0, 2},        {hm::FxDlFeedback, 20024, 0, 1000},   {hm::FxDlTime, 20025, -384, 384},
      {hm::FxDlOffset, 20026, -512, 512}, {hm::FxDlVol, 20027, -1000, 1000},    {hm::FxDlPan, 20028, -128, 128},
      {hm::FxDlLowCut, 20029, 0, 1000},   {hm::FxDlHighCut, 20030, 0, 1100},    {hm::FxDlDamp, 20031, 0, 1},
      {hm::FxCompAmount, 20032, 0, 1000}, {hm::FxCompLow, 20033, 0, 2000},      {hm::FxCompMid, 20034, 0, 2000},
      {hm::FxCompHigh, 20035, 0, 2000}};
  return t;
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  const Layout l = layout(b);
  const int np = l.ver >= 16 ? 386 : 385;
  for (int p = 0; p < 2; ++p) {
    if (!l.part[p]) continue;
    const size_t t0 = l.part[p], rec = t0 + static_cast<size_t>(np) * 4 + 12 * 4;
    auto tag = [&](int gl, int t, int lo, int hi) { f.push_back(i32(gl, t0 + static_cast<size_t>(t) * 4, lo, hi)); };
    for (const T& t : partTable()) tag(hm::pf(p, t.field), t.tag, t.lo, t.hi);
    for (int k = 0; k < 2; ++k) {
      for (const T& t : filterTable()) tag(hm::flt(p, k, t.field), t.tag + 12 * k, t.lo, t.hi);
      f.push_back(i32(hm::flt(p, k, hm::PfF1Type), rec + 52 + static_cast<size_t>(k) * 4, 0, hm::kFltShapes));
      f.push_back(i32(hm::flt(p, k, hm::PfF1ResType), rec + 60 + static_cast<size_t>(k) * 4, 100, 100 + hm::kResShapes - 1));
    }
    tag(hm::flt(p, 0, hm::PfF1Osc), 62, 0, 2048);
    f.push_back(i32(hm::pf(p, hm::PfBlend), rec, 0, 5));
    f.push_back(i32(hm::pf(p, hm::PfPrismMul), rec + 12, 0, 1));
    f.push_back(i32(hm::pf(p, hm::PfClipMode), rec + 20, 0, 4));
    f.push_back(i32(hm::pf(p, hm::PfSubMode), rec + 24, 0, 1));
    f.push_back(i32(hm::pf(p, hm::PfPhType), rec + 40, 200, 210));
    f.push_back(field(hm::pf(p, hm::PfEnabled), rec + 68, Type::U8, 0, 1));
    if (p == 0) f.push_back(field(hm::FxEqMix, rec + 48, Type::F32, 0, 1));
  }
  if (l.global) {
    for (const T& t : globalTable()) {
      const int idx = t.tag >= 20000 ? 17 + t.tag - 20000 : t.tag - 10000;
      f.push_back(i32(t.field, l.global + static_cast<size_t>(idx) * 4, t.lo, t.hi));
    }
    const size_t rec = l.global + 53 * 4 + 6 * 4;
    f.push_back(i32(hm::GmMono, rec + 8, 0, 3));
    f.push_back(i32(hm::FxDistType, rec + 48, 0, 11));
    f.push_back(i32(hm::FxCompType, rec + 52, 0, 4));
  }
  return f;
}

Encoder encoder() {
  Encoder e;
  e.flName = "Harmor";
  e.glType = "gl.harmor";
  e.nativeRank = 0;
  e.generator = true;
  e.fields = fields;
  return e;
}

}  // namespace harmor

// =================================================================== Sytrus
// Layout (FlpGenSytrus.cpp): dword header (version), zlib stream of the
// patch. The encoder works on an uncompressed view: write() re-packs the
// stream as stored deflate blocks (so every value sits at a fixed offset of
// the state and ordinary Fields apply); finish() compresses it again (own
// fixed-Huffman deflate) with a correct adler-32 trailer.
// No default state is built (operator harmonics and 50+ articulators of
// curves would all need FL's defaults).
namespace sytrus {

namespace mf = gl::packs::synths::mfm;
constexpr size_t kBlock = 65535;

bool inflate(const Bytes& b, Bytes& raw) {
  if (b.size() < 6) return false;
  return gen::inflateZlib(gen::Bytes(b.data() + 4, b.size() - 4), raw, 4u << 20);
}
uint32_t adler32(const Bytes& d) {
  uint32_t a = 1, s = 0;
  for (uint8_t x : d) a = (a + x) % 65521u, s = (s + a) % 65521u;
  return (s << 16) | a;
}
void putAdler(ByteWriter& w, const Bytes& raw) {
  const uint32_t ad = adler32(raw);
  for (int k = 3; k >= 0; --k) w.u8((ad >> (8 * k)) & 0xFF);
}
// State offset of raw byte o in the stored form.
size_t stored(size_t o) { return 4 + 2 + (o / kBlock + 1) * 5 + o; }
bool isStored(const Bytes& b, size_t rawSize) {
  if (b.size() != 4 + 2 + ((rawSize + kBlock - 1) / kBlock) * 5 + rawSize + 4 || b[4] != 0x78 || b[5] != 0x01) return false;
  for (size_t at = 6, left = rawSize; left; ) {
    const size_t n = std::min(left, kBlock);
    if ((b[at] & 6) != 0 || getU16(b, at + 1) != n) return false;
    at += 5 + n, left -= n;
  }
  return true;
}
Bytes storedForm(const Bytes& head, const Bytes& raw) {
  ByteWriter w;
  w.bytes(head.data(), 4);
  w.u8(0x78), w.u8(0x01);
  for (size_t at = 0; at < raw.size(); at += kBlock) {
    const size_t n = std::min(kBlock, raw.size() - at);
    w.u8(at + n >= raw.size() ? 1 : 0);
    w.u16(static_cast<uint32_t>(n)), w.u16(static_cast<uint32_t>(~n & 0xFFFF));
    w.bytes(raw.data() + at, n);
  }
  putAdler(w, raw);
  return w.b;
}

// Deflate, fixed Huffman codes, greedy LZ77 over a 32 KB window.
struct BitOut {
  Bytes b;
  uint32_t acc = 0, n = 0;
  void bits(uint32_t v, uint32_t k) {  // LSB first
    acc |= v << n, n += k;
    while (n >= 8) b.push_back(static_cast<uint8_t>(acc)), acc >>= 8, n -= 8;
  }
  void huff(uint32_t code, uint32_t len) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (uint32_t i = 0; i < len; ++i) r = (r << 1) | ((code >> i) & 1);
    bits(r, len);
  }
  void flush() {
    if (n) b.push_back(static_cast<uint8_t>(acc));
    acc = 0, n = 0;
  }
};
void litLen(BitOut& o, int sym) {
  if (sym < 144) o.huff(0x30 + static_cast<uint32_t>(sym), 8);
  else if (sym < 256) o.huff(0x190 + static_cast<uint32_t>(sym - 144), 9);
  else if (sym < 280) o.huff(static_cast<uint32_t>(sym - 256), 7);
  else o.huff(0xC0 + static_cast<uint32_t>(sym - 280), 8);
}
Bytes deflateFixed(const Bytes& in) {
  static const uint16_t lenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t lenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t distBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  BitOut o;
  o.bits(1, 1), o.bits(1, 2);  // final block, fixed codes
  constexpr size_t kHash = 1 << 15, kWin = 32768;
  std::vector<int64_t> head(kHash, -1), prev(in.size(), -1);
  auto hash = [&](size_t i) { return ((static_cast<uint32_t>(in[i]) << 10) ^ (static_cast<uint32_t>(in[i + 1]) << 5) ^ in[i + 2]) & (kHash - 1); };
  auto insert = [&](size_t i) {
    if (i + 2 >= in.size()) return;
    const uint32_t h = hash(i);
    prev[i] = head[h], head[h] = static_cast<int64_t>(i);
  };
  for (size_t i = 0; i < in.size();) {
    size_t bestLen = 0, bestDist = 0;
    if (i + 2 < in.size()) {
      int chain = 64;
      for (int64_t j = head[hash(i)]; j >= 0 && i - static_cast<size_t>(j) <= kWin && chain--; j = prev[static_cast<size_t>(j)]) {
        size_t l = 0;
        while (l < 258 && i + l < in.size() && in[static_cast<size_t>(j) + l] == in[i + l]) ++l;
        if (l > bestLen) bestLen = l, bestDist = i - static_cast<size_t>(j);
        if (l == 258) break;
      }
    }
    if (bestLen >= 3) {
      int c = 28;
      while (lenBase[c] > bestLen) --c;
      litLen(o, 257 + c);
      o.bits(static_cast<uint32_t>(bestLen - lenBase[c]), lenExtra[c]);
      int d = 29;
      while (distBase[d] > bestDist) --d;
      o.huff(static_cast<uint32_t>(d), 5);
      o.bits(static_cast<uint32_t>(bestDist - distBase[d]), distExtra[d]);
      for (size_t k = 0; k < bestLen; ++k) insert(i + k);
      i += bestLen;
    } else {
      litLen(o, in[i]);
      insert(i);
      ++i;
    }
  }
  litLen(o, 256);
  o.flush();
  return o.b;
}

// Offsets (in the uncompressed patch) of everything the decoder reads as one number.
struct Offsets {
  int ver = 0;
  size_t soften = 0, gibbs = 0, delays = 0, rev = 0, revOn = 0, revTempo = 0, eq = 0;
  bool hasSoften = false, hasGibbs = false, hasRev = false, hasRevTempo = false, hasEq = false, eqType = false;
  struct Unit {
    size_t u = 0, ratio = 0, freq = 0, noise = 0, tail = 0, flags = 0, fb = 0, wsBip = 0;
    bool hasWsBip = false;
    int nf = 12;
    struct Art {
      size_t envTail = 0, p1 = 0, lfoTail = 0, p2 = 0;
    };
    std::vector<Art> arts;
  } units[mf::kUnits];
  bool ok = false;
};

Offsets offsets(const Bytes& raw, int ver) {
  Offsets o;
  o.ver = ver;
  size_t at = 0;
  bool bad = false;
  auto need = [&](size_t k) {
    if (bad || at + k > raw.size()) bad = true;
    return !bad;
  };
  auto i32 = [&]() {
    if (!need(4)) return 0;
    const int v = geti(raw, at);
    at += 4;
    return v;
  };
  auto skip = [&](size_t k) {
    if (need(k)) at += k;
  };
  auto pts = [&](int fmt, int n) {
    if (n < 0 || n > 4096) return void(bad = true);
    skip(static_cast<size_t>(n) * (fmt <= 0 ? 12 : fmt == 1 ? 16 : 24));
  };
  auto list = [&]() {
    i32();
    const int fmt = i32(), n = i32();
    pts(fmt, n);
  };
  skip(240 * 4 + 2);
  if (ver > 3) o.soften = at, o.hasSoften = true, skip(4);
  if (ver > 5) skip(1);
  if (ver > 6) skip(1);
  if (ver > 12) o.gibbs = at, o.hasGibbs = true, skip(1);
  int n507 = 0;
  if (ver > 13) n507 = (ver > 14 ? 5 : 1) + (ver > 17 ? 3 : 0) + (ver > 18 ? 1 : 0);
  skip(static_cast<size_t>(n507));
  o.delays = at, skip(3 * 23);
  if (ver >= 10) {
    o.rev = at, o.hasRev = true, skip(36);
    o.revOn = at, skip(1);
    if (ver > 10) o.revTempo = at, o.hasRevTempo = true, skip(1);
  }
  if (ver > 2) o.eq = at, o.hasEq = true, o.eqType = ver > 13, skip(4 + 3 * (ver > 13 ? 16 : 12));
  if (ver < 0x17) {
    const int len = i32();
    if (len < 0) return o;
    skip(static_cast<size_t>(len));
  } else {
    int len = need(1) ? raw[at++] : 0;
    if (len == 0xff) len = i32();
    if (len < 0) return o;
    skip(static_cast<size_t>(len) * 2);
  }
  for (int u = 0; u < mf::kUnits && !bad; ++u) {
    Offsets::Unit& U = o.units[u];
    U.u = at, skip(40);
    int na = 2;
    if (u < mf::kOps) {
      na = 6;
      int nh = 0;
      U.ratio = at, U.freq = at + 4;
      if (ver < 8) {
        skip(8), nh = 24;
      } else {
        U.noise = at + 8, skip(12), nh = ver < 0x11 ? 128 : 256;
      }
      skip(static_cast<size_t>(nh) * 8);
      U.tail = at, skip(20);
      if (ver < 8) U.noise = at, skip(4), U.flags = at, skip(7);
      else U.flags = at, skip(8);
    } else if (u < mf::kOps + mf::kFilters) {
      na = 8;
      U.nf = ver < 9 ? 10 : 12;
      U.fb = at, skip(static_cast<size_t>(U.nf) * 4);
      if (ver < 9) skip(1);
      list();
      skip(20);
      if (ver >= 12) U.wsBip = at, U.hasWsBip = true, skip(1);
    }
    for (int a = 0; a < na && !bad; ++a) {
      Offsets::Unit::Art A;
      const int h0 = i32();
      skip(16);
      const int fmt = i32(), n = i32();
      pts(fmt, n);
      A.envTail = at, skip(16);
      A.p1 = at, skip(20);
      list();
      A.lfoTail = at, skip(16);
      A.p2 = at, skip(h0 >= 3 ? 24 : 20);
      for (int k = 0; k < mf::kMaps; ++k) list(), skip(20);
      U.arts.push_back(A);
    }
  }
  o.ok = !bad;
  return o;
}

int version(const Bytes& b) {
  const uint32_t hdr = b.size() >= 4 ? getU32(b, 0) : 0;
  return static_cast<int>(hdr > 0xffff ? hdr >> 16 : hdr);
}

std::vector<Field> fields(const Bytes& b) {
  std::vector<Field> f;
  Bytes raw;
  if (!inflate(b, raw)) return f;
  const Offsets o = offsets(raw, version(b));
  if (!o.ok) return f;
  auto F = [&](int gl, size_t rawAt, int lo, int hi, Type t = Type::I32) { f.push_back(field(gl, stored(rawAt), t, lo, hi)); };
  auto M = [&](int gl, int i, int lo, int hi) { F(gl, static_cast<size_t>(i) * 4, lo, hi); };
  M(mf::MVolume, 0, 0, 256), M(mf::MLfoLevel, 1, -128, 128), M(mf::MPitch, 2, -48, 48);
  for (int r = 0; r < mf::kRows; ++r)
    for (int col = 0; col < mf::kCols; ++col) {
      M(mf::fm(r, col), 3 + r * 9 + col, -256, 256);
      if (r < mf::kOps && col < mf::kOps) M(mf::rm(r, col), 84 + r * 9 + col, -256, 256);
    }
  const int adsr[8] = {mf::MVolA, mf::MVolD, mf::MVolS, mf::MVolR, mf::MFltA, mf::MFltD, mf::MFltS, mf::MFltR};
  for (int i = 0; i < 8; ++i) M(adsr[i], 165 + i, i % 4 == 2 ? -128 : 0, i % 4 == 2 ? 128 : 256);
  M(mf::MUniOrder, 173, 1, 9), M(mf::MUniPan, 174, 0, 128), M(mf::MUniVol, 175, 16, 128), M(mf::MUniPitch, 176, 0, 256);
  M(mf::MUniSub, 177, 0, 96), M(mf::MUniPhase, 178, 0, 128), M(mf::MUniEnv, 179, 0, 192), M(mf::FxPan, 180, -128, 128);
  M(mf::ChOrder, 181, 0, 9), M(mf::ChDepth, 182, 0, 1024), M(mf::ChSpeed, 183, 0, 1024), M(mf::ChDelay, 184, 0, 128);
  M(mf::ChSpread, 185, 0, 128), M(mf::ChCross, 186, -128, 128), M(mf::ChVol, 187, -128, 128);
  M(mf::MModX, 190, 0, 512), M(mf::MModY, 191, 0, 512);
  if (o.hasSoften) F(mf::MSoften, o.soften, 0, 128);
  if (o.hasGibbs) F(mf::MGibbs, o.gibbs, 0, 1, Type::U8);
  for (int i = 0; i < 3; ++i) {
    const size_t d = o.delays + static_cast<size_t>(i) * 23;
    F(mf::dly(i, 0), d + 22, 0, 1, Type::U8), F(mf::dly(i, 1), d + 16, 0, 2), F(mf::dly(i, 2), d, 0, 128);
    F(mf::dly(i, 3), d + 4, 0, 768), F(mf::dly(i, 4), d + 8, -512, 512), F(mf::dly(i, 5), d + 12, -128, 128);
    F(mf::dly(i, 6), d + 20, 0, 1, Type::U8);
  }
  if (o.hasRev) {
    const int rv[9] = {mf::RvLowCut, mf::RvHighCut, mf::RvPreDelay, mf::RvRoom, mf::RvDiff, mf::RvDecay, mf::RvHighDamp, mf::RvColor, mf::RvVol};
    const int lo[9] = {19, 5, 0, 1, 0, 1, 5, 0, -128}, hi[9] = {3000, 220, 192, 100, 100, 200, 220, 4, 128};
    for (int i = 0; i < 9; ++i) F(rv[i], o.rev + static_cast<size_t>(i) * 4, lo[i], hi[i]);
    F(mf::RvOn, o.revOn, 0, 1, Type::U8);
    if (o.hasRevTempo) F(mf::RvTempo, o.revTempo, 0, 1, Type::U8);
  }
  if (o.hasEq) {
    F(mf::MEqTarget, o.eq, 0, 3);
    const size_t stride = o.eqType ? 16 : 12;
    for (int i = 0; i < 3; ++i) {
      const size_t e = o.eq + 4 + static_cast<size_t>(i) * stride;
      F(mf::eq(i, 3), e, -1800, 1800), F(mf::eq(i, 1), e + 4, 0, 65536), F(mf::eq(i, 2), e + 8, 0, 65536);
      if (o.eqType) F(mf::eq(i, 0), e + 12, 0, 2);
    }
  }
  for (int u = 0; u < mf::kUnits; ++u) {
    const Offsets::Unit& U = o.units[u];
    if (u < mf::kOps) {
      F(mf::op(u, mf::OpPhase), U.u, 0, 256), F(mf::op(u, mf::OpVol), U.u + 4, -256, 256), F(mf::op(u, mf::OpPitchRange), U.u + 8, 0, 4800);
      F(mf::op(u, mf::OpRatio), U.ratio, 0, 640000), F(mf::op(u, mf::OpFreq), U.freq, -500000, 500000), F(mf::op(u, mf::OpNoise), U.noise, 0, 128);
      F(mf::op(u, mf::OpShape), U.tail, 0, 512), F(mf::op(u, mf::OpTension), U.tail + 4, -128, 128), F(mf::op(u, mf::OpSkew), U.tail + 8, 0, 128);
      F(mf::op(u, mf::OpSine), U.tail + 12, 0, 128), F(mf::op(u, mf::OpFilter), U.tail + 16, 0, 128);
      const int flagParams[7] = {mf::OpHalf, mf::OpEven, mf::OpAbs, mf::OpGlobal, mf::OpCenter, mf::OpDeclick, mf::OpPluck};
      for (int i = 0; i < 7; ++i) F(mf::op(u, flagParams[i]), U.flags + static_cast<size_t>(i), 0, 1, Type::U8);
    } else if (u < mf::kOps + mf::kFilters) {
      const int j = u - mf::kOps;
      const int fl[10] = {mf::FlMode, mf::FlEnv, mf::FlCut, mf::FlRes, mf::FlLow, mf::FlBand, mf::FlHigh, mf::FlWsAmp, mf::FlWsMix, mf::FlNext};
      const int lo[10] = {0, 0, 0, 0, 0, 0, 0, 64, 0, -256}, hi[10] = {mf::kFltModes - 1, 256, 256, 256, 128, 128, 128, 192, 128, 256};
      for (int i = 0; i < 10; ++i) F(mf::flt(j, fl[i]), U.u + static_cast<size_t>(i) * 4, lo[i], hi[i]);
      if (U.nf == 12) F(mf::flt(j, mf::FlType), U.fb + 44, 0, mf::kFltTypes - 1);
      F(mf::flt(j, mf::FlWsOn), U.fb + 40, 0, 1);
      F(mf::flt(j, mf::FlHq), U.fb, 0, 1, Type::U8), F(mf::flt(j, mf::FlFlat), U.fb + 1, 0, 1, Type::U8);
      if (U.hasWsBip) F(mf::flt(j, mf::FlWsBipolar), U.wsBip, 0, 1, Type::U8);
    }
    for (int a = 0; a < static_cast<int>(U.arts.size()) && a < mf::unitArts(u); ++a) {
      const auto& A = U.arts[static_cast<size_t>(a)];
      F(mf::art(u, a, mf::AfEnvOn), A.envTail + 4, 0, 1), F(mf::art(u, a, mf::AfLfoOn), A.lfoTail + 4, 0, 1);
      F(mf::art(u, a, mf::AfAtt), A.p1 + 4, 0, 256), F(mf::art(u, a, mf::AfDec), A.p1 + 8, 0, 256);
      F(mf::art(u, a, mf::AfSus), A.p1 + 12, -128, 128), F(mf::art(u, a, mf::AfRel), A.p1 + 16, 0, 256);
      F(mf::art(u, a, mf::AfTempo), A.lfoTail, 0, 3), F(mf::art(u, a, mf::AfLfoGlobal), A.lfoTail, 0, 3);
      F(mf::art(u, a, mf::AfLfoSpeed), A.p2 + 4, 0, 65536), F(mf::art(u, a, mf::AfLfoSkew), A.p2 + 12, -128, 128);
    }
  }
  return f;
}

// Stored form for the solver; un-mutes matrix cells the target uses.
void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  Bytes raw;
  if (!inflate(b, raw) || raw.size() < 962) return;
  if (version(b) >= 2)
    for (int layer = 0; layer < 2; ++layer)
      for (int r = 0; r < mf::kRows; ++r)
        for (int col = 0; col < mf::kCols; ++col) {
          if (layer == 1 && (r >= mf::kOps || col >= mf::kOps)) continue;
          const float want = pv(g, layer ? mf::rm(r, col) : mf::fm(r, col));
          const size_t at = 800 + static_cast<size_t>(layer * 81 + r * 9 + col);
          if (want != 0.0f && raw[at] != 0) raw[at] = 0;
        }
  const Bytes head(b.begin(), b.begin() + 4);
  b = storedForm(head, raw);
}

// Compressed again, with the adler-32 of the final patch.
void finish(Bytes& b) {
  Bytes raw;
  if (b.size() < 10 || !inflate(b, raw) || !isStored(b, raw.size())) return;
  ByteWriter w;
  w.bytes(b.data(), 4);
  w.u8(0x78), w.u8(0x01);
  w.bytes(deflateFixed(raw));
  putAdler(w, raw);
  b = w.b;
}

Encoder encoder() {
  Encoder e;
  e.flName = "Sytrus";
  e.glType = "gl.matrixfm";
  e.nativeRank = 0;
  e.generator = true;
  e.fields = fields;
  e.write = write;
  e.finish = finish;
  return e;
}

}  // namespace sytrus

}  // namespace

void registerGenA(std::vector<Encoder>& v) {
  v.push_back(osc::encoder());
  v.push_back(keys::encoder());
  v.push_back(midiout::encoder());
  v.push_back(envctl::encoder());
  v.push_back(gms::encoder());
  {
    namespace sy = gl::packs::synths;
    v.push_back(flex::encoder("gl.analog", {{sy::AnTranspose, 39}, {sy::AnAmpA, 0}, {sy::AnAmpR, 4}, {sy::AnCutoff, 18}, {sy::AnCutoff, 21}, {sy::AnFxDrive + sy::FxRevMix, 30}, {sy::AnFxDrive + sy::FxDlyMix, 25}}));
    v.push_back(flex::encoder("gl.keys", {{KyRelease, 4}, {KyMuffle, 18}, {KyMuffle, 21}}));
    v.push_back(flex::encoder("gl.fm", {{fmParam(0, FmAttack), 0}, {fmParam(0, FmRelease), 4}, {FmOctave, 39}}));
  }
  v.push_back(dw::encoder());
  v.push_back(harmless::encoder());
  v.push_back(harmor::encoder());
  v.push_back(sytrus::encoder());
}

}  // namespace gl::flp::enc
