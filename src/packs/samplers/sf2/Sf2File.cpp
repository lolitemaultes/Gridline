// Gridline: the shared SoundFont reader (see Sf2File.h).
#include "Sf2File.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include "../../../engine/Sample.h"
#include "../../../engine/WavPack.h"

namespace gl::packs::samplers {

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
std::string rdName(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  size_t z = s.find('\0');
  if (z != std::string::npos) s.resize(z);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// Decodes the Vorbis-compressed samples of an .sf3 into 16-bit data with low bytes (24-bit), so
// the rest of Gridline reads it like any SoundFont. Headers are rewritten to frame positions.
void decodeSf3(Sf2File& f, const uint8_t* smpl, size_t bytes) {
  std::vector<int16_t> out;
  std::vector<uint8_t> low;
  for (auto& h : f.samples) {
    if (!(h.type & 0x10) || (h.type & 0x8000)) continue;
    h.type = static_cast<uint16_t>(h.type & ~0x10);
    const size_t a = std::min<size_t>(h.start, bytes), e = std::min<size_t>(h.end, bytes);
    DecodedAudio d;
    std::string err;
    if (e <= a || !decodeOggVorbis(smpl + a, e - a, d, &err) || d.frames() == 0) {
      ++f.undecodable;
      h.start = h.end = h.loopStart = h.loopEnd = 0;
      continue;
    }
    const uint32_t start = static_cast<uint32_t>(out.size());
    const size_t n = d.frames();
    for (size_t i = 0; i < n; ++i) {
      const float v = std::clamp(d.interleaved[i * static_cast<size_t>(d.channels)], -1.f, 1.f);
      const int32_t q = std::clamp(static_cast<int32_t>(std::lround(v * 8388608.f)), -8388608, 8388607);
      out.push_back(static_cast<int16_t>(q >> 8));
      low.push_back(static_cast<uint8_t>(q & 0xFF));
    }
    for (int i = 0; i < 46; ++i) out.push_back(0), low.push_back(0);
    // .sf3 loop points count frames from the sample's own start.
    h.loopStart = start + std::min<uint32_t>(h.loopStart, static_cast<uint32_t>(n));
    h.loopEnd = start + std::min<uint32_t>(h.loopEnd, static_cast<uint32_t>(n));
    h.start = start;
    h.end = start + static_cast<uint32_t>(n);
    if (d.sampleRate > 0 && h.rate == 0) h.rate = static_cast<uint32_t>(d.sampleRate);
  }
  f.data = std::move(out);
  f.data24 = std::move(low);
}

}  // namespace

int32_t sf2GenDefault(int op) {
  switch (op) {
    case kFilterFc: return 13500;
    case kDelayModLfo: case kDelayVibLfo: case kDelayModEnv: case kAttackModEnv: case kHoldModEnv: case kDecayModEnv:
    case kReleaseModEnv: case kDelayVolEnv: case kAttackVolEnv: case kHoldVolEnv: case kDecayVolEnv: case kReleaseVolEnv:
      return -12000;
    case kKeynum: case kVelocity: case kRootKey: return -1;
    case kScaleTuning: return 100;
    default: return 0;
  }
}

bool sf2PresetAdds(int op) {
  switch (op) {
    case kStartOfs: case kEndOfs: case kLoopStartOfs: case kLoopEndOfs: case kStartCoarse: case kEndCoarse: case kLoopStartCoarse:
    case kLoopEndCoarse: case kKeynum: case kVelocity: case kSampleId: case kSampleModes: case kExclusive: case kRootKey:
    case kInstrument: case kKeyRange: case kVelRange:
      return false;
    default: return op >= 0 && op < kSf2GenCount;
  }
}

const std::vector<Sf2Mod>& sf2DefaultMods() {
  // Source words: index | CC flag 0x80 | negative 0x100 | bipolar 0x200 | type << 10 (1 concave, 3 switch).
  static const std::vector<Sf2Mod> m = {
      {0x0502, kAttenuation, 960, 0, 0},       // velocity, concave, negative -> level
      {0x0102, kFilterFc, -2400, 0x0C02, 0},   // velocity -> cutoff (only from velocity 64 up)
      {0x000D, kVibLfoToPitch, 50, 0, 0},      // channel pressure -> vibrato depth
      {0x0081, kVibLfoToPitch, 50, 0, 0},      // mod wheel -> vibrato depth
      {0x0587, kAttenuation, 960, 0, 0},       // CC 7 volume
      {0x028A, kPan, 1000, 0, 0},              // CC 10 pan
      {0x058B, kAttenuation, 960, 0, 0},       // CC 11 expression
      {0x00DB, kReverbSend, 200, 0, 0},        // CC 91 reverb send
      {0x00DD, kChorusSend, 200, 0, 0},        // CC 93 chorus send
      {0x020E, kFineTune, 12700, 0x0010, 0},   // pitch wheel x sensitivity
  };
  return m;
}

bool isSoundFontPath(const std::string& path) {
  if (path.size() < 4) return false;
  std::string e = path.substr(path.size() - 4);
  for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".sf2" || e == ".sf3";
}

std::shared_ptr<const Sf2File> parseSf2(const uint8_t* b, size_t n, std::string* err) {
  auto fail = [&](const char* m) -> std::shared_ptr<const Sf2File> {
    if (err) *err = m;
    return nullptr;
  };
  if (!b || n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "sfbk", 4) != 0) return fail("not an SF2 file");
  auto f = std::make_shared<Sf2File>();
  size_t riffEnd = std::min(n, static_cast<size_t>(rd32(b + 4)) + 8);
  const uint8_t *phdr = nullptr, *pbag = nullptr, *pmod = nullptr, *pgen = nullptr, *inst = nullptr, *ibag = nullptr, *imod = nullptr,
                *igen = nullptr, *shdr = nullptr;
  size_t nPhdr = 0, nPbag = 0, nPmod = 0, nPgen = 0, nInst = 0, nIbag = 0, nImod = 0, nIgen = 0, nShdr = 0;
  const uint8_t *smpl = nullptr, *sm24 = nullptr;
  size_t smplBytes = 0, sm24Bytes = 0;
  size_t pos = 12;
  while (pos + 8 <= riffEnd) {
    const uint8_t* ck = b + pos;
    size_t sz = rd32(ck + 4);
    if (sz > riffEnd - pos - 8) sz = riffEnd - pos - 8;
    if (std::memcmp(ck, "LIST", 4) == 0 && sz >= 4) {
      size_t lp = pos + 12, lend = pos + 8 + sz;
      while (lp + 8 <= lend) {
        const uint8_t* sc = b + lp;
        size_t ssz = rd32(sc + 4);
        if (ssz > lend - lp - 8) ssz = lend - lp - 8;
        const uint8_t* d = sc + 8;
        auto is = [&](const char* id) { return std::memcmp(sc, id, 4) == 0; };
        if (is("INAM")) f->name = rdName(d, ssz);
        else if (is("ifil") && ssz >= 4) f->versionMajor = rd16(d), f->versionMinor = rd16(d + 2);
        else if (is("smpl")) smpl = d, smplBytes = ssz;
        else if (is("sm24")) sm24 = d, sm24Bytes = ssz;
        else if (is("phdr")) phdr = d, nPhdr = ssz / 38;
        else if (is("pbag")) pbag = d, nPbag = ssz / 4;
        else if (is("pmod")) pmod = d, nPmod = ssz / 10;
        else if (is("pgen")) pgen = d, nPgen = ssz / 4;
        else if (is("inst")) inst = d, nInst = ssz / 22;
        else if (is("ibag")) ibag = d, nIbag = ssz / 4;
        else if (is("imod")) imod = d, nImod = ssz / 10;
        else if (is("igen")) igen = d, nIgen = ssz / 4;
        else if (is("shdr")) shdr = d, nShdr = ssz / 46;
        lp += 8 + ssz + (ssz & 1);
      }
    }
    pos += 8 + sz + (sz & 1);
  }
  if (!phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr || nPhdr < 2 || nInst < 2 || nShdr < 1)
    return fail("the SF2 file is missing preset data");
  if (!smpl) return fail("the SF2 file has no sample data");
  auto readGens = [](const uint8_t* p, size_t cnt, std::vector<Sf2File::Gen>& out) {
    out.resize(cnt);
    for (size_t i = 0; i < cnt; ++i) {
      out[i].oper = rd16(p + i * 4);
      out[i].amount = static_cast<int16_t>(rd16(p + i * 4 + 2));
      out[i].lo = p[i * 4 + 2];
      out[i].hi = p[i * 4 + 3];
    }
  };
  auto readMods = [](const uint8_t* p, size_t cnt, std::vector<Sf2Mod>& out) {
    out.resize(p ? cnt : 0);
    for (size_t i = 0; i < out.size(); ++i) {
      const uint8_t* q = p + i * 10;
      out[i] = {rd16(q), rd16(q + 2), static_cast<int16_t>(rd16(q + 4)), rd16(q + 6), rd16(q + 8)};
    }
  };
  auto readBags = [](const uint8_t* p, size_t cnt, std::vector<Sf2File::Bag>& out) {
    out.resize(cnt);
    for (size_t i = 0; i < cnt; ++i) out[i] = {rd16(p + i * 4), rd16(p + i * 4 + 2)};
  };
  readGens(pgen, nPgen, f->pgen);
  readGens(igen, nIgen, f->igen);
  readMods(pmod, nPmod, f->pmod);
  readMods(imod, nImod, f->imod);
  readBags(pbag, nPbag, f->pbag);
  readBags(ibag, nIbag, f->ibag);
  f->inst.resize(nInst);
  for (size_t i = 0; i < nInst; ++i) {
    f->inst[i].name = rdName(inst + i * 22, 20);
    f->inst[i].bag = rd16(inst + i * 22 + 20);
  }
  f->samples.resize(nShdr);
  bool compressed = f->versionMajor == 3;
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
    s.link = rd16(p + 42);
    s.type = rd16(p + 44);
    if (s.type & 0x10) compressed = true;
  }
  if (compressed) {
    f->sf3 = true;
    decodeSf3(*f, smpl, smplBytes);
  } else {
    f->data.resize(smplBytes / 2);
    for (size_t i = 0; i < f->data.size(); ++i) f->data[i] = static_cast<int16_t>(rd16(smpl + i * 2));
    // 24-bit: one low byte per frame (2.04 and later); a chunk of the wrong size is ignored.
    const bool v204 = f->versionMajor > 2 || (f->versionMajor == 2 && f->versionMinor >= 4);
    if (sm24 && v204 && sm24Bytes >= f->data.size() && sm24Bytes <= f->data.size() + 1) f->data24.assign(sm24, sm24 + f->data.size());
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
  if (f->presets.empty()) return fail("the SF2 file has no patches");
  return f;
}

// ------------------------------------------------------------------ cache
namespace {
std::mutex& sfMu() {
  static auto* m = new std::mutex;  // never destroyed: a background read may end during exit
  return *m;
}
std::map<std::string, std::shared_ptr<const Sf2File>>& sfFiles() {
  static auto* m = new std::map<std::string, std::shared_ptr<const Sf2File>>;
  return *m;
}
std::mutex& failedMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::map<std::string, std::string>& failedFiles() {
  static auto* m = new std::map<std::string, std::string>;
  return *m;
}
std::shared_ptr<const Sf2File> cached(const std::string& path) {
  std::lock_guard<std::mutex> lk(sfMu());
  auto it = sfFiles().find(path);
  return it != sfFiles().end() ? it->second : nullptr;
}
std::shared_ptr<const Sf2File> readNow(const std::string& path, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open " + path;
    return nullptr;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto f = parseSf2(bytes.data(), bytes.size(), err);
  if (f) {
    std::lock_guard<std::mutex> lk(sfMu());
    sfFiles()[path] = f;
  }
  return f;
}
// A built-in sound ("builtin:<name>", the drum sounds every sample instrument can play) as a
// one-patch file: one zone over every key, rooted at C5, played once. A Patch Player without a
// SoundFont still sounds, as the Sampler does with the same path.
std::shared_ptr<const Sf2File> builtinFile(const std::string& path) {
  if (path.rfind("builtin:", 0) != 0) return nullptr;
  if (auto f = cached(path)) return f;
  SamplePtr smp = sampleCache().get(path);
  if (!smp || smp->frames() < 2) return nullptr;
  auto f = std::make_shared<Sf2File>();
  const std::string nm = path.substr(8);
  f->name = "Built-in " + nm;
  const size_t n = smp->frames();
  f->data.resize(n + 46, 0);  // the format keeps 46 silent frames after each sample
  for (size_t i = 0; i < n; ++i) {
    const float v = 0.5f * (smp->left[i] + smp->right[i]);
    f->data[i] = static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
  }
  Sf2SampleHeader h;
  h.name = nm;
  h.start = 0, h.end = static_cast<uint32_t>(n), h.loopStart = 0, h.loopEnd = static_cast<uint32_t>(n - 1);
  h.rate = static_cast<uint32_t>(std::lround(smp->sampleRate > 0 ? smp->sampleRate : 44100.0));
  h.originalPitch = 60;
  f->samples = {h};
  f->inst = {{nm, 0}, {"EOI", 1}};
  f->ibag = {{0, 0}, {1, 0}};
  f->igen = {{kSampleId, 0, 0, 0}};
  f->pbag = {{0, 0}, {1, 0}};
  f->pgen = {{kInstrument, 0, 0, 0}};
  Sf2Preset p;
  p.name = nm, p.bank = 0, p.program = 0, p.bagStart = 0, p.bagEnd = 1;
  f->presets = {p};
  std::lock_guard<std::mutex> lk(sfMu());
  sfFiles()[path] = f;
  return f;
}
void readLater(const std::string& path) {
  readInBackground("sf2\n" + path, [path] {
    std::string e;
    if (!readNow(path, &e)) {
      std::lock_guard<std::mutex> lk(failedMu());
      failedFiles()[path] = e.empty() ? "cannot read " + path : e;
    }
  });
}
}  // namespace

std::shared_ptr<const Sf2File> loadSf2(const std::string& path, std::string* err) {
  if (auto f = cached(path)) return f;
  if (path.rfind("builtin:", 0) == 0) {
    auto f = builtinFile(path);
    if (!f && err) *err = "no built-in sound " + path.substr(8);
    return f;
  }
  if (mustNotReadNow(path)) {  // FRZ-1118: read on a background thread; the rebuild after it finds it above
    {
      std::lock_guard<std::mutex> lk(failedMu());
      auto it = failedFiles().find(path);
      if (it != failedFiles().end()) {
        if (err) *err = it->second;
        return nullptr;
      }
    }
    readLater(path);
    if (err) *err = "not read yet (a web or network drive)";
    return nullptr;
  }
  return readNow(path, err);
}

std::shared_ptr<const Sf2File> requestSf2(const std::string& path, bool* failed, std::string* err) {
  if (failed) *failed = false;
  if (path.empty()) {
    if (failed) *failed = true;
    return nullptr;
  }
  if (auto f = cached(path)) return f;
  if (path.rfind("builtin:", 0) == 0) {  // rendered in microseconds: here, on any thread but audio
    auto f = builtinFile(path);
    if (!f) {
      if (failed) *failed = true;
      if (err) *err = "no built-in sound " + path.substr(8);
    }
    return f;
  }
  {
    std::lock_guard<std::mutex> lk(failedMu());
    auto it = failedFiles().find(path);
    if (it != failedFiles().end()) {
      if (failed) *failed = true;
      if (err) *err = it->second;
      return nullptr;
    }
  }
  if (!readsDeferredHere()) {  // a worker or a test: read it here
    std::string e;
    auto f = readNow(path, &e);
    if (!f) {
      if (failed) *failed = true;
      if (err) *err = e;
    }
    return f;
  }
  readLater(path);
  return nullptr;
}

// ------------------------------------------------------------------ zones of a preset
namespace {
struct GenSet {
  std::array<int32_t, kSf2GenCount> v{};
  int klo = 0, khi = 127, vlo = 0, vhi = 127;
  std::vector<Sf2Mod> mods;
  void apply(const Sf2File::Gen& g) {
    if (g.oper >= kSf2GenCount) return;
    if (g.oper == kKeyRange) klo = g.lo, khi = g.hi;
    else if (g.oper == kVelRange) vlo = g.lo, vhi = g.hi;
    else v[g.oper] = g.amount;
  }
};
std::pair<size_t, size_t> range(size_t start, size_t next, size_t size) {
  start = std::min(start, size);
  return {start, std::clamp(next, start, size)};
}
// A zone's local modulators replace identical ones from the global zone.
void addMods(std::vector<Sf2Mod>& into, const std::vector<Sf2Mod>& all, size_t a, size_t e) {
  for (size_t i = a; i < e; ++i) {
    const Sf2Mod& m = all[i];
    if (m.src == 0 && m.dest == 0 && m.amount == 0) continue;
    auto it = std::find_if(into.begin(), into.end(), [&](const Sf2Mod& o) { return o.sameAs(m); });
    if (it != into.end()) *it = m;
    else into.push_back(m);
  }
}
}  // namespace

std::vector<Sf2Region> sf2PresetRegions(const Sf2File& f, size_t preset) {
  std::vector<Sf2Region> out;
  if (preset >= f.presets.size()) return out;
  const Sf2Preset& pr = f.presets[preset];
  auto bagGens = [](const std::vector<Sf2File::Bag>& bags, size_t nGens, size_t bag) {
    if (bag >= bags.size()) return std::pair<size_t, size_t>{0, 0};
    return range(bags[bag].gen, bag + 1 < bags.size() ? bags[bag + 1].gen : nGens, nGens);
  };
  auto bagMods = [](const std::vector<Sf2File::Bag>& bags, size_t nMods, size_t bag) {
    if (bag >= bags.size()) return std::pair<size_t, size_t>{0, 0};
    return range(bags[bag].mod, bag + 1 < bags.size() ? bags[bag + 1].mod : nMods, nMods);
  };
  GenSet pGlobal;  // preset values are offsets: they start at zero
  bool firstP = true;
  for (size_t pb = pr.bagStart; pb < pr.bagEnd && pb < f.pbag.size(); ++pb) {
    auto [g0, g1] = bagGens(f.pbag, f.pgen.size(), pb);
    auto [m0, m1] = bagMods(f.pbag, f.pmod.size(), pb);
    GenSet pz = pGlobal;
    bool hasInst = false;
    for (size_t g = g0; g < g1; ++g) {
      pz.apply(f.pgen[g]);
      if (f.pgen[g].oper == kInstrument) hasInst = true;
    }
    addMods(pz.mods, f.pmod, m0, m1);
    if (!hasInst) {
      if (firstP) pGlobal = pz;
      firstP = false;
      continue;
    }
    firstP = false;
    const size_t ii = static_cast<uint16_t>(pz.v[kInstrument]);
    if (ii + 1 >= f.inst.size()) continue;
    GenSet iGlobal;
    for (int g = 0; g < kSf2GenCount; ++g) iGlobal.v[static_cast<size_t>(g)] = sf2GenDefault(g);
    bool firstI = true;
    for (size_t ib = f.inst[ii].bag; ib < f.inst[ii + 1].bag && ib < f.ibag.size(); ++ib) {
      auto [h0, h1] = bagGens(f.ibag, f.igen.size(), ib);
      auto [n0, n1] = bagMods(f.ibag, f.imod.size(), ib);
      GenSet iz = iGlobal;
      bool hasSmp = false;
      for (size_t g = h0; g < h1; ++g) {
        iz.apply(f.igen[g]);
        if (f.igen[g].oper == kSampleId) hasSmp = true;
      }
      addMods(iz.mods, f.imod, n0, n1);
      if (!hasSmp) {
        if (firstI) iGlobal = iz;
        firstI = false;
        continue;
      }
      firstI = false;
      const size_t si = static_cast<uint16_t>(iz.v[kSampleId]);
      if (si >= f.samples.size()) continue;
      if (f.samples[si].type & 0x8000) continue;  // ROM sample
      Sf2Region r;
      r.klo = std::max(iz.klo, pz.klo), r.khi = std::min(iz.khi, pz.khi);
      r.vlo = std::max(iz.vlo, pz.vlo), r.vhi = std::min(iz.vhi, pz.vhi);
      if (r.klo > r.khi || r.vlo > r.vhi) continue;
      for (int g = 0; g < kSf2GenCount; ++g) {
        const auto k = static_cast<size_t>(g);
        r.gen[k] = iz.v[k] + (sf2PresetAdds(g) ? pz.v[k] : 0);
      }
      r.sample = static_cast<int>(si);
      r.instrument = static_cast<int>(ii);
      // Modulators: the defaults, replaced by identical instrument ones; preset ones add.
      r.mods = sf2DefaultMods();
      for (const auto& m : iz.mods) {
        auto it = std::find_if(r.mods.begin(), r.mods.end(), [&](const Sf2Mod& o) { return o.sameAs(m); });
        if (it != r.mods.end()) *it = m;
        else r.mods.push_back(m);
      }
      for (const auto& m : pz.mods) {
        auto it = std::find_if(r.mods.begin(), r.mods.end(), [&](const Sf2Mod& o) { return o.sameAs(m); });
        if (it != r.mods.end()) it->amount = static_cast<int16_t>(std::clamp(it->amount + m.amount, -32768, 32767));
        else r.mods.push_back(m);
      }
      out.push_back(std::move(r));
    }
  }
  return out;
}

int sf2FindPreset(const Sf2File& f, int bank, int program) {
  for (size_t i = 0; i < f.presets.size(); ++i)
    if (f.presets[i].bank == bank && f.presets[i].program == program) return static_cast<int>(i);
  return -1;
}

int sf2NearestPreset(const Sf2File& f, int bank, int program, bool* exact) {
  if (exact) *exact = false;
  if (f.presets.empty()) return -1;
  int i = sf2FindPreset(f, bank, program);
  if (i >= 0) {
    if (exact) *exact = true;
    return i;
  }
  if (bank == 128) {
    if ((i = sf2FindPreset(f, 128, 0)) >= 0) return i;
  } else if (bank != 0 && (i = sf2FindPreset(f, 0, program)) >= 0) {
    return i;
  }
  // The nearest program in the bank (General MIDI families are blocks of eight).
  const int b = sf2FindPreset(f, bank, program) < 0 && bank != 128 && bank != 0 ? 0 : bank;
  int best = -1, dist = 1 << 20;
  for (size_t k = 0; k < f.presets.size(); ++k) {
    const auto& p = f.presets[k];
    if (p.bank != b) continue;
    const bool sameFamily = p.program / 8 == program / 8;
    const int d = std::abs(p.program - program) + (sameFamily ? 0 : 1000);
    if (d < dist) dist = d, best = static_cast<int>(k);
  }
  return best >= 0 ? best : 0;
}

}  // namespace gl::packs::samplers
