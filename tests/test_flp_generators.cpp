// Gridline — FLP import: generator (instrument) decoders, on plugin states
// built byte by byte here from the documented layouts (no FL Studio content).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/PluginParams.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/ChannelIns.h"
#include "../src/engine/Render.h"
#include "../src/packs/models/models.h"
#include "../src/packs/modulation/modulation.h"
#include "../src/packs/samplers/dwave.h"
#include "../src/packs/samplers/samplers.h"
#include "../src/packs/synths/synths.h"
#include "../src/packs/synths/gms.h"
#include "Check.h"

using namespace gl;

namespace {

// ---------------------------------------------------------------- builders
struct Bin {
  std::vector<uint8_t> b;
  Bin& u8(uint8_t v) { return b.push_back(v), *this; }
  Bin& i32(int32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    return *this;
  }
  Bin& u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    return *this;
  }
  Bin& f32(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return i32(static_cast<int32_t>(u));
  }
  Bin& f64(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u64(u);
  }
  Bin& str(const std::string& s) { return b.insert(b.end(), s.begin(), s.end()), *this; }
  Bin& pstr(const std::string& s) { return u8(static_cast<uint8_t>(s.size())).str(s); }
  Bin& utf16(const std::string& s) {
    for (char ch : s) u8(static_cast<uint8_t>(ch)).u8(0);
    return *this;
  }
  Bin& zeros(size_t n) { return b.insert(b.end(), n, 0), *this; }
  Bin& rec(uint32_t id, const Bin& payload) { return i32(static_cast<int32_t>(id)).u64(payload.b.size()).str(std::string(payload.b.begin(), payload.b.end())); }
};

// Events of one plugin: 201 name + 213 state (+ extra events such as 218 / 219).
struct Plug {
  std::string name16;  // UTF-16LE name payload
  std::vector<uint8_t> state;
  std::vector<std::vector<uint8_t>> extra;
  std::vector<uint8_t> extraIds;
  std::vector<flp::Event> events() const {
    std::vector<flp::Event> v;
    flp::Event n;
    n.id = 201, n.data = reinterpret_cast<const uint8_t*>(name16.data()), n.size = static_cast<uint32_t>(name16.size());
    v.push_back(n);
    for (size_t i = 0; i < extra.size(); ++i) {
      flp::Event e;
      e.id = extraIds[i], e.data = extra[i].data(), e.size = static_cast<uint32_t>(extra[i].size());
      v.push_back(e);
    }
    flp::Event s;
    s.id = 213, s.data = state.data(), s.size = static_cast<uint32_t>(state.size());
    v.push_back(s);
    return v;
  }
};
std::string name16(const std::string& s) {
  std::string o;
  for (char ch : s) o += ch, o += '\0';
  return o + std::string(2, '\0');
}

struct Env {
  flp::File file;
  Song song;
  flp::Report rep;
  flp::Context ctx{file, song, rep};
  Env() { song.tempo = 120; }
  bool run(const std::string& fl, const Plug& p, flp::PluginMapping& m) {
    auto it = flp::decoders().find(fl);
    if (it == flp::decoders().end()) return false;
    auto evs = p.events();
    bool ok = it->second(ctx, evs, m);
    if (ok) normalizePluginState(m.state);
    return ok;
  }
};

float prm(const PluginState& s, int i) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : -999.f; }
bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

// One channel envelope record (event 218).
std::vector<uint8_t> envRec(bool on, int att, int hold, int dec, int sus, int rel, int amount, int lfoAmt = 0) {
  Bin b;
  b.i32(0).i32(on ? -1 : 0).i32(100).i32(att).i32(hold).i32(dec).i32(sus).i32(rel).i32(amount);
  b.i32(100).i32(20000).i32(lfoAmt).i32(32950).i32(0).i32(0).i32(0).i32(0);
  return b.b;
}

// 3x Osc state, version 14 (osc params, flags, envelope copies, levels copy).
std::vector<uint8_t> osc3State(const int p[21], const uint8_t flags[5]) {
  Bin b;
  b.i32(14);
  for (int i = 0; i < 21; ++i) b.i32(p[i]);
  for (int i = 0; i < 5; ++i) b.u8(flags[i]);
  b.zeros(3);
  for (int t = 0; t < 5; ++t) {
    auto e = envRec(false, 20000, 20000, 30000, 50, 20000, 0);
    b.b.insert(b.b.end(), e.begin(), e.end());
  }
  b.i32(0).i32(0).i32(256).i32(0).i32(0).i32(0);  // levels copy: 0, 0, cutoff, resonance, 0, type (J-34)
  return b.b;
}

// Renders `notes` (pitch) for 0.5 s each through a Gridline state; returns the peak.
double renderPeak(const PluginState& st, int pitch = 60) {
  Song s;
  s.tempo = 120;
  s.ppq = 96;
  ensureMixer(s);
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "T";
  ch.plugin = st;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  Pattern p;
  p.id = s.allocateId();
  Note n;
  n.start = 0, n.length = 96, n.pitch = pitch, n.velocity = 100;
  p.notes[ch.id].push_back(n);
  s.patterns.push_back(p);
  Clip c;
  c.id = s.allocateId(), c.pattern = p.id, c.start = 0, c.length = 192, c.track = 0;
  s.playlist.push_back(c);
  RenderOptions o;
  o.tail = 96;
  AudioBuffer buf = renderSong(s, o);
  double peak = 0;
  for (float v : buf.data) {
    if (!std::isfinite(v)) return -1;
    peak = std::max(peak, static_cast<double>(std::fabs(v)));
  }
  return peak;
}

}  // namespace

int main() {
  // ------------------------------------------------------------- 3x Osc
  {
    // osc 1: pan -32, square, -12 st, +5 ct, phase 64, detune 10;
    // osc 2: mix 64, pan 0, saw, +7, 0, 0, -20; osc 3: mix 32, pan 64, noise, -24, 0, -32, 0; rand 32
    const int p[21] = {-32, 2, -12, 5, 64, 10, 64, 0, 3, 7, 0, 0, -20, 32, 64, 5, -24, 0, -32, 0, 32};
    const uint8_t flags[5] = {1, 0, 1, 1, 1};  // invert 1, invert 3, osc 3 AM, HQ
    Plug pl{name16("3x Osc"), osc3State(p, flags), {}, {}};
    Env e;
    flp::PluginMapping m;
    expect(e.run("3x Osc", pl, m) && m.glType == "gl.synth", "3x Osc → Trio Synth");
    const auto& s = m.state;
    using namespace pp;
    expect(prm(s, SynOsc1Shape) == 3 && prm(s, SynOsc2Shape) == 2 && prm(s, SynOsc3Shape) == 4, "3x Osc shapes (square, saw, noise)");
    expect(near(prm(s, SynOsc1Level), 1) && near(prm(s, SynOsc2Level), 0.5) && near(prm(s, SynOsc3Level), 0.25), "3x Osc mix levels");
    expect(prm(s, SynOsc1Coarse) == -12 && prm(s, SynOsc1Fine) == 5 && prm(s, SynOsc2Coarse) == 7 && prm(s, SynOsc3Coarse) == -24, "3x Osc tuning");
    expect(near(prm(s, SynOsc1Pan), -0.5) && near(prm(s, SynOsc3Pan), 1.0), "3x Osc panning");
    expect(near(prm(s, SynOsc1Phase), 0.5) && near(prm(s, SynOsc3Phase), -0.25) && prm(s, SynOsc1Detune) == 10 && prm(s, SynOsc2Detune) == -20,
           "3x Osc stereo phase / detune");
    expect(prm(s, SynOsc1Invert) == 1 && prm(s, SynOsc2Invert) == 0 && prm(s, SynOsc3Invert) == 1, "3x Osc invert flags");
    expect(prm(s, SynOsc3Mode) == SynModeAM && near(prm(s, SynPhaseRand), 0.5), "3x Osc AM + phase randomness");
    expect(m.exact, "3x Osc without channel envelope is exact");
    expect(near(prm(s, SynCutoff), 20000) && near(prm(s, SynSustain), 1), "3x Osc: open filter, gate envelope");
    // Automation: tag 6 (osc 2 mix) and tag 8 (osc 2 shape), tag 20 (phase rand).
    expect(m.param && m.param(6) == SynOsc2Level && m.param(8) == SynOsc2Shape && m.param(20) == SynPhaseRand && m.param(99) == -1,
           "3x Osc automation targets");
    expect(m.value && near(m.value(9, 1.0), 24) && near(m.value(8, 0.5), 2) && near(m.value(0, 0.25), -0.5), "3x Osc automation values");
    double peak = renderPeak(s);
    expect(peak > 0.05 && peak < 4, "3x Osc import renders sound");

    // Channel envelope (vol) + filter (cutoff 128, BP) + mod X envelope from the channel's own events.
    Plug pe = pl;
    for (int t = 0; t < 5; ++t) {
      pe.extraIds.push_back(218);
      pe.extra.push_back(t == 1 ? envRec(true, 100, 100, 20000, 0, 3000, 0) : t == 2 ? envRec(true, 100, 100, 30000, 0, 100, 64) : envRec(false, 20000, 20000, 30000, 50, 20000, 0));
    }
    Bin lv;
    lv.i32(6400).i32(10000).i32(0).i32(128).i32(64).i32(2);
    pe.extraIds.push_back(219);
    pe.extra.push_back(lv.b);
    Env e2;
    flp::PluginMapping m2;
    expect(e2.run("3x Osc", pe, m2), "3x Osc with channel envelopes decodes");
    // J-33: the channel settings run as the instrument's channel envelopes / filter.
    InsSettings ins;
    expect(parseIns(m2.state.ins, ins) && ins.t[InsVol].on && ins.t[InsVol].decay == 20000 && ins.t[InsVol].release == 3000 && ins.t[InsVol].sustain == 0,
           "channel volume envelope carried (raw)");
    expect(ins.type == 2 && ins.cutoff == 128 && ins.reso == 64 && ins.t[InsModX].on && ins.t[InsModX].amount == 64 &&
               prm(m2.state, SynCutoff) == 20000,
           "channel filter + mod X envelope carried, Trio filter open");
    expect(m2.exact, "carried channel settings are exact");

    // Old version (10: 20 parameters, no flags) still decodes.
    Bin old;
    old.i32(10);
    for (int i = 0; i < 20; ++i) old.i32(p[i]);
    Env e3;
    flp::PluginMapping m3;
    expect(e3.run("3x Osc", Plug{name16("3x Osc"), old.b, {}, {}}, m3) && prm(m3.state, SynPhaseRand) == 0 && prm(m3.state, SynOsc1Invert) == 0,
           "3x Osc version 10");
  }
  // ------------------------------------------------------------ FL Keys
  {
    Bin b;
    b.i32(1).pstr("Grand Piano");
    const int k[15] = {0, 0, 1000, -1000, 0, 2000, 0, 2000, 500, 250, 0, 1000, 500, 32768, 1000};
    for (int v : k) b.i32(v);
    Env e;
    flp::PluginMapping m;
    // J-33: FL Keys → Sample Keys (raw knob values, the sound set by name).
    expect(e.run("FL Keys", Plug{name16("FL Keys"), b.b, {}, {}}, m) && m.glType == "gl.samplekeys", "FL Keys → Sample Keys");
    expect(prm(m.state, 2) == 1000 && prm(m.state, 3) == -1000 && prm(m.state, 13) == 32768 && prm(m.state, 14) == 1000 &&
               m.state.data == "Grand Piano",
           "FL Keys raw knobs + sound set name");
    expect(m.param(11) == 11 && near(m.value(11, 0.0), -1000), "FL Keys automation");
    if (m.exact)  // the reference sound sets are installed
      expect(renderPeak(m.state) > 0.01, "FL Keys import renders sound");
  }
  // ----------------------------------------------------------- MIDI Out
  {
    Bin b;
    b.i32(4).i32(9).i32(12).i32(3).i32(-1).i32(0).i32(2).i32(1);
    b.u8(0).u8(0);
    b.i32(100);  // controller 0 value
    while (b.b.size() < 0x142) b.u8(0);
    b.u8(0).u8(74).u8(0).u8(0).i32(128).u8(0).i32(0).i32(127).i32(-1).pstr("CUT").pstr("Cutoff");  // 21-byte record header (J-34)
    b.u8(0xFF).pstr("Page 1");
    Env e;
    flp::PluginMapping m;
    expect(e.run("MIDI Out", Plug{name16("MIDI Out"), b.b, {}, {}}, m) && m.glType == "gl.midiout", "MIDI Out → MIDI Out");
    using namespace pp;
    expect(prm(m.state, MoChannel) == 10 && prm(m.state, MoProgram) == 13 && prm(m.state, MoBankMsb) == 3 && prm(m.state, MoBankLsb) == -1,
           "MIDI Out channel / program / bank");
    expect(prm(m.state, moKnob(0, MoKnobNumber)) == 74 && prm(m.state, moKnob(0, MoKnobMax)) == 127 && prm(m.state, moKnob(0, MoKnobValue)) == 100,
           "MIDI Out controller");
  }
  // ----------------------------------------------------------- Harmless
  {
    Bin b;
    b.i32(12);
    int p[100] = {};
    p[1] = 256, p[23] = 500, p[27] = 1000, p[28] = 250, p[31] = 1000, p[36] = 1000, p[38] = -1200, p[54] = 4096, p[56] = 500, p[57] = 1000,
    p[59] = 300, p[52] = 400, p[65] = 3, p[67] = 128, p[66] = 128, p[71] = 500, p[89] = 3000, p[96] = 3, p[97] = 500;
    for (int k = 0; k < 12; ++k) p[11 + k] = k % 2 ? 0 : 1000;
    for (int v : p) b.i32(v);
    b.i32(4).i32(0).i32(0).i32(0).i32(11).i32(1).i32(0).i32(4);  // 4-entry custom timbre
    b.f32(1.0f).f32(0.5f).f32(0.0f).f32(0.25f);
    b.f32(0).f32(0).f32(0).f32(0);
    b.u8(1);
    Env e;
    flp::PluginMapping m;
    expect(e.run("Harmless", Plug{name16("Harmless"), b.b, {}, {}}, m) && m.glType == "gl.additive", "Harmless → Additive");
    namespace md = packs::models;
    expect(near(prm(m.state, md::AdPitch), -12) && near(prm(m.state, md::AdSustain), 1) && prm(m.state, md::AdUniOrder) == 3, "Harmless pitch / env / unison");
    expect(near(prm(m.state, md::AdMaskBase + 0), 1) && near(prm(m.state, md::AdMaskBase + 1), 0) && near(prm(m.state, md::AdMaskMix), 0.5),
           "Harmless harmonic mask");
    md::AdditiveData cd;
    expect(prm(m.state, md::AdSpectrumA) == md::SpCustom && md::parseAdditiveData(m.state.data, cd) && cd.level.size() == 4 &&
               near(cd.level[1], 0.5f),
           "Harmless custom timbre → custom table");
    expect(prm(m.state, md::AdFltCutoff) > 200 && prm(m.state, md::AdFEnvAmt) > 3 && prm(m.state, md::AdFx + md::FxReverb) > 0, "Harmless filter + reverb");
    expect(m.param(54) == md::AdFltCutoff && m.param(52) == md::AdFEnvAmt && !m.exact, "Harmless automation targets, approximate");
    expect(renderPeak(m.state) > 0.01, "Harmless import renders sound");
  }
  // --------------------------------------------------------- DirectWave
  {
    auto zone = [](int root, int lo, int hi, float vol, float pan, const std::string& path, int loop) {
      Bin z, k;
      k.u8(static_cast<uint8_t>(root)).u8(static_cast<uint8_t>(lo)).u8(static_cast<uint8_t>(hi)).u8(0).u8(127).u8(0).u8(0).u8(0xFF).u8(0);
      k.f32(vol).f32(pan).f32(0).zeros(4);
      z.rec(500, k);
      z.rec(501, Bin().str("Z"));
      z.rec(502, Bin().str(path));
      Bin info;
      info.i32(1000).i32(0).i32(1).i32(16).f32(44100).i32(loop).i32(loop ? 100 : 0).i32(loop ? 900 : 0);
      z.rec(503, info);
      return z;
    };
    Bin prog, empty, st;
    prog.rec(0x64, Bin().zeros(30)).rec(0x66, Bin().str("Program"));
    prog.rec(3, zone(48, 0, 54, 1.0f, 0.5f, "/tmp/a.wav", 0));
    prog.rec(3, zone(60, 55, 127, 0.5f, 1.0f, "/tmp/b.wav", 2));
    empty.rec(0x64, Bin().zeros(30));
    st.i32(38).rec(6, Bin().zeros(16)).rec(1, prog).rec(1, empty);
    Env e;
    flp::PluginMapping m;
    // J-33: DirectWave → Wave Program (the reference zone engine; zones kept with their raw values).
    expect(e.run("DirectWave", Plug{name16("DirectWave"), st.b, {}, {}}, m) && m.glType == "gl.waveprogram", "DirectWave → Wave Program");
    packs::samplers::dw::DwProgram wp;
    expect(packs::samplers::dw::parseProgram(m.state.data, wp) && wp.zones.size() == 2, "DirectWave zones");
    if (wp.zones.size() == 2) {
      const auto &a = wp.zones[0], &z = wp.zones[1];
      expect(a.root == 48 && a.lo == 0 && a.hi == 54 && near(a.gain, 1) && near(a.pan, 0.5) && a.path == "/tmp/a.wav", "DirectWave zone 1");
      expect(z.root == 60 && z.lo == 55 && z.hi == 127 && near(z.gain, 0.5) && near(z.pan, 1) && z.loopMode == 2 && z.loopStart == 100 &&
                 z.loopEnd == 900,
             "DirectWave zone 2 (gain, pan, loop)");
    }
  }
  // --------------------------------------------------------------- FLEX
  {
    auto flex = [](const std::string& lib, const std::string& preset, float pitch) {
      Bin id, params, inner, st;
      id.str(lib);
      while (id.b.size() < 0x401) id.u8(0);
      id.str(preset);
      while (id.b.size() < 0x812) id.u8(0);
      inner.i32(2).i32(128);
      for (int k = 0; k < 128; ++k) inner.f32(k == 39 ? pitch : (k < 10 || k == 18 ? 0.5f : 0.0f));
      params.rec(0, inner);
      st.i32(1).rec(1, id).rec(2, params);
      return st.b;
    };
    Env e;
    flp::PluginMapping m;
    expect(e.run("FLEX", Plug{name16("FLEX"), flex("Library", "Grand Piano Dark", 0.5f), {}, {}}, m) && m.glType == "gl.keys", "FLEX piano → Keys");
    flp::PluginMapping m2;
    expect(e.run("FLEX", Plug{name16("FLEX"), flex("Library", "Crystal Bell", 0.5f), {}, {}}, m2) && m2.glType == "gl.fm", "FLEX bell → FM Synth");
    flp::PluginMapping m3;
    expect(e.run("FLEX", Plug{name16("FLEX"), flex("Library", "Deep Sub Bass", 0.25f), {}, {}}, m3) && m3.glType == "gl.analog" &&
               near(prm(m3.state, packs::synths::AnTranspose), -24),
           "FLEX bass → Analog with pitch");
    expect(renderPeak(m3.state, 48) > 0.01 && renderPeak(m2.state) > 0.01, "FLEX imports render sound");
  }
  // ------------------------------------------------ Envelope Controller
  {
    Bin b;
    b.u8(3).u8(0).u8(6).u8(0);
    while (b.b.size() < 0x180) b.u8(0);
    b.i32(3).f32(0.05f).f32(1).f32(0.5f).i32(1).i32(3).i32(3);
    b.f64(0).f64(0).f32(0).i32(0);
    b.f64(0.25).f64(0.75).f32(0).i32(0);
    b.f64(0.5).f64(1.0).f32(0).i32(0);
    Env e;
    e.song.tempo = 120;
    flp::PluginMapping m;
    expect(e.run("Fruity Envelope Controller", Plug{name16("Fruity Envelope Controller"), b.b, {}, {}}, m) && m.glType == "gl.envctl",
           "Envelope Controller → Envelope Controller");
    namespace mo = packs::modulation;
    expect(prm(m.state, mo::envParam(0, mo::EaNumPoints)) == 3 && near(prm(m.state, mo::envPointTime(0, 1)), 0.125) &&
               near(prm(m.state, mo::envPointLevel(0, 1)), 0.75) && near(prm(m.state, mo::envPointLevel(0, 2)), 1),
           "Envelope Controller points (beats → seconds)");
  }
  // ---------------------------------------------------- Control Surface
  {
    Bin b;
    b.i32(1).rec(2000, Bin().zeros(64)).rec(2003, Bin().i32(1)).rec(2100, Bin().i32(3).zeros(28));
    b.rec(2103, Bin().utf16("Drop"));
    b.rec(2102, Bin().f32(1).f32(0).f32(0));
    b.rec(2105, Bin().utf16("control TQuickBtn\r\n  StayDown = True\r\nend"));
    b.rec(2101, Bin());
    Env e;
    flp::PluginMapping m;
    expect(e.run("Control Surface", Plug{name16("Control Surface"), b.b, {}, {}}, m) && m.glType == "gl.surface", "Control Surface → Control Surface");
    expect(m.state.data.rfind("3 Drop\n", 0) == 0 && near(prm(m.state, 0), 1), "Control Surface toggle control + value");
  }
  // ------------------------------------------------ BooBass, Fruit Kick
  {
    Env e;
    flp::PluginMapping m;
    expect(e.run("BooBass", Plug{name16("BooBass"), Bin().i32(1).i32(65535).i32(32767).i32(0).b, {}, {}}, m) && m.glType == "gl.fingerbass" &&
               near(prm(m.state, 0), 1) && near(prm(m.state, 2), 0),
           "BooBass → Finger Bass (J-22)");
    flp::PluginMapping k;
    expect(e.run("Fruit Kick", Plug{name16("Fruit Kick"), Bin().i32(0).i32(0).i32(-600).i32(64).i32(128).i32(64).i32(128).i32(0).b, {}, {}}, k) &&
               k.glType == "gl.sweepkick" && near(prm(k.state, 4), 64) && near(prm(k.state, 5), 128),
           "Fruit Kick → Sweep Kick (click / distortion tags 1:1)");
    expect(renderPeak(k.state, 48) > 0.01, "Fruit Kick import renders sound");
  }
  // --------------------------------------------- Sytrus (zlib), Harmor, GMS
  {
    // zlib stream with one stored block: master level 128, ?, master pitch -24.
    Bin raw;
    raw.i32(128).i32(128).i32(-24);
    raw.zeros(64);
    Bin z;
    z.u8(0x78).u8(0x01).u8(0x01);
    const uint16_t len = static_cast<uint16_t>(raw.b.size());
    z.u8(len & 0xFF).u8(len >> 8).u8(static_cast<uint8_t>(~len & 0xFF)).u8(static_cast<uint8_t>((~len >> 8) & 0xFF));
    z.str(std::string(raw.b.begin(), raw.b.end()));
    Bin st;
    st.u8(3).u8(0).u8(25).u8(0).str(std::string(z.b.begin(), z.b.end()));
    const std::string bassName = name16("Bass");
    Plug p{name16("Sytrus"), st.b, {std::vector<uint8_t>(bassName.begin(), bassName.end())}, {203}};
    Env e;
    flp::PluginMapping m;
    expect(e.run("Sytrus", p, m) && m.glType == "gl.fm" && prm(m.state, pp::FmOctave) == -2, "Sytrus (zlib) → FM bass, master pitch");
    expect(renderPeak(m.state, 48) > 0.01, "Sytrus import renders sound");

    Bin h;
    h.u8(3).u8(0).u8(19).u8(0).i32(7).u64(94 * 4);
    for (int t = 1; t <= 94; ++t) h.i32(t == 40 ? 10000 : t == 41 ? 2 : t == 34 ? 3 : t == 52 ? 4096 : t == 75 ? 1000 : t == 84 ? 10000 : 0);
    flp::PluginMapping mh;
    // An early, made-up layout: the full Resynth decoder refuses what does not walk.
    expect(!e.run("Harmor", Plug{name16("Harmor"), h.b, {}, {}}, mh), "Harmor: malformed state refused");

    Bin g;
    g.str("SSLF").i32(4960).str("4nyS").str("C:\\Waves\\Square Retro.wav");
    while (g.b.size() < 3608) g.u8(0);
    std::vector<float> gp(61, 0.f);
    gp[0] = 0.8f, gp[2] = 0.5f, gp[6] = 1.f, gp[8] = 0.5f, gp[12] = 1.f, gp[40] = 1.f, gp[44] = 0.5f, gp[55] = 1.f / 3.f, gp[57] = 1.f / 6.f;
    for (float v : gp) g.f32(v);
    flp::PluginMapping mg;
    namespace sy = packs::synths;
    namespace gm = packs::synths::gms;
    const float w0 = prm(mg.state, gm::osc(0, gm::OsWave));
    expect(e.run("GMS", Plug{name16("GMS"), g.b, {}, {}}, mg) && mg.glType == "gl.gms" &&
               (prm(mg.state, gm::osc(0, gm::OsWave)) == gm::WvFile || prm(mg.state, gm::osc(0, gm::OsWave)) == gm::WvSquare) &&
               prm(mg.state, gm::osc(0, gm::OsSemi)) == -12 && prm(mg.state, gm::osc(1, gm::OsSemi)) == -24 && prm(mg.state, gm::GMono) == 1,
           "GMS → Groove Synth (wave, osc tuning, mono)");
    (void)w0;
    expect(renderPeak(mg.state) > 0.01, "GMS import renders sound");
  }
  // ------------------------------------------- robustness: truncated states
  {
    const char* names[] = {"3x Osc", "FL Keys", "MIDI Out", "Harmless", "DirectWave", "FLEX", "Fruity Envelope Controller", "Control Surface",
                           "BooBass", "Fruit Kick", "GMS", "Harmor", "Sytrus"};
    bool ok = true;
    for (const char* n : names)
      for (size_t len : {0u, 3u, 9u, 40u}) {
        Plug pl{name16(n), std::vector<uint8_t>(len, 0xFF), {}, {}};
        Env e;
        flp::PluginMapping m;
        auto it = flp::decoders().find(n);
        auto evs = pl.events();
        if (it == flp::decoders().end()) {
          ok = false;
          continue;
        }
        if (it->second(e.ctx, evs, m)) {
          normalizePluginState(m.state);
          for (float v : m.state.params) ok = ok && std::isfinite(v);
        }
      }
    expect(ok, "garbage / truncated states never crash and give finite params");
  }
  return finish();
}
