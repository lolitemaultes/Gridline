// Gridline — chord progression editor window (J-31). See ChordEditorUi.h.
//
// Layout follows the reference tool's grouping: a top strip (target, key,
// scale, octave, names, chord count and length, undo / redo), the generator
// row (adventure, generate, analyze, preview, what gets written), the chord
// lane (one block per chord; click, drag to swap, drag the right edge to
// resize, right-click for the chord menu), then performance and preset
// panels. Edits write into the pattern as they happen, merged into one undo
// step; Accept keeps them, Cancel puts the pattern back.
#include "../core/I18n.h"
#include "ChordEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../core/ChordTools.h"
#include "../core/NoteTools.h"
#include "App.h"
#include "ContextMenus.h"
#include "J31Ui.h"
#include "RunLatch.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::j31 {

namespace ch = gl::chord;
using theme::pal;

namespace {

struct Session {
  bool open = false;
  ObjectId pat = kNoId, chan = kNoId;
  std::vector<Note> base;  // the pattern's notes before the editor wrote (unlinked chords baked in)
  std::vector<Note> written;  // the notes as the editor last left them (edits made elsewhere since are kept: J-36)
  uint64_t key = 0;
  bool changed = false;
  ch::ChordProgression prog;
  std::vector<ch::ChordProgression> undo, redo;
  std::set<size_t> sel;
  bool roman = false;
  double adventure = 0.35;
  uint32_t seed = 1;
  int newLen = 2;  // 0 beat, 1 two beats, 2 bar, 3 two bars
  int snap = 1;    // chord length snap: 0 step, 1 beat, 2 bar
  ch::NonChordMode mode = ch::NonChordMode::Keep;
  bool withMain = true, withBass = true;
  bool live = true;
  int startBar = 0;
  std::vector<ch::Chord> clip;
  int preset = 0;
  char text[256] = {};
  bool learn = false, autoLearn = false;
  std::set<int> held;
  std::vector<int> learned;  // the most notes held at once since the last release of all
  // Preview: progression playback and the chord held by a click.
  RunLatch pv;  // J-110: Stop / panic / focus loss end the preview (the engine released its notes)
  double pvT0 = 0;
  Tick pvFrom = 0;
  int pvChord = -1;
  std::vector<int> pvHeld, pressHeld;
  int altFor = -1;
  std::vector<ch::Chord> alts;
  int drag = -1, dragKind = 0;  // 0 move, 1 resize
  float dragX = 0;
  Tick dragLen = 0;
  bool dragging = false;
  int typeFor = -1;
};
Session S;
uint64_t sessionCounter = 0;

Tick lenTicks(const Song& s, int k) {
  switch (k) {
    case 0: return s.beatTicks();
    case 1: return 2 * s.beatTicks();
    case 3: return 2 * s.barTicks();
    default: return s.barTicks();
  }
}
Tick snapTicks(const Song& s) { return S.snap == 0 ? std::max<Tick>(1, s.ppq / 4) : S.snap == 2 ? s.barTicks() : s.beatTicks(); }
Tick startTick(const Song& s) { return s.barTicks() * S.startBar; }

std::string chordLabel(const ch::Chord& c) {
  return S.roman ? ch::romanName(c, S.prog.key, S.prog.scale) : ch::chordName(c, false);
}

// ------------------------------------------------------------------ writing

void writeNow(App& a) {
  if (!S.live) return;
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(S.pat);
  if (!p || !s.channel(S.chan)) return;
  ch::WriteOpts o;
  o.start = startTick(s);
  o.mode = S.mode;
  o.withMain = S.withMain;
  o.withBass = S.withBass;
  auto it = p->notes.find(S.chan);
  const std::vector<Note> cur = it == p->notes.end() ? std::vector<Note>{} : it->second;
  S.base = ch::rebase(S.base, S.written, cur);  // notes drawn / scripted / undone meanwhile stay
  std::vector<Note> notes = ch::write(S.base, S.prog, o);
  if (notes != cur) {
    a.doc.setNotes(S.pat, S.chan, notes, "chord progression", S.key);
    S.changed = true;
  }
  S.written = notes;
}

// One editor edit: remembered for the editor's own undo, then written.
void edit(App& a, const std::function<void(ch::ChordProgression&)>& fn) {
  ch::ChordProgression before = S.prog;
  fn(S.prog);
  if (S.prog == before) return;
  S.undo.push_back(std::move(before));
  if (S.undo.size() > 200) S.undo.erase(S.undo.begin());
  S.redo.clear();
  for (auto it = S.sel.begin(); it != S.sel.end();) it = *it >= S.prog.chords.size() ? S.sel.erase(it) : std::next(it);
  S.alts.clear();
  S.altFor = -1;
  writeNow(a);
}

// Edits every selected chord (or chord i when it is not selected).
void editChords(App& a, size_t i, const std::function<void(ch::Chord&)>& fn) {
  edit(a, [&](ch::ChordProgression& p) {
    if (S.sel.count(i)) {
      for (size_t k : S.sel)
        if (k < p.chords.size()) fn(p.chords[k]);
    } else if (i < p.chords.size()) {
      fn(p.chords[i]);
    }
  });
}

// Unlinked chords leave their notes behind as ordinary notes.
void setLinked(App& a, size_t i, bool link) {
  std::vector<size_t> which;
  if (S.sel.count(i))
    which.assign(S.sel.begin(), S.sel.end());
  else
    which.push_back(i);
  const Song& s = a.doc.song();
  if (!link) {
    // Bake: the current pattern minus the notes the still-linked chords render.
    const Pattern* p = s.pattern(S.pat);
    std::vector<Note> cur;
    if (p)
      if (auto it = p->notes.find(S.chan); it != p->notes.end()) cur = it->second;
    ch::ChordProgression after = S.prog;
    for (size_t k : which)
      if (k < after.chords.size()) after.chords[k].linked = false;
    for (size_t k = 0; k < after.chords.size(); ++k) {
      if (!after.chords[k].linked) continue;
      for (const Note& n : ch::renderChord(after, k, startTick(s), S.withMain, S.withBass)) {
        auto f = std::find_if(cur.begin(), cur.end(), [&](const Note& q) { return q.start == n.start && q.pitch == n.pitch && q.length == n.length; });
        if (f != cur.end()) cur.erase(f);
      }
    }
    S.base = cur;
  }
  edit(a, [&](ch::ChordProgression& p) {
    for (size_t k : which)
      if (k < p.chords.size()) p.chords[k].linked = link;
  });
}

// ------------------------------------------------------------------ preview

void releaseHeld(App& a, std::vector<int>& held) {
  for (int p : held) a.engine.noteOff(S.chan, p);
  held.clear();
}

// own: the progression preview (J-112: plugin playback, which Play stops
// while the transport is stopped); otherwise a chord held by a click.
void soundChord(App& a, size_t i, std::vector<int>& held, bool own = false) {
  releaseHeld(a, held);
  if (i >= S.prog.chords.size()) return;
  const auto v = ch::voice(S.prog, i);
  const ch::Chord& c = S.prog.chords[i];
  std::vector<int> ps;
  if (c.main && S.withMain) ps = v.main;
  if (c.bass && S.withBass && v.bass >= 0) ps.push_back(v.bass);
  for (int p : ps) {
    const float vel = static_cast<float>(S.prog.perf.velocity) / 127.0f;
    if (own) a.engine.ownNoteOn(S.chan, p, vel, AudioEngine::OwnPlay::ChordPreview);
    else a.engine.noteOn(S.chan, p, vel);
    held.push_back(p);
  }
}

void startPreview(App& a, size_t from) {
  S.pv.start(a.engine);
  S.pvT0 = a.now;
  S.pvFrom = ch::chordStart(S.prog, from);
  S.pvChord = -1;
}

void stopPreview(App& a) {
  S.pv.stop();
  S.pvChord = -1;
  releaseHeld(a, S.pvHeld);
}

void tickPreview(App& a) {
  if (S.pv.sync(a.engine, true) != RunLatch::End::None) {  // stopped from outside: its notes are already released
    S.pvChord = -1;
    S.pvHeld.clear();
    return;
  }
  if (!S.pv.on) return;
  const Song& s = a.doc.song();
  const double ticks = (a.now - S.pvT0) * s.tempo * s.ppq / 60.0 + static_cast<double>(S.pvFrom);
  const int i = ch::chordAt(S.prog, static_cast<Tick>(ticks));
  if (i < 0) {
    stopPreview(a);
    return;
  }
  if (i != S.pvChord) {
    S.pvChord = i;
    soundChord(a, static_cast<size_t>(i), S.pvHeld, true);
  }
}

// ------------------------------------------------------------------ session

void closeEditor(App& a, bool keep) {
  stopPreview(a);
  releaseHeld(a, S.pressHeld);
  if (!keep && S.changed) {
    std::vector<Note> cur;
    if (const Pattern* p = a.doc.song().pattern(S.pat))
      if (auto it = p->notes.find(S.chan); it != p->notes.end()) cur = it->second;
    a.doc.setNotes(S.pat, S.chan, ch::rebase(S.base, S.written, cur), "cancel chord progression", S.key);
  }
  a.doc.endGesture();
  S.open = false;
  S.changed = false;
}

void beginSession(App& a) {
  const Song& s = a.doc.song();
  S.pat = a.curPattern;
  S.chan = a.selChannel;
  S.base.clear();
  if (const Pattern* p = s.pattern(S.pat))
    if (auto it = p->notes.find(S.chan); it != p->notes.end()) S.base = it->second;
  S.written = S.base;
  S.key = 0x43485244ull ^ (++sessionCounter << 20);
  S.changed = false;
  S.undo.clear();
  S.redo.clear();
  S.sel.clear();
  S.alts.clear();
  S.altFor = -1;
  if (S.prog.chords.empty() || S.prog.chordLength <= 0) {
    const int key = a.piano.scale > 0 ? a.piano.scaleRoot : 0;
    const int scale = a.piano.scale > 0 ? a.piano.scale : 1;
    S.prog = ch::generate(key, scale, 4, lenTicks(s, S.newLen), 0.0, 1);
    for (auto& c : S.prog.chords) c = ch::diatonicChord(key, scale, 1);
    const int deg[4] = {1, 5, 6, 4};
    for (size_t k = 0; k < 4; ++k) S.prog.chords[k] = ch::diatonicChord(key, scale, deg[k]);
  } else {
    for (auto& c : S.prog.chords) c.linked = true;
  }
  S.prog.chordLength = std::max<Tick>(1, S.prog.chordLength);
  S.open = true;
}

// ------------------------------------------------------------------ menus

void chordMenu(App& a, size_t i) {
  const bool multi = S.sel.size() > 1 && S.sel.count(i);
  ch::Chord& c = S.prog.chords[i];
  if (multi) ImGui::TextDisabled(i18n::tr("%zu chords selected"), S.sel.size());
  else ImGui::TextDisabled("%s", chordLabel(c).c_str());
  ImGui::Separator();
  if (!multi) {
    if (ImGui::MenuItem(i18n::tr("Preview"))) {
      soundChord(a, i, S.pressHeld);
      S.pvT0 = a.now;
    }
    if (ImGui::MenuItem(i18n::tr("Play from this chord"))) startPreview(a, i);
  }
  if (ImGui::MenuItem(multi ? i18n::tr("Lock") : (c.locked ? i18n::tr("Unlock") : i18n::tr("Lock")))) {
    const bool to = multi ? true : !c.locked;
    editChords(a, i, [to](ch::Chord& x) { x.locked = to; });
  }
  if (multi && ImGui::MenuItem(i18n::tr("Unlock"))) editChords(a, i, [](ch::Chord& x) { x.locked = false; });
  if (ImGui::BeginMenu(i18n::tr("Main notes"))) {
    if (ImGui::MenuItem(i18n::tr("On"), nullptr, c.main)) editChords(a, i, [](ch::Chord& x) { x.main = true; });
    if (ImGui::MenuItem(i18n::tr("Off"), nullptr, !c.main)) editChords(a, i, [](ch::Chord& x) { x.main = false; });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Bass note"))) {
    if (ImGui::MenuItem(i18n::tr("On"), nullptr, c.bass)) editChords(a, i, [](ch::Chord& x) { x.bass = true; });
    if (ImGui::MenuItem(i18n::tr("Off"), nullptr, !c.bass)) editChords(a, i, [](ch::Chord& x) { x.bass = false; });
    ImGui::EndMenu();
  }
  ui::Label("CHANGE CHORD");
  if (ImGui::MenuItem(i18n::tr("Regenerate"))) {
    std::vector<size_t> which = multi ? std::vector<size_t>(S.sel.begin(), S.sel.end()) : std::vector<size_t>{i};
    S.seed = S.seed * 1664525u + 1013904223u;
    edit(a, [&](ch::ChordProgression& p) { ch::regenerate(p, which, S.adventure, S.seed); });
  }
  if (!multi) {
    if (ImGui::MenuItem(i18n::tr("Show other chords that fit..."))) {
      S.altFor = static_cast<int>(i);
      S.alts = ch::alternatives(S.prog, i);
    }
    if (ImGui::MenuItem(i18n::tr("Add a passing chord after it"), nullptr, false, ch::chordLength(S.prog, i) >= 2 * a.doc.song().beatTicks()))
      edit(a, [&](ch::ChordProgression& p) { ch::addPassingChord(p, i, a.doc.song().beatTicks()); });
    if (ImGui::BeginMenu(i18n::tr("Choose the chord"))) {
      ui::Label("ROOT NOTE");
      for (int k = 0; k < 12; ++k) {
        if (k % 6) ImGui::SameLine();
        if (ui::TextButton(ch::noteName(k).c_str(), c.root == k, 30)) editChords(a, i, [k](ch::Chord& x) { ch::setRoot(x, k); });
      }
      ui::Label("CHORD");
      const auto& q = ch::qualities();
      for (size_t k = 0; k < q.size(); ++k) {
        if (std::string(q[k].symbol) == "(8va)") continue;
        const std::string l = ch::noteName(c.root) + q[k].symbol + "  " + q[k].name;
        if (ImGui::MenuItem(l.c_str(), nullptr, c.quality == static_cast<int>(k))) editChords(a, i, [k](ch::Chord& x) { x.quality = static_cast<int>(k); });
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Type the chord name..."))) S.typeFor = static_cast<int>(i);
    if (ImGui::MenuItem(i18n::tr("Learn from MIDI input"), nullptr, S.learn)) {
      S.learn = !S.learn;
      S.sel = {i};
      S.learned.clear();
    }
  }
  ui::Label("CHANGE NOTES");
  if (ImGui::BeginMenu(i18n::tr("Invert"))) {
    if (ImGui::MenuItem(i18n::tr("Up"))) editChords(a, i, [](ch::Chord& x) { ch::invert(x, 1); });
    if (ImGui::MenuItem(i18n::tr("Down"))) editChords(a, i, [](ch::Chord& x) { ch::invert(x, -1); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Transpose"))) {
    if (ImGui::MenuItem(i18n::tr("A semitone up"))) editChords(a, i, [](ch::Chord& x) { ch::transpose(x, 1); });
    if (ImGui::MenuItem(i18n::tr("A semitone down"))) editChords(a, i, [](ch::Chord& x) { ch::transpose(x, -1); });
    if (ImGui::MenuItem(i18n::tr("An octave up"))) editChords(a, i, [](ch::Chord& x) { x.octave += 1; });
    if (ImGui::MenuItem(i18n::tr("An octave down"))) editChords(a, i, [](ch::Chord& x) { x.octave -= 1; });
    if (ImGui::MenuItem(i18n::tr("Bass an octave up"))) editChords(a, i, [](ch::Chord& x) { x.bassOctave += 1; });
    if (ImGui::MenuItem(i18n::tr("Bass an octave down"))) editChords(a, i, [](ch::Chord& x) { x.bassOctave -= 1; });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Fixed bass"))) {
    if (ImGui::MenuItem(i18n::tr("The chord's root"), nullptr, c.fixedBass < 0)) editChords(a, i, [](ch::Chord& x) { x.fixedBass = -1; });
    for (int k = 0; k < 12; ++k)
      if (ImGui::MenuItem(ch::noteName(k).c_str(), nullptr, c.fixedBass == k)) editChords(a, i, [k](ch::Chord& x) { x.fixedBass = k; });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Extra notes"))) {
    if (ImGui::MenuItem(i18n::tr("Leave one note out"), nullptr, c.extra < 0)) editChords(a, i, [](ch::Chord& x) { x.extra = -1; });
    static const char* const n[] = {"None", "One note", "Two notes", "Three notes"};
    for (int k = 0; k < 4; ++k)
      if (ImGui::MenuItem(n[k], nullptr, c.extra == k)) editChords(a, i, [k](ch::Chord& x) { x.extra = k; });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Voicing"))) {
    for (int k = 0; k < static_cast<int>(ch::Voicing::Count); ++k)
      if (ImGui::MenuItem(ch::voicingName(static_cast<ch::Voicing>(k)), nullptr, static_cast<int>(c.voicing) == k))
        editChords(a, i, [k](ch::Chord& x) { x.voicing = static_cast<ch::Voicing>(k); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    for (int k = 0; k < kNoteColors; ++k)
      if (ImGui::MenuItem(std::to_string(k + 1).c_str(), nullptr, c.color == k)) editChords(a, i, [k](ch::Chord& x) { x.color = k; });
    ImGui::EndMenu();
  }
  ui::Label("PIANO ROLL NOTES");
  if (ImGui::MenuItem(i18n::tr("Snap the pattern's notes to the chords"))) {
    std::vector<Note> b = S.base;
    if (ch::snapNotes(b, S.prog, startTick(a.doc.song())) > 0) {
      S.base = b;
      writeNow(a);
    }
  }
  if (ImGui::BeginMenu(i18n::tr("Note linking"))) {
    if (ImGui::MenuItem(i18n::tr("Write the chord again (replace)"))) setLinked(a, i, true);
    if (ImGui::MenuItem(i18n::tr("Link"), nullptr, c.linked)) setLinked(a, i, true);
    if (ImGui::MenuItem(i18n::tr("Unlink (keep its notes as they are)"), nullptr, !c.linked)) setLinked(a, i, false);
    ImGui::EndMenu();
  }
  ui::Label("CHORD ACTIONS");
  if (ImGui::MenuItem(i18n::tr("Copy"))) {
    S.clip.clear();
    if (multi)
      for (size_t k : S.sel) S.clip.push_back(S.prog.chords[k]);
    else
      S.clip.push_back(c);
  }
  if (ImGui::MenuItem(i18n::tr("Paste after it"), nullptr, false, !S.clip.empty()))
    edit(a, [&](ch::ChordProgression& p) {
      int at = static_cast<int>(i) + 1;
      for (const auto& k : S.clip) ch::addChord(p, k, at++);
    });
  if (!multi && ImGui::BeginMenu(i18n::tr("Swap with"))) {
    if (ImGui::MenuItem(i18n::tr("The chord on the left"), nullptr, false, i > 0)) edit(a, [&](ch::ChordProgression& p) { ch::swapChords(p, i, i - 1); });
    if (ImGui::MenuItem(i18n::tr("The chord on the right"), nullptr, false, i + 1 < S.prog.chords.size()))
      edit(a, [&](ch::ChordProgression& p) { ch::swapChords(p, i, i + 1); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Delete"), nullptr, false, S.prog.chords.size() > 1)) {
    std::vector<size_t> which = multi ? std::vector<size_t>(S.sel.begin(), S.sel.end()) : std::vector<size_t>{i};
    std::sort(which.rbegin(), which.rend());
    edit(a, [&](ch::ChordProgression& p) {
      for (size_t k : which)
        if (p.chords.size() > 1) ch::removeChord(p, k);
    });
    S.sel.clear();
  }
}

void genModeMenu() {
  ui::Label("NOTES WRITTEN");
  ImGui::MenuItem(i18n::tr("Chord notes"), nullptr, &S.withMain);
  ImGui::MenuItem(i18n::tr("Bass notes"), nullptr, &S.withBass);
  ui::Label("NOTES ALREADY IN THE PATTERN");
  for (int k = 0; k < static_cast<int>(ch::NonChordMode::Count); ++k)
    if (ImGui::MenuItem(ch::nonChordModeName(static_cast<ch::NonChordMode>(k)), nullptr, static_cast<int>(S.mode) == k))
      S.mode = static_cast<ch::NonChordMode>(k);
}

// ------------------------------------------------------------------ the chord lane

void chordLane(App& a, float height) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float W = std::max(200.0f, ImGui::GetContentRegionAvail().x);
  ImGui::InvisibleButton("##lane", ImVec2(W, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 p1(p0.x + W, p0.y + height);
  dl->AddRectFilled(p0, p1, P.well, theme::kRadius);
  const Tick total = std::max<Tick>(1, ch::totalLength(S.prog));
  const Tick viewLen = std::max<Tick>(total, 4 * s.barTicks());
  const float pxPerTick = (W - 8) / static_cast<float>(viewLen);
  auto xOf = [&](Tick t) { return p0.x + 4 + static_cast<float>(t) * pxPerTick; };
  // Beat / bar lines.
  for (Tick t = 0; t <= viewLen; t += s.beatTicks())
    dl->AddLine(ImVec2(std::round(xOf(t)), p0.y + 2), ImVec2(std::round(xOf(t)), p1.y - 2), t % s.barTicks() == 0 ? P.gridBar : P.gridBeat);
  const ImGuiIO& io = ImGui::GetIO();
  int hoverChord = -1;
  bool hoverEdge = false;
  const int playing = S.pv.on ? S.pvChord : -1;
  for (size_t i = 0; i < S.prog.chords.size(); ++i) {
    const ch::Chord& c = S.prog.chords[i];
    const float x0 = xOf(ch::chordStart(S.prog, i)) + 1, x1 = xOf(ch::chordStart(S.prog, i) + ch::chordLength(S.prog, i)) - 1;
    const ImVec2 a0(x0, p0.y + 6), a1(x1, p1.y - 6);
    const bool sel = S.sel.count(i) > 0;
    const ImU32 tint = theme::lit(theme::rgb(theme::role::kStamp));
    dl->AddRectFilled(a0, a1, sel ? theme::withAlpha(tint, 0.38f) : P.surface700, theme::kRadiusLg);
    if (static_cast<int>(i) == playing) dl->AddRectFilled(a0, a1, theme::withAlpha(P.playhead, 0.25f), theme::kRadiusLg);
    dl->AddRect(a0, a1, sel ? tint : P.lineStrong, theme::kRadiusLg, 0, sel ? 1.5f : 1.0f);
    // Colour group stripe.
    static const uint32_t stripe[16] = {0x6E9F4B, 0x5A8FD8, 0xD8A25A, 0xC0504D, 0x8064A2, 0x4BACC6, 0xF79646, 0x9BBB59,
                                        0x2C4D75, 0x772C2A, 0x5F7530, 0x4D3B62, 0x276A7C, 0xB65708, 0x7F7F7F, 0xD99694};
    dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + 3), theme::rgb(stripe[std::clamp(c.color, 0, 15)]), theme::kRadiusLg);
    // Name, second name, notes.
    const std::string name = chordLabel(c);
    const std::string other = S.roman ? ch::chordName(c) : ch::romanName(c, S.prog.key, S.prog.scale);
    const float bw = x1 - x0;
    ImFont* big = a.fontBold;
    dl->PushClipRect(a0, a1, true);
    dl->AddText(big, theme::kFontTitle, ImVec2(x0 + 6, a0.y + 8), c.linked ? P.text : P.textDim, name.c_str());
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x0 + 6, a0.y + 28), P.textDim, other.c_str());
    // Mini keyboard dots: one row per voiced note (bass in its own colour).
    const auto v = ch::voice(S.prog, i);
    const float ky0 = a0.y + 46, ky1 = a1.y - 6;
    if (ky1 > ky0 + 8) {
      auto yOf = [&](int pitch) { return ky1 - (static_cast<float>(pitch - 36) / 60.0f) * (ky1 - ky0); };
      if (c.main)
        for (int pitch : v.main) dl->AddRectFilled(ImVec2(x0 + 6, yOf(pitch) - 1.5f), ImVec2(x0 + std::min(bw - 6, 40.0f), yOf(pitch) + 1.5f), theme::lit(theme::rgb(theme::role::kDraw)), 1);
      if (c.bass && v.bass >= 0)
        dl->AddRectFilled(ImVec2(x0 + 6, yOf(v.bass) - 1.5f), ImVec2(x0 + std::min(bw - 6, 40.0f), yOf(v.bass) + 1.5f), theme::lit(theme::rgb(theme::role::kGraph)), 1);
    }
    if (c.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(x1 - 10, a0.y + 14), 11, P.textDim);
    if (!c.linked) dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(x0 + 6, a1.y - 16), P.textFaint, i18n::tr("unlinked"));
    dl->PopClipRect();
    if (hovered && io.MousePos.x >= a0.x && io.MousePos.x < a1.x + 3) {
      hoverChord = static_cast<int>(i);
      hoverEdge = io.MousePos.x > a1.x - 6;
    }
  }
  if (hovered) {
    if (hoverEdge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    a.setHint(hoverChord >= 0 ? "Click: select (Ctrl adds) · drag: swap places · drag the right edge: length · double-click: type a name · right-click: chord menu"
                              : "Right-click: add a chord");
  }
  // Mouse.
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hoverChord >= 0) {
    const size_t i = static_cast<size_t>(hoverChord);
    if (io.KeyCtrl || io.KeySuper) {
      if (!S.sel.erase(i)) S.sel.insert(i);
    } else if (io.KeyShift && !S.sel.empty()) {
      const size_t a0 = std::min(i, *S.sel.begin()), a1 = std::max(i, *S.sel.rbegin());
      for (size_t k = a0; k <= a1; ++k) S.sel.insert(k);
    } else if (!S.sel.count(i)) {
      S.sel = {i};
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) S.typeFor = hoverChord;
    S.drag = hoverChord;
    S.dragKind = hoverEdge ? 1 : 0;
    S.dragX = io.MousePos.x;
    S.dragLen = ch::chordLength(S.prog, i);
    S.dragging = false;
    soundChord(a, i, S.pressHeld);
  }
  if (S.drag >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float dx = io.MousePos.x - S.dragX;
    if (std::fabs(dx) > 3 && !S.dragging) {
      S.dragging = true;
      if (S.dragKind == 1) {  // the whole resize drag is one editor step
        S.undo.push_back(S.prog);
        S.redo.clear();
      }
    }
    if (S.dragging && S.dragKind == 1) {
      const Tick g = snapTicks(s);
      Tick len = S.dragLen + static_cast<Tick>(std::llround(dx / pxPerTick / static_cast<float>(g))) * g;
      len = std::max(g, len);
      const size_t i = static_cast<size_t>(S.drag);
      if (i < S.prog.chords.size() && len != ch::chordLength(S.prog, i)) {
        S.prog.chords[i].length = len;
        writeNow(a);
      }
    }
  }
  if (S.drag >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (S.dragging && S.dragKind == 0 && hoverChord >= 0 && hoverChord != S.drag) {
      const size_t from = static_cast<size_t>(S.drag), to = static_cast<size_t>(hoverChord);
      edit(a, [&](ch::ChordProgression& p) { ch::swapChords(p, from, to); });
      S.sel = {to};
    }
    S.drag = -1;
    releaseHeld(a, S.pressHeld);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    if (hoverChord >= 0 && !S.sel.count(static_cast<size_t>(hoverChord))) S.sel = {static_cast<size_t>(hoverChord)};
    ImGui::OpenPopup(hoverChord >= 0 ? "##chordmenu" : "##newchord");
    S.typeFor = -1;
    S.drag = -1;
    if (hoverChord >= 0) ImGui::GetStateStorage()->SetInt(ImGui::GetID("##menuchord"), hoverChord);
  }
  if (ImGui::BeginPopup("##chordmenu")) {
    const int i = ImGui::GetStateStorage()->GetInt(ImGui::GetID("##menuchord"), -1);
    if (i >= 0 && static_cast<size_t>(i) < S.prog.chords.size()) chordMenu(a, static_cast<size_t>(i));
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##newchord")) {
    if (ImGui::MenuItem(i18n::tr("Add a chord at the end")))
      edit(a, [&](ch::ChordProgression& p) {
        ch::Chord c = p.chords.empty() ? ch::diatonicChord(p.key, p.scale, 1) : p.chords.back();
        c.length = lenTicks(s, S.newLen);
        c.locked = false;
        ch::addChord(p, c, -1);
      });
    if (ImGui::MenuItem(i18n::tr("Paste at the end"), nullptr, false, !S.clip.empty()))
      edit(a, [&](ch::ChordProgression& p) {
        for (const auto& k : S.clip) ch::addChord(p, k, -1);
      });
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ panels

// "- value +" stepper; returns true when the value changed.
bool stepper(const char* id, int* v, int lo, int hi, const char* fmt) {
  ImGui::PushID(id);
  bool ch = false;
  if (ui::TextButton(GL_ICON_LABEL("minus", "-"), false, 22) && *v > lo) --*v, ch = true;
  ImGui::SameLine(0, 2);
  char b[32];
  std::snprintf(b, sizeof b, fmt, *v);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(b);
  ImGui::SameLine(0, 2);
  if (ui::TextButton(GL_ICON_LABEL("plus", "+"), false, 22) && *v < hi) ++*v, ch = true;
  ImGui::PopID();
  return ch;
}

void helperCombo(const char* id, const char* preview, float w, const std::function<void()>& items) {
  if (!ui::BeginCombo(id, preview, w)) return;
  items();
  ImGui::EndCombo();
}

void topStrip(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Channel* c = s.channel(S.chan);
  const Pattern* pt = s.pattern(S.pat);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s  ·  %s", c ? c->name.c_str() : "?", pt ? pt->name.c_str() : "?");
  ui::Hint("The channel and pattern the progression is written into (where the editor was opened)");
  ImGui::SameLine(0, theme::kS3);
  int key = S.prog.key, scale = S.prog.scale;
  helperCombo("##key", ch::noteName(key).c_str(), 52, [&] {
    for (int k = 0; k < 12; ++k)
      if (ImGui::Selectable(ch::noteName(k).c_str(), key == k)) key = k;
  });
  ui::Hint("Key of the progression (roman numerals and new chords follow it)");
  ImGui::SameLine(0, 2);
  const auto& sc = tools::scales();
  helperCombo("##scale", sc[static_cast<size_t>(std::clamp(scale, 1, static_cast<int>(sc.size()) - 1))].name, 130, [&] {
    for (size_t k = 1; k < sc.size(); ++k)
      if (ImGui::Selectable(sc[k].name, scale == static_cast<int>(k))) scale = static_cast<int>(k);
  });
  if (key != S.prog.key || scale != S.prog.scale) edit(a, [&](ch::ChordProgression& p) { p.key = key, p.scale = scale; });
  ImGui::SameLine(0, theme::kS2);
  int oct = S.prog.octave;
  if (stepper("##oct", &oct, 1, 8, "Oct %d")) edit(a, [oct](ch::ChordProgression& p) { p.octave = oct; });
  ui::Hint("Base octave of the progression (every chord moves with it)");
  ImGui::SameLine(0, theme::kS2);
  static const char* const names[] = {"Names", "Numerals"};
  int r = S.roman ? 1 : 0;
  if (ui::Segmented("##names", names, 2, &r, 66)) S.roman = r == 1;
  ui::Hint("Chord names: letter names, or numerals relative to the key");
  ImGui::SameLine(0, theme::kS2);
  // Chord count.
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Chords"));
  ImGui::SameLine(0, 4);
  int count = static_cast<int>(S.prog.chords.size());
  if (stepper("##count", &count, 1, 32, "%d")) {
    edit(a, [&](ch::ChordProgression& p) {
      while (static_cast<int>(p.chords.size()) < count) {
        ch::Chord c = p.chords.empty() ? ch::diatonicChord(p.key, p.scale, 1) : p.chords.back();
        c.length = lenTicks(s, S.newLen);
        c.locked = false;
        ch::addChord(p, c, -1);
      }
      while (static_cast<int>(p.chords.size()) > count) p.chords.pop_back();
    });
  }
  ImGui::SameLine(0, 4);
  static const char* const lens[] = {"Beat", "2 beats", "Bar", "2 bars"};
  helperCombo("##newlen", lens[S.newLen], 76, [&] {
    for (int k = 0; k < 4; ++k)
      if (ImGui::Selectable(lens[k], S.newLen == k)) S.newLen = k;
  });
  ui::Hint("Length of chords added to the progression");
  ImGui::SameLine(0, 2);
  if (ui::TextButton("Fit", false, 34)) edit(a, [&](ch::ChordProgression& p) {
    ch::rescale(p, lenTicks(s, S.newLen) * static_cast<Tick>(p.chords.size()), snapTicks(s));
  });
  ui::Hint("Rescale: every chord gets the chosen length (proportions kept)");
  ImGui::SameLine(0, theme::kS2);
  static const char* const snaps[] = {"Snap step", "Snap beat", "Snap bar"};
  helperCombo("##snap", snaps[S.snap], 86, [&] {
    for (int k = 0; k < 3; ++k)
      if (ImGui::Selectable(snaps[k], S.snap == k)) S.snap = k;
  });
  ui::Hint("Grid for resizing chords");
  ImGui::SameLine(0, theme::kS2);
  if (ui::IconButton("##undo", ui::Icon::Undo, false, theme::kControlH, "Undo the last chord edit") && !S.undo.empty()) {
    S.redo.push_back(S.prog);
    S.prog = S.undo.back();
    S.undo.pop_back();
    writeNow(a);
  }
  ImGui::SameLine(0, 2);
  if (ui::IconButton("##redo", ui::Icon::Redo, false, theme::kControlH, "Redo") && !S.redo.empty()) {
    S.undo.push_back(S.prog);
    S.prog = S.redo.back();
    S.redo.pop_back();
    writeNow(a);
  }
}

void generatorRow(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Adventure"));
  ImGui::SameLine(0, 4);
  float adv = static_cast<float>(S.adventure);
  ImGui::SetNextItemWidth(120);
  if (ImGui::SliderFloat("##adv", &adv, 0.0f, 1.0f, "%.2f")) S.adventure = adv;
  ui::Hint("How far generated chords stray from the usual moves of the key");
  ImGui::SameLine(0, 4);
  if (ui::TextButton("Generate", true, 80, "New chords for every unlocked chord")) {
    S.seed = S.seed * 1664525u + 1013904223u;
    edit(a, [&](ch::ChordProgression& p) { ch::regenerate(p, {}, S.adventure, S.seed); });
  }
  ImGui::SameLine(0, 4);
  if (ui::TextButton("From the notes", false, 104, "Read the chords the pattern's notes already form (one per chord slot)")) {
    const auto an = ch::analyze(S.base, S.prog.key, S.prog.scale, startTick(s), S.prog.chordLength, static_cast<int>(S.prog.chords.size()));
    edit(a, [&](ch::ChordProgression& p) {
      for (size_t k = 0; k < p.chords.size() && k < an.chords.size(); ++k) {
        if (p.chords[k].locked) continue;
        p.chords[k].root = an.chords[k].root;
        p.chords[k].quality = an.chords[k].quality;
        p.chords[k].fixedBass = an.chords[k].fixedBass;
      }
    });
  }
  ImGui::SameLine(0, theme::kS3);
  if (ui::IconButton("##play", S.pv.on ? ui::Icon::Stop : ui::Icon::Play, S.pv.on, theme::kControlH,
                     S.pv.on ? "Stop the preview" : "Play the progression on the target channel")) {
    if (S.pv.on)
      stopPreview(a);
    else
      startPreview(a, S.sel.empty() ? 0 : *S.sel.begin());
  }
  ImGui::SameLine(0, theme::kS3);
  if (ui::TextButton("Notes...", false, 70, "Which notes are written and what happens to notes already in the pattern")) ImGui::OpenPopup("##genmode");
  if (ImGui::BeginPopup("##genmode")) {
    const auto before = std::make_tuple(S.withMain, S.withBass, S.mode);
    genModeMenu();
    if (before != std::make_tuple(S.withMain, S.withBass, S.mode)) writeNow(a);
    ImGui::EndPopup();
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("From bar"));
  ImGui::SameLine(0, 4);
  int sb = S.startBar + 1;
  if (stepper("##start", &sb, 1, 999, "%d")) {
    S.startBar = sb - 1;
    writeNow(a);
  }
  ImGui::SameLine(0, theme::kS2);
  if (ImGui::Checkbox(i18n::tr("Write while editing"), &S.live) && S.live) writeNow(a);
  ui::Hint("Off: the pattern changes only when you press Accept");
}

void alternativesRow(App& a) {
  if (S.altFor < 0 || static_cast<size_t>(S.altFor) >= S.prog.chords.size()) return;
  const auto& P = pal();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), i18n::tr("Instead of %s:"), chordLabel(S.prog.chords[static_cast<size_t>(S.altFor)]).c_str());
  for (size_t k = 0; k < S.alts.size(); ++k) {
    ImGui::SameLine(0, 4);
    ImGui::PushID(static_cast<int>(k));
    if (ui::TextButton(chordLabel(S.alts[k]).c_str(), false, 0, "Use this chord")) {
      const size_t i = static_cast<size_t>(S.altFor);
      const ch::Chord pick = S.alts[k];
      edit(a, [&](ch::ChordProgression& p) {
        p.chords[i].root = pick.root;
        p.chords[i].quality = pick.quality;
        p.chords[i].fixedBass = -1;
      });
      S.altFor = -1;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      // Right-click: hear the alternative.
      ch::ChordProgression tmp = S.prog;
      tmp.chords[static_cast<size_t>(S.altFor)].root = S.alts[k].root;
      tmp.chords[static_cast<size_t>(S.altFor)].quality = S.alts[k].quality;
      std::swap(tmp, S.prog);
      soundChord(a, static_cast<size_t>(S.altFor), S.pressHeld);
      std::swap(tmp, S.prog);
    }
    ImGui::PopID();
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) releaseHeld(a, S.pressHeld);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Close##alts", false, 50)) S.altFor = -1;
}

void performancePanel(App& a) {
  const Song& s = a.doc.song();
  ui::BeginSection("PERFORMANCE", 0);
  ch::Performance pf = S.prog.perf;
  const auto& rs = ch::rhythms();
  helperCombo("##rhythm", rs[static_cast<size_t>(std::clamp(pf.rhythm, 0, static_cast<int>(rs.size()) - 1))].name, 160, [&] {
    for (size_t k = 0; k < rs.size(); ++k)
      if (ImGui::Selectable(rs[k].name, pf.rhythm == static_cast<int>(k))) pf.rhythm = static_cast<int>(k);
  });
  ui::Hint("Rhythm the chords are played with");
  ImGui::SameLine(0, 4);
  static const char* const cyc[] = {"Per chord", "Beat", "2 beats", "Bar", "2 bars"};
  const Tick cycTicks[] = {0, s.beatTicks(), 2 * s.beatTicks(), s.barTicks(), 2 * s.barTicks()};
  int ci = 0;
  for (int k = 0; k < 5; ++k)
    if (pf.cycle == cycTicks[k]) ci = k;
  helperCombo("##cycle", cyc[ci], 90, [&] {
    for (int k = 0; k < 5; ++k)
      if (ImGui::Selectable(cyc[k], ci == k)) pf.cycle = cycTicks[k];
  });
  ui::Hint("Length of one rhythm cycle (it restarts on every chord)");
  float strum = static_cast<float>(pf.strum) / static_cast<float>(std::max(1, s.ppq));
  ImGui::SetNextItemWidth(110);
  if (ImGui::SliderFloat(i18n::tr("Strum##st"), &strum, 0.0f, 1.0f, "%.2f beat")) pf.strum = static_cast<Tick>(std::lround(strum * s.ppq));
  ImGui::SameLine(0, 4);
  ImGui::Checkbox(i18n::tr("Top first"), &pf.strumDown);
  float gate = static_cast<float>(pf.gate);
  ImGui::SetNextItemWidth(110);
  if (ImGui::SliderFloat(i18n::tr("Gate##gt"), &gate, 0.05f, 1.0f, "%.2f")) pf.gate = gate;
  ImGui::SameLine(0, 4);
  ImGui::SetNextItemWidth(90);
  ImGui::SliderInt(i18n::tr("Velocity##vl"), &pf.velocity, 1, 127);
  ui::EndSection();
  static bool perfEditing = false;  // a slider drag is one editor step
  if (!(pf == S.prog.perf)) {
    if (!perfEditing) {
      S.undo.push_back(S.prog);
      S.redo.clear();
    }
    perfEditing = ImGui::IsAnyItemActive();
    S.prog.perf = pf;
    writeNow(a);
  } else if (!ImGui::IsAnyItemActive()) {
    perfEditing = false;
  }
}

void presetPanel(App& a) {
  const Song& s = a.doc.song();
  ui::BeginSection("PRESETS AND TEXT", 0);
  const auto& ps = ch::presets();
  helperCombo("##preset", ps[static_cast<size_t>(S.preset)].name, 150, [&] {
    for (size_t k = 0; k < ps.size(); ++k) {
      const std::string l = std::string(ps[k].name) + "   " + ps[k].text;
      if (ImGui::Selectable(l.c_str(), S.preset == static_cast<int>(k))) S.preset = static_cast<int>(k);
    }
  });
  ImGui::SameLine(0, 4);
  if (ui::TextButton("Use", false, 40, "Replace the chords with the preset (in the editor's key)")) {
    const auto& pr = ps[static_cast<size_t>(S.preset)];
    edit(a, [&](ch::ChordProgression& p) {
      ch::ChordProgression n = p;
      if (pr.minor && ch::degreeOffset(p.scale, 2) != 3) n.scale = 2;
      if (!pr.minor && ch::degreeOffset(p.scale, 2) == 3) n.scale = 1;
      if (ch::parseProgression(pr.text, n.key, n.scale, lenTicks(s, S.newLen), n)) p = n;
    });
  }
  ImGui::SameLine(0, 4);
  if (ui::TextButton("Random", false, 60, "A random preset")) {
    S.seed = S.seed * 1664525u + 1013904223u;
    S.preset = static_cast<int>((S.seed >> 8) % ps.size());
  }
  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##text", i18n::tr("C - Am - F - G   or   I V vi IV"), S.text, sizeof S.text);
  ImGui::SameLine(0, 4);
  if (ui::TextButton("Read", false, 46, "Make the progression from the text (names or numerals, separated by spaces, commas or dashes)")) {
    int bad = 0;
    ch::ChordProgression n = S.prog;
    if (ch::parseProgression(S.text, S.prog.key, S.prog.scale, lenTicks(s, S.newLen), n, &bad)) {
      edit(a, [&](ch::ChordProgression& p) { p = n; });
      if (bad) a.notify(std::to_string(bad) + " word" + (bad == 1 ? "" : "s") + " not read as chords", true);
    } else {
      a.notify("No chords in the text", true);
    }
  }
  ImGui::SameLine(0, 4);
  if (ui::TextButton("Write out", false, 64, "Put the progression into the text field")) {
    std::snprintf(S.text, sizeof S.text, "%s", ch::progressionText(S.prog, S.roman).c_str());
  }
  if (ImGui::Checkbox(i18n::tr("Learn chords from MIDI input"), &S.learn)) S.learned.clear();
  ui::Hint("Play a chord on a MIDI keyboard: it replaces the selected chord");
  ImGui::SameLine(0, 4);
  ImGui::Checkbox(i18n::tr("Then the next chord"), &S.autoLearn);
  ui::Hint("After each learned chord the selection moves on to the next one");
  ui::EndSection();
}

}  // namespace

// ================================================================ entry points

void openChordEditor(App& a) {
  if (!a.doc.song().pattern(a.curPattern) || !a.doc.song().channel(a.selChannel)) {
    a.notify("Select a channel and a pattern first", true);
    return;
  }
  if (S.open) closeEditor(a, true);
  beginSession(a);
  a.focusWindow = "Chord progression";
}

void chordEditorNote(App& a, int pitch, bool on) {
  if (!S.open || !S.learn) return;
  if (on) {
    S.held.insert(pitch);
    if (S.held.size() >= S.learned.size()) S.learned.assign(S.held.begin(), S.held.end());
    return;
  }
  S.held.erase(pitch);
  if (!S.held.empty() || S.learned.size() < 2) return;
  // All keys up: the chord played becomes the selected chord.
  ch::Chord c;
  const std::vector<int> notes = S.learned;
  S.learned.clear();
  if (!ch::detect(notes, c)) {
    a.notify("Not a chord the editor knows", true);
    return;
  }
  const size_t i = S.sel.empty() ? 0 : *S.sel.begin();
  if (i >= S.prog.chords.size()) return;
  edit(a, [&](ch::ChordProgression& p) {
    ch::Chord& k = p.chords[i];
    k.root = c.root;
    k.quality = c.quality;
    k.inversion = c.inversion;
    k.fixedBass = -1;
  });
  if (S.autoLearn && i + 1 < S.prog.chords.size()) S.sel = {i + 1};
}

void drawChordEditor(App& a) {
  tickPreview(a);
  if (!S.open) return;
  // The target went away (pattern or channel deleted): end the session.
  if (!a.doc.song().pattern(S.pat) || !a.doc.song().channel(S.chan)) {
    S.open = false;
    stopPreview(a);
    return;
  }
  ui::PlaceNextDialog(ImVec2(1010, 500), ImGuiCond_FirstUseEver, ImVec2(820, 400));  // J-54: never wider / taller than the screen
  bool open = true;
  if (!ImGui::Begin(ui::windowTitle("Chord progression").c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) closeEditor(a, true);
    return;
  }
  ui::KeepWindowInViewport();
  topStrip(a);
  generatorRow(a);
  ImGui::Dummy(ImVec2(0, 2));
  // J-54: the lane takes what the rows below it leave (measured last frame), so
  // the window never needs a scrollbar of its own.
  static float belowLane = 200.0f;
  const float laneH = std::clamp(ImGui::GetContentRegionAvail().y - belowLane, 96.0f, 280.0f);
  chordLane(a, laneH);
  const float laneBottom = ImGui::GetCursorScreenPos().y;
  alternativesRow(a);
  if (ImGui::BeginTable("##panels", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(i18n::tr("perf"), ImGuiTableColumnFlags_WidthFixed, 340);
    // J-36: an explicit weight. With SizingStretchProp the stretch column takes its
    // auto-fit width as weight, 0 on the first frame: 0 / 0 gave NaN column widths.
    ImGui::TableSetupColumn(i18n::tr("presets"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextColumn();
    performancePanel(a);
    ImGui::TableNextColumn();
    presetPanel(a);
    ImGui::EndTable();
  }
  // Type in a chord name.
  if (S.typeFor >= 0) {
    const int i = S.typeFor;
    S.typeFor = -1;
    if (static_cast<size_t>(i) < S.prog.chords.size())
      menus::openPrompt("Chord name (e.g. Am7, F/C, IV, bVII)", ch::chordName(S.prog.chords[static_cast<size_t>(i)]), [&a, i](const std::string& t) {
        ch::Chord c = S.prog.chords[static_cast<size_t>(i)];
        if (!ch::parseChord(t, S.prog.key, S.prog.scale, c)) {
          a.notify("Not a chord name: " + t, true);
          return;
        }
        edit(a, [&](ch::ChordProgression& p) {
          if (static_cast<size_t>(i) < p.chords.size()) p.chords[static_cast<size_t>(i)] = c;
        });
      });
  }
  // Keys: Delete removes the selected chords, Ctrl+Z / Ctrl+Y the editor's own history.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !S.sel.empty() && S.prog.chords.size() > S.sel.size()) {
      std::vector<size_t> which(S.sel.rbegin(), S.sel.rend());
      edit(a, [&](ch::ChordProgression& p) {
        for (size_t k : which) ch::removeChord(p, k);
      });
      S.sel.clear();
    }
  }
  ui::DialogFooter(ui::ButtonRowWidth(3, 88));  // J-54: one button width, kS2 apart, like the other dialogs
  if (ui::TextButton("Accept", true, 88, "Keep the written chords (one undo step) and close")) {
    if (!S.live) {
      S.live = true;
      writeNow(a);
      S.live = false;
    }
    closeEditor(a, true);
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Reset", false, 88, "Put the pattern back as it was and start the progression again")) {
    if (S.changed) a.doc.setNotes(S.pat, S.chan, S.base, "chord progression", S.key);
    S.prog.chords.clear();
    S.undo.clear();
    S.redo.clear();
    beginSession(a);
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Cancel", false, 88, "Put the pattern back as it was and close")) closeEditor(a, false);
  belowLane = ImGui::GetItemRectMax().y - laneBottom;
  ImGui::End();
  if (!open) closeEditor(a, true);
}

}  // namespace gl::j31
