// Gridline — piano roll / channel rack tool windows (see EditTools.h).
#include "EditTools.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include "../../third_party/imgui/imgui.h"
#include "../core/NoteTools.h"
#include "App.h"
#include "J31Ui.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;

namespace {

// ------------------------------------------------------------------ settings ----
// Kept between openings (like the reference tools remember their last values).
struct ToolSettings {
  // Quantize.
  tools::QuantizeOpts q;
  double qGrid = 4;        // index into kGrids
  int qDuration = 0;
  int qGrooveChannel = -1; // >= 0: index into the song's channels used as groove
  // Chop.
  int chopPattern = 0;
  double chopTime = 1.0, chopGate = 1.0;
  // Strum.
  tools::StrumOpts strum;
  double strumBeats = 0.125;
  // Arpeggiate.
  tools::ArpOpts arp;
  double arpGrid = 4;
  // Flam.
  int flamPreset = 0;
  tools::FlamOpts flam;
  double flamTime48 = 4;
  // Articulate.
  int artPreset = 0;
  tools::ArticulateOpts art;
  double artGap48 = 0;
  // Randomize.
  tools::RandomOpts rnd;
  double rndGrid = 4;
  int rndBars = 1;
  bool rndReplace = true;
  tools::LevelRandom levels;
  // LFO.
  tools::NoteLfoOpts lfo;
  double lfoStartDiv = 5, lfoEndDiv = 5;
  // Scale levels.
  tools::ScaleLevelOpts sl;
  // Limit.
  int limLo = 48, limHi = 72, limMode = 0;
  // Riff.
  tools::RiffOpts riff;
  double riffGrid = 4;
  bool riffReplace = true;
  // Note properties.
  double props[static_cast<int>(tools::NoteProp::Count)] = {0.79, 0.5, 0.5, 0.5, 0.5, 0.5};
  bool propsTouched[static_cast<int>(tools::NoteProp::Count)] = {false, false, false, false, false, false};
  int propsColor = -1;
  // Steps.
  int fillType = 0;  // 0 every N, 1 euclidean
  double fillEvery = 4, fillOffset = 0, fillHits = 5, fillLength = 16, fillRotation = 0, fillVel = 100;
  double rndProb = 0.4, rndVelLo = 70, rndVelHi = 115;
  uint32_t stepSeed = 1;
};
ToolSettings S;

// Grid sizes offered by the tools (fractions of a beat).
struct GridDef { const char* name; double beats; };
const GridDef kGrids[] = {{"1/16 beat", 1.0 / 16}, {"1/8 beat", 1.0 / 8}, {"1/6 beat", 1.0 / 6}, {"1/3 beat", 1.0 / 3},
                          {"1/4 beat (step)", 0.25}, {"1/2 beat", 0.5}, {"Beat", 1.0}, {"2 beats", 2.0}, {"Bar", 4.0}};
constexpr int kGridCount = static_cast<int>(sizeof kGrids / sizeof kGrids[0]);
Tick gridTicks(const Song& s, double idx) {
  const GridDef& g = kGrids[std::clamp(static_cast<int>(std::lround(idx)), 0, kGridCount - 1)];
  double beats = g.beats == 4.0 ? static_cast<double>(s.barTicks()) / s.ppq : g.beats;
  return std::max<Tick>(1, static_cast<Tick>(std::llround(beats * s.ppq)));
}
const char* gridName(double idx) { return kGrids[std::clamp(static_cast<int>(std::lround(idx)), 0, kGridCount - 1)].name; }

// LFO speeds (cycle lengths in beats).
struct DivDef { const char* name; double beats; };
const DivDef kDivs[] = {{"1/4 beat", 0.25}, {"1/2 beat", 0.5}, {"1 beat", 1}, {"2 beats", 2}, {"1 bar", 4}, {"2 bars", 8},
                        {"4 bars", 16}, {"8 bars", 32}, {"16 bars", 64}};
constexpr int kDivCount = static_cast<int>(sizeof kDivs / sizeof kDivs[0]);
Tick divTicks(const Song& s, double idx) {
  return std::max<Tick>(1, static_cast<Tick>(std::llround(kDivs[std::clamp(static_cast<int>(std::lround(idx)), 0, kDivCount - 1)].beats * s.ppq)));
}

const char* const kKeys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// --------------------------------------------------------------- session ----
struct Session {
  NoteTool tool = NoteTool::None;
  StepTool stepTool = StepTool::None;
  ObjectId pat = kNoId, ch = kNoId;
  std::vector<Note> base;  // notes when the window opened
  tools::NoteSel baseSel;
  std::vector<uint8_t> baseRow;
  std::vector<StepProps> baseProps;
  uint64_t key = 0;
  bool changed = false;
  bool focus = false;
};
Session T;
uint64_t sessionCounter = 1;

ParamDesc desc(double lo, double hi, double def, ParamScale sc = ParamScale::Linear) {
  ParamDesc d;
  d.min = lo;
  d.max = hi;
  d.def = def;
  d.scale = sc;
  return d;
}
ParamDesc choiceDesc(int count, int def) {
  ParamDesc d = desc(0, count - 1, def);
  d.kind = ParamKind::Choice;
  for (int i = 0; i < count; ++i) d.choices.push_back(std::to_string(i));
  return d;
}

std::string fmt(const char* f, double v) {
  char b[48];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string pct(double v) { return fmt("%.0f%%", v * 100); }
std::string spct(double v) { return fmt("%+.0f%%", v * 100); }

// Knob with a label and a value line under it, in a fixed-width cell.
bool knob(const char* id, double* v, const ParamDesc& d, const char* label, const std::string& text, bool bipolar = false) {
  const auto& P = pal();
  const float cell = 70;
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float r = 16;
  ImGui::SetCursorScreenPos(ImVec2(p.x + (cell - 2 * r) * 0.5f, p.y));
  bool ch = ui::Knob(id, v, d, r, nullptr, bipolar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y = p.y + 2 * r + 4;
  auto centred = [&](ImFont* f, float size, ImU32 col, const std::string& t) {
    float tw = f->CalcTextSizeA(size, 1e9f, 0, t.c_str()).x;
    if (tw > cell - 2)
      theme::textEllipsis(dl, f, size, ImVec2(p.x + 1, y), cell - 2, col, t.c_str());
    else
      dl->AddText(f, size, ImVec2(std::round(p.x + (cell - tw) * 0.5f), y), col, t.c_str());
    y += size + 3;
  };
  centred(app().fontRegular, theme::kFontLabel, P.textDim, label);
  centred(app().fontMono, theme::kFontLabel, P.text, text);
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(cell, y - p.y));
  ImGui::EndGroup();
  return ch;
}
bool knobInt(const char* id, int* v, int lo, int hi, const char* label, const std::string& text) {
  double d = *v;
  bool ch = knob(id, &d, desc(lo, hi, *v), label, text);
  *v = std::clamp(static_cast<int>(std::lround(d)), lo, hi);
  return ch;
}
void same() { ImGui::SameLine(0, 2); }
void section(const char* t) {
  ImGui::Dummy(ImVec2(0, 2));
  ui::Label(t);
}
bool combo(const char* id, const char* preview, float w, const std::function<void()>& items) {
  if (!ui::BeginCombo(id, preview, w)) return false;
  items();
  ImGui::EndCombo();
  return true;
}
void keyCombo(const char* id, int* root) {
  combo(id, kKeys[((*root % 12) + 12) % 12], 70, [&] {
    for (int k = 0; k < 12; ++k)
      if (ImGui::Selectable(kKeys[k], *root == k)) *root = k;
  });
}
void scaleCombo(const char* id, int* scale, bool allowNone) {
  const auto& sc = tools::scales();
  combo(id, *scale == 0 && allowNone ? "Off" : sc[static_cast<size_t>(*scale)].name, 150, [&] {
    for (size_t k = allowNone ? 0 : 1; k < sc.size(); ++k)
      if (ImGui::Selectable(k == 0 ? "Off" : sc[k].name, *scale == static_cast<int>(k))) *scale = static_cast<int>(k);
  });
}
void seedButton(const char* id, uint32_t* seed) {
  ImGui::PushID(id);
  if (ui::TextButton("New variation", false, 0, "Roll new random values")) *seed = *seed * 1664525u + 1013904223u + 7u;
  ImGui::PopID();
}

// Current notes of the session target.
const std::vector<Note>* targetNotes(const Song& s) {
  const Pattern* p = s.pattern(T.pat);
  if (!p) return nullptr;
  static const std::vector<Note> empty;
  auto it = p->notes.find(T.ch);
  return it == p->notes.end() ? &empty : &it->second;
}

void close(App& a, bool keep) {
  if (!keep && T.changed) {
    if (T.stepTool != StepTool::None)
      ops::editStepRow(a.doc, T.pat, T.ch, "cancel", [&](tools::StepRow& r, tools::StepPropRow& p) { r = T.baseRow, p = T.baseProps; }, T.key);
    else
      a.doc.setNotes(T.pat, T.ch, T.base, "cancel", T.key);
    a.piano.selected.clear();
    for (size_t i = 0; i < T.baseSel.size(); ++i)
      if (T.baseSel[i]) a.piano.selected.insert(i);
  }
  a.doc.endGesture();
  T = Session{};
}

// Applies a preview result (one merged undo step per session).
void preview(App& a, const char* label, std::vector<Note> notes, tools::NoteSel sel) {
  const std::vector<Note>* cur = targetNotes(a.doc.song());
  if (!cur) return;
  ops::sortNotes(notes, sel);
  if (notes != *cur) {
    a.doc.setNotes(T.pat, T.ch, notes, label, T.key);
    T.changed = true;
  }
  a.piano.selPattern = T.pat;
  a.piano.selChannel = T.ch;
  a.piano.selected.clear();
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) a.piano.selected.insert(i);
}

std::string targetLine(const App& a) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(T.ch);
  size_t n = T.base.size(), k = 0;
  for (bool b : T.baseSel) k += b;
  std::string who = c ? c->name : std::string("?");
  if (k) return who + "  \xC2\xB7  " + std::to_string(k) + " of " + std::to_string(n) + " notes";
  return who + "  \xC2\xB7  all " + std::to_string(n) + " notes";
}

// ------------------------------------------------------------ the tools ----

void quantizeBody(App& a) {
  const Song& s = a.doc.song();
  section("GRID AND GROOVE");
  combo("##qgrid", gridName(S.qGrid), 130, [&] {
    for (int i = 0; i < kGridCount; ++i)
      if (ImGui::Selectable(kGrids[i].name, std::lround(S.qGrid) == i)) S.qGrid = i;
  });
  ui::Hint("Grid the notes move to");
  ImGui::SameLine();
  std::string gname = S.qGrooveChannel >= 0 && S.qGrooveChannel < static_cast<int>(s.channels.size())
                          ? "Notes of " + s.channels[static_cast<size_t>(S.qGrooveChannel)].name
                          : tools::grooves()[static_cast<size_t>(S.q.groove)].name;
  combo("##qgroove", gname.c_str(), 170, [&] {
    for (size_t i = 0; i < tools::grooves().size(); ++i)
      if (ImGui::Selectable(tools::grooves()[i].name, S.qGrooveChannel < 0 && S.q.groove == static_cast<int>(i)))
        S.q.groove = static_cast<int>(i), S.qGrooveChannel = -1;
    ImGui::Separator();
    ImGui::TextDisabled("Timing of another channel");
    const Pattern* p = s.pattern(T.pat);
    for (size_t i = 0; i < s.channels.size(); ++i) {
      const Channel& c = s.channels[i];
      if (c.id == T.ch || !p || !p->notes.count(c.id) || p->notes.at(c.id).empty()) continue;
      std::string l = c.name + "##gc" + std::to_string(i);
      if (ImGui::Selectable(l.c_str(), S.qGrooveChannel == static_cast<int>(i))) S.qGrooveChannel = static_cast<int>(i);
    }
  });
  ui::Hint("Groove template: shifts (and accents) per grid line; or the timing of another channel's notes");
  section("STRENGTH");
  knob("##qs", &S.q.startStrength, desc(0, 1, 1), "Start", pct(S.q.startStrength));
  same();
  knob("##qsw", &S.q.swing, desc(0, 1, 0), "Swing", pct(S.q.swing));
  same();
  knob("##qw", &S.q.window, desc(0.05, 1, 1), "Window", S.q.window >= 1 ? std::string("All") : pct(S.q.window));
  same();
  knob("##qgv", &S.q.grooveVelocity, desc(0, 1, 0), "Accents", pct(S.q.grooveVelocity));
  section("LENGTHS");
  static const char* const durs[] = {"Keep", "Keep ends", "Lengths", "Ends"};
  ui::Segmented("##qdur", durs, 4, &S.qDuration, 70);
  ui::Hint("Keep lengths · keep end times · quantize lengths · quantize end times");
  ImGui::SameLine();
  knob("##qds", &S.q.durationStrength, desc(0, 1, 1), "Strength", pct(S.q.durationStrength));

  tools::QuantizeOpts o = S.q;
  o.grid = gridTicks(s, S.qGrid);
  o.duration = static_cast<tools::DurationMode>(S.qDuration);
  auto n = T.base;
  auto sel = T.baseSel;
  const Pattern* p = s.pattern(T.pat);
  if (S.qGrooveChannel >= 0 && S.qGrooveChannel < static_cast<int>(s.channels.size()) && p &&
      p->notes.count(s.channels[static_cast<size_t>(S.qGrooveChannel)].id)) {
    auto shifts = tools::extractGroove(p->notes.at(s.channels[static_cast<size_t>(S.qGrooveChannel)].id), o.grid, 4);
    tools::quantizeWithShifts(n, sel, o, shifts);
  } else {
    tools::quantizeEx(n, sel, o);
  }
  preview(a, "quantize notes", n, sel);
}

void chopBody(App& a) {
  const Song& s = a.doc.song();
  section("PATTERN");
  const auto& cp = tools::chopPatterns();
  combo("##cpat", cp[static_cast<size_t>(S.chopPattern)].name, 180, [&] {
    for (size_t i = 0; i < cp.size(); ++i)
      if (ImGui::Selectable(cp[i].name, S.chopPattern == static_cast<int>(i))) S.chopPattern = static_cast<int>(i);
  });
  ui::Hint("Piece lengths (and pitch steps) the notes are cut into, repeated over each note");
  section("OPTIONS");
  ParamDesc dt = desc(0.125, 8, 1, ParamScale::Log);
  knob("##ct", &S.chopTime, dt, "Time", fmt("x%.2f", S.chopTime));
  same();
  knob("##cg", &S.chopGate, desc(0.05, 1, 1), "Gate", pct(S.chopGate));
  auto n = T.base;
  auto sel = T.baseSel;
  tools::chopByPattern(n, sel, cp[static_cast<size_t>(S.chopPattern)], std::max<Tick>(1, s.ppq / 4), S.chopTime, S.chopGate);
  preview(a, "chop notes", n, sel);
}

void strumBody(App& a) {
  const Song& s = a.doc.song();
  section("START");
  knob("##st", &S.strumBeats, desc(0, 1, 0.125), "Time", fmt("%.3f beat", S.strumBeats));
  same();
  knob("##stt", &S.strum.timeTension, desc(-1, 1, 0), "Tension", spct(S.strum.timeTension), true);
  same();
  knob("##sv", &S.strum.velocity, desc(0, 1, 0), "Velocity", pct(S.strum.velocity));
  same();
  knob("##svt", &S.strum.velocityTension, desc(-1, 1, 0), "Vel. tension", spct(S.strum.velocityTension), true);
  section("END");
  knob("##se", &S.strum.end, desc(0, 1, 0), "End", pct(S.strum.end));
  same();
  knob("##set", &S.strum.endTension, desc(-1, 1, 0), "Tension", spct(S.strum.endTension), true);
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ui::Label("DIRECTION");
  static const char* const dirs[] = {"Up", "Down", "Alternate"};
  ui::Segmented("##sdir", dirs, 3, &S.strum.direction, 70);
  ImGui::EndGroup();
  tools::StrumOpts o = S.strum;
  o.time = static_cast<Tick>(std::llround(S.strumBeats * s.ppq));
  auto n = T.base;
  auto sel = T.baseSel;
  tools::strumEx(n, sel, o);
  preview(a, "strum chords", n, sel);
}

void arpBody(App& a) {
  const Song& s = a.doc.song();
  section("PATTERN");
  combo("##apat", tools::arpPatternName(S.arp.pattern), 140, [&] {
    for (int i = 0; i < static_cast<int>(tools::ArpPattern::Count); ++i)
      if (ImGui::Selectable(tools::arpPatternName(static_cast<tools::ArpPattern>(i)), static_cast<int>(S.arp.pattern) == i))
        S.arp.pattern = static_cast<tools::ArpPattern>(i);
  });
  ImGui::SameLine();
  combo("##agrid", gridName(S.arpGrid), 130, [&] {
    for (int i = 0; i < kGridCount; ++i)
      if (ImGui::Selectable(kGrids[i].name, std::lround(S.arpGrid) == i)) S.arpGrid = i;
  });
  ui::Hint("Time between arpeggio notes");
  if (S.arp.pattern == tools::ArpPattern::Random) {
    ImGui::SameLine();
    seedButton("aseed", &S.arp.seed);
  }
  section("RANGE");
  static const char* const oct[] = {"1 oct", "2 oct", "3 oct", "4 oct"};
  int o = S.arp.octaves - 1;
  if (ui::Segmented("##aoct", oct, 4, &o, 52)) S.arp.octaves = o + 1;
  ImGui::SameLine();
  static const char* const modes[] = {"Normal", "Flip", "Alternate"};
  ui::Segmented("##arm", modes, 3, &S.arp.rangeMode, 70);
  ui::Hint("Octave passes: repeat · reverse every other pass · alternate each note with its octave");
  section("OPTIONS");
  knob("##ag", &S.arp.gate, desc(0.05, 1, 0.9), "Gate", pct(S.arp.gate));
  same();
  knob("##aa", &S.arp.velocityAccent, desc(0, 1, 0), "Accent", pct(S.arp.velocityAccent));
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ui::Label("SYNC");
  static const char* const sync[] = {"Grid", "Chord"};
  ui::Segmented("##async", sync, 2, &S.arp.sync, 60);
  ui::Hint("Grid: the pattern follows the song grid · Chord: it restarts at every chord");
  ImGui::EndGroup();
  tools::ArpOpts ao = S.arp;
  ao.step = gridTicks(s, S.arpGrid);
  auto n = T.base;
  auto sel = T.baseSel;
  tools::arpeggiateEx(n, sel, ao);
  preview(a, "arpeggiate", n, sel);
}

void flamBody(App& a) {
  const Song& s = a.doc.song();
  section("PRESET");
  const auto& fp = tools::flamPresets();
  combo("##fpre", fp[static_cast<size_t>(S.flamPreset)].name, 200, [&] {
    for (size_t i = 0; i < fp.size(); ++i)
      if (ImGui::Selectable(fp[i].name, S.flamPreset == static_cast<int>(i))) {
        S.flamPreset = static_cast<int>(i);
        S.flam = fp[i].opts;
        S.flamTime48 = static_cast<double>(fp[i].opts.time);
      }
  });
  section("OPTIONS");
  knob("##ft", &S.flamTime48, desc(1, 24, 4), "Time", fmt("%.0f/48 beat", S.flamTime48));
  same();
  knob("##fv", &S.flam.velocity, desc(0.05, 1, 0.55), "Velocity", pct(S.flam.velocity));
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ui::Label("GRACE NOTES");
  static const char* const cnt[] = {"One", "Two"};
  int c = S.flam.count - 1;
  if (ui::Segmented("##fc", cnt, 2, &c, 50)) S.flam.count = c + 1;
  static const char* const pos[] = {"Before", "On the beat"};
  int e = S.flam.early ? 0 : 1;
  if (ui::Segmented("##fe", pos, 2, &e, 90)) S.flam.early = e == 0;
  ImGui::EndGroup();
  tools::FlamOpts o = S.flam;
  o.time = std::max<Tick>(1, static_cast<Tick>(std::llround(S.flamTime48 * s.ppq / 48.0)));
  auto n = T.base;
  auto sel = T.baseSel;
  tools::flam(n, sel, o);
  preview(a, "grace notes", n, sel);
}

void articulateBody(App& a) {
  const Song& s = a.doc.song();
  section("PRESET");
  const auto& ap = tools::articulatePresets();
  combo("##apre", ap[static_cast<size_t>(S.artPreset)].name, 210, [&] {
    for (size_t i = 0; i < ap.size(); ++i)
      if (ImGui::Selectable(ap[i].name, S.artPreset == static_cast<int>(i))) {
        S.artPreset = static_cast<int>(i);
        S.art = ap[i].opts;
        S.artGap48 = static_cast<double>(ap[i].opts.gap);
      }
  });
  section("OPTIONS");
  knob("##al", &S.art.length, desc(0.05, 2, 1), "Length", pct(S.art.length));
  same();
  knob("##agap", &S.artGap48, desc(0, 24, 0), "Gap", fmt("%.0f/48 beat", S.artGap48));
  same();
  knob("##av", &S.art.variation, desc(0, 1, 0), "Variation", pct(S.art.variation));
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ui::Label("MODE");
  if (ui::TextButton("Trim overlaps only", S.art.onlyOverlaps, 0, "Only shorten notes that run into the next note"))
    S.art.onlyOverlaps = !S.art.onlyOverlaps;
  if (S.art.variation > 0) seedButton("artseed", &S.art.seed);
  ImGui::EndGroup();
  tools::ArticulateOpts o = S.art;
  o.gap = static_cast<Tick>(std::llround(S.artGap48 * s.ppq / 48.0));
  auto n = T.base;
  auto sel = T.baseSel;
  tools::articulate(n, sel, o);
  preview(a, "articulate notes", n, sel);
}

void randomizeBody(App& a) {
  const Song& s = a.doc.song();
  section("PATTERN");
  keyCombo("##rkey", &S.rnd.root);
  ImGui::SameLine();
  scaleCombo("##rsc", &S.rnd.scale, false);
  ImGui::SameLine();
  combo("##rgrid", gridName(S.rndGrid), 130, [&] {
    for (int i = 0; i < kGridCount; ++i)
      if (ImGui::Selectable(kGrids[i].name, std::lround(S.rndGrid) == i)) S.rndGrid = i;
  });
  ImGui::SameLine();
  seedButton("rseed", &S.rnd.seed);
  knobInt("##roct", &S.rnd.octave, 1, 8, "Octave", std::to_string(S.rnd.octave));
  same();
  knobInt("##rrng", &S.rnd.octaves, 1, 4, "Range", std::to_string(S.rnd.octaves) + " oct");
  same();
  knobInt("##rpoly", &S.rnd.maxPoly, 1, 6, "Max notes", std::to_string(S.rnd.maxPoly));
  same();
  knob("##rprob", &S.rnd.probability, desc(0, 1, 0.5), "Chance", pct(S.rnd.probability));
  same();
  knob("##rlen", &S.rnd.length, desc(0.25, 8, 1, ParamScale::Log), "Length", fmt("%.2f grid", S.rnd.length));
  same();
  knob("##rlv", &S.rnd.lengthVariation, desc(0, 1, 0), "Len. var.", pct(S.rnd.lengthVariation));
  same();
  knobInt("##rbars", &S.rndBars, 1, 16, "Bars", std::to_string(S.rndBars));
  section("LEVELS (RANDOM AMOUNT)");
  static const char* const ln[] = {"Velocity", "Pan", "Release", "Fine", "Mod X", "Mod Y"};
  for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k) {
    if (k) same();
    ImGui::PushID(k);
    knob("##lv", &S.levels.strength[k], desc(0, 1, 0), ln[k], pct(S.levels.strength[k]));
    ImGui::PopID();
  }
  if (ui::TextButton("Replace notes", S.rndReplace, 0, "Replace the notes in the generated range (off: add to them)")) S.rndReplace = !S.rndReplace;
  tools::RandomOpts o = S.rnd;
  o.step = gridTicks(s, S.rndGrid);
  o.span = s.barTicks() * S.rndBars;
  o.from = 0;
  auto gen = tools::randomNotes(o);
  tools::NoteSel gs(gen.size(), true);
  S.levels.seed = S.rnd.seed + 17;
  tools::randomizeLevels(gen, gs, S.levels);
  std::vector<Note> n;
  for (const auto& x : T.base)
    if (!S.rndReplace || x.start >= o.span) n.push_back(x);
  tools::NoteSel sel(n.size(), false);
  for (auto& x : gen) n.push_back(x), sel.push_back(true);
  if (const Pattern* p = s.pattern(T.pat); p && p->bars < S.rndBars) a.doc.setPatternLength(p->id, S.rndBars, p->stepsPerBar);
  preview(a, "randomize notes", n, sel);
}

void lfoBody(App& a) {
  const Song& s = a.doc.song();
  section("PROPERTY");
  static const char* const ln[] = {"Velocity", "Pan", "Release", "Fine", "Mod X", "Mod Y"};
  int pr = static_cast<int>(S.lfo.prop);
  if (ui::Segmented("##lprop", ln, 6, &pr, 62)) S.lfo.prop = static_cast<tools::NoteProp>(pr);
  ParamDesc dd = choiceDesc(kDivCount, 5);
  for (int side = 0; side < 2; ++side) {
    ImGui::PushID(side);
    if (side) ImGui::SameLine(0, theme::kS4);
    ImGui::BeginGroup();
    section(side ? "END" : "START");
    double& v = side ? S.lfo.endValue : S.lfo.startValue;
    double& r = side ? S.lfo.endRange : S.lfo.startRange;
    double& d = side ? S.lfoEndDiv : S.lfoStartDiv;
    knob("##v", &v, desc(0, 1, 0.5), "Value", pct(v));
    same();
    knob("##r", &r, desc(0, 1, 0.5), "Range", pct(r));
    same();
    knob("##d", &d, dd, "Speed", kDivs[std::clamp(static_cast<int>(std::lround(d)), 0, kDivCount - 1)].name);
    ImGui::EndGroup();
    ImGui::PopID();
  }
  section("SHAPE");
  static const char* const waves[] = {"Sine", "Triangle", "Square"};
  int w = static_cast<int>(S.lfo.wave);
  if (ui::Segmented("##lw", waves, 3, &w, 70)) S.lfo.wave = static_cast<tools::LfoWave>(w);
  if (S.lfo.wave == tools::LfoWave::Square) {
    ImGui::SameLine();
    knob("##lg", &S.lfo.gate, desc(0.05, 0.95, 0.5), "Gate", pct(S.lfo.gate));
  }
  tools::NoteLfoOpts o = S.lfo;
  o.startPeriod = divTicks(s, S.lfoStartDiv);
  o.endPeriod = divTicks(s, S.lfoEndDiv);
  auto n = T.base;
  auto sel = T.baseSel;
  tools::noteLfo(n, sel, o);
  preview(a, "note LFO", n, sel);
}

void scaleLevelsBody(App& a) {
  section("PROPERTY");
  static const char* const ln[] = {"Velocity", "Pan", "Release", "Fine", "Mod X", "Mod Y"};
  int pr = static_cast<int>(S.sl.prop);
  if (ui::Segmented("##slprop", ln, 6, &pr, 62)) S.sl.prop = static_cast<tools::NoteProp>(pr);
  section("SCALING");
  knob("##slc", &S.sl.centre, desc(0, 1, 0.5), "Pivot", pct(S.sl.centre));
  same();
  knob("##sls", &S.sl.scale, desc(-2, 2, 1), "Scale", pct(S.sl.scale), true);
  same();
  knob("##slo", &S.sl.offset, desc(-1, 1, 0), "Offset", spct(S.sl.offset), true);
  same();
  knob("##slf", &S.sl.rampFrom, desc(0, 2, 1), "Ramp from", pct(S.sl.rampFrom));
  same();
  knob("##slt", &S.sl.rampTo, desc(0, 2, 1), "Ramp to", pct(S.sl.rampTo));
  auto n = T.base;
  auto sel = T.baseSel;
  tools::scaleLevels(n, sel, S.sl);
  preview(a, "scale levels", n, sel);
}

void limitBody(App& a) {
  section("KEY RANGE");
  knobInt("##llo", &S.limLo, 0, 127, "Lowest", tools::pitchName(S.limLo));
  same();
  knobInt("##lhi", &S.limHi, 0, 127, "Highest", tools::pitchName(S.limHi));
  ImGui::SameLine(0, theme::kS3);
  ImGui::BeginGroup();
  ui::Label("NOTES OUTSIDE");
  static const char* const m[] = {"Fold", "Clamp", "Remove"};
  ui::Segmented("##lmode", m, 3, &S.limMode, 64);
  ui::Hint("Fold moves notes by octaves into the range · Clamp pins them to its edges · Remove deletes them");
  ImGui::EndGroup();
  auto n = T.base;
  auto sel = T.baseSel;
  tools::limitKeys(n, sel, S.limLo, S.limHi, static_cast<tools::LimitMode>(S.limMode));
  preview(a, "limit key range", n, sel);
}

void riffBody(App& a) {
  const Song& s = a.doc.song();
  section("KEY");
  keyCombo("##fkey", &S.riff.root);
  ImGui::SameLine();
  scaleCombo("##fsc", &S.riff.scale, false);
  ImGui::SameLine();
  combo("##fgrid", gridName(S.riffGrid), 130, [&] {
    for (int i = 0; i < kGridCount; ++i)
      if (ImGui::Selectable(kGrids[i].name, std::lround(S.riffGrid) == i)) S.riffGrid = i;
  });
  ImGui::SameLine();
  seedButton("fseed", &S.riff.seed);
  section("RHYTHM");
  static const char* const rh[] = {"Even", "Syncopated", "Long", "Driving"};
  ui::Segmented("##frh", rh, 4, &S.riff.rhythm, 82);
  knob("##fden", &S.riff.density, desc(0.05, 1, 0.55), "Density", pct(S.riff.density));
  same();
  knob("##frep", &S.riff.repeat, desc(0, 1, 0.5), "Repeat", pct(S.riff.repeat));
  same();
  knob("##fleap", &S.riff.leap, desc(0, 1, 0.25), "Leaps", pct(S.riff.leap));
  same();
  knobInt("##frng", &S.riff.range, 2, 14, "Range", std::to_string(S.riff.range) + " deg.");
  same();
  knobInt("##foct", &S.riff.octave, 2, 7, "Octave", std::to_string(S.riff.octave - 1));
  same();
  knobInt("##fbars", &S.riff.bars, 1, 16, "Bars", std::to_string(S.riff.bars));
  same();
  knobInt("##fvel", &S.riff.velocity, 1, 127, "Velocity", std::to_string(S.riff.velocity));
  if (ui::TextButton("Chords", S.riff.chords, 0, "Add a chord under the first note of every bar")) S.riff.chords = !S.riff.chords;
  ImGui::SameLine();
  if (ui::TextButton("Replace notes", S.riffReplace, 0, "Replace the notes in the riff's range (off: add to them)")) S.riffReplace = !S.riffReplace;
  tools::RiffOpts o = S.riff;
  o.barTicks = s.barTicks();
  o.step = gridTicks(s, S.riffGrid);
  auto gen = tools::generateRiff(o);
  const Tick span = o.barTicks * o.bars;
  std::vector<Note> n;
  for (const auto& x : T.base)
    if (!S.riffReplace || x.start >= span) n.push_back(x);
  tools::NoteSel sel(n.size(), false);
  for (auto& x : gen) n.push_back(x), sel.push_back(true);
  if (const Pattern* p = s.pattern(T.pat); p && p->bars < o.bars) a.doc.setPatternLength(p->id, o.bars, p->stepsPerBar);
  preview(a, "generate riff", n, sel);
}

void propertiesBody(App& a) {
  section("LEVELS (MOVED CONTROLS ARE APPLIED)");
  static const char* const ln[] = {"Velocity", "Pan", "Release", "Fine pitch", "Mod X", "Mod Y"};
  for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k) {
    if (k) same();
    ImGui::PushID(k);
    const auto prop = static_cast<tools::NoteProp>(k);
    Note probe;
    tools::setNotePropNorm(probe, prop, S.props[k]);
    std::string txt;
    switch (prop) {
      case tools::NoteProp::Velocity: txt = std::to_string(probe.velocity); break;
      case tools::NoteProp::Pan: txt = probe.pan == 0 ? "C" : (probe.pan < 0 ? "L" : "R") + std::to_string(std::abs(probe.pan)); break;
      case tools::NoteProp::Release: txt = std::to_string(probe.release); break;
      case tools::NoteProp::Fine: txt = fmt("%+.0f ct", probe.fine); break;
      case tools::NoteProp::ModX: txt = pct(probe.modX / 255.0); break;
      default: txt = pct(probe.modY / 255.0); break;
    }
    if (!S.propsTouched[k]) txt = "\xE2\x80\x94";
    if (knob("##p", &S.props[k], desc(0, 1, k == 0 ? 0.79 : 0.5), ln[k], txt, tools::notePropBipolar(prop))) S.propsTouched[k] = true;
    ImGui::PopID();
  }
  section("COLOUR GROUP");
  for (int c = -1; c < kNoteColors; ++c) {
    if (c > -1) ImGui::SameLine(0, 3);
    ImGui::PushID(c + 100);
    std::string l = c < 0 ? "Keep" : std::to_string(c + 1);
    if (ui::TextButton(l.c_str(), S.propsColor == c, c < 0 ? 44.0f : 24.0f)) S.propsColor = c;
    ImGui::PopID();
  }
  auto n = T.base;
  auto sel = T.baseSel;
  auto t = sel;
  bool any = std::find(t.begin(), t.end(), true) != t.end();
  for (size_t i = 0; i < n.size(); ++i) {
    if (any && !t[i]) continue;
    for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k)
      if (S.propsTouched[k]) tools::setNotePropNorm(n[i], static_cast<tools::NoteProp>(k), S.props[k]);
    if (S.propsColor >= 0) n[i].color = S.propsColor;
  }
  preview(a, "note properties", n, sel);
}

// -------------------------------------------------------------- step tools ----
void stepBody(App& a) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(T.pat);
  if (!p) return;
  tools::StepRow row = T.baseRow;
  tools::StepPropRow props = T.baseProps;
  if (T.stepTool == StepTool::Fill) {
    section("TYPE");
    static const char* const types[] = {"Every N steps", "Even spread"};
    ui::Segmented("##ft", types, 2, &S.fillType, 110);
    ui::Hint("Every N: a hit every N steps from the offset · Even spread: N hits spread as evenly as possible over the length");
    section("SEQUENCE");
    const int total = static_cast<int>(row.size());
    if (S.fillType == 0) {
      knob("##fe", &S.fillEvery, desc(1, 16, 4), "Every", fmt("%.0f steps", std::round(S.fillEvery)));
      same();
      knob("##fo", &S.fillOffset, desc(0, 15, 0), "Offset", fmt("%.0f", std::round(S.fillOffset)));
    } else {
      knob("##fh", &S.fillHits, desc(1, 32, 5), "Hits", fmt("%.0f", std::round(S.fillHits)));
      same();
      knob("##fl", &S.fillLength, desc(2, 32, 16), "Length", fmt("%.0f steps", std::round(S.fillLength)));
      same();
      knob("##fr", &S.fillRotation, desc(0, 31, 0), "Rotate", fmt("%.0f", std::round(S.fillRotation)));
    }
    same();
    knob("##fv", &S.fillVel, desc(1, 127, 100), "Velocity", fmt("%.0f", std::round(S.fillVel)));
    const auto vel = static_cast<uint8_t>(std::lround(S.fillVel));
    if (S.fillType == 0)
      tools::fillEvery(row, static_cast<int>(std::lround(S.fillEvery)), static_cast<int>(std::lround(S.fillOffset)), vel);
    else
      tools::euclidSteps(row, static_cast<int>(std::lround(S.fillHits)), std::min(total, static_cast<int>(std::lround(S.fillLength))),
                         static_cast<int>(std::lround(S.fillRotation)), vel);
  } else {
    section("RANDOM STEPS");
    knob("##rp", &S.rndProb, desc(0, 1, 0.4), "Chance", pct(S.rndProb));
    same();
    knob("##rl", &S.rndVelLo, desc(1, 127, 70), "Vel. low", fmt("%.0f", std::round(S.rndVelLo)));
    same();
    knob("##rh", &S.rndVelHi, desc(1, 127, 115), "Vel. high", fmt("%.0f", std::round(S.rndVelHi)));
    ImGui::SameLine(0, theme::kS3);
    seedButton("sseed", &S.stepSeed);
    tools::randomizeSteps(row, S.rndProb, static_cast<int>(std::lround(S.rndVelLo)), static_cast<int>(std::lround(S.rndVelHi)), S.stepSeed);
  }
  // Preview strip of the resulting steps.
  ImGui::Dummy(ImVec2(0, 4));
  {
    const auto& P = pal();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q = ImGui::GetCursorScreenPos();
    const float w = std::clamp(420.0f / std::max<size_t>(1, row.size()), 6.0f, 20.0f);
    const Channel* c = s.channel(T.ch);
    for (size_t k = 0; k < row.size(); ++k) {
      ImVec2 a0(q.x + k * (w + 1) + (k / 4) * 3, q.y), a1(a0.x + w, a0.y + 16);
      ui::StepPad(dl, a0, a1, row[k] > 0, (k / 4) % 2 == 1, false, c ? theme::rgb(c->color) : P.accent, row[k] / 127.0f);
    }
    ImGui::Dummy(ImVec2(row.size() * (w + 1) + (row.size() / 4) * 3, 18));
  }
  tools::StepRow curRow;
  tools::StepPropRow curProps;
  ops::readStepRow(*p, T.ch, curRow, curProps);
  if (curRow != row) {
    ops::editStepRow(a.doc, T.pat, T.ch, T.stepTool == StepTool::Fill ? "fill steps" : "randomize steps",
                     [&](tools::StepRow& r, tools::StepPropRow& pp) { r = row, pp = props; }, T.key);
    T.changed = true;
  }
}

}  // namespace

const char* noteToolTitle(NoteTool t) {
  switch (t) {
    case NoteTool::Quantize: return "Quantize";
    case NoteTool::Chop: return "Chop";
    case NoteTool::Strum: return "Strum";
    case NoteTool::Arpeggiate: return "Arpeggiate";
    case NoteTool::Flam: return "Grace notes";
    case NoteTool::Articulate: return "Articulate";
    case NoteTool::Randomize: return "Randomize";
    case NoteTool::Lfo: return "Note LFO";
    case NoteTool::ScaleLevels: return "Scale levels";
    case NoteTool::Limit: return "Limit key range";
    case NoteTool::Riff: return "Riff generator";
    case NoteTool::Properties: return "Note properties";
    default: return "";
  }
}

bool noteToolOpen() { return T.tool != NoteTool::None || T.stepTool != StepTool::None; }

void openNoteTool(App& a, NoteTool t) {
  if (noteToolOpen()) close(a, true);
  const Song& s = a.doc.song();
  if (!s.pattern(a.curPattern) || !s.channel(a.selChannel)) {
    a.notify("Select a pattern and a channel first");
    return;
  }
  T = Session{};
  T.tool = t;
  T.pat = a.curPattern;
  T.ch = a.selChannel;
  T.key = 0x4A31310000000000ull ^ (sessionCounter++ << 4);
  T.focus = true;
  if (const auto* n = targetNotes(s)) T.base = *n;
  T.baseSel.assign(T.base.size(), false);
  if (a.piano.selPattern == T.pat && a.piano.selChannel == T.ch)
    for (size_t i : a.piano.selected)
      if (i < T.baseSel.size()) T.baseSel[i] = true;
  if (t == NoteTool::Properties) {  // start from the first targeted note's values
    for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k) S.propsTouched[k] = false;
    S.propsColor = -1;
    for (size_t i = 0; i < T.base.size(); ++i) {
      if (std::find(T.baseSel.begin(), T.baseSel.end(), true) != T.baseSel.end() && !T.baseSel[i]) continue;
      for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k)
        S.props[k] = tools::notePropNorm(T.base[i], static_cast<tools::NoteProp>(k));
      break;
    }
  }
  if (t == NoteTool::Randomize || t == NoteTool::Riff) {  // the piano roll's scale, when one is set
    if (a.piano.scale > 0) {
      S.rnd.root = S.riff.root = a.piano.scaleRoot;
      S.rnd.scale = S.riff.scale = a.piano.scale;
    }
  }
}

void openStepTool(App& a, StepTool t, ObjectId channel) {
  if (noteToolOpen()) close(a, true);
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  if (!p || !s.channel(channel)) return;
  T = Session{};
  T.stepTool = t;
  T.pat = p->id;
  T.ch = channel;
  T.key = 0x4A31315300000000ull ^ (sessionCounter++ << 4);
  T.focus = true;
  ops::readStepRow(*p, T.ch, T.baseRow, T.baseProps);
}

void drawEditTools(App& a) {
  if (!noteToolOpen()) return;
  const Song& s = a.doc.song();
  if (!s.pattern(T.pat) || !s.channel(T.ch)) {
    T = Session{};
    return;
  }
  const bool steps = T.stepTool != StepTool::None;
  std::string title = steps ? (T.stepTool == StepTool::Fill ? "Advanced fill" : "Randomize steps") : noteToolTitle(T.tool);
  title += " \xE2\x80\x94 " + s.channel(T.ch)->name + "###j11tool";
  ImGuiViewport* vp = ImGui::GetMainViewport();
  // Low on the screen so the edited notes / steps stay visible above it.
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 40), ImGuiCond_Appearing,
                          ImVec2(0.5f, 1.0f));
  if (T.focus) ImGui::SetNextWindowFocus(), T.focus = false;
  bool open = true;
  if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) close(a, false);
    return;
  }
  const auto& P = pal();
  if (!steps) ImGui::TextColored(theme::vec(P.textDim), "%s", targetLine(a).c_str());
  switch (T.tool) {
    case NoteTool::Quantize: quantizeBody(a); break;
    case NoteTool::Chop: chopBody(a); break;
    case NoteTool::Strum: strumBody(a); break;
    case NoteTool::Arpeggiate: arpBody(a); break;
    case NoteTool::Flam: flamBody(a); break;
    case NoteTool::Articulate: articulateBody(a); break;
    case NoteTool::Randomize: randomizeBody(a); break;
    case NoteTool::Lfo: lfoBody(a); break;
    case NoteTool::ScaleLevels: scaleLevelsBody(a); break;
    case NoteTool::Limit: limitBody(a); break;
    case NoteTool::Riff: riffBody(a); break;
    case NoteTool::Properties: propertiesBody(a); break;
    default: if (steps) stepBody(a); break;
  }
  ImGui::Dummy(ImVec2(0, 6));
  bool accept = ui::TextButton("Accept", true, 90, "Keep the changes (Enter)");
  ImGui::SameLine();
  if (ui::TextButton("Reset", false, 90, "Back to the default settings")) {
    ToolSettings fresh;
    switch (T.tool) {
      case NoteTool::Quantize: S.q = fresh.q, S.qGrid = fresh.qGrid, S.qDuration = 0, S.qGrooveChannel = -1; break;
      case NoteTool::Chop: S.chopPattern = 0, S.chopTime = 1, S.chopGate = 1; break;
      case NoteTool::Strum: S.strum = fresh.strum, S.strumBeats = fresh.strumBeats; break;
      case NoteTool::Arpeggiate: S.arp = fresh.arp, S.arpGrid = fresh.arpGrid; break;
      case NoteTool::Flam: S.flam = fresh.flam, S.flamTime48 = fresh.flamTime48, S.flamPreset = 0; break;
      case NoteTool::Articulate: S.art = fresh.art, S.artGap48 = 0, S.artPreset = 0; break;
      case NoteTool::Randomize: S.rnd = fresh.rnd, S.levels = fresh.levels, S.rndGrid = fresh.rndGrid, S.rndBars = 1; break;
      case NoteTool::Lfo: S.lfo = fresh.lfo, S.lfoStartDiv = S.lfoEndDiv = 5; break;
      case NoteTool::ScaleLevels: S.sl = fresh.sl; break;
      case NoteTool::Limit: S.limLo = 48, S.limHi = 72, S.limMode = 0; break;
      case NoteTool::Riff: S.riff = fresh.riff, S.riffGrid = fresh.riffGrid; break;
      case NoteTool::Properties:
        for (int k = 0; k < static_cast<int>(tools::NoteProp::Count); ++k) S.propsTouched[k] = false;
        S.propsColor = -1;
        break;
      default:
        S.fillType = 0, S.fillEvery = 4, S.fillOffset = 0, S.fillHits = 5, S.fillLength = 16, S.fillRotation = 0, S.fillVel = 100;
        S.rndProb = 0.4, S.rndVelLo = 70, S.rndVelHi = 115;
        break;
    }
  }
  ImGui::SameLine();
  bool cancel = ui::TextButton("Cancel", false, 90, "Close and restore the notes (Esc)");
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
  }
  ui::KeepWindowInViewport();
  ImGui::End();
  if (accept) {
    if (T.changed) a.notify(std::string(steps ? "Steps" : noteToolTitle(T.tool)) + " applied");
    close(a, true);
  } else if (cancel || !open) {
    close(a, false);
  }
}

void devOpenTool(App& a, const std::string& name) {
  static const std::pair<const char*, NoteTool> map[] = {
      {"quantize", NoteTool::Quantize}, {"chop", NoteTool::Chop},       {"strum", NoteTool::Strum},
      {"arp", NoteTool::Arpeggiate},    {"flam", NoteTool::Flam},       {"articulate", NoteTool::Articulate},
      {"randomize", NoteTool::Randomize}, {"lfo", NoteTool::Lfo},       {"levels", NoteTool::ScaleLevels},
      {"limit", NoteTool::Limit},       {"riff", NoteTool::Riff},       {"props", NoteTool::Properties}};
  for (const auto& [n, t] : map)
    if (name == n) return openNoteTool(a, t);
  if (name == "fill") openStepTool(a, StepTool::Fill, a.selChannel);
  if (name == "randsteps") openStepTool(a, StepTool::Randomize, a.selChannel);
  if (name == "chords") j31::openChordEditor(a);  // J-31 chord progression editor
}

}  // namespace gl
