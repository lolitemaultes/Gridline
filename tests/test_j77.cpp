// Gridline — J-77: lit piano-roll keys (engine note state), pad flashes, the
// "Keep this change?" flow (interface size, theme, language, layout, audio),
// tool icon sizes (test_icons) and the channel rack shown when PAT is chosen.
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "Check.h"
#include "app/DisplayConfirm.h"
#include "app/KeyLightState.h"
#include "app/RackReveal.h"
#include "core/Settings.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/NoteActivity.h"
#include "engine/Program.h"

using namespace gl;

namespace {

// ------------------------------------------------------------------ note state bitset
void testNoteActivity() {
  auto na = std::make_unique<NoteActivity>();
  expect(!na->held(60) && na->bits(0) == 0 && na->bits(1) == 0, "nothing held at first");
  na->on(1, 60, 0.5f);
  expect(na->held(60) && na->hits(60) == 1 && std::fabs(na->velocity(60) - 0.5f) < 0.01f, "note on: held, one hit, velocity");
  expect(((na->bits(0) >> 60) & 1u) && na->bits(1) == 0, "key 60: bit 60 of the lower word");
  na->on(9, 100, 1.0f);
  expect(((na->bits(1) >> 36) & 1u) && na->held(100), "key 100: bit 36 of the upper word");
  na->off(9);
  na->on(2, 60, 1.0f);
  na->off(1);
  expect(na->held(60) && na->hits(60) == 2, "two notes on one key: held until both end");
  na->off(2);
  expect(!na->held(60), "both ended: released");
  na->on(3, 10, 1.0f);
  expect(na->held(10) && (na->bits(0) >> 10 & 1u), "low key in the lower word");
  na->on(3, 10, 0.8f);  // same id retriggered
  na->off(3);
  expect(!na->held(10) && na->hits(10) == 2, "a retriggered id is one note (one off releases it)");
  na->on(4, 61.6f, 1.0f);
  expect(na->held(62) && !na->held(61), "fractional pitch lights the nearest key");
  na->retarget(4, 67);
  expect(!na->held(62) && na->held(67), "portamento / legato moves the lit key");
  na->on(5, 200, 1.0f);
  na->on(6, -3, 1.0f);
  expect(na->hits(127) == 0 && !na->held(127), "out-of-range pitches are ignored");
  na->off(999);
  expect(na->held(67), "an unknown note-off changes nothing");
  na->clear();
  expect(!na->held(67) && na->bits(0) == 0 && na->bits(1) == 0 && na->hits(62) == 1, "clear (panic / stop): nothing held, counters kept");
  for (uint32_t i = 0; i < NoteActivity::kSlots + 10; ++i) na->on(100 + i, 40, 1.0f);
  expect(na->held(40) && na->hits(40) == static_cast<uint32_t>(NoteActivity::kSlots + 10), "more notes than slots: still lit, every hit counted");
  for (uint32_t i = 0; i < NoteActivity::kSlots + 10; ++i) na->off(100 + i);
  expect(!na->held(40), "all released after overflow");
}

// Engine: sequenced and live notes set the channel's bits on the audio thread.
void testEngineNoteState() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "synth";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  const ObjectId ch = c.id;
  s.patterns[0].notes[ch] = {Note(0, 96, 60, 100), Note(0, 96, 64, 50)};
  ProgramBuilder builder;
  builder.setSampleRate(48000);
  AudioEngine eng{48000};
  auto prog = builder.build(s);
  eng.setProgram(prog);
  eng.setMode(PlayMode::Pattern);
  eng.setPattern(s.patterns[0].id);
  auto rt = builder.channelRuntime(ch);
  expect(rt != nullptr, "channel runtime");
  if (!rt) return;
  std::vector<float> L(256), R(256);
  auto run = [&](int frames) {
    for (int d = 0; d < frames; d += 256) eng.process(L.data(), R.data(), 256);
  };
  // Live: MIDI / typing keyboard / clicked keys / previews all arrive as engine.noteOn.
  eng.noteOn(ch, 72, 0.5f);
  run(256);
  expect(rt->keys.held(72) && std::fabs(rt->keys.velocity(72) - 0.5f) < 0.01f, "live note: held with its velocity");
  eng.noteOff(ch, 72);
  run(256);
  expect(!rt->keys.held(72) && rt->keys.hits(72) == 1, "live note off: released");
  // Playback.
  eng.play();
  run(2048);
  expect(rt->keys.held(60) && rt->keys.held(64) && !rt->keys.held(62), "playback: the pattern's keys are held");
  expect(rt->keys.velocity(64) < rt->keys.velocity(60), "per-key velocity");
  const double spt = 48000.0 * 60.0 / (s.tempo * s.ppq);
  run(static_cast<int>(spt * 150));
  expect(!rt->keys.held(60) && !rt->keys.held(64), "note ends: keys released");
  const uint32_t h = rt->keys.hits(60);
  run(static_cast<int>(spt * s.barTicks()));
  expect(rt->keys.hits(60) > h, "the pattern loops: a new hit");
  eng.stop();
  run(512);
  expect(rt->keys.bits(0) == 0 && rt->keys.bits(1) == 0, "stop: nothing held");
  eng.noteOn(ch, 50, 1.0f);
  run(256);
  eng.panic();
  run(256);
  expect(!rt->keys.held(50), "panic: nothing held");
}

// ------------------------------------------------------------------ key light / pad flash
void testKeyLightAndFlash() {
  using namespace keylight;
  expect(flashEnvelope(-0.1f, 1) == 0, "no flash before the hit");
  expect(flashEnvelope(0, 1) >= 0.6f, "bright at once (quick attack)");
  expect(std::fabs(flashEnvelope(kFlashAttack, 1) - 1.0f) < 1e-4f, "full at the end of the attack");
  bool falling = true;
  float prev = flashEnvelope(kFlashAttack, 1);
  for (float t = kFlashAttack + 0.02f; t <= kFlashAttack + kFlashDecay; t += 0.02f) {
    const float v = flashEnvelope(t, 1);
    falling = falling && v <= prev + 1e-6f;
    prev = v;
  }
  expect(falling, "fades smoothly (never brightens again)");
  expect(flashEnvelope(kFlashAttack + 0.1f, 1) > 0.6f && flashEnvelope(kFlashAttack + 0.5f, 1) < 0.2f, "ease-out: most of it gone by half a second");
  expect(flashEnvelope(kFlashAttack + kFlashDecay, 1) < 1e-4f && kFlashDecay >= 0.9f && kFlashDecay <= 1.1f, "gone after about a second");
  expect(flashEnvelope(0.1f, 0.2f) < flashEnvelope(0.1f, 1.0f) && flashEnvelope(0.1f, 0.0f) > 0, "scaled by velocity, soft hits still show");

  auto na = std::make_unique<NoteActivity>();
  auto L = std::make_unique<ChannelLights>();
  L->update(*na, 0.016f);
  na->on(1, 36, 1.0f);
  L->update(*na, 0.016f);
  expect(L->key(36) == 1.0f && L->flash(36) >= 0.6f, "hit: key lit, pad flashes");
  for (int i = 0; i < 20; ++i) L->update(*na, 0.016f);
  expect(L->key(36) == 1.0f, "held: stays lit");
  const float mid = L->flash(36);
  expect(mid < 0.8f && mid > 0.1f, "flash fading while the note is held");
  na->on(2, 36, 1.0f);  // re-trigger
  L->update(*na, 0.016f);
  expect(L->flash(36) > mid + 0.2f, "a re-trigger restarts the flash");
  na->off(1), na->off(2);
  L->update(*na, 0.05f);
  expect(L->key(36) > 0.0f && L->key(36) < 1.0f, "released: fading");
  L->update(*na, kKeyRelease);
  expect(L->key(36) == 0.0f, "faded out after the release time");
  for (int i = 0; i < 80; ++i) L->update(*na, 0.016f);
  expect(L->flash(36) == 0.0f, "flash over after about a second");
  // A note that starts and ends between two frames still blinks.
  na->on(3, 40, 0.7f);
  na->off(3);
  L->update(*na, 0.016f);
  expect(L->key(40) == 1.0f && L->flash(40) > 0, "short note between frames: lit and flashed");
  // A UI that starts late does not flash old hits.
  auto L2 = std::make_unique<ChannelLights>();
  L2->update(*na, 0.016f);
  expect(L2->flash(40) == 0.0f && L2->key(40) == 0.0f, "first frame: earlier hits are not replayed");
}

// ------------------------------------------------------------------ Keep / Revert
void testScaleConfirm() {
  using namespace dispconf;
  Settings st;
  Confirm cf;
  // Dragging the slider: nothing applied, nothing asked.
  DeferredValue drag;
  for (double v : {1.05, 1.2, 1.35, 1.5}) drag.edit(v);
  expect(st.getNumber("ui.scale") == 1.0 && !cf.pending() && drag.shown(1.0) == 1.5, "dragging: the slider moves, the interface does not");
  double to = 0;
  expect(drag.release(1.0, &to) && to == 1.5 && !drag.editing, "let go: the chosen value");
  DeferredValue same;
  same.edit(1.0);
  expect(!same.release(1.0, &to), "let go at the same value: nothing to ask");
  // Keep.
  cf.request(scaleChange(st, to), 0.0);
  expect(cf.pending() && st.getNumber("ui.scale") == 1.0, "asked: not applied yet");
  expect(cf.current()->before == "100%" && cf.current()->after == "150%" && cf.current()->preview == Preview::Scale, "before / after sizes");
  cf.keep();
  expect(!cf.pending() && st.getNumber("ui.scale") == 1.5 && cf.last() == Result::Kept, "Keep applies the size");
  // Revert.
  cf.request(scaleChange(st, 0.75), 1.0);
  cf.revert();
  expect(st.getNumber("ui.scale") == 1.5 && cf.last() == Result::Reverted, "Revert: the size stays");
  // Auto-revert.
  cf.request(scaleChange(st, 2.0), 10.0);
  expect(cf.tick(20.0) == Result::None && cf.pending() && std::fabs(cf.remaining(20.0) - 5.0) < 1e-9, "countdown running");
  expect(cf.tick(25.0) == Result::TimedOut && st.getNumber("ui.scale") == 1.5, "15 s without an answer: reverted");
  cf.autoRevert = false;
  cf.request(scaleChange(st, 2.0), 30.0);
  expect(cf.tick(300.0) == Result::None && cf.pending(), "auto-revert off: waits");
  cf.keep();
  expect(st.getNumber("ui.scale") == 2.0, "kept later");
  cf.autoRevert = true;
  // Same setting again while asking: the new value replaces it, the start stays.
  st.set("ui.scale", 1.0);
  cf.request(scaleChange(st, 1.25), 40.0);
  cf.request(scaleChange(st, 1.75), 41.0);
  expect(cf.current()->before == "100%" && cf.current()->after == "175%" && cf.remaining(41.0) == 15.0, "re-chosen: newest value, countdown restarts");
  cf.request(scaleChange(st, 1.0), 42.0);
  expect(!cf.pending() && st.getNumber("ui.scale") == 1.0, "chosen back to the current size: nothing to keep");
  cf.request(scaleChange(st, 1.0), 43.0);
  expect(!cf.pending(), "no change: no dialog");
}

void testApplyFirst() {
  using namespace dispconf;
  Settings st;
  Confirm cf;
  int applied = 0;
  // Theme (mode + contrast): on screen at once, Revert restores everything.
  const Snapshot before = take(st, themeKeys());
  st.set("theme.mode", "light");
  st.set("ui.darkTheme", false);
  st.set("theme.contrast", 0.4);
  const Snapshot after = take(st, themeKeys());
  restore(st, before);
  cf.request(snapshotChange(st, "theme", "Keep this theme?", "Theme", "Dark", "Light", before, after, Preview::Theme, [&] { ++applied; }), 0);
  expect(st.getString("theme.mode") == "light" && st.getNumber("theme.contrast") == 0.4 && applied == 1, "theme: applied at once");
  cf.revert();
  expect(st.getString("theme.mode") == before.vals[0].s && st.getBool("ui.darkTheme") && st.getNumber("theme.contrast") == 0 && applied == 2,
         "theme Revert: earlier mode and contrast back, theme re-applied");
  cf.request(snapshotChange(st, "theme", "Keep this theme?", "Theme", "Dark", "Light", before, after, Preview::Theme, [&] { ++applied; }), 0);
  cf.keep();
  expect(st.getString("theme.mode") == "light" && cf.last() == Result::Kept, "theme Keep");
  restore(st, before);
  cf.request(snapshotChange(st, "theme", "t", "Theme", "Dark", "Light", before, after, Preview::Theme, nullptr), 0);
  expect(cf.tick(15.0) == Result::TimedOut && take(st, themeKeys()) == before, "theme: unconfirmed for 15 s, back to the earlier theme");
  // Quitting while a theme change waits: the earlier theme is what gets saved, without re-applying it.
  int reapplied = applied;
  cf.request(snapshotChange(st, "theme", "t", "Theme", "Dark", "Light", before, after, Preview::Theme, [&] { ++applied; }), 0);
  reapplied = applied;
  cf.abandon();
  expect(take(st, themeKeys()) == before && applied == reapplied && !cf.pending(), "quit while asking: earlier settings, no re-apply");
  // J-129: the control style is a theme key: applied at once, Revert brings the earlier style back.
  {
    const Snapshot cFrom = take(st, themeKeys());
    st.set("theme.controls", "lumen");
    const Snapshot cTo = take(st, themeKeys());
    restore(st, cFrom);
    cf.request(snapshotChange(st, "theme", "t", "Control style", "Slate", "Lumen", cFrom, cTo, Preview::Theme, nullptr), 0);
    expect(st.getString("theme.controls") == "lumen", "control style: applied at once");
    cf.revert();
    expect(st.getString("theme.controls") == "slate", "control style Revert: slate back");
  }
  // Language: applied at once, reverts.
  const Snapshot lFrom = take(st, {{"general.language", 's'}});
  Settings tmp;
  tmp.set("general.language", "pseudo");
  const Snapshot lTo = take(tmp, {{"general.language", 's'}});
  Change lc = snapshotChange(st, "general.language", "Keep this language?", "Display language", "English", "Pseudo", lFrom, lTo, Preview::Language,
                             [&] { ++applied; });
  lc.keepBefore = "Keep";
  cf.request(std::move(lc), 0);
  expect(st.getString("general.language") == "pseudo" && cf.current()->keepBefore == "Keep", "language: applied, old-language button text kept");
  cf.revert();
  expect(st.getString("general.language") == "en", "language Revert");
  // Audio: sample rate + buffer, re-opened on both sides.
  int reopen = 0;
  const Snapshot aFrom = take(st, audioKeys());
  st.set("audio.sampleRate", 96000);
  st.set("audio.bufferFrames", 64);
  const Snapshot aTo = take(st, audioKeys());
  cf.request(snapshotChange(st, "audio", "Keep these audio settings?", "Audio output", "44100", "96000", aFrom, aTo, Preview::Audio, [&] { ++reopen; }), 0);
  expect(reopen == 1, "audio: device re-opened with the new settings");
  cf.tick(16.0);
  expect(st.getNumber("audio.sampleRate") == 44100 && st.getNumber("audio.bufferFrames") == 256 && reopen == 2, "audio: no answer, earlier settings back");
  // Layout reset / device: custom changes (preview-first flag, apply-first closures).
  bool resetLayout = false;
  Change lay;
  lay.id = "ui.layout", lay.before = "Yours", lay.after = "Default", lay.mode = Mode::PreviewFirst;
  lay.apply = [&] { resetLayout = true; };
  cf.request(lay, 0);
  cf.revert();
  expect(!resetLayout, "layout Revert: nothing reset");
  cf.request(lay, 0);
  cf.keep();
  expect(resetLayout, "layout Keep: reset");
  std::string device = "A";
  Change dev;
  dev.id = "audio.device", dev.before = "A", dev.after = "B", dev.mode = Mode::ApplyFirst;
  dev.apply = [&] { device = "B"; };
  dev.revert = [&] { device = "A"; };
  cf.request(dev, 0);
  expect(device == "B", "device: switched at once");
  cf.revert();
  expect(device == "A", "device Revert");
  // A different setting while one waits: an applied change is kept, a preview dropped.
  cf.request(dev, 0);
  cf.request(scaleChange(st, 1.5), 1);
  expect(device == "B" && cf.current()->id == "ui.scale", "another change: the applied one stays");
  cf.request(dev, 2);
  expect(st.getNumber("ui.scale") == 1.0 && cf.current()->id == "audio.device", "another change: a waiting preview is dropped");
  cf.keep();
}

// ------------------------------------------------------------------ PAT shows the channel rack
void testRackReveal() {
  RackReveal r;
  using A = RackReveal::Action;
  expect(r.step(PlayMode::Pattern, true) == A::None, "already in pattern mode: nothing");
  expect(r.step(PlayMode::Song, true) == A::None, "SONG leaves the rack alone");
  expect(r.step(PlayMode::Pattern, false) == A::Show, "PAT with the rack closed: shown (where it was)");
  r.step(PlayMode::Live, true);
  expect(r.step(PlayMode::Pattern, true) == A::Front, "PAT with the rack open: brought forward, not moved");
  expect(r.step(PlayMode::Pattern, true) == A::None, "stays in PAT: once only");
  r.step(PlayMode::Song, true);
  r.quietSwitch(PlayMode::Pattern);
  expect(r.step(PlayMode::Pattern, false) == A::None, "project load / piano roll scrub: no rack");
}

}  // namespace

int main() {
  testNoteActivity();
  testEngineNoteState();
  testKeyLightAndFlash();
  testScaleConfirm();
  testApplyFirst();
  testRackReveal();
  return finish();
}
