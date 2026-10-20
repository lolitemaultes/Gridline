// Gridline — FLP export: plugin state encoders for FlpJ22*.cpp (drums, strings, VA, DCO, metal groups).
// The reverse of the decoders in those files; see FlpEncode.h for the model.
//
// Layouts follow the decoders' layout notes (FlpJ22Drums / Strings / Va / Dco /
// Metal.cpp). Value laws are never inverted here: fields name where a value is
// stored and its stored range; the engine solves each through the decoder. The
// optional inverses are fast paths only (checked by the engine).
//
// Defaults: every state is built field by field. Parameter values come from
// the Gridline descriptors' defaults (which carry the reference controls'
// defaults, controls.md), written through each field's inverse; structure
// (version words, sizes, counts, names, tables) is written with named values.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../../packs/models/drums.h"
#include "../../packs/models/metal.h"
#include "../../packs/models/strings.h"
#include "../../packs/synths/dco.h"
#include "../../packs/synths/va.h"
#include "../PluginInfo.h"
#include "../Png.h"
#include "FlpEncode.h"
#include "FlpGen.h"
#include "FlpJ22Metal.h"
#include "FlpJ22Strings.h"

namespace gl::flp::enc {

namespace {

namespace dr = gl::packs::models::dr;
namespace strs = gl::packs::models::strs;
namespace va = gl::packs::synths::va;
namespace dc = gl::packs::synths::dco;
namespace mt = gl::packs::models::metal;
namespace hm = gl::packs::models::hm;

// ------------------------------------------------------------------ helpers

Field withInverse(int gl, size_t off, Type t, double lo, double hi, std::function<double(double)> inv) {
  return Field{gl, off, t, lo, hi, std::move(inv)};
}
double ident(double v) { return v; }
// f32 knob stored as the value itself (0..1 unless given).
Field f32(int gl, size_t off, double lo = 0, double hi = 1) { return withInverse(gl, off, Type::F32, lo, hi, ident); }
Field i32v(int gl, size_t off, double lo, double hi) { return withInverse(gl, off, Type::I32, lo, hi, ident); }

const ParamDesc* desc(const char* glType, int k) {
  const PluginInfo* info = findPlugin(glType);
  if (!info || k < 0 || static_cast<size_t>(k) >= info->params.size()) return nullptr;
  return &info->params[static_cast<size_t>(k)];
}
double defOf(const char* glType, int k) {
  const ParamDesc* d = desc(glType, k);
  return d ? d->def : 0.0;
}
int choiceCount(const ParamDesc* d) { return d && d->kind == ParamKind::Choice ? static_cast<int>(d->choices.size()) : 0; }

// Writes each field's stored value for the descriptor default (= the reference
// control's default) through the field's inverse (fields without one are left).
void storeDefaults(Bytes& b, const char* glType, const std::vector<Field>& fs) {
  for (const Field& f : fs)
    if (f.inverse) writeField(b, f.offset, f.type, f.inverse(defOf(glType, f.gl)));
}

// Fixed-size NUL-padded byte field (raw bytes, as the decoders read them).
void putText(Bytes& b, size_t at, size_t len, const std::string& s) {
  if (at + len > b.size() || len == 0) return;
  std::memset(b.data() + at, 0, len);
  std::memcpy(b.data() + at, s.data(), std::min(s.size(), len - 1));
}
// UTF-8 → Latin-1 (the decoders turn Latin-1 names into UTF-8).
std::string toLatin1(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      o += static_cast<char>(c);
    } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
      const unsigned v = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
      o += static_cast<char>(v < 0x100 ? v : '?');
      ++i;
    } else {
      o += '?';
      while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
    }
  }
  return o;
}
void putTag(ByteWriter& w, const char* s) { w.bytes(s, 4); }

// The decoded Gridline state of `b` (false when the decoder rejects it).
bool decodeNow(const std::string& flName, const Bytes& b, const EncodeContext& c, PluginState& out) {
  PluginMapping m;
  if (!decodeState(flName, b, m, c)) return false;
  out = m.state;
  return true;
}
float param(const PluginState& g, int k) {
  return k >= 0 && static_cast<size_t>(k) < g.params.size() ? g.params[static_cast<size_t>(k)] : 0.0f;
}

Encoder base(const char* flName, const char* glType, int rank) {
  Encoder e;
  e.flName = flName;
  e.glType = glType;
  e.nativeRank = rank;
  e.generator = true;
  return e;
}

// ================================================================== drums

// ------------------------------------------------------------ Fruit Kick
// i32 version (0), 6 × i32 controls (tags 0..5), i32 seed (low 24 bits).
// Version 1 states (a copy of the channel records after the seed) keep their tail.
constexpr int kFkVersion = 0;
Encoder fruitKick() {
  Encoder e = base("Fruit Kick", "gl.sweepkick", 0);
  static const double lo[6] = {-2400, -1200, 0, 0, 0, 0}, hi[6] = {3600, 1200, 256, 256, 64, 128};
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < 6; ++k) f.push_back(i32v(k, 4 + 4 * static_cast<size_t>(k), lo[k], hi[k]));
    f.push_back(withInverse(dr::SkSeed, 28, Type::U32, 0, 0xFFFFFF, ident));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    w.i32(kFkVersion);
    w.zeros(7 * 4);
    storeDefaults(w.b, "gl.sweepkick", f(w.b));
    return w.b;
  };
  return e;
}

// ------------------------------------------------------------ BassDrum
// "SSLF", u32 size, "2kiK", preset name [256], noise sample path [1024], 23 × f32.
constexpr size_t kBdName = 12, kBdPath = kBdName + 256, kBdVals = kBdPath + 1024, kBdEnd = kBdVals + 23 * 4;
std::string bassDrumNoisePath(int k) {
  return std::string("%FLStudioFactoryData%\\Plugins\\Fruity\\Generators\\BassDrum\\Noise Samples\\") + dr::kickNoiseName(k) + ".wav";
}
std::string cField(const Bytes& b, size_t at, size_t len) {
  std::string s;
  for (size_t k = 0; k < len && at + k < b.size() && b[at + k]; ++k) s += static_cast<char>(b[at + k]);
  return s;
}
Encoder bassDrum() {
  Encoder e = base("BassDrum", "gl.kicksculpt", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < 23; ++k) f.push_back(f32(k, kBdVals + 4 * static_cast<size_t>(k)));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kBdEnd - 8));
    putTag(w, "2kiK");
    w.zeros(kBdEnd - 12);
    storeDefaults(w.b, "gl.kicksculpt", f(w.b));
    putText(w.b, kBdPath, 1024, bassDrumNoisePath(static_cast<int>(defOf("gl.kicksculpt", dr::KsNoiseSample))));
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    if (b.size() != kBdEnd) return;
    if (cField(b, kBdName, 256) != g.data) putText(b, kBdName, 256, g.data);  // preset name
    const int want = static_cast<int>(std::lround(param(g, dr::KsNoiseSample)));
    if (std::max(0, dr::kickNoiseIndex(cField(b, kBdPath, 1024))) != want) putText(b, kBdPath, 1024, bassDrumNoisePath(want));
  };
  return e;
}

// ------------------------------------------------------------ Drumpad
// "SSLF", u32 chunk (288), "3PMD", preset name [64], drum name [64] (kept), 38 × f32
// drum model, f32 output level, u32 0 (older saves: 284-byte chunk, no output).
constexpr size_t kDpName = 12, kDpModel = 0x8C, kDpOld = kDpModel + dr::MdCount * 4, kDpNew = kDpOld + 8;
// Model fields at `at` (the pad stores the repeat selector normalized, the kit
// "3SMD" as the raw index).
void modelFields(std::vector<Field>& f, size_t at, int base, bool rawSelector) {
  for (int k = 0; k < dr::MdCount; ++k) {
    const size_t off = at + 4 * static_cast<size_t>(k);
    if (k >= dr::MdVelSrc1 && k <= dr::MdVelSrc4)
      f.push_back(f32(base + k, off, 0, dr::velDestCount() - 1));
    else if (k == dr::MdRetrig)
      f.push_back(rawSelector ? f32(base + k, off, 0, 3) : withInverse(base + k, off, Type::F32, 0, 1, [](double v) { return v / 3.0; }));
    else
      f.push_back(f32(base + k, off));
  }
}
Encoder drumpad() {
  Encoder e = base("Drumpad", "gl.membranepad", 0);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    modelFields(f, kDpModel, dr::PdModel, false);
    if (b.size() == kDpNew) f.push_back(f32(dr::PdOutput, kDpOld));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kDpOld - 4));  // chunk: header … output level (the trailing word is outside)
    putTag(w, "3PMD");
    w.zeros(kDpNew - 12);
    storeDefaults(w.b, "gl.membranepad", f(w.b));
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext&) {
    if (b.size() == kDpOld && param(g, dr::PdOutput) != 1.0f) {  // older save: append the output level
      b.resize(kDpNew, 0);
      putU32(b, 4, static_cast<uint32_t>(kDpOld - 4));
      putF32(b, kDpOld, param(g, dr::PdOutput));
    }
    if (cField(b, kDpName, 64) != g.data) putText(b, kDpName, 64, g.data);
  };
  return e;
}

// ------------------------------------------------------------ Drumaxx
constexpr size_t kDxName = 12, kDxPads = kDxName + 256, kDxPadBytes = dr::kKitStride * 4,
                 kDxMaster = kDxPads + dr::kKitPads * kDxPadBytes, kDxPatterns = kDxMaster + 16 * 4,
                 kDxPatSize = 2 + dr::kKitPads * dr::kKitSteps, kDxNames = kDxPatterns + dr::kKitPatterns * kDxPatSize,
                 kDxLinks = kDxNames + dr::kKitPads * 128, kDxEnd = kDxLinks + 32;
constexpr uint8_t kDxNone = 0x7F;      // unused pattern length / no trigger key
constexpr uint8_t kDxDefVelocity = 100;  // step velocity of a fresh pattern
constexpr int kDxDefSteps = 16, kDxDefKey = 60;  // fresh pattern: 16 steps, started by C4 + pattern index

void writeKitData(Bytes& b, const dr::KitData& d) {
  putText(b, kDxName, 256, d.kitName);
  for (int p = 0; p < dr::kKitPads; ++p) {
    const std::string& n = d.padName[p];
    const size_t bar = n.find('|');
    const size_t at = kDxNames + static_cast<size_t>(p) * 128;
    putText(b, at, 64, n.substr(0, bar));
    putText(b, at + 64, 64, bar == std::string::npos ? std::string() : n.substr(bar + 1));
  }
  for (int k = 0; k < dr::kKitPatterns; ++k) {
    const size_t at = kDxPatterns + static_cast<size_t>(k) * kDxPatSize;
    const dr::KitPattern& P = d.pattern[k];
    b[at] = P.length <= 0 ? kDxNone : static_cast<uint8_t>(std::min(P.length, dr::kKitSteps) - 1);
    b[at + 1] = P.key < 0 ? kDxNone : static_cast<uint8_t>(P.key);
    std::memcpy(b.data() + at + 2, &P.cell[0][0], dr::kKitPads * dr::kKitSteps);
  }
  std::memcpy(b.data() + kDxLinks, d.link, sizeof d.link);
}
Encoder drumaxx() {
  Encoder e = base("Drumaxx", "gl.membranekit", 0);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const bool raw = b.size() > 12 && std::memcmp(b.data() + 8, "3SMD", 4) == 0;
    for (int p = 0; p < dr::kKitPads; ++p) {
      const size_t at = kDxPads + static_cast<size_t>(p) * kDxPadBytes;
      for (int k : {dr::KpVolume, dr::KpPan, dr::KpMute, dr::KpSolo, dr::KpSpare}) f.push_back(f32(dr::kitPad(p, k), at + 4 * static_cast<size_t>(k)));
      f.push_back(f32(dr::kitPad(p, dr::KpKey), at + 4 * dr::KpKey, 0, 127));
      modelFields(f, at + 4 * dr::KpModel, dr::kitPad(p, dr::KpModel), raw);
    }
    for (int k = 0; k < 16; ++k) f.push_back(f32(dr::kKitMaster + k, kDxMaster + 4 * static_cast<size_t>(k)));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kDxEnd - 8));
    putTag(w, "4SMD");  // version 4: repeat selector stored normalized
    w.zeros(kDxEnd - 12);
    storeDefaults(w.b, "gl.membranekit", f(w.b));
    dr::KitData d;
    for (int p = 0; p < dr::kKitPads; ++p) d.padName[p] = "...";  // unused channel
    for (int k = 0; k < dr::kKitPatterns; ++k) {
      d.pattern[k].length = kDxDefSteps;
      d.pattern[k].key = kDxDefKey + k;
      for (auto& row : d.pattern[k].cell) std::fill(std::begin(row), std::end(row), kDxDefVelocity);
    }
    writeKitData(w.b, d);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    if (b.size() != kDxEnd) return;
    PluginState cur;
    dr::KitData d;
    if (decodeNow("Drumaxx", b, c, cur) && cur.data == g.data) return;
    if (dr::decodeKitData(g.data, d)) writeKitData(b, d);
  };
  return e;
}

// ================================================================== strings

// ------------------------------------------------------------ Plucked!
Encoder plucked() {
  Encoder e = base("Plucked!", "gl.plucker", 0);
  e.fields = [](const Bytes&) {
    return std::vector<Field>{i32v(strs::PkDecay, 0, 0, 256), i32v(strs::PkColor, 4, 0, 128), i32v(strs::PkNormalize, 8, 0, 1),
                              i32v(strs::PkGate, 12, 0, 1), i32v(strs::PkWiden, 16, 0, 1)};
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    w.zeros(5 * 4);
    storeDefaults(w.b, "gl.plucker", f(w.b));
    return w.b;
  };
  return e;
}

// ------------------------------------------------------------ Sakura
// "SSLF", u32 size, "1rkS", 114 × f32 (tag order), preset name [128].
constexpr size_t kTsVals = 12, kTsName = kTsVals + 4 * strs::TsCount, kTsEnd = kTsName + 128;
bool tsRaw(int t) {
  if (t == strs::TsS2Ratio || t == strs::TsDlySync) return true;
  if (t < strs::TsMod1 || t >= strs::TsS2Ratio) return false;
  const int k = (t - strs::TsMod1) % strs::TmStride;
  return k == strs::TmSource || k == strs::TmDest;
}
Encoder sakura() {
  Encoder e = base("Sakura", "gl.twinstring", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int t = 0; t < strs::TsCount; ++t) {
      const size_t off = kTsVals + 4 * static_cast<size_t>(t);
      if (t == strs::TsTranspose)  // octave step 0..6 stored as a 0..1 knob position
        f.push_back(withInverse(t, off, Type::F32, 0, 1, [](double s) { return s / 6.0; }));
      else if (tsRaw(t))
        f.push_back(f32(t, off, 0, std::max(1, choiceCount(desc("gl.twinstring", t)) - 1)));
      else
        f.push_back(f32(t, off));
    }
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kTsEnd - 8));
    putTag(w, "1rkS");
    w.zeros(kTsEnd - 12);
    storeDefaults(w.b, "gl.twinstring", f(w.b));
    return w.b;
  };
  return e;
}

// ------------------------------------------------------------ Transistor Bass
// See parseStepBass (FlpJ22Strings.h). Offsets of the values the decoder reads.
struct TbLayout {
  bool ok = false;
  size_t root = 0, flag = 0, trailer = 0;
  std::vector<size_t> progPatch, progSeq;  // offsets of each program's patch / sequence index
  std::vector<size_t> patch;               // offset of each patch's first float
  std::vector<size_t> bpm;                 // offset of each sequence's tempo (f64)
};
constexpr size_t kTbHead = 48;  // 12 header words
TbLayout tbLayout(const Bytes& b) {
  TbLayout L;
  size_t at = kTbHead + 2;
  auto has = [&](size_t k) { return at <= b.size() && k <= b.size() - at; };
  auto name = [&]() {
    if (!has(4)) return false;
    const uint32_t len = getU32(b, at);
    at += 4;
    if (len > 256 || !has(len)) return false;
    at += len;
    return true;
  };
  L.root = at;
  at += 8;
  L.flag = at++;
  for (int k = 0; k < strs::kSbPrograms; ++k) {
    at += 3;
    if (!name() || !has(12)) return L;
    L.progPatch.push_back(at);
    L.progSeq.push_back(at + 4);
    at += 12;
  }
  for (int k = 0; k < 256; ++k) {
    if (!name() || !has(4 * strs::kSbPatchFloats)) return L;
    L.patch.push_back(at);
    at += 4 * strs::kSbPatchFloats;
  }
  for (int k = 0; k < strs::kSbPrograms; ++k) {
    at += 2;
    if (!name() || !has(6 + 21 * strs::kSbSteps + 8 + 9)) return L;
    at += 6 + 21 * strs::kSbSteps;
    L.bpm.push_back(at);
    at += 8 + 9;
  }
  L.trailer = at;
  L.ok = has(24) && at + 24 == b.size();
  return L;
}
// Serializes a parsed bank (inverse of parseStepBass); header sizes recomputed.
Bytes tbSerialize(const j22::StepBassState& s) {
  ByteWriter w;
  for (uint32_t h : s.head) w.u32(h);
  w.u16(s.version);
  w.u32(s.root);
  w.u32(s.root2);
  w.u8(s.flag);
  auto name = [&w](const std::string& n) {  // u32 length (with the NUL) + text + NUL
    const std::string t = n.substr(0, 255);
    w.u32(static_cast<uint32_t>(t.size() + 1));
    w.bytes(t.data(), t.size());
    w.u8(0);
  };
  for (const auto& p : s.programs) {
    w.u16(p.a);
    w.u8(p.b);
    name(p.name);
    w.u32(p.patch);
    w.u32(p.sequence);
    w.u32(p.c);
  }
  for (const auto& p : s.patches) {
    name(p.name);
    for (float v : p.v) w.f32(v);
  }
  for (const auto& q : s.sequences) {
    w.u16(q.pre);
    name(q.name);
    w.u16(q.b);
    w.u32(q.length);
    for (const auto& st : q.steps) {
      w.u8(st.lead);
      w.u8(st.b1);
      w.u8(st.accent);
      w.i32(st.note);
      w.u8(st.gate);
      w.f64(st.d);
      w.f32(st.x);
      w.u8(st.slide);
    }
    w.f64(q.bpm);
    w.bytes(q.tail, sizeof q.tail);
  }
  for (uint32_t t : s.trailer) w.u32(t);
  const auto n = static_cast<uint32_t>(w.b.size());
  putU32(w.b, 7 * 4, n - 36);  // the two size words the decoder checks
  putU32(w.b, 10 * 4, n - 48);
  return w.b;
}
// Applies Gridline's bank text (stepBassData) onto a parsed bank.
bool tbApplyData(const std::string& text, j22::StepBassState& s) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != "stepbass 1") return false;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string key;
    size_t k = 0;
    if (!(ls >> key >> k)) continue;
    std::string nm;
    const size_t bar = line.find(" | ");
    if (bar != std::string::npos) nm = line.substr(bar + 3);
    if (key == "prog" && k < s.programs.size()) {
      ls >> s.programs[k].patch >> s.programs[k].sequence;
    } else if (key == "patch" && k < s.patches.size()) {
      for (int i = 0; i < strs::SbPatchCount; ++i) ls >> s.patches[k].v[i];
      s.patches[k].name = nm;
    } else if (key == "seq" && k < s.sequences.size()) {
      auto& q = s.sequences[k];
      ls >> q.length >> q.bpm;
      for (auto& st : q.steps) {
        std::string cell;
        ls >> cell;
        int note = 60, gate = 1, acc = 0, slide = 0;
        if (std::sscanf(cell.c_str(), "%d:%d:%d:%d", &note, &gate, &acc, &slide) == 4) {
          st.note = note;
          st.gate = static_cast<uint8_t>(gate != 0);
          st.accent = static_cast<uint8_t>(acc != 0);
          st.slide = static_cast<uint8_t>(slide != 0);
        }
      }
      q.name = nm;
    }
  }
  return true;
}
// A fresh bank: 128 programs (program k = patch k + sequence k), default patches, empty sequences.
constexpr uint16_t kTbVersion = 3;
constexpr uint32_t kTbHeadWords[12] = {1, 1, 8, 0, 2, 3, 2, 0, 0, 0, 0, 0};  // words 7 / 10: sizes (set on write)
constexpr uint8_t kTbProgFlag = 1;  // second program byte (1 in every shipped bank)
constexpr uint32_t kTbTrailer[6] = {4, 0, 0, 0, 0x3F800000, 4};  // mode, program / sequence / patch selectors, f32 1.0, word
Bytes tbDefaults() {
  j22::StepBassState s;
  std::copy(std::begin(kTbHeadWords), std::end(kTbHeadWords), s.head);
  s.version = kTbVersion;
  s.root = s.root2 = static_cast<uint32_t>(defOf("gl.stepbass", strs::SbRoot));
  s.flag = static_cast<uint8_t>(defOf("gl.stepbass", strs::SbSeqOn) != 0);
  s.programs.resize(strs::kSbPrograms);
  for (int k = 0; k < strs::kSbPrograms; ++k) {
    auto& p = s.programs[static_cast<size_t>(k)];
    p.b = kTbProgFlag;
    p.patch = p.sequence = static_cast<uint32_t>(k);
  }
  s.patches.resize(256);  // 128 working patches + their stored originals (zero)
  for (int k = 0; k < strs::kSbPrograms; ++k)
    for (int i = 0; i < strs::SbPatchCount; ++i) s.patches[static_cast<size_t>(k)].v[i] = static_cast<float>(defOf("gl.stepbass", i));
  s.sequences.resize(strs::kSbPrograms);
  for (auto& q : s.sequences) q.bpm = defOf("gl.stepbass", strs::SbBpm);
  std::copy(std::begin(kTbTrailer), std::end(kTbTrailer), s.trailer);
  return tbSerialize(s);
}
Encoder transistorBass() {
  Encoder e = base("Transistor Bass", "gl.stepbass", 0);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const TbLayout L = tbLayout(b);
    if (!L.ok || L.trailer + 8 > b.size()) return f;
    const uint32_t prog = std::min<uint32_t>(getU32(b, L.trailer + 4), strs::kSbPrograms - 1);
    const uint32_t patch = std::min<uint32_t>(getU32(b, L.progPatch[prog]), 255), seq = getU32(b, L.progSeq[prog]);
    for (int i = 0; i < strs::SbPatchCount; ++i) f.push_back(f32(i, L.patch[patch] + 4 * static_cast<size_t>(i)));
    f.push_back(withInverse(strs::SbRoot, L.root, Type::U32, 0, 127, ident));
    f.push_back(withInverse(strs::SbSeqOn, L.flag, Type::U8, 0, 1, ident));
    if (seq < L.bpm.size()) f.push_back(withInverse(strs::SbBpm, L.bpm[seq], Type::F64, 10, 522, ident));
    return f;
  };
  e.defaults = tbDefaults;
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    j22::StepBassState s;
    if (!j22::parseStepBass(b.data(), b.size(), s)) return;
    PluginState cur;
    if (decodeNow("Transistor Bass", b, c, cur) && cur.data == g.data && param(cur, strs::SbProgram) == param(g, strs::SbProgram)) return;
    tbApplyData(g.data, s);
    s.trailer[1] = static_cast<uint32_t>(std::clamp(std::lround(param(g, strs::SbProgram)), 0L, static_cast<long>(strs::kSbPrograms - 1)));
    b = tbSerialize(s);
  };
  return e;
}

// ================================================================== VA

// Field of a VA control stored as a 0..1 position (see FlpJ22Va.cpp `put`):
// raw combo / digit indices as the value; switches / two-way selectors on at
// any non-zero value; selectors by position; knobs linear in the position.
Field vaField(const char* glType, int t, size_t off, bool raw) {
  const ParamDesc* d = desc(glType, t);
  if (!d) return f32(t, off);
  if (raw) return f32(t, off, d->min, d->max);
  const int n = choiceCount(d);
  if (d->kind == ParamKind::Toggle || n == 2) return f32(t, off);
  if (n > 2) return withInverse(t, off, Type::F32, 0, 1, [n](double v) { return v / (n - 1); });
  const double lo = d->min, span = d->max - d->min;
  return withInverse(t, off, Type::F32, 0, 1, [lo, span](double v) { return span != 0 ? (v - lo) / span : 0.0; });
}
// Whole-semitone pitch stored on a ±24 st knob: the middle of the step.
Field semiField(int t, size_t off, bool towardZero) {
  return withInverse(t, off, Type::F32, 0, 1, [towardZero](double st) {
    const double h = towardZero && st < 0 ? -0.5 : 0.5;
    return std::clamp((st + 24.0 + h) / 48.0, 0.0, 1.0);
  });
}
// "va 1" text: "cc <tag> <controller>" lines and "name <text>".
struct VaText {
  std::map<int, int> cc;
  std::string name;
};
VaText vaText(const std::string& s) {
  VaText v;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("cc ", 0) == 0) {
      int t = -1, n = -1;
      if (std::sscanf(line.c_str() + 3, "%d %d", &t, &n) == 2) v.cc[t] = n;
    } else if (line.rfind("name ", 0) == 0) {
      v.name = line.substr(5);
    }
  }
  return v;
}

// ------------------------------------------------------------ PoiZone
// "SSLF", u32 size, "4reV", 81 × f32 controls, 74 × f32 CC links (−1 none), u32 0, name [64].
constexpr int kPzTags = 81, kPzCc = 74;
constexpr size_t kPzVals = 12, kPzCcAt = kPzVals + 4 * kPzTags, kPzZero = kPzCcAt + 4 * kPzCc, kPzName = kPzZero + 4, kPzEnd = kPzName + 64;
constexpr float kNoCc = -1.0f;
Encoder poiZone() {
  Encoder e = base("PoiZone", "gl.twinva", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int t = 0; t < kPzTags; ++t) {
      const size_t off = kPzVals + 4 * static_cast<size_t>(t);
      if (t == va::TwModWheel) continue;  // tag 74: unused by the reference (stored 0)
      if (t == va::TwBPitch || t == va::TwTranspose) f.push_back(semiField(t, off, false));
      else f.push_back(vaField("gl.twinva", t, off, t >= va::TwUnison));
    }
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kPzEnd - 8));
    putTag(w, "4reV");
    w.zeros(kPzEnd - 12);
    storeDefaults(w.b, "gl.twinva", f(w.b));
    for (int t = 0; t < kPzCc; ++t) putF32(w.b, kPzCcAt + 4 * static_cast<size_t>(t), kNoCc);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (b.size() != kPzEnd || (decodeNow("PoiZone", b, c, cur) && cur.data == g.data)) return;
    const VaText v = vaText(g.data);
    for (int t = 0; t < kPzCc; ++t) {
      auto it = v.cc.find(t);
      putF32(b, kPzCcAt + 4 * static_cast<size_t>(t), it == v.cc.end() ? kNoCc : static_cast<float>(it->second));
    }
    putText(b, kPzName, 64, toLatin1(v.name));
  };
  return e;
}

// ------------------------------------------------------------ Sawer
// "SSLF", u32 size, "1rwS", 96 × f32 controls, 96 × u8 CC links (0xFF none), name [128].
constexpr int kSwTags = 96;
constexpr size_t kSwVals = 12, kSwCcAt = kSwVals + 4 * kSwTags, kSwName = kSwCcAt + kSwTags, kSwEnd = kSwName + 128;
constexpr uint8_t kNoCcByte = 0xFF;
Encoder sawer() {
  Encoder e = base("Sawer", "gl.sawswarm", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int t = 0; t < kSwTags; ++t) {
      const size_t off = kSwVals + 4 * static_cast<size_t>(t);
      if (t == va::SwModWheel) continue;  // tag 73: unused by the reference (stored 0)
      if (t == va::SwTranspose) f.push_back(semiField(t, off, true));
      else f.push_back(vaField("gl.sawswarm", t, off, (t >= va::SwArpSync && t <= va::SwBendRange) || t >= va::SwModDest1));
    }
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    putTag(w, "SSLF");
    w.u32(static_cast<uint32_t>(kSwEnd - 8));
    putTag(w, "1rwS");
    w.zeros(kSwEnd - 12);
    storeDefaults(w.b, "gl.sawswarm", f(w.b));
    std::memset(w.b.data() + kSwCcAt, kNoCcByte, kSwTags);
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (b.size() != kSwEnd || (decodeNow("Sawer", b, c, cur) && cur.data == g.data)) return;
    const VaText v = vaText(g.data);
    for (int t = 0; t < kSwTags; ++t) {
      auto it = v.cc.find(t);
      b[kSwCcAt + static_cast<size_t>(t)] = it == v.cc.end() ? kNoCcByte : static_cast<uint8_t>(it->second);
    }
    putText(b, kSwName, 128, toLatin1(v.name));
  };
  return e;
}

// ------------------------------------------------------------ BooBass
// u32 version (1), i32 bass, mid, treble 0..65535.
constexpr uint32_t kBbVersion = 1;
Encoder booBass() {
  Encoder e = base("BooBass", "gl.fingerbass", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int k = 0; k < 3; ++k)
      f.push_back(withInverse(k, 4 + 4 * static_cast<size_t>(k), Type::I32, 0, 65535, [](double v) { return std::round(v * 65535.0); }));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    w.u32(kBbVersion);
    w.zeros(3 * 4);
    storeDefaults(w.b, "gl.fingerbass", f(w.b));
    return w.b;
  };
  return e;
}

// ================================================================== DCO

// ------------------------------------------------------------ Fruity DX10
// i32 version (1), i32 per tag 0..20 (0..65536), i32 octave −3..3 (version 2 adds a channel copy).
constexpr int32_t kDx10Version = 1;
Encoder dx10() {
  Encoder e = base("Fruity DX10", "gl.fmduo", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int t = 0; t <= 20; ++t)
      f.push_back(withInverse(t, 4 + 4 * static_cast<size_t>(t), Type::I32, 0, 65536, [](double v) { return std::round(v * 65536.0); }));
    f.push_back(i32v(dc::FdOctave, 4 + 4 * 21, -3, 3));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    w.i32(kDx10Version);
    w.zeros(22 * 4);
    storeDefaults(w.b, "gl.fmduo", f(w.b));
    return w.b;
  };
  return e;
}

// ------------------------------------------------------------ SimSynth
// 45 f64, 19 spare i32, 19 i32 switches, 12 spare i32, i32 version (14).
constexpr int kSimDouble[36] = {3, 4, 5, 10, 11, 12, 17, 18, 19, 21, 22, 26, 27, 28, 29, 32, 33, 35,
                                36, 37, 38, 39, 41, 42, 43, 44, 0, 1, 2, 7, 8, 9, 14, 15, 16, 30};
constexpr int kSimSwitch[18] = {18, 13, 16, 12, 14, 0, 4, 8, 17, 1, 2, 3, 5, 6, 7, 9, 10, 11};
constexpr int kSimShapeDouble[4] = {6, 13, 20, 24};
constexpr double kSimShapeValue[5] = {0.2, 0.1, 0.0, 0.4, 0.3};  // stored double of each selector entry
constexpr size_t kSimSwitches = 45 * 8 + 19 * 4, kSimVersionAt = kSimSwitches + 19 * 4 + 12 * 4, kSimEnd = kSimVersionAt + 4;
constexpr int32_t kSimVersion = 14;
Encoder simSynth() {
  Encoder e = base("SimSynth", "gl.warmsynth", 0);
  e.fields = [](const Bytes&) {
    std::vector<Field> f;
    for (int t = 0; t < 36; ++t) {
      const size_t off = 8 * static_cast<size_t>(kSimDouble[t]);
      if (t == dc::WmOsc1Coarse || t == dc::WmOsc2Coarse || t == dc::WmOsc3Coarse)
        f.push_back(withInverse(t, off, Type::F64, 0, 1, [](double st) { return (st + 24.5) / 48.0; }));
      else
        f.push_back(withInverse(t, off, Type::F64, 0, 1, ident));
    }
    for (int k = 0; k < 18; ++k) f.push_back(i32v(36 + k, kSimSwitches + 4 * static_cast<size_t>(kSimSwitch[k]), 0, 1));
    for (int k = 0; k < 4; ++k)
      f.push_back(withInverse(54 + k, 8 * static_cast<size_t>(kSimShapeDouble[k]), Type::F64, 0, 0.4,
                              [](double s) { return kSimShapeValue[std::clamp(static_cast<int>(std::lround(s)), 0, 4)]; }));
    return f;
  };
  e.defaults = [f = e.fields] {
    Bytes b(kSimEnd, 0);
    storeDefaults(b, "gl.warmsynth", f(b));
    putI32(b, kSimVersionAt, kSimVersion);
    return b;
  };
  return e;
}

// ------------------------------------------------------------ Kepler (classic)
// u32 0, u32 size − 12, u32 0, u32 name length, name, u32 version (4), u32
// count (36), count × f32, u8 arp quantize, f32 velocity → volume, f32 sub
// phase, (version ≥ 3) u8 chorus gate, u8 chorus noise, u8 oversampling.
constexpr int kKepler1Tag[36] = {85, 89, 88, 86, 87, 48, 49, 50, 51, 81, 0, 5, 6, 7, 8, 9, 10, 34,
                                 36, 37, 38, 43, 44, 45, 39, 47, 61, 62, 63, 64, 69, 82, 83, 72, 52, 53};
constexpr uint32_t kKepler1Version = 4, kKepler1Count = 36;
const char* const kKepler1Name = "Kepler-1";
const char* const kKeplerExoName = "Kepler-2";
// A DCO Synth control stored as a 0..1 position (selectors / switches by position).
Field dcoField(int gl, size_t off) {
  const ParamDesc* d = desc("gl.dcosynth", gl);
  const int n = choiceCount(d);
  if (n > 1) return withInverse(gl, off, Type::F32, 0, 1, [n](double v) { return v / (n - 1); });
  return f32(gl, off);
}
struct K1 {
  bool ok = false;
  size_t vals = 0, tail = 0;
  uint32_t ver = 0, count = 0;
};
K1 k1Layout(const Bytes& b) {
  K1 L;
  if (b.size() < 16) return L;
  const uint32_t nameLen = getU32(b, 12);
  if (nameLen > 64 || 16 + nameLen + 8 > b.size()) return L;
  L.ver = getU32(b, 16 + nameLen);
  L.count = getU32(b, 16 + nameLen + 4);
  L.vals = 16 + nameLen + 8;
  L.tail = L.vals + 4 * static_cast<size_t>(std::min<uint32_t>(L.count, 36));
  L.ok = L.count <= 36 && L.tail + (L.ver > 2 ? 12 : 9) == b.size();
  return L;
}
Bytes k1Build(const std::vector<float>& tags, const uint8_t tail[12]) {
  ByteWriter w;
  const auto nameLen = static_cast<uint32_t>(std::strlen(kKepler1Name) + 1);
  w.u32(0);
  w.u32(0);  // size − 12 (set below)
  w.u32(0);
  w.u32(nameLen);
  w.bytes(kKepler1Name, nameLen);
  w.u32(kKepler1Version);
  w.u32(static_cast<uint32_t>(tags.size()));
  for (float v : tags) w.f32(v);
  w.bytes(tail, 12);
  putU32(w.b, 4, static_cast<uint32_t>(w.b.size() - 12));
  return w.b;
}
Encoder kepler() {
  Encoder e = base("Kepler", "gl.dcosynth", 0);
  e.accepts = [](const PluginState& s) { return std::lround(param(s, dc::DcPanel)) == dc::PanelOne; };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const K1 L = k1Layout(b);
    if (!L.ok) return f;
    for (uint32_t k = 0; k < L.count; ++k) f.push_back(dcoField(kKepler1Tag[k], L.vals + 4 * k));
    f.push_back(withInverse(dc::DcArpQuantize, L.tail, Type::U8, 0, 1, ident));
    f.push_back(dcoField(dc::DcVelVol, L.tail + 1));
    f.push_back(dcoField(dc::DcDco1SubPhase, L.tail + 5));
    if (L.ver > 2) {
      f.push_back(withInverse(dc::DcChorusGate, L.tail + 9, Type::U8, 0, 1, ident));
      f.push_back(withInverse(dc::DcChorusNoise, L.tail + 10, Type::U8, 0, 1, [](double v) { return v > 0 ? 1.0 : 0.0; }));
    }
    return f;
  };
  e.defaults = [f = e.fields] {
    const uint8_t tail[12] = {};  // options off; oversampling 0
    Bytes b = k1Build(std::vector<float>(kKepler1Count, 0.0f), tail);
    storeDefaults(b, "gl.dcosynth", f(b));
    return b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    // Older layouts (version 2: no chorus options, older sync rates; 34
    // tags) are upgraded when a value they cannot hold is wanted.
    const K1 L = k1Layout(b);
    if (!L.ok || (L.ver >= kKepler1Version && L.count == kKepler1Count)) return;
    PluginState cur;
    if (!decodeNow("Kepler", b, c, cur)) return;
    bool need = false;
    for (int k : {dc::DcLfo1PhaseOn, dc::DcLfo1Phase, dc::DcChorusGate, dc::DcChorusNoise, dc::DcLfo1Rate}) need |= param(cur, k) != param(g, k);
    if (!need) return;
    std::vector<float> tags(kKepler1Count, 0.0f);
    for (uint32_t k = 0; k < L.count; ++k) tags[k] = getF32(b, L.vals + 4 * k);
    uint8_t tail[12] = {};
    std::memcpy(tail, b.data() + L.tail, L.ver > 2 ? 12 : 9);
    b = k1Build(tags, tail);  // the new tags / options are solved as fields next
  };
  return e;
}

// ------------------------------------------------------------ Kepler Exo
// u32 1, u32 1, u32 size − 12, u32 0, u32 0, u32 size − 24, u32 0, u32 name
// length, name, u32 version (2), u32 count (151), count × f32, u32 1, u32 rows
// (8), rows × {i32 source, i32 destination}, u32 2, u64 runtime size, runtime.
constexpr uint32_t kExoVersion = 2, kExoCount = 151, kExoRows = 8, kExoRuntime = 56;
constexpr int32_t kExoDco1Pitch = 256, kExoDco2Pitch = 257, kExoNone = -1;
struct Exo {
  bool ok = false;
  size_t vals = 0, matrix = 0;
  uint32_t count = 0, rows = 0;
};
Exo exoLayout(const Bytes& b) {
  Exo L;
  if (b.size() < 32) return L;
  const uint32_t nameLen = getU32(b, 28);
  if (nameLen > 64 || 32 + nameLen + 8 > b.size()) return L;
  L.count = getU32(b, 32 + nameLen + 4);
  L.vals = 32 + nameLen + 8;
  L.matrix = L.vals + 4 * static_cast<size_t>(L.count);
  L.rows = getU32(b, L.matrix + 4);
  L.ok = L.count <= 155 && L.rows <= kExoRows && L.matrix + 8 + 8 * static_cast<size_t>(L.rows) + 12 <= b.size();
  return L;
}
Encoder keplerExo() {
  Encoder e = base("Kepler Exo", "gl.dcosynth", 0);
  e.accepts = [](const PluginState& s) { return std::lround(param(s, dc::DcPanel)) == dc::PanelTwo; };
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    const Exo L = exoLayout(b);
    if (!L.ok) return f;
    for (uint32_t t = 0; t < L.count && t < static_cast<uint32_t>(dc::kDcTags); ++t) f.push_back(dcoField(static_cast<int>(t), L.vals + 4 * t));
    return f;
  };
  e.defaults = [f = e.fields] {
    ByteWriter w;
    const auto nameLen = static_cast<uint32_t>(std::strlen(kKeplerExoName) + 1);
    w.u32(1);
    w.u32(1);
    w.u32(0);  // size − 12
    w.u32(0);
    w.u32(0);
    w.u32(0);  // size − 24
    w.u32(0);
    w.u32(nameLen);
    w.bytes(kKeplerExoName, nameLen);
    w.u32(kExoVersion);
    w.u32(kExoCount);
    w.zeros(4 * kExoCount);
    w.u32(1);  // matrix chunk
    w.u32(kExoRows);
    for (uint32_t r = 0; r < kExoRows; ++r) w.i32(kExoNone), w.i32(kExoNone);
    w.u32(2);  // runtime chunk (rebuilt by the plugin on load)
    w.u64(kExoRuntime);
    w.zeros(kExoRuntime);
    putU32(w.b, 8, static_cast<uint32_t>(w.b.size() - 12));
    putU32(w.b, 20, static_cast<uint32_t>(w.b.size() - 24));
    storeDefaults(w.b, "gl.dcosynth", f(w.b));
    return w.b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    // Matrix rows: source and destination decode together (an empty side
    // clears the row), so they are written as pairs, not solved one by one.
    const Exo L = exoLayout(b);
    PluginState cur;
    if (!L.ok || !decodeNow("Kepler Exo", b, c, cur)) return;
    bool same = true;
    for (int r = 0; r < dc::kDcMatrix; ++r) same = same && param(cur, dc::DcMxSrc1 + r) == param(g, dc::DcMxSrc1 + r) && param(cur, dc::DcMxDst1 + r) == param(g, dc::DcMxDst1 + r);
    if (same) return;
    Bytes rows;
    for (uint32_t r = 0; r < kExoRows; ++r) {
      const long src = std::lround(param(g, dc::DcMxSrc1 + static_cast<int>(r))), dst = std::lround(param(g, dc::DcMxDst1 + static_cast<int>(r)));
      int32_t s = kExoNone, d = kExoNone;
      if (src > 0 && dst > 0) {
        s = static_cast<int32_t>(src - 1);
        d = dst == dc::kDcDestDco1Pitch ? kExoDco1Pitch : dst == dc::kDcDestDco2Pitch ? kExoDco2Pitch : static_cast<int32_t>(dst - 1);
      }
      ByteWriter w;
      w.i32(s);
      w.i32(d);
      rows.insert(rows.end(), w.b.begin(), w.b.end());
    }
    const size_t at = L.matrix + 8;
    b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + 8 * L.rows));
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), rows.begin(), rows.end());
    putU32(b, L.matrix + 4, kExoRows);
    putU32(b, 8, static_cast<uint32_t>(b.size() - 12));
    putU32(b, 20, static_cast<uint32_t>(b.size() - 24));
  };
  return e;
}

// ================================================================== metal

// ------------------------------------------------------------ Fruity Granulizer
// u32 version (7), i32 per tag 0..14 (tag 14: tempo lock, low byte), five
// channel envelope / LFO records (5 × 68 bytes), the levels record (24 bytes).
constexpr int32_t kGrVersion = 7;
constexpr int kGrTags = 15;
constexpr size_t kEnvRec = 68, kEnvRecs = 5, kLevels = 24, kGrEnv = 4 + 4 * kGrTags, kGrEnd = kGrEnv + kEnvRecs * kEnvRec + kLevels;
constexpr int kGrVolEnv = 1;  // record of the volume envelope
int grTags(int ver) { return ver <= 1 ? 10 : ver <= 3 ? 11 : ver == 4 ? 13 : ver <= 6 ? 14 : 15; }
// A channel envelope / LFO record with FL's fresh-channel values.
void putEnvRecord(Bytes& b, size_t at) {
  const gen::ChanEnv d;
  const int32_t v[17] = {0, d.enabled ? 1 : 0, d.predelay, d.attack, d.hold, d.decay, d.sustain, d.release, d.amount,
                         d.lfoPredelay, d.lfoAttack, d.lfoAmount, d.lfoSpeed, d.lfoShape, d.attackTension, d.decayTension, d.releaseTension};
  for (size_t k = 0; k < 17; ++k) putI32(b, at + 4 * k, v[k]);
}
// Levels record of a fresh channel: pan 0, volume / pitch words, cutoff word 256 … (as FL writes it).
constexpr int32_t kGrLevels[6] = {0, 0, 256, 0, 0, 0};
Encoder granulizer() {
  Encoder e = base("Fruity Granulizer", "gl.grainscan", 0);
  e.fields = [](const Bytes& b) {
    std::vector<Field> f;
    if (b.size() < 4) return f;
    const int ver = static_cast<int>(getU32(b, 0));
    const int tags = std::min(grTags(ver), static_cast<int>((b.size() - 4) / 4));
    for (int t = 0; t < tags; ++t) {
      const ParamDesc* d = desc("gl.grainscan", t);
      const int n = choiceCount(d);
      f.push_back(i32v(t, 4 + 4 * static_cast<size_t>(t), n ? 0 : d ? d->min : 0, n ? n - 1 : d ? d->max : 1));
    }
    if (ver >= 7 && b.size() >= kGrEnd) {
      const size_t r = kGrEnv + kGrVolEnv * kEnvRec;
      f.push_back(i32v(mt::GsEnvOn, r + 4, 0, 1));
      f.push_back(field(mt::GsEnvAttack, r + 12, Type::I32, 100, 65536));
      f.push_back(field(mt::GsEnvHold, r + 16, Type::I32, 100, 65536));
      f.push_back(field(mt::GsEnvDecay, r + 20, Type::I32, 100, 65536));
      f.push_back(field(mt::GsEnvSustain, r + 24, Type::I32, 0, 128));
      f.push_back(field(mt::GsEnvRelease, r + 28, Type::I32, 100, 65536));
    }
    return f;
  };
  e.defaults = [f = e.fields] {
    Bytes b(kGrEnd, 0);
    putI32(b, 0, kGrVersion);
    for (size_t k = 0; k < kEnvRecs; ++k) putEnvRecord(b, kGrEnv + k * kEnvRec);
    for (size_t k = 0; k < 6; ++k) putI32(b, kGrEnv + kEnvRecs * kEnvRec + 4 * k, kGrLevels[k]);
    storeDefaults(b, "gl.grainscan", f(b));
    return b;
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    // Older versions hold fewer tags and no envelope: upgraded to version 7
    // when the tempo lock or the envelope is wanted.
    if (b.size() < 8 || static_cast<int>(getU32(b, 0)) >= kGrVersion) return;
    PluginState cur;
    if (!decodeNow("Fruity Granulizer", b, c, cur)) return;
    bool need = false;
    for (int k = mt::GsTempoLock; k <= mt::GsEnvRelease; ++k) need |= param(cur, k) != param(g, k);
    if (!need) return;
    const int tags = std::min(grTags(static_cast<int>(getU32(b, 0))), static_cast<int>((b.size() - 4) / 4));
    Bytes n(kGrEnd, 0);
    putI32(n, 0, kGrVersion);
    std::memcpy(n.data() + 4, b.data() + 4, 4 * static_cast<size_t>(tags));
    for (size_t k = 0; k < kEnvRecs; ++k) putEnvRecord(n, kGrEnv + k * kEnvRec);
    for (size_t k = 0; k < 6; ++k) putI32(n, kGrEnv + kEnvRecs * kEnvRec + 4 * k, kGrLevels[k]);
    b = std::move(n);
  };
  return e;
}

// ------------------------------------------------------------ Wave Traveller
// i32 1000, i32 12 + path length, i32 1000005, i32 current key, i32 n + path,
// one patch record per key (i32 2000, i32 size without the point bodies, i32
// key, f32 beats / speed / attack / release / start / end, i32 flags, i32 n +
// name, i32 n + n × {i32 on, f32 x}, position then volume points: i32 n + n ×
// 60 bytes), then {i32 3000, i32 0}. Rebuilt from Gridline's text form.
constexpr int32_t kSsHead = 1000, kSsMagic = 1000005, kSsPatchTag = 2000, kSsEndTag = 3000;
constexpr size_t kSsPoint = 60;
constexpr int kSsPointBodyCounted = 8;  // the size word counts 8 bytes of each 60-byte point
constexpr float kSsHandle = 0.042f;     // Bezier handle offset FL stores for a fresh point
// Stored segment type of each Gridline curve (SsCurve order).
constexpr int32_t kSsCurveCode[mt::SsCurveCount] = {2, 1, 3, 4, 5, 6};
void ssPoints(ByteWriter& w, const std::vector<mt::SsPoint>& v) {
  w.i32(static_cast<int32_t>(v.size()));
  for (const mt::SsPoint& p : v) {
    w.i32(0);  // runtime pointer
    w.f32(0);
    w.f32(p.x);
    w.f32(p.y);
    for (float h : {0.0f, -kSsHandle, 0.0f, 0.0f, kSsHandle, 0.0f}) w.f32(h);
    w.f32(p.tension);
    w.i32(0);
    w.i32(kSsCurveCode[std::min<int>(p.curve, mt::SsCurveCount - 1)]);
    w.i32(0);
    w.i32(p.locked ? 1 : 0);
  }
}
Bytes ssSerialize(const mt::SsData& d, const EncodeContext& c) {
  ByteWriter w;
  const std::string path = toLatin1(flPath(d.path, c));
  w.i32(kSsHead);
  w.i32(static_cast<int32_t>(12 + path.size()));
  w.i32(kSsMagic);
  w.i32(d.current);
  w.i32(static_cast<int32_t>(path.size()));
  w.bytes(path.data(), path.size());
  for (int k = 0; k < mt::kSsKeys; ++k) {
    const mt::SsPatch& p = d.keys[static_cast<size_t>(k)].used ? d.keys[static_cast<size_t>(k)] : mt::ssDefaultPatch();
    const size_t start = w.size();
    w.i32(kSsPatchTag);
    w.i32(0);  // size (below)
    w.i32(k);
    for (float f : {p.beats, p.speed, p.attack, p.release, p.start, p.end}) w.f32(f);
    w.i32(0);  // flags
    const std::string name = toLatin1(p.name);
    w.i32(static_cast<int32_t>(name.size()));
    w.bytes(name.data(), name.size());
    w.i32(static_cast<int32_t>(p.mutes.size()));
    for (const auto& m : p.mutes) w.i32(m.first ? 1 : 0), w.f32(m.second);
    ssPoints(w, p.pos);
    ssPoints(w, p.vol);
    const size_t pts = p.pos.size() + p.vol.size();
    putU32(w.b, start + 4, static_cast<uint32_t>(w.size() - start - 8 - pts * (kSsPoint - kSsPointBodyCounted)));
  }
  w.i32(kSsEndTag);
  w.i32(0);
  return w.b;
}
Encoder waveTraveller() {
  Encoder e = base("Wave Traveller", "gl.splinescratch", 0);
  e.defaults = [] {
    mt::SsData d;
    for (auto& k : d.keys) k = mt::ssDefaultPatch();
    return ssSerialize(d, EncodeContext{});
  };
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    mt::SsData d;
    if ((decodeNow("Wave Traveller", b, c, cur) && cur.data == g.data) || !mt::parseScratchSpl(g.data, d)) return;
    b = ssSerialize(d, c);
  };
  return e;
}

// ------------------------------------------------------------ Morphine
// "SSLF", u32 size, "Mor0", "MPR0" (ids stored reversed), four programs
// {MVC0 {MZH0 {i32 zones, per zone: i32 key, f32 gain, MSP0 {i32 frames, per
// frame: i32 hold, f32 level, MBP0 {u8 mask + f32 per set bit, 8 harmonics
// per group}}, 1060-byte loop record}}, 28-byte program record}, then the
// global blocks. Programs are rebuilt from Gridline's text form; the program /
// loop records and the global blocks (not read by the decoder) are kept from
// the state written over. No default state is built (the global blocks'
// layout is not known): a native instance is written without a state.
uint32_t revTag(const char* s) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[3])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24);
}
constexpr size_t kMoPrograms = 16, kMoLoopRec = 1060;
constexpr int32_t kMoEmptyKey = 128;  // key of the placeholder zone of an empty corner
constexpr float kMoLoopNeutral = 0.5f, kMoHarmonicGain = 1.0f;
constexpr int32_t kMoLoopKind = 3;
struct MoBlocks {
  bool ok = false;
  size_t end = 0;                                          // end of the four programs
  std::vector<Bytes> progRec;                              // each program's record
  std::vector<std::vector<Bytes>> loopRec;                 // each zone's loop record
};
MoBlocks moBlocks(const Bytes& b) {
  MoBlocks M;
  size_t at = kMoPrograms;
  auto blk = [&](size_t& end) {
    if (at + 8 > b.size()) return false;
    const uint32_t sz = getU32(b, at);
    if (sz < 4 || sz > b.size() - at - 4) return false;
    end = at + 4 + sz;
    at += 8;
    return true;
  };
  for (int p = 0; p < mt::kSmPrograms; ++p) {
    size_t vEnd = 0, zEnd = 0;
    if (!blk(vEnd) || !blk(zEnd)) return M;
    const uint32_t zones = getU32(b, at);
    at += 4;
    std::vector<Bytes> loops;
    for (uint32_t z = 0; z < zones && z < 64; ++z) {
      at += 8;
      size_t sEnd = 0;
      if (!blk(sEnd) || sEnd < kMoLoopRec) return M;
      loops.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(sEnd - kMoLoopRec), b.begin() + static_cast<std::ptrdiff_t>(sEnd));
      at = sEnd;
    }
    M.loopRec.push_back(std::move(loops));
    M.progRec.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(zEnd), b.begin() + static_cast<std::ptrdiff_t>(vEnd));
    at = vEnd;
  }
  M.end = at;
  M.ok = true;
  return M;
}
Bytes moLoopRecord(int loopStart, int loopEnd) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(kMoLoopRec));
  w.i32(loopStart);
  w.i32(loopEnd);
  w.i32(0);
  w.i32(kMoLoopKind);
  for (int k = 0; k < 3; ++k) w.f32(kMoLoopNeutral);
  w.i32(0);
  for (int k = 0; k < 256; ++k) w.f32(kMoHarmonicGain);
  return w.b;
}
// Sized block: u32 size (from the tag), tag, content.
void moBlock(ByteWriter& w, const char* tag, const Bytes& content) {
  w.u32(static_cast<uint32_t>(content.size() + 4));
  w.u32(revTag(tag));
  w.bytes(content);
}
Bytes moZone(const mt::SmZone& z, const Bytes* loopTemplate) {
  ByteWriter sp;
  sp.i32(static_cast<int32_t>(z.frames.size()));
  for (const mt::SmFrame& f : z.frames) {
    sp.i32(f.hold);
    sp.f32(f.level);
    ByteWriter bp;
    for (size_t g = 0; g < f.amp.size(); g += 8) {
      uint8_t mask = 0;
      for (size_t k = 0; k < 8 && g + k < f.amp.size(); ++k)
        if (f.amp[g + k] != 0.0f) mask = static_cast<uint8_t>(mask | (1u << k));
      bp.u8(mask);
      for (size_t k = 0; k < 8 && g + k < f.amp.size(); ++k)
        if (f.amp[g + k] != 0.0f) bp.f32(f.amp[g + k]);
    }
    moBlock(sp, "MBP0", bp.b);
  }
  Bytes loop = loopTemplate && loopTemplate->size() == kMoLoopRec ? *loopTemplate : moLoopRecord(z.loopStart, z.loopEnd);
  putI32(loop, 4, z.loopStart);
  putI32(loop, 8, z.loopEnd);
  sp.bytes(loop);
  ByteWriter zw;
  zw.i32(z.key);
  zw.f32(z.gain);
  moBlock(zw, "MSP0", sp.b);
  return zw.b;
}
Encoder morphine() {
  Encoder e = base("Morphine", "gl.spectramorph", 0);
  e.write = [](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    mt::SmData d;
    if ((decodeNow("Morphine", b, c, cur) && cur.data == g.data) || !mt::parseMorph(g.data, d)) return;
    const MoBlocks M = moBlocks(b);
    if (!M.ok) return;
    ByteWriter w;
    w.bytes(b.data(), kMoPrograms);  // "SSLF", size, "Mor0", "MPR0"
    for (int p = 0; p < mt::kSmPrograms; ++p) {
      const auto& zones = d.prog[static_cast<size_t>(p)].zones;
      ByteWriter zh;
      if (zones.empty()) {  // an empty corner: one silent placeholder zone
        mt::SmZone z;
        z.key = kMoEmptyKey;
        z.frames.resize(1);
        zh.i32(1);
        zh.bytes(moZone(z, nullptr));
      } else {
        zh.i32(static_cast<int32_t>(zones.size()));
        for (size_t k = 0; k < zones.size(); ++k)
          zh.bytes(moZone(zones[k], k < M.loopRec[static_cast<size_t>(p)].size() ? &M.loopRec[static_cast<size_t>(p)][k] : nullptr));
      }
      ByteWriter vc;
      moBlock(vc, "MZH0", zh.b);
      vc.bytes(M.progRec[static_cast<size_t>(p)]);
      moBlock(w, "MVC0", vc.b);
    }
    w.bytes(b.data() + M.end, b.size() - M.end);  // global blocks
    putU32(w.b, 4, static_cast<uint32_t>(w.size() - 8));
    b = std::move(w.b);
  };
  return e;
}

// ------------------------------------------------------------ Ogun / Autogun
// u32 version (19 << 16 | 3), zlib stream. Inflated: i32 tags 0..2, the record
// from tag 3 (tags 3..48, then the 60-byte option block), 3 option bytes
// (delay tempo, delay on, -), 3 × i32 EQ band types, the patch notes (u8 length
// or 0xFF + i32, UTF-16), (Autogun, version 19: one word), 13 articulators
// {i32 3, f32 × 3, envelope curve, 4 knobs, LFO curve, 4 knobs, u32 LFO phase,
// key / velocity / X / Y map curves}, 4 timbre EQ curves. Curves: i32 kind, i32
// format (3: f64 x / y), i32 n, n × {f64 x, f64 y, f32 tension, u8 type, 3
// bytes}, i32 flags (1 tempo, 2 global), i32 enabled (−1 / 0), decay, loop
// start, sustain (−1 none). Everything sits in the compressed stream, so the
// whole state is rebuilt from the Gridline parameters and text form.
constexpr uint32_t kAlVersion = (19u << 16) | 3u;
constexpr int32_t kAlArtVersion = 3, kAlCurveFormat = 3, kAlEnvKind = 1, kAlLfoKind = 2, kAlMapKind = 3;
constexpr float kAlArtHead[3] = {0.0f, 1.0f, 1.0f};  // articulator header floats (constant in FL's content)
constexpr int32_t kAlTrue = -1;                     // stored "enabled"
constexpr size_t kAlRecord = 0xF4, kAlOptAt = 0x200 - 0x148, kAlOptLen = 0x3C;
struct AlloyModel {
  int32_t params[49] = {};
  uint8_t opt[64] = {};
  uint8_t extra3[3] = {};
  int32_t eqTypes[3] = {0, 1, 2};
  mt::AlloyData data;
  int32_t knobs[mt::AaCount][8] = {};
  uint32_t phase[mt::AaCount] = {};
  bool autogunWord = false;
};
int32_t optI(const uint8_t* o, int at) {
  return static_cast<int32_t>(static_cast<uint32_t>(o[at]) | (static_cast<uint32_t>(o[at + 1]) << 8) | (static_cast<uint32_t>(o[at + 2]) << 16) |
                              (static_cast<uint32_t>(o[at + 3]) << 24));
}
void setOptI(uint8_t* o, int at, int32_t v) {
  for (int k = 0; k < 4; ++k) o[at + k] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * k));
}
void alCurve(ByteWriter& w, int32_t kind, const hm::Curve& c) {
  w.i32(kind);
  w.i32(kAlCurveFormat);
  w.i32(static_cast<int32_t>(c.pts.size()));
  for (const hm::Pt& p : c.pts) {
    w.f64(p.dt);
    w.f64(p.y);
    w.f32(p.tension);
    w.u8(p.kind);
    w.zeros(3);
  }
  w.i32((c.tempo ? 1 : 0) | (c.global ? 2 : 0));
  w.i32(c.enabled ? kAlTrue : 0);
  w.i32(c.decay);
  w.i32(c.loopStart);
  w.i32(c.sustain);
}
Bytes alSerialize(const AlloyModel& M, uint32_t version) {
  ByteWriter w;
  for (int t = 0; t < 3; ++t) w.i32(M.params[t]);
  uint8_t rec[kAlRecord] = {};
  for (int t = 3; t < 49; ++t) setOptI(rec, (t - 3) * 4, M.params[t]);
  std::memcpy(rec + kAlOptAt, M.opt, kAlOptLen);
  w.bytes(rec, kAlRecord);
  w.bytes(M.extra3, 3);
  for (int32_t t : M.eqTypes) w.i32(t);
  // Notes: UTF-8 → UTF-16 code units.
  std::vector<uint16_t> u;
  const std::string& s = M.data.info;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    if (c >= 0xE0 && i + 2 < s.size()) cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu), i += 2;
    else if (c >= 0xC0 && i + 1 < s.size()) cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu), i += 1;
    u.push_back(static_cast<uint16_t>(cp));
  }
  if (u.size() < 0xFF) w.u8(static_cast<uint32_t>(u.size()));
  else w.u8(0xFF), w.i32(static_cast<int32_t>(u.size()));
  for (uint16_t c : u) w.u16(c);
  if (M.autogunWord) w.i32(0);  // the preset player's extra word (version 19)
  for (int a = 0; a < mt::AaCount; ++a) {
    const mt::AlloyArtData& A = M.data.arts[static_cast<size_t>(a)];
    w.i32(kAlArtVersion);
    for (float f : kAlArtHead) w.f32(f);
    alCurve(w, kAlEnvKind, A.env);
    for (int k = 0; k < 4; ++k) w.i32(M.knobs[a][k]);
    alCurve(w, kAlLfoKind, A.lfo);
    for (int k = 4; k < 8; ++k) w.i32(M.knobs[a][k]);
    w.u32(M.phase[a]);
    for (const hm::Curve& m : A.maps) alCurve(w, kAlMapKind, m);
  }
  for (const hm::Curve& c : M.data.eq) alCurve(w, kAlMapKind, c);
  ByteWriter out;
  out.u32(version);
  out.bytes(png::deflateZlib(w.b.data(), w.b.size()));
  return out.b;
}
std::string g6(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  return buf;
}
// Articulator knobs reproducing the text form's scale values (the decoder's
// value of a knob printed as the text form prints it).
void alKnobsFromData(const mt::AlloyArtData& A, int32_t k[8], uint32_t& phase) {
  auto scale = [](float v) { return static_cast<int32_t>(std::lround(v * 128.0)); };
  k[0] = scale(A.att), k[1] = scale(A.dec), k[2] = scale(A.sus), k[3] = scale(A.rel);
  k[5] = scale(A.lfoTension), k[6] = scale(A.lfoSkew), k[7] = scale(A.lfoPw);
  auto speed = [](int32_t x) {
    return static_cast<float>(std::clamp((std::pow(10001.0, std::clamp(x, 0, 65536) / 65536.0) - 1.0) / 625.0, 0.0, 200.0));
  };
  const auto k0 = static_cast<int32_t>(std::lround(std::log(static_cast<double>(A.lfoSpeed) * 625.0 + 1.0) / std::log(10001.0) * 65536.0));
  k[4] = std::clamp(k0, 0, 65536);
  const std::string want = g6(A.lfoSpeed);
  for (int32_t d = 0; d <= 8; ++d) {
    if (g6(speed(k0 - d)) == want) { k[4] = std::clamp(k0 - d, 0, 65536); break; }
    if (g6(speed(k0 + d)) == want) { k[4] = std::clamp(k0 + d, 0, 65536); break; }
  }
  phase = static_cast<uint32_t>(std::clamp(std::llround(static_cast<double>(A.lfoPhase) * 4294967296.0), 0LL, 4294967295LL));
}
// Option block / extra bytes from the Gridline parameters (only where the
// decoder's reading differs, so unchanged bytes stay as they were).
void alApplyParams(const PluginState& g, AlloyModel& M) {
  auto P = [&g](int k) { return static_cast<int32_t>(std::lround(param(g, k))); };
  for (int t = 0; t < 49; ++t) {
    const ParamDesc* d = desc("gl.alloy", t);
    if (!d || static_cast<float>(d->clamp(M.params[t])) != param(g, t)) M.params[t] = P(t);
  }
  uint8_t* o = M.opt;
  auto i32opt = [&](int at, int k, int lo, int hi, int ofs) {
    if (std::clamp(optI(o, at) - ofs, lo, hi) != P(k)) setOptI(o, at, P(k) + ofs);
  };
  auto flag = [&](int at, int k) {
    if ((o[at] != 0) != (P(k) != 0)) o[at] = static_cast<uint8_t>(P(k) != 0);
  };
  i32opt(0x00, mt::AlFltType, 0, 3, 0);
  i32opt(0x04, mt::AlFltMode, 0, 4, 0);
  if ((optI(o, 0x08) != 0) != (P(mt::AlFltFlat) != 0)) setOptI(o, 0x08, P(mt::AlFltFlat) != 0);
  i32opt(0x0C, mt::AlRichness, 1, 11, 5);
  flag(0x10, mt::AlRvOn);
  flag(0x11, mt::AlRvTempo);
  i32opt(0x14, mt::AlQuality, 0, 4, 0);
  flag(0x18, mt::AlModSmooth);
  flag(0x19, mt::AlOneVoicePerKey);
  flag(0x1A, mt::AlVelVol);
  flag(0x1B, mt::AlKeyPitch);
  flag(0x1C, mt::AlKeySeed);
  o[0x1D] = static_cast<uint8_t>((o[0x1D] & ~3u) | (P(mt::AlVelAttTime) ? 1u : 0u) | (P(mt::AlVelAttLen) ? 2u : 0u));
  flag(0x1E, mt::AlDeclick);
  flag(0x1F, mt::AlRelLink);
  flag(0x20, mt::AlPorta);
  flag(0x21, mt::AlPortaVar);
  if (std::clamp(static_cast<int>(o[0x22]), 0, 5) != P(mt::AlPortaLimit)) o[0x22] = static_cast<uint8_t>(P(mt::AlPortaLimit));
  flag(0x23, mt::AlNatScale);
  {  // harmonic comb: the first non-zero of the four
    int comb = 0;
    for (int k = 0; k < 4 && comb == 0; ++k) comb = std::clamp(optI(o, 0x24 + 4 * k), 0, 7);
    if (comb != P(mt::AlComb))
      for (int k = 0; k < 4; ++k) setOptI(o, 0x24 + 4 * k, k == 0 ? P(mt::AlComb) : 0);
  }
  flag(0x34, mt::AlAltDecay);
  flag(0x35, mt::AlAutoLoud);
  flag(0x36, mt::AlNatWeight);
  i32opt(0x38, mt::AlDust, 0, 1000, 0);
  if ((M.extra3[0] != 0) != (P(mt::AlDlTempo) != 0)) M.extra3[0] = static_cast<uint8_t>(P(mt::AlDlTempo) != 0);
  if ((M.extra3[1] != 0) != (P(mt::AlDlOn) != 0)) M.extra3[1] = static_cast<uint8_t>(P(mt::AlDlOn) != 0);
  for (int k = 0; k < 3; ++k)
    if (std::clamp(M.eqTypes[k], 0, 2) != P(mt::AlEqType1 + k)) M.eqTypes[k] = P(mt::AlEqType1 + k);
}
void alApply(const PluginState& g, AlloyModel& M) {
  alApplyParams(g, M);
  mt::AlloyData d;
  if (mt::parseAlloy(g.data, d)) M.data = d;
  for (int a = 0; a < mt::AaCount; ++a) alKnobsFromData(M.data.arts[static_cast<size_t>(a)], M.knobs[a], M.phase[a]);
  for (int a = 0; a < mt::kAlloyEnvArts; ++a)  // the articulation targets' knobs are parameters
    for (int k = 0; k < mt::AkCount; ++k) M.knobs[a][k] = static_cast<int32_t>(std::lround(param(g, mt::alKnob(a, k))));
}
// Only tags / options differ (same text form, same articulator knobs): patch
// them inside the template's own record, whatever its version (older records
// leave out articulators a current one cannot express the way the decoder
// reads them).
bool alPatchInPlace(const PluginState& g, const PluginState& cur, const j22::AlloyDecoded& d, Bytes& b) {
  if (cur.data != g.data || b.size() < 8) return false;
  for (int k = mt::AlArtKnobs; k < mt::AlCount; ++k)
    if (param(cur, k) != param(g, k)) return false;
  const int ver = d.version;
  if (ver < 8 && param(g, mt::AlFine) != param(cur, mt::AlFine)) return false;  // no fine pitch before version 8
  std::vector<uint8_t> z;
  if (!gen::inflateZlib(gen::Bytes(b.data() + 4, b.size() - 4), z, 32u << 20)) return false;
  int len = static_cast<int>(kAlRecord);  // record length by version (as the decoder reads it)
  if (ver < 1) len -= 1;
  if (ver < 2) len -= 3;
  if (ver < 7) len -= 1;
  if (ver < 9) len -= 16;
  if (ver < 11) len -= 1;
  if (ver < 12) len -= 1;
  if (ver < 13) len -= 6;
  const size_t rec = ver < 8 ? 8 : 12, optLen = static_cast<size_t>(len) - kAlOptAt;
  if (rec + static_cast<size_t>(len) + 3 + 12 > z.size()) return false;
  AlloyModel M;
  std::copy(std::begin(d.params), std::end(d.params), M.params);
  std::memcpy(M.opt, d.opt, sizeof M.opt);
  std::memcpy(M.extra3, d.extra3, sizeof M.extra3);
  std::copy(std::begin(d.eqTypes), std::end(d.eqTypes), M.eqTypes);
  alApplyParams(g, M);
  for (int t = 0; t < 49; ++t) {
    if (t == 2 && ver < 8) continue;
    const size_t at = t < 3 ? 4 * static_cast<size_t>(t) : rec + 4 * static_cast<size_t>(t - 3);
    putI32(z, at, M.params[t]);
  }
  std::memcpy(z.data() + rec + kAlOptAt, M.opt, std::min(optLen, kAlOptLen));
  std::memcpy(z.data() + rec + static_cast<size_t>(len), M.extra3, 3);
  for (size_t k = 0; k < 3; ++k) putI32(z, rec + static_cast<size_t>(len) + 3 + 4 * k, M.eqTypes[k]);
  ByteWriter out;
  out.u32(getU32(b, 0));
  out.bytes(png::deflateZlib(z.data(), z.size()));
  b = std::move(out.b);
  return true;
}

Encoder alloy(const char* flName, int rank, bool autogun) {
  Encoder e = base(flName, "gl.alloy", rank);
  e.defaults = [autogun] {
    AlloyModel M;
    M.autogunWord = autogun;
    M.opt[0x21] = 1;  // the option block's fresh values (portamento: variable time) — the
    M.opt[0x38] = 0xAE, M.opt[0x39] = 0x01;  // decoder's defaults for old states; magic dust 430
    PluginState g = defaultPluginState("gl.alloy");
    g.data = mt::serializeAlloy(mt::AlloyData{});
    for (int t = 0; t < 49; ++t) M.params[t] = static_cast<int32_t>(std::lround(param(g, t)));
    alApply(g, M);
    return alSerialize(M, kAlVersion);
  };
  e.write = [flName = std::string(flName), autogun](const PluginState& g, Bytes& b, const EncodeContext& c) {
    PluginState cur;
    if (decodeNow(flName, b, c, cur) && cur.params == g.params && cur.data == g.data) return;
    AlloyModel M;
    M.autogunWord = autogun;
    j22::AlloyDecoded d;
    uint32_t version = kAlVersion;
    if (j22::decodeAlloyState(b.data(), b.size(), d, autogun) && alPatchInPlace(g, cur, d, b)) return;
    if (j22::decodeAlloyState(b.data(), b.size(), d, autogun)) {  // keep what the template holds
      std::copy(std::begin(d.params), std::end(d.params), M.params);
      std::memcpy(M.opt, d.opt, sizeof M.opt);
      std::memcpy(M.extra3, d.extra3, sizeof M.extra3);
      std::copy(std::begin(d.eqTypes), std::end(d.eqTypes), M.eqTypes);
      M.data = d.data;
      if (d.version >= 18) version = getU32(b, 0);
      M.autogunWord = autogun && d.version >= 19;
    }
    alApply(g, M);
    b = alSerialize(M, version);
  };
  return e;
}

}  // namespace

void registerJ22(std::vector<Encoder>& v) {
  v.push_back(fruitKick());
  v.push_back(bassDrum());
  v.push_back(drumpad());
  v.push_back(drumaxx());
  v.push_back(plucked());
  v.push_back(sakura());
  v.push_back(transistorBass());
  v.push_back(poiZone());
  v.push_back(sawer());
  v.push_back(booBass());
  v.push_back(dx10());
  v.push_back(simSynth());
  v.push_back(kepler());
  v.push_back(keplerExo());
  v.push_back(granulizer());
  v.push_back(waveTraveller());
  v.push_back(morphine());
  v.push_back(alloy("Ogun", 0, false));
  v.push_back(alloy("Autogun", -1, true));
}

}  // namespace gl::flp::enc
