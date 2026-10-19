// Gridline — "samplers" pack tests: Multisampler zone selection, loops,
// round robin, choke, SF2 import (a soundfont generated here), file-name
// mapping, Scratch Sampler moves; sound out, extremes, block-size invariance.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/WavWriter.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/samplers/samplers.h"

using namespace gl;
using namespace gl::packs::samplers;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir() {
  fs::path d = fs::temp_directory_path() / "gridline_test_samplers";
  fs::create_directories(d);
  return d;
}

// Mono WAV at 44.1 kHz from a generator function.
std::string writeTestWav(const std::string& name, size_t frames, const std::function<float(size_t)>& f) {
  AudioBuffer b(1, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = f(i);
  std::string p = (tmpDir() / name).string();
  writeWav(p, b, 44100, 24);
  return p;
}

struct Rig {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> gen;
  ProcessContext ctx;
  explicit Rig(const std::string& type, const std::string& data = "") {
    st = defaultPluginState(type);
    st.data = data;
    prepare();
    gen = createGenerator(type);
    ctx.sampleRate = 44100;
    ctx.tempo = 120;
  }
  void prepare() {
    aux = ChannelAux{};
    packs::prepareAux(st, aux);
  }
  void set(int k, float v) { st.params[static_cast<size_t>(k)] = v; }
  // Renders n frames in blocks; returns interleaved-free L and R.
  void run(std::vector<float>& L, std::vector<float>& R, size_t n, int block = 256) {
    size_t base = L.size();
    L.resize(base + n, 0.f);
    R.resize(base + n, 0.f);
    for (size_t p = 0; p < n; p += static_cast<size_t>(block)) {
      int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - p));
      gen->process(st, aux, ctx, L.data() + base + p, R.data() + base + p, len);
    }
  }
  float levelAfter(size_t settle, size_t n) {
    std::vector<float> L, R;
    run(L, R, settle + n);
    double s = 0;
    for (size_t i = settle; i < settle + n; ++i) s += L[i];
    return static_cast<float>(s / static_cast<double>(n));
  }
};

bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}
float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

std::string zoneText(const std::vector<Zone>& zs) {
  ZoneMap m;
  m.zones = zs;
  return serializeZoneMap(m);
}

Zone dcZone(const std::string& path, int lo, int hi, int vlo = 0, int vhi = 127) {
  Zone z;
  z.path = path;
  z.lo = lo, z.hi = hi, z.vlo = vlo, z.vhi = vhi;
  z.root = 60;
  z.loop = LoopForward;  // DC samples loop so they sound for the whole note
  return z;
}

// Minimal multisampler set-up: no velocity scaling, unity volume.
Rig dcRig(const std::vector<Zone>& zs) {
  Rig r("gl.multisampler", zoneText(zs));
  r.set(MsVelAmp, 0);
  r.set(MsVolume, 1);
  r.set(MsAttack, 0.001f);
  r.set(MsRelease, 0.005f);
  return r;
}

// ------------------------------------------------------------- tiny SF2 writer
struct Bytes {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(static_cast<uint8_t>(v & 0xFF)), u8(static_cast<uint8_t>(v >> 8)); }
  void u32(uint32_t v) { u16(static_cast<uint16_t>(v & 0xFFFF)), u16(static_cast<uint16_t>(v >> 16)); }
  void id(const char* s) { b.insert(b.end(), s, s + 4); }
  void name(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) u8(i < std::strlen(s) ? static_cast<uint8_t>(s[i]) : 0);
  }
  void append(const Bytes& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};
Bytes chunk(const char* id, const Bytes& body) {
  Bytes c;
  c.id(id);
  c.u32(static_cast<uint32_t>(body.b.size()));
  c.append(body);
  if (body.b.size() & 1) c.u8(0);
  return c;
}
Bytes list(const char* type, const std::vector<Bytes>& kids) {
  Bytes body;
  body.id(type);
  for (const auto& k : kids) body.append(k);
  return chunk("LIST", body);
}

std::vector<uint8_t> makeSf2() {
  // Samples: 0 = 200-frame sine (22.05 kHz), 1 = 200-frame square; 46 zero frames after each.
  Bytes smpl;
  for (int i = 0; i < 200; ++i) smpl.u16(static_cast<uint16_t>(static_cast<int16_t>(12000 * std::sin(i * 0.2))));
  for (int i = 0; i < 46; ++i) smpl.u16(0);
  for (int i = 0; i < 200; ++i) smpl.u16(static_cast<uint16_t>(static_cast<int16_t>((i / 10) % 2 ? 9000 : -9000)));
  for (int i = 0; i < 46; ++i) smpl.u16(0);
  Bytes ifil, inam;
  ifil.u16(2), ifil.u16(1);
  inam.name("Test Font", 10);
  Bytes phdr;
  auto preset = [&](const char* n, uint16_t prog, uint16_t bank, uint16_t bag) {
    phdr.name(n, 20), phdr.u16(prog), phdr.u16(bank), phdr.u16(bag), phdr.u32(0), phdr.u32(0), phdr.u32(0);
  };
  preset("Lead", 5, 0, 0);
  preset("Drums", 0, 128, 1);
  preset("EOP", 0, 0, 2);
  Bytes pbag;
  for (uint16_t g : {0, 1, 2}) pbag.u16(g), pbag.u16(0);
  Bytes pgen;
  pgen.u16(41), pgen.u16(0);  // preset zone 0 -> instrument 0
  pgen.u16(41), pgen.u16(0);  // preset zone 1 -> instrument 0
  pgen.u16(0), pgen.u16(0);   // terminal
  Bytes pmod, imod;
  for (int i = 0; i < 10; ++i) pmod.u8(0), imod.u8(0);
  Bytes inst;
  inst.name("Inst", 20), inst.u16(0);
  inst.name("EOI", 20), inst.u16(3);
  Bytes ibag;
  for (uint16_t g : {0, 1, 5, 8}) ibag.u16(g), ibag.u16(0);
  Bytes igen;
  auto gen = [&](uint16_t op, int16_t amt) { igen.u16(op), igen.u16(static_cast<uint16_t>(amt)); };
  auto range = [&](uint16_t op, uint8_t lo, uint8_t hi) { igen.u16(op), igen.u8(lo), igen.u8(hi); };
  gen(48, 60);          // global zone: 6 dB attenuation
  range(43, 0, 59);     // zone 1
  gen(58, 48);          // overriding root key
  gen(54, 1);           // loop continuously
  gen(53, 0);           // sample 0
  range(43, 60, 127);   // zone 2
  gen(52, -10);         // fine tune
  gen(53, 1);           // sample 1
  gen(0, 0);            // terminal
  Bytes shdr;
  auto sh = [&](const char* n, uint32_t s, uint32_t e, uint32_t ls, uint32_t le, uint32_t rate, uint8_t pitch, int8_t corr) {
    shdr.name(n, 20), shdr.u32(s), shdr.u32(e), shdr.u32(ls), shdr.u32(le), shdr.u32(rate), shdr.u8(pitch),
        shdr.u8(static_cast<uint8_t>(corr)), shdr.u16(0), shdr.u16(1);
  };
  sh("Sine", 0, 200, 50, 150, 22050, 60, 0);
  sh("Square", 246, 446, 256, 436, 44100, 72, 5);
  sh("EOS", 0, 0, 0, 0, 0, 0, 0);
  Bytes body;
  body.id("sfbk");
  body.append(list("INFO", {chunk("ifil", ifil), chunk("INAM", inam)}));
  body.append(list("sdta", {chunk("smpl", smpl)}));
  body.append(list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen), chunk("inst", inst),
                            chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)}));
  return chunk("RIFF", body).b;
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("samplers");
  // ---------------------------------------------------------------- catalogue
  const PluginInfo* ms = findPlugin("gl.multisampler");
  const PluginInfo* sc = findPlugin("gl.scratch");
  expect(ms && ms->params.size() == MsCount && ms->kind == PluginKind::Generator, "multisampler in catalogue");
  expect(sc && sc->params.size() == ScCount && sc->usesSample, "scratch sampler in catalogue");
  expect(ms && ms->presets.size() >= 2 && ms->presets.size() <= 6 && ms->editorWidth > 0, "multisampler presets + editor width");
  expect(sc && sc->presets.size() >= 2 && sc->presets.size() <= 6, "scratch presets");
  expect(createGenerator("gl.multisampler") != nullptr && createGenerator("gl.scratch") != nullptr, "generators created");
  {
    PluginState s;
    s.type = "gl.multisampler";
    s.params = {0.5f};
    normalizePluginState(s);
    expect(s.params.size() == MsCount && s.params[MsPolyphony] == 32.f, "normalize pads short multisampler state");
    PluginState t;
    t.type = "gl.scratch";
    normalizePluginState(t);
    expect(t.params.size() == ScCount, "normalize pads empty scratch state");
    ChannelAux aux;
    PluginState other = defaultPluginState("gl.synth");
    expect(!packs::prepareAux(other, aux), "prepareAux ignores other types");
  }

  // ---------------------------------------------------------------- factory sound
  {
    Rig r("gl.multisampler");
    expect(!r.aux.samples.empty() && r.aux.samples[0], "factory map prepared for empty data");
    r.gen->noteOn(1, 60, 0.8f, 0);
    std::vector<float> L, R;
    r.run(L, R, 22050);
    expect(peak(L) > 0.05f && finite(L) && finite(R), "factory set sounds after note on");
    r.gen->noteOff(1, 0);
    r.run(L, R, 44100);
    expect(peak(L, L.size() - 4410) < 1e-4f && !r.gen->active(), "release ends the voice");
    for (int set = 0; set < 5; ++set) {
      ZoneMap m = factoryZoneMap(set);
      bool ok = !m.zones.empty();
      for (const auto& z : m.zones) {
        SamplePtr s = resolveSample(z.path);
        ok = ok && s && z.loop == LoopSustain && z.loopEnd > z.loopStart;
        if (s) ok = ok && std::fabs(s->left[static_cast<size_t>(z.loopEnd - 1)] - s->left[static_cast<size_t>(z.loopStart - 1)]) < 2e-3f;
      }
      expect(ok, ("factory set " + std::to_string(set) + " has seamless loops").c_str());
    }
  }

  // ---------------------------------------------------------------- zone selection
  std::string dcA = writeTestWav("dc_a.wav", 4000, [](size_t) { return 0.25f; });
  std::string dcB = writeTestWav("dc_b.wav", 4000, [](size_t) { return -0.5f; });
  {
    Rig r = dcRig({dcZone(dcA, 0, 59), dcZone(dcB, 60, 127)});
    r.gen->noteOn(1, 50, 1.f, 0);
    float a = r.levelAfter(2000, 1000);
    r.gen->kill();
    r.gen->noteOn(2, 70, 1.f, 0);
    float b = r.levelAfter(2000, 1000);
    expect(a > 0.2f && a < 0.3f, "key 50 picks the low zone");
    expect(b < -0.4f && b > -0.6f, "key 70 picks the high zone");
  }
  {
    Rig r = dcRig({dcZone(dcA, 0, 127, 0, 63), dcZone(dcB, 0, 127, 64, 127)});
    r.gen->noteOn(1, 60, 0.3f, 0);
    float a = r.levelAfter(2000, 1000);
    r.gen->kill();
    r.gen->noteOn(2, 60, 0.9f, 0);
    float b = r.levelAfter(2000, 1000);
    expect(a > 0.2f && b < -0.4f, "velocity picks the layer");
  }
  {
    Rig r = dcRig({dcZone(dcA, 0, 127), dcZone(dcB, 0, 127)});
    r.gen->noteOn(1, 60, 1.f, 0);
    float a = r.levelAfter(2000, 1000);
    expect(std::fabs(a + 0.25f) < 0.03f, "overlapping zones layer");
  }
  {
    Zone a = dcZone(dcA, 0, 127), b = dcZone(dcB, 0, 127);
    a.group = b.group = 1;
    Rig r = dcRig({a, b});
    r.set(MsRoundRobin, RrCycle);
    float lv[4];
    for (int k = 0; k < 4; ++k) {
      r.gen->kill();
      r.gen->noteOn(static_cast<uint32_t>(k + 1), 60, 1.f, 0);
      lv[k] = r.levelAfter(1500, 500);
    }
    expect(lv[0] > 0 && lv[1] < 0 && lv[2] > 0 && lv[3] < 0, "round robin cycles through the group");
    r.set(MsRoundRobin, RrAvoid);
    bool noRepeat = true;
    float prev = 0;
    for (int k = 0; k < 16; ++k) {
      r.gen->kill();
      r.gen->noteOn(static_cast<uint32_t>(k + 10), 60, 1.f, 0);
      float l = r.levelAfter(1500, 200);
      if (k && (l > 0) == (prev > 0)) noRepeat = false;
      prev = l;
    }
    expect(noRepeat, "no-repeat round robin never repeats");
    r.set(MsRoundRobin, RrLayer);
    r.gen->kill();
    r.gen->noteOn(99, 60, 1.f, 0);
    expect(std::fabs(r.levelAfter(1500, 500) + 0.25f) < 0.03f, "layer mode plays the whole group");
  }
  {
    Zone a = dcZone(dcA, 0, 127);
    a.choke = 3;
    Rig r = dcRig({a});
    r.gen->noteOn(1, 60, 1.f, 0);
    r.gen->noteOn(2, 62, 1.f, 0);
    float l = r.levelAfter(2000, 500);
    expect(std::fabs(l - 0.25f) < 0.03f, "choke group cuts the previous note");
  }
  {
    Rig r = dcRig({dcZone(dcA, 0, 127)});
    r.set(MsPolyphony, 2);
    for (int k = 0; k < 5; ++k) r.gen->noteOn(static_cast<uint32_t>(k + 1), 60 + k, 1.f, 0);
    float l = r.levelAfter(2000, 500);
    expect(std::fabs(l - 0.5f) < 0.04f, "polyphony limit steals voices");
    r.gen->kill();
    r.set(MsPlayMode, PlayLegato);
    r.gen->noteOn(10, 60, 1.f, 0);
    r.gen->noteOn(11, 64, 1.f, 100);
    float m = r.levelAfter(2000, 500);
    expect(std::fabs(m - 0.25f) < 0.03f, "legato keeps a single voice");
  }

  // ---------------------------------------------------------------- loops
  std::string ramp = writeTestWav("ramp.wav", 1000, [](size_t i) { return static_cast<float>(i) / 1000.f; });
  {
    Zone z = dcZone(ramp, 0, 127);
    z.loop = LoopOff;
    Rig r = dcRig({z});
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 3000);
    expect(peak(L, 0, 990) > 0.5f && peak(L, 1100) == 0.f && !r.gen->active(), "unlooped zone stops at the sample end");
    z.loop = LoopForward;
    z.loopStart = 200, z.loopEnd = 800;
    Rig f = dcRig({z});
    f.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L2, R2;
    f.run(L2, R2, 10000);
    float lo = 1, hi = 0;
    for (size_t i = 5000; i < L2.size(); ++i) lo = std::min(lo, L2[i]), hi = std::max(hi, L2[i]);
    expect(f.gen->active() && lo > 0.15f && hi < 0.85f && hi > 0.7f, "forward loop stays inside the loop");
    z.loop = LoopBounce;
    Rig b = dcRig({z});
    b.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L3, R3;
    b.run(L3, R3, 10000);
    bool smooth = true;
    for (size_t i = 3000; i + 1 < L3.size(); ++i)
      if (std::fabs(L3[i + 1] - L3[i]) > 0.01f) smooth = false;
    expect(smooth && peak(L3, 5000) < 0.85f, "bounce loop reverses without jumps");
    z.loop = LoopSustain;
    Rig s = dcRig({z});
    s.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L4, R4;
    s.run(L4, R4, 5000);
    bool held = s.gen->active();
    s.set(MsRelease, 5.f);  // long release: the sample end must stop it
    s.gen->noteOff(1, 0);
    s.run(L4, R4, 3000);
    expect(held && !s.gen->active(), "sustain loop releases into the tail and ends");
    z.loop = LoopForward;
    z.xfade = 100;
    Rig x = dcRig({z});
    x.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L5, R5;
    x.run(L5, R5, 6000);
    float maxJump = 0;
    for (size_t i = 2000; i + 1 < L5.size(); ++i) maxJump = std::max(maxJump, std::fabs(L5[i + 1] - L5[i]));
    expect(finite(L5) && maxJump < 0.02f, "crossfaded loop has no jump at the seam");
    z = dcZone(ramp, 0, 127);
    z.loop = LoopOff;
    z.reverse = true;
    Rig rv = dcRig({z});
    rv.set(MsAttack, 0.001f);
    rv.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L6, R6;
    rv.run(L6, R6, 600);
    expect(L6[100] > L6[400] && L6[100] > 0.8f, "reverse plays from the end");
    z.reverse = false;
    z.start = 500;
    Rig so = dcRig({z});
    so.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L7, R7;
    so.run(L7, R7, 200);
    expect(L7[100] > 0.55f, "sample start offset");
  }
  {
    // Pitch: root 60 at key 72 plays an octave up = twice as fast through the ramp.
    Zone z = dcZone(ramp, 0, 127);
    z.loop = LoopOff;
    Rig r = dcRig({z});
    r.gen->noteOn(1, 72, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 1000);
    expect(L[300] > 0.55f && L[300] < 0.65f && peak(L, 520) == 0.f, "transposed zone plays at the right rate");
  }

  // ---------------------------------------------------------------- serialization + mapping
  {
    Zone a = dcZone("/x/Piano C4 soft.wav", 0, 60);
    a.tune = -12.5f, a.gain = -3, a.pan = 0.25f, a.loopStart = 10, a.loopEnd = 90, a.xfade = 5, a.group = 2, a.choke = 1;
    a.ownEnv = true, a.attack = 0.01f, a.release = 1.5f, a.name = "Soft | 60 = C";
    ZoneMap m;
    m.zones = {a, dcZone("/y/with space.wav", 61, 127)};
    m.sf2Path = "/z/font.sf2", m.sf2Bank = 0, m.sf2Program = 3, m.sf2Name = "Grand Piano";
    ZoneMap p = parseZoneMap(serializeZoneMap(m));
    expect(p.zones.size() == 2 && p.zones[0] == m.zones[0] && p.zones[1] == m.zones[1], "zone map round trip");
    expect(p.sf2Path == m.sf2Path && p.sf2Program == 3 && p.sf2Name == "Grand Piano", "soundfont header round trip");
    ZoneMap bare = parseZoneMap("/some/file.wav");
    expect(bare.zones.size() == 1 && bare.zones[0].lo == 0 && bare.zones[0].hi == 127, "bare path is a full-range zone");
  }
  expect(noteFromName("Piano_C4") == 60, "note name C4");
  expect(noteFromName("Bass A#2 soft.wav") == 46, "note name A#2");
  expect(noteFromName("str_Db3") == 49, "note name Db3");
  expect(noteFromName("Strings_060") == 60, "midi number in name");
  expect(noteFromName("kick") == -1 && noteFromName("Bass") == -1, "no note in name");
  {
    std::vector<Zone> zs = {zoneForFile("p_C3_soft.wav"), zoneForFile("p_C3_hard.wav"), zoneForFile("p_C4.wav"), zoneForFile("p_C5.wav")};
    autoMap(zs);
    expect(zs[0].lo == 0 && zs[0].hi == 54 && zs[2].lo == 55 && zs[2].hi == 66 && zs[3].lo == 67 && zs[3].hi == 127, "auto map key splits");
    expect(zs[0].vlo == 0 && zs[0].vhi == 63 && zs[1].vlo == 64 && zs[1].vhi == 127, "auto map velocity layers soft < hard");
    mapAsDrumKit(zs, 36);
    expect(zs[0].lo == 36 && zs[3].hi == 39 && zs[3].root == 39, "drum kit mapping");
  }

  // ---------------------------------------------------------------- SoundFont 2
  {
    auto bytes = makeSf2();
    std::string err;
    auto f = parseSf2(bytes.data(), bytes.size(), &err);
    expect(f != nullptr, ("sf2 parses: " + err).c_str());
    if (f) {
      expect(f->name == "Test Font" && f->presets.size() == 2, "sf2 name and presets");
      expect(f->presets[0].name == "Lead" && f->presets[0].program == 5 && f->presets[1].bank == 128, "sf2 presets sorted by bank");
      auto zs = sf2PresetZones(*f, "mem.sf2", 0);
      expect(zs.size() == 2, "sf2 preset has two zones");
      if (zs.size() == 2) {
        expect(zs[0].lo == 0 && zs[0].hi == 59 && zs[1].lo == 60 && zs[1].hi == 127, "sf2 key ranges");
        expect(zs[0].root == 48 && zs[1].root == 72, "sf2 root keys (override and sample pitch)");
        expect(zs[0].loop == LoopForward && zs[0].loopStart == 50 && zs[0].loopEnd == 150, "sf2 loop points relative to sample");
        expect(zs[1].loop == LoopOff && std::fabs(zs[1].tune + 5.f) < 1e-3f, "sf2 tuning (fine + correction)");
        expect(std::fabs(zs[0].gain + 2.4f) < 1e-3f && zs[0].ownEnv, "sf2 attenuation and envelope");
      }
      SamplePtr s = sf2MakeSample(*f, 0);
      expect(s && s->frames() == 200 && s->sampleRate == 22050, "sf2 sample data");
    }
    expect(parseSf2(bytes.data(), 40) == nullptr, "truncated sf2 rejected");
    std::vector<uint8_t> junk(64, 7);
    expect(parseSf2(junk.data(), junk.size()) == nullptr, "non-sf2 rejected");
    std::string path = (tmpDir() / "test.sf2").string();
    {
      std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ZoneMap m = sf2PresetMap(path, 0);
    expect(m.zones.size() == 2 && m.sf2Name == "Lead" && m.sf2Program == 5, "sf2 preset map from file");
    SamplePtr rs = resolveSample(m.zones.empty() ? "" : m.zones[0].path);
    expect(rs && rs->frames() == 200, "sf2 sample path resolves");
    Rig r("gl.multisampler", serializeZoneMap(m));
    r.gen->noteOn(1, 48, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 8000);
    expect(peak(L, 4000) > 0.05f && finite(L), "soundfont preset plays (looped)");
    r.gen->kill();
    r.gen->noteOn(2, 72, 1.f, 0);
    std::vector<float> L2, R2;
    r.run(L2, R2, 2000);
    expect(peak(L2, 0, 150) > 0.05f && peak(L2, 400) == 0.f, "unlooped soundfont zone ends");
  }

  // ---------------------------------------------------------------- extremes + block size
  {
    for (int ext = 0; ext < 2; ++ext) {
      Rig r("gl.multisampler");
      for (size_t k = 0; k < ms->params.size(); ++k) {
        const auto& d = ms->params[k];
        if (k == MsFactorySet) continue;
        r.set(static_cast<int>(k), static_cast<float>(ext ? d.max : d.min));
      }
      r.set(MsFilterType, ext ? FiltNotch : FiltLow);
      r.prepare();
      for (int k = 0; k < 6; ++k) r.gen->noteOn(static_cast<uint32_t>(k + 1), 0 + k * 25, 1.f, k * 10);
      std::vector<float> L, R;
      r.run(L, R, 20000);
      for (int k = 0; k < 6; ++k) r.gen->noteOff(static_cast<uint32_t>(k + 1), 0);
      r.run(L, R, 4000);
      expect(finite(L) && finite(R) && peak(L) < 50.f, ext ? "multisampler max params finite" : "multisampler min params finite");
    }
    auto renderSeq = [&](int block) {
      Rig r("gl.multisampler");
      r.set(MsFilterType, FiltLow);
      r.set(MsLfoCutoff, 1.f);
      r.set(MsLfoPitch, 30.f);
      r.set(MsEnvAmount, 2.f);
      r.set(MsGlide, 0.05f);
      std::vector<float> L, R;
      r.gen->noteOn(1, 60, 0.9f, 0);
      r.run(L, R, 3000, block);
      r.gen->noteOn(2, 67, 0.5f, 0);
      r.run(L, R, 3000, block);
      r.gen->noteOff(1, 0);
      r.run(L, R, 3000, block);
      return L;
    };
    auto a = renderSeq(64), b = renderSeq(1000);
    double diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(a.size() == b.size() && diff < 1e-6, "multisampler block-size invariant");
    Rig si("gl.multisampler");
    si.set(MsInterp, InterpSinc);
    si.gen->noteOn(1, 61, 1.f, 0);
    std::vector<float> L, R;
    si.run(L, R, 4000);
    expect(peak(L) > 0.05f && finite(L), "sinc interpolation sounds");
  }

  // ---------------------------------------------------------------- Scratch Sampler
  {
    ScratchMove m = factoryMove(0);
    expect(std::fabs(evalMove(m, 0) - 0.f) < 1e-6f && std::fabs(evalMove(m, 0.5f) - 1.f) < 1e-6f && evalMove(m, 0.25f) > 0.4f &&
               evalMove(m, 0.25f) < 0.6f,
           "baby move shape");
    ScratchData d;
    d.path = "/a/b c.wav";
    for (int k = 0; k < kMoves; ++k) d.moves[static_cast<size_t>(k)] = factoryMove(k);
    d.moves[3].pts = {{0, 0.2f, CurveLinear}, {0.4f, 0.9f, CurveHold}, {1, 0.1f, CurveEaseIn}};
    d.moves[3].mutes = 0x0F0F;
    ScratchData p = parseScratch(serializeScratch(d));
    expect(p.path == d.path && p.moves[3].pts.size() == 3 && p.moves[3].mutes == 0x0F0Fu && p.moves[3].pts[1].curve == CurveHold,
           "scratch data round trip");
    expect(parseScratch("/only/a/path.wav").path == "/only/a/path.wav", "scratch bare path");

    Rig r("gl.scratch");
    expect(!r.aux.samples.empty() && r.aux.samples[0], "scratch default phrase prepared");
    r.gen->noteOn(1, 60, 1.f, 0);
    std::vector<float> L, R;
    r.run(L, R, 44100);
    expect(peak(L) > 0.05f && finite(L), "scratch sounds after note on");
    r.gen->noteOff(1, 0);
    r.run(L, R, 22050);
    expect(!r.gen->active(), "scratch releases");
    // Position follows the move: a straight move over a ramp at 1 beat (0.5 s @120) reproduces the ramp.
    std::string longRamp = writeTestWav("long_ramp.wav", 44100, [](size_t i) { return static_cast<float>(i) / 44100.f; });
    Rig s("gl.scratch", longRamp);
    s.set(ScStart, 0), s.set(ScEnd, 1), s.set(ScSmoothing, 0), s.set(ScKeyMode, KeyMoves), s.set(ScRoot, 60), s.set(ScLength, 1);
    s.set(ScLoopMove, 0);
    s.gen->noteOn(1, 71, 1.f, 0);  // B = move 11 "Straight"
    std::vector<float> L2, R2;
    s.run(L2, R2, 30000);
    expect(std::fabs(L2[11025] / 0.8f - 0.5f) < 0.05f && L2[20000] > L2[10000], "scratch position follows the move");
    expect(peak(L2, 26000) < 1e-3f, "move without looping ends");
    // Mutes: Forward cut (move 2) is silent on the back stroke.
    Rig c("gl.scratch");
    c.set(ScSmoothing, 0);
    c.gen->noteOn(1, 62, 1.f, 0);  // D = move 2
    std::vector<float> L3, R3;
    c.run(L3, R3, 22050);
    expect(peak(L3, 0, 9000) > 0.01f && peak(L3, 12500, 21000) < 1e-3f, "mute lane silences steps");
    // Speed curve: "Straight" (0 -> 1) sweeps the platter from full reverse to double speed.
    Rig sp("gl.scratch", longRamp);
    sp.set(ScMotion, MotionSpeed), sp.set(ScStart, 0.25f), sp.set(ScEnd, 0.75f), sp.set(ScSmoothing, 0), sp.set(ScLength, 4);
    sp.gen->noteOn(1, 71, 1.f, 0);
    std::vector<float> L6, R6;
    sp.run(L6, R6, 88200);
    bool inRegion = true;
    for (size_t i = 400; i < L6.size(); ++i)
      if (L6[i] / 0.8f < 0.24f || L6[i] / 0.8f > 0.76f) inRegion = false;
    int down = 0, up = 0;
    for (size_t i = 1000; i < 2000; ++i) down += L6[i] < L6[i - 1];
    for (size_t i = 80000; i < 81000; ++i) up += L6[i] > L6[i - 1];
    expect(finite(L6) && inRegion && down > 900 && up > 900, "speed curve moves the head inside the region");
    for (int ext = 0; ext < 2; ++ext) {
      Rig e("gl.scratch");
      for (size_t k = 0; k < sc->params.size(); ++k) e.set(static_cast<int>(k), static_cast<float>(ext ? sc->params[k].max : sc->params[k].min));
      for (int k = 0; k < 4; ++k) e.gen->noteOn(static_cast<uint32_t>(k + 1), k * 40, 1.f, k);
      std::vector<float> L4, R4;
      e.run(L4, R4, 30000);
      expect(finite(L4) && finite(R4) && peak(L4) < 10.f, ext ? "scratch max params finite" : "scratch min params finite");
    }
    auto seq = [&](int block) {
      Rig q("gl.scratch");
      std::vector<float> L5, R5;
      q.gen->noteOn(1, 64, 1.f, 0);
      q.run(L5, R5, 5000, block);
      q.gen->noteOn(2, 65, 0.7f, 0);
      q.run(L5, R5, 5000, block);
      q.gen->noteOff(1, 0);
      q.run(L5, R5, 5000, block);
      return L5;
    };
    auto a = seq(64), b = seq(777);
    double diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
    expect(diff < 1e-6, "scratch block-size invariant");
  }
  return finish();
}
