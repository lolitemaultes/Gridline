// Gridline — "seltmann" pack tests: the Ribbon editor's pure pieces (seltmann_editor.h).
//   heights      the form from the host's room and the person's choice
//   follow       old stretch modes load as the stretch (Follow: Stretch / Resample / Off); the
//                stretch modes 0..2 render bit-identical, so only Rubber Band states change
//   layouts      a preview leaves the model untouched; one letter system (slots resolved to regions)
//                plays the same regions and renders bit-identical; stars and recent per person
//   key names    per instance: from the base note one name per region, the pattern octave below
//   linked send  a sent pattern follows sequence edits until its notes are edited, Relink, and the
//                link survives a save / load (the form collab sends)
//   takes        a new take keeps the chop
//   editor keys  each key the editor takes is free in every key set
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/Keymap.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "packs/Packs.h"
#include "packs/seltmann/seltmann.h"
#include "packs/seltmann/seltmann_editor.h"
#include "seltmann_synth.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_seltmann_ribbon_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

struct Fixture {
  SamplePtr src;
  std::string path;
  sm::Model base;
};

Fixture& fixture() {
  static Fixture f;
  if (f.src) return f;
  seltsynth::LoopSpec sp;
  sp.bpm = 96;
  sp.bars = 2;
  sp.late = 0.01;
  const auto song = seltsynth::loop(sp);
  f.path = (tmpDir() / "Break 96.wav").string();
  sm::writeLoopWav(f.path, song.sample->left, song.sample->right.vec(), song.sample->sampleRate);
  f.src = sampleCache().get(f.path);
  if (!f.src) return f;
  sm::Analysis a;
  sm::analyse(*f.src, a);
  f.base = sm::defaultModel(*f.src, a);
  return f;
}

bool sameAudio(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// ------------------------------------------------------------------ heights (S0a)
void testHeights() {
  using namespace sm;
  expect(heightClassFor(0, -1) == HeightFull, "heights: a floating editor is Full");
  expect(heightClassFor(168, -1) == HeightCompact, "heights: a 168 px band is Compact");
  expect(heightClassFor(299, -1) == HeightCompact && heightClassFor(300, -1) == HeightStandard, "heights: Compact below 300 px");
  expect(heightClassFor(420, -1) == HeightStandard, "heights: a 420 px pane is Standard");
  expect(heightClassFor(kStandardBelow, -1) == HeightFull && heightClassFor(900, -1) == HeightFull, "heights: Full from 440 px");
  expect(heightClassFor(900, HeightCompact) == HeightCompact && heightClassFor(120, HeightFull) == HeightFull && heightClassFor(0, HeightStandard) == HeightStandard,
         "heights: the person's choice wins over the room");
  bool round = true;
  for (int c = -1; c < HeightCount; ++c) round = round && heightChoice(heightChoiceName(c)) == c;
  expect(round && heightChoice("nonsense") == -1, "heights: the choice is stored by name (auto, compact, standard, full)");
}

// ------------------------------------------------------------------ follow (S0e)
void testFollow() {
  Fixture& f = fixture();
  if (!f.src) {
    expect(false, "follow: test audio");
    return;
  }
  PluginState st = defaultPluginState(sm::kType);
  bool all = true;
  for (int mode = 0; mode <= 3; ++mode) {
    PluginState s = st;
    s.params[sm::SmStretchMode] = static_cast<float>(mode);
    s.params[sm::SmSync] = sm::SyncStretch;
    normalizePluginState(s);
    all = all && s.params[sm::SmStretchMode] == 0 && s.params[sm::SmSync] == sm::SyncStretch;
  }
  expect(all, "follow: every old stretch mode (Auto, Transient, Mono, Rubber Band) loads as the stretch");
  PluginState r = st;
  r.params[sm::SmSync] = sm::SyncResample;
  r.params[sm::SmStretchMode] = 2;
  normalizePluginState(r);
  PluginState o = st;
  o.params[sm::SmSync] = sm::SyncOriginal;
  normalizePluginState(o);
  expect(r.params[sm::SmSync] == sm::SyncResample && o.params[sm::SmSync] == sm::SyncOriginal, "follow: Resample and Off (the sample's tempo) stay");
  // the stretch modes 0..2 always rendered the same stretch: old projects with them sound the same
  sm::RenderOpts ro;
  ro.sampleRate = 44100;
  ro.tempo = 104;
  ro.sync = sm::SyncStretch;
  std::vector<float> L0, R0;
  ro.stretchMode = 0;
  sm::renderPatternLoop(f.base, f.src, ro, L0, R0);
  bool same = !L0.empty();
  for (int mode = 1; mode <= 3; ++mode) {
    std::vector<float> L, R;
    ro.stretchMode = mode;
    sm::renderPatternLoop(f.base, f.src, ro, L, R);
    same = same && sameAudio(L, L0) && sameAudio(R, R0);
  }
  expect(same, "follow: every stored stretch mode renders bit-identical to the stretch");
}

// ------------------------------------------------------------------ layouts (S0c, S0d)
void testLayouts() {
  Fixture& f = fixture();
  if (!f.src) {
    expect(false, "layouts: test audio");
    return;
  }
  const sm::Model before = f.base;
  const std::string beforeText = sm::modelToText(before);
  bool untouched = true, sameAsApply = true, regionsSame = true, noSlots = true, soundSame = true;
  int resolvedSteps = 0;
  sm::RenderOpts ro;
  ro.sampleRate = 44100;
  ro.tempo = 96;
  ro.sync = sm::SyncOriginal;
  int rendered = 0;
  for (const auto& p : sm::factoryPresets()) {
    std::vector<std::pair<int, float>> params;
    const sm::Model pv = sm::previewModel(before, f.src.get(), p, 5, &params);
    untouched = untouched && sm::modelToText(before) == beforeText;
    // the layout as it applied before: applyPreset on the model (slot steps kept)
    sm::Model old = before;
    const auto oldParams = sm::applyPreset(old, f.src.get(), p, 5);
    sameAsApply = sameAsApply && oldParams == params && old.slices == pv.slices && old.pattern.slots == pv.pattern.slots;
    // one letter system: the same regions play, step for step
    const auto ra = sm::stepRegions(old, 1 << 20), rb = sm::stepRegions(pv, 1 << 20);
    if (ra != rb) {
      std::printf("# %s: regions differ\n", p.name.c_str());
      regionsSame = false;
    }
    for (const auto& s : pv.pattern.steps) noSlots = noSlots && (!s.slot || s.src == sm::kRest);
    for (const auto& s : old.pattern.steps) resolvedSteps += s.slot && s.src != sm::kRest ? 1 : 0;
    // and it sounds the same (every layout without a generator; the generated ones are random takes)
    if (!p.gen && rendered < 12) {
      std::vector<float> L1, R1, L2, R2;
      sm::renderPatternLoop(old, f.src, ro, L1, R1);
      sm::renderPatternLoop(pv, f.src, ro, L2, R2);
      soundSame = soundSame && !L1.empty() && sameAudio(L1, L2) && sameAudio(R1, R2);
      ++rendered;
    }
  }
  expect(untouched, "preview: a layout heard on the chop leaves the model untouched");
  expect(sameAsApply, "preview: the previewed model is the layout as it applies (chop, parameters, slots)");
  expect(regionsSame, "one letter system: every factory layout plays the same regions as before");
  expect(noSlots && resolvedSteps > 0, "one letter system: slot steps resolve to region steps on apply");
  expect(soundSame && rendered >= 6, "one letter system: the resolved layouts render bit-identical");
  // resolveSlots keeps the slot table (the Letters key map of old projects)
  sm::Model m = before;
  sm::applyPreset(m, f.src.get(), sm::factoryPresets().front(), 1);
  const auto slots = m.pattern.slots;
  sm::resolveSlots(m);
  expect(m.pattern.slots == slots, "one letter system: the slot table stays for the Letters key map");
  // a step's regions as a card shows them
  const auto chips = sm::stepRegions(before, 4);
  expect(chips.size() == 4 && chips[0] == 0 && chips[1] == 1 && chips[3] == 3, "layout card chips: the regions in step order");
  // stars and recent, per person
  sm::LayoutPrefs pr;
  pr.toggleStar("Instant Flip");
  pr.toggleStar("Soul Stab");
  pr.toggleStar("Instant Flip");
  expect(pr.stars.size() == 1 && pr.starred("Soul Stab") && !pr.starred("Instant Flip"), "stars: a second click takes the star away");
  for (const char* n : {"A", "B", "C", "D", "B"}) pr.used(n);
  expect(pr.recent.size() == 3 && pr.recent[0] == "B" && pr.recent[1] == "D" && pr.recent[2] == "C", "recent: the last three, newest first, no repeats");
  const std::string file = (tmpDir() / "prefs" / "seltmann" / "layouts.txt").string();
  expect(sm::saveLayoutPrefs(file, pr), "stars: saved to the person's config folder");
  const sm::LayoutPrefs back = sm::loadLayoutPrefs(file);
  expect(back.stars == pr.stars && back.recent == pr.recent, "stars: read back the same");
  expect(sm::loadLayoutPrefs((tmpDir() / "nope.txt").string()).stars.empty(), "stars: no file, no stars");
  const sm::LayoutPrefs junk = sm::layoutPrefsFromText("garbage\nstar \nrecent a\nrecent b\nrecent c\nrecent d\n");
  expect(junk.stars.empty() && junk.recent.size() == 3, "stars: a damaged file reads what it can");
}

// ------------------------------------------------------------------ key names (S0f)
void testKeyNames() {
  const auto v = sm::keyNamesFor(60, 4);
  expect(v.size() == 128 && v[60] == "A" && v[61] == "B" && v[63] == "D" && v[64].empty(), "key names: one per region from the base note");
  expect(v[48] == "Pattern" && v[49] == "Pattern +1" && v[59] == "Pattern +11" && v[47].empty(), "key names: the pattern octave below");
  const auto lo = sm::keyNamesFor(5, 3);
  expect(lo[5] == "A" && lo[7] == "C" && lo[0] == "Pattern +7", "key names: a low base note clips the pattern octave");
  Fixture& f = fixture();
  if (!f.src) return;
  PluginState st = defaultPluginState(sm::kType);
  st.data = sm::formatData(f.path, f.base);
  const auto n1 = sm::instanceKeyNames(st);
  const int regions = static_cast<int>(f.base.slices.size());
  expect(static_cast<int>(n1.size()) == 128 && n1[60] == "A" && n1[static_cast<size_t>(59 + regions)] == sm::regionName(regions - 1) &&
             n1[static_cast<size_t>(60 + regions)].empty(),
         "instance key names: the regions of this instance");
  st.params[sm::SmBaseNote] = 48;
  const auto n2 = sm::instanceKeyNames(st);
  expect(n2[48] == "A" && n2[36] == "Pattern" && n2[60] != "A", "instance key names: follow the base note");
  sm::Model fewer = f.base;
  fewer.slices.resize(3);
  fewer.slices.back().end = f.base.slices.back().end;
  st.data = sm::formatData(f.path, fewer);
  const auto n3 = sm::instanceKeyNames(st);
  expect(n3[50] == "C" && n3[51].empty(), "instance key names: follow the region count");
  PluginState other = defaultPluginState("gl.sampler");
  expect(sm::instanceKeyNames(other).empty(), "instance key names: only for Seltmann");
  PluginState bare = defaultPluginState(sm::kType);
  bare.data = f.path;
  expect(sm::instanceKeyNames(bare).empty(), "instance key names: none before the analysis");
  const PluginInfo* info = findPlugin(sm::kType);
  expect(info && info->instanceKeyNames == &sm::instanceKeyNames, "instance key names: the piano roll's hook is set");
}

// ------------------------------------------------------------------ linked send (S0g)
void testLinked() {
  Fixture& f = fixture();
  if (!f.src) return;
  Song s;
  s.tempo = 96;
  Channel c;
  c.id = s.allocateId();
  c.plugin = defaultPluginState(sm::kType);
  c.plugin.data = sm::formatData(f.path, f.base);
  s.channels.push_back(c);
  const ObjectId plain = sm::sendPatternToSong(s, c.id, f.base, c.plugin.params, 1, 0, 0);
  const ObjectId pid = sm::sendPatternToSong(s, c.id, f.base, c.plugin.params, 2, 1, 0, true);
  gl::Pattern* p = nullptr;
  const gl::Pattern* q = nullptr;
  for (auto& x : s.patterns) {
    if (x.id == pid) p = &x;
    if (x.id == plain) q = &x;
  }
  if (!p || !q) {
    expect(false, "linked send: patterns made");
    return;
  }
  expect(q->linkChannel == kNoId && sm::linkState(*q, c.id) == sm::LinkNone, "linked send: a plain send is not linked");
  expect(p->linkChannel == c.id && p->linkCycles == 2 && sm::linkState(*p, c.id) == sm::LinkFollowing, "linked send: the pattern follows the channel");
  expect(p->notes[c.id].size() == 2 * f.base.pattern.steps.size(), "linked send: the same notes as a plain send (2 cycles)");
  // a sequence edit: the pattern takes it
  sm::Model ed = f.base;
  std::swap(ed.pattern.steps[0], ed.pattern.steps[1]);
  const int firstBefore = p->notes[c.id][0].pitch;
  expect(sm::refreshLinked(s, *p, c.id, ed, c.plugin.params), "linked send: a sequence edit re-renders the pattern");
  expect(p->notes[c.id][0].pitch != firstBefore && sm::linkState(*p, c.id) == sm::LinkFollowing, "linked send: ... with the new order, still following");
  expect(!sm::refreshLinked(s, *p, c.id, ed, c.plugin.params), "linked send: nothing to do when it is up to date");
  // edited in the piano roll: it stops following
  p->notes[c.id][0].velocity = 31;
  expect(sm::linkState(*p, c.id) == sm::LinkEdited, "linked send: an edited note stops the link (badge)");
  const gl::Pattern kept = *p;
  expect(!sm::refreshLinked(s, *p, c.id, f.base, c.plugin.params) && *p == kept, "linked send: an edited pattern keeps the edits");
  expect(sm::refreshLinked(s, *p, c.id, f.base, c.plugin.params, true) && sm::linkState(*p, c.id) == sm::LinkFollowing, "linked send: Relink follows again");
  // save / load (the project text collab sends patterns in)
  const std::string text = projectToText(s);
  Song t;
  std::string err;
  expect(loadProjectText(text, "", t, &err), "linked send: the project loads");
  const gl::Pattern* tp = nullptr;
  for (const auto& x : t.patterns)
    if (x.id == pid) tp = &x;
  expect(tp && tp->linkChannel == c.id && tp->linkHash == p->linkHash && tp->linkCycles == 2 && sm::linkState(*tp, c.id) == sm::LinkFollowing,
         "linked send: the link survives a save / load");
  expect(text.find("\"link\"") != std::string::npos && projectToText(t) == text, "linked send: written only for linked patterns, the same text back");
  // a link to a channel that is gone is dropped on load
  Song o2 = s;
  for (auto& x : o2.patterns)
    if (x.id == pid) x.linkChannel = 999999;
  const std::string orphan = projectToText(o2);
  Song u;
  loadProjectText(orphan, "", u, &err);
  bool dropped = true;
  for (const auto& x : u.patterns) dropped = dropped && (x.id != pid || x.linkChannel == kNoId);
  expect(dropped, "linked send: a link to a missing channel is dropped on load");
}

// ------------------------------------------------------------------ takes (S0h)
void testTakes() {
  Fixture& f = fixture();
  if (!f.src) return;
  const sm::Model& m = f.base;
  const int64_t n = m.srcFrames;
  sm::Model same = sm::keepChopOnTake(m, n, m.srcRate);
  expect(same.slices == m.slices && same.pattern == m.pattern && same.regionEnd == m.regionEnd, "takes: a take as long keeps the chop as it is");
  sm::Model longer = sm::keepChopOnTake(m, n * 2, m.srcRate);
  expect(longer.slices == m.slices && longer.srcFrames == n * 2 && longer.pattern == m.pattern, "takes: a longer take keeps every region");
  const int64_t half = (m.slices[m.slices.size() / 2].start + m.slices[m.slices.size() / 2].end) / 2;
  sm::Model shorter = sm::keepChopOnTake(m, half, m.srcRate);
  bool inside = !shorter.slices.empty();
  for (const auto& s : shorter.slices) inside = inside && s.start >= 0 && s.end <= half && s.end > s.start;
  expect(inside && shorter.slices.size() < m.slices.size() && shorter.regionEnd <= half, "takes: a shorter take drops the regions past its end");
  bool stepsOk = !shorter.pattern.steps.empty();
  for (const auto& st : shorter.pattern.steps)
    stepsOk = stepsOk && (st.src == sm::kRest || sm::stepSlice(shorter.pattern, st, static_cast<int>(shorter.slices.size())) < static_cast<int>(shorter.slices.size()));
  expect(stepsOk, "takes: the sequence only plays regions that are left");
  sm::Model rate = sm::keepChopOnTake(m, n * 2, m.srcRate * 2);
  expect(rate.slices.size() == m.slices.size() && rate.slices[1].start == m.slices[1].start * 2 && rate.srcRate == m.srcRate * 2,
         "takes: a take at another rate keeps the cuts where they are in time");
}

// ------------------------------------------------------------------ editor keys (S0i)
void testKeys() {
  int free = 0;
  for (int k = 0; k < sm::KeyCount; ++k) {
    const keys::Chord c = sm::editorChord(k);
    const bool f = sm::editorKeyFree(c);
    free += f ? 1 : 0;
    std::printf("# key %s (%s): %s\n", keys::keyName(c.key), sm::editorKeyLabel(k), f ? "free in every key set" : "taken (menu only)");
  }
  // a chord a key set uses globally is never free: Space (play) in every set
  expect(!sm::editorKeyFree({keys::Key::Space, 0}), "editor keys: Space (play) is never taken");
  // the check matches the key sets one by one
  bool consistent = true;
  for (int k = 0; k < sm::KeyCount; ++k) {
    const keys::Chord c = sm::editorChord(k);
    bool used = false;
    for (int p = 0; p < static_cast<int>(keys::Profile::Count); ++p)
      for (bool sep : {false, true})
        for (const auto& [id, chords] : keys::profileDefaults(static_cast<keys::Profile>(p), sep)) {
          const keys::Command* cmd = keys::findCommand(id);
          if (cmd && cmd->context == keys::Context::Global && std::find(chords.begin(), chords.end(), c) != chords.end()) used = true;
        }
    consistent = consistent && used == !sm::editorKeyFree(c);
  }
  expect(consistent, "editor keys: free means no key set binds the chord globally");
  expect(free >= 1, "editor keys: some of the editor keys are free");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "heights") testHeights();
  if (only.empty() || only == "follow") testFollow();
  if (only.empty() || only == "layouts") testLayouts();
  if (only.empty() || only == "keynames") testKeyNames();
  if (only.empty() || only == "linked") testLinked();
  if (only.empty() || only == "takes") testTakes();
  if (only.empty() || only == "keys") testKeys();
  return finish();
}
