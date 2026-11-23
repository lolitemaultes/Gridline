// Gridline tests: a small SoundFont writer. Every bank the tests read is made here, in the test,
// from samples computed in the test (nothing is shipped from elsewhere).
//   sf2t::Bank b; b.samples.push_back(...); b.insts.push_back(...); b.presets.push_back(...);
//   std::vector<uint8_t> bytes = b.build();
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace sf2t {

struct Bytes {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(static_cast<uint8_t>(v & 0xFF)), u8(static_cast<uint8_t>(v >> 8)); }
  void u32(uint32_t v) { u16(static_cast<uint16_t>(v & 0xFFFF)), u16(static_cast<uint16_t>(v >> 16)); }
  void id(const char* s) { b.insert(b.end(), s, s + 4); }
  void name(const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) u8(i < s.size() && i + 1 < n ? static_cast<uint8_t>(s[i]) : 0);
  }
  void append(const Bytes& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};
inline Bytes chunk(const char* id, const Bytes& body) {
  Bytes c;
  c.id(id);
  c.u32(static_cast<uint32_t>(body.b.size()));
  c.append(body);
  if (body.b.size() & 1) c.u8(0);
  return c;
}
inline Bytes list(const char* type, const std::vector<Bytes>& kids) {
  Bytes body;
  body.id(type);
  for (const auto& k : kids) body.append(k);
  return chunk("LIST", body);
}

struct Mod {
  uint16_t src = 0, dest = 0;
  int16_t amount = 0;
  uint16_t amtSrc = 0, trans = 0;
};
struct Zone {
  std::vector<std::pair<uint16_t, uint16_t>> gens;  // operator, raw amount
  std::vector<Mod> mods;
  Zone& g(uint16_t op, int amt) {
    gens.emplace_back(op, static_cast<uint16_t>(static_cast<int16_t>(amt)));
    return *this;
  }
  Zone& range(uint16_t op, int lo, int hi) {
    gens.emplace_back(op, static_cast<uint16_t>((lo & 0xFF) | ((hi & 0xFF) << 8)));
    return *this;
  }
  Zone& mod(uint16_t src, uint16_t dest, int amount, uint16_t amtSrc = 0, uint16_t trans = 0) {
    mods.push_back({src, dest, static_cast<int16_t>(amount), amtSrc, trans});
    return *this;
  }
};
struct Inst {
  std::string name;
  std::vector<Zone> zones;  // a first zone without sampleID (53) is the global zone
};
struct Preset {
  std::string name;
  uint16_t program = 0, bank = 0;
  std::vector<Zone> zones;  // a first zone without instrument (41) is the global zone
};
struct Sample {
  std::string name;
  std::vector<int16_t> pcm;    // 16-bit frames (SF2)
  std::vector<uint8_t> low24;  // optional low bytes (sm24); same length as pcm
  std::vector<uint8_t> ogg;    // SF3: Vorbis stream (pcm then only gives the frame count)
  uint32_t rate = 44100;
  uint8_t pitch = 60;
  int8_t correction = 0;
  uint32_t loopStart = 0, loopEnd = 0;  // frames, relative to the sample's start
  uint16_t type = 1, link = 0;          // 1 mono, 2 right, 4 left
};

struct Bank {
  std::string name = "Test Bank";
  std::vector<Sample> samples;
  std::vector<Inst> insts;
  std::vector<Preset> presets;
  bool sm24 = false;
  bool sf3 = false;

  std::vector<uint8_t> build() const {
    Bytes smpl, sm24c, shdr;
    for (const auto& s : samples) {
      uint32_t start = 0, end = 0;
      if (sf3) {
        start = static_cast<uint32_t>(smpl.b.size());
        smpl.b.insert(smpl.b.end(), s.ogg.begin(), s.ogg.end());
        end = static_cast<uint32_t>(smpl.b.size());
        shdr.name(s.name, 20), shdr.u32(start), shdr.u32(end), shdr.u32(s.loopStart), shdr.u32(s.loopEnd);
      } else {
        start = static_cast<uint32_t>(smpl.b.size() / 2);
        for (int16_t v : s.pcm) smpl.u16(static_cast<uint16_t>(v));
        for (size_t i = 0; i < s.pcm.size(); ++i) sm24c.u8(i < s.low24.size() ? s.low24[i] : 0);
        end = static_cast<uint32_t>(smpl.b.size() / 2);
        for (int i = 0; i < 46; ++i) smpl.u16(0), sm24c.u8(0);  // the spec's 46 zero frames after each sample
        shdr.name(s.name, 20), shdr.u32(start), shdr.u32(end), shdr.u32(start + s.loopStart), shdr.u32(start + s.loopEnd);
      }
      shdr.u32(s.rate), shdr.u8(s.pitch), shdr.u8(static_cast<uint8_t>(s.correction)), shdr.u16(s.link),
          shdr.u16(static_cast<uint16_t>(s.type | (sf3 ? 0x10 : 0)));
    }
    shdr.name("EOS", 20);
    for (int i = 0; i < 26; ++i) shdr.u8(0);
    if (sm24c.b.size() & 1) sm24c.u8(0);
    // Instruments.
    Bytes inst, ibag, imod, igen;
    uint16_t bagN = 0, genN = 0, modN = 0;
    auto zones = [&](const std::vector<Zone>& zs, Bytes& bag, Bytes& gen, Bytes& mod) {
      for (const auto& z : zs) {
        bag.u16(genN), bag.u16(modN);
        ++bagN;
        // Key and velocity ranges must come first.
        std::vector<std::pair<uint16_t, uint16_t>> ordered;
        for (const auto& g : z.gens)
          if (g.first == 43) ordered.push_back(g);
        for (const auto& g : z.gens)
          if (g.first == 44) ordered.push_back(g);
        for (const auto& g : z.gens)
          if (g.first != 43 && g.first != 44 && g.first != 41 && g.first != 53) ordered.push_back(g);
        for (const auto& g : z.gens)
          if (g.first == 41 || g.first == 53) ordered.push_back(g);
        for (const auto& g : ordered) gen.u16(g.first), gen.u16(g.second), ++genN;
        for (const auto& m : z.mods) mod.u16(m.src), mod.u16(m.dest), mod.u16(static_cast<uint16_t>(m.amount)), mod.u16(m.amtSrc), mod.u16(m.trans), ++modN;
      }
    };
    for (const auto& in : insts) {
      inst.name(in.name, 20), inst.u16(bagN);
      zones(in.zones, ibag, igen, imod);
    }
    inst.name("EOI", 20), inst.u16(bagN);
    ibag.u16(genN), ibag.u16(modN);
    igen.u32(0);
    for (int i = 0; i < 10; ++i) imod.u8(0);
    Bytes phdr, pbag, pmod, pgen;
    bagN = genN = modN = 0;
    for (const auto& p : presets) {
      phdr.name(p.name, 20), phdr.u16(p.program), phdr.u16(p.bank), phdr.u16(bagN), phdr.u32(0), phdr.u32(0), phdr.u32(0);
      zones(p.zones, pbag, pgen, pmod);
    }
    phdr.name("EOP", 20), phdr.u16(0), phdr.u16(0), phdr.u16(bagN), phdr.u32(0), phdr.u32(0), phdr.u32(0);
    pbag.u16(genN), pbag.u16(modN);
    pgen.u32(0);
    for (int i = 0; i < 10; ++i) pmod.u8(0);
    Bytes ifil, inam, isng;
    ifil.u16(sf3 ? 3 : 2), ifil.u16(sf3 ? 1 : 4);
    inam.name(name, name.size() + 2);
    isng.name("EMU8000", 8);
    Bytes body;
    body.id("sfbk");
    body.append(list("INFO", {chunk("ifil", ifil), chunk("isng", isng), chunk("INAM", inam)}));
    std::vector<Bytes> sd = {chunk("smpl", smpl)};
    if (sm24 && !sf3) sd.push_back(chunk("sm24", sm24c));
    body.append(list("sdta", sd));
    body.append(list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen), chunk("inst", inst),
                              chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)}));
    return chunk("RIFF", body).b;
  }
};

inline bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream o(path, std::ios::binary);
  o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(o);
}

// A sine of `frames` frames, `cycles` cycles long (loops cleanly when cycles is whole).
inline std::vector<int16_t> sine(size_t frames, double cycles, double amp = 16000) {
  std::vector<int16_t> v(frames);
  for (size_t i = 0; i < frames; ++i) v[i] = static_cast<int16_t>(std::lround(amp * std::sin(6.283185307179586 * cycles * static_cast<double>(i) / static_cast<double>(frames))));
  return v;
}
// Constant level (a DC sample: envelopes and gains read straight off the output).
inline std::vector<int16_t> dc(size_t frames, int16_t v = 16384) { return std::vector<int16_t>(frames, v); }

// SF2 operator numbers the tests use.
enum Op : uint16_t {
  StartOfs = 0, EndOfs = 1, LoopStartOfs = 2, LoopEndOfs = 3, StartCoarse = 4, ModLfoToPitch = 5, VibLfoToPitch = 6, ModEnvToPitch = 7,
  FilterFc = 8, FilterQ = 9, ModLfoToFc = 10, ModEnvToFc = 11, EndCoarse = 12, ModLfoToVol = 13, Chorus = 15, Reverb = 16, Pan = 17,
  DelayModLfo = 21, FreqModLfo = 22, DelayVibLfo = 23, FreqVibLfo = 24, DelayModEnv = 25, AttackModEnv = 26, HoldModEnv = 27,
  DecayModEnv = 28, SustainModEnv = 29, ReleaseModEnv = 30, KeyToModEnvHold = 31, KeyToModEnvDecay = 32, DelayVolEnv = 33,
  AttackVolEnv = 34, HoldVolEnv = 35, DecayVolEnv = 36, SustainVolEnv = 37, ReleaseVolEnv = 38, KeyToVolEnvHold = 39,
  KeyToVolEnvDecay = 40, Instrument = 41, KeyRange = 43, VelRange = 44, LoopStartCoarse = 45, Keynum = 46, Velocity = 47,
  Attenuation = 48, LoopEndCoarse = 50, CoarseTune = 51, FineTune = 52, SampleId = 53, SampleModes = 54, ScaleTuning = 56,
  Exclusive = 57, RootKey = 58
};

}  // namespace sf2t
