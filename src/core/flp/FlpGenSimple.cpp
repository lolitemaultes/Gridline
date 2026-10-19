// Gridline — FLP import: small FL generators → Gridline instruments.
//   FL Keys          → Keys ("gl.keys")
//   MIDI Out         → MIDI Out ("gl.midiout")
//   Envelope Ctrl    → Envelope Controller ("gl.envctl")
//   Control Surface  → Control Surface ("gl.surface")
//   BooBass          → Analog ("gl.analog")
//   Fruit Kick / BassDrum → Kick ("gl.kick")
#include <algorithm>
#include <cmath>
#include "../PluginParams.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/synths/synths.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

using namespace gl::pp;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

// ------------------------------------------------------------------ FL Keys
// State: dword version 1, ANSI string (1-byte length) naming the sample set
// ("Grand Piano", "Piano", "mda Piano", "Rhodes (mda ePiano)"), then 15 dwords
// in control-tag order: decay, release, hardness, velocity → hardness, muffle,
// velocity → muffle, velocity sensitivity, stereo, tune, detune, stretch,
// treble, pan / tremolo, LFO rate, overdrive.
struct KeysConv {
  static double decay(double v) { return clampd(3.5 * std::pow(2.0, v / 1000.0 * 1.8), 0.3, 12); }
  static double release(double v) { return clampd(0.25 * std::pow(3.5, v / 1000.0), 0.02, 3); }
  static double hardness(double v) { return clampd(0.45 + v / 1000.0 * 0.45, 0, 1); }
  static double muffle(double v) { return clampd(20000.0 * std::pow(0.1, v / 1000.0), 500, 20000); }
  static double velSens(double v) { return clampd(0.7 + v / 1000.0 * 0.3, 0, 1); }
  static double lfoRate(double v) { return clampd(0.1 * std::pow(100.0, v / 65536.0), 0.1, 10); }
};

bool decodeKeys(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 5)) return false;
  size_t at = 0;
  std::string model = b.pstr(4, &at);
  int p[15] = {0, 0, 0, -1000, 784, 502, -248, 1000, 0, 123, 0, 0, 0, 0, 0};
  for (int i = 0; i < 15 && b.has(at + static_cast<size_t>(i) * 4, 4); ++i) p[i] = b.i32(at + static_cast<size_t>(i) * 4);
  PluginState s = base("gl.keys");
  setp(s, KyDecay, KeysConv::decay(p[0]));
  setp(s, KyRelease, KeysConv::release(p[1]));
  setp(s, KyHardness, KeysConv::hardness(p[2]));
  setp(s, KyVelHardness, clampd(p[3] / 1000.0, -1, 1));
  setp(s, KyMuffle, KeysConv::muffle(p[4]));
  setp(s, KyVelMuffle, clampd(p[5] / 2000.0, 0, 1));
  setp(s, KyVelocity, KeysConv::velSens(p[6]));
  setp(s, KyStereo, clampd(p[7] / 2000.0, 0, 1));
  setp(s, KyTune, clampd(p[8] / 10.0, -100, 100));
  setp(s, KyDetune, clampd(p[9] / 500.0 * 30.0, 0, 30));
  setp(s, KyStretch, clampd(p[10] / 10.0, -50, 50));
  setp(s, KyTreble, clampd(p[11] / 1000.0 * 12.0, -12, 12));
  setp(s, KyModMode, p[12] < 0 ? 1 : 0);  // negative = auto-pan, positive = tremolo
  setp(s, KyTremDepth, clampd(std::fabs(static_cast<double>(p[12])) / 1000.0, 0, 1));
  setp(s, KyTremRate, KeysConv::lfoRate(p[13]));
  setp(s, KyDrive, clampd(p[14] / 1000.0, 0, 1));
  // Sample set → the closest of the Keys models (Gridline's keys are modelled, not sampled).
  int mdl = 5;  // pianos: the hammered-string models (grand / upright)
  if (model.find("Grand") != std::string::npos) mdl = 4;
  if (model.find("Rhodes") != std::string::npos || model.find("ePiano") != std::string::npos) mdl = 0;
  if (model.find("Wurl") != std::string::npos || model.find("Reed") != std::string::npos) mdl = 1;
  setp(s, KyModel, mdl);
  setp(s, KyVolume, 0.8);
  m.glType = "gl.keys";
  m.state = s;
  m.exact = false;
  note(c, "FL Keys", "\"" + model + "\" played by the modelled Keys (all 15 controls mapped, timbre approximated)");
  ParamTable t;
  t.fn(0, KyDecay, [](double n) { return KeysConv::decay(-1000 + 2000 * n); });
  t.fn(1, KyRelease, [](double n) { return KeysConv::release(-1000 + 2000 * n); });
  t.fn(2, KyHardness, [](double n) { return KeysConv::hardness(-1000 + 2000 * n); });
  t.lin(3, KyVelHardness, -1, 1);
  t.fn(4, KyMuffle, [](double n) { return KeysConv::muffle(1000 * n); });
  t.lin(5, KyVelMuffle, 0, 1);
  t.fn(6, KyVelocity, [](double n) { return KeysConv::velSens(-1000 + 2000 * n); });
  t.lin(7, KyStereo, 0, 1);
  t.lin(8, KyTune, -100, 100);
  t.lin(9, KyDetune, 0, 30);
  t.lin(10, KyStretch, -50, 50);
  t.lin(11, KyTreble, -12, 12);
  t.fn(12, KyTremDepth, [](double n) { return std::fabs(n * 2 - 1); });
  t.fn(13, KyTremRate, [](double n) { return KeysConv::lfoRate(65536 * n); });
  t.lin(14, KyDrive, 0, 1);
  // Control ranges (tag order): decay .. treble, pan / tremolo, LFO rate, overdrive.
  const int keysRange[15][2] = {{-1000, 1000}, {-1000, 1000}, {-1000, 1000}, {-1000, 1000}, {0, 1000}, {0, 2000}, {-1000, 1000}, {0, 2000},
                                {-1000, 1000}, {0, 500},     {-500, 500},   {-1000, 1000}, {-1000, 1000}, {0, 65536}, {0, 1000}};
  for (int k = 0; k < 15; ++k) t.range(k, keysRange[k][0], keysRange[k][1]);
  t.install(m);
  return true;
}

// ----------------------------------------------------------------- MIDI Out
// State: dword version, MIDI channel (0-based), program (-1 none), bank MSB,
// bank LSB (-1 none), page, port, flags. Version 4+: controller values from
// 0x22 (dword each), controller records from 0x142: byte index (0xFF ends),
// word controller number, byte kind, dword ?, dword min, dword max, dword
// channel (-1 = the plugin's), two ANSI names.
bool decodeMidiOut(Context&, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 32)) return false;
  const int version = b.i32(0);
  PluginState s = base("gl.midiout");
  setp(s, MoChannel, std::clamp(b.i32(4), 0, 15) + 1);
  const int prog = b.i32(8);
  setp(s, MoProgram, prog >= 0 && prog <= 127 ? prog + 1 : 0);
  setp(s, MoBankMsb, std::clamp(b.i32(12), -1, 127));
  setp(s, MoBankLsb, std::clamp(b.i32(16), -1, 127));
  setp(s, MoPort, std::clamp(b.i32(24), 0, 15));
  bool exact = true;
  if (version >= 4) {
    size_t at = 0x142;
    int k = 0;
    while (b.has(at, 21) && b.u8(at) != 0xFF) {
      const int idx = b.u8(at);
      const int number = b.u16(at + 1);
      const int lo = b.i32(at + 8), hi = b.i32(at + 12), chn = b.i32(at + 16);
      size_t next = at + 20;
      b.pstr(next, &next);
      b.pstr(next, &next);
      if (k < kMoKnobs) {
        setp(s, moKnob(k, MoKnobType), 0);  // CC
        setp(s, moKnob(k, MoKnobNumber), std::clamp(number, 0, 16383));
        setp(s, moKnob(k, MoKnobMin), std::clamp(lo, 0, 16383));
        setp(s, moKnob(k, MoKnobMax), std::clamp(hi, 0, 16383));
        setp(s, moKnob(k, MoKnobChannel), chn >= 0 && chn < 16 ? chn + 1 : 0);
        const size_t vAt = 0x22 + static_cast<size_t>(idx) * 4;
        if (vAt + 4 <= 0x142) setp(s, moKnob(k, MoKnobValue), std::clamp(b.i32(vAt), 0, 127));
      } else {
        exact = false;
      }
      ++k;
      if (next <= at) break;
      at = next;
    }
  }
  m.glType = "gl.midiout";
  m.state = s;
  m.exact = exact;
  // Automation of the controller knobs (FL parameter k = controller k).
  ParamTable t;
  for (int k = 0; k < kMoKnobs; ++k) t.lin(k, moKnob(k, MoKnobValue), 0, 127), t.range(k, 0, 127);
  t.install(m);
  return true;
}

// ------------------------------------------------------ Envelope Controller
// State: word version, word count, then per-articulator parameter blocks and
// FL envelope lists. An envelope list is: dword point count N, three floats,
// three dwords, then N points of {double Δtime (beats), double level 0..1,
// float tension, dword flags}. The first list is articulator 1's envelope.
namespace mod = gl::packs::modulation;

// An articulator's envelope list: header {count, float, float 1.0, float 0.5,
// dword 1, dword, dword} then points; mapping lists (keyboard, velocity …)
// that follow have zero floats and are skipped. Points are kept up to the
// first invalid one.
struct EnvList {
  size_t at = 0;
  int points = 0;
};
std::vector<EnvList> findEnvelopes(Bytes b, size_t from) {
  std::vector<EnvList> out;
  for (size_t p = from; b.has(p, 28 + 24 * 2);) {
    const int n = b.i32(p);
    if (n < 2 || n > 64 || std::fabs(b.f32(p + 8) - 1.0f) > 1e-6f || std::fabs(b.f32(p + 12) - 0.5f) > 1e-6f || b.i32(p + 16) != 1) {
      p += 4;
      continue;
    }
    int ok = 0;
    for (int k = 0; k < n && b.has(p + 28 + static_cast<size_t>(k) * 24, 24); ++k) {
      double dt = b.f64(p + 28 + static_cast<size_t>(k) * 24), y = b.f64(p + 28 + static_cast<size_t>(k) * 24 + 8);
      if (!(dt >= 0 && dt < 1000 && y >= -0.001 && y <= 1.001)) break;
      ++ok;
    }
    if (ok < 2) {
      p += 4;
      continue;
    }
    out.push_back({p, ok});
    p += 28 + static_cast<size_t>(ok) * 24;
  }
  return out;
}

// Output k of the FL controller = articulator k = Gridline output k (Env A..D).
bool decodeEnvController(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 16)) return false;
  PluginState s = base("gl.envctl");
  std::vector<EnvList> lists = findEnvelopes(b, 0x100);
  const double secPerBeat = 60.0 / std::max(20.0, c.song.tempo);
  const int arts = std::min(static_cast<int>(lists.size()), mod::kEnvArts);
  for (int a = 0; a < arts; ++a) {
    const size_t at = lists[static_cast<size_t>(a)].at;
    const int np = std::min(lists[static_cast<size_t>(a)].points, mod::kEnvPoints);
    for (int k = 0; k < np; ++k) {
      double dt = b.f64(at + 28 + static_cast<size_t>(k) * 24), y = b.f64(at + 28 + static_cast<size_t>(k) * 24 + 8);
      setp(s, mod::envPointTime(a, k), clampd(dt * secPerBeat, 0.001, 10));
      setp(s, mod::envPointLevel(a, k), clampd(y, 0, 1));
    }
    setp(s, mod::envParam(a, mod::EaNumPoints), std::max(2, np));
    setp(s, mod::envParam(a, mod::EaSustain), 0);  // one-shot shape; the level holds after the last point
    setp(s, mod::envParam(a, mod::EaBase), 0.0);
    setp(s, mod::envParam(a, mod::EaEnvAmt), 1.0);
    setp(s, mod::envParam(a, mod::EaLfoAmt), 0.0);
  }
  m.glType = "gl.envctl";
  m.state = s;
  m.exact = false;
  note(c, "Fruity Envelope Controller",
       arts ? std::to_string(arts) + " articulator envelope(s) → outputs Env A.. (beats → seconds at the song tempo); LFOs / key mappings not imported"
            : "no envelope found; default controller");
  // FL parameters 1 / 2 (envelope / LFO level of the current articulator) → articulator A.
  ParamTable t;
  t.lin(1, mod::envParam(0, mod::EaEnvAmt), -1, 1);
  t.lin(2, mod::envParam(0, mod::EaLfoAmt), -1, 1);
  t.range(1, -256, 256), t.range(2, -256, 256);
  t.install(m);
  return true;
}


// ------------------------------------------------------------------ BooBass
// State: dword version, then bass, mid, treble (0..65535, centre 32767).
bool decodeBooBass(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  namespace sy = gl::packs::synths;
  Bytes b = stateOf(events);
  if (!b.has(0, 16)) return false;
  const double bass = b.i32(4) / 65535.0, mid = b.i32(8) / 65535.0, treble = b.i32(12) / 65535.0;
  PluginState s = base("gl.analog");
  setp(s, sy::anOsc(0, sy::OscWave), sy::WaveSaw);
  setp(s, sy::anOsc(0, sy::OscLevel), 0.4 + 0.6 * mid);
  setp(s, sy::anOsc(1, sy::OscOn), 0);
  setp(s, sy::AnSubLevel, clampd(bass, 0, 1));
  setp(s, sy::AnSubWave, 0);
  setp(s, sy::AnFltMode, sy::FltLp24);
  setp(s, sy::AnCutoff, 150.0 * std::pow(40.0, clampd(treble, 0, 1)));
  setp(s, sy::AnReso, 0.1);
  setp(s, sy::AnFltEnv, 0.2);
  setp(s, sy::AnAmpA, 0.002), setp(s, sy::AnAmpD, 1.5), setp(s, sy::AnAmpS, 0.6), setp(s, sy::AnAmpR, 0.12);
  setp(s, sy::AnPlayMode, sy::PlayMono);
  m.glType = "gl.analog";
  m.state = s;
  m.exact = false;
  note(c, "BooBass", "approximated by Analog (bass → sub, mid → saw level, treble → cutoff)");
  ParamTable t;
  t.lin(0, sy::AnSubLevel, 0, 1);
  t.fn(1, sy::anOsc(0, sy::OscLevel), [](double n) { return 0.4 + 0.6 * n; });
  t.fn(2, sy::AnCutoff, [](double n) { return 150.0 * std::pow(40.0, n); });
  for (int k = 0; k < 3; ++k) t.range(k, 0, 65535);
  t.install(m);
  return true;
}

// --------------------------------------------------------------- Fruit Kick
// State: dword version 0, then control tags 0..5: max frequency (cents),
// min frequency (cents), frequency decay 0..256, amp decay 0..256, click
// 0..64, distortion 0..128; then a dword seed.
struct KickConv {
  static double start(double cents) { return clampd(220.0 * std::pow(2.0, cents / 1200.0), 40, 1000); }
  static double end(double cents) { return clampd(55.0 * std::pow(2.0, (cents + 600.0) / 1200.0), 25, 200); }
  static double sweep(double v) { return clampd(0.005 * std::pow(100.0, v / 256.0), 0.005, 0.5); }
  static double decay(double v) { return clampd(0.05 * std::pow(40.0, v / 256.0), 0.05, 2); }
};

bool decodeFruitKick(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 28)) return false;
  PluginState s = base("gl.kick");
  setp(s, KickPitch, KickConv::start(b.i32(4)));
  setp(s, KickEndPitch, KickConv::end(b.i32(8)));
  setp(s, KickSweep, KickConv::sweep(b.i32(12)));
  setp(s, KickDecay, KickConv::decay(b.i32(16)));
  setp(s, KickClick, clampd(b.i32(20) / 64.0, 0, 1));
  setp(s, KickDrive, clampd(b.i32(24) / 128.0, 0, 1));
  setp(s, KickKeyTrack, 1);
  m.glType = "gl.kick";
  m.state = s;
  m.exact = false;
  note(c, "Fruit Kick", "mapped to Kick (pitch sweep, decays, click, drive)");
  ParamTable t;
  t.fn(0, KickPitch, [](double n) { return KickConv::start(-2400 + 6000 * n); });  // max freq control -2400..3600
  t.fn(1, KickEndPitch, [](double n) { return KickConv::end(-1200 + 2400 * n); });
  t.fn(2, KickSweep, [](double n) { return KickConv::sweep(256 * n); });
  t.fn(3, KickDecay, [](double n) { return KickConv::decay(256 * n); });
  t.lin(4, KickClick, 0, 1);
  t.lin(5, KickDrive, 0, 1);
  t.range(0, -2400, 3600), t.range(1, -1200, 1200), t.range(2, 0, 256), t.range(3, 0, 256), t.range(4, 0, 64), t.range(5, 0, 128);
  t.install(m);
  return true;
}

}  // namespace

void registerSimpleDecoders(std::map<std::string, PluginDecoder>& d) {
  d["FL Keys"] = decodeKeys;
  d["MIDI Out"] = decodeMidiOut;
  d["MIDI out"] = decodeMidiOut;
  d["Fruity Envelope Controller"] = decodeEnvController;
  // "Control Surface" is decoded by FlpFxMisc.cpp (same state format; one decoder for both hosts).
  d["BooBass"] = decodeBooBass;
  d["Fruit Kick"] = decodeFruitKick;
  d["Fruit kick"] = decodeFruitKick;
}

}  // namespace gl::flp::gen
