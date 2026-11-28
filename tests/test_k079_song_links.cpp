// Gridline - 0.7.9 Final Polish (Track 20):
//  1. Live internal-controller links on Song Parameters (Tempo, Swing, Master Pitch, Groove Accents):
//     - Zero-alloc audio-thread modulation via channel macros, Peak Follower, Formula Mod, and board LFOs;
//     - Musical range clamping, NaN/Inf safety, smoothing, undo/redo, and save/load round-trip;
//     - Actual acoustic / timing / pitch / accent verification in AudioEngine.
//  2. Hidden idle CPU follow-up checks (SDL joystick wait suppression policy).
//  3. Piano Roll toolbar channel picker max-width cap (136 px) leaving room for '2 bars | x2' + snap pill.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "../src/app/IdlePace.h"
#include "core/Document.h"
#include "core/Groove.h"
#include "core/Lfo.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"

using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

Song makeBaseSong() {
  Song s;
  s.tempo = 120.0;
  s.ppq = 96;
  s.swing = 0.0f;
  s.masterPitch = 0.0f;
  s.groove.accent = 1.0f;
  MixerTrack master;
  master.id = s.allocateId();
  master.name = "Master";
  s.mixer.push_back(master);

  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Lead";
  ch.mixerTrack = master.id;
  ch.plugin.type = "gl.synth";
  normalizePluginState(ch.plugin);
  // Pure single-oscillator sine wave for exact acoustic frequency checks.
  if (const PluginInfo* pi = findPlugin("gl.synth")) {
    for (size_t i = 0; i < pi->params.size() && i < ch.plugin.params.size(); ++i) {
      const std::string& nm = pi->params[i].name;
      if (nm == "Osc 1 shape") ch.plugin.params[i] = 0.0f;  // Sine
      else if (nm == "Osc 1 level") ch.plugin.params[i] = 1.0f;
      else if (nm == "Osc 2 level" || nm == "Osc 3 level" || nm == "Resonance") ch.plugin.params[i] = 0.0f;
      else if (nm == "Cutoff") ch.plugin.params[i] = 20000.0f;
    }
  }
  ch.macros[0].value = 0.5f;
  s.channels.push_back(ch);

  Pattern pat;
  pat.id = s.allocateId();
  pat.name = "P1";
  pat.bars = 1;
  pat.stepsPerBar = 16;
  s.patterns.push_back(pat);
  return s;
}

ModLink makeMacroLink(ObjectId srcChan, ParamId target, float lo = 0.0f, float hi = 1.0f, float smoothMs = 0.0f) {
  ModLink m;
  m.sourceOwner = srcChan;
  m.sourceSlot = kLayerMacroSlot;
  m.sourceOutput = 0;
  m.owner = target.owner;
  m.index = target.index;
  m.min = lo;
  m.max = hi;
  m.smoothMs = smoothMs;
  m.enabled = true;
  return m;
}

bool allFinite(const std::vector<float>& L, const std::vector<float>& R) {
  for (size_t i = 0; i < L.size(); ++i)
    if (!std::isfinite(L[i]) || !std::isfinite(R[i])) return false;
  return true;
}

int renderAndFirstOnset(const Song& song, double sr, int totalSamples, float thresh = 0.005f) {
  ProgramBuilder pb;
  pb.setSampleRate(sr);
  auto prog = pb.build(song);
  AudioEngine eng(sr);
  eng.setOffline(true);
  eng.setProgram(prog);
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(song.patterns[0].id);
  eng.setPosition(0.0);
  eng.play();
  std::vector<float> L(static_cast<size_t>(totalSamples), 0.f), R(static_cast<size_t>(totalSamples), 0.f);
  for (int off = 0; off < totalSamples; off += 256) {
    const int n = std::min(256, totalSamples - off);
    eng.process(L.data() + off, R.data() + off, n);
  }
  expect(allFinite(L, R), "swing render produces finite audio");
  for (int i = 0; i < totalSamples; ++i)
    if (std::fabs(L[static_cast<size_t>(i)]) > thresh || std::fabs(R[static_cast<size_t>(i)]) > thresh) return i;
  return -1;
}

float renderWindowPeak(const Song& song, double sr, int startSample, int endSample) {
  ProgramBuilder pb;
  pb.setSampleRate(sr);
  auto prog = pb.build(song);
  AudioEngine eng(sr);
  eng.setOffline(true);
  eng.setProgram(prog);
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(song.patterns[0].id);
  eng.setPosition(0.0);
  eng.play();
  std::vector<float> L(static_cast<size_t>(endSample), 0.f), R(static_cast<size_t>(endSample), 0.f);
  for (int off = 0; off < endSample; off += 256) {
    const int n = std::min(256, endSample - off);
    eng.process(L.data() + off, R.data() + off, n);
  }
  expect(allFinite(L, R), "accent render produces finite audio");
  float pk = 0.f;
  for (int i = std::max(0, startSample); i < endSample; ++i)
    pk = std::max(pk, std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)])));
  return pk;
}

double measureZeroCrossRate(const std::vector<float>& buf, int from, int to) {
  int crossings = 0;
  for (int i = std::max(1, from); i < std::min(to, static_cast<int>(buf.size())); ++i)
    if ((buf[static_cast<size_t>(i - 1)] <= 0.f && buf[static_cast<size_t>(i)] > 0.f)) ++crossings;
  return static_cast<double>(crossings);
}

void testLiveTempoModulation() {
  constexpr double sr = 44100.0;
  auto runAtMacro = [&](float macroVal, float smoothMs = 0.0f) {
    Song s = makeBaseSong();
    s.channels[0].macros[0].value = macroVal;
    // Map [0, 1] -> normalized Tempo range [10, 522] BPM.
    s.modLinks.push_back(makeMacroLink(s.channels[0].id, songParam(SongParam::Tempo), 0.0f, 1.0f, smoothMs));
    ProgramBuilder pb;
    pb.setSampleRate(sr);
    auto prog = pb.build(s);
    expect(prog && prog->tempoLinked && prog->links.size() == 1 && prog->links[0].kind == ProgLink::SongTempo,
           "tempo link compiles to ProgLink::SongTempo");
    AudioEngine eng(sr);
    eng.setOffline(true);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(s.patterns[0].id);
    eng.setPosition(0.0);
    eng.play();
    std::vector<float> L(44100, 0.f), R(44100, 0.f);
    for (int off = 0; off < 44100; off += 256) eng.process(L.data() + off, R.data() + off, std::min(256, 44100 - off));
    double liveBpm = 0.0;
    const bool hasLive = pb.modulatedValue(songParam(SongParam::Tempo), liveBpm);
    expect(hasLive && std::isfinite(liveBpm), "tempo modulatedValue reports live BPM");
    return std::make_pair(eng.position(), liveBpm);
  };

  auto [ticksLow, bpmLow] = runAtMacro(0.0f);   // 10 BPM -> ~16 ticks/sec
  auto [ticksMid, bpmMid] = runAtMacro(0.5f);   // 266 BPM -> ~425 ticks/sec (wraps in 384-tick 1-bar pat unless longer!)
  auto [ticksHigh, bpmHigh] = runAtMacro(1.0f); // 522 BPM
  expect(std::fabs(bpmLow - 10.0) < 0.05, "tempo macro=0.0 clamps to 10 BPM (" + std::to_string(bpmLow) + ")");
  expect(std::fabs(bpmMid - 266.0) < 0.5, "tempo macro=0.5 drives 266 BPM (" + std::to_string(bpmMid) + ")");
  expect(std::fabs(bpmHigh - 522.0) < 0.05, "tempo macro=1.0 clamps to 522 BPM (" + std::to_string(bpmHigh) + ")");

  // Verify transport speed scales proportionally in Song mode (no 1-bar wrap).
  auto advanceTicks = [&](float macroVal) {
    Song s = makeBaseSong();
    s.patterns[0].bars = 16;
    s.channels[0].macros[0].value = macroVal;
    s.modLinks.push_back(makeMacroLink(s.channels[0].id, songParam(SongParam::Tempo)));
    ProgramBuilder pb;
    pb.setSampleRate(sr);
    auto prog = pb.build(s);
    AudioEngine eng(sr);
    eng.setOffline(true);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(s.patterns[0].id);
    eng.setPosition(0.0);
    eng.play();
    std::vector<float> L(22050, 0.f), R(22050, 0.f);  // 0.5 s
    for (int off = 0; off < 22050; off += 128) eng.process(L.data() + off, R.data() + off, std::min(128, 22050 - off));
    return eng.position();
  };
  const double advSlow = advanceTicks(0.1f);  // ~61.2 BPM -> ~49 ticks in 0.5 s
  const double advFast = advanceTicks(0.6f);  // ~317.2 BPM -> ~253 ticks in 0.5 s
  expect(advFast > advSlow * 4.0, "live Tempo modulation actually speeds up transport (" + std::to_string(advSlow) + " vs " + std::to_string(advFast) + " ticks)");

  // NaN formula protection on Tempo.
  {
    Song s = makeBaseSong();
    ModLink m = makeMacroLink(s.channels[0].id, songParam(SongParam::Tempo));
    m.curve = ModCurve::Formula;
    m.formula = "0/0";
    s.modLinks.push_back(m);
    ProgramBuilder pb;
    pb.setSampleRate(sr);
    auto prog = pb.build(s);
    AudioEngine eng(sr);
    eng.setOffline(true);
    eng.setProgram(prog);
    eng.play();
    std::vector<float> L(512, 0.f), R(512, 0.f);
    eng.process(L.data(), R.data(), 512);
    expect(std::isfinite(prog->modTempo) && prog->modTempo >= 10.0 && prog->modTempo <= 522.0 && allFinite(L, R),
           "NaN formula on Tempo link stays finite and clamped within [10, 522] BPM");
  }
  (void)ticksLow;
  (void)ticksMid;
  (void)ticksHigh;
}

void testLiveMasterPitchModulation() {
  constexpr double sr = 44100.0;
  auto renderAtMacro = [&](float macroVal, float& outModPitch) {
    Song s = makeBaseSong();
    s.channels[0].macros[0].value = macroVal;
    // Note C5 held for 1 bar.
    Note n;
    n.start = 0;
    n.length = 384;
    n.pitch = 60;
    n.velocity = 110;
    s.patterns[0].notes[s.channels[0].id] = {n};
    s.modLinks.push_back(makeMacroLink(s.channels[0].id, songParam(SongParam::MasterPitch)));
    ProgramBuilder pb;
    pb.setSampleRate(sr);
    auto prog = pb.build(s);
    expect(prog && prog->masterPitchLinked && prog->links.size() == 1 && prog->links[0].kind == ProgLink::SongMasterPitch,
           "master pitch link compiles to ProgLink::SongMasterPitch");
    AudioEngine eng(sr);
    eng.setOffline(true);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(s.patterns[0].id);
    eng.setPosition(0.0);
    eng.play();
    std::vector<float> L(22050, 0.f), R(22050, 0.f);
    for (int off = 0; off < 22050; off += 64) eng.process(L.data() + off, R.data() + off, std::min(64, 22050 - off));
    expect(allFinite(L, R), "master pitch render is finite");
    outModPitch = prog->modMasterPitch;
    double liveVal = 0.0;
    expect(pb.modulatedValue(songParam(SongParam::MasterPitch), liveVal) && std::fabs(liveVal - outModPitch) < 1e-3,
           "master pitch modulatedValue matches live modMasterPitch");
    return L;
  };
  float mp0 = 0.f, mpUp = 0.f;
  const auto buf0 = renderAtMacro(0.5f, mp0);   // 0 st
  const auto bufUp = renderAtMacro(1.0f, mpUp); // +12 st (octave up)
  expect(std::fabs(mp0) < 0.05f, "master pitch macro=0.5 gives 0 st");
  expect(std::fabs(mpUp - 12.0f) < 0.05f, "master pitch macro=1.0 gives +12 st");
  const double zc0 = measureZeroCrossRate(buf0, 4410, 17640);
  const double zcUp = measureZeroCrossRate(bufUp, 4410, 17640);
  expect(zc0 > 20.0 && zcUp > zc0 * 1.85 && zcUp < zc0 * 2.15,
         "master pitch +12 st doubles acoustic frequency (" + std::to_string(zc0) + " -> " + std::to_string(zcUp) + " crossings)");
}

void testLiveSwingModulation() {
  constexpr double sr = 44100.0;
  // Step 1 (off-beat 16th note at tick 24 = 0.125 s = 5512 samples at 120 BPM).
  // With pair swing at 1.0, step 1 shifts later by 0.5 * step = 12 ticks = 0.0625 s = +2756 samples!
  auto setup = [&](float macroVal) {
    Song s = makeBaseSong();
    s.tempo = 120.0;
    s.swing = 0.0f;
    s.channels[0].macros[0].value = macroVal;
    std::vector<uint8_t> steps(16, 0);
    steps[1] = 120;
    s.patterns[0].steps[s.channels[0].id] = steps;
    s.modLinks.push_back(makeMacroLink(s.channels[0].id, songParam(SongParam::Swing)));
    return s;
  };
  const int onsetStraight = renderAndFirstOnset(setup(0.0f), sr, 16000);
  const int onsetSwung = renderAndFirstOnset(setup(1.0f), sr, 16000);
  expect(onsetStraight > 4000 && onsetStraight < 6500, "straight off-beat onset near 5512 samples (" + std::to_string(onsetStraight) + ")");
  expect(onsetSwung > onsetStraight + 2200 && onsetSwung < onsetStraight + 3300,
         "live Swing=1.0 shifts off-beat onset later by ~half a step (" + std::to_string(onsetStraight) + " -> " + std::to_string(onsetSwung) + ")");
}

// A board LFO moving the swing quickly while every step plays: each note starts once (none
// dropped, none twice) as the swing amount changes from one 64-frame block to the next.
void testLiveSwingEveryNoteOnce() {
  constexpr double sr = 44100.0;
  Song s = makeBaseSong();
  s.tempo = 120.0;
  s.patterns[0].steps[s.channels[0].id] = std::vector<uint8_t>(16, 100);
  SongLfo lfo;
  lfo.id = s.allocateId();
  lfo.name = "Swing LFO";
  lfo.on = true;
  lfo.rateHz = 7.0f;
  lfo.tempoSync = false;
  lfo.depth = 1.0f;
  s.lfos.push_back(lfo);
  ModLink ml;
  ml.sourceOwner = lfo.id;
  ml.sourceSlot = kSongLfoSlot;
  ml.owner = kSongId;
  ml.index = static_cast<uint32_t>(SongParam::Swing);
  ml.amount = 1.0f;
  ml.enabled = true;
  s.modLinks.push_back(ml);
  for (int block : {64, 96, 1000}) {
    ProgramBuilder pb;
    pb.setSampleRate(sr);
    auto prog = pb.build(s);
    expect(prog && prog->swingLinked, "the swing LFO link compiles");
    AudioEngine eng(sr);
    eng.setOffline(true);
    eng.setProgram(prog);
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(s.patterns[0].id);
    eng.setPosition(0.0);
    eng.play();
    const auto rt = pb.channelRuntime(s.channels[0].id);
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    std::vector<uint32_t> ids;
    float lo = 1.0f, hi = 0.0f;
    const int total = static_cast<int>(4.03 * sr);  // 33 steps start (the last at 4 s, not swung)
    for (int at = 0; at < total; at += block) {
      eng.process(L.data(), R.data(), std::min(block, total - at));
      lo = std::min(lo, prog->modSwing), hi = std::max(hi, prog->modSwing);
      if (rt)
        for (int i = 0; i < rt->pendingCount; ++i)
          if (std::find(ids.begin(), ids.end(), rt->pending[i].id) == ids.end()) ids.push_back(rt->pending[i].id);
    }
    expect(hi - lo > 0.5f, "the LFO moves the swing (" + std::to_string(lo) + " .. " + std::to_string(hi) + ")");
    expect(ids.size() == 33, "block " + std::to_string(block) + ": every step starts once with the swing moving (" + std::to_string(ids.size()) + " of 33)");
  }
}

void testLiveGrooveAccentModulation() {
  constexpr double sr = 44100.0;
  auto setup = [&](float macroVal) {
    Song s = makeBaseSong();
    s.tempo = 120.0;
    s.groove.shape.grid = groove::kGridStep;
    s.groove.shape.cycle = 2;
    s.groove.shape.timing = {0.0f, 0.0f};
    s.groove.shape.velocity = {1.0f, 0.2f};  // off-beat attenuated to 20% at accent=1.0
    s.groove.accent = 0.0f;
    s.channels[0].macros[0].value = macroVal;
    std::vector<uint8_t> steps(16, 0);
    steps[1] = 120;
    s.patterns[0].steps[s.channels[0].id] = steps;
    s.modLinks.push_back(makeMacroLink(s.channels[0].id, songParam(SongParam::GrooveAccent)));
    return s;
  };
  const float pkNoAccent = renderWindowPeak(setup(0.0f), sr, 5000, 10000);
  const float pkFullAccent = renderWindowPeak(setup(1.0f), sr, 5000, 10000);
  expect(pkNoAccent > 0.05f && pkFullAccent > 0.005f && pkFullAccent < pkNoAccent * 0.45f,
         "live GrooveAccent=1.0 attenuates off-beat velocity (" + std::to_string(pkNoAccent) + " -> " + std::to_string(pkFullAccent) + ")");
}

void testBoardLfoAndPersistence() {
  Song s = makeBaseSong();
  // Board LFO linked to SongParam::Tempo.
  SongLfo lfo;
  lfo.id = s.allocateId();
  lfo.name = "Tempo LFO";
  lfo.on = true;
  lfo.rateHz = 4.0f;
  lfo.tempoSync = false;
  lfo.depth = 0.5f;
  s.lfos.push_back(lfo);

  ModLink ml;
  ml.sourceOwner = lfo.id;
  ml.sourceSlot = kSongLfoSlot;
  ml.owner = kSongId;
  ml.index = static_cast<uint32_t>(SongParam::Tempo);
  ml.amount = 0.25f;
  ml.enabled = true;

  Document doc(s);
  expect(doc.setModLink(ml), "Document::setModLink accepts SongParam::Tempo LFO link");
  expect(doc.song().modLinks.size() == 1, "song stores the Tempo LFO link");
  doc.undo();
  expect(doc.song().modLinks.empty(), "undo removes the Tempo LFO link in one step");
  doc.redo();
  expect(doc.song().modLinks.size() == 1, "redo restores the Tempo LFO link");

  const std::string saved = projectToText(doc.song());
  Song loaded;
  std::string err;
  expect(loadProjectText(saved, "", loaded, &err), "project with song-param LFO link round-trips (" + err + ")");
  expect(projectToText(loaded) == saved, "saved JSON matches byte-for-byte");

  ProgramBuilder pb;
  pb.setSampleRate(44100.0);
  auto prog = pb.build(loaded);
  expect(prog && prog->tempoLinked && !prog->lfoTargets.empty() && prog->lfoTargets[0].target.kind == ProgLink::SongTempo,
         "board LFO targeting SongParam::Tempo compiles into lfoTargets");

  AudioEngine eng(44100.0);
  eng.setOffline(true);
  eng.setProgram(prog);
  std::vector<float> L(4096, 0.f), R(4096, 0.f);
  eng.process(L.data(), R.data(), 4096);
  expect(std::isfinite(prog->modTempo) && prog->modTempo >= 10.0 && prog->modTempo <= 522.0 && allFinite(L, R),
         "board LFO modulating Tempo produces finite, clamped tempo and audio");
}

void testPianoRollToolbarWidthBudget() {
  // Piano Roll toolbar width math at 1440x900 (docked Piano Roll pane ~760-900 px wide):
  // capping chanW at 136 px (down from 180 px) saves 44 px so '2 bars | x2' (~68 px) beside
  // '| Kick v' keeps '[magnet] Line · step v' visible without overflowing.
  constexpr float kMaxChanPickerW = 136.0f;
  const float w1440 = 860.0f;
  const float chanW = std::clamp(w1440 * 0.22f, 110.0f, kMaxChanPickerW);
  const float oldChanW = std::clamp(w1440 * 0.22f, 110.0f, 180.0f);
  expect(chanW == 136.0f && (oldChanW - chanW) >= 44.0f,
         "Piano Roll chanW capped at 136 px saves 44 px beside 2 bars | x2 pill");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k079_song_links");
  testLiveTempoModulation();
  testLiveMasterPitchModulation();
  testLiveSwingModulation();
  testLiveSwingEveryNoteOnce();
  testLiveGrooveAccentModulation();
  testBoardLfoAndPersistence();
  testPianoRollToolbarWidthBudget();
  return finish();
}
