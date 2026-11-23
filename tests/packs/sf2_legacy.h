// Gridline tests: the Multisampler's SoundFont reader as it was before the shared reader (0.7.7),
// kept as the reference the shared reader's zones and samples are compared with.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "engine/Sample.h"
#include "packs/samplers/samplers.h"

namespace sf2legacy {
using gl::Sample;
using gl::SamplePtr;
using gl::packs::samplers::Zone;
using gl::packs::samplers::LoopForward;
using gl::packs::samplers::LoopSustain;
using gl::packs::samplers::LoopBounce;
using gl::packs::samplers::kMaxGroups;

struct Sf2Preset {
  std::string name;
  int bank = 0, program = 0;
  uint32_t bagStart = 0, bagEnd = 0;
};
struct Sf2SampleHeader {
  std::string name;
  uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0, rate = 44100;
  int originalPitch = 60, correction = 0;
  uint16_t type = 1;
};
struct Sf2File {
  std::string name;
  std::vector<Sf2Preset> presets;  // sorted by bank, program
  // pdta records (generators as {oper, amount}).
  struct Bag { uint16_t gen = 0; };
  struct Gen { uint16_t oper = 0; int16_t amount = 0; uint8_t lo = 0, hi = 0; };
  struct Inst { std::string name; uint16_t bag = 0; };
  std::vector<Bag> pbag, ibag;
  std::vector<Gen> pgen, igen;
  std::vector<Inst> inst;
  std::vector<Sf2SampleHeader> samples;
  std::vector<int16_t> data;  // smpl chunk
};
namespace {
void clampZone(Zone& z) {
  z.lo = std::clamp(z.lo, 0, 127);
  z.hi = std::clamp(z.hi, z.lo, 127);
  z.vlo = std::clamp(z.vlo, 0, 127);
  z.vhi = std::clamp(z.vhi, z.vlo, 127);
  z.root = std::clamp(z.root, 0, 127);
  z.loop = std::clamp(z.loop, 0, static_cast<int>(LoopBounce));
  z.tune = std::clamp(z.tune, -4800.f, 4800.f);
  z.keyTrack = std::clamp(z.keyTrack, 0.f, 2.f);
  z.gain = std::clamp(z.gain, -96.f, 24.f);
  z.pan = std::clamp(z.pan, -1.f, 1.f);
  z.start = std::max<int64_t>(0, z.start);
  z.end = std::max<int64_t>(0, z.end);
  z.loopStart = std::max<int64_t>(0, z.loopStart);
  z.loopEnd = std::max<int64_t>(0, z.loopEnd);
  z.xfade = std::max<int64_t>(0, z.xfade);
  z.group = std::clamp(z.group, 0, kMaxGroups - 1);
  z.choke = std::clamp(z.choke, 0, 127);
  z.cutoff = std::clamp(z.cutoff, 0.f, 20000.f);
  z.attack = std::clamp(z.attack, 0.0005f, 20.f);
  z.hold = std::clamp(z.hold, 0.f, 20.f);
  z.decay = std::clamp(z.decay, 0.001f, 60.f);
  z.sustain = std::clamp(z.sustain, 0.f, 1.f);
  z.release = std::clamp(z.release, 0.001f, 60.f);
}
}  // namespace

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
std::string rdName(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  size_t z = s.find('\0');
  if (z != std::string::npos) s.resize(z);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// SF2 generator operators used here.
enum Gen {
  gStartOfs = 0, gEndOfs = 1, gLoopStartOfs = 2, gLoopEndOfs = 3, gStartCoarse = 4, gFilterFc = 8, gEndCoarse = 12,
  gPan = 17, gDelayVol = 33, gAttackVol = 34, gHoldVol = 35, gDecayVol = 36, gSustainVol = 37, gReleaseVol = 38,
  gInstrument = 41, gKeyRange = 43, gVelRange = 44, gLoopStartCoarse = 45, gAttenuation = 48, gLoopEndCoarse = 50,
  gCoarseTune = 51, gFineTune = 52, gSampleId = 53, gSampleModes = 54, gScaleTuning = 56, gExclusive = 57,
  gRootKey = 58, gCount = 61
};

struct GenSet {
  std::array<int, gCount> v{};
  std::array<bool, gCount> set{};
  int klo = 0, khi = 127, vlo = 0, vhi = 127;
  void defaults() {
    v.fill(0);
    set.fill(false);
    v[gFilterFc] = 13500;
    for (int g : {21, 23, 25, 26, 27, 28, 30, 33, 34, 35, 36, 38}) v[static_cast<size_t>(g)] = -12000;
    v[gScaleTuning] = 100;
    v[gRootKey] = -1;
    klo = vlo = 0;
    khi = vhi = 127;
  }
  void apply(const Sf2File::Gen& g) {
    if (g.oper >= gCount) return;
    if (g.oper == gKeyRange) {
      klo = g.lo, khi = g.hi;
    } else if (g.oper == gVelRange) {
      vlo = g.lo, vhi = g.hi;
    } else {
      v[g.oper] = g.amount;
    }
    set[g.oper] = true;
  }
};

double tc2s(int tc) { return std::pow(2.0, std::clamp(tc, -12000, 8000) / 1200.0); }

}  // namespace

std::shared_ptr<const Sf2File> parseSf2(const uint8_t* b, size_t n, std::string* err) {
  auto fail = [&](const char* m) -> std::shared_ptr<const Sf2File> {
    if (err) *err = m;
    return nullptr;
  };
  if (n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "sfbk", 4) != 0) return fail("not a SoundFont 2 file");
  auto f = std::make_shared<Sf2File>();
  size_t riffEnd = std::min(n, static_cast<size_t>(rd32(b + 4)) + 8);
  const uint8_t *phdr = nullptr, *pbag = nullptr, *pgen = nullptr, *inst = nullptr, *ibag = nullptr, *igen = nullptr, *shdr = nullptr;
  size_t nPhdr = 0, nPbag = 0, nPgen = 0, nInst = 0, nIbag = 0, nIgen = 0, nShdr = 0;
  const uint8_t* smpl = nullptr;
  size_t smplBytes = 0;
  size_t pos = 12;
  while (pos + 8 <= riffEnd) {
    const uint8_t* ck = b + pos;
    size_t sz = rd32(ck + 4);
    if (pos + 8 + sz > riffEnd) sz = riffEnd - pos - 8;
    if (std::memcmp(ck, "LIST", 4) == 0 && sz >= 4) {
      size_t lp = pos + 12, lend = pos + 8 + sz;
      while (lp + 8 <= lend) {
        const uint8_t* sc = b + lp;
        size_t ssz = rd32(sc + 4);
        if (lp + 8 + ssz > lend) ssz = lend - lp - 8;
        const uint8_t* d = sc + 8;
        auto is = [&](const char* id) { return std::memcmp(sc, id, 4) == 0; };
        if (is("INAM")) f->name = rdName(d, ssz);
        else if (is("smpl")) smpl = d, smplBytes = ssz;
        else if (is("phdr")) phdr = d, nPhdr = ssz / 38;
        else if (is("pbag")) pbag = d, nPbag = ssz / 4;
        else if (is("pgen")) pgen = d, nPgen = ssz / 4;
        else if (is("inst")) inst = d, nInst = ssz / 22;
        else if (is("ibag")) ibag = d, nIbag = ssz / 4;
        else if (is("igen")) igen = d, nIgen = ssz / 4;
        else if (is("shdr")) shdr = d, nShdr = ssz / 46;
        lp += 8 + ssz + (ssz & 1);
      }
    }
    pos += 8 + sz + (sz & 1);
  }
  if (!phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr || nPhdr < 2 || nInst < 2 || nShdr < 1)
    return fail("SoundFont is missing preset data");
  if (!smpl) return fail("SoundFont has no sample data");
  f->data.resize(smplBytes / 2);
  for (size_t i = 0; i < f->data.size(); ++i) f->data[i] = static_cast<int16_t>(rd16(smpl + i * 2));
  auto readGens = [](const uint8_t* p, size_t cnt, std::vector<Sf2File::Gen>& out) {
    out.resize(cnt);
    for (size_t i = 0; i < cnt; ++i) {
      out[i].oper = rd16(p + i * 4);
      out[i].amount = static_cast<int16_t>(rd16(p + i * 4 + 2));
      out[i].lo = p[i * 4 + 2];
      out[i].hi = p[i * 4 + 3];
    }
  };
  readGens(pgen, nPgen, f->pgen);
  readGens(igen, nIgen, f->igen);
  f->pbag.resize(nPbag);
  for (size_t i = 0; i < nPbag; ++i) f->pbag[i].gen = rd16(pbag + i * 4);
  f->ibag.resize(nIbag);
  for (size_t i = 0; i < nIbag; ++i) f->ibag[i].gen = rd16(ibag + i * 4);
  f->inst.resize(nInst);
  for (size_t i = 0; i < nInst; ++i) {
    f->inst[i].name = rdName(inst + i * 22, 20);
    f->inst[i].bag = rd16(inst + i * 22 + 20);
  }
  f->samples.resize(nShdr);
  for (size_t i = 0; i < nShdr; ++i) {
    const uint8_t* p = shdr + i * 46;
    auto& s = f->samples[i];
    s.name = rdName(p, 20);
    s.start = rd32(p + 20);
    s.end = rd32(p + 24);
    s.loopStart = rd32(p + 28);
    s.loopEnd = rd32(p + 32);
    s.rate = rd32(p + 36);
    s.originalPitch = p[40];
    s.correction = static_cast<int8_t>(p[41]);
    s.type = rd16(p + 44);
  }
  for (size_t i = 0; i + 1 < nPhdr; ++i) {
    const uint8_t* p = phdr + i * 38;
    Sf2Preset pr;
    pr.name = rdName(p, 20);
    pr.program = rd16(p + 20);
    pr.bank = rd16(p + 22);
    pr.bagStart = rd16(p + 24);
    pr.bagEnd = rd16(p + 38 + 24);
    if (pr.bagEnd < pr.bagStart || pr.bagEnd > nPbag) continue;
    f->presets.push_back(pr);
  }
  std::stable_sort(f->presets.begin(), f->presets.end(), [](const Sf2Preset& a, const Sf2Preset& c) {
    return a.bank != c.bank ? a.bank < c.bank : a.program < c.program;
  });
  if (f->presets.empty()) return fail("SoundFont has no presets");
  return f;
}

SamplePtr sf2MakeSample(const Sf2File& f, size_t idx) {
  if (idx >= f.samples.size()) return nullptr;
  const auto& h = f.samples[idx];
  size_t a = std::min<size_t>(h.start, f.data.size()), e = std::min<size_t>(h.end, f.data.size());
  if (e <= a + 1) return nullptr;
  auto s = std::make_shared<Sample>();
  s->sampleRate = h.rate > 0 ? h.rate : 44100;
  s->left.resize(e - a);
  for (size_t i = a; i < e; ++i) s->left[i - a] = static_cast<float>(f.data[i]) / 32768.f;
  s->right = s->left;
  return s;
}

std::vector<Zone> sf2PresetZones(const Sf2File& f, const std::string& path, size_t preset) {
  std::vector<Zone> out;
  if (preset >= f.presets.size()) return out;
  const Sf2Preset& pr = f.presets[preset];
  auto gensOf = [](const std::vector<Sf2File::Bag>& bags, const std::vector<Sf2File::Gen>& gens, size_t bag) {
    std::pair<size_t, size_t> r{0, 0};
    if (bag >= bags.size()) return r;
    r.first = bags[bag].gen;
    r.second = bag + 1 < bags.size() ? bags[bag + 1].gen : gens.size();
    r.first = std::min(r.first, gens.size());
    r.second = std::clamp(r.second, r.first, gens.size());
    return r;
  };
  GenSet pGlobal;
  pGlobal.defaults();
  // Preset values are offsets: start the global preset zone from zeros.
  pGlobal.v.fill(0);
  bool firstP = true;
  for (size_t pb = pr.bagStart; pb < pr.bagEnd && pb < f.pbag.size(); ++pb) {
    auto [g0, g1] = gensOf(f.pbag, f.pgen, pb);
    GenSet pz = pGlobal;
    bool hasInst = false;
    for (size_t g = g0; g < g1; ++g) {
      pz.apply(f.pgen[g]);
      if (f.pgen[g].oper == gInstrument) hasInst = true;
    }
    if (!hasInst) {
      if (firstP) pGlobal = pz;
      firstP = false;
      continue;
    }
    firstP = false;
    size_t ii = static_cast<size_t>(static_cast<uint16_t>(pz.v[gInstrument]));
    if (ii + 1 >= f.inst.size()) continue;
    GenSet iGlobal;
    iGlobal.defaults();
    bool firstI = true;
    for (size_t ib = f.inst[ii].bag; ib < f.inst[ii + 1].bag && ib < f.ibag.size(); ++ib) {
      auto [h0, h1] = gensOf(f.ibag, f.igen, ib);
      GenSet iz = iGlobal;
      bool hasSmp = false;
      for (size_t g = h0; g < h1; ++g) {
        iz.apply(f.igen[g]);
        if (f.igen[g].oper == gSampleId) hasSmp = true;
      }
      if (!hasSmp) {
        if (firstI) iGlobal = iz;
        firstI = false;
        continue;
      }
      firstI = false;
      size_t si = static_cast<size_t>(static_cast<uint16_t>(iz.v[gSampleId]));
      if (si >= f.samples.size()) continue;
      const auto& sh = f.samples[si];
      if (sh.type & 0x8000) continue;  // ROM sample
      int klo = std::max(iz.klo, pz.klo), khi = std::min(iz.khi, pz.khi);
      int vlo = std::max(iz.vlo, pz.vlo), vhi = std::min(iz.vhi, pz.vhi);
      if (klo > khi || vlo > vhi) continue;
      // Instrument value + preset offset for additive generators.
      auto val = [&](int g) {
        bool additive = g != gSampleId && g != gSampleModes && g != gExclusive && g != gRootKey && g != gKeyRange &&
                        g != gVelRange && g != gInstrument && !(g <= 4) && g != gEndCoarse && g != gLoopStartCoarse && g != gLoopEndCoarse;
        return iz.v[static_cast<size_t>(g)] + (additive ? pz.v[static_cast<size_t>(g)] : 0);
      };
      Zone z;
      z.path = "sf2:" + path + "#" + std::to_string(si);
      z.name = sh.name;
      z.lo = klo, z.hi = khi, z.vlo = vlo, z.vhi = vhi;
      int rk = iz.v[gRootKey];
      z.root = rk >= 0 && rk <= 127 ? rk : (sh.originalPitch <= 127 ? sh.originalPitch : 60);
      z.tune = static_cast<float>(val(gCoarseTune) * 100 + val(gFineTune) + sh.correction);
      z.keyTrack = static_cast<float>(val(gScaleTuning)) / 100.f;
      z.gain = -0.04f * static_cast<float>(std::clamp(val(gAttenuation), 0, 1440));  // EMU-style 0.4 dB per cB step
      z.pan = std::clamp(static_cast<float>(val(gPan)) / 500.f, -1.f, 1.f);
      int64_t len = static_cast<int64_t>(sh.end) - sh.start;
      int64_t st = static_cast<int64_t>(iz.v[gStartOfs]) + 32768LL * iz.v[gStartCoarse];
      int64_t en = len + iz.v[gEndOfs] + 32768LL * iz.v[gEndCoarse];
      int64_t ls = static_cast<int64_t>(sh.loopStart) - sh.start + iz.v[gLoopStartOfs] + 32768LL * iz.v[gLoopStartCoarse];
      int64_t le = static_cast<int64_t>(sh.loopEnd) - sh.start + iz.v[gLoopEndOfs] + 32768LL * iz.v[gLoopEndCoarse];
      z.start = std::clamp<int64_t>(st, 0, std::max<int64_t>(0, len - 1));
      en = std::clamp<int64_t>(en, z.start + 1, len);
      z.end = en >= len ? 0 : en;
      int modes = iz.v[gSampleModes] & 3;
      if ((modes == 1 || modes == 3) && le > ls + 1 && ls >= 0 && le <= len) {
        z.loop = modes == 1 ? LoopForward : LoopSustain;
        z.loopStart = ls;
        z.loopEnd = le;
      }
      z.choke = std::clamp(iz.v[gExclusive], 0, 127);
      int fc = val(gFilterFc);
      if (fc < 13500) z.cutoff = static_cast<float>(std::clamp(8.176 * std::pow(2.0, fc / 1200.0), 20.0, 20000.0));
      z.ownEnv = true;
      z.attack = static_cast<float>(tc2s(val(gAttackVol)) + (val(gDelayVol) > -12000 ? tc2s(val(gDelayVol)) : 0.0));
      z.hold = static_cast<float>(val(gHoldVol) > -12000 ? tc2s(val(gHoldVol)) : 0.0);
      z.decay = static_cast<float>(tc2s(val(gDecayVol)));
      z.sustain = static_cast<float>(std::pow(10.0, -std::clamp(val(gSustainVol), 0, 1440) / 200.0));
      z.release = static_cast<float>(tc2s(val(gReleaseVol)));
      clampZone(z);
      out.push_back(z);
    }
  }
  return out;
}

}  // namespace sf2legacy
