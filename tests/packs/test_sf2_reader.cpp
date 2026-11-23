// Gridline: the shared SoundFont reader. A bank made here (every generator, modulators on both
// levels, 24-bit samples, stereo links) reads back as written; damaged files are refused or
// trimmed without a crash; an .sf3 decodes to the same samples as the .sf2 made from its audio.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "engine/WavPack.h"
#include "packs/samplers/sf2/Sf2File.h"
#include "sf2_bank.h"

using namespace gl;
using namespace gl::packs::samplers;

namespace {

// A short Vorbis stream (a 441 Hz sine, 22.05 kHz, 40 ms) encoded for this test.
const uint8_t kOgg[] = {
#include "sf3_tone.inc"
};

const Sf2Mod* findMod(const std::vector<Sf2Mod>& v, uint16_t src, uint16_t dest) {
  for (const auto& m : v)
    if (m.src == src && m.dest == dest) return &m;
  return nullptr;
}

bool skipOp(int op) { return op == 41 || op == 43 || op == 44 || op == 53 || op == 14 || op == 18 || op == 19 || op == 20 || op == 42 || op == 49 || op == 55; }
int opValue(int op) { return op == 54 ? 1 : op == 58 ? 64 : op == 46 || op == 47 ? -1 : 10 + op; }

// Every generator a zone may hold, each set to a value of its own.
sf2t::Zone everyGenerator() {
  sf2t::Zone z;
  z.range(sf2t::KeyRange, 20, 100).range(sf2t::VelRange, 10, 120);
  for (int op = 0; op <= 58; ++op)
    if (!skipOp(op)) z.g(static_cast<uint16_t>(op), opValue(op));
  z.g(sf2t::SampleId, 0);
  return z;
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("sf2reader");
  using namespace sf2t;
  // ------------------------------------------------------------ every generator, modulators, sm24
  {
    Bank b;
    Sample s{"Left", sine(2000, 20), {}, {}, 44100, 60, 0, 100, 1900, 4, 1};
    s.low24.assign(2000, 0x80);
    Sample r{"Right", sine(2000, 20, 8000), {}, {}, 44100, 60, 0, 100, 1900, 2, 0};
    r.low24.assign(2000, 0x7F);
    b.samples = {s, r};
    b.sm24 = true;
    Inst in{"All", {}};
    in.zones.push_back(Zone{}.g(Attenuation, 100).mod(0x0502, Attenuation, 480).mod(0x0081, FilterFc, -600));  // global zone
    Zone z = everyGenerator();
    z.mod(0x0081, FilterFc, -1200);  // replaces the global zone's identical one
    in.zones.push_back(z);
    in.zones.push_back(Zone{}.range(KeyRange, 0, 127).g(Pan, 500).g(SampleId, 1));
    b.insts = {in};
    Preset p{"Everything", 3, 0, {}};
    p.zones.push_back(Zone{}.g(CoarseTune, 2).mod(0x0502, Attenuation, 100));  // global: adds
    p.zones.push_back(Zone{}.range(KeyRange, 0, 90).g(FineTune, 7).g(StartOfs, 999).g(SampleModes, 3).g(Reverb, 30).mod(0x00DB, Chorus, 300)
                          .g(Instrument, 0));
    Preset drums{"Kit", 0, 128, {Zone{}.g(Instrument, 0)}};
    Preset other{"Bank Eight", 5, 8, {Zone{}.g(Instrument, 0)}};
    b.presets = {drums, p, other};
    auto bytes = b.build();
    std::string err;
    auto f = parseSf2(bytes.data(), bytes.size(), &err);
    expect(f != nullptr, ("bank parses: " + err).c_str());
    if (f) {
      expect(f->presets.size() == 3 && f->presets[0].bank == 0 && f->presets[1].bank == 8 && f->presets[2].bank == 128, "presets sorted by bank");
      expect(f->versionMajor == 2 && f->versionMinor == 4 && !f->data24.empty(), "version and sm24 read");
      expect(f->samples.size() == 3 && f->samples[0].type == 4 && f->samples[0].link == 1 && f->samples[1].type == 2, "stereo link fields");
      const float v24 = f->at(f->samples[0].start + 5);
      const float v16 = static_cast<float>(f->data[f->samples[0].start + 5]) / 32768.f;
      expect(std::fabs(v24 - (v16 + 128.f / 8388608.f)) < 1e-7f, "24-bit frame = 16 bits plus the low byte");
      auto regs = sf2PresetRegions(*f, 0);
      expect(regs.size() == 2, "two sounding zones (global zones are not zones)");
      if (regs.size() == 2) {
        const Sf2Region& g = regs[0];
        bool all = true;
        for (int op = 0; op <= 58; ++op) {
          if (skipOp(op)) continue;
          int want = opValue(op);
          if (op == CoarseTune) want += 2;  // preset global zone
          if (op == FineTune) want += 7;    // preset zone
          if (op == Reverb) want += 30;
          if (g.gen[static_cast<size_t>(op)] != want) {
            std::printf("  generator %d: %d, expected %d\n", op, g.gen[static_cast<size_t>(op)], want);
            all = false;
          }
        }
        expect(all, "every generator: instrument value plus preset offset (instrument-only ones not added)");
        expect(g.klo == 20 && g.khi == 90 && g.vlo == 10 && g.vhi == 120, "key and velocity ranges intersect");
        expect(regs[1].gen[Attenuation] == 100 && regs[1].gen[Pan] == 500 && regs[1].gen[CoarseTune] == 2, "global zone values and preset offsets");
        expect(regs[1].gen[FilterFc] == 13500 && regs[1].gen[AttackVolEnv] == -12000 && regs[1].gen[ScaleTuning] == 100 &&
                   regs[1].gen[RootKey] == -1,
               "defaults where nothing sets a generator");
        const Sf2Mod* vel = findMod(g.mods, 0x0502, Attenuation);
        expect(vel && vel->amount == 580, "instrument modulator replaces the default, preset one adds to it");
        const Sf2Mod* wheel = findMod(g.mods, 0x0081, FilterFc);
        expect(wheel && wheel->amount == -1200, "local modulator replaces the global zone's");
        expect(findMod(g.mods, 0x00DB, Chorus) && findMod(g.mods, 0x020E, FineTune) && findMod(g.mods, 0x0587, Attenuation),
               "preset's own modulator and the defaults present");
        const Sf2Mod* gw = findMod(regs[1].mods, 0x0081, FilterFc);
        expect(gw && gw->amount == -600, "global zone modulator");
      }
      expect(sf2DefaultMods().size() == 10, "ten default modulators");
      bool exact = false;
      expect(sf2NearestPreset(*f, 0, 3, &exact) == 0 && exact, "exact preset found");
      expect(f->presets[static_cast<size_t>(sf2NearestPreset(*f, 8, 3, &exact))].bank == 0 && !exact, "missing bank falls back to bank 0");
      expect(f->presets[static_cast<size_t>(sf2NearestPreset(*f, 128, 25))].bank == 128, "missing drum kit falls back to kit 0");
      expect(sf2NearestPreset(*f, 0, 7) == 0, "missing program: the nearest in the family");
    }
    // ------------------------------------------------------------ damaged files
    for (size_t cut = 0; cut < bytes.size(); cut += 7) {
      auto g = parseSf2(bytes.data(), cut);
      if (g)
        for (size_t q = 0; q < g->presets.size(); ++q) (void)sf2PresetRegions(*g, q);
    }
    expect(parseSf2(bytes.data(), 40) == nullptr, "truncated file refused");
    std::vector<uint8_t> bad = bytes;
    for (size_t i = 12; i + 8 < bad.size(); ++i)
      if (std::memcmp(bad.data() + i, "pbag", 4) == 0) bad[i + 4] = 0xFF, bad[i + 5] = 0xFF, bad[i + 6] = 0xFF, bad[i + 7] = 0x7F;
    {
      auto g = parseSf2(bad.data(), bad.size());
      if (g)
        for (size_t q = 0; q < g->presets.size(); ++q) (void)sf2PresetRegions(*g, q);
      expect(true, "oversized chunk size read without a crash");
    }
    // Damage: every byte of the preset data flipped in turn.
    std::vector<uint8_t> noisy = bytes;
    size_t pdta = 0;
    for (size_t i = 12; i + 4 < noisy.size(); ++i)
      if (std::memcmp(noisy.data() + i, "pdta", 4) == 0) pdta = i;
    bool sane = true;
    int kept = 0;
    for (size_t i = pdta; i < noisy.size(); ++i) {
      const uint8_t keep = noisy[i];
      noisy[i] = static_cast<uint8_t>(keep ^ 0xA5);
      if (auto g = parseSf2(noisy.data(), noisy.size())) {
        for (size_t q = 0; q < g->presets.size(); ++q)
          for (const auto& rg : sf2PresetRegions(*g, q)) {
            ++kept;
            if (rg.sample < 0 || static_cast<size_t>(rg.sample) >= g->samples.size()) sane = false;
          }
      }
      noisy[i] = keep;
    }
    expect(pdta > 0 && kept > 0 && sane, "damaged preset data: every zone kept points at a real sample");
    std::vector<uint8_t> junk(64, 7);
    expect(parseSf2(junk.data(), junk.size()) == nullptr && parseSf2(nullptr, 0) == nullptr, "not a SoundFont refused");
  }
  // ------------------------------------------------------------ SF3 (Vorbis) = SF2 from the same audio
  {
    DecodedAudio d;
    std::string err;
    expect(decodeOggVorbis(kOgg, sizeof kOgg, d, &err) && d.frames() > 500, ("test stream decodes: " + err).c_str());
    const size_t n = d.frames();
    Sample s2{"Tone", {}, {}, {}, 22050, 69, 0, 100, static_cast<uint32_t>(n > 200 ? n - 100 : 1), 1, 0};
    for (size_t i = 0; i < n; ++i) {
      const float x = std::clamp(d.interleaved[i * static_cast<size_t>(d.channels)], -1.f, 1.f);
      const int32_t q = std::clamp(static_cast<int32_t>(std::lround(x * 8388608.f)), -8388608, 8388607);
      s2.pcm.push_back(static_cast<int16_t>(q >> 8));
      s2.low24.push_back(static_cast<uint8_t>(q & 0xFF));
    }
    Bank a;
    a.samples = {s2};
    a.sm24 = true;
    a.insts = {Inst{"Tone", {Zone{}.g(SampleModes, 1).g(SampleId, 0)}}};
    a.presets = {Preset{"Tone", 0, 0, {Zone{}.g(Instrument, 0)}}};
    Bank c = a;
    c.sf3 = true;
    c.samples[0].ogg.assign(kOgg, kOgg + sizeof kOgg);
    auto ab = a.build(), cb = c.build();
    auto fa = parseSf2(ab.data(), ab.size()), fc = parseSf2(cb.data(), cb.size(), &err);
    expect(fa && fc, ("sf2 and sf3 parse: " + err).c_str());
    if (fa && fc) {
      expect(fc->sf3 && fc->undecodable == 0 && !fa->sf3, "sf3 known as compressed");
      const auto &ha = fa->samples[0], &hc = fc->samples[0];
      expect(hc.end - hc.start == ha.end - ha.start && hc.loopStart - hc.start == ha.loopStart - ha.start &&
                 hc.loopEnd - hc.start == ha.loopEnd - ha.start && hc.rate == 22050 && (hc.type & 0x10) == 0,
             "sf3 sample header in frames");
      double diff = 0;
      for (size_t i = 0; i < n; ++i) diff = std::max(diff, static_cast<double>(std::fabs(fa->at(ha.start + i) - fc->at(hc.start + i))));
      expect(diff < 1e-6, "sf3 decodes to the same samples as its sf2");
      expect(sf2PresetRegions(*fc, 0).size() == 1, "sf3 preset has its zone");
    }
    Bank broken = c;
    broken.samples[0].ogg.assign(40, 3);
    auto bb = broken.build();
    auto fb = parseSf2(bb.data(), bb.size());
    expect(fb && fb->undecodable == 1, "an undecodable sf3 sample is counted, the file still opens");
  }
  // ------------------------------------------------------------ cache
  {
    Bank b;
    b.samples = {Sample{"S", dc(100), {}, {}, 44100, 60, 0, 0, 0, 1, 0}};
    b.insts = {Inst{"I", {Zone{}.g(SampleId, 0)}}};
    b.presets = {Preset{"P", 0, 0, {Zone{}.g(Instrument, 0)}}};
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp ? tmp : "/tmp") + "/cache test.sf2";
    writeFile(path, b.build());
    bool failed = true;
    auto f1 = requestSf2(path, &failed);
    expect(f1 && !failed && loadSf2(path) == f1, "request reads off the interface thread and caches");
    bool f2failed = false;
    expect(!requestSf2(path + ".missing", &f2failed) && f2failed, "a missing file is reported");
    expect(isSoundFontPath("A.SF2") && isSoundFontPath("b.sf3") && !isSoundFontPath("c.sfz"), "soundfont extensions");
  }
  return finish();
}
