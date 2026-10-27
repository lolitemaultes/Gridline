// Gridline — "plasma" pack: the ARP panel (lower area, x 165..1190) and the
// CLIP editor (page + lower area): sections of settings, a pattern / piano
// roll editor, automation lanes and the 12-slot bank row.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../core/I18n.h"
#include "plasma_ui.h"
#include "../../app/Widgets.h"

namespace gl::packs::plasma::ui {

namespace {

constexpr float kAX = 165, kAY = kLowerY + 30;  // arp body (under the tabs)
constexpr float kSlotY = 562;

bool sameArp(const ArpSlot& p, const ArpSlot& q) {
  if (p.steps.size() != q.steps.size()) return false;
  for (size_t i = 0; i < p.steps.size(); ++i) {
    const auto &x = p.steps[i], &y = q.steps[i];
    if (x.time != y.time || x.len != y.len || x.row != y.row || x.vel != y.vel || x.accent != y.accent || x.strum != y.strum) return false;
  }
  return p.shape == q.shape && p.rate == q.rate && p.hz == q.hz && p.bpm == q.bpm && p.trip == q.trip && p.dot == q.dot && p.shift == q.shift &&
         p.range == q.range && p.rangeShape == q.rangeShape && p.latch == q.latch && p.thru == q.thru && p.offset == q.offset && p.repeats == q.repeats &&
         p.gate == q.gate && p.chance == q.chance && p.chancePre == q.chancePre && p.retrigLaunch == q.retrigLaunch && p.retrigRate == q.retrigRate &&
         p.retrigNote == q.retrigNote && p.retrigFirst == q.retrigFirst && p.retrigDiv == q.retrigDiv && p.velo == q.velo && p.veloRetrig == q.veloRetrig &&
         p.veloDecay == q.veloDecay && p.veloTarget == q.veloTarget && p.patternBeats == q.patternBeats && p.stepMode == q.stepMode && p.wrap == q.wrap &&
         p.wrapPitch == q.wrapPitch && p.rangeWrap == q.rangeWrap && p.patTime == q.patTime;
}

void sectionTitle(const Ctx& c, float x0, float x1, float y, const char* t) { caption(c, x0, x1, y, t); }

// The value a data knob shows: range, default (ArpSlot's), unit and mapping.
ParamDesc dataDesc(const char* name, double lo, double hi, double def, const char* unit = "", ParamScale scale = ParamScale::Linear) {
  ParamDesc d;
  d.name = name, d.unit = unit, d.min = lo, d.max = hi, d.def = def, d.scale = scale;
  return d;
}

// A knob bound to a float of the model (described by d); commits one undo step per drag.
bool dataKnob(Ctx& c, const char* id, float& v, const ParamDesc& d, float cx, float cy, const char* label, const std::string& valueText, int what) {
  float n = static_cast<float>(d.toNormalized(v));
  if (knobFree(c, id, n, cx, cy, 13.5f, label, false, valueText.c_str(), d)) {
    v = static_cast<float>(d.fromNormalized(std::clamp(n, 0.f, 1.f)));
    commit(c, label, what);
    return true;
  }
  return false;
}
bool dataStepper(Ctx& c, const char* id, int& v, int lo, int hi, float x, float y, float w, float h, const char* shown, const char* label) {
  int t = v;
  if (stepperFree(c, id, t, lo, hi, x, y, w, h, shown)) {
    v = t;
    commit(c, label);
    return true;
  }
  return false;
}
bool dataCheck(Ctx& c, const char* id, bool& v, float x, float y, const char* label, float px = 8.5f) {
  ImGui::PushID(id);
  const float w = 13 + textW(c, label, px, true);
  ImGui::SetCursorScreenPos(c.p(x, y - 1));
  const bool click = ImGui::InvisibleButton("##c", c.F.sz(w, 11));
  if (click) {
    v = !v;
    commit(c, label);
  }
  const ImVec2 a = c.p(x, y + 0.5f), b = c.p(x + 8.5f, y + 9);
  if (v) skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.2f), c.P.accent, c.s(2));
  else c.dl->AddRectFilled(a, b, c.P.well, c.s(2)), c.dl->AddRect(a, b, c.P.lineHi, c.s(2));
  text(c, x + 13, y, label, v ? c.P.text : c.P.textDim, px, true);
  ImGui::PopID();
  return click;
}
bool dataToggle(Ctx& c, const char* id, bool& v, float x, float y, float w, float h, const char* label, float px = 8.f) {
  if (button(c, id, x, y, w, h, label, v, px)) {
    v = !v;
    commit(c, label);
    return true;
  }
  return false;
}
// Division drop-down bound to an int of the model.
void divisionDrop(Ctx& c, const char* id, int& div, float x, float y, float w, float h) {
  const auto& dn = divisionNames();
  const std::string shown = dn[static_cast<size_t>(std::clamp(div, 0, static_cast<int>(dn.size()) - 1))];
  int step = 0;
  dropField(c, id, x, y, w, h, shown.c_str(),
            [&] {
              for (int i = 0; i < static_cast<int>(dn.size()); ++i)
                if (ImGui::Selectable(dn[static_cast<size_t>(i)].c_str(), i == div)) {
                  div = i;
                  commit(c, "Division");
                }
            },
            false, &step);
}

// ------------------------------------------------------------------ slot row (arp + clip)
void slotRow(Ctx& c, bool clip) {
  const float x0 = kAX, x1 = 1038, y = kSlotY;
  const float w = (x1 - x0) / 12;
  c.dl->AddRectFilled(c.p(x0, y), c.p(kW - kGutter - 1, kStripY - kGutter - 1), c.P.panelLo, c.s(2.0f), ImDrawFlags_RoundCornersBottom);
  hline(c, x0, kW - kGutter - 1, y + 0.5f, c.P.frame);
  int& edit = clip ? c.u.clipSlot : c.u.arpSlot;
  int& active = clip ? c.m.clipActive : c.m.arpActive;
  for (int i = 0; i < 12; ++i) {
    const float sx = x0 + i * w;
    ImGui::PushID(i + (clip ? 8100 : 8000));
    const bool ed = edit == i, act = active == i;
    const bool live = c.hasLive && act && ((clip && val(c, G_ClipOn) >= 0.5) || (!clip && val(c, G_ArpOn) >= 0.5));
    // header (number) + body: select for editing
    ImGui::SetCursorScreenPos(c.p(sx, y + 1));
    if (ImGui::InvisibleButton("##sl", c.F.sz(w - 1, 62))) edit = i;
    const bool hv = ImGui::IsItemHovered();
    panelRect(c, sx + 0.5f, y + 1, w - 1, 22, ed ? c.P.headOn : c.P.head);
    if (ed) c.dl->AddRectFilled(c.p(sx + 0.5f, y + 1), c.p(sx + w - 0.5f, y + 3), c.P.accent);
    char num[4];
    std::snprintf(num, sizeof num, "%d", i + 1);
    text(c, sx + 6, y + 7, num, ed ? c.P.text : c.P.textDim, 8.5f, true);
    panelRect(c, sx + 0.5f, y + 23, w - 1, 40, hv ? c.P.panelHi : c.P.panel);
    std::string l1, l2;
    if (clip) {
      const ClipSlot& cs = c.m.clip[static_cast<size_t>(i)];
      l1 = cs.name.empty() ? (cs.notes.empty() ? std::string("\xE2\x80\x94") : std::to_string(cs.notes.size()) + " notes") : cs.name;
      char b[24];
      std::snprintf(b, sizeof b, "%g beats", static_cast<double>(cs.lengthBeats));
      l2 = b;
    } else {
      const ArpSlot& as = c.m.arp[static_cast<size_t>(i)];
      const auto& sn = arpShapeNames();
      l1 = as.name.empty() ? sn[static_cast<size_t>(std::clamp(as.shape, 0, static_cast<int>(sn.size()) - 1))] : as.name;
      const auto& dn = divisionNames();
      l2 = as.bpm ? dn[static_cast<size_t>(std::clamp(as.rate, 0, static_cast<int>(dn.size()) - 1))] : std::to_string(static_cast<int>(as.hz)) + " Hz";
    }
    textMid(c, sx + 2, sx + w - 2, y + 36, l1.c_str(), ed ? c.P.text : c.P.textDim, 8.5f, true);
    textMid(c, sx + 2, sx + w - 2, y + 51, l2.c_str(), c.P.textFaint, 8, true);
    // launch (makes it the playing slot)
    ImGui::SetCursorScreenPos(c.p(sx, y + 64));
    if (ImGui::InvisibleButton("##go", c.F.sz(w - 1, 24))) {
      active = i;
      commit(c, clip ? "Choose clip" : "Choose arp pattern");
    }
    const bool gh = ImGui::IsItemHovered();
    panelRect(c, sx + 0.5f, y + 64, w - 1, 24, act ? c.P.accentSoft : (gh ? c.P.panelHi : c.P.panelLo));
    const ImVec2 q = c.p(sx + w * 0.5f, y + 76);
    const float r = c.s(4.5f);
    ::gl::ui::drawIconFit(c.dl, ::gl::ui::Icon::Play, ImVec2(q.x - r * 0.7f, q.y - r), ImVec2(q.x + r, q.y + r),
                          act ? (live ? c.P.good : c.P.accent) : (gh ? c.P.text : c.P.textFaint));
    if (gh) c.a.setHint(std::string("Play this ") + (clip ? "clip" : "pattern") + " (the trigger octave on the keyboard also picks slots)");
    ImGui::PopID();
  }
  // MIDI out + trigger octave
  {
    const float x = x1 + 6;
    text(c, x, y + 40, "NOTE LATCH", c.P.textDim, 8, true);
    checkBox(c, G_NoteLatch, x + 124, y + 40, "", 8);
    text(c, x, y + 70, "MIDI OUT", c.P.textDim, 8, true);
    choiceBox(c, G_MidiOut, x + 54, y + 65, 84, 18);
  }
}

// ------------------------------------------------------------------ arp pattern editor (shape = Pattern)
void patternEditor(Ctx& c, ArpSlot& a, float x, float y, float w, float h) {
  displayWellPublic(c, x, y, w, h);
  constexpr int rows = 8;
  const float beats = std::max(0.25f, a.patternBeats);
  const int cols = std::max(1, static_cast<int>(std::lround(beats / 0.25f)));
  for (int i = 1; i < cols; ++i) vline(c, x + w * i / cols, y, y + h, (i % 4) ? c.P.grid : c.P.gridStrong);
  for (int r = 1; r < rows; ++r) hline(c, x, x + w, y + h * r / rows, c.P.grid);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##pat", c.F.sz(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const ImVec2 m = ImGui::GetIO().MousePos, p0 = c.p(x, y), p1 = c.p(x + w, y + h);
  const float fx = std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.f, 0.9999f), fy = std::clamp((m.y - p0.y) / (p1.y - p0.y), 0.f, 0.9999f);
  const int col = static_cast<int>(fx * cols), row = rows - 1 - static_cast<int>(fy * rows);
  int hit = -1;
  for (size_t i = 0; i < a.steps.size(); ++i) {
    const auto& st = a.steps[i];
    if (st.row == row && st.time <= col * 0.25f + 1e-3f && st.time + st.len > col * 0.25f + 1e-3f) hit = static_cast<int>(i);
  }
  if (ImGui::IsItemClicked(0)) {
    if (hit >= 0) a.steps.erase(a.steps.begin() + hit);
    else a.steps.push_back({col * 0.25f, 0.25f, row, 0.8f, false, 0});
    std::sort(a.steps.begin(), a.steps.end(), [](const ArpSlot::Step& p, const ArpSlot::Step& q) { return p.time < q.time || (p.time == q.time && p.row < q.row); });
    commit(c, "Edit arp pattern");
  }
  if (ImGui::IsItemClicked(1) && hit >= 0) {
    a.steps[static_cast<size_t>(hit)].accent = !a.steps[static_cast<size_t>(hit)].accent;
    commit(c, "Accent");
  }
  if (ImGui::IsItemHovered() && hit >= 0 && ImGui::GetIO().MouseWheel != 0) {
    auto& v = a.steps[static_cast<size_t>(hit)].vel;
    v = std::clamp(v + ImGui::GetIO().MouseWheel * 0.05f, 0.05f, 1.f);
    commit(c, "Step velocity", 8200);
  }
  for (const auto& st : a.steps) {
    const float sx0 = x + w * st.time / beats, sx1 = x + w * std::min(beats, st.time + st.len) / beats;
    const float sy = y + h * (rows - 1 - std::clamp(st.row, 0, rows - 1)) / rows;
    c.dl->AddRectFilled(c.p(sx0 + 1, sy + 2), c.p(sx1 - 1, sy + h / rows - 2), theme::withAlpha(st.accent ? c.P.warn : c.P.accent, 0.35f + 0.65f * st.vel),
                        c.s(2));
  }
  if (c.hasLive && c.live.arpStep >= 0 && !a.steps.empty()) {
    const auto& st = a.steps[static_cast<size_t>(c.live.arpStep % static_cast<int>(a.steps.size()))];
    vline(c, x + w * st.time / beats, y, y + h, c.P.mod);
  }
  if (ImGui::IsItemHovered())
    c.a.setHint("Pattern: rows are the held notes from the lowest (rows past them wrap) \xC2\xB7 click: add / remove \xC2\xB7 right-click: accent \xC2\xB7 wheel: velocity");
}

}  // namespace

// ================================================================== ARP panel
void arpPanel(Ctx& c) {
  const bool editAll = c.u.arpEditAll;
  ArpSlot& a = c.m.arp[static_cast<size_t>(std::clamp(c.u.arpSlot, 0, kArpSlots - 1))];
  // with EDIT ALL, the change made to the edited slot is copied to every slot after the frame
  const ArpSlot before = a;
  const float y0 = kAY + 4;
  // GLOBAL
  {
    const float x = kAX + 10, x1 = kAX + 160;
    sectionTitle(c, x, x1, y0, "GLOBAL");
    std::string nm = a.name.empty() ? std::string("Slot ") + std::to_string(c.u.arpSlot + 1) : a.name;
    dropField(c, "bank", x, y0 + 22, x1 - x, 18, nm.c_str(), [&] {
      static ArpSlot clip;
      static bool has = false;
      if (ImGui::MenuItem(i18n::tr("Copy slot"))) clip = a, has = true;
      if (ImGui::MenuItem(i18n::tr("Paste slot"), nullptr, false, has)) {
        a = clip;
        commit(c, "Paste arp slot");
      }
      if (ImGui::MenuItem(i18n::tr("Reset slot"))) {
        a = ArpSlot{};
        commit(c, "Reset arp slot");
      }
      ImGui::Separator();
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s", a.name.c_str());
      ImGui::SetNextItemWidth(c.s(140));
      if (ImGui::InputText(i18n::tr("Name"), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        a.name = buf;
        commit(c, "Rename arp slot");
      }
    });
    divisionDrop(c, "lq", c.m.arpLaunchQuant, x, y0 + 64, 80, 20);
    textC(c, x + 40, y0 + 90, "LAUNCH QUANT", c.P.textDim, 8, true);
    if (button(c, "editall", x + 96, y0 + 64, 54, 20, "EDIT ALL", editAll, 7.5f)) c.u.arpEditAll = !c.u.arpEditAll;
    if (ImGui::IsItemHovered()) c.a.setHint("Edit all twelve slots at once");
  }
  // PATTERN
  {
    const float x = kAX + 176, x1 = kAX + 376;
    sectionTitle(c, x, x1, y0, "PATTERN");
    const auto& sn = arpShapeNames();
    int step = 0;
    dropField(c, "shape", x, y0 + 22, x1 - x, 18, sn[static_cast<size_t>(std::clamp(a.shape, 0, static_cast<int>(sn.size()) - 1))].c_str(),
              [&] {
                for (int i = 0; i < static_cast<int>(sn.size()); ++i)
                  if (ImGui::Selectable(sn[static_cast<size_t>(i)].c_str(), i == a.shape)) {
                    a.shape = i;
                    commit(c, "Arp shape");
                  }
              },
              true, &step);
    if (step) {
      a.shape = (a.shape + step + ArpShapeCount) % ArpShapeCount;
      commit(c, "Arp shape");
    }
    // BPM / HZ, rate
    const float ry = y0 + 70;
    wellRect(c, x, ry - 14, x1 - x, 30, 4);
    ImGui::SetCursorScreenPos(c.p(x + 4, ry - 10));
    if (ImGui::InvisibleButton("##bpm", c.F.sz(44, 20))) {
      a.bpm = !a.bpm;
      commit(c, "Arp BPM");
    }
    text(c, x + 8, ry - 5, "BPM", a.bpm ? c.P.accent : c.P.textFaint, 8.5f, true);
    text(c, x + 32, ry - 5, "HZ", a.bpm ? c.P.textFaint : c.P.accent, 8.5f, true);
    if (a.bpm) {
      float r = static_cast<float>(a.rate);
      const auto& dn = divisionNames();
      const int nd = static_cast<int>(dn.size()) - 1;
      // the division list runs from long to short: the knob turns faster to the right
      float rn = static_cast<float>(nd) - r;
      ParamDesc rd = dataDesc("Rate", 0, nd, nd - ArpSlot{}.rate);
      rd.kind = ParamKind::Choice;
      for (int i = nd; i >= 0; --i) rd.choices.push_back(dn[static_cast<size_t>(i)]);
      if (dataKnob(c, "rate", rn, rd, x + 72, ry + 1, "RATE", dn[static_cast<size_t>(std::clamp(a.rate, 0, nd))], 8300)) {
        a.rate = std::clamp(nd - static_cast<int>(std::lround(rn)), 0, nd);
        commit(c, "Arp rate", 8300);
      }
      textC(c, x + 116, ry - 5, dn[static_cast<size_t>(std::clamp(a.rate, 0, nd))].c_str(), c.P.accent, 9.5f, true);
    } else {
      char b[24];
      std::snprintf(b, sizeof b, "%.2f Hz", static_cast<double>(a.hz));
      float hz = std::max(0.05f, a.hz);  // 0.05 .. 20 Hz on a log travel
      if (dataKnob(c, "hz", hz, dataDesc("Rate", 0.05, 20.0, ArpSlot{}.hz, "Hz", ParamScale::Log), x + 72, ry + 1, "RATE", b, 8301)) {
        a.hz = hz;
        commit(c, "Arp rate", 8301);
      }
      textC(c, x + 116, ry - 5, b, c.P.accent, 9.5f, true);
    }
    dataToggle(c, "trip", a.trip, x + 140, ry - 10, 28, 20, "T", 9);
    dataToggle(c, "dot", a.dot, x + 170, ry - 10, 28, 20, "D", 9);
    if (a.shape == ArpPattern) {
      if (button(c, "editpat", x, y0 + 98, x1 - x, 18, c.u.arpPattern ? "CLOSE PATTERN" : "EDIT PATTERN", c.u.arpPattern, 8)) c.u.arpPattern = !c.u.arpPattern;
    }
  }
  if (a.shape == ArpPattern && c.u.arpPattern) {
    // pattern editor replaces the right-hand sections
    const float x = kAX + 392, w = kW - x - 12;
    sectionTitle(c, x, x + w, y0, "STEPS");
    patternEditor(c, a, x, y0 + 18, w - 150, kSlotY - y0 - 30);
    const float px = x + w - 140;
    float pb = a.patternBeats;
    char b[24];
    std::snprintf(b, sizeof b, "%g beats", static_cast<double>(pb));
    if (dataKnob(c, "plen", pb, dataDesc("Pattern length", 0.25, 16.0, ArpSlot{}.patternBeats, "beats"), px + 24, y0 + 44, "LENGTH", b, 8310)) {
      a.patternBeats = std::round(pb * 4.f) / 4.f;
      commit(c, "Pattern length", 8310);
    }
    std::snprintf(b, sizeof b, "x%.2f", static_cast<double>(a.patTime));
    dataKnob(c, "ptime", a.patTime, dataDesc("Step length", 0.0625, 4.0, ArpSlot{}.patTime, "times"), px + 84, y0 + 44, "STEP LEN", b, 8311);
    const char* wraps[] = {"Octave", "Interval", "Repeat", "Skip"};
    dropField(c, "wrap", px, y0 + 82, 130, 18, (std::string("Wrap: ") + wraps[std::clamp(a.wrap, 0, 3)]).c_str(), [&] {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(wraps[i], i == a.wrap)) {
          a.wrap = i;
          commit(c, "Pattern wrap");
        }
    });
    if (a.wrap == 1) dataStepper(c, "wp", a.wrapPitch, -48, 48, px, y0 + 104, 130, 18, (std::to_string(a.wrapPitch) + " st").c_str(), "Wrap interval");
    const char* sm[] = {"Gate", "Tie", "Short", "Accents only"};
    dropField(c, "sm", px, y0 + 126, 130, 18, (std::string("Steps: ") + sm[std::clamp(a.stepMode, 0, 3)]).c_str(), [&] {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(sm[i], i == a.stepMode)) {
          a.stepMode = i;
          commit(c, "Pattern steps");
        }
    });
    if (button(c, "clr", px, y0 + 148, 130, 18, "CLEAR", false, 8)) {
      a.steps.clear();
      commit(c, "Clear pattern");
    }
  } else {
    // TRANSPOSE
    {
      const float x = kAX + 392, x1 = kAX + 512;
      sectionTitle(c, x, x1, y0, "TRANSPOSE");
      const char* rs[] = {"Up", "Down", "Up/Down", "Random"};
      int step = 0;
      dropField(c, "rs", x, y0 + 22, x1 - x, 18, rs[std::clamp(a.rangeShape, 0, 3)], [&] {
        for (int i = 0; i < 4; ++i)
          if (ImGui::Selectable(rs[i], i == a.rangeShape)) {
            a.rangeShape = i;
            commit(c, "Transpose shape");
          }
      }, true, &step);
      if (step) {
        a.rangeShape = (a.rangeShape + step + 4) % 4;
        commit(c, "Transpose shape");
      }
      dataStepper(c, "shift", a.shift, -24, 24, x, y0 + 64, 56, 20, (a.shift == 0 ? std::string("oct") : std::to_string(a.shift)).c_str(), "Arp shift");
      dataStepper(c, "range", a.range, 0, 8, x + 64, y0 + 64, 56, 20, std::to_string(a.range).c_str(), "Arp range");
      textC(c, x + 28, y0 + 90, "SHIFT", c.P.textDim, 8, true);
      textC(c, x + 92, y0 + 90, "RANGE", c.P.textDim, 8, true);
      dataStepper(c, "rw", a.rangeWrap, 0, 3, x, y0 + 108, 120, 18, (std::to_string(a.rangeWrap + 1) + (a.rangeWrap ? " cycles / step" : " cycle / step")).c_str(),
                  "Cycles per transpose step");
    }
    // PLAYBACK
    {
      const float x = kAX + 528, x1 = kAX + 778;
      sectionTitle(c, x, x1, y0, "PLAYBACK");
      dataToggle(c, "latch", a.latch, x, y0 + 26, 50, 22, "LATCH", 8);
      dataToggle(c, "thru", a.thru, x, y0 + 74, 50, 22, "THRU", 8);
      if (ImGui::IsItemHovered()) c.a.setHint("Thru: the held notes also play");
      char b[24];
      float off = static_cast<float>(a.offset);
      std::snprintf(b, sizeof b, "%d", a.offset);
      if (dataKnob(c, "off", off, dataDesc("Offset", -16, 16, ArpSlot{}.offset), x + 84, y0 + 72, "OFFSET", b, 8320)) a.offset = static_cast<int>(std::lround(off)), commit(c, "Arp offset", 8320);
      textC(c, x + 84, y0 + 30, b, c.P.accent, 9, true);
      float rep = static_cast<float>(a.repeats);
      std::snprintf(b, sizeof b, "%s", a.repeats <= 0 ? "INF" : std::to_string(a.repeats).c_str());
      if (dataKnob(c, "rep", rep, dataDesc("Repeats", 0, 16, ArpSlot{}.repeats), x + 134, y0 + 72, "REPEATS", b, 8321)) a.repeats = std::max(0, static_cast<int>(std::lround(rep))), commit(c, "Arp repeats", 8321);
      textC(c, x + 134, y0 + 30, b, c.P.accent, 9, true);
      std::snprintf(b, sizeof b, "%.0f%%", a.gate * 100.0);
      dataKnob(c, "gate", a.gate, dataDesc("Gate", 0.05, 2.0, ArpSlot{}.gate, "%"), x + 184, y0 + 72, "GATE", b, 8322);
      textC(c, x + 184, y0 + 30, b, c.P.accent, 9, true);
      std::snprintf(b, sizeof b, "%.0f%%", a.chance * 100.0);
      dataKnob(c, "chance", a.chance, dataDesc("Chance", 0, 1, ArpSlot{}.chance, "%"), x + 234, y0 + 72, "CHANCE", b, 8323);
      textC(c, x + 234, y0 + 30, b, c.P.accent, 9, true);
      dataCheck(c, "pre", a.chancePre, x + 190, y0 + 106, "CHANCE BEFORE REPEATS", 7.5f);
    }
    // RETRIGGER
    {
      const float x = kAX + 794, x1 = kAX + 904;
      sectionTitle(c, x, x1, y0, "RETRIGGER");
      dataCheck(c, "rl", a.retrigLaunch, x, y0 + 26, "LAUNCH");
      dataCheck(c, "rr", a.retrigRate, x, y0 + 54, "RATE");
      divisionDrop(c, "rd", a.retrigDiv, x + 54, y0 + 50, 56, 18);
      dataCheck(c, "rn", a.retrigNote, x, y0 + 82, "NOTE");
      dataCheck(c, "rf", a.retrigFirst, x + 54, y0 + 82, "FIRST");
    }
    // VELOCITY
    {
      const float x = kAX + 918, x1 = kW - 10;
      sectionTitle(c, x, x1, y0, "VELOCITY");
      dataCheck(c, "ve", a.velo, x, y0 + 26, "ON");
      dataCheck(c, "vr", a.veloRetrig, x + 44, y0 + 26, "RETRIG");
      char b[24];
      std::snprintf(b, sizeof b, "%.0f%%", a.veloDecay * 100.0);
      dataKnob(c, "vd", a.veloDecay, dataDesc("Velocity decay", 0, 1, ArpSlot{}.veloDecay, "%"), x + 26, y0 + 72, "DECAY", b, 8330);
      std::snprintf(b, sizeof b, "%.0f%%", a.veloTarget * 100.0);
      dataKnob(c, "vt", a.veloTarget, dataDesc("Velocity target", 0, 1, ArpSlot{}.veloTarget, "%"), x + 76, y0 + 72, "TARGET", b, 8331);
    }
    // modulation offsets (parameters: the matrix and automation move them)
    moreButton(c, "arpmod", kW - 28, kSlotY - 22, {G_ArpRate, G_ArpShift, G_ArpRange, G_ArpOffset, G_ArpRepeats, G_ArpGate, G_ArpChance, G_ArpRetrigRate,
                                                   G_ArpVeloDecay, G_ArpVeloTarget, G_ArpTranspose, G_ArpWrapTranspose, G_ArpWrapRange, G_ArpWrapPhantom, G_Swing, G_SwingDiv},
               {}, "Arp modulation offsets (also matrix destinations under Global) and swing");
  }
  // EDIT ALL: copy the change to every slot (names stay)
  if (editAll && !sameArp(before, a)) {
    for (auto& s : c.m.arp) {
      const std::string nm = s.name;
      s = a;
      s.name = nm;
    }
    commit(c, "Edit all arp slots");
  }
  slotRow(c, false);
}

// ================================================================== CLIP editor
namespace {
constexpr float kCx0 = 0, kCw = 165, kRollX = 217, kRollY = kPageY + 2, kRulerH = 18, kMarkH = 16, kLaneH = 70;
constexpr int kGridCount = 8;
const char* const kGridName[kGridCount] = {"1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "Off"};
const float kGridBeats[kGridCount] = {1.f, 0.5f, 0.25f, 0.125f, 0.0625f, 1.f / 3, 1.f / 6, 0.f};
float snapOf(const Ctx& c) { return kGridBeats[std::clamp(c.u.clipGrid, 0, kGridCount - 1)]; }

std::string beatsText(float b) {
  const int bars = static_cast<int>(b / 4), beats = static_cast<int>(std::fmod(b, 4.f)), sixteenth = static_cast<int>(std::lround(std::fmod(b, 1.f) * 4));
  char t[24];
  std::snprintf(t, sizeof t, "%d. %d. %d", bars, beats, sixteenth);
  return t;
}

void clipSettings(Ctx& c, ClipSlot& cs) {
  const float x = kCx0 + 8, w = kCw - 16;
  float y = kPageY + 8;
  frameBox(c, kCx0, kPageY, kCw, kStripY - kPageY, c.P.panelLo);
  {
    bool sm = c.u.clipMacros;
    ImGui::SetCursorScreenPos(c.p(x, y - 2));
    if (ImGui::InvisibleButton("##showmac", c.F.sz(w, 16))) c.u.clipMacros = !sm;
    const ImVec2 a = c.p(x + 22, y + 1), b = c.p(x + 30.5f, y + 9.5f);
    if (sm) skin::gradientRect(c.dl, a, b, skin::lighten(c.P.accent, 0.2f), c.P.accent, c.s(2));
    else c.dl->AddRectFilled(a, b, c.P.well, c.s(2)), c.dl->AddRect(a, b, c.P.lineHi, c.s(2));
    text(c, x + 36, y + 1, "SHOW MACROS", sm ? c.P.text : c.P.textDim, 8.5f, true);
    iconArrow(c.dl, c.p(x + 116, y + 5.5f), c.s(7), 1, c.P.textDim);
  }
  y += 24;
  hline(c, kGutter + 1, kCw - kGutter - 1, y - 6, c.P.line);
  if (c.u.clipMacros) {
    // the eight macros (same as the lower panel), in two columns
    const float cw = w / 2, ch = 64;
    for (int i = 0; i < kMacros; ++i) {
      const float mx = x + (i / 4) * cw, my = y + (i % 4) * ch;
      ImGui::PushID(i + 8700);
      frameBox(c, mx, my, cw, ch, c.P.panelHi);
      knob(c, macroP(i), mx + cw * 0.5f, my + 24, 14.0f, nullptr, KnobNoLabel);
      const std::string& nm = c.m.macroName[static_cast<size_t>(i)];
      textMid(c, mx + 2, mx + cw - 2, my + ch - 11, nm.empty() ? ("Macro " + std::to_string(i + 1)).c_str() : nm.c_str(), c.P.textDim, 8, true);
      ImGui::PopID();
    }
    return;
  }
  caption(c, x, x + w, y, "GLOBAL");
  y += 18;
  if (button(c, "mono", x, y, 46, 20, "MONO", !c.m.clipPoly, 8)) c.m.clipPoly = false, commit(c, "Clip trigger mode");
  if (button(c, "poly", x + 48, y, 46, 20, "POLY", c.m.clipPoly, 8)) c.m.clipPoly = true, commit(c, "Clip trigger mode");
  if (button(c, "eall", x + 100, y, 49, 20, "EDIT ALL", c.u.clipEditAll, 7.5f)) c.u.clipEditAll = !c.u.clipEditAll;
  y += 30;
  caption(c, x, x + w, y, "CLIP SETTINGS");
  y += 18;
  {
    std::string nm = cs.name.empty() ? "Clip " + std::to_string(c.u.clipSlot + 1) : cs.name;
    int step = 0;
    dropField(c, "clip", x, y, w, 18, nm.c_str(), [&] {
      static ClipSlot clip;
      static bool has = false;
      if (ImGui::MenuItem(i18n::tr("Copy clip"))) clip = cs, has = true;
      if (ImGui::MenuItem(i18n::tr("Paste clip"), nullptr, false, has)) cs = clip, commit(c, "Paste clip");
      if (ImGui::MenuItem(i18n::tr("Clear notes"))) cs.notes.clear(), commit(c, "Clear clip");
      if (ImGui::MenuItem(i18n::tr("Double the length"))) {
        const size_t n = cs.notes.size();
        for (size_t i = 0; i < n; ++i) {
          ClipNote d = cs.notes[i];
          d.start += cs.lengthBeats;
          cs.notes.push_back(d);
        }
        cs.lengthBeats = std::min(64.f, cs.lengthBeats * 2);
        commit(c, "Double clip");
      }
      ImGui::Separator();
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s", cs.name.c_str());
      ImGui::SetNextItemWidth(c.s(140));
      if (ImGui::InputText(i18n::tr("Name"), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) cs.name = buf, commit(c, "Rename clip");
    }, true, &step);
    if (step) c.u.clipSlot = (c.u.clipSlot + step + kClipSlots) % kClipSlots;
  }
  y += 26;
  auto row = [&](const char* label) {
    text(c, x, y + 5, label, c.P.textDim, 8.5f, true);
  };
  row("LENGTH");
  {
    int sixteenths = static_cast<int>(std::lround(cs.lengthBeats * 4));
    if (dataStepper(c, "len", sixteenths, 1, 256, x + 66, y, w - 66, 20, beatsText(cs.lengthBeats).c_str(), "Clip length")) cs.lengthBeats = sixteenths / 4.f, commit(c, "Clip length");
  }
  y += 26;
  row("KB SPAN");
  {
    const char* sp[] = {"Off", "Mono", "Poly", "Offset"};
    dropField(c, "span", x + 66, y, w - 66, 20, sp[std::clamp(cs.span, 0, 3)], [&] {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(sp[i], i == cs.span)) cs.span = i, commit(c, "KB span");
    });
  }
  y += 26;
  row("TRANS");
  dataStepper(c, "tr", cs.transpose, -48, 48, x + 66, y, w - 66, 20, (std::to_string(cs.transpose) + " st").c_str(), "Clip transpose");
  y += 26;
  row("MODE");
  {
    const char* md[] = {"Normal", "Reverse", "Pendulum", "Random", "Random No Repeat", "Random Start", "Random End", "One Shot", "Static"};
    dropField(c, "mode", x + 66, y, w - 66, 20, md[std::clamp(cs.mode, 0, 8)], [&] {
      for (int i = 0; i < 9; ++i)
        if (ImGui::Selectable(md[i], i == cs.mode)) cs.mode = i, commit(c, "Clip mode");
    });
  }
  y += 24;
  row("  TIME");
  {
    int sixteenths = static_cast<int>(std::lround(cs.modeTime * 4));
    char b[24];
    std::snprintf(b, sizeof b, "%g beat%s", static_cast<double>(cs.modeTime), cs.modeTime == 1.f ? "" : "s");
    if (dataStepper(c, "mt", sixteenths, 1, 64, x + 66, y, w - 66, 18, b, "Mode time")) cs.modeTime = sixteenths / 4.f, commit(c, "Mode time");
  }
  y += 26;
  row("RATE");
  {
    static const float rates[] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
    int ri = 2;
    for (int i = 0; i < 5; ++i)
      if (std::fabs(cs.rate - rates[i]) < 1e-3f) ri = i;
    char b[16];
    std::snprintf(b, sizeof b, "%gx", static_cast<double>(cs.rate));
    if (dataStepper(c, "rate", ri, 0, 4, x + 66, y, w - 66, 20, b, "Clip rate")) cs.rate = rates[ri], commit(c, "Clip rate");
  }
  y += 24;
  dataCheck(c, "cbpm", cs.bpm, x + 6, y + 4, "BPM", 8);
  dataToggle(c, "ctrip", cs.trip, x + 66, y, 40, 18, "T", 9);
  dataToggle(c, "cdot", cs.dot, x + 108, y, 40, 18, "D", 9);
  y += 28;
  row("LAUNCH");
  divisionDrop(c, "clq", cs.launchQuant, x + 66, y, w - 66, 20);
  y += 28;
  dataCheck(c, "crt", cs.retrig, x, y, "RETRIG", 8);
  dataCheck(c, "cvt", cs.veloTrig, x + 72, y, "VELO TRIG", 8);
  y += 18;
  dataCheck(c, "cng", cs.noteGate, x, y, "NOTE GATE", 8);
  y += 22;
  // offsets (parameters, modulatable)
  moreButton(c, "clipmod", x, y, {G_ClipTranspose, G_ClipRate, G_ClipOffset, G_Swing, G_SwingDiv}, [&] {
    ImGui::Separator();
    ImGui::SetNextItemWidth(c.s(150));
    float so = cs.startOffset;
    if (ImGui::SliderFloat(i18n::tr("Start offset (beats)"), &so, 0.f, std::max(0.25f, cs.lengthBeats))) cs.startOffset = so, commit(c, "Clip start", 8400);
  }, "Clip offsets (modulatable), swing and start offset");
  text(c, x + 24, y + 4, "OFFSETS", c.P.textFaint, 7.5f, true);
}

// RECORD / OVERDUB / EXTEND / quantize (bottom of the left column)
void clipRecordBlock(Ctx& c) {
  const float x = kCx0 + 8, w = kCw - 16, y = kStripY - 80;
  hline(c, 0, kCw, y - 10, c.P.line);
  Ui& u = c.u;
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##rec", c.F.sz(w, 26))) {
    u.clipRec = !u.clipRec;
    u.clipRecStart = -1, u.clipRecCleared = false;
    for (float& v : u.clipRecOn) v = 0;
  }
  const bool hv = ImGui::IsItemHovered();
  panelRect(c, x, y, w, 26, u.clipRec ? theme::withAlpha(c.P.danger, 0.25f) : (hv ? c.P.panelHi : c.P.panel), 4);
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + 26), u.clipRec ? c.P.danger : c.P.line, c.s(4));
  const bool blink = u.clipRec && std::fmod(ImGui::GetTime(), 1.0) < 0.6;
  c.dl->AddCircleFilled(c.p(x + w * 0.5f - 30, y + 13), c.s(4.5f), u.clipRec ? (blink ? c.P.danger : theme::withAlpha(c.P.danger, 0.5f)) : c.P.textDim);
  text(c, x + w * 0.5f - 20, y + 8, "RECORD", u.clipRec ? c.P.text : c.P.textDim, 9, true);
  if (hv)
    c.a.setHint(u.clipRec ? "Recording: played notes go into this clip (they sound directly while recording)"
                          : "Record played notes into the clip (from the first note; aligned to the bar when the song plays)");
  const float ry = y + 34;
  panelRect(c, x, ry, w, 24, c.P.panel, 4);
  if (button(c, "ovd", x + 2, ry + 2, 58, 20, "OVERDUB", u.clipOverdub, 7.5f)) u.clipOverdub = !u.clipOverdub;
  if (ImGui::IsItemHovered()) c.a.setHint("Overdub: keep the notes already in the clip (off: the first recorded note clears it)");
  if (button(c, "ext", x + 62, ry + 2, 46, 20, "EXTEND", u.clipExtend, 7.5f)) u.clipExtend = !u.clipExtend;
  if (ImGui::IsItemHovered()) c.a.setHint("Extend: the clip grows (by bars) while you record past its end, instead of wrapping");
  if (button(c, "qnt", x + 110, ry + 2, 37, 20, "", u.clipQuant, 7.5f)) u.clipQuant = !u.clipQuant;
  {
    // quantize glyph: a note snapping onto a grid line
    const ImU32 col = u.clipQuant ? c.P.accent : c.P.textDim;
    const ImVec2 q = c.p(x + 128.5f, ry + 12);
    for (int i = -1; i <= 1; ++i) c.dl->AddLine(ImVec2(q.x + c.s(6) * i, q.y - c.s(6)), ImVec2(q.x + c.s(6) * i, q.y + c.s(6)), theme::withAlpha(col, 0.5f));
    c.dl->AddRectFilled(ImVec2(q.x - c.s(6), q.y - c.s(2)), ImVec2(q.x + c.s(1), q.y + c.s(2)), col, c.s(1));
  }
  if (ImGui::IsItemHovered()) c.a.setHint("Quantize recorded notes to the grid");
}

// Recorded notes from the instance into the clip (UI side of RECORD).
void clipRecordPoll(Ctx& c, ClipSlot& cs) {
  Ui& u = c.u;
  if (!c.t.isChannel()) return;
  auto rt = c.a.builder.channelRuntime(c.t.owner);
  if (!rt || !rt->gen) return;
  Generator* g = rt->gen.get();
  static bool wasOn = false;
  if (u.clipRec != wasOn) {
    setClipRecording(g, u.clipRec);
    wasOn = u.clipRec;
    // start reading from now
    RecordedEvent skip[64];
    while (recordedEvents(g, u.clipRecRead, skip, 64) > 0) {
    }
    if (!u.clipRec) return;
  }
  if (!u.clipRec) return;
  setClipRecording(g, true);  // keep it on (the instance may have been rebuilt)
  RecordedEvent ev[64];
  const int n = recordedEvents(g, u.clipRecRead, ev, 64);
  bool changed = false;
  const float snap = snapOf(c) > 0 ? snapOf(c) : 0.25f;
  for (int i = 0; i < n; ++i) {
    const RecordedEvent& e = ev[i];
    if (u.clipRecStart < 0) {
      if (!e.on) continue;
      u.clipRecHost = e.playing;
      // with the song playing the take is aligned to the bar the first note falls in
      u.clipRecStart = e.playing ? std::floor(e.beat / 4.0) * 4.0 : e.clock;
      if (!u.clipOverdub && !u.clipRecCleared) cs.notes.clear(), u.clipRecCleared = true;
    }
    double p = (u.clipRecHost ? e.beat : e.clock) - u.clipRecStart;
    if (p < 0) p = 0;
    const double len = std::max(0.25f, cs.lengthBeats);
    if (u.clipExtend) {
      if (p >= len && p < 64) cs.lengthBeats = static_cast<float>(std::min(64.0, std::ceil((p + 1e-3) / 4.0) * 4.0)), changed = true;
    } else {
      p = std::fmod(p, len);
    }
    const int k = std::clamp(e.key, 0, 127);
    if (e.on) {
      u.clipRecOn[k] = static_cast<float>(p) + 1, u.clipRecVel[k] = e.vel;
    } else if (u.clipRecOn[k] > 0) {
      ClipNote nn;
      float st = u.clipRecOn[k] - 1;
      float ln = static_cast<float>(p) - st;
      if (ln <= 0) ln += static_cast<float>(len);  // wrapped past the clip end
      if (u.clipQuant) st = std::round(st / snap) * snap, ln = std::max(snap, std::round(ln / snap) * snap);
      nn.start = std::clamp(st, 0.f, std::max(0.f, cs.lengthBeats - 1.f / 64));
      nn.len = std::max(1.f / 64, ln);
      nn.key = k;
      nn.vel = std::clamp(u.clipRecVel[k], 0.02f, 1.f);
      cs.notes.push_back(nn);
      u.clipRecOn[k] = 0;
      changed = true;
    }
  }
  if (changed) commit(c, "Record clip", 8510);
}

void pianoRoll(Ctx& c, ClipSlot& cs) {
  const float x0 = kRollX, y0 = kRollY + kRulerH + kMarkH, x1 = kW - 4, y1 = kSlotY - kLaneH - 22;
  const float len = std::max(0.25f, cs.lengthBeats);
  const float view = len;  // the whole clip in view
  int& top = c.u.clipTopKey;
  // rows: every key from `top` down, or (FOLD) only the keys the clip uses
  std::vector<int> rows;
  if (c.u.clipFold) {
    bool used[128] = {};
    for (const auto& n : cs.notes) used[std::clamp(n.key, 0, 127)] = true;
    for (int k = 127; k >= 0; --k)
      if (used[k]) rows.push_back(k);
    if (rows.empty()) rows.push_back(60);
  }
  const float keyH = c.u.clipFold ? std::clamp((y1 - y0) / static_cast<float>(rows.size()), 9.f, 22.f) : 9.f;
  const int visible = static_cast<int>((y1 - y0) / keyH);
  if (!c.u.clipFold) {
    // scrolls to the notes when none is in view (a newly shown clip or preset)
    bool inView = cs.notes.empty();
    for (const auto& n : cs.notes) inView = inView || (n.key <= top && n.key > top - visible);
    static size_t shown = 0;
    const size_t sig = std::hash<std::string>{}(c.m.presetName + "\x1f" + cs.name) * 31u + static_cast<size_t>(c.u.clipSlot);
    const bool fresh = sig != shown;
    shown = sig;
    if ((fresh || !inView) && !cs.notes.empty() && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1)) {
      int lo = 127, hi = 0;
      for (const auto& n : cs.notes) lo = std::min(lo, n.key), hi = std::max(hi, n.key);
      top = std::min(127, std::max(std::min(hi + 4, 127), (lo + hi) / 2 + visible / 2));
    }
    top = std::clamp(top, visible - 1, 127);
    for (int k = top; k > top - visible && k >= 0; --k) rows.push_back(k);
  }
  int rowOf[128];
  std::fill(std::begin(rowOf), std::end(rowOf), -1);
  for (size_t i = 0; i < rows.size(); ++i) rowOf[rows[i]] = static_cast<int>(i);
  auto keyY = [&](int k) { return rowOf[std::clamp(k, 0, 127)] >= 0 ? y0 + rowOf[std::clamp(k, 0, 127)] * keyH : -1e6f; };
  auto beatX = [&](float b) { return x0 + (x1 - x0) * b / view; };
  // grid division + FOLD / audition (left of the ruler)
  panelRect(c, kCw, kRollY, kRollX - kCw, kRulerH + kMarkH, c.P.head);
  dropField(c, "cgrid", kCw + 2, kRollY + 1, kRollX - kCw - 4, kRulerH - 2, kGridName[std::clamp(c.u.clipGrid, 0, kGridCount - 1)], [&] {
    for (int i = 0; i < kGridCount; ++i)
      if (ImGui::Selectable(kGridName[i], i == c.u.clipGrid)) c.u.clipGrid = i;
  });
  if (ImGui::IsItemHovered()) c.a.setHint("Snap grid for notes and lane points (Shift: no snap)");
  if (button(c, "fold", kCw + 2, kRollY + kRulerH + 1, 30, kMarkH - 2, "FOLD", c.u.clipFold, 6.5f)) c.u.clipFold = !c.u.clipFold;
  if (ImGui::IsItemHovered()) c.a.setHint("Fold: show only the keys the clip uses");
  if (button(c, "aud", kCw + 34, kRollY + kRulerH + 1, kRollX - kCw - 36, kMarkH - 2, "", c.u.clipAudition, 7)) c.u.clipAudition = !c.u.clipAudition;
  {
    // headphones glyph
    const ImVec2 q = c.p(kCw + 34 + (kRollX - kCw - 36) * 0.5f, kRollY + kRulerH + 8.5f);
    const ImU32 col = c.u.clipAudition ? c.P.accent : c.P.textDim;
    ::gl::ui::drawIconGlyph(c.dl, ::gl::ui::Icon::Headphones, q, c.s(5), c.s(1.2f), col);
  }
  if (ImGui::IsItemHovered()) c.a.setHint("Audition: hear notes while you place them");
  // start / end markers (clip start offset, clip length)
  panelRect(c, kRollX, kRollY + kRulerH, kW - kRollX, kMarkH, c.P.panelLo);
  {
    static int mdrag = -1;
    const float my = kRollY + kRulerH;
    for (int mk2 = 0; mk2 < 2; ++mk2) {
      const float mx = mk2 == 0 ? beatX(std::clamp(cs.startOffset, 0.f, len)) : beatX(len);
      ImGui::PushID(mk2 + 8800);
      ImGui::SetCursorScreenPos(c.p(mx - (mk2 ? 10 : 0), my));
      ImGui::InvisibleButton("##mk", c.F.sz(10, kMarkH));
      if (ImGui::IsItemActivated()) mdrag = mk2;
      if (ImGui::IsItemDeactivated()) mdrag = -1;
      if (ImGui::IsItemActive() && mdrag == mk2) {
        const float b = (ImGui::GetIO().MousePos.x - c.p(x0, 0).x) / c.s(x1 - x0) * view;
        const float g = snapOf(c) > 0 ? snapOf(c) : 0.25f;
        if (mk2 == 0) cs.startOffset = std::clamp(std::round(b / g) * g, 0.f, len - g), commit(c, "Clip start", 8520);
        else cs.lengthBeats = std::clamp(std::round(b / g) * g, g, 64.f), commit(c, "Clip length", 8521);
      }
      if (ImGui::IsItemHovered()) c.a.setHint(mk2 ? "Clip end: drag to change the length" : "Clip start: playback begins here");
      const ImVec2 q = c.p(mx, my + 2);
      const float s = c.s(8);
      if (mk2 == 0) c.dl->AddTriangleFilled(q, ImVec2(q.x + s, q.y), ImVec2(q.x, q.y + s), c.P.accent);
      else c.dl->AddTriangleFilled(ImVec2(q.x - s, q.y), q, ImVec2(q.x, q.y + s), c.P.accent);
      ImGui::PopID();
    }
  }
  // ruler
  panelRect(c, kRollX, kRollY, kW - kRollX, kRulerH, c.P.head);
  for (int b = 0; b <= static_cast<int>(len * 4); ++b) {
    const float bx = beatX(b / 4.f);
    if (b % 4 == 0) {
      vline(c, bx, kRollY + 4, kRollY + kRulerH, c.P.textDim);
      char t[16];
      std::snprintf(t, sizeof t, "%d.%d", b / 16 + 1, (b / 4) % 4 + 1);
      text(c, bx + 3, kRollY + 3, t, c.P.textDim, 8, true);
    } else {
      vline(c, bx, kRollY + 12, kRollY + kRulerH, c.P.textFaint);
    }
  }
  // keyboard
  for (int k : rows) {
    const int pc = k % 12;
    const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    const float ky = keyY(k);
    panelRect(c, kCw + 2, ky, kRollX - kCw - 4, keyH - 0.5f, black ? c.P.keyBlack : c.P.keyWhite);
    if (pc == 0 || c.u.clipFold) {
      static const char* const nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      char t[16];
      std::snprintf(t, sizeof t, "%s%d", nn[pc], k / 12 - 2);
      text(c, kCw + 4, ky + (keyH - 9) * 0.5f - 0.5f, t, black ? c.P.keyWhite : c.P.keyBlack, 7, true);
    }
    panelRect(c, x0, ky, x1 - x0, keyH, black ? c.P.panelLo : c.P.panel);
    if (pc == 0) hline(c, x0, x1, ky + keyH - 0.5f, c.P.gridStrong);
  }
  for (int b = 1; b < static_cast<int>(len * 4); ++b) vline(c, beatX(b / 4.f), y0, y1, (b % 4) ? c.P.grid : c.P.gridStrong);
  // interaction
  ImGui::SetCursorScreenPos(c.p(x0, y0));
  ImGui::InvisibleButton("##roll", c.F.sz(x1 - x0, y1 - y0), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const float mb = std::clamp((m.x - c.p(x0, 0).x) / c.s(x1 - x0) * view, 0.f, len);
  const int mrow = std::clamp(static_cast<int>(std::floor((m.y - c.p(0, y0).y) / c.s(keyH))), 0, static_cast<int>(rows.size()) - 1);
  const int mk = rows[static_cast<size_t>(mrow)];
  const float grid = io.KeyShift || snapOf(c) <= 0 ? 1.f / 64 : snapOf(c);
  int hit = -1;
  bool edge = false;
  for (size_t i = 0; i < cs.notes.size(); ++i) {
    const auto& n = cs.notes[i];
    if (n.key == mk && mb >= n.start && mb <= n.start + n.len) {
      hit = static_cast<int>(i);
      edge = c.s((x1 - x0) * (n.start + n.len - mb) / view) < c.s(5);
    }
  }
  static int drag = -1;
  static bool dragEdge = false;
  static float grabOff = 0;
  if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
    if (hit >= 0) {
      drag = hit, dragEdge = edge, grabOff = mb - cs.notes[static_cast<size_t>(hit)].start;
      c.u.clipSel = hit;
    } else {
      ClipNote n;
      n.start = std::floor(mb / grid) * grid;
      n.len = c.u.clipLastLen > 0 ? c.u.clipLastLen : 0.25f;
      n.key = mk;
      n.vel = 0.8f;
      cs.notes.push_back(n);
      drag = static_cast<int>(cs.notes.size()) - 1, dragEdge = true, grabOff = 0;
      c.u.clipSel = drag;
      commit(c, "Add note", 8500);
    }
    if (c.u.clipAudition && !c.u.clipRec) previewKey(c, mk, true), c.u.heldKey = mk;
  }
  if (ImGui::IsItemDeactivated() && c.u.heldKey >= 0) previewKey(c, c.u.heldKey, false), c.u.heldKey = -1;
  if (ImGui::IsItemActive() && drag >= 0 && drag < static_cast<int>(cs.notes.size()) && ImGui::IsMouseDragging(0, 1)) {
    ClipNote& n = cs.notes[static_cast<size_t>(drag)];
    if (dragEdge) n.len = std::max(grid, std::round((mb - n.start) / grid) * grid), c.u.clipLastLen = n.len;
    else n.start = std::clamp(std::round((mb - grabOff) / grid) * grid, 0.f, len - grid), n.key = mk;
    commit(c, "Edit note", 8501);
  }
  if (ImGui::IsItemDeactivated()) drag = -1;
  if (ImGui::IsItemHovered() && (ImGui::IsMouseClicked(1) || ImGui::IsMouseDoubleClicked(0)) && hit >= 0) {
    cs.notes.erase(cs.notes.begin() + hit);
    c.u.clipSel = -1, drag = -1;
    commit(c, "Remove note");
  }
  if (ImGui::IsItemHovered() && io.MouseWheel != 0 && !c.u.clipFold) top = std::clamp(top + static_cast<int>(io.MouseWheel * 3), visible - 1, 127);
  if (ImGui::IsWindowFocused() && c.u.clipSel >= 0 && c.u.clipSel < static_cast<int>(cs.notes.size()) && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
    cs.notes.erase(cs.notes.begin() + c.u.clipSel);
    c.u.clipSel = -1;
    commit(c, "Remove note");
  }
  // notes
  c.dl->PushClipRect(c.p(x0, y0), c.p(x1, y1), true);
  for (size_t i = 0; i < cs.notes.size(); ++i) {
    const auto& n = cs.notes[i];
    const float ny = keyY(n.key);
    if (ny < y0 - keyH || ny > y1) continue;
    const bool sel = static_cast<int>(i) == c.u.clipSel;
    const ImVec2 a = c.p(beatX(n.start) + 0.5f, ny + 0.5f), b = c.p(beatX(n.start + n.len) - 0.5f, ny + keyH - 0.5f);
    c.dl->AddRectFilled(a, b, theme::withAlpha(n.mute ? c.P.textFaint : c.P.accent, 0.45f + 0.55f * n.vel), c.s(2));
    c.dl->AddRect(a, b, sel ? c.P.text : theme::withAlpha(c.P.frame, 0.6f), c.s(2));
  }
  if (c.hasLive && c.live.clipPos >= 0) vline(c, beatX(static_cast<float>(c.live.clipPos)), y0, y1, c.P.mod);
  c.dl->PopClipRect();
  if (ImGui::IsItemHovered())
    c.a.setHint("Click: add a note \xC2\xB7 drag: move (the right edge: length) \xC2\xB7 right-click / double-click: remove \xC2\xB7 Shift: fine grid \xC2\xB7 wheel: scroll");
  // velocity / automation lanes
  const float ly = y1 + 22, lh = kLaneH - 4;
  {
    std::vector<std::string> names = {"Velocity"};
    for (const auto& l : cs.lanes) names.push_back(l.target >= 0 ? paramShortName(l.target) : "?");
    float tx = kRollX;
    int& lane = c.u.clipLane;
    lane = std::clamp(lane, 0, static_cast<int>(names.size()) - 1);
    panelRect(c, kCw, y1 + 2, kW - kCw, 20, c.P.head);
    for (size_t i = 0; i < names.size(); ++i) {
      ImGui::PushID(static_cast<int>(i) + 8600);
      const float tw = std::max(60.f, textW(c, names[i].c_str(), 8.5f, true) + 18);
      if (button(c, "lane", tx, y1 + 3, tw, 18, names[i].c_str(), lane == static_cast<int>(i), 8.5f)) lane = static_cast<int>(i);
      if (i > 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
        cs.lanes.erase(cs.lanes.begin() + static_cast<long>(i - 1));
        lane = 0;
        commit(c, "Remove lane");
        ImGui::PopID();
        break;
      }
      if (i > 0 && ImGui::IsItemHovered()) c.a.setHint("Right-click: remove this lane");
      tx += tw + 2;
      ImGui::PopID();
    }
    if (button(c, "addlane", tx, y1 + 3, 22, 18, GL_ICON_LABEL("plus", "+"), false, 10)) ImGui::OpenPopup("##addlane");
    if (ImGui::BeginPopup("##addlane")) {
      int out = -2;
      if (destMenuItems(c, out) && out >= 0 && cs.lanes.size() < 32) {
        cs.lanes.push_back({out, {{0.f, static_cast<float>(pdesc(out) ? pdesc(out)->toNormalized(val(c, out)) : 0.5)}}});
        c.u.clipLane = static_cast<int>(cs.lanes.size());
        commit(c, "Add lane");
      }
      ImGui::EndPopup();
    }
    displayWellPublic(c, x0, ly, x1 - x0, lh);
    for (int b = 1; b < static_cast<int>(len * 4); ++b) vline(c, beatX(b / 4.f), ly, ly + lh, (b % 4) ? c.P.grid : c.P.gridStrong);
    ImGui::SetCursorScreenPos(c.p(x0, ly));
    ImGui::InvisibleButton("##lane", c.F.sz(x1 - x0, lh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const float lv = std::clamp(1.f - (m.y - c.p(0, ly).y) / c.s(lh), 0.f, 1.f);
    const float lb = std::clamp((m.x - c.p(x0, 0).x) / c.s(x1 - x0) * view, 0.f, len);
    if (lane == 0) {
      // velocity bars: drag over the notes to set them
      for (const auto& n : cs.notes) {
        const float bx = beatX(n.start);
        c.dl->AddRectFilled(c.p(bx, ly + lh - n.vel * (lh - 4)), c.p(bx + 3, ly + lh), c.P.accent);
      }
      if (ImGui::IsItemActive())
        for (auto& n : cs.notes)
          if (std::fabs(c.s((x1 - x0) * (n.start - lb) / view)) < c.s(4)) n.vel = std::max(0.02f, lv), commit(c, "Note velocity", 8502);
      if (ImGui::IsItemHovered()) c.a.setHint("Drag over the bars: note velocities");
    } else {
      ClipLane& L = cs.lanes[static_cast<size_t>(lane - 1)];
      std::vector<ImVec2> pts;
      for (const auto& [b, v] : L.pts) pts.push_back(c.p(beatX(b), ly + lh - v * (lh - 4) - 2));
      if (!L.pts.empty()) {
        pts.insert(pts.begin(), c.p(x0, ly + lh - L.pts.front().second * (lh - 4) - 2));
        pts.push_back(c.p(x1, ly + lh - L.pts.back().second * (lh - 4) - 2));
      }
      if (pts.size() >= 2) c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c.P.mod, 0, std::max(1.4f, c.s(1.6f)));
      static int ldrag = -1;
      int lhit = -1;
      for (size_t i = 0; i < L.pts.size(); ++i) {
        const ImVec2 q = c.p(beatX(L.pts[i].first), ly + lh - L.pts[i].second * (lh - 4) - 2);
        c.dl->AddCircleFilled(q, c.s(3), c.P.mod);
        if (std::hypot(q.x - m.x, q.y - m.y) < c.s(6)) lhit = static_cast<int>(i);
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        if (lhit >= 0) {
          ldrag = lhit;
        } else {
          L.pts.push_back({std::round(lb / grid) * grid, lv});
          std::sort(L.pts.begin(), L.pts.end());
          for (size_t i = 0; i < L.pts.size(); ++i)
            if (std::fabs(L.pts[i].first - std::round(lb / grid) * grid) < 1e-5f) ldrag = static_cast<int>(i);
          commit(c, "Add lane point", 8503);
        }
      }
      if (ImGui::IsItemActive() && ldrag >= 0 && ldrag < static_cast<int>(L.pts.size())) {
        L.pts[static_cast<size_t>(ldrag)] = {std::round(lb / grid) * grid, lv};
        const auto keep = L.pts[static_cast<size_t>(ldrag)];
        std::sort(L.pts.begin(), L.pts.end());
        for (size_t i = 0; i < L.pts.size(); ++i)
          if (L.pts[i] == keep) ldrag = static_cast<int>(i);
        commit(c, "Edit lane", 8504);
      }
      if (ImGui::IsItemDeactivated()) ldrag = -1;
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && lhit >= 0) {
        L.pts.erase(L.pts.begin() + lhit);
        commit(c, "Remove lane point");
      }
      if (ImGui::IsItemHovered()) c.a.setHint("Automation of " + paramLongName(L.target) + ": click to add a point, drag to move, right-click to remove");
    }
  }
}
}  // namespace

void clipEditor(Ctx& c) {
  panelRect(c, 0, kPageY, kW, kStripY - kPageY, c.P.panel);
  ClipSlot& cs = c.m.clip[static_cast<size_t>(std::clamp(c.u.clipSlot, 0, kClipSlots - 1))];
  const ClipSlot before = cs;
  clipSettings(c, cs);
  clipRecordBlock(c);
  clipRecordPoll(c, cs);
  pianoRoll(c, cs);
  if (c.u.clipEditAll) {
    // settings (not the notes) copied to every clip
    auto same = [](const ClipSlot& a, const ClipSlot& b) {
      return a.span == b.span && a.transpose == b.transpose && a.mode == b.mode && a.modeTime == b.modeTime && a.rate == b.rate && a.bpm == b.bpm &&
             a.trip == b.trip && a.dot == b.dot && a.launchQuant == b.launchQuant && a.retrig == b.retrig && a.veloTrig == b.veloTrig &&
             a.noteGate == b.noteGate && a.lengthBeats == b.lengthBeats;
    };
    if (!same(before, cs)) {
      for (auto& o : c.m.clip) {
        o.span = cs.span, o.transpose = cs.transpose, o.mode = cs.mode, o.modeTime = cs.modeTime, o.rate = cs.rate, o.bpm = cs.bpm;
        o.trip = cs.trip, o.dot = cs.dot, o.launchQuant = cs.launchQuant, o.retrig = cs.retrig, o.veloTrig = cs.veloTrig, o.noteGate = cs.noteGate;
        o.lengthBeats = cs.lengthBeats;
      }
      commit(c, "Edit all clips");
    }
  }
  slotRow(c, true);
  // close
  if (button(c, "closeclip", kW - 70, kPageY + 2, 64, 15, "CLOSE", false, 8)) c.u.clip = false;
}

}  // namespace gl::packs::plasma::ui
