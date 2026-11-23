// Gridline - K-142 (0.7.8), CPU sweep of every built-in plugin: one instance of each registered
// instrument, effect and note effect in a fresh project, the transport stopped, realtime resting on
// (as the app plays), call counting on:
//  * untouched: does it run DSP with nothing played into it;
//  * after one note (instruments, note effects: half a second held) or one second of sound (effects:
//    a resting synth playing into the insert the effect sits on), then silence: how long its tail
//    lasts (the last sample that is not exact silence, and the last above -100 dBFS), whether it then
//    rests (no DSP call in the last two seconds of 30), and what its blocks cost while idle;
//  * an offline render of the same note (no resting) hashed, so a change to a plugin can be shown
//    to leave its sound bit-identical (--hashes <file> writes them, --compare <file> checks them).
// Plain run (ctest): every plugin must rest after its tail unless it is listed in kMayNotRest with
// the reason, and a plugin listed there that does rest is reported so the list stays honest.
//   gridline_tests_k142_plugins [--tsv <file>] [--hashes <file>] [--compare <file>] [--only <type>]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Denormals.h"
#include "engine/Effects.h"
#include "packs/edison/edison.h"
#include "packs/utility/utility_rt.h"
#include "engine/Program.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;
constexpr int kBlock = 256;

// Plugins that keep running at rest by design (they make sound or keep time without notes), with
// the reason. Everything else must rest once its tail has gone.
const std::map<std::string, std::string>& mayNotRest() {
  static const std::map<std::string, std::string> m = {
      // instruments that sound without notes (K-277: mayRest() false)
      {"gl.platter", "decks play on their own"},
      {"gl.scratch", "its editor scratches the sample"},
      {"gl.turntable", "its editor scratches the record"},
      {"gl.splinescratch", "its editor scratches the sample"},
      {"gl.tracker", "a module player: its editor previews rows"},
      {"gl.seltmann", "a latched pattern and its editor's pads play"},
      {"gl.cvin", "an audio input"},
      {"gl.patcher", "its modules may play without notes"},
      // control and routing (the engine keeps them running: their outputs feed others)
      {"gl.envctl", "controller"}, {"gl.keyctl", "controller"}, {"gl.cvctl", "controller"},
      {"gl.peakctl", "controller"}, {"gl.xyctl", "controller"}, {"gl.xyzctl", "controller"},
      {"gl.formulactl", "controller"}, {"gl.surface", "controller"},
      {"gl.send", "send"}, {"gl.bussend", "send"}, {"gl.busreturn", "return"},
      {"gl.patcherfx", "its modules may play without input"},
      // effects that replay what they captured, whose capture can stand still (a freeze, a stage not in use)
      {"gl.hitloop", "loops what it caught; its capture can freeze"},
      {"gl.effector", "its grain stage replays what it caught; the stage may not be in use"},
      {"gl.scratcher", "scratches what it caught (hidden, old projects)"},
      {"gl.turntablefx", "plays its own record"},
      // engine plugins that drive external gear
      {"gl.midiout", "engine: MIDI out"},
      {"gl.hwinsert", "engine: a hardware insert"},
  };
  return m;
}

struct Row {
  std::string type, name, kind;
  bool sound = false;             // made any sound for the note / passed the sound on
  double peakDb = -999;
  bool untouchedRuns = false;     // ran DSP before anything was played
  double tailExact = -1;          // seconds after the note off / end of sound to the last non-zero sample (-1: none)
  double tail100 = -1;            // ... to the last sample above -100 dBFS
  double restAfter = -1;          // seconds after the note off until its last DSP call (-1: never ran after)
  bool rests = false;             // no DSP call in the last 2 s
  uint64_t lastCalls = 0, lastBlocks = 0;
  double idlePct = 0;             // engine time per block in the last 2 s, % of real time
  uint64_t hash = 0;              // offline render of the same note
  // Effects, run on their own (a fresh instance, the program's state and aux) by the rule the engine
  // rests hosted and declared built-in effects with (GitHub #66: stopped, fed exact silence, output
  // at or below -90 dBFS for 4 s; sound out of 0.25 s of quiet marks one that plays by itself):
  bool declares = false;          // restsWhenFaint()
  double faintRestAt = -1;        // seconds after the sound ended when that rule rests it (-1: never in 30 s)
  bool selfSound = false;
  double silentPct = 0;           // its DSP fed silence, % of real time (what resting saves)
};

uint64_t fnv(const std::vector<float>& a, const std::vector<float>& b) {
  uint64_t h = 1469598103934665603ull;
  for (const auto* v : {&a, &b})
    for (float x : *v) {
      uint32_t u;
      std::memcpy(&u, &x, 4);
      for (int k = 0; k < 4; ++k) h = (h ^ ((u >> (8 * k)) & 0xff)) * 1099511628211ull;
    }
  return h;
}

struct Setup {
  Song song;
  ObjectId chan = kNoId, track = kNoId;  // the instrument's channel; the effect's insert
  bool effect = false;
};

Setup makeSetup(const PluginInfo& pi) {
  Setup st;
  st.song = makeEmptySong();
  Channel c;
  c.id = st.song.allocateId();
  c.name = "Test";
  if (pi.kind == PluginKind::Generator) {
    c.plugin = defaultPluginState(pi.type);
  } else {
    c.plugin = defaultPluginState("gl.synth");
    if (pi.kind == PluginKind::NoteFx) c.noteFx.push_back(defaultPluginState(pi.type));
  }
  c.mixerTrack = st.song.masterId();
  if (pi.kind == PluginKind::Effect) {
    st.effect = true;
    MixerTrack* ins = nullptr;
    for (auto& m : st.song.mixer)
      if (m.id != st.song.masterId()) {
        ins = &m;
        break;
      }
    if (ins) {
      if (ins->slots.empty()) ins->slots.resize(1);
      ins->slots[0].plugin = defaultPluginState(pi.type);
      st.track = ins->id;
      c.mixerTrack = ins->id;
    }
  }
  st.chan = c.id;
  st.song.channels.push_back(c);
  return st;
}

uint64_t callsOf(const ProgramBuilder& b, const Setup& st) {
  if (st.effect) {
    auto rt = b.effectRuntime(st.track, 0);
    return rt ? rt->dspCalls.load() : 0;
  }
  auto rt = b.channelRuntime(st.chan);
  return rt ? rt->dspCalls.load() : 0;
}

// Seconds of sound played: instruments half a second of one note, effects a second of a synth note.
double heldSeconds(const Setup& st) { return st.effect ? 1.0 : 0.5; }


// The effect alone: one second of a decaying saw (-6 dBFS), then 30 s of silence, by the engine's
// faint rule (AudioEngine::noteEffectQuiet), stopped. Also what its DSP costs on silence.
void simulateFaintRest(const PluginInfo& pi, const Program& prog, ObjectId track, Row& r) {
  const ProgSlot* slot = nullptr;
  for (const ProgMixer& m : prog.mixer)
    if (m.id == track && !m.slots.empty()) slot = &m.slots[0];
  if (!slot) return;
  auto fx = createEffect(pi.type);
  if (!fx) return;
  ScopedFlushDenormals ftz;  // as on the audio thread
  fx->prepareHost(slot->plugin, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  std::vector<float> L(kBlock), R(kBlock);
  double ph = 0;
  int64_t faint = 0;
  const float kFaint = AudioEngine::kRestFaint;
  double ns = 0;
  int timed = 0;
  for (int at = 0; at < 31 * kSr; at += kBlock) {
    const bool sound = at < kSr;
    for (int i = 0; i < kBlock; ++i) {
      float x = 0;
      if (sound) {
        ph += 220.0 / kSr;
        ph -= std::floor(ph);
        x = static_cast<float>((2 * ph - 1) * 0.5 * std::exp(-2.0 * (at + i) / kSr));
      }
      L[i] = x;
      R[i] = x * 0.9f;
    }
    // (asked before each block, as the engine does: an effect may say yes only after a while)
    const bool may = fx->restsWhenFaint();
    r.declares = r.declares || may;
    const auto t0 = std::chrono::steady_clock::now();
    fx->processAux(slot->plugin, slot->aux, ctx, L.data(), R.data(), kBlock);
    const auto t1 = std::chrono::steady_clock::now();
    if (at >= 29 * kSr) {
      ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
      ++timed;
    }
    if (sound || !may) {
      faint = 0;
      continue;
    }
    float pk = 0;
    for (int i = 0; i < kBlock; ++i) pk = std::max({pk, std::fabs(L[i]), std::fabs(R[i])});
    const double gap = static_cast<double>(fx->faintGap());  // K-142 (0.7.8): a delay's echo gap
    if (pk > kFaint) {
      if (static_cast<double>(faint) >= std::max(AudioEngine::kRestSelfAfter * kSr, gap)) r.selfSound = true;
      faint = 0;
      continue;
    }
    faint += kBlock;
    if (r.faintRestAt < 0 && !r.selfSound && static_cast<double>(faint) >= std::max(AudioEngine::kRestHoldFaint * kSr, gap))
      r.faintRestAt = (at + kBlock - kSr) / double(kSr);
  }
  r.silentPct = timed ? 100.0 * ns / (timed * kBlock * 1e9 / kSr) : 0;
}

// K-142: an effect whose editor (or a panel) reads its live data never rests while that goes on: the
// utility meters, scopes, clock and Morph EQ, and Loupe (its capture, its playback).
void watchedNeverRests() {
  for (const char* type : {"gl.scope", "gl.spectro", "gl.meter", "gl.clock", "gl.bankeq"}) {
    auto fx = createEffect(type);
    auto* u = dynamic_cast<packs::utility::UtilityFx*>(fx.get());
    expect(u != nullptr, (std::string(type) + ": a utility effect").c_str());
    if (!u) continue;
    expect(u->restsWhenFaint(), (std::string(type) + ": unwatched, it may rest").c_str());
    (void)u->ring(), (void)u->levels(), (void)u->transport(), (void)u->bandTelemetry();
    expect(!u->restsWhenFaint(), (std::string(type) + ": watched, it keeps running").c_str());
  }
  auto fx = createEffect("gl.edison");
  auto* ed = dynamic_cast<packs::edison::EdisonFx*>(fx.get());
  expect(ed != nullptr, "gl.edison: Loupe's runtime");
  if (!ed) return;
  expect(ed->restsWhenFaint(), "Loupe: nothing playing, nobody recording: it may rest");
  (void)ed->capture();
  expect(!ed->restsWhenFaint(), "Loupe: an editor drains its capture: it keeps running");
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  expect(ed->restsWhenFaint(), "Loupe: a second after the last read it may rest again");
  ed->play(0, 0, false);
  expect(!ed->restsWhenFaint(), "Loupe: playing from its window, it keeps running");
  auto sc = createEffect("gl.scope");
  auto* u = dynamic_cast<packs::utility::UtilityFx*>(sc.get());
  (void)u->ring();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  expect(u->restsWhenFaint(), "scope: a second after the last read it may rest again");
}

Row measure(const PluginInfo& pi) {
  Row r;
  r.type = pi.type;
  r.name = pi.name;
  r.kind = pi.kind == PluginKind::Generator ? "instrument" : pi.kind == PluginKind::Effect ? "effect" : "notefx";
  Setup st = makeSetup(pi);
  std::vector<float> L(kBlock), R(kBlock);
  {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setRestInstruments(true);
    auto prog = b.build(st.song);
    if (st.effect) simulateFaintRest(pi, *prog, st.track, r);
    e.setProgram(prog);
    e.setCallCounting(true);
    e.setLooping(false);
    // untouched: 4 s, the last second counted
    for (int at = 0; at < 3 * kSr; at += kBlock) e.process(L.data(), R.data(), kBlock);
    const uint64_t u0 = callsOf(b, st);
    for (int at = 0; at < kSr; at += kBlock) e.process(L.data(), R.data(), kBlock);
    r.untouchedRuns = callsOf(b, st) > u0;
    // the note
    e.noteOn(st.chan, 60, 0.9f);
    const int held = static_cast<int>(heldSeconds(st) * kSr);
    float peak = 0;
    for (int at = 0; at < held; at += kBlock) {
      e.process(L.data(), R.data(), kBlock);
      for (int i = 0; i < kBlock; ++i) peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
    }
    e.noteOff(st.chan, 60);
    const int after = 30 * kSr, window = 2 * kSr;
    int64_t lastNz = -1, last100 = -1, lastCall = -1;
    uint64_t prevCalls = callsOf(b, st);
    const uint64_t blocks0 = 0;
    (void)blocks0;
    uint64_t wCalls0 = 0, wBlocks = 0;
    double ns = 0;
    for (int at = 0; at < after; at += kBlock) {
      const bool inWindow = at >= after - window;
      if (inWindow && wBlocks == 0) wCalls0 = callsOf(b, st);
      const auto t0 = std::chrono::steady_clock::now();
      e.process(L.data(), R.data(), kBlock);
      const auto t1 = std::chrono::steady_clock::now();
      if (inWindow) {
        ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
        ++wBlocks;
      }
      for (int i = 0; i < kBlock; ++i) {
        const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
        peak = std::max(peak, a);
        if (a != 0.f) lastNz = at + i;
        if (a > 1e-5f) last100 = at + i;
      }
      const uint64_t c = callsOf(b, st);
      if (c != prevCalls) lastCall = at + kBlock;
      prevCalls = c;
    }
    r.sound = peak > 0;
    r.peakDb = peak > 0 ? 20 * std::log10(peak) : -999;
    r.tailExact = lastNz < 0 ? -1 : lastNz / double(kSr);
    r.tail100 = last100 < 0 ? -1 : last100 / double(kSr);
    r.restAfter = lastCall < 0 ? -1 : lastCall / double(kSr);
    r.lastCalls = callsOf(b, st) - wCalls0;
    r.lastBlocks = wBlocks;
    r.rests = r.lastCalls == 0;
    r.idlePct = 100.0 * ns / (window * 1e9 / kSr);
  }
  {  // offline: the same note, no resting
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setOffline(true);
    e.setProgram(b.build(st.song));
    e.setLooping(false);
    const int frames = 8 * kSr;
    std::vector<float> oL(static_cast<size_t>(frames)), oR(static_cast<size_t>(frames));
    e.noteOn(st.chan, 60, 0.9f);
    const int held = static_cast<int>(heldSeconds(st) * kSr);
    for (int at = 0; at < frames; at += kBlock) {
      if (at == (held / kBlock) * kBlock) e.noteOff(st.chan, 60);
      e.process(oL.data() + at, oR.data() + at, std::min(kBlock, frames - at));
    }
    r.hash = fnv(oL, oR);
  }
  return r;
}

std::string fmt(double v) {
  if (v < 0) return "-";
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  gl::test::hermeticTestEnv("k142plugins");
  std::string tsv, hashes, compare, only;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--tsv") tsv = argv[++i];
    else if (a == "--hashes") hashes = argv[++i];
    else if (a == "--compare") compare = argv[++i];
    else if (a == "--only") only = argv[++i];
  }
  std::map<std::string, uint64_t> want;
  if (!compare.empty()) {
    std::ifstream f(compare);
    std::string t;
    unsigned long long h;
    while (f >> t >> h) want[t] = h;
  }
  std::vector<Row> rows;
  for (const PluginInfo& pi : pluginCatalog()) {
    if (isHostedType(pi.type)) continue;
    if (!only.empty() && pi.type != only) continue;
    rows.push_back(measure(pi));
    const Row& r = rows.back();
    std::printf("%-22s %-10s sound %d peak %7.1f untouched %d tail %s / %s s, last DSP %s s, rests %d (%llu of %llu), idle %.3f%%, faint rule %s (declares %d, self %d), silence DSP %.3f%%\n",
                r.type.c_str(), r.kind.c_str(), r.sound ? 1 : 0, r.peakDb, r.untouchedRuns ? 1 : 0, fmt(r.tailExact).c_str(),
                fmt(r.tail100).c_str(), fmt(r.restAfter).c_str(), r.rests ? 1 : 0, static_cast<unsigned long long>(r.lastCalls),
                static_cast<unsigned long long>(r.lastBlocks), r.idlePct, fmt(r.faintRestAt).c_str(), r.declares ? 1 : 0, r.selfSound ? 1 : 0, r.silentPct);
    std::fflush(stdout);
  }
  if (!tsv.empty()) {
    std::ofstream f(tsv);
    f << "type\tname\tkind\tsound\tpeak_db\tuntouched_runs\ttail_exact_s\ttail_100db_s\tlast_dsp_s\trests\tidle_calls\tidle_blocks\tidle_pct\tdeclares\tfaint_rest_s\tself_sound\tsilence_dsp_pct\n";
    for (const Row& r : rows)
      f << r.type << '\t' << r.name << '\t' << r.kind << '\t' << r.sound << '\t' << r.peakDb << '\t' << r.untouchedRuns << '\t'
        << fmt(r.tailExact) << '\t' << fmt(r.tail100) << '\t' << fmt(r.restAfter) << '\t' << r.rests << '\t' << r.lastCalls << '\t'
        << r.lastBlocks << '\t' << r.idlePct << '\t' << r.declares << '\t' << fmt(r.faintRestAt) << '\t' << r.selfSound << '\t' << r.silentPct << '\n';
  }
  if (!hashes.empty()) {
    std::ofstream f(hashes);
    for (const Row& r : rows) f << r.type << ' ' << r.hash << '\n';
  }
  int changed = 0;
  for (const Row& r : rows) {
    auto w = want.find(r.type);
    if (w != want.end() && w->second != r.hash) {
      std::printf("render changed: %s\n", r.type.c_str());
      ++changed;
    }
  }
  if (!compare.empty()) expect(changed == 0, ("offline renders bit-identical to " + compare).c_str());
  if (only.empty()) watchedNeverRests();
  // K-142: effects that replay what they caught rest only once all of it is silence (capture
  // length at 44.1 kHz, then the rule's 4 s).
  for (const Row& r : rows) {
    double cap = r.type == "gl.minihalt" ? 524288.0 / kSr : r.type == "gl.chater" ? 1048576.0 / kSr : -1;
    if (cap > 0)
      expect(r.faintRestAt >= cap + AudioEngine::kRestHoldFaint - 1.0 - 0.1 && r.faintRestAt < 30,
             (r.type + ": rests only after its whole capture is silence (" + fmt(r.faintRestAt) + " s)").c_str());
  }
  int bad = 0, waiting = 0;
  for (const Row& r : rows) {
    const bool allowed = mayNotRest().count(r.type) != 0;
    const bool wouldRest = r.declares && r.faintRestAt >= 0 && !r.selfSound;
    // One the rule rests only in the last 4 s (Chater after its 23.8 s capture) counts as resting once the
    // engine stopped calling it before the end of the run.
    const bool rests = r.rests || (wouldRest && r.faintRestAt > 26.0 && r.restAfter >= 0 && r.restAfter < 30.0);
    if (r.declares && !wouldRest) {
      std::printf("says it rests when faint but its output does not stay faint: %s (self sound %d)\n", r.type.c_str(), r.selfSound ? 1 : 0);
      ++bad;
    } else if (!rests && wouldRest) {
      std::printf("says it rests when faint and the rule would rest it, but the engine does not: %s\n", r.type.c_str());
      ++waiting;
      ++bad;
    } else if (!rests && !allowed) {
      std::printf("does not rest: %s (%s)\n", r.type.c_str(), r.name.c_str());
      ++bad;
    } else if (rests && allowed) {
      std::printf("listed as not resting but rests: %s\n", r.type.c_str());
      ++bad;
    }
  }
  if (waiting) std::printf("%d effects say they rest when faint but the engine kept them running\n", waiting);
  expect(bad == 0, (std::to_string(bad) + " plugins break the resting rule").c_str());
  return finish();
}
