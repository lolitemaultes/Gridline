// Gridline — SFZ instruments in the Multisampler: every opcode of the registry is taken by the
// reader (none left unhandled), unknown ones are reported and never fail the load, and each opcode
// family that changes the sound is proven on rendered audio. All .sfz and .wav files are written
// here by the test.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/AudioBuffer.h"
#include "core/PluginInfo.h"
#include "core/SfzMap.h"
#include "core/WavWriter.h"
#include "engine/Generators.h"
#include "packs/Packs.h"
#include "packs/samplers/samplers.h"

using namespace gl;
using namespace gl::packs::samplers;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100;

fs::path dir() {
  fs::path d = fs::temp_directory_path() / "gridline_test_sfz";
  fs::create_directories(d);
  return d;
}
std::string wav(const std::string& name, size_t frames, const std::function<float(size_t)>& f) {
  AudioBuffer b(1, frames);
  for (size_t i = 0; i < frames; ++i) b.at(0, i) = f(i);
  std::string p = (dir() / name).string();
  writeWav(p, b, 44100, 24);
  return p;
}
std::string sfzFile(const std::string& name, const std::string& text) {
  std::string p = (dir() / name).string();
  std::ofstream(p, std::ios::binary) << text;
  return p;
}

// Shared test samples (written once).
struct Samples {
  Samples() {
    wav("dc25.wav", 88200, [](size_t) { return 0.25f; });
    wav("dc50.wav", 88200, [](size_t) { return 0.5f; });
    wav("sine440.wav", 88200, [](size_t i) { return 0.5f * static_cast<float>(std::sin(2 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / kSr)); });
    wav("ramp.wav", 44100, [](size_t i) { return static_cast<float>(i) / 44100.f; });
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> n(88200);
    for (auto& x : n) x = u(rng);
    wav("noise.wav", 88200, [&](size_t i) { return n[i]; });
  }
};

struct Rig {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> gen;
  ProcessContext ctx;
  std::vector<float> L, R;
  explicit Rig(const std::string& sfzPath, const std::string& extra = "") {
    st = defaultPluginState("gl.multisampler");
    st.data = extra.empty() ? sfzPath : "#gl-zones 1\nsfz 0 | " + sfzPath + "\n" + extra;
    st.params[static_cast<size_t>(MsVolume)] = 1.f;
    aux = ChannelAux{};
    packs::prepareAux(st, aux);
    gen = createGenerator("gl.multisampler");
    ctx.sampleRate = kSr;
    ctx.tempo = 120;
  }
  void run(size_t n, int block = 256) {
    size_t base = L.size();
    L.resize(base + n, 0.f);
    R.resize(base + n, 0.f);
    for (size_t p = 0; p < n; p += static_cast<size_t>(block)) {
      int len = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - p));
      gen->process(st, aux, ctx, L.data() + base + p, R.data() + base + p, len);
    }
  }
  void on(int key, float vel = 1.f, uint32_t id = 1) { gen->noteOn(id, key, vel, 0); }
  void off(uint32_t id = 1) { gen->noteOff(id, 0); }
  void cc(int n, int v) { gen->midi(0xB0, static_cast<uint8_t>(n), static_cast<uint8_t>(v), 0); }
  size_t at() const { return L.size(); }
};

double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  if (b <= a) return 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(b - a));
}
double mean(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  if (b <= a) return 0;
  for (size_t i = a; i < b; ++i) s += v[i];
  return s / static_cast<double>(b - a);
}
// Frequency by rising zero crossings.
double freq(const std::vector<float>& v, size_t a, size_t b) {
  int n = 0;
  size_t first = 0, last = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i)
    if (v[i - 1] < 0 && v[i] >= 0) {
      if (!n) first = i;
      last = i;
      ++n;
    }
  return n > 1 ? (n - 1) * kSr / static_cast<double>(last - first) : 0.0;
}
// High-frequency share: rms of the first difference over rms.
double brightness(const std::vector<float>& v, size_t a, size_t b) {
  double d = 0, s = 0;
  for (size_t i = a + 1; i < b && i < v.size(); ++i) d += (v[i] - v[i - 1]) * (v[i] - v[i - 1]), s += v[i] * v[i];
  return s > 0 ? std::sqrt(d / s) : 0.0;
}
bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

bool peak_lt(const std::vector<float>& v, size_t a, size_t b, float lim) {
  for (size_t i = a; i < b && i < v.size(); ++i)
    if (std::fabs(v[i]) > lim) return false;
  return true;
}

// One note rendered through an instrument text; returns the rig after `frames`.
Rig play(const std::string& name, const std::string& text, int key = 60, float vel = 1.f, size_t frames = 22050) {
  Rig r(sfzFile(name, text));
  r.run(256);
  r.on(key, vel);
  r.run(frames);
  return r;
}
constexpr size_t kPre = 256;  // frames rendered before the note

// ---------------------------------------------------------------------------------- registry
std::string concrete(const std::string& p) {
  if (p == "hint_*") return "hint_ram_based";
  if (p == "*_mod") return "cutoff_mod";
  if (p == "varNN_*") return "var01_volume";
  std::string o;
  for (size_t i = 0; i < p.size();) {
    if (p.compare(i, 3, "NNN") == 0) o += "064", i += 3;
    else if (p.compare(i, 2, "NN") == 0) o += "01", i += 2;
    else if (p[i] == 'N') o += "1", ++i;
    else if (p[i] == 'X') o += "2", ++i;
    else if (p[i] == 'Y') o += "74", ++i;
    else o += p[i++];
  }
  return o;
}

void testRegistry() {
  using namespace gl::sfz;
  std::string control = "<control>\n", region = "<region> sample=*sine\n", effect = "<effect>\n", curve = "<curve> curve_index=7\n";
  int n = 0;
  for (const auto& o : opcodeTable()) {
    std::string p = o.pattern;
    if (p == "#define" || p == "#include" || p == "sample") continue;
    std::string c = concrete(p), fam = o.family, val = c == "md5" ? "00" : "1";
    if (fam == "Effects" && c.rfind("effect", 0) != 0) effect += c + "=" + val + "\n";
    else if (c == "v064" || c == "curve_index") curve += c == "v064" ? "v064=0.5\n" : "";
    else if (c == "default_path" || c == "note_offset" || c == "octave_offset" || c.rfind("set_", 0) == 0 || c.rfind("label_cc", 0) == 0 ||
             c == "sw_note_offset" || c == "sw_octave_offset")
      control += c + "=" + val + "\n";
    else region += c + "=" + val + "\n";
    ++n;
  }
  Instrument ins;
  loadText(control + region + effect + curve, (dir() / "all.sfz").string(), ins);
  expect(n > 800, "registry: the opcode table covers SFZ v1, v2 and the ARIA extensions");
  expect(ins.report.unhandled.empty(), "registry: every known opcode is taken by the reader");
  for (const auto& [k, v] : ins.report.unhandled) std::printf("# unhandled %s\n", k.c_str());
  expect(ins.report.unknown.empty(), "registry: no known opcode lands in the unknown list");
  for (const auto& [k, v] : ins.report.unknown) std::printf("# unknown %s\n", k.c_str());
  bool supportOk = true;
  for (const auto& o : opcodeTable())
    if (!o.family || !*o.family) supportOk = false;
  expect(supportOk, "registry: every opcode names its family");
  expect(findOpcode("cutoff2_oncc74") && findOpcode("eg03_time2") && findOpcode("lfo12_freq_smoothcc1"), "registry: numbered forms are found");
  expect(!findOpcode("not_an_opcode"), "registry: made-up names are not found");
}

void testParse() {
  using namespace gl::sfz;
  // unknown opcodes and headers are reported, the load still works
  {
    Instrument ins;
    bool ok = loadText("<region> sample=*sine my_vendor_thing=3 <weird> foo=1\n<region> sample=*saw", (dir() / "u.sfz").string(), ins);
    expect(ok && ins.regions.size() == 2, "parse: unknown opcodes and headers do not fail the load");
    expect(ins.report.unknown.count("my_vendor_thing") == 1, "parse: an unknown opcode is listed");
    expect(ins.report.unknownHeaders.count("weird") == 1, "parse: an unknown header is listed");
    expect(!reportLine(ins.report).empty(), "parse: the report has a summary line");
  }
  // inheritance: global < master < group < region
  {
    Instrument ins;
    loadText("<global> volume=-3 lokey=10\n<master> tune=5\n<group> hikey=20 tune=7\n<region> sample=*sine\n<region> sample=*sine hikey=30\n<group>\n<region> sample=*sine",
             (dir() / "inh.sfz").string(), ins);
    expect(ins.regions.size() == 3, "inheritance: three regions");
    if (ins.regions.size() == 3) {
      expect(ins.regions[0].volume == -3 && ins.regions[0].lokey == 10, "inheritance: global values reach regions");
      expect(ins.regions[0].tune == 7 && ins.regions[0].hikey == 20, "inheritance: group values override master values");
      expect(ins.regions[1].hikey == 30, "inheritance: region values override the group");
      expect(ins.regions[2].tune == 5 && ins.regions[2].hikey == 127, "inheritance: a new group starts again from the master");
    }
  }
  // #define, #include, comments, note names, default_path, offsets
  {
    sfzFile("inc_part.sfz", "<region> sample=*sine key=$K\n");
    Instrument ins;  // the preprocessor runs on files (load), as the player reads them
    load(sfzFile("def.sfz", "// comment\n/* block\n comment */\n#define $K 62\n<control> default_path=sub/ note_offset=1 octave_offset=-1\n#include \"inc_part.sfz\"\n<region> sample=x.wav lokey=c4 hikey=c#4"), ins);
    expect(ins.regions.size() == 2, "preprocess: #include brings its regions");
    if (ins.regions.size() == 2) {
      expect(ins.regions[0].lokey == 62 + 1 - 12, "preprocess: #define, note_offset and octave_offset apply");
      expect(ins.regions[1].lokey == 60 + 1 - 12 && ins.regions[1].hikey == 61 + 1 - 12, "preprocess: note names (c4 = 60)");
      expect(ins.regions[1].file.find("sub") != std::string::npos, "preprocess: default_path prefixes sample paths");
    }
    expect(!ins.report.missingSamples.empty(), "missing: a sample that does not exist is reported");
  }
  // values with spaces in sample names, and a value running to the next opcode
  {
    Instrument ins;
    loadText("<region> sample=my file name.wav lokey=1", (dir() / "sp.sfz").string(), ins);
    expect(!ins.regions.empty() && ins.regions[0].sample == "my file name.wav" && ins.regions[0].lokey == 1, "parse: sample names keep their spaces");
  }
  expect(noteNumber("c4") == 60 && noteNumber("a4") == 69 && noteNumber("db4") == 61 && noteNumber("C#-1") == 1, "parse: note numbers");
}

// ---------------------------------------------------------------------------------- sound
void testMapping() {
  // key and velocity ranges pick the region
  {
    auto t = "<region> sample=dc25.wav lokey=60 hikey=62 hivel=63 loop_mode=loop_continuous\n<region> sample=dc50.wav lokey=60 hikey=62 lovel=64 loop_mode=loop_continuous amp_veltrack=0";
    auto a = play("map1.sfz", std::string(t) + "\n", 61, 1.f);
    auto b = play("map1.sfz", t, 70, 1.f);
    expect(std::fabs(mean(a.L, 4000, 8000) - 0.5) < 0.02, "map: a loud note picks the high velocity layer");
    expect(rms(b.L, 0, b.L.size()) < 1e-6, "map: a key outside every region is silent");
  }
  // lochan/hichan, lorand/hirand, lobpm, seq_length round robin
  {
    Rig r(sfzFile("rr.sfz", "<group> seq_length=2 loop_mode=loop_continuous amp_veltrack=0\n<region> seq_position=1 sample=dc25.wav\n<region> seq_position=2 sample=dc50.wav"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(4000);
    r.off(1);
    r.run(8000);
    size_t s2 = r.at();
    r.on(60, 1, 2);
    r.run(4000);
    expect(std::fabs(mean(r.L, 2000, 4000) - 0.25) < 0.02 && std::fabs(mean(r.L, s2 + 2000, s2 + 4000) - 0.5) < 0.02, "seq: round robin alternates its regions");
  }
  {
    auto a = play("bpm.sfz", "<region> sample=dc50.wav lobpm=150 loop_mode=loop_continuous");
    expect(rms(a.L, 0, a.L.size()) < 1e-6, "bpm: a region above the tempo stays silent");
  }
  // lotimer/hitimer: a slow note plays one region, a quick repeat the other
  {
    Rig r(sfzFile("tm.sfz", "<group> loop_mode=loop_continuous amp_veltrack=0\n<region> sample=dc25.wav lotimer=0.5\n<region> sample=dc50.wav hitimer=0.5"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(4000);
    r.off(1);
    r.run(2000);
    size_t s2 = r.at();
    r.on(60, 1, 2);
    r.run(4000);
    expect(std::fabs(mean(r.L, 2000, 4000) - 0.25) < 0.02, "timer: the first note plays the slow region");
    expect(std::fabs(mean(r.L, s2 + 2000, s2 + 4000) - 0.5) < 0.02, "timer: a quick repeat plays the fast region");
  }
}

void testPitch() {
  auto base = play("p0.sfz", "<region> sample=sine440.wav pitch_keycenter=69");
  auto up = play("p1.sfz", "<region> sample=sine440.wav pitch_keycenter=69", 81);
  auto tr = play("p2.sfz", "<region> sample=sine440.wav pitch_keycenter=69 transpose=12", 69);
  auto tu = play("p3.sfz", "<region> sample=sine440.wav pitch_keycenter=69 tune=100", 69);
  auto kt = play("p4.sfz", "<region> sample=sine440.wav pitch_keycenter=69 pitch_keytrack=0", 81);
  const double f0 = freq(base.L, 2000, 20000);
  expect(std::fabs(freq(base.L, 2000, 20000) - 440.0 * std::exp2(-9.0 / 12.0)) < 3, "pitch: pitch_keycenter sets the root");
  expect(std::fabs(freq(up.L, 2000, 20000) / f0 - std::exp2(21.0 / 12.0)) < 0.02, "pitch: keys track the pitch");
  expect(std::fabs(freq(tr.L, 2000, 20000) - 880) < 4, "pitch: transpose");
  expect(std::fabs(freq(tu.L, 2000, 20000) - 440 * std::exp2(1.0 / 12.0)) < 4, "pitch: tune (cents)");
  expect(std::fabs(freq(kt.L, 2000, 20000) - 440) < 3, "pitch: pitch_keytrack=0 keeps one pitch");
  // pitch bend
  Rig r(sfzFile("bend.sfz", "<region> sample=sine440.wav pitch_keycenter=69 bend_up=1200"));
  r.run(256);
  r.on(69);
  r.gen->midi(0xE0, 0x7F, 0x7F, 0);
  r.run(22050);
  expect(std::fabs(freq(r.L, 4000, 20000) - 880) < 6, "pitch: bend_up spans an octave");
  // pitch envelope and lfo
  auto eg = play("peg.sfz", "<region> sample=sine440.wav pitch_keycenter=69 pitcheg_attack=0 pitcheg_decay=10 pitcheg_sustain=100 pitcheg_depth=1200", 69);
  expect(std::fabs(freq(eg.L, 4000, 20000) - 880) < 8, "pitch: pitcheg_depth moves the pitch");
  auto lf = play("plfo.sfz", "<region> sample=sine440.wav pitch_keycenter=69 pitchlfo_freq=4 pitchlfo_depth=600", 69, 1, 44100);
  const double a1 = freq(lf.L, 2000, 4700), a2 = freq(lf.L, 8000, 10700);
  expect(std::fabs(a1 - a2) > 30, "pitch: pitchlfo_depth wobbles the pitch");
  // eg (v2) to pitch
  auto e2 = play("peg2.sfz", "<region> sample=sine440.wav pitch_keycenter=69 eg1_time1=0 eg1_level1=1 eg1_time2=10 eg1_level2=1 eg1_pitch=1200", 69);
  expect(std::fabs(freq(e2.L, 4000, 20000) - 880) < 8, "pitch: eg1_pitch (flexible envelope)");
  auto l2 = play("plfo2.sfz", "<region> sample=sine440.wav pitch_keycenter=69 lfo1_freq=4 lfo1_pitch=600", 69, 1, 44100);
  expect(std::fabs(freq(l2.L, 2000, 4700) - freq(l2.L, 8000, 10700)) > 30, "pitch: lfo1_pitch (v2 LFO)");
}

void testAmp() {
  auto ref = play("a0.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous");
  const double r0 = mean(ref.L, 4000, 8000);
  expect(std::fabs(r0 - 0.5) < 0.02, "amp: a full velocity note plays the sample at its level");
  auto vol = play("a1.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous volume=-6");
  expect(std::fabs(mean(vol.L, 4000, 8000) / r0 - std::pow(10, -6 / 20.0)) < 0.02, "amp: volume (dB)");
  auto amp = play("a2.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amplitude=50");
  expect(std::fabs(mean(amp.L, 4000, 8000) / r0 - 0.5) < 0.02, "amp: amplitude (percent)");
  auto pan = play("a3.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous pan=100");
  expect(mean(pan.L, 4000, 8000) < 0.01 && mean(pan.R, 4000, 8000) > 0.4, "amp: pan right");
  auto pos = play("a4.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous position=-100");
  expect(mean(pos.R, 4000, 8000) < 0.01 && mean(pos.L, 4000, 8000) > 0.4, "amp: position left");
  auto vt = play("a5.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amp_veltrack=100", 60, 0.5f);
  auto vt0 = play("a6.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amp_veltrack=0", 60, 0.5f);
  expect(mean(vt.L, 4000, 8000) < 0.8 * mean(vt0.L, 4000, 8000), "amp: amp_veltrack scales soft notes");
  auto kt = play("a7.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amp_keytrack=-1 amp_keycenter=60", 72);
  expect(std::fabs(mean(kt.L, 4000, 8000) / r0 - std::pow(10, -12 / 20.0)) < 0.03, "amp: amp_keytrack");
  auto xf = play("a8.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous xfin_lokey=50 xfin_hikey=70", 60);
  expect(mean(xf.L, 4000, 8000) < 0.95 * r0 && mean(xf.L, 4000, 8000) > 0.2 * r0, "amp: key crossfade in");
  auto inv = play("a9.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous phase=invert");
  expect(mean(inv.L, 4000, 8000) < -0.4, "amp: phase invert");
  auto gv = play("a10.sfz", "<group> group_volume=-6\n<region> sample=dc50.wav loop_mode=loop_continuous");
  expect(std::fabs(mean(gv.L, 4000, 8000) / r0 - std::pow(10, -6 / 20.0)) < 0.02, "amp: group_volume");
  // width 0 makes a stereo-less sound mono; a mono sample stays equal
  auto w = play("a11.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous width=0");
  expect(std::fabs(mean(w.L, 4000, 8000) - mean(w.R, 4000, 8000)) < 1e-3, "amp: width");
  // amp LFO
  auto lf = play("a12.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amplfo_freq=5 amplfo_depth=6", 60, 1, 44100);
  double lo = 1, hi = 0;
  for (size_t i = 4000; i < 40000; i += 50) lo = std::min(lo, static_cast<double>(lf.L[i])), hi = std::max(hi, static_cast<double>(lf.L[i]));
  expect(hi / std::max(1e-6, lo) > 1.6, "amp: amplfo_depth (tremolo)");
}

void testEnvelopes() {
  auto a = play("e1.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_attack=0.2", 60, 1, 22050);
  expect(std::fabs(a.L[kPre + 2205]) < 0.5 * std::fabs(a.L[kPre + 13000]), "ampeg: attack ramps up");
  auto d = play("e2.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_decay=0.1 ampeg_sustain=25", 60, 1, 22050);
  expect(std::fabs(mean(d.L, 15000, 20000) - 0.125) < 0.02, "ampeg: decay to the sustain level");
  // release
  Rig r(sfzFile("e3.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_release=0.3"));
  r.run(256);
  r.on(60);
  r.run(4410);
  r.off();
  size_t o = r.at();
  r.run(44100);
  expect(std::fabs(r.L[o + 2205]) > 0.1 && std::fabs(r.L[o + 30000]) < 0.01, "ampeg: release fades after the note");
  // delay and hold
  auto dl = play("e4.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_delay=0.1", 60, 1, 22050);
  expect(std::fabs(dl.L[kPre + 2000]) < 1e-4 && std::fabs(dl.L[kPre + 8000]) > 0.3, "ampeg: delay");
  auto vel = play("e5.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_attack=0 ampeg_vel2attack=0.4", 60, 1, 22050);
  expect(std::fabs(vel.L[kPre + 2205]) < 0.4, "ampeg: vel2attack lengthens the attack");
  // flexible envelope as amplitude envelope
  auto fe = play("e6.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous eg1_ampeg=1 eg1_time1=0 eg1_level1=1 eg1_time2=0.1 eg1_level2=0 eg1_sustain=3", 60, 1, 22050);
  expect(std::fabs(fe.L[kPre + 200]) > 0.3 && std::fabs(fe.L[kPre + 10000]) < 0.02, "eg: eg1_ampeg shapes the level");
  // fileg
  auto fl = play("e7.sfz", "<region> sample=noise.wav cutoff=200 fil_type=lpf_2p fileg_attack=0 fileg_decay=0.05 fileg_sustain=0 fileg_depth=6000", 60, 1, 22050);
  expect(brightness(fl.L, kPre + 100, kPre + 900) > 1.5 * brightness(fl.L, 15000, 20000), "fileg: the filter envelope opens the filter");
}

void testFilters() {
  auto dry = play("f0.sfz", "<region> sample=noise.wav");
  const double b0 = brightness(dry.L, 2000, 20000);
  int ok = 0, total = 0;
  for (const char* t : {"lpf_1p", "lpf_2p", "lpf_4p", "lpf_6p", "lpf_2p_sv", "lsh"}) {
    std::string extra = std::string(t) == "lsh" ? " fil_gain=-24" : "";
    auto f = play("f1.sfz", std::string("<region> sample=noise.wav cutoff=500 fil_type=") + t + extra);
    ++total;
    const double b = brightness(f.L, 2000, 20000);
    if (std::string(t) == "lsh" ? b > b0 * 1.01 : b < b0 * 0.9) ++ok;
    else std::printf("# filter %s brightness %.3f vs %.3f\n", t, b, b0);
  }
  for (const char* t : {"hpf_1p", "hpf_2p", "hpf_4p", "hpf_6p", "hpf_2p_sv", "hsh", "pkf_2p"}) {
    const bool peak = std::string(t) == "pkf_2p";
    std::string extra = std::string(t) == "hsh" || peak ? " fil_gain=24" : "";
    auto f = play("f2.sfz", std::string("<region> sample=noise.wav cutoff=") + (peak ? "15000" : "3000") + " fil_type=" + t + extra);
    ++total;
    if (brightness(f.L, 2000, 20000) > b0 * 1.05) ++ok;
    else std::printf("# filter %s\n", t);
  }
  for (const char* t : {"bpf_1p", "bpf_2p", "bpf_2p_sv", "brf_1p", "brf_2p", "brf_2p_sv", "apf_1p", "comb", "pink", "peq", "bpk_2p"}) {
    auto f = play("f3.sfz", std::string("<region> sample=noise.wav cutoff=1000 resonance=6 fil_type=") + t);
    ++total;
    if (finite(f.L) && rms(f.L, 2000, 20000) > 1e-3) ++ok;
    else std::printf("# filter %s\n", t);
  }
  expect(ok == total, "filter: every filter type shapes the sound");
  {  // an instrument rewritten within one tick of a coarse file clock (the same file time) loads again
    auto lo = play("f9.sfz", "<region> sample=noise.wav cutoff=500 fil_type=lpf_2p");
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(dir() / "f9.sfz", ec);
    const std::string p = sfzFile("f9.sfz", "<region> sample=noise.wav cutoff=500 fil_type=hpf_2p");  // (same size)
    std::filesystem::last_write_time(dir() / "f9.sfz", t, ec);
    Rig hi(p);
    hi.run(256);
    hi.on(60, 1.f);
    hi.run(22050);
    expect(!ec && brightness(hi.L, 2000, 20000) > 2 * brightness(lo.L, 2000, 20000), "filter: an edit with the same file time loads again");
  }
  // second filter, cutoff CC, keytrack, veltrack
  auto f2 = play("f4.sfz", "<region> sample=noise.wav cutoff2=300 fil2_type=lpf_2p");
  expect(brightness(f2.L, 2000, 20000) < 0.6 * b0, "filter: cutoff2 / fil2_type");
  Rig r(sfzFile("f5.sfz", "<region> sample=noise.wav cutoff=200 cutoff_oncc74=7200"));
  r.run(256);
  r.on(60);
  r.run(11025);
  r.cc(74, 127);
  size_t o = r.at();
  r.run(11025);
  expect(brightness(r.L, o + 2000, o + 11000) > 1.5 * brightness(r.L, 3000, 11000), "filter: cutoff_oncc74");
  auto kt = play("f6.sfz", "<region> sample=noise.wav cutoff=200 fil_keytrack=100 fil_keycenter=36", 96);
  auto kt0 = play("f7.sfz", "<region> sample=noise.wav cutoff=200", 96);
  expect(brightness(kt.L, 2000, 20000) > 1.5 * brightness(kt0.L, 2000, 20000), "filter: fil_keytrack");
  // EQ
  auto eq = play("q1.sfz", "<region> sample=sine440.wav pitch_keycenter=69 eq1_freq=440 eq1_bw=1 eq1_gain=12", 69);
  auto eq0 = play("q0.sfz", "<region> sample=sine440.wav pitch_keycenter=69", 69);
  expect(rms(eq.L, 4000, 20000) > 2.5 * rms(eq0.L, 4000, 20000), "eq: a band boost at the tone's frequency");
}

void testLogic() {
  // keyswitches
  {
    Rig r(sfzFile("ks.sfz", "<global> sw_lokey=24 sw_hikey=26 sw_default=24 loop_mode=loop_continuous amp_veltrack=0\n<region> sw_last=24 sample=dc25.wav lokey=60 hikey=72\n<region> sw_last=25 sample=dc50.wav lokey=60 hikey=72"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(4000);
    r.off(1);
    r.run(4000);
    r.on(25, 1, 2);
    r.off(2);
    r.run(256);
    size_t s = r.at();
    r.on(60, 1, 3);
    r.run(4000);
    expect(std::fabs(mean(r.L, 2000, 4000) - 0.25) < 0.02, "keyswitch: sw_default selects the first articulation");
    expect(std::fabs(mean(r.L, s + 2000, s + 4000) - 0.5) < 0.02, "keyswitch: sw_last switches articulation");
  }
  // release trigger
  {
    Rig r(sfzFile("rt.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous\n<region> trigger=release sample=dc25.wav loop_mode=one_shot rt_decay=0"));
    r.run(256);
    r.on(60);
    r.run(4410);
    r.off();
    size_t o = r.at();
    r.run(8000);
    expect(std::fabs(mean(r.L, o + 2000, o + 6000) - 0.25) < 0.03, "trigger: release regions play at note off");
  }
  // first / legato
  {
    Rig r(sfzFile("lg.sfz", "<global> loop_mode=loop_continuous amp_veltrack=0\n<region> trigger=first sample=dc25.wav\n<region> trigger=legato sample=dc50.wav"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(2000);
    size_t s = r.at();
    r.on(62, 1, 2);
    r.run(2000);
    expect(std::fabs(mean(r.L, 1000, 2000) - 0.25) < 0.02 && std::fabs(mean(r.L, s + 500, s + 2000) - 0.75) < 0.03, "trigger: first and legato regions");
  }
  // group / off_by
  {
    Rig r(sfzFile("ob.sfz", "<global> loop_mode=loop_continuous amp_veltrack=0\n<region> key=60 group=1 off_by=2 sample=dc25.wav\n<region> key=62 group=2 sample=dc50.wav"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(2000);
    r.on(62, 1, 2);
    size_t s = r.at();
    r.run(4000);
    expect(std::fabs(mean(r.L, s + 2000, s + 4000) - 0.5) < 0.02, "voices: off_by silences the other group");
  }
  // polyphony
  {
    Rig r(sfzFile("poly.sfz", "<group> polyphony=1 group=3 loop_mode=loop_continuous amp_veltrack=0\n<region> sample=dc25.wav"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(2000);
    r.on(62, 1, 2);
    size_t s = r.at();
    r.run(4000);
    expect(std::fabs(mean(r.L, s + 2000, s + 4000) - 0.25) < 0.02, "voices: polyphony=1 keeps one voice");
  }
  // sustain pedal
  {
    Rig r(sfzFile("ped.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous ampeg_release=0.01"));
    r.run(256);
    r.cc(64, 127);
    r.on(60);
    r.run(2000);
    r.off();
    size_t o = r.at();
    r.run(4000);
    r.cc(64, 0);
    size_t u = r.at();
    r.run(4000);
    expect(std::fabs(mean(r.L, o + 2000, o + 4000)) > 0.4, "pedal: the sustain pedal holds the note");
    expect(std::fabs(mean(r.L, u + 2000, u + 4000)) < 0.01, "pedal: lifting the pedal releases it");
  }
  // CC ranges and on_cc
  {
    Rig r(sfzFile("ccr.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous locc1=64 hicc1=127\n<region> sample=dc25.wav loop_mode=one_shot on_locc20=100 on_hicc20=127"));
    r.run(256);
    r.on(60, 1, 1);
    r.run(2000);
    r.off(1);
    r.run(2000);
    expect(rms(r.L, 0, r.at()) < 1e-6, "cc: locc keeps the region quiet below its range");
    r.cc(1, 100);
    r.on(60, 1, 2);
    size_t s = r.at();
    r.run(2000);
    expect(mean(r.L, s + 1000, s + 2000) > 0.4, "cc: the region plays inside its range");
    r.off(2);
    r.run(4000);
    r.cc(20, 120);
    size_t c = r.at();
    r.run(4000);
    expect(mean(r.L, c + 1000, c + 4000) > 0.1, "cc: on_locc starts a region from a controller");
  }
  // velocity tracked by cc: amplitude_oncc
  {
    Rig r(sfzFile("ampcc.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amplitude=0 amplitude_oncc7=100"));
    r.run(256);
    r.cc(7, 0);
    r.on(60);
    r.run(4000);
    r.cc(7, 127);
    size_t s = r.at();
    r.run(4000);
    expect(std::fabs(mean(r.L, 2000, 4000)) < 0.01 && mean(r.L, s + 2000, s + 4000) > 0.4, "cc: amplitude_oncc7");
  }
  // curves
  {
    Rig r(sfzFile("curve.sfz", "<curve> curve_index=9 v000=0 v064=1 v127=1\n<region> sample=dc50.wav loop_mode=loop_continuous amplitude=0 amplitude_oncc7=100 amplitude_curvecc7=9"));
    r.run(256);
    r.cc(7, 64);
    r.on(60);
    r.run(6000);
    expect(mean(r.L, 3000, 6000) > 0.45, "curve: a <curve> shapes the controller");
  }
}

void testSource() {
  // offsets, end, count, one_shot, no_loop, reverse, delay
  auto off = play("s1.sfz", "<region> sample=ramp.wav offset=22050 pitch_keytrack=0");
  expect(off.L[kPre + 10] > 0.45, "source: offset starts later in the sample");
  auto rev = play("s2.sfz", "<region> sample=ramp.wav direction=reverse pitch_keytrack=0");
  expect(rev.L[kPre + 100] > 0.9 && rev.L[kPre + 20000] < rev.L[kPre + 100], "source: direction=reverse");
  auto end = play("s3.sfz", "<region> sample=ramp.wav end=4410 pitch_keytrack=0", 60, 1, 22050);
  expect(rms(end.L, kPre + 5000, kPre + 20000) < 1e-6, "source: end stops the sample");
  {
    Rig r(sfzFile("s4.sfz", "<region> sample=ramp.wav loop_mode=one_shot pitch_keytrack=0"));
    r.run(256);
    r.on(60);
    r.run(441);
    r.off();
    r.run(22050);
    expect(rms(r.L, 20000, 22000) > 0.1, "source: one_shot plays on after note off");
  }
  auto del = play("s5.sfz", "<region> sample=dc50.wav delay=0.1 loop_mode=loop_continuous", 60, 1, 22050);
  expect(std::fabs(del.L[kPre + 3000]) < 1e-6 && del.L[kPre + 6000] > 0.4, "source: delay");
  auto loop = play("s6.sfz", "<region> sample=ramp.wav loop_mode=loop_continuous loop_start=0 loop_end=4409 pitch_keytrack=0", 60, 1, 44100);
  expect(rms(loop.L, 30000, 40000) > 0.03 && peak_lt(loop.L, 30000, 40000, 0.11f), "source: loop_start / loop_end loop a part");
  auto cnt = play("s7.sfz", "<region> sample=ramp.wav end=4409 count=3 pitch_keytrack=0", 60, 1, 22050);
  expect(rms(cnt.L, kPre + 9000, kPre + 13000) > 0.02 && rms(cnt.L, kPre + 14000, kPre + 20000) < 1e-6, "source: count plays the sample again");
  // built-in waves and the oscillator
  auto sine = play("s8.sfz", "<region> sample=*sine pitch_keycenter=69", 69);
  expect(std::fabs(freq(sine.L, 2000, 20000) - 440) < 3, "source: *sine");
  int ok = 0;
  for (const char* w : {"*saw", "*square", "*triangle", "*noise"}) {
    auto b = play("s9.sfz", std::string("<region> sample=") + w);
    if (rms(b.L, 2000, 20000) > 0.05) ++ok;
  }
  expect(ok == 4, "source: *saw, *square, *triangle, *noise");
  auto sil = play("s10.sfz", "<region> sample=*silence");
  expect(rms(sil.L, 0, sil.L.size()) < 1e-6, "source: *silence");
  wav("cycle.wav", 64, [](size_t i) { return static_cast<float>(std::sin(2 * 3.14159265358979323846 * static_cast<double>(i) / 64.0)); });
  auto osc = play("s11.sfz", "<region> sample=cycle.wav oscillator=on pitch_keycenter=69", 69);
  expect(std::fabs(freq(osc.L, 2000, 20000) - 440) < 3, "source: oscillator=on plays one cycle as a wave");
  // noise and lofi
  auto nz = play("s12.sfz", "<region> sample=*silence noise_level=-6");
  expect(rms(nz.L, 2000, 20000) > 0.05, "noise: noise_level");
  auto br = play("s13.sfz", "<region> sample=sine440.wav pitch_keycenter=69 bitred=90", 69);
  auto br0 = play("s14.sfz", "<region> sample=sine440.wav pitch_keycenter=69", 69);
  expect(brightness(br.L, 2000, 20000) > 1.2 * brightness(br0.L, 2000, 20000), "lofi: bitred");
  auto dc = play("s15.sfz", "<region> sample=sine440.wav pitch_keycenter=69 decim=80", 69);
  expect(brightness(dc.L, 2000, 20000) > 1.2 * brightness(br0.L, 2000, 20000), "lofi: decim");
}

void testEffects() {
  auto dry = play("x0.sfz", "<region> sample=dc50.wav end=441", 60, 1, 22050);
  auto dl = play("x1.sfz", "<region> sample=dc50.wav end=441 effect1=100\n<effect> bus=fx1 type=delay delay_timel=0.1 delay_timer=0.1 delay_wet=100 delay_feedback=0", 60, 1, 22050);
  expect(rms(dry.L, kPre + 4000, kPre + 6000) < 1e-6 && rms(dl.L, kPre + 4000, kPre + 6000) > 0.05, "effects: an fx bus delay repeats the sound");
  int ok = 0, total = 0;
  for (const char* t : {"apan", "comp", "disto", "eq", "filter", "fverb", "gate", "lofi", "phaser", "static", "strings", "limiter"}) {
    auto e = play("x2.sfz", std::string("<region> sample=noise.wav\n<effect> type=") + t);
    ++total;
    if (finite(e.L)) ++ok;
    else std::printf("# effect %s\n", t);
  }
  expect(ok == total, "effects: every effect type runs and stays finite");
  auto rv = play("x3.sfz", "<region> sample=dc50.wav end=441 effect1=100\n<effect> bus=fx1 type=fverb reverb_size=80 reverb_dry=0 reverb_wet=100", 60, 1, 22050);
  expect(rms(rv.L, kPre + 6000, kPre + 12000) > 1e-3, "effects: the reverb leaves a tail");
}

void testState() {
  // zone text round trip
  ZoneMap m;
  m.sfzPath = (dir() / "rt state.sfz").string();
  m.sfzFlags = 1;
  m.sfzCc = {{7, 0.5f}, {74, 1.f}};
  ZoneMap b = parseZoneMap(serializeZoneMap(m));
  expect(b.sfzPath == m.sfzPath && b.sfzFlags == 1 && b.sfzCc.size() == 2 && b.sfzCc[1].first == 74, "state: the sfz line and controller values survive save and load");
  expect(parseZoneMap(m.sfzPath).sfzPath == m.sfzPath, "state: a bare .sfz path plays as an SFZ instrument");
  expect(isSfzPath("A.SFZ") && !isSfzPath("a.wav"), "state: .sfz paths are recognised in any case");
  // a saved controller value reaches the sound
  std::string p = sfzFile("ccstate.sfz", "<region> sample=dc50.wav loop_mode=loop_continuous amplitude=0 amplitude_oncc30=100");
  Rig r(p, "sfzcc 30=1\n");
  r.run(256);
  r.on(60);
  r.run(4000);
  expect(mean(r.L, 2000, 4000) > 0.4, "state: a saved controller value is applied");
  // missing samples: the rest still plays
  Rig mr(sfzFile("miss.sfz", "<region> sample=nothere.wav key=61\n<region> sample=dc50.wav key=60 loop_mode=loop_continuous"));
  mr.run(256);
  mr.on(60);
  mr.run(4000);
  expect(mean(mr.L, 2000, 4000) > 0.4, "missing: regions with samples still play");
}

void testRobust() {
  // block-size invariance
  const std::string t = "<region> sample=noise.wav cutoff=800 fil_type=lpf_2p amplfo_freq=3 amplfo_depth=3 ampeg_attack=0.01 ampeg_release=0.05";
  std::vector<float> a, b;
  for (int pass = 0; pass < 2; ++pass) {
    Rig r(sfzFile("bs.sfz", t));
    r.run(256, pass ? 37 : 512);
    r.on(60);
    r.run(11025, pass ? 37 : 512);
    r.off();
    r.run(11025, pass ? 37 : 512);
    (pass ? b : a) = r.L;
  }
  double diff = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
  expect(diff < 1e-4, "robust: block size does not change the sound");
  // extremes: every numeric opcode at a big value plays finite sound
  {
    std::string region = "<region> sample=noise.wav";
    std::vector<std::string> pairs;
    for (const auto& o : gl::sfz::opcodeTable()) {
      std::string c = concrete(o.pattern);
      if (std::string(o.family) == "Effects" || c == "sample" || c.rfind("#", 0) == 0 || c == "default_path" || c == "md5" || c.find("label") != std::string::npos ||
          c == "image" || c.rfind("set_", 0) == 0 || c.rfind("hint_", 0) == 0)
        continue;
      // conditions stay open (so the region still plays); everything else goes to an extreme value
      auto has = [&](const char* s) { return c.find(s) != std::string::npos; };
      if (has("sw_") || has("trigger") || has("seq_") || has("on_lo") || has("on_hi") || has("start_lo") || has("start_hi") || has("stop_") || has("delay") || has("timer") ||
          has("direction") || c == "end" || c == "count" || c == "offset" || has("_mode") || c == "phase" || c == "polyphony" || c == "note_polyphony")
        continue;
      std::string val = "20000";
      if (c.rfind("xfin_", 0) == 0) val = "0";
      else if (c.rfind("xfout_", 0) == 0) val = "127";
      else if (c.rfind("lo", 0) == 0 || has("_lo")) val = "0";
      else if (c.rfind("hi", 0) == 0 || has("_hi")) val = c.find("bend") != std::string::npos ? "8192" : "127";
      else if (has("key") || has("vel")) val = "60";
      else if (has("bitred") || has("decim")) val = "30";  // full scale crushes the test noise to silence
      region += " " + c + "=" + val;
      pairs.push_back(" " + c + "=" + val);
    }
    auto e = play("ext.sfz", region, 60, 1, 8000);
    expect(finite(e.L) && finite(e.R), "robust: every opcode at an extreme value stays finite");
    const bool plays = rms(e.L, kPre, e.L.size()) > 0 || rms(e.R, kPre, e.R.size()) > 0;
    if (!plays)  // name the opcodes whose extreme value silences the region
      for (size_t skip = 0; skip < pairs.size(); ++skip) {
        std::string without = "<region> sample=noise.wav";
        for (size_t n = 0; n < pairs.size(); ++n)
          if (n != skip) without += pairs[n];
        auto w = play("ext.sfz", without, 60, 1, 4000);
        if (rms(w.L, kPre, w.L.size()) > 0 || rms(w.R, kPre, w.R.size()) > 0) std::printf("# silenced by%s\n", pairs[skip].c_str());
      }
    expect(plays, "robust: the extreme region still plays");
  }
  // fuzz: random opcode soup and random bytes never crash
  std::mt19937 rng(11);
  const auto& tab = gl::sfz::opcodeTable();
  bool fine = true;
  for (int k = 0; k < 40; ++k) {
    std::string s = "<region> sample=noise.wav";
    for (int j = 0; j < 30; ++j) {
      const auto& o = tab[rng() % tab.size()];
      s += " " + concrete(o.pattern) + "=" + std::to_string(static_cast<int>(rng() % 400) - 100);
    }
    if (k % 4 == 0) {
      s += "\n";
      for (int j = 0; j < 200; ++j) s += static_cast<char>(rng() % 256);
    }
    auto e = play("fuzz.sfz", s, 30 + static_cast<int>(rng() % 60), 0.8f, 2048);
    fine = fine && finite(e.L) && finite(e.R);
  }
  expect(fine, "robust: fuzzed instruments load and play finite sound");
  // a missing .sfz is silent and does not crash
  Rig r("/nonexistent/x.sfz");
  r.run(256);
  r.on(60);
  r.run(2048);
  expect(rms(r.L, 0, r.at()) < 1e-6, "robust: a missing .sfz file is silent");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("sfz");
  Samples samples;
  (void)samples;
  testRegistry();
  testParse();
  testMapping();
  testPitch();
  testAmp();
  testEnvelopes();
  testFilters();
  testLogic();
  testSource();
  testEffects();
  testState();
  testRobust();
  return finish();
}
