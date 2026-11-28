// Gridline — "seltmann" pack: the editor's cards. Everything that used to hide in a drawer opens as a
// small card right where it is asked for, one at a time, over the editor (a window of its own, so a
// card is never cut off by a short device band):
//   Region / This step  a click on a region in the waveform or a block of the sequence: the region's
//                       pitch, gain, fades, start, length, choke, reverse, split, join, start here; the
//                       step's own pitch, gain, start, length, stutter, reverse, rest, length, copies
//   Sound               the header's Sound: keys, speed, lock, mono, base note, envelope, filter, mix
//   Grid                the header's Grid: tap, nudge, meter, key, confidence, analyse again
//   Loops               the header's Loops: phrases and sections to drag out
//   Chop                the action row's More...: every chop mode, snap, zero crossings
//   Steps               the waveform's Steps: step length, add or remove a step, the step language
// Esc, the close button or a click outside closes the card.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../../third_party/imgui/imgui_internal.h"
#include "../../app/Keys.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;

void openCard(Ed& e, int card, ImVec2 anchor, bool below) {
  e.card = card;
  e.cardAnchor = anchor;
  e.cardBelow = below;
  e.cardTouched = true;
}

void toggleCard(Ed& e, int card, ImVec2 anchor, bool below) {
  if (e.card == card) {
    closeCard(e);
    e.cardTouched = true;
  } else {
    openCard(e, card, anchor, below);
  }
}

void closeCard(Ed& e) { e.card = CardNone; }

namespace {

// ------------------------------------------------------------------ region / step
// The Region / This step switch (bottom right of the card).
void scopeSwitch(App& a, Ed& e, ImVec2 p0, float w) {
  static const char* const l[2] = {i18n::N_("Region"), i18n::N_("This step")};
  static const char* const h[2] = {i18n::N_("Settings of the region: every step that plays it"),
                                   i18n::N_("Settings of this step only (the other steps of the region keep theirs)")};
  int cur = e.card == CardStep ? 1 : 0;
  const int step = e.selStep >= 0 && e.selStep < static_cast<int>(e.m.pattern.steps.size()) ? e.selStep : firstStepOf(e.m, e.selSlice);
  if (pills(a, "##scardscope", p0, w, 24, l, 2, cur, h)) {
    if (cur == 1 && step >= 0) {
      e.selStep = step;
      e.card = CardStep;
    } else {
      e.card = CardRegion;
    }
  }
}

void regionCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (e.m.slices.empty()) return;
  e.selSlice = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
  Slice s = e.m.slices[static_cast<size_t>(e.selSlice)];
  float x = p0.x;
  float y = p0.y;
  bool rel = false, changed = false;
  const float kw = 66;
  float off = s.offset * 100, len = s.length * 100, choke = static_cast<float>(s.choke);
  changed |= fieldKnob(a, "##kp", ImVec2(x + 0 * kw, y), s.pitch, -24, 24, 0, i18n::N_("Pitch"), "%+.1f st", &rel, true);
  changed |= fieldKnob(a, "##kg", ImVec2(x + 1 * kw, y), s.gain, -24, 12, 0, i18n::N_("Gain"), "%+.1f dB", &rel, true);
  changed |= fieldKnob(a, "##kfi", ImVec2(x + 2 * kw, y), s.fadeIn, 0, 200, 0, i18n::N_("Fade in"), "%.0f ms", &rel);
  changed |= fieldKnob(a, "##kfo", ImVec2(x + 3 * kw, y), s.fadeOut, 0, 400, 0, i18n::N_("Fade out"), "%.0f ms", &rel);
  changed |= fieldKnob(a, "##ko", ImVec2(x + 4 * kw, y), off, 0, 90, 0, i18n::N_("Start"), "%.0f%%", &rel);
  changed |= fieldKnob(a, "##kl", ImVec2(x + 5 * kw, y), len, 5, 100, 100, i18n::N_("Length"), "%.0f%%", &rel);
  changed |= fieldKnob(a, "##kc", ImVec2(x + 6 * kw, y), choke, 0, 8, 0, i18n::N_("Choke"), "%.0f", &rel, false, 1);
  s.offset = off / 100, s.length = len / 100, s.choke = static_cast<int>(std::lround(choke));
  y += 92;
  if (button("##krev", ImVec2(x, y), ImVec2(76, 24), i18n::N_("Reverse"), s.reverse, i18n::N_("Play this region backwards"))) {
    s.reverse = !s.reverse;
    changed = true;
    rel = true;
  }
  noteBtn(e, "reverse");
  if (changed || rel) {
    Model nm = e.m;
    nm.slices[static_cast<size_t>(e.selSlice)] = s;
    if (rel) commit(a, t, e, nm, i18n::N_("Region settings"), 0x5E17D000ull + static_cast<uint64_t>(e.selSlice));
    else e.m = nm;  // live while dragging; written on release
  }
  x += 88;
  x += inlineLabel(dl, a, ImVec2(x, y), 24, i18n::N_("SPLIT"));
  {
    static const int parts[4] = {2, 3, 4, 8};
    static const char* const txt[4] = {"2", "3", "4", "8"};
    static const char* const hint[4] = {i18n::N_("Split the region in two (the next letters move on)"), i18n::N_("Split the region in three"),
                                        i18n::N_("Split the region in four"), i18n::N_("Split the region in eight")};
    for (int i = 0; i < 4; ++i) {
      char id[16];
      std::snprintf(id, sizeof id, "##ssplit%d", parts[i]);
      if (button(id, ImVec2(x + i * 28, y), ImVec2(26, 24), txt[i], false, hint[i])) splitRegionUi(a, t, e, e.selSlice, parts[i]);
      if (i == 0) noteBtn(e, "split2");
    }
    x += 4 * 28 + 10;
  }
  {
    const bool can = e.m.slices.size() > 1;
    const int with = e.selSlice > 0 ? e.selSlice - 1 : 1;
    char lb[32];
    std::snprintf(lb, sizeof lb, "%s %s", i18n::tr("Join"), regionName(with).c_str());
    if (button("##sjoin", ImVec2(x, y), ImVec2(62, 24), lb, false,
               can ? i18n::N_("Join the region with the one before it (the first one joins the next)") : i18n::N_("The last region cannot be joined")) &&
        can)
      deleteRegionUi(a, t, e, e.selSlice);
    noteBtn(e, "join");
    x += 70;
  }
  {
    const int step = firstStepOf(e.m, e.selSlice);
    if (button("##sstarthere", ImVec2(x, y), ImVec2(84, 24), i18n::N_("Start here"), false,
               step >= 0 ? i18n::N_("Start the sequence on this region (the steps before move to the end)") : i18n::N_("The region is not in the sequence")) &&
        step > 0)
      startHereUi(a, t, e, step, true);
    x += 92;
  }
  scopeSwitch(a, e, ImVec2(p0.x + w - 150, y), 150);
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 32), P.textDim,
              i18n::tr("Wheel over the region: pitch. Choke: regions in one group cut each other off. Esc closes."));
}

void stepCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int ns = static_cast<int>(e.m.pattern.steps.size());
  if (e.selStep < 0 || e.selStep >= ns) {
    e.card = CardRegion;
    return;
  }
  const int i = e.selStep;
  const int nSl = static_cast<int>(e.m.slices.size());
  Model nm = e.m;
  Step& st = nm.pattern.steps[static_cast<size_t>(i)];
  float x = p0.x, y = p0.y;
  bool rel = false, ch = false, structural = false;
  const float kw = 66;
  float pitch = st.pitch, gain = st.gain, len = st.length * 100, off = st.offset * 100, stut = static_cast<float>(st.stutter);
  ch |= fieldKnob(a, "##tp", ImVec2(x + 0 * kw, y), pitch, -24, 24, 0, i18n::N_("Pitch"), "%+.1f st", &rel, true);
  ch |= fieldKnob(a, "##tg", ImVec2(x + 1 * kw, y), gain, -30, 6, 0, i18n::N_("Gain"), "%+.1f dB", &rel, true);
  ch |= fieldKnob(a, "##to", ImVec2(x + 2 * kw, y), off, 0, 90, 0, i18n::N_("Start"), "%.0f%%", &rel);
  ch |= fieldKnob(a, "##tl", ImVec2(x + 3 * kw, y), len, 5, 100, 100, i18n::N_("Length"), "%.0f%%", &rel);
  ch |= fieldKnob(a, "##ts", ImVec2(x + 4 * kw, y), stut, 1, 16, 1, i18n::N_("Stutter"), "x%.0f", &rel, false, 1);
  st.pitch = pitch, st.gain = gain, st.length = len / 100, st.offset = off / 100, st.stutter = std::clamp(static_cast<int>(std::lround(stut)), 1, 16);
  // what the step plays: a region letter or a rest
  {
    const float px = x + 5 * kw + 8;
    groupLabel(dl, a, ImVec2(px, y), i18n::N_("PLAYS"));
    const int k = st.src == kRest ? -1 : stepSlice(nm.pattern, st, nSl);
    ImGui::SetCursorScreenPos(ImVec2(px, y + 16));
    ImGui::SetNextItemWidth(96);
    if (ImGui::BeginCombo("##tplays", k < 0 ? i18n::tr("Rest") : regionName(k).c_str())) {
      if (ImGui::Selectable(i18n::tr("Rest"), k < 0)) st.src = kRest, st.slot = false, structural = true;
      for (int l = 0; l < nSl && l < 128; ++l)
        if (ImGui::Selectable(regionName(l).c_str(), l == k)) st.slot = false, st.src = l, structural = true;
      ImGui::EndCombo();
    }
  }
  y += 92;
  if (button("##trev", ImVec2(x, y), ImVec2(76, 24), i18n::N_("Reverse"), st.reverse, i18n::N_("Play this step backwards"))) {
    st.reverse = !st.reverse;
    structural = true;
  }
  x += 84;
  // length: a unit more or less (or the step's own length back to its default)
  {
    const double unit = std::max(kMinStepBeats, nm.pattern.unitBeats);
    if (button("##tlonger", ImVec2(x, y), ImVec2(64, 24), i18n::N_("Longer"), false, i18n::N_("The step lasts one step length more"))) {
      if (st.beats > 0) structural = setStepBeats(nm, i, st.beats + unit) || structural;
      else ++st.ties, structural = true;
    }
    x += 68;
    const bool canShorter = st.beats > 0 ? st.beats > unit + 1e-9 : st.ties > 0;
    if (button("##tshorter", ImVec2(x, y), ImVec2(64, 24), i18n::N_("Shorter"), false, i18n::N_("The step lasts one step length less")) && canShorter) {
      if (st.beats > 0) structural = setStepBeats(nm, i, st.beats - unit) || structural;
      else --st.ties, structural = true;
    }
    x += 72;
  }
  if (button("##tdup", ImVec2(x, y), ImVec2(64, 24), i18n::N_("Copy"), false, i18n::N_("Duplicate the step (the copy follows it)"))) {
    const Step copy = st;
    nm.pattern.steps.insert(nm.pattern.steps.begin() + i + 1, copy);
    structural = true;
  }
  x += 72;
  if (button("##tdel", ImVec2(x, y), ImVec2(64, 24), i18n::N_("Delete"), false, ns > 1 ? i18n::N_("Delete the step") : i18n::N_("The last step stays")) &&
      ns > 1) {
    nm.pattern.steps.erase(nm.pattern.steps.begin() + i);
    e.selStep = -1;
    structural = true;
  }
  if (ch || rel || structural) {
    markEdited(nm);
    if (rel || structural) commit(a, t, e, nm, i18n::N_("Edit a step"), structural ? 0 : 0x5E17F000ull + static_cast<uint64_t>(i));
    else e.m = nm;  // live while dragging
  }
  scopeSwitch(a, e, ImVec2(p0.x + w - 150, y), 150);
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 32), P.textDim,
              i18n::tr("Wheel over a block: its pitch. Drag a block to move it, its right edge to set its length."));
}

// ------------------------------------------------------------------ sound
void soundCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float x = p0.x;
  const float y = p0.y;
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("KEYS PLAY"));
  {
    static const char* const l[2] = {i18n::N_("Regions"), i18n::N_("Letters")};
    static const char* const h[2] = {i18n::N_("Keys from the base note play the regions in order"),
                                     i18n::N_("Keys from the base note play the letters of older layouts")};
    paramPills(a, t, SmKeyMap, ImVec2(x, y + 16), 150, 26, l, 2, h);
  }
  x += 166;
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("SPEED"));
  {
    static const char* const l[3] = {"1/2", "1x", "2x"};
    static const char* const h[3] = {i18n::N_("Pattern at half speed"), i18n::N_("Pattern at normal speed"), i18n::N_("Pattern at double speed")};
    paramPills(a, t, SmSpeed, ImVec2(x, y + 16), 120, 26, l, 3, h);
  }
  x += 136;
  ImGui::SetCursorScreenPos(ImVec2(x, y + 18));
  pui::led(a, t, SmLock, i18n::tr("Lock to song"));
  ImGui::SetCursorScreenPos(ImVec2(x + 120, y + 18));
  pui::led(a, t, SmMono, i18n::tr("Mono"));
  x += 220;
  ImGui::SetCursorScreenPos(ImVec2(x, y + 16));
  pui::stepper(a, t, SmBaseNote, 120, i18n::tr("Base"));
  {
    char b[48];
    std::snprintf(b, sizeof b, "%s %s", i18n::tr("pattern key"), noteName(baseNote(a, t) - 12).c_str());
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(x, y + 46), P.textFaint, b);
  }
  const float ky = y + 64;
  const float kw = pui::knobCellWidth(pui::KnobSmall) + 4;
  const int params[13] = {SmSwing, SmGate, SmVelocity, SmPitch, SmAttack, SmDecay, SmSustain, SmRelease, SmDeclick, SmCutoff, SmReso, SmPan, SmVolume};
  const char* names[13] = {i18n::N_("Swing"),   i18n::N_("Gate"),    i18n::N_("Velocity"), i18n::N_("Pitch"),     i18n::N_("Attack"),
                           i18n::N_("Decay"),   i18n::N_("Sustain"), i18n::N_("Release"),  i18n::N_("Declick"),   i18n::N_("Cutoff"),
                           i18n::N_("Resonance"), i18n::N_("Pan"),   i18n::N_("Volume")};
  for (int k = 0; k < 13; ++k) pui::knobAt(a, t, params[k], ImVec2(p0.x + k * kw, ky), i18n::tr(names[k]), pui::KnobSmall);
  ImGui::SetCursorScreenPos(ImVec2(p0.x, ky + pui::knobCellHeight(pui::KnobSmall) + 8));
  pui::led(a, t, SmReverse, i18n::tr("Reverse every region"));
  (void)w;
  (void)e;
}

// ------------------------------------------------------------------ steps
void stepsCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Model& m = e.m;
  float x = p0.x;
  groupLabel(dl, a, ImVec2(x, p0.y), i18n::N_("STEP LENGTH"));
  static const char* const unitLabels[8] = {"1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
  static const int unitDiv[8] = {Div1Bar, DivHalf, DivBeat, Div8th, Div16th, Div32nd, Div8T, Div16T};
  static const char* const unitH[8] = {i18n::N_("Steps of one bar"),     i18n::N_("Steps of half a bar"),  i18n::N_("Steps of a beat"),
                                       i18n::N_("Steps of an eighth"),   i18n::N_("Steps of a sixteenth"), i18n::N_("Steps of a thirty-second"),
                                       i18n::N_("Eighth-triplet steps"), i18n::N_("Sixteenth-triplet steps")};
  int cu = -1;
  for (int i = 0; i < 8; ++i)
    if (std::fabs(divisionBeats(unitDiv[i], m.grid.beatsPerBar) - m.pattern.unitBeats) < 1e-6) cu = i;
  if (pills(a, "##sunit", ImVec2(x, p0.y + 16), 340, 26, unitLabels, 8, cu, unitH, true) && cu >= 0) {
    Model nm = m;
    nm.pattern.unitBeats = divisionBeats(unitDiv[cu], nm.grid.beatsPerBar);
    commit(a, t, e, nm, i18n::N_("Step length"));
  }
  x += 352;
  groupLabel(dl, a, ImVec2(x, p0.y), i18n::N_("STEPS"));
  if (button("##sminus", ImVec2(x, p0.y + 16), ImVec2(30, 26), "", false, i18n::N_("Remove the last step"), static_cast<int>(ui::Icon::Minus)) &&
      m.pattern.steps.size() > 1) {
    Model nm = m;
    nm.pattern.steps.pop_back();
    markEdited(nm);
    commit(a, t, e, nm, i18n::N_("Remove a step"));
  }
  if (button("##splus", ImVec2(x + 34, p0.y + 16), ImVec2(30, 26), "", false, i18n::N_("Add a step (the next region)"), static_cast<int>(ui::Icon::Plus)) &&
      m.pattern.steps.size() < static_cast<size_t>(kMaxSteps)) {
    Model nm = m;
    Step st;
    st.slot = false;
    st.src = static_cast<int>(nm.pattern.steps.size()) % std::max(1, static_cast<int>(nm.slices.size()));
    nm.pattern.steps.push_back(st);
    markEdited(nm);
    commit(a, t, e, nm, i18n::N_("Add a step"));
  }
  noteBtn(e, "addstep");
  x += 80;
  const bool natural = isNaturalOrder(m);
  if (button("##sseqreset", ImVec2(x, p0.y + 16), ImVec2(120, 26), i18n::N_("Reset order"), false,
             natural ? i18n::N_("The sequence already plays every region in order")
                     : i18n::N_("Play every region once, in order (A B C D ...), each as long as its region"),
             static_cast<int>(ui::Icon::Loop)) &&
      !natural) {
    Model nm = m;
    nm.preset.clear();
    naturalPattern(nm);
    commit(a, t, e, nm, i18n::N_("Reset the order"));
    e.selStep = -1;
  }
  // the step language
  const std::string cur = patternToText(m.pattern);
  if (cur != e.dslFor) {
    std::snprintf(e.dsl, sizeof e.dsl, "%s", cur.c_str());
    e.dslFor = cur;
  }
  const float ly = p0.y + 52;
  groupLabel(dl, a, ImVec2(p0.x, ly + 6), i18n::N_("TYPE"));
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 44, ly));
  ImGui::SetNextItemWidth(w - 44);
  ImGui::PushFont(a.fontMono);
  const bool enter = ImGui::InputText("##sdsl", e.dsl, sizeof e.dsl, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopFont();
  if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
    ui::Tooltip(i18n::tr("Type the steps: 1, 2, 3 ... = the regions A, B, C ... in order; . = rest, ~ = hold. Options after a colon: r reverse, "
                         "p+7 pitch, g-6 gain dB, l50 length %, x4 stutter, o25 start %. Enter applies."));
  if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
    Pattern p = m.pattern;
    std::string err;
    if (patternFromText(e.dsl, p, &err)) {
      Model nm = m;
      nm.pattern.steps = p.steps;
      resolveSlots(nm);  // one letter system: typed letters become their regions
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Type the steps"));
    } else {
      a.notify(std::string(i18n::tr("Steps not changed: ")) + err, true);
      e.dslFor.clear();
    }
  }
}

// ------------------------------------------------------------------ loops
void loopsCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const float w = p1.x - p0.x;
  ImGui::SetCursorScreenPos(p0);
  ImGui::BeginChild("##sloops", ImVec2(w, p1.y - p0.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const int bpb = std::max(1, e.m.grid.beatsPerBar);
  struct Row {
    std::string item, label, info;
    int color;
    float score;
  };
  std::vector<Row> rows;
  {
    char b[96];
    const PluginState* sp = t.state(a.doc.song());
    const double pb = patternBars(e.m, sp ? sp->params : std::vector<float>{}, bpb);
    std::snprintf(b, sizeof b, "%.4g %s", pb, std::fabs(pb - 1) < 1e-9 ? i18n::tr("bar") : i18n::tr("bars"));
    rows.push_back({"pattern", std::string(i18n::tr("Pattern")) + ": " + e.m.pattern.name, b, -1, -1});
  }
  for (size_t i = 0; i < e.m.grid.loops.size(); ++i) {
    const auto& l = e.m.grid.loops[i];
    const int bar = static_cast<int>(std::floor(static_cast<double>(l.firstBeat - e.m.grid.bar1) / bpb)) + 1;
    char it[64], lab[64], info[64];
    std::snprintf(it, sizeof it, "span:%d:%d", l.firstBeat, l.firstBeat + l.bars * bpb);
    std::snprintf(lab, sizeof lab, "%s %d", i18n::tr("Phrase at bar"), bar);
    std::snprintf(info, sizeof info, "%d %s", l.bars, l.bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
    int sec = 0;
    for (const auto& s : e.m.grid.sections)
      if (l.firstBeat >= s.firstBeat && l.firstBeat < s.endBeat) sec = s.label;
    rows.push_back({it, lab, info, sec, l.score});
  }
  for (size_t i = 0; i < e.m.grid.sections.size(); ++i) {
    const auto& s = e.m.grid.sections[i];
    char it[64], lab[64], info[64];
    std::snprintf(it, sizeof it, "span:%d:%d", s.firstBeat, s.endBeat);
    std::snprintf(lab, sizeof lab, "%s %s%zu", i18n::tr("Section"), slotName(s.label), i + 1);
    std::snprintf(info, sizeof info, "%d %s", (s.endBeat - s.firstBeat) / bpb, i18n::tr("bars"));
    rows.push_back({it, lab, info, s.label, -1});
  }
  const float rowH = 34, colGap = 12;
  const float colW = std::floor((w - colGap - 10) / 2);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    const int col = static_cast<int>(i % 2), line = static_cast<int>(i / 2);
    const ImVec2 r0(o.x + col * (colW + colGap), o.y + line * rowH), r1(r0.x + colW, r0.y + rowH - 4);
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetCursorScreenPos(r0);
    ImGui::InvisibleButton("##lrow", ImVec2(colW - 34, rowH - 4));
    const bool hv = ImGui::IsItemHovered();
    if (hv && !ImGui::IsItemActive()) ui::Tooltip(i18n::tr("Drag into the playlist (audio clip), the channel rack (sampler) or the browser. Right-click to add it without dragging"));
    if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dragOut(a, t, e, r.item, r.label);
    cl->AddRectFilled(r0, r1, hv ? P.surface700 : P.surface900, theme::kRadius);
    cl->AddRectFilled(r0, ImVec2(r0.x + 4, r1.y), r.color >= 0 ? slotColor(r.color) : accent(), 1);
    grip(cl, ImVec2(r0.x + 15, (r0.y + r1.y) * 0.5f), hv ? P.text : P.textFaint);
    theme::textEllipsis(cl, a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 26, r0.y + 8), colW - 150, P.text, r.label.c_str());
    textR(cl, a.fontMono, theme::kFontLabel, ImVec2(r1.x - 38, r0.y + 9), P.textDim, r.info.c_str());
    if (r.score >= 0)
      for (int s = 0; s < 3; ++s)
        cl->AddCircleFilled(ImVec2(r1.x - 100 + s * 6.0f, (r0.y + r1.y) * 0.5f), 2, r.score > 0.55f + s * 0.12f ? accent() : P.surface600, 8);
    if (button("##lplay", ImVec2(r1.x - 30, r0.y + 3), ImVec2(26, rowH - 10), "", false, i18n::N_("Listen to this loop"), static_cast<int>(ui::Icon::Play))) {
      const std::string f = renderItem(a, t, e, r.item, r.label);
      if (!f.empty()) a.previewSample(f);
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + ((rows.size() + 1) / 2) * rowH));
  ImGui::Dummy(ImVec2(1, 1));
  ImGui::EndChild();
}

int loopRows(const Ed& e) { return 1 + static_cast<int>(e.m.grid.loops.size() + e.m.grid.sections.size()); }

// ------------------------------------------------------------------ takes
void takesCard(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y = p0.y;
  if (e.takes.empty()) {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 4), P.textDim, i18n::tr("No takes yet. Record take records a new one into this instance."));
    return;
  }
  for (size_t i = 0; i < e.takes.size(); ++i) {
    const Take& tk = e.takes[i];
    ImGui::PushID(static_cast<int>(i));
    const bool cur = tk.path == e.path;
    char lb[160];
    std::snprintf(lb, sizeof lb, "%s %zu  \xC2\xB7  %s%s", i18n::tr("Take"), i + 1, fmtTime(static_cast<double>(tk.frames) / std::max(1.0, tk.rate)).c_str(),
                  cur ? i18n::tr("  \xC2\xB7  in use") : "");
    dl->AddText(cur ? a.fontBold : a.fontRegular, theme::kFontCaption, ImVec2(p0.x, y + 6), cur ? P.text : P.textDim, lb);
    if (button("##tkkeep", ImVec2(p0.x + w - 220, y), ImVec2(104, 24), i18n::N_("Keep the chop"), false,
               i18n::N_("Use this take with the regions and the sequence as they are")) &&
        !cur)
      useTake(a, t, tk.path, true, tk.frames, tk.rate);
    if (button("##tkfresh", ImVec2(p0.x + w - 108, y), ImVec2(108, 24), i18n::N_("Analyse fresh"), false,
               i18n::N_("Use this take and analyse it from the start (a new grid and chop)")))
      useTake(a, t, tk.path, false, tk.frames, tk.rate);
    ImGui::PopID();
    y += 30;
  }
}

const char* cardTitle(int card) {
  switch (card) {
    case CardRegion: return i18n::tr("Region");
    case CardStep: return i18n::tr("Step");
    case CardSound: return i18n::tr("Sound");
    case CardGrid: return i18n::tr("Tempo and grid");
    case CardLoops: return i18n::tr("Loops to drag out");
    case CardChop: return i18n::tr("Chop");
    case CardSteps: return i18n::tr("Steps");
    case CardTakes: return i18n::tr("Takes");
    default: return "";
  }
}

}  // namespace

void cardBody(App& a, const PluginTarget& t, Ed& e, int card, ImVec2 c0, ImVec2 c1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cw = c1.x - c0.x;
  switch (card) {
    case CardRegion: regionCard(a, t, e, c0, cw); break;
    case CardStep: stepCard(a, t, e, c0, cw); break;
    case CardSound: soundCard(a, t, e, c0, cw); break;
    case CardGrid: drawGridTools(a, t, e, c0, c1); break;
    case CardLoops: loopsCard(a, t, e, c0, c1); break;
    case CardChop: {
      drawChopSettings(a, t, e, c0, cw);
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(c0.x, c0.y + 40), P.textDim,
                  i18n::tr("Cut tool: double-click the waveform to add a cut, drag a cut to move it, right-click a cut to remove it."));
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(c0.x, c0.y + 60), P.textDim,
                  i18n::tr("Grid chops stay on the bars; cuts never make a region shorter than half a step (tiny pieces join their neighbour)."));
      break;
    }
    case CardSteps: stepsCard(a, t, e, c0, cw); break;
    case CardTakes: takesCard(a, t, e, c0, cw); break;
    default: break;
  }
}

ImVec2 cardSize(const Ed& e, int card, float maxW) {
  ImVec2 s(560, 196);
  switch (card) {
    case CardRegion:
    case CardStep: s = ImVec2(600, 196); break;
    case CardSound: s = ImVec2(std::max(700.0f, 13 * (pui::knobCellWidth(pui::KnobSmall) + 4) + 28), 104 + pui::knobCellHeight(pui::KnobSmall) + 40); break;
    case CardGrid: s = ImVec2(830, 138); break;
    case CardLoops: s = ImVec2(620, std::clamp(56.0f + static_cast<float>((loopRows(e) + 1) / 2) * 34.0f, 100.0f, 330.0f)); break;
    case CardChop: s = ImVec2(800, 140); break;
    case CardSteps: s = ImVec2(720, 124); break;
    case CardTakes: s = ImVec2(420, 56 + 30 * std::max<float>(1, static_cast<float>(e.takes.size()))); break;
    default: break;
  }
  s.x = std::min(s.x, std::max(320.0f, maxW));
  return s;
}

void drawCard(App& a, const PluginTarget& t, Ed& e, const LiveView& lv) {
  (void)lv;
  if (e.card == CardNone || !e.hasModel) {
    e.card = CardNone;
    e.card0 = e.card1 = ImVec2(0, 0);
    e.cardTouched = false;
    return;
  }
  const auto& P = pal();
  const float roomW = e.win1.x - e.win0.x - 16;
  const ImVec2 sz = cardSize(e, e.card, roomW);
  // where it opens: under the point it was asked for (above when there is no room), inside the room
  float x = std::round(e.cardAnchor.x - sz.x * 0.5f);
  x = std::clamp(x, e.win0.x + 8, std::max(e.win0.x + 8, e.win1.x - 8 - sz.x));
  float y = e.cardBelow ? e.cardAnchor.y + 10 : e.cardAnchor.y - 10 - sz.y;
  const ImGuiViewport* vp = ImGui::GetWindowViewport();
  const float vy0 = vp->WorkPos.y + 4, vy1 = vp->WorkPos.y + vp->WorkSize.y - 4;
  if (e.cardBelow && y + sz.y > vy1) y = e.cardAnchor.y - 10 - sz.y;
  if (!e.cardBelow && y < vy0) y = e.cardAnchor.y + 10;
  y = std::clamp(y, vy0, std::max(vy0, vy1 - sz.y));
  ImGui::SetNextWindowPos(ImVec2(x, std::round(y)));
  ImGui::SetNextWindowSize(sz);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGuiWindow* const editorWin = ImGui::GetCurrentWindow();
  char name[64];
  std::snprintf(name, sizeof name, "##seltcard%llx", static_cast<unsigned long long>(t.key()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  const bool shown = ImGui::Begin(name, nullptr, fl);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  ImGui::BringWindowToDisplayFront(win);
  // The card belongs to the editor: drawn reduced or magnified with it (the editor size, K-147),
  // the pointer mapped the same way, and the editor's focus covers it (its keys keep working).
  // ImGui sets a top-level window's parent each frame in Begin(); this one names the editor.
  win->ParentWindow = editorWin;
  const ImVec2 p0 = ImGui::GetWindowPos(), p1(p0.x + sz.x, p0.y + sz.y);
  e.card0 = p0, e.card1 = p1;
  bool close = false;
  if (shown) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the card: a raised panel with a small arrow towards where it was opened
    skin::raisedFace(dl, p0, p1, P.surface800, theme::kRadiusLg);
    dl->AddRect(p0, p1, theme::withAlpha(accent(), 0.55f), theme::kRadiusLg, 0, 1.0f);
    // header: a badge (regions), the title, facts, play and the WAV handle (regions), close
    const float hx = p0.x + 14, hy = p0.y + 10;
    float tx = hx;
    const bool reg = e.card == CardRegion || e.card == CardStep;
    if (reg && !e.m.slices.empty()) {
      const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
      const ImU32 c = regionColor(k);
      dl->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + 22, hy + 22), c, theme::kRadius);
      textC(dl, a.fontBold, 13, ImVec2(hx + 11, hy + 11), theme::contrastText(c), regionName(k).c_str());
      tx += 30;
      char tb[48];
      if (e.card == CardStep) std::snprintf(tb, sizeof tb, "%s %d", i18n::tr("Step"), e.selStep + 1);
      else std::snprintf(tb, sizeof tb, "%s %s", i18n::tr("Region"), regionName(k).c_str());
      dl->AddText(a.fontBold, theme::kFontBody, ImVec2(tx, hy + 3), P.text, tb);
      tx += a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, tb).x + 10;
      const Slice& s = e.m.slices[static_cast<size_t>(k)];
      const int base = baseNote(a, t);
      char fb[128];
      const int step = e.card == CardStep ? e.selStep : firstStepOf(e.m, k);
      std::snprintf(fb, sizeof fb, "%.3f s  \xC2\xB7  %s %s%s", static_cast<double>(s.end - s.start) / std::max(1.0, e.m.srcRate), i18n::tr("key"),
                    base + k <= 127 ? noteName(base + k).c_str() : "-", step >= 0 ? "" : i18n::tr("  \xC2\xB7  not in the sequence"));
      if (step >= 0) {
        const size_t l = std::strlen(fb);
        std::snprintf(fb + l, sizeof fb - l, "  \xC2\xB7  %s %d", i18n::tr("step"), step + 1);
      }
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(tx, hy + 6), P.textDim, fb);
      // play the region, drag it out as audio
      const float rx = p1.x - 14 - 26 - 8 - 64;
      if (button("##cplay", ImVec2(rx, hy - 1), ImVec2(30, 24), "", false, i18n::N_("Play the region"), static_cast<int>(ui::Icon::Play)))
        auditionStart(a, t, e, k);
      if (ImGui::IsItemDeactivated()) auditionStop(a, t, e);
      char lab[32];
      std::snprintf(lab, sizeof lab, "region %s", regionName(k).c_str());
      dragChip(a, t, e, "##cwav", ImVec2(rx + 34, hy - 1), 24, "WAV", "slice:" + std::to_string(k), lab,
               i18n::N_("Drag the region out as audio: into the playlist, the channel rack or the browser"));
    } else {
      dl->AddText(a.fontBold, theme::kFontBody, ImVec2(tx, hy + 3), P.text, cardTitle(e.card));
    }
    ImGui::SetCursorScreenPos(ImVec2(p1.x - 14 - 22, hy));
    if (ui::IconButton("##cclose", ui::Icon::Close, false, 22, i18n::tr("Close (Esc)"))) close = true;
    noteBtn(e, "cardclose");
    const ImVec2 c0(p0.x + 14, p0.y + 44);
    cardBody(a, t, e, e.card, c0, ImVec2(p1.x - 14, p1.y - 10));
  }
  const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                    ImGui::IsMouseHoveringRect(p0, p1, false);
  ImGui::End();
  // closes with Esc, the close button or a click elsewhere (not the click that opened it)
  const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (!e.cardTouched && !over && !popupOpen && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) close = true;
  if (!popupOpen && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    keys::claimEscape();
    close = true;
  }
  if (close) closeCard(e);
  e.cardTouched = false;
}

}  // namespace gl::packs::seltmann::sui
