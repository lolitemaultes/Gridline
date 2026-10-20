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
#include "../../packs/modulation/envc.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/samplers/dwave.h"
#include "../../packs/samplers/samplers.h"
#include "../../packs/synths/gms.h"
#include "../../packs/synths/samplekeys.h"
#include "../../packs/synths/synths.h"

namespace gl::flp::enc {

namespace {

using namespace gl::pp;

// Parameter k of a state (NaN when absent).
float pv(const PluginState& s, int k) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : std::numeric_limits<float>::quiet_NaN();
}
int32_t geti(const Bytes& b, size_t at) { return static_cast<int32_t>(getU32(b, at)); }


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

constexpr size_t kEnv = 96, kLev = 436, kSize14 = 460;  // channel envelope copies: 5 × 68 bytes from kEnv
// Channel envelope record fields (dword index × 4).
enum : size_t { RFlag = 0, REnabled = 4, RPredelay = 8, RAttack = 12, RHold = 16, RDecay = 20, RSustain = 24, RRelease = 28, RAmount = 32,
                RLfoPredelay = 36, RLfoAttack = 40, RLfoAmount = 44, RLfoSpeed = 48, RLfoShape = 52, RTension = 56 };

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
  // (J-33: the channel envelope / filter copies decode to PluginState::ins,
  // written whole by write(); the Trio's own envelope and filter stay neutral.)
  return f;
}

// The channel envelope / LFO / filter copies follow PluginState::ins (J-33).
void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  if (b.size() < kSize14) return;  // only v14+ carries the channel copies
  PluginMapping m;
  if (!decodeState("3x Osc", b, m, c)) return;
  if (m.state.ins != g.ins) writeInsCopy(b, kEnv, kLev, g.ins);
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
  // J-33: Sample Keys keeps the 15 raw knob values (parameter index = tag).
  namespace sk = gl::packs::synths::skeys;
  std::vector<Field> f;
  for (int tag = 0; tag < sk::SkCount; ++tag) {
    const size_t o = at + static_cast<size_t>(tag) * 4;
    if (at && o + 4 <= b.size()) f.push_back(i32(tag, o, sk::kSkMin[tag], sk::kSkMax[tag]));
  }
  return f;
}

// The sound set name (PluginState::data).
void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  if (b.size() < 5 || !decodeState("FL Keys", b, m, c) || m.state.data == g.data) return;
  const size_t at = valuesAt(b);
  ByteWriter w;
  w.bytes(b.data(), 4);
  pstr(w, g.data);
  if (at <= b.size()) w.b.insert(w.b.end(), b.begin() + static_cast<std::ptrdiff_t>(at), b.end());
  b = w.b;
}

Encoder encoder() {
  Encoder e;
  e.flName = "FL Keys";
  e.glType = "gl.samplekeys";
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
// Layout: packs/modulation/envc.h (J-41). The state is written whole from the
// articulator parameters (knob block, record floats / extras, option bit) and
// the model text (records, flags, key ranges); nothing is left to the field
// search. Simple-model instances (model 0) export FL's default controller.
namespace envctl {

namespace md = gl::packs::modulation;

md::EnvcState stateOf(const PluginState& g) {
  md::EnvcState st = md::defaultEnvcState();
  if (pv(g, md::EcModel) < 0.5f) return st;
  st.model = md::envcOf(g.data);
  for (int k = 0; k < md::kEnvcKnobs; ++k) {
    const float v = pv(g, md::envcKnobParam(k));
    if (std::isfinite(v)) st.knobs[static_cast<size_t>(k)] = md::envcParamToKnob(k, v);
  }
  st.options = (st.model.uiBits & ~4) | (pv(g, md::EcContinuous) >= 0.5f ? 4 : 0);
  return st;
}

void write(const PluginState& g, Bytes& b, const EncodeContext&) {
  b = md::writeEnvcState(stateOf(g));
}

Encoder encoder() {
  Encoder e;
  e.flName = "Fruity Envelope Controller";
  e.glType = "gl.envctl";
  e.nativeRank = 0;
  e.generator = true;
  e.defaults = [] { return md::writeEnvcState(md::defaultEnvcState()); };
  e.fields = [](const Bytes&) { return std::vector<Field>{}; };
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
    // (in reverse: of two controls behind one parameter, e.g. cutoff 18 / 21, the
    // later one is solved first — J-39: the kept values are 6-digit text, so the
    // finer control absorbs the rounding and the coarse one stays as kept)
    for (auto it = map.rbegin(); it != map.rend(); ++it)
      if (it->second < count) f.push_back(field(it->first, at + static_cast<size_t>(it->second) * 4, Type::F32, 0, 1));
    return f;
  };
}

Encoder encoder(const std::string& type, std::vector<std::pair<int, int>> map) {
  Encoder e;
  e.flName = "FLEX";
  e.glType = type;
  // An approximation of FLEX content: only for states that carry FLEX's
  // selection and controls (imports from FLEX, PluginState::data "flex 1"),
  // which write() puts back — also without the original state (J-39).
  e.nativeRank = 0;
  e.accepts = [](const PluginState& s) { return s.data.rfind("flex 1", 0) == 0; };
  e.generator = true;
  e.defaults = defaults;
  e.fields = fieldsFor(std::move(map));
  e.write = write;
  return e;
}

}  // namespace flex

// =============================================================== DirectWave
// Layout (directwave_spec / FlpJ33.cpp decodeWaveProgram): dword version
// 36..38, records; record 1 = a program (128); zones = record 3 with
// sub-records 500..516. The default state (128 empty programs, ~370 KB) is not
// built: native Wave Program instances are only written over their origin.
namespace dw {

namespace pw = gl::packs::samplers::dw;

// J-33: the decoder produces Wave Program (gl.waveprogram): program tags
// 0..31 in the parameters, everything else in PluginState::data. A changed
// instance rewrites the sounding program (channel 0's) from them: header,
// FX sections, LFOs, Mod Vals, trigger groups and every zone record.
Bytes zoneRecord(const pw::DwZone& z, const std::string& path) {
  std::vector<Rec> rs;
  auto rec = [&](uint32_t id, const ByteWriter& w) { rs.push_back({id, w.b}); };
  {
    ByteWriter w;
    w.u8(z.root), w.u8(z.lo), w.u8(z.hi), w.u8(z.vlo), w.u8(z.vhi), w.u8(z.muteGroup), w.u8(0), w.u8(0), w.u8(z.parallel);
    w.f32(z.gain), w.f32(z.pan), w.i32(z.trigGroup), w.i32(512);
    rec(500, w);
  }
  {
    ByteWriter w;
    w.bytes(z.name.data(), z.name.size());
    rec(501, w);
  }
  {
    ByteWriter w;
    w.bytes(path.data(), path.size());
    rec(502, w);
  }
  {
    ByteWriter w;
    w.u64(0), w.i32(1), w.i32(z.ticks), w.f32(z.rate), w.i32(z.loopMode), w.i32(z.loopStart), w.i32(z.loopEnd), w.i32(z.sampleStart), w.i32(16);
    rec(503, w);
  }
  {
    ByteWriter w;
    w.f32(z.tune), w.u8(static_cast<uint8_t>(z.transpose)), w.u8(static_cast<uint8_t>(z.fine)), w.u16(static_cast<uint16_t>(z.keytrack));
    rec(504, w);
  }
  {
    ByteWriter w;
    w.u8(z.ts), w.u8(z.tsSync), w.f32(z.tsTime), w.f32(z.tsGrain), w.f32(z.tsSmooth);
    rec(505, w);
  }
  {
    ByteWriter w;
    for (float v : z.send) w.f32(v);
    w.zeros(32);
    rec(506, w);
  }
  for (const auto& f : z.f) {
    ByteWriter w;
    w.u8(f.type), w.zeros(3), w.f32(f.cut), w.f32(f.res), w.f32(f.shape), w.f32(0);
    rec(507, w);
  }
  {
    ByteWriter w;
    w.u8(z.beatSync), w.u8(z.waveSlice);
    rec(508, w);
  }
  {
    ByteWriter w;
    for (float v : z.amp) w.f32(v);
    rec(509, w);
  }
  for (int k = 0; k < 4; ++k) {
    ByteWriter w;
    w.u8(z.fxOn[k]), w.f32(z.fxA[k]), w.f32(z.fxB[k]);
    rec(510 + static_cast<uint32_t>(k), w);
  }
  for (const auto& e : z.menv) {
    ByteWriter w;
    for (float v : e) w.f32(v);
    rec(514, w);
  }
  for (const auto& l : z.lfo) {
    ByteWriter w;
    w.u8(l.wave), w.u8(l.sync), w.zeros(2), w.f32(l.rate), w.f32(l.phase), w.f32(l.attack), w.f32(0);
    rec(515, w);
  }
  for (const auto& s : z.slot) {
    ByteWriter w;
    w.u16(static_cast<uint16_t>(s.src)), w.u16(static_cast<uint16_t>(s.dst)), w.f32(s.amt);
    rec(516, w);
  }
  rs.push_back({4, {}});
  ByteWriter w;
  putRecs(w, rs);
  return w.b;
}

void write(const PluginState& g, Bytes& b, const EncodeContext& c) {
  PluginMapping m;
  if (b.size() < 16) return;
  // (a template without zones — defaults() — does not decode: it is filled all the same)
  if (decodeState("DirectWave", b, m, c) && m.state.data == g.data && m.state.params == g.params) return;
  pw::DwProgram p;
  if (!pw::parseProgram(g.data, p)) return;
  auto t = [&](int k) { return static_cast<float>(pv(g, k)); };
  std::vector<Rec> top = parseRecs(b, 4);
  int chanProgram = 0;
  for (Rec& r : top) {
    if (r.id == 6 && r.data.size() >= 4) putU32(r.data, 0, (getU32(r.data, 0) & ~3u) | (p.clipVolume ? 1u : 0u) | (p.compatVelocity ? 2u : 0u));
    if (r.id == 518 && r.data.size() >= 6) putI32(r.data, 1, p.sinc ? 1 : 0), r.data[5] = p.hq ? 1 : 0;
    if (r.id == 5 && r.data.size() >= 0x248 && getU32(r.data, 0) == 0) {
      chanProgram = r.data[0x208];
      putI32(r.data, 0x214, p.bendRange);
      putF32(r.data, 0x238, p.cc1), putF32(r.data, 0x23C, p.cc7), putF32(r.data, 0x240, p.cc10), putF32(r.data, 0x244, p.cc11);
    }
  }
  for (Rec& prog : top) {
    if (prog.id != 1) continue;
    std::vector<Rec> pr = parseRecs(prog.data, 0);
    bool mine = false;
    for (const Rec& h : pr)
      if (h.id == 100 && h.data.size() >= 4 && static_cast<int>(getU32(h.data, 0)) == chanProgram) mine = true;
    if (!mine) continue;
    std::vector<Rec> out;
    int lfoK = 0, modK = 0;
    for (Rec r : pr) {
      Bytes& d = r.data;
      switch (r.id) {
        case 100:
          if (d.size() >= 18) {
            d[4] = static_cast<uint8_t>(p.playMode), d[5] = p.glideAuto;
            putF32(d, 6, t(pw::DwMaster)), putF32(d, 10, t(pw::DwGlide)), putU32(d, 14, static_cast<uint32_t>(p.zones.size()));
          }
          break;
        case 102: d.assign(p.name.begin(), p.name.end()); break;
        case 104:
          if (d.size() >= 10) d[0] = p.driveOn[0], d[1] = p.driveOn[1], putF32(d, 2, t(pw::DwDriveA)), putF32(d, 6, t(pw::DwDriveB));
          break;
        case 105:
          if (d.size() >= 18) {
            d[0] = p.delayOn, d[1] = p.pingPong;
            for (int k = 0; k < 4; ++k) putF32(d, 2 + 4 * static_cast<size_t>(k), t(pw::DwDelayTime + k));
          }
          break;
        case 106:
          if (d.size() >= 17) {
            d[0] = p.revOn;
            for (int k = 0; k < 4; ++k) putF32(d, 1 + 4 * static_cast<size_t>(k), t(pw::DwRevRoom + k));
          }
          break;
        case 107:
          if (d.size() >= 17) {
            d[0] = p.choOn;
            for (int k = 0; k < 4; ++k) putF32(d, 1 + 4 * static_cast<size_t>(k), t(pw::DwChoDelay + k));
          }
          break;
        case 108:
          if (d.size() >= 20 && lfoK < 2) {
            d[0] = static_cast<uint8_t>(p.plfo[lfoK].wave), d[1] = p.plfo[lfoK].sync;
            putF32(d, 4, t(lfoK ? pw::DwLfo2Rate : pw::DwLfo1Rate));
            putF32(d, 8, t(lfoK ? pw::DwLfo2Phase : pw::DwLfo1Phase));
            putF32(d, 12, t(lfoK ? pw::DwLfo2Attack : pw::DwLfo1Attack));
            ++lfoK;
          }
          break;
        case 109:
          if (d.size() == 4 && modK < 4) putF32(d, 0, t(pw::DwModVal1 + modK++));
          break;
        case 110:
          if (d.size() >= 13) {
            const uint32_t gi = getU32(d, 0);
            if (gi >= 1 && gi <= 99) {
              const pw::DwTrigger& tg = p.trig[gi];
              d[4] = static_cast<uint8_t>(tg.type), putF32(d, 5, tg.prob), putF32(d, 9, tg.overlap);
            }
          }
          break;
        default: break;
      }
      if (r.id == 3 || r.id == 2) continue;  // zones are rebuilt below; the end record follows them
      out.push_back(std::move(r));
    }
    for (const auto& z : p.zones) out.push_back({3, zoneRecord(z, flPath(z.path, c))});
    out.push_back({2, {}});
    ByteWriter w;
    putRecs(w, out);
    prog.data = w.b;
    break;
  }
  ByteWriter w;
  w.bytes(b.data(), 4);
  putRecs(w, top);
  b = w.b;
}

// J-39: a fresh version-38 state (what FL 20..25 save): settings records 6 /
// 518, the 16 MIDI channel records (channel n plays program n: pitch bend
// range 200, CC 7 / 10 / 11 at FL's defaults) and 128 empty programs (header,
// "---" name, FX / LFO / Mod Val records at FL's defaults, 100 trigger
// groups). write() then fills the sounding program from Gridline's data.
Bytes defaults() {
  static const Bytes d = [] {
    std::vector<Rec> top;
    top.push_back({6, Bytes(16, 0)});
    top.push_back({518, Bytes{1, 0, 0, 0, 0, 0, 0}});
    for (uint32_t ch = 0; ch < 16; ++ch) {
      Bytes r(628, 0);
      putU32(r, 0, ch), putU32(r, 4, 60), putU32(r, 0x208, ch), putU32(r, 0x214, 200);
      putU32(r, 0x23C, 0x3F499326u), putF32(r, 0x240, 0.5f), putF32(r, 0x244, 1.0f);
      top.push_back({5, r});
    }
    auto floats = [](uint8_t lead, std::initializer_list<float> v) {
      ByteWriter w;
      w.u8(lead);
      for (float x : v) w.f32(x);
      return w.b;
    };
    for (uint32_t p = 0; p < 128; ++p) {
      std::vector<Rec> pr;
      {
        Bytes h(30, 0);
        putU32(h, 0, p), putF32(h, 6, 1.0f);
        pr.push_back({100, h});
      }
      pr.push_back({102, Bytes{'-', '-', '-'}});
      pr.push_back({103, {}});
      pr.push_back({104, Bytes(10, 0)});
      {
        ByteWriter w;
        w.u8(0), w.u8(0), w.f32(0.75f), w.f32(0.75f), w.f32(0.5f), w.f32(0.75f);
        pr.push_back({105, w.b});
      }
      pr.push_back({106, floats(0, {0.25f, 0.5f, 0.75f, 0.25f})});
      pr.push_back({107, floats(0, {0.25f, 0.5f, 0.25f, 0.0f})});
      for (int k = 0; k < 2; ++k) {
        ByteWriter w;
        w.u32(0), w.f32(0.1f), w.f32(1.0f), w.u32(0), w.u32(0);
        pr.push_back({108, w.b});
      }
      for (int k = 0; k < 4; ++k) pr.push_back({109, Bytes(4, 0)});
      for (uint32_t gidx = 0; gidx < 100; ++gidx) {
        ByteWriter w;
        w.u32(gidx), w.u8(0), w.f32(1.0f), w.f32(0.0f);
        pr.push_back({110, w.b});
      }
      pr.push_back({2, {}});
      ByteWriter w;
      putRecs(w, pr);
      top.push_back({1, w.b});
    }
    ByteWriter w;
    w.u32(38);
    putRecs(w, top);
    return w.b;
  }();
  return d;
}

Encoder encoder() {
  Encoder e;
  e.flName = "DirectWave";
  e.glType = "gl.waveprogram";
  e.nativeRank = 0;  // J-39: also without the origin (from defaults())
  e.generator = true;
  e.defaults = defaults;
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
  v.push_back(harmorEncoder());  // FlpEncHarmor.cpp (J-39)
  v.push_back(sytrusEncoder());  // FlpEncSytrus.cpp (J-39)
}

}  // namespace gl::flp::enc
