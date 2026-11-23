// Gridline — Shaper editor (drawn envelopes + filter), 0.5 rack-panel style.
#include <map>
#include <memory>
#include "../core/I18n.h"
#include "../engine/ShaperSteps.h"
#include "DrawSmoothUi.h"
#include "PluginEditorsCommon.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

void knobs(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, const char*>> ks, float width, int style = pui::KnobSmall) {
  float c = width / static_cast<float>(ks.size());
  bool first = true;
  for (auto [k, l] : ks) {
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    pui::knob(a, t, k, l, style, c);
  }
}

// ======================================================================= Shaper (drawn envelopes + filter)
// K-257 phase 2: lanes of up to 64 steps with their own step size and swing, per-step chance and
// repeats, 8 patterns and a chain (engine/ShaperSteps.h holds the step bank in the state text).
struct ShaperUi {
  int lane = 0;  // 0 volume, 1 cutoff, 2 pan
  int mode = 0;  // what the lane edits: 0 level, 1 chance, 2 repeats
  bool haveClip = false;
  float clipVal[shsteps::kLanes][shsteps::kSteps] = {};
  uint8_t clipChance[shsteps::kLanes][shsteps::kSteps] = {}, clipHits[shsteps::kLanes][shsteps::kSteps] = {};
};
std::map<uint64_t, ShaperUi> shaperUi;

// The parsed step bank of one plugin, refreshed when its text changes.
struct ShaperCache {
  std::string data;
  std::unique_ptr<shsteps::Bank> bank;
};
std::map<uint64_t, ShaperCache> shaperCache;
const shsteps::Bank& shaperBank(uint64_t key, const std::string& data) {
  ShaperCache& c = shaperCache[key];
  if (!c.bank || c.data != data) {
    if (!c.bank) c.bank = std::make_unique<shsteps::Bank>();
    shsteps::parse(data.c_str(), *c.bank);
    c.data = data;
  }
  return *c.bank;
}

// One undo step (drags fold by mergeKey): pattern 1's first 16 levels go to the parameters, the rest to the text.
void commitShaper(App& a, const PluginTarget& t, const PluginState& st, const shsteps::Bank& b, const char* what, uint64_t mergeKey = 0) {
  PluginState ns = st;
  for (int l = 0; l < shsteps::kLanes; ++l)
    for (int k = 0; k < kShaperPoints; ++k) {
      const size_t i = static_cast<size_t>(shsteps::laneBase(l) + k);
      if (i < ns.params.size()) ns.params[i] = b.val[0][l][k];
    }
  ns.data = shsteps::write(b);
  if (ns.params != st.params || ns.data != st.data) setPluginState(a, t, ns, what, mergeKey);
}

// A working copy of the bank with pattern 1's first 16 levels taken from the parameters.
std::unique_ptr<shsteps::Bank> shaperEdit(const PluginState& st, const shsteps::Bank& b) {
  auto e = std::make_unique<shsteps::Bank>(b);
  for (int l = 0; l < shsteps::kLanes; ++l)
    for (int k = 0; k < kShaperPoints; ++k) e->val[0][l][k] = shsteps::level(st, b, 0, l, k);
  return e;
}

// The view shows 16 steps, or the lane's length rounded up to whole groups of 16 (at most 64).
int shaperCols(int steps) { return std::clamp((steps + kShaperPoints - 1) / kShaperPoints * kShaperPoints, kShaperPoints, shsteps::kSteps); }

void stepLane(App& a, const PluginTarget& t, const PluginState& st, const shsteps::Bank& bank, const ShaperUi& ui, int pat, ImVec2 sz, float playPh,
              int steps) {
  const auto& P = pal();
  const int li = ui.lane;
  const bool bipolar = li == 2 && ui.mode == 0;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(shsteps::laneBase(li) + 200 * ui.mode);
  ImGui::InvisibleButton("##steps", sz);
  pui::displayFrame(dl, p, ImVec2(p.x + sz.x, p.y + sz.y));
  const int cols = shaperCols(steps);
  float cw = sz.x / static_cast<float>(cols);
  float mid = bipolar ? p.y + sz.y / 2 : p.y + sz.y;
  double tension = val(st, ShTension);
  auto lvl = [&](int k) { return pat == 0 && k < kShaperPoints ? val(st, shsteps::laneBase(li) + k, shsteps::laneDefault(li)) : bank.val[pat][li][k]; };
  auto shown = [&](int k) -> float {  // the value the mode edits, 0..1 (pan levels -1..1)
    if (ui.mode == 1) return bank.chance[pat][li][k] / 100.f;
    if (ui.mode == 2) return (bank.hits[pat][li][k] - 1) / 3.f;
    return lvl(k);
  };
  for (int k = 0; k < cols; ++k) {
    float v = shown(k);
    float x0 = p.x + k * cw;
    float y = bipolar ? p.y + sz.y / 2 - v * (sz.y / 2 - 3) : p.y + sz.y - v * (sz.y - 4);
    const float pad = cw > 8 ? 1.5f : 0.75f;
    const int chance = bank.chance[pat][li][k], hits = bank.hits[pat][li][k];
    float alpha = 0.25f + 0.35f * std::fabs(v);
    if (ui.mode == 0 && chance < 100) alpha *= 0.45f + 0.55f * chance / 100.f;  // chance: a fainter step
    if (ui.mode == 2) y = std::min(y, p.y + sz.y - 4);                         // one hit still shows a sliver
    dl->AddRectFilled(ImVec2(x0 + pad, std::min(y, mid)), ImVec2(x0 + cw - pad, std::max(y, mid)), theme::withAlpha(P.text, alpha), 2);
    if (ui.mode == 0 && hits > 1)  // repeats: the step split into its hits
      for (int h = 1; h < hits; ++h) {
        const float xh = x0 + cw * h / hits;
        dl->AddLine(ImVec2(xh, std::min(y, mid)), ImVec2(xh, std::max(y, mid)), theme::withAlpha(P.surface950, 0.9f), 1.5f);
      }
    if (ui.mode == 0 && chance < 100)  // chance: a dot over the step
      dl->AddCircleFilled(ImVec2(x0 + cw / 2, p.y + 5), std::min(2.2f, cw * 0.25f), theme::withAlpha(P.accent, 0.85f));
    if (ui.mode != 0 && cw >= 14 && (ui.mode == 1 ? chance < 100 : hits > 1)) {
      char b[8];
      std::snprintf(b, sizeof b, "%d", ui.mode == 1 ? chance : hits);
      const ImVec2 ts = ImGui::CalcTextSize(b);
      if (ts.x < cw - 2) dl->AddText(ImVec2(x0 + (cw - ts.x) / 2, std::max(p.y + 2, y - ts.y - 1)), P.textDim, b);
    }
    if (k % 4 == 0) dl->AddLine(ImVec2(x0, p.y), ImVec2(x0, p.y + sz.y), k % kShaperPoints == 0 ? P.gridBar : theme::withAlpha(P.gridBar, 0.5f));
  }
  if (steps < cols) {  // K-257: steps past the lane's own length are not played
    const float xe = p.x + steps * cw;
    dl->AddRectFilled(ImVec2(xe, p.y + 1), ImVec2(p.x + sz.x - 1, p.y + sz.y - 1), theme::withAlpha(P.surface950, 0.8f), 2);
    dl->AddLine(ImVec2(xe, p.y), ImVec2(xe, p.y + sz.y), P.accent, 1.5f);
  }
  if (ui.mode == 0) {  // the curve as played (tension interpolates between steps; repeats pulse)
    constexpr int np = 320;
    ImVec2 pts[np + 1];
    for (int i = 0; i <= np; ++i) {
      double f = static_cast<double>(i) / np * cols;
      if (f >= steps) f = std::fmod(f, static_cast<double>(steps));  // K-257: the lane repeats its own steps
      int k = std::min(steps - 1, static_cast<int>(f));
      double a0 = lvl(k), v = a0, fr = f - k;
      const int hits = bank.hits[pat][li][k];
      if (hits > 1) {
        const double sub = fr * hits;
        v = sub - std::floor(sub) < 0.5 ? a0 : shsteps::laneRest(li);
      } else if (std::fabs(tension) > 1e-4) {
        double b = lvl((k + 1) % steps);
        v = a0 + (b - a0) * std::pow(fr, std::pow(2.0, -tension * 3.0));
      }
      float y = bipolar ? p.y + sz.y / 2 - static_cast<float>(v) * (sz.y / 2 - 3) : p.y + sz.y - static_cast<float>(v) * (sz.y - 4);
      pts[i] = ImVec2(p.x + sz.x * i / np, y);
    }
    dl->AddPolyline(pts, np + 1, P.paper, 0, 1.6f);
  }
  if (playPh >= 0) dl->AddLine(ImVec2(p.x + playPh * sz.x, p.y), ImVec2(p.x + playPh * sz.x, p.y + sz.y), P.accent, 1.5f);
  {  // J-99: fast strokes fill every step between samples; steps stay raw (each is a deliberate level).
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float v = bipolar ? std::clamp(1 - (m.y - p.y) / sz.y * 2, -1.0f, 1.0f) : std::clamp(1 - (m.y - p.y) / sz.y, 0.0f, 1.0f);
    float cur[shsteps::kSteps];
    for (int k = 0; k < cols; ++k) cur[k] = shown(k);
    drawui::Spec spec;
    spec.surface = "steps";
    spec.raw = true;
    spec.lo = bipolar ? -1.0f : 0.0f;
    if (const std::vector<float>* res = drawui::stroke(a, ImGui::GetItemID(), ImGui::IsItemActive(), (m.x - p.x) / sz.x, v, cur, cols, spec)) {
      auto e = shaperEdit(st, bank);
      for (int k = 0; k < cols; ++k) {
        const float r = (*res)[static_cast<size_t>(k)];
        if (ui.mode == 1) e->chance[pat][li][k] = static_cast<uint8_t>(std::clamp(std::lround(r * 100.0), 0L, 100L));
        else if (ui.mode == 2) e->hits[pat][li][k] = static_cast<uint8_t>(std::clamp(1L + std::lround(r * 3.0), 1L, 4L));
        else e->val[pat][li][k] = r;
      }
      static const char* what[3] = {"draw envelope", "draw step chance", "draw step repeats"};
      commitShaper(a, t, st, *e, what[ui.mode],
                   0x5348000000000000ull ^ t.key() ^ static_cast<uint64_t>(shsteps::laneBase(li) + 97 * ui.mode + 1000 * pat));
    }
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered())
    a.setHint(ui.mode == 1   ? i18n::tr("Draw each step's chance to play: drag across the steps")
              : ui.mode == 2 ? i18n::tr("Draw each step's repeats (1 to 4 hits): drag across the steps")
                             : i18n::tr("Draw: drag across the steps"));
  ImGui::PopID();
}

// The pattern rows: the eight patterns (the one shown, and played without the chain or notes),
// copy / paste / clear, and the chain.
void shaperPatterns(App& a, const PluginTarget& t, const PluginState& st, const shsteps::Bank& bank, ShaperUi& ui, int pat, int chainAt, float width) {
  const auto& P = pal();
  for (int k = 0; k < shsteps::kPatterns; ++k) {
    if (k) ImGui::SameLine(0, 2);
    ImGui::PushID(k);
    char b[4];
    std::snprintf(b, sizeof b, "%d", k + 1);
    if (ui::TextButton(b, pat == k, 30)) pui::setParamValue(a, t, ShPattern, k);
    ImGui::PopID();
  }
  ImGui::SameLine(0, 12);
  if (ui::TextButton("Copy", false, 64, "Copy this pattern's steps (every lane)")) {
    auto e = shaperEdit(st, bank);
    for (int l = 0; l < shsteps::kLanes; ++l)
      for (int k = 0; k < shsteps::kSteps; ++k)
        ui.clipVal[l][k] = e->val[pat][l][k], ui.clipChance[l][k] = e->chance[pat][l][k], ui.clipHits[l][k] = e->hits[pat][l][k];
    ui.haveClip = true;
  }
  ImGui::SameLine(0, 2);
  ImGui::BeginDisabled(!ui.haveClip);
  if (ui::TextButton("Paste", false, 64, "Paste the copied steps into this pattern")) {
    auto e = shaperEdit(st, bank);
    for (int l = 0; l < shsteps::kLanes; ++l)
      for (int k = 0; k < shsteps::kSteps; ++k)
        e->val[pat][l][k] = ui.clipVal[l][k], e->chance[pat][l][k] = ui.clipChance[l][k], e->hits[pat][l][k] = ui.clipHits[l][k];
    commitShaper(a, t, st, *e, "paste pattern");
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 2);
  if (ui::TextButton("Clear", false, 64, "Reset this pattern's steps (every lane)")) {
    auto e = shaperEdit(st, bank);
    for (int l = 0; l < shsteps::kLanes; ++l)
      for (int k = 0; k < shsteps::kSteps; ++k) e->val[pat][l][k] = shsteps::laneDefault(l), e->chance[pat][l][k] = 100, e->hits[pat][l][k] = 1;
    commitShaper(a, t, st, *e, "clear pattern");
  }
  // The chain: up to 16 slots, each playing for one Length; click a slot to pick its pattern.
  const float x0 = ImGui::GetCursorPosX();
  pui::led(a, t, ShChain, i18n::tr("Chain"));
  ImGui::SameLine(0, 12);
  const float avail = width - (ImGui::GetCursorPosX() - x0);
  const float cell = std::floor(std::min(30.0f, avail / shsteps::kChain - 2));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool chainOn = bval(st, ShChain);
  for (int k = 0; k < shsteps::kChain; ++k) {
    if (k) ImGui::SameLine(0, 2);
    ImGui::PushID(100 + k);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = 22;  // the LED's height: one row with it
    ImGui::InvisibleButton("##slot", ImVec2(cell, h));
    const bool hov = ImGui::IsItemHovered();
    const int c = bank.chain[k];
    const ImVec2 p1(p0.x + cell, p0.y + h);
    const bool playing = chainOn && k == chainAt;
    dl->AddRectFilled(p0, p1, playing ? theme::withAlpha(P.accent, 0.35f) : hov ? P.surface700 : P.surface900, 3);
    dl->AddRect(p0, p1, c >= 0 ? theme::withAlpha(P.accent, chainOn ? 0.9f : 0.45f) : P.surface700, 3);
    char b[4];
    if (c >= 0) std::snprintf(b, sizeof b, "%d", c + 1);
    else std::snprintf(b, sizeof b, "-");
    const ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(p0.x + std::round((cell - ts.x) / 2), p0.y + std::round((h - ts.y) / 2)), c >= 0 ? P.text : P.textDim, b);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##chainslot");
    if (hov) {
      ui::Tooltip("Chain slot: the pattern it plays for one Length (click to pick, wheel to step)");
      const float wh = ImGui::GetIO().MouseWheel;
      if (wh != 0) {
        auto e = shaperEdit(st, bank);
        e->chain[k] = static_cast<int8_t>(std::clamp(c + (wh > 0 ? 1 : -1), -1, shsteps::kPatterns - 1));
        commitShaper(a, t, st, *e, "chain slot", 0x5343000000000000ull ^ t.key() ^ static_cast<uint64_t>(k));
      }
    }
    if (ImGui::BeginPopup("##chainslot")) {
      auto pick = [&](int v) {
        auto e = shaperEdit(st, bank);
        e->chain[k] = static_cast<int8_t>(v);
        commitShaper(a, t, st, *e, "chain slot");
      };
      if (ImGui::MenuItem(i18n::tr("Empty"), nullptr, c < 0)) pick(-1);
      for (int q = 0; q < shsteps::kPatterns; ++q) {
        const std::string l = std::string(i18n::tr("Pattern")) + " " + std::to_string(q + 1);
        if (ImGui::MenuItem(l.c_str(), nullptr, c == q)) pick(q);
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
}

void editorShaper(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ShaperUi& ui = shaperUi[t.key()];
  const shsteps::Bank& bank = shaperBank(t.key(), st.data);
  const int pat = std::clamp(ival(st, ShPattern), 0, shsteps::kPatterns - 1);
  float right = 178, main = width - right - 8;
  ImGui::BeginGroup();
  // Unit strip: IN | FILTER | DRIVE | OUT (the reference's per-unit row).
  float wIn = main * 0.21f, wF = main * 0.42f, wD = main * 0.16f, wO = main - wIn - wF - wD - 24;
  const float uh = 150;
  pui::beginPanel("IN", wIn, uh);
  knobs(a, t, {{ShInput, "Input"}, {ShBandFreq, "Split"}}, wIn - 20);  // K-257: the band split
  static const char* bands[3] = {"Full", "Lows", "Highs"};
  pui::segmented(a, t, ShBand, wIn - 22, bands);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("FILTER", wF, uh);
  static const char* ft[9] = {"Off", "LP", "BP", "HP", "Nch", "LSh", "Pk", "HSh", "AP"};
  pui::segmented(a, t, ShFilter, wF - 22, ft);
  float c = (wF - 20) / 5;
  pui::knob(a, t, ShCutMin, "Min", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShCutMax, "Max", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShReso, "Res / BW", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShShelfGain, "Gain", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, ShCutDepth, "Env amt", pui::KnobSmall, c);
  static const char* sl[3] = {"Single", "Dual", "Triple"};
  pui::segmented(a, t, ShSlope, wF - 22, sl);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("DRIVE", wD, uh);
  knobs(a, t, {{ShDrive, "Drive"}, {ShDriveMix, "Mix"}}, wD - 20);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("OUT", wO, uh);
  knobs(a, t, {{ShOutput, "Output"}, {ShMix, "Mix"}}, wO - 20);
  pui::endPanel();
  // Envelope area: target tabs + step editor.
  pui::beginPanel("ARTICULATOR", main);
  const char* tabs[3] = {"Volume", "Cutoff", "Pan"};
  // K-257: the row also holds the lane's Steps, so the length switch takes what is left
  const float rowW = ImGui::GetContentRegionAvail().x, stepsW = 56 + 6 + ImGui::CalcTextSize("Steps").x;
  const float lenW = std::clamp(rowW - 3 * 76 - 4 - 12 - 8 - 96 - 8 - stepsW - 4, 170.0f, 250.0f);
  // One centre line per row: the tabs are a frame tall, the switch and the stepper 22 px, so
  // those sit half the difference lower (their labels then share the tabs' baseline).
  float rowY = ImGui::GetCursorScreenPos().y, boxY = rowY + std::round((ImGui::GetFrameHeight() - 22) * 0.5f);
  auto at = [](float y) { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y)); };
  // A dial on the row's centre line, its name and value beside it on the line the stepper's value takes.
  auto rowDial = [&](int param, const char* label) {
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    pui::dial(a, t, param, 11);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto d = describeParam(a.doc.song(), t.pid(param));
    const auto v = getParam(a.doc.song(), t.pid(param));
    ImGui::PushFont(a.fontMono, 11.0f);
    const float vy = p.y + (22 - ui::TextSizeExact("0").y) * 0.5f;  // as the stepper centres its value
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 12.0f);
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 28, p.y + std::round((22 - ls.y) * 0.5f)), pal().textDim, label);
    ImGui::PopFont();
    if (d && v) {
      ImGui::PushFont(a.fontMono, 11.0f);
      dl->PushClipRect(ImVec2(p.x, p.y - 2), ImVec2(p.x + 100, p.y + 24), true);
      dl->AddText(ImVec2(p.x + 28 + ls.x + 6, vy), pal().text, d->format(*v).c_str());
      dl->PopClipRect();
      ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(96, 22));
    ImGui::EndGroup();
  };
  auto rowEnd = [&] {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                     std::max(ImGui::GetCursorScreenPos().y, rowY + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y)));
  };
  for (int k = 0; k < 3; ++k) {
    if (k) ImGui::SameLine(0, 2);
    ImGui::PushID(k);
    if (ui::TextButton(tabs[k], ui.lane == k, 76)) ui.lane = k;
    ImGui::PopID();
  }
  ImGui::SameLine(0, 12);
  at(boxY);
  static const char* len[5] = {"1/4", "1/2", "1 bar", "2 bars", "4 bars"};
  pui::segmented(a, t, ShLength, lenW, len);
  ImGui::SameLine(0, 8);
  at(boxY);
  rowDial(ShTension, "Curve");
  // K-257: the lane's own length in steps (lanes of different lengths run against each other)
  const int stepsP = ui.lane == 0 ? ShVolSteps : ui.lane == 1 ? ShCutSteps : ShPanSteps;
  const int rateP = ui.lane == 0 ? ShVolRate : ui.lane == 1 ? ShCutRate : ShPanRate;
  const int swingP = ui.lane == 0 ? ShVolSwing : ui.lane == 1 ? ShCutSwing : ShPanSwing;
  const int steps = std::clamp(ival(st, stepsP, kShaperPoints - 1) + 1, 1, kShaperMaxSteps);
  ImGui::SameLine(0, 8);
  at(rowY);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", "Steps");
  ImGui::SameLine(0, 6);
  at(boxY);
  pui::stepper(a, t, stepsP, 56, nullptr);
  rowEnd();
  // Phase 2: what the lane edits (level, chance, repeats), and the lane's step size and swing.
  rowY = ImGui::GetCursorScreenPos().y, boxY = rowY + std::round((ImGui::GetFrameHeight() - 22) * 0.5f);
  static const char* modes[3] = {"Level", "Chance", "Repeats"};
  static const char* modeHints[3] = {"Draw each step's level", "Draw each step's chance to play (an export repeats exactly)",
                                     "Draw each step's repeats: 1 to 4 hits in the step"};
  for (int k = 0; k < 3; ++k) {
    if (k) ImGui::SameLine(0, 2);
    ImGui::PushID(10 + k);
    if (ui::TextButton(modes[k], ui.mode == k, 76, modeHints[k])) ui.mode = k;
    ImGui::PopID();
  }
  ImGui::SameLine(0, 12);
  pui::combo(a, t, rateP, lenW, i18n::tr("Step"));
  ImGui::SameLine(0, 8);
  at(boxY);
  rowDial(swingP, "Swing");
  rowEnd();
  const int cols = shaperCols(steps);
  static const double beats[] = {1, 2, 4, 8, 16};
  const double lenB = beats[std::clamp(ival(st, ShLength, 2), 0, 4)];
  float playPh = -1;
  int chainAt = -1;
  if (a.engine.playing()) {
    const double beat = a.engine.position() / std::max(1, a.doc.song().ppq);
    const int rate = std::clamp(ival(st, rateP), 0, kShaperRates - 1);
    const double stepB = rate == 0 ? lenB / kShaperPoints : kShaperRateBeats[rate];
    playPh = static_cast<float>(std::fmod(beat / stepB, static_cast<double>(steps)) / cols);
    if (bank.chainLen > 0) chainAt = static_cast<int>(static_cast<int64_t>(std::floor(beat / lenB)) % bank.chainLen);
  }
  stepLane(a, t, st, bank, ui, pat, ImVec2(main - 22, 150), playPh, steps);
  pui::endPanel();
  pui::beginPanel("PATTERNS", main);
  shaperPatterns(a, t, st, bank, ui, pat, chainAt, main - 22);
  pui::endPanel();
  ImGui::EndGroup();
  const float leftH = ImGui::GetItemRectSize().y;
  ImGui::SameLine(0, 8);
  pui::beginPanel("MODULATION", right, leftH);
  pui::caption("FOLLOWER");
  knobs(a, t, {{ShFollow, "Amount"}, {ShFollowAtt, "Attack"}}, right - 20);
  pui::knob(a, t, ShFollowRel, "Release", pui::KnobSmall, (right - 20) / 2);
  ImGui::Dummy(ImVec2(0, 4));
  pui::caption("LFO TO CUTOFF");
  knobs(a, t, {{ShLfoAmt, "Amount"}, {ShLfoRate, "Rate"}}, right - 20);
  static const char* lsh[4] = {"Sin", "Tri", "Saw", "Sq"};
  pui::segmented(a, t, ShLfoShape, right - 22, lsh);
  pui::knob(a, t, ShSmooth, "Smooth", pui::KnobSmall, (right - 20) / 2);
  // Phase 2: notes that pick the pattern (the first note picks pattern 1, the seven above it the others).
  ImGui::Dummy(ImVec2(0, 4));
  pui::caption(i18n::tr("PATTERN NOTES"));
  pui::combo(a, t, ShNoteSrc, right - 22);
  const float fromW = ImGui::CalcTextSize(i18n::tr("From")).x, portW = ImGui::CalcTextSize(i18n::tr("Port")).x, lblW = std::max(fromW, portW);
  pui::stepper(a, t, ShNoteBase, right - 22 - 6 - lblW, i18n::tr("From"));
  if (ival(st, ShNoteSrc) == 1) pui::stepper(a, t, ShNotePort, right - 22 - 6 - lblW, i18n::tr("Port"));
  pui::endPanel();
}

}  // namespace

bool drawMiscEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type != "gl.shaper") return false;
  editorShaper(a, t, width);
  return true;
}

}  // namespace gl
