// Gridline — Shuttle (gl.timegate) editor. J-100 layout:
//
//   header   HOLD pad, time / volume mix dials (lane coloured), the two chosen
//            slots as live "now" chips, note-trigger LED, Options
//   left     the time and volume slot banks (36 cells each: name over a faint
//            copy of the slot's envelope; picked / playing / edited states)
//   right    page switch (time / volume) + slot name + length, a labelled
//            tool bar (snap, step, slide, freeze, edit menu), the envelope
//            editor (axis gutter, beat numbers, playhead), the volume
//            smoothing rail (attack / release / tension) beside it and the
//            slot's playback settings underneath (link, key-held, one-shot,
//            force hold, trigger and position sync)
//   options  a labelled panel in place of the envelope (triggering, sound,
//            the quick effect / step gate used without a slot, engine)
//
// Every colour comes from Theme roles (dark and light) with the lane colours
// (time green, volume orange) lit through theme::litRole. Sizes scale with the
// editor width (k = width / 1000). Below ~860 design px the banks fold to the
// lane being edited; from ~1300 they get six columns.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include "../core/I18n.h"
#include "../engine/Program.h"
#include "../engine/TimeGateSlots.h"
#include "EnvEditor.h"
#include "LayoutAudit.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

namespace {

using namespace ed;
namespace sk = skin;

struct GbUi {
  bool init = false;
  bool volPage = false;
  int edit[2] = {0, 0};  // slot shown per page (0-based)
  int snap = 1;          // 0 off, 1 1/16, 2 1/12, 3 1/8, 4 1/6, 5 1/4, 6 1/3 beat
  bool step = false, slide = false, freeze = false, previews = true;
  bool options = false;  // options panel in place of the envelope
  envui::State env;
  char rename[32] = {};
  int renameBank = -1, renameSlot = -1;
  std::string clip;  // copied slot line
};
std::map<uint64_t, GbUi> gbUi;

// Banks are large: one cached parse per plugin key, refreshed when the text changes.
struct GbCache {
  std::string data;
  std::unique_ptr<tg::Bank> bank;
};
std::map<uint64_t, GbCache> gbCache;
tg::Bank& bankFor(uint64_t key, const std::string& data) {
  GbCache& c = gbCache[key];
  if (!c.bank || c.data != data) {
    if (!c.bank) c.bank = std::make_unique<tg::Bank>();
    tg::factory(*c.bank);
    tg::parse(data.c_str(), *c.bank);
    c.data = data;
  }
  return *c.bank;
}

const double kSnapDiv[7] = {0, 16, 12, 8, 6, 4, 3};  // snap cells per beat

void commitSlot(App& a, const PluginTarget& t, const PluginState& st, bool vol, int k, const tg::Slot& s, const char* what) {
  PluginState ns = st;
  ns.data = tg::replaceSlot(st.data, vol ? 'V' : 'T', k, s);
  setPluginState(a, t, ns, what, 0x4742000000000000ull ^ t.key() ^ static_cast<uint64_t>(k + (vol ? 64 : 0)));
}

// ------------------------------------------------------------------ drawing kit
// One editor's drawing context: palette roles, lane colours, scale and fonts.
struct Kit {
  App& a;
  const PluginTarget& t;
  ImDrawList* dl;
  theme::Palette P;
  float k;                 // size scale (width / 1000, clamped)
  ImU32 laneT, laneV;      // time green / volume orange (lit for the theme)
  ImU32 card, cell, face;  // panel card, slot cell, raised button face
  ImU32 onText;            // text on a lit lane face
  float fBody, fSmall, fCaps, fBig;
  sk::Skin knob;           // skin for the bound dials
  float px(float v) const { return std::round(v * k); }
  ImU32 lane(bool vol) const { return vol ? laneV : laneT; }
};

// Text fitted to maxW with an ellipsis (returns the shown string).
std::string fitText(ImFont* f, float size, const char* s, float maxW) {
  if (f->CalcTextSizeA(size, FLT_MAX, 0, s).x <= maxW) return s;
  std::string o = s;
  while (!o.empty()) {
    o.pop_back();
    while (!o.empty() && (static_cast<unsigned char>(o.back()) & 0xC0) == 0x80) o.pop_back();  // whole UTF-8 characters
    const std::string t = o + "\xE2\x80\xA6";
    if (f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x <= maxW) return t;
  }
  return "";
}

// Text in [p0, p1]: align -1 left, 0 centre, 1 right; fitted with an ellipsis.
// Returns the drawn width.
float text(const Kit& c, ImFont* f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* s, int align = -1) {
  if (!s || !*s) return 0;
  const std::string fit = fitText(f, size, s, std::max(0.0f, p1.x - p0.x));
  if (fit.empty()) return 0;
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, fit.c_str());
  const float x = align < 0 ? p0.x : align > 0 ? p1.x - ts.x : (p0.x + p1.x - ts.x) * 0.5f;
  const float y = (p0.y + p1.y - ts.y) * 0.5f;
  audit::control(ImVec2(x, y), ImVec2(x + ts.x, y + ts.y), "text");
  c.dl->AddText(f, size, ImVec2(std::round(x), std::round(y)), col, fit.c_str());
  return ts.x;
}
ImFont* fReg() { return app().fontRegular; }
ImFont* fBold() { return app().fontBold ? app().fontBold : app().fontRegular; }
ImFont* fMono() { return app().fontMono; }
ImFont* fMonoBold() { return app().fontMonoBold ? app().fontMonoBold : app().fontMono; }

// Rounded panel card (frame for the layout audit and the shared inset).
void card(const Kit& c, ImVec2 p0, ImVec2 p1, ImU32 col, const char* what) {
  audit::frame(p0, p1, what);
  sk::noteFrame(p0, p1);
  const float r = c.px(6);
  sk::gradientRect(c.dl, p0, p1, sk::lighten(col, c.P.dark ? 0.03f : 0.25f), col, r);
  c.dl->AddRect(p0, p1, c.P.dark ? theme::withAlpha(c.P.line, 0.9f) : c.P.line, r, 0, 1.0f);
  c.dl->AddLine(ImVec2(p0.x + r, p0.y + 1), ImVec2(p1.x - r, p0.y + 1), theme::withAlpha(c.P.paper, c.P.dark ? 0.05f : 0.6f), 1.0f);
}

// Small-caps caption (mono bold, dim).
void caps(const Kit& c, ImVec2 p0, ImVec2 p1, const char* s, ImU32 col = 0, int align = -1) {
  text(c, fMonoBold(), c.fCaps, p0, p1, col ? col : c.P.textDim, s, align);
}

// Flat push button; `on` latches it in the accent. Icon (optional) left of the
// label, or centred when there is no label. Returns true when clicked.
bool button(const Kit& c, const char* id, ImVec2 p0, ImVec2 p1, bool on, ImU32 accent, ui::Icon icon, bool hasIcon, const char* label,
            const char* tip, bool enabled = true) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::InvisibleButton("##b", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  ImGui::EndDisabled();
  const bool hov = enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled), act = ImGui::IsItemActive();
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    c.a.setHint(tip);
    ImGui::SetItemTooltip("%s", tip);
  }
  audit::control(p0, p1, "button");
  const float r = c.px(4);
  ImU32 bg = on ? theme::mix(c.face, accent, c.P.dark ? 0.30f : 0.22f) : c.face;
  if (act) bg = sk::darken(bg, 0.08f);
  else if (hov) bg = sk::lighten(bg, c.P.dark ? 0.07f : 0.35f);
  sk::gradientRect(c.dl, p0, p1, sk::lighten(bg, c.P.dark ? 0.04f : 0.3f), bg, r);
  c.dl->AddRect(p0, p1, on ? theme::withAlpha(accent, 0.85f) : (c.P.dark ? c.P.line : c.P.lineStrong), r, 0, 1.0f);
  ImU32 fg = !enabled ? c.P.textFaint : on ? (c.P.dark ? c.P.paper : c.P.text) : c.P.text;
  ImU32 ic = !enabled ? c.P.textFaint : on ? accent : c.P.textDim;
  const float h = p1.y - p0.y, is = std::round(h * 0.56f);
  if (hasIcon && !(label && *label)) {
    ui::drawIcon(c.dl, icon, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), is, ic);
  } else {
    float x = p0.x + c.px(8);
    if (hasIcon) {
      ui::drawIcon(c.dl, icon, ImVec2(x + is * 0.5f, (p0.y + p1.y) * 0.5f), is, ic);
      x += is + c.px(6);
    }
    text(c, fReg(), c.fBody, ImVec2(x, p0.y), ImVec2(p1.x - c.px(6), p1.y), fg, label, hasIcon ? -1 : 0);
  }
  ImGui::PopID();
  return clicked;
}

// Drop-down face (field, value, chevron); returns true when clicked.
bool dropFace(const Kit& c, const char* id, ImVec2 p0, ImVec2 p1, const char* value, const char* tip, ImU32 accent = 0) {
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton("##d", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered();
  if (tip && hov) c.a.setHint(tip), ImGui::SetItemTooltip("%s", tip);
  audit::control(p0, p1, "combo");
  const float r = c.px(4), h = p1.y - p0.y;
  sk::insetField(c.dl, p0, p1, hov ? sk::lighten(c.P.field, c.P.dark ? 0.05f : 0.0f) : c.P.field, r);
  c.dl->AddRect(p0, p1, hov ? c.P.lineStrong : (c.P.dark ? c.P.line : c.P.lineStrong), r, 0, 1.0f);
  if (accent) c.dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + c.px(5)), ImVec2(p0.x + 1 + c.px(2.5f), p1.y - c.px(5)), accent, c.px(1));
  text(c, fReg(), c.fBody, ImVec2(p0.x + c.px(accent ? 9 : 8), p0.y), ImVec2(p1.x - h * 0.9f, p1.y), c.P.text, value);
  ui::drawIcon(c.dl, ui::Icon::ChevronDown, ImVec2(p1.x - h * 0.45f, (p0.y + p1.y) * 0.5f), std::round(h * 0.42f), hov ? c.P.text : c.P.textDim);
  ImGui::PopID();
  return clicked;
}

// Drop-down over a plain value (slot data / editor state). Labels are English
// (i18n::N_), translated here. Returns true when the value changed.
bool dropFree(const Kit& c, const char* id, ImVec2 p0, ImVec2 p1, const char* const* items, int count, int& value, const char* tip) {
  const char* cur = value >= 0 && value < count ? i18n::tr(items[value]) : "";
  const bool open = dropFace(c, id, p0, p1, cur, tip);
  ImGui::PushID(id);
  if (open) ImGui::OpenPopup("##dp");
  bool ch = false;
  if (ImGui::BeginPopup("##dp")) {
    for (int i = 0; i < count; ++i)
      if (ImGui::MenuItem(i18n::tr(items[i]), nullptr, value == i) && value != i) value = i, ch = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return ch;
}

// Drop-down bound to a choice parameter (labels = the parameter's choices).
bool dropParam(const Kit& c, int param, ImVec2 p0, ImVec2 p1) {
  const ParamId pid = c.t.pid(param);
  auto d = describeParam(c.a.doc.song(), pid);
  if (!d || d->choices.empty()) return false;
  const int n = static_cast<int>(d->choices.size());
  const int cur = std::clamp(static_cast<int>(std::lround(pui::paramValue(c.a, c.t, param) - d->min)), 0, n - 1);
  char id[24];
  std::snprintf(id, sizeof id, "dp%d", param);
  const std::string tip = d->name + ": " + d->choices[static_cast<size_t>(cur)];
  const bool open = dropFace(c, id, p0, p1, i18n::tr(d->choices[static_cast<size_t>(cur)].c_str()), tip.c_str());
  ImGui::PushID(id);
  if (open) ImGui::OpenPopup("##dpp");
  bool ch = false;
  if (ImGui::BeginPopup("##dpp")) {
    for (int i = 0; i < n; ++i)
      if (ImGui::MenuItem(i18n::tr(d->choices[static_cast<size_t>(i)].c_str()), nullptr, i == cur) && i != cur)
        c.a.doc.setParam(pid, d->min + i, false), ch = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return ch;
}

// LED toggle bound to a toggle parameter: lamp + label; click flips.
bool ledParam(const Kit& c, int param, ImVec2 p0, ImVec2 p1, const char* label, ImU32 onCol = 0) {
  const ParamId pid = c.t.pid(param);
  auto d = describeParam(c.a.doc.song(), pid);
  if (!d) return false;
  const bool on = pui::paramValue(c.a, c.t, param) >= 0.5;
  ImGui::PushID(param + 9100);
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton("##led", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, p1.y - p0.y)));
  const bool hov = ImGui::IsItemHovered();
  if (hov) c.a.setHint(d->name + ": " + (on ? i18n::tr("on") : i18n::tr("off")) + "   (" + i18n::tr("click to toggle") + ")");
  if (clicked) c.a.doc.setParam(pid, on ? d->min : d->max, false);
  audit::control(p0, p1, "led");
  if (hov) c.dl->AddRectFilled(p0, p1, theme::withAlpha(c.P.text, 0.06f), c.px(4));
  const float h = p1.y - p0.y, r = std::max(3.0f, h * 0.17f);
  const ImVec2 lc(p0.x + c.px(6) + r, (p0.y + p1.y) * 0.5f);
  const ImU32 lit = onCol ? onCol : c.P.led;
  sk::ledDot(c.dl, lc, r, on, c.knob, lit);
  text(c, fReg(), c.fBody, ImVec2(lc.x + r + c.px(7), p0.y), ImVec2(p1.x - c.px(4), p1.y), on ? c.P.text : c.P.textDim, label);
  ImGui::PopID();
  return clicked;
}

// Bound dial with caption + value: dial left, caption and value to the right
// (side = true) or underneath (side = false).
void dial(const Kit& c, int param, ImVec2 tl, float size, ImU32 fill, const char* label, bool side, float boxW) {
  sk::wheel(c.a, c.t, param, tl, size, c.knob, sk::WheelBigLine, fill);
  const ParamId pid = c.t.pid(param);
  auto d = describeParam(c.a.doc.song(), pid);
  if (!d) return;
  const std::string v = d->format(pui::paramValue(c.a, c.t, param));
  if (side) {
    const float x = tl.x + size + c.px(8), x1 = tl.x + boxW;
    const float cy = tl.y + size * 0.5f;
    caps(c, ImVec2(x, cy - c.px(15)), ImVec2(x1, cy - c.px(1)), label);
    text(c, fMono(), c.fBody, ImVec2(x, cy + c.px(1)), ImVec2(x1, cy + c.px(15)), c.P.text, v.c_str());
  } else {
    const float x0 = tl.x + size * 0.5f - boxW * 0.5f, x1 = x0 + boxW;
    caps(c, ImVec2(x0, tl.y + size + c.px(2)), ImVec2(x1, tl.y + size + c.px(14)), label, 0, 0);
    text(c, fMono(), c.fSmall, ImVec2(x0, tl.y + size + c.px(14)), ImVec2(x1, tl.y + size + c.px(27)), c.P.text, v.c_str(), 0);
  }
}

// Faint copy of a slot's envelope (cell background / chips).
void miniEnv(const Kit& c, const tg::Slot& s, ImVec2 p0, ImVec2 p1, ImU32 col, float fillA, float lineA) {
  constexpr int N = 40;
  ImVec2 pts[N];
  for (int i = 0; i < N; ++i) {
    const double x = s.len * i / (N - 1.0);
    const double y = std::clamp(s.eval(x), 0.0, 1.0);
    pts[i] = ImVec2(p0.x + (p1.x - p0.x) * i / (N - 1.0f), p1.y - static_cast<float>(y) * (p1.y - p0.y));
  }
  if (fillA > 0)
    for (int i = 1; i < N; ++i) {
      ImVec2 q[4] = {pts[i - 1], pts[i], ImVec2(pts[i].x, p1.y), ImVec2(pts[i - 1].x, p1.y)};
      c.dl->AddConvexPolyFilled(q, 4, theme::withAlpha(col, fillA));
    }
  c.dl->AddPolyline(pts, N, theme::withAlpha(col, lineA), 0, std::max(1.0f, c.k * 1.1f));
}

}  // namespace

void editorGrossBeat(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  const PluginState st = *t.state(a.doc.song());
  GbUi& ui = gbUi[t.key()];
  tg::Bank& bank = bankFor(t.key(), st.data);
  const int selT = ival(st, TgTimeSlot), selV = ival(st, TgVolSlot);
  if (!ui.init) {  // open on the slot that is playing (time first)
    ui.init = true;
    if (selT > 0) ui.edit[0] = selT - 1;
    if (selV > 0) ui.edit[1] = selV - 1;
    ui.volPage = selT <= 0 && selV > 0;
  }

  // ---------------------------------------------------------------- kit
  const float k = std::clamp(width / 1000.0f, 0.8f, 2.0f);
  Kit c{a, t, ImGui::GetWindowDrawList(), theme::pal(), k, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {}};
  const theme::Palette& P = c.P;
  c.laneT = theme::litRole(0x86C964);
  c.laneV = theme::litRole(0xFFAA55);
  c.card = P.dark ? P.well : P.surface900;
  c.cell = P.dark ? theme::mix(P.well, P.surface800, 0.55f) : P.surface800;
  c.face = P.dark ? P.surface800 : P.surface800;
  c.onText = P.dark ? P.paper : P.text;
  c.fBody = std::clamp(12.5f * k, 11.0f, 26.0f);
  c.fSmall = std::clamp(11.0f * k, 10.0f, 22.0f);
  c.fCaps = std::clamp(10.5f * k, 9.5f, 21.0f);
  c.fBig = std::clamp(14.0f * k, 12.0f, 28.0f);
  {
    sk::Skin& s = c.knob;
    s.panel = P.surface900, s.panelLo = P.well, s.panelHi = P.surface800;
    s.rail = P.dark ? P.field : P.surface600;
    s.fill = P.accent, s.fillPressed = sk::lighten(P.accent, 0.2f);
    s.cap = P.dark ? P.surface700 : P.surface800;
    s.pointer = P.text;
    s.text = P.text, s.textDim = P.textDim;
    s.lcdBg = P.field, s.lcdText = P.text;
    s.well = P.well;
    s.select = P.accent;
  }
  ImDrawList* dl = c.dl;
  const float u = width / k;  // width in design px
  const bool narrow = u < 860.0f, wide = u >= 1300.0f;

  // Live state from the engine.
  float tPos = -1, vPos = -1;
  int playT = 0, playV = 0;
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
      tPos = rt->bands[0].load(), playT = static_cast<int>(rt->bands[1].load()), playV = static_cast<int>(rt->bands[2].load());
      vPos = rt->bands[3].load();
    }
  const bool refEngine = bval(st, TgModel);

  // ---------------------------------------------------------------- frame
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float pad = c.px(10), gap = c.px(8);
  const float headH = c.px(64), bodyH = c.px(566);
  const float H = pad + headH + gap + bodyH + pad;
  const ImVec2 E0 = o, E1(o.x + width, o.y + H);
  audit::frame(E0, E1, "form");
  sk::noteFrame(E0, E1);
  dl->AddRectFilled(E0, E1, P.surface900, c.px(6));

  // ---------------------------------------------------------------- header
  const ImVec2 h0(o.x + pad, o.y + pad), h1(o.x + width - pad, o.y + pad + headH);
  card(c, h0, h1, c.card, "header");
  {
    const float cy = (h0.y + h1.y) * 0.5f;
    float x = h0.x + c.px(12);
    // HOLD pad (bar repeat).
    {
      const bool on = bval(st, TgHold);
      const ImU32 g = theme::litRole(0x9EE04A);
      const ImVec2 p0(x, cy - c.px(21)), p1(x + c.px(96), cy + c.px(21));
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID("hold");
      if (ImGui::InvisibleButton("##h", ImVec2(p1.x - p0.x, p1.y - p0.y))) a.doc.setParam(t.pid(TgHold), on ? 0 : 1, false);
      const bool hov = ImGui::IsItemHovered();
      if (hov) a.setHint(i18n::tr("Hold: repeat the current bar while on (click to toggle)"));
      ImGui::PopID();
      audit::control(p0, p1, "hold");
      const float r = c.px(6);
      if (on) {
        dl->AddRectFilled(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), theme::withAlpha(g, 0.25f), r + 3);
        sk::gradientRect(dl, p0, p1, sk::lighten(g, 0.22f), sk::darken(g, 0.10f), r);
        dl->AddLine(ImVec2(p0.x + r, p0.y + 1), ImVec2(p1.x - r, p0.y + 1), IM_COL32(255, 255, 255, 120), 1.0f);
      } else {
        sk::insetField(dl, p0, p1, theme::mix(P.field, g, hov ? 0.14f : 0.07f), r);
        dl->AddRect(p0, p1, theme::withAlpha(g, hov ? 0.85f : 0.55f), r, 0, 1.2f);
      }
      ImGui::PushFont(fBold(), c.fBig);
      const char* hl = i18n::tr("HOLD");
      const ImVec2 ts = ImGui::CalcTextSize(hl);
      ImGui::PopFont();
      const ImVec2 tp(std::round((p0.x + p1.x - ts.x) * 0.5f), std::round(cy - ts.y * 0.5f));
      audit::control(tp, ImVec2(tp.x + ts.x, tp.y + ts.y), "text");
      dl->AddText(fBold(), c.fBig, tp, on ? IM_COL32(0x12, 0x22, 0x08, 255) : g, hl);
      x = p1.x + c.px(20);
    }
    // Mix dials.
    const float ds = c.px(42);
    dial(c, TgTimeMix, ImVec2(x, cy - ds * 0.5f), ds, c.laneT, i18n::tr("TIME MIX"), true, c.px(118));
    x += c.px(126);
    dial(c, TgVolMix, ImVec2(x, cy - ds * 0.5f), ds, c.laneV, i18n::tr("VOLUME MIX"), true, c.px(126));
    x += c.px(138);
    dl->AddLine(ImVec2(x, h0.y + c.px(12)), ImVec2(x, h1.y - c.px(12)), P.dark ? P.line : P.lineStrong, 1.0f);
    x += c.px(12);
    // Right group: note triggers + options.
    const float optW = c.px(100), ledW = c.px(132);
    const ImVec2 o1(h1.x - c.px(12), cy + c.px(15)), o0(o1.x - optW, cy - c.px(15));
    if (button(c, "opts", o0, o1, ui.options, P.accent, ui::Icon::Settings, true, i18n::tr("Options"),
               i18n::tr("Options: triggering, sound, quick effect and step gate, engine")))
      ui.options = !ui.options;
    const ImVec2 l1(o0.x - c.px(8), cy + c.px(14)), l0(l1.x - ledW, cy - c.px(14));
    ledParam(c, TgMidiTrigger, l0, l1, i18n::tr("Note triggers"));
    // "Now" chips: the chosen time and volume slots, lit while playing; click opens the slot.
    const float chipsX1 = l0.x - c.px(10);
    const float cw = std::max(c.px(60), (chipsX1 - x - c.px(8)) * 0.5f);
    for (int L = 0; L < 2 && cw >= c.px(120); ++L) {  // no room for the chips: the banks show the same
      const bool vol = L == 1;
      const int sel = vol ? selV : selT, play = vol ? playV : playT;
      const ImVec2 p0(x + L * (cw + c.px(8)), cy - c.px(21)), p1(p0.x + cw, cy + c.px(21));
      ImGui::PushID(vol ? "chipV" : "chipT");
      ImGui::SetCursorScreenPos(p0);
      const bool clicked = ImGui::InvisibleButton("##c", ImVec2(std::max(1.f, p1.x - p0.x), p1.y - p0.y));
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      if (hov) a.setHint(vol ? i18n::tr("Volume slot playing now (click to edit it)") : i18n::tr("Time slot playing now (click to edit it)"));
      if (clicked && sel > 0) ui.volPage = vol, ui.edit[L] = sel - 1, ui.options = false;
      audit::control(p0, p1, "chip");
      const ImU32 ln = c.lane(vol);
      const bool playing = sel > 0 && play == sel;
      sk::insetField(dl, p0, p1, hov ? sk::lighten(P.field, P.dark ? 0.04f : 0.0f) : P.field, c.px(5));
      dl->AddRect(p0, p1, playing ? theme::withAlpha(ln, 0.9f) : (P.dark ? P.line : P.lineStrong), c.px(5), 0, playing ? 1.5f : 1.0f);
      if (sel > 0) {
        const tg::Slot& s = vol ? bank.vol[sel - 1] : bank.time[sel - 1];
        miniEnv(c, s, ImVec2(p1.x - c.px(64), p0.y + c.px(8)), ImVec2(p1.x - c.px(8), p1.y - c.px(8)), ln, 0.16f, 0.75f);
      }
      const float tx = p0.x + c.px(10), tx1 = p1.x - (sel > 0 ? c.px(72) : c.px(8));
      dl->AddCircleFilled(ImVec2(tx + c.px(3.5f), p0.y + c.px(12.5f)), c.px(3.5f), playing ? ln : theme::withAlpha(ln, 0.55f), 12);
      caps(c, ImVec2(tx + c.px(11), p0.y + c.px(5)), ImVec2(tx1, p0.y + c.px(20)), vol ? i18n::tr("VOLUME") : i18n::tr("TIME"), ln);
      char lb[48];
      if (sel > 0) {
        const tg::Slot& s = vol ? bank.vol[sel - 1] : bank.time[sel - 1];
        std::snprintf(lb, sizeof lb, "%d  %s", sel, s.name[0] ? s.name : "");
      } else {
        std::snprintf(lb, sizeof lb, "%s", i18n::tr("Off"));
      }
      text(c, fBold(), c.fBody, ImVec2(tx, p0.y + c.px(20)), ImVec2(tx1, p1.y - c.px(3)), sel > 0 ? P.text : P.textFaint, lb);
    }
  }

  // ---------------------------------------------------------------- body geometry
  const float bodyY0 = h1.y + gap, bodyY1 = bodyY0 + bodyH;
  const float leftW = std::round((narrow ? 0.42f : wide ? 0.40f : 0.37f) * (width - 2 * pad - gap));
  const ImVec2 L0(o.x + pad, bodyY0), L1(L0.x + leftW, bodyY1);
  const ImVec2 R0(L1.x + gap, bodyY0), R1(o.x + width - pad, bodyY1);

  // Volume smoothing dials (attack / release / tension): the rail beside the
  // envelope, or a row under the bank when the editor is narrow.
  auto smoothingDials = [&](ImVec2 p0, bool vertical, float span) {
    const float ds = c.px(42);
    const int pa = refEngine ? TgRAttack : TgVolAttack, pr = refEngine ? TgRRelease : TgVolRelease;
    const int ps[3] = {pa, pr, TgTension};
    const char* nm[3] = {i18n::tr("ATTACK"), i18n::tr("RELEASE"), i18n::tr("TENSION")};
    for (int i = 0; i < 3; ++i) {
      const ImVec2 tl = vertical ? ImVec2(p0.x - ds * 0.5f, p0.y + i * span) : ImVec2(p0.x + i * span - ds * 0.5f, p0.y);
      dial(c, ps[i], tl, ds, c.laneV, nm[i], false, std::min(span, c.px(78)));
    }
  };

  // ---------------------------------------------------------------- slot banks
  card(c, L0, L1, c.card, "banks");
  {
    const int cols = wide ? 6 : 4, rows = (tg::kSlots + cols - 1) / cols;
    const int banks = narrow ? 1 : 2;
    const float ip = c.px(10), hdrH = c.px(24), cg = c.px(3), bankGap = c.px(10);
    const float smoothH = narrow ? c.px(104) : 0.f;  // narrow: the smoothing dials sit under the bank
    const float bankH = (L1.y - L0.y - 2 * ip - smoothH - (banks - 1) * bankGap) / banks;
    if (narrow) {
      const float y = L1.y - ip - smoothH;
      dl->AddLine(ImVec2(L0.x + ip, y + c.px(4)), ImVec2(L1.x - ip, y + c.px(4)), theme::withAlpha(P.textDim, 0.2f), 1.0f);
      caps(c, ImVec2(L0.x + ip, y + c.px(6)), ImVec2(L1.x - ip, y + c.px(20)), i18n::tr("VOLUME SMOOTHING"), c.laneV);
      const float span = (L1.x - L0.x - 2 * ip) / 3.0f;
      smoothingDials(ImVec2(L0.x + ip + span * 0.5f, y + c.px(24)), false, span);
    }
    for (int b = 0; b < banks; ++b) {
      const bool vol = narrow ? ui.volPage : b == 1;
      const ImU32 ln = c.lane(vol);
      const int sel = vol ? selV : selT, play = vol ? playV : playT;
      const ImVec2 b0(L0.x + ip, L0.y + ip + b * (bankH + bankGap)), b1(L1.x - ip, b0.y + bankH);
      // Bank header: lane tick, title, the chosen slot, previews toggle (first bank).
      {
        dl->AddRectFilled(ImVec2(b0.x, b0.y + c.px(6)), ImVec2(b0.x + c.px(3), b0.y + hdrH - c.px(6)), ln, c.px(1.5f));
        const float tw = text(c, fBold(), c.fBody, ImVec2(b0.x + c.px(9), b0.y), ImVec2(b1.x, b0.y + hdrH - c.px(2)), P.text,
                              vol ? i18n::tr("Volume slots") : i18n::tr("Time slots"));
        float rx = b1.x;
        if (b == 0) {
          const float bs = hdrH - c.px(4);
          if (button(c, "prev", ImVec2(rx - bs, b0.y + c.px(1)), ImVec2(rx, b0.y + bs + c.px(1)), ui.previews, P.accent,
                     ui.previews ? ui::Icon::Eye : ui::Icon::EyeOff, true, nullptr, i18n::tr("Show the slot envelopes in the cells")))
            ui.previews = !ui.previews;
          rx -= bs + c.px(8);
        }
        char sl[48];
        if (sel > 0) std::snprintf(sl, sizeof sl, "%s %d", i18n::tr("playing"), sel);
        else std::snprintf(sl, sizeof sl, "%s", i18n::tr("off"));
        text(c, fMono(), c.fSmall, ImVec2(b0.x + c.px(9) + tw + c.px(10), b0.y), ImVec2(rx, b0.y + hdrH - c.px(2)), sel > 0 ? ln : P.textFaint, sl, 1);
      }
      const float gy0 = b0.y + hdrH + c.px(2);
      const float cw = (b1.x - b0.x - (cols - 1) * cg) / cols, ch = (b1.y - gy0 - (rows - 1) * cg) / rows;
      ImGui::PushID(vol ? "vgrid" : "tgrid");
      for (int s = 0; s < tg::kSlots; ++s) {
        const int cx = s % cols, cyi = s / cols;
        const ImVec2 a0(std::round(b0.x + cx * (cw + cg)), std::round(gy0 + cyi * (ch + cg)));
        const ImVec2 a1(std::round(b0.x + cx * (cw + cg) + cw), std::round(gy0 + cyi * (ch + cg) + ch));
        ImGui::PushID(s);
        ImGui::SetCursorScreenPos(a0);
        const bool clicked = ImGui::InvisibleButton("##s", ImVec2(std::max(1.f, a1.x - a0.x), std::max(1.f, a1.y - a0.y)));
        const bool hov = ImGui::IsItemHovered();
        const tg::Slot& sl = vol ? bank.vol[s] : bank.time[s];
        if (clicked) {
          a.doc.setParam(t.pid(vol ? TgVolSlot : TgTimeSlot), sel == s + 1 ? 0 : s + 1, false);
          ui.volPage = vol, ui.edit[vol ? 1 : 0] = s, ui.options = false;
        }
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          ui.volPage = vol, ui.edit[vol ? 1 : 0] = s;
          ImGui::OpenPopup("##slotm");
        }
        if (ImGui::BeginPopup("##slotm")) {
          if (ImGui::MenuItem(i18n::tr("Reset"))) {
            PluginState ns = st;
            ns.data = tg::resetSlot(st.data, vol ? 'V' : 'T', s);
            setPluginState(a, t, ns, "reset slot");
          }
          if (ImGui::MenuItem(i18n::tr("Edit slot name..."))) {
            std::snprintf(ui.rename, sizeof ui.rename, "%s", sl.name);
            ui.renameBank = vol ? 1 : 0, ui.renameSlot = s;
          }
          auto move = [&](int to) {
            const tg::Slot& other = vol ? bank.vol[to] : bank.time[to];
            PluginState ns = st;
            ns.data = tg::replaceSlot(tg::replaceSlot(st.data, vol ? 'V' : 'T', to, sl), vol ? 'V' : 'T', s, other);
            setPluginState(a, t, ns, "move slot");
            ui.edit[vol ? 1 : 0] = to;
          };
          if (ImGui::MenuItem(i18n::tr("Move slot left"), nullptr, false, s > 0)) move(s - 1);
          if (ImGui::MenuItem(i18n::tr("Move slot right"), nullptr, false, s + 1 < tg::kSlots)) move(s + 1);
          ImGui::EndPopup();
        }
        const bool on = sel == s + 1, playing = play == s + 1 && on, shown = ui.volPage == vol && ui.edit[vol ? 1 : 0] == s && !ui.options;
        audit::control(a0, a1, "slot");
        const float r = c.px(3);
        ImU32 face = on ? theme::mix(c.cell, ln, P.dark ? 0.40f : 0.34f) : c.cell;
        if (hov) face = sk::lighten(face, P.dark ? 0.07f : 0.4f);
        sk::gradientRect(dl, a0, a1, sk::lighten(face, P.dark ? 0.04f : 0.2f), face, r);
        const float chh = a1.y - a0.y;
        const float nameY1 = ui.previews ? a0.y + chh * 0.64f : a1.y;  // name above, envelope band underneath
        if (ui.previews)
          miniEnv(c, sl, ImVec2(a0.x + c.px(on ? 8 : 6), a0.y + chh * 0.62f), ImVec2(a1.x - c.px(5), a1.y - c.px(3)), on ? c.onText : ln, 0.10f,
                  on ? 0.55f : 0.6f);
        if (on) dl->AddRectFilled(a0, ImVec2(a0.x + std::max(2.0f, c.px(3)), a1.y), ln, r, ImDrawFlags_RoundCornersLeft);
        if (playing) dl->AddRect(a0, a1, ln, r, 0, std::max(1.5f, c.px(1.5f)));
        if (shown) dl->AddRect(ImVec2(a0.x + 1.5f, a0.y + 1.5f), ImVec2(a1.x - 1.5f, a1.y - 1.5f), theme::withAlpha(P.text, 0.85f), r, 0, 1.0f);
        char lb[40];
        if (sl.name[0]) std::snprintf(lb, sizeof lb, "%s", sl.name);
        else std::snprintf(lb, sizeof lb, "%d", s + 1);
        const ImU32 tc = on ? c.onText : sl.name[0] ? P.text : P.textFaint;
        text(c, sl.name[0] ? fReg() : fMono(), std::min(c.fSmall, chh * 0.5f), ImVec2(a0.x + c.px(on ? 8 : 6), a0.y + c.px(1)),
             ImVec2(a1.x - c.px(4), nameY1), tc, lb);
        if (hov)
          a.setHint(std::string(vol ? i18n::tr("Volume slot") : i18n::tr("Time slot")) + " " + std::to_string(s + 1) +
                    (sl.name[0] ? std::string(": ") + sl.name : "") + "   (" + i18n::tr("click: play and edit · click again: off · right-click: menu") + ")");
        ImGui::PopID();
      }
      ImGui::PopID();
    }
  }
  if (ui.renameSlot >= 0) ImGui::OpenPopup("##gbrename");
  if (ImGui::BeginPopup("##gbrename")) {
    ImGui::TextDisabled("%s", i18n::tr("Slot name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##nm", ui.rename, sizeof ui.rename, ImGuiInputTextFlags_EnterReturnsTrue)) {
      bool vb = ui.renameBank == 1;
      tg::Slot s = vb ? bank.vol[ui.renameSlot] : bank.time[ui.renameSlot];
      s.setName(ui.rename);
      commitSlot(a, t, st, vb, ui.renameSlot, s, "rename slot");
      ui.renameSlot = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else {
    ui.renameSlot = -1;
  }

  // ---------------------------------------------------------------- right card
  card(c, R0, R1, c.card, "editor");
  const float ip = c.px(10);
  const ImVec2 r0(R0.x + ip, R0.y + ip), r1(R1.x - ip, R1.y - ip);

  if (ui.options) {
    // ------------------------------------------------------------ options panel
    const float rowH = c.px(30), capH = c.px(24);
    {
      text(c, fBold(), c.fBig, ImVec2(r0.x, r0.y), ImVec2(r1.x - c.px(100), r0.y + c.px(30)), P.text, i18n::tr("Options"));
      if (button(c, "done", ImVec2(r1.x - c.px(84), r0.y + c.px(1)), ImVec2(r1.x, r0.y + c.px(29)), false, P.accent, ui::Icon::Close, true,
                 i18n::tr("Done"), i18n::tr("Back to the envelope")))
        ui.options = false;
    }
    const float colGap = c.px(18);
    const float colW = (r1.x - r0.x - colGap) * 0.5f;
    auto section = [&](float x, float& y, const char* title) {
      dl->AddLine(ImVec2(x, y + capH - c.px(4)), ImVec2(x + colW, y + capH - c.px(4)), theme::withAlpha(P.textDim, 0.25f), 1.0f);
      caps(c, ImVec2(x, y), ImVec2(x + colW, y + capH - c.px(6)), title);
      y += capH + c.px(2);
    };
    auto labelled = [&](float x, float y, const char* label) {
      text(c, fReg(), c.fBody, ImVec2(x + c.px(4), y), ImVec2(x + colW * 0.48f, y + rowH), P.textDim, label);
      return ImVec2(x + colW * 0.5f, y + c.px(2));
    };
    // Column 1: triggering + sound.
    {
      float x = r0.x, y = r0.y + c.px(44);
      section(x, y, i18n::tr("TRIGGERING"));
      ledParam(c, TgMidiTrigger, ImVec2(x, y), ImVec2(x + colW, y + rowH), i18n::tr("Trigger slots from notes"));
      y += rowH + c.px(2);
      ledParam(c, TgFreeRun, ImVec2(x, y), ImVec2(x + colW, y + rowH), i18n::tr("Run when stopped"));
      y += rowH + c.px(6);
      {
        const ImVec2 f0 = labelled(x, y, i18n::tr("Default trigger sync"));
        dropParam(c, TgTrigSync, f0, ImVec2(x + colW, y + rowH - c.px(2)));
      }
      y += rowH + c.px(18);
      section(x, y, i18n::tr("SOUND"));
      {
        const ImVec2 f0 = labelled(x, y, i18n::tr("Click reduction"));
        dropParam(c, TgDeclick, f0, ImVec2(x + colW, y + rowH - c.px(2)));
      }
      y += rowH + c.px(6);
      ledParam(c, TgCompensate, ImVec2(x, y), ImVec2(x + colW, y + rowH), i18n::tr("Smoothing attack compensation"));
      y += rowH + c.px(2);
      ledParam(c, TgDcRemove, ImVec2(x, y), ImVec2(x + colW, y + rowH), i18n::tr("Remove DC offset"));
      y += rowH + c.px(2);
      ledParam(c, TgHq, ImVec2(x, y), ImVec2(x + colW, y + rowH), i18n::tr("HQ resampling"));
    }
    // Column 2: without a slot + engine.
    {
      const float x = r0.x + colW + colGap;
      float y = r0.y + c.px(44);
      section(x, y, i18n::tr("WITHOUT A SLOT"));
      {
        const ImVec2 f0 = labelled(x, y, i18n::tr("Quick time effect"));
        dropParam(c, TgMode, f0, ImVec2(x + colW, y + rowH - c.px(2)));
      }
      y += rowH + c.px(6);
      ledParam(c, TgGateOn, ImVec2(x, y), ImVec2(x + colW * 0.6f, y + rowH), i18n::tr("Step gate"));
      y += rowH + c.px(4);
      sk::stepGrid(a, t, TgGate, kGateSteps, ImVec2(x + c.px(4), y), ImVec2(colW - c.px(8), c.px(26)), [&] {
        sk::Skin s = c.knob;
        s.fill = c.laneV, s.panelLo = P.field, s.text = P.text;
        return s;
      }());
      y += c.px(40);
      const float ds = c.px(38);
      dial(c, TgSmooth, ImVec2(x + c.px(30), y), ds, P.accent, i18n::tr("SMOOTHING"), false, c.px(90));
      dial(c, TgMix, ImVec2(x + c.px(150), y), ds, P.accent, i18n::tr("MIX"), false, c.px(90));
      y += ds + c.px(42);
      section(x, y, i18n::tr("ENGINE"));
      {
        const ImVec2 f0 = labelled(x, y, i18n::tr("Engine"));
        dropParam(c, TgModel, f0, ImVec2(x + colW, y + rowH - c.px(2)));
      }
      y += rowH + c.px(10);
      dial(c, refEngine ? TgRScratch : TgScratch, ImVec2(x + c.px(30), y), ds, P.accent, i18n::tr("SCRATCH"), false, c.px(90));
    }
  } else {
    // ------------------------------------------------------------ slot editor
    const bool vol = ui.volPage;
    const ImU32 ln = c.lane(vol);
    const int ks = std::clamp(ui.edit[vol ? 1 : 0], 0, tg::kSlots - 1);
    tg::Slot slot = vol ? bank.vol[ks] : bank.time[ks];
    bool slotChanged = false;
    const float rowA = c.px(32), rowB = c.px(30);
    // Row A: page switch, slot name, length.
    {
      const float segW = narrow ? c.px(150) : c.px(184);
      const ImVec2 s0 = r0, s1(r0.x + segW, r0.y + rowA);
      sk::insetField(dl, s0, s1, P.field, c.px(5));
      for (int i = 0; i < 2; ++i) {
        const bool on = vol == (i == 1);
        const ImVec2 a0(s0.x + c.px(2) + i * (segW - c.px(4)) * 0.5f, s0.y + c.px(2)), a1(a0.x + (segW - c.px(4)) * 0.5f, s1.y - c.px(2));
        ImGui::PushID(450 + i);
        ImGui::SetCursorScreenPos(a0);
        if (ImGui::InvisibleButton("##pg", ImVec2(a1.x - a0.x, a1.y - a0.y))) ui.volPage = i == 1;
        const bool hov = ImGui::IsItemHovered();
        if (hov) a.setHint(i == 0 ? i18n::tr("Edit the time envelopes") : i18n::tr("Edit the volume envelopes"));
        ImGui::PopID();
        audit::control(a0, a1, "page");
        const ImU32 lc = c.lane(i == 1);
        if (on) {
          sk::gradientRect(dl, a0, a1, theme::mix(c.face, lc, P.dark ? 0.42f : 0.30f), theme::mix(c.face, lc, P.dark ? 0.30f : 0.22f), c.px(4));
          dl->AddRect(a0, a1, theme::withAlpha(lc, 0.9f), c.px(4), 0, 1.0f);
        } else if (hov) {
          dl->AddRectFilled(a0, a1, theme::withAlpha(P.text, 0.07f), c.px(4));
        }
        const char* lb = i == 0 ? i18n::tr("TIME") : i18n::tr("VOLUME");
        ImGui::PushFont(fMonoBold(), c.fSmall);
        const float tw = ImGui::CalcTextSize(lb).x;
        ImGui::PopFont();
        const float dot = c.px(7), tot = dot + c.px(7) + tw;
        const float x = std::round((a0.x + a1.x - tot) * 0.5f), cy = (a0.y + a1.y) * 0.5f;
        dl->AddCircleFilled(ImVec2(x + dot * 0.5f, cy), dot * 0.5f, on ? lc : theme::withAlpha(lc, 0.6f), 12);
        text(c, fMonoBold(), c.fSmall, ImVec2(x + dot + c.px(7), a0.y), ImVec2(a1.x, a1.y), on ? c.onText : P.textDim, lb);
      }
      // Length (right).
      const float lenW = narrow ? c.px(84) : c.px(128);
      const ImVec2 n1(r1.x - lenW - c.px(8), r0.y + rowA);
      {
        static const float lens[5] = {1, 2, 4, 8, 16};
        static const char* names[5] = {i18n::N_("1 beat"), i18n::N_("2 beats"), i18n::N_("1 bar"), i18n::N_("2 bars"), i18n::N_("4 bars")};
        int cur = -1;
        for (int i = 0; i < 5; ++i)
          if (std::fabs(slot.len - lens[i]) < 1e-3f) cur = i;
        char lt[40];
        std::snprintf(lt, sizeof lt, "%s%s%s", narrow ? "" : i18n::tr("Length"), narrow ? "" : " ", cur >= 0 ? i18n::tr(names[cur]) : "-");
        if (dropFace(c, "len", ImVec2(r1.x - lenW, r0.y), ImVec2(r1.x, r0.y + rowA), lt, i18n::tr("Slot length: the envelope is stretched to it")))
          ImGui::OpenPopup("##lenp");
        if (ImGui::BeginPopup("##lenp")) {
          for (int i = 0; i < 5; ++i)
            if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, i == cur) && i != cur) {
              const float sc = lens[i] / slot.len;
              for (int j = 0; j < slot.env.n; ++j) slot.env.x[j] *= sc;
              slot.len = lens[i];
              slotChanged = true;
            }
          ImGui::EndPopup();
        }
      }
      // Slot name (click to rename).
      {
        const ImVec2 n0(s1.x + c.px(8), r0.y);
        ImGui::SetCursorScreenPos(n0);
        ImGui::PushID("name");
        if (ImGui::InvisibleButton("##n", ImVec2(std::max(1.f, n1.x - n0.x), n1.y - n0.y))) {
          std::snprintf(ui.rename, sizeof ui.rename, "%s", slot.name);
          ui.renameBank = vol ? 1 : 0, ui.renameSlot = ks;
        }
        const bool hov = ImGui::IsItemHovered();
        if (hov) a.setHint(i18n::tr("Click to rename the slot"));
        ImGui::PopID();
        audit::control(n0, n1, "name");
        sk::insetField(dl, n0, n1, P.field, c.px(5));
        dl->AddRect(n0, n1, hov ? P.lineStrong : (P.dark ? P.line : P.lineStrong), c.px(5), 0, 1.0f);
        char badge[8];
        std::snprintf(badge, sizeof badge, "%s%d", vol ? "V" : "T", ks + 1);
        ImGui::PushFont(fMonoBold(), c.fSmall);
        const float bw = ImGui::CalcTextSize(badge).x + c.px(12);
        ImGui::PopFont();
        const ImVec2 bb0(n0.x + c.px(5), n0.y + c.px(5)), bb1(bb0.x + bw, n1.y - c.px(5));
        dl->AddRectFilled(bb0, bb1, theme::withAlpha(ln, P.dark ? 0.85f : 0.9f), c.px(3));
        text(c, fMonoBold(), c.fSmall, bb0, bb1, P.dark ? IM_COL32(0x14, 0x18, 0x1A, 255) : P.paper, badge, 0);
        const float pe = hov ? c.px(22) : 0;
        text(c, fBold(), c.fBody, ImVec2(bb1.x + c.px(9), n0.y), ImVec2(n1.x - c.px(8) - pe, n1.y), slot.name[0] ? P.text : P.textFaint,
             slot.name[0] ? slot.name : i18n::tr("Unnamed slot"));
        if (hov) ui::drawIcon(dl, ui::Icon::Draw, ImVec2(n1.x - c.px(14), (n0.y + n1.y) * 0.5f), c.px(13), P.textDim);
      }
    }
    // Row B: tool bar.
    const float tbY = r0.y + rowA + c.px(8);
    {
      float x = r0.x;
      const ImU32 snapCol = theme::litRole(theme::role::kSnap);
      // Snap: magnet + drop-down.
      {
        static const char* snaps[7] = {i18n::N_("Off"), "1/16", "1/12", "1/8", "1/6", "1/4", "1/3"};
        const ImVec2 p0(x, tbY), p1(x + c.px(118), tbY + rowB);
        char v[32];
        std::snprintf(v, sizeof v, "%s %s", i18n::tr("Snap"), ui.snap == 0 ? i18n::tr("off") : snaps[ui.snap]);
        if (dropFace(c, "snap", p0, p1, v, i18n::tr("Snap: points move on a grid of this many cells per beat"), ui.snap ? snapCol : 0))
          ImGui::OpenPopup("##snapp");
        if (ImGui::BeginPopup("##snapp")) {
          for (int i = 0; i < 7; ++i) {
            char it[32];
            if (i == 0) std::snprintf(it, sizeof it, "%s", i18n::tr("Off"));
            else std::snprintf(it, sizeof it, "%s %s", snaps[i], i18n::tr("beat"));
            if (ImGui::MenuItem(it, nullptr, ui.snap == i)) ui.snap = i;
          }
          ImGui::EndPopup();
        }
        x = p1.x + c.px(8);
      }
      auto tool = [&](const char* id, ui::Icon ic, const char* label, bool& on, const char* tip, float w) {
        if (narrow) w = rowB * 1.25f, label = nullptr;  // icon only (the tooltip names it)
        if (button(c, id, ImVec2(x, tbY), ImVec2(x + w, tbY + rowB), on, ln, ic, true, label, tip)) on = !on;
        x += w + c.px(6);
      };
      tool("step", ui::Icon::StepEdit, i18n::tr("Step"), ui.step, i18n::tr("Step editing: drag across the graph to draw flat steps of the snap size"),
           c.px(76));
      tool("slide", ui::Icon::StretchMode, i18n::tr("Slide"), ui.slide, i18n::tr("Slide: moving a point moves every later point with it"), c.px(78));
      tool("freeze", ui::Icon::Lock, i18n::tr("Freeze"), ui.freeze, i18n::tr("Freeze: lock the envelope against edits"), c.px(86));
      // Edit menu (right).
      const ImVec2 m1(r1.x, tbY + rowB), m0(r1.x - (narrow ? rowB * 1.25f : c.px(92)), tbY);
      if (button(c, "envm", m0, m1, false, ln, ui::Icon::Menu, true, narrow ? nullptr : i18n::tr("Edit"), i18n::tr("Envelope: copy, paste, clear, flip, reverse, factory content")))
        ImGui::OpenPopup("##gbenv");
      if (ImGui::BeginPopup("##gbenv")) {
        if (ImGui::MenuItem(i18n::tr("Copy slot"))) ui.clip = tg::serialize(vol ? 'V' : 'T', ks, slot);
        if (ImGui::MenuItem(i18n::tr("Paste slot"), nullptr, false, !ui.clip.empty())) {
          auto tmp = std::make_unique<tg::Bank>();
          std::string line = "GB2\n" + std::string(1, vol ? 'V' : 'T') + std::to_string(ks + 1) + ui.clip.substr(ui.clip.find(' '));
          tg::factory(*tmp);
          tg::parse(line.c_str(), *tmp);
          slot = vol ? tmp->vol[ks] : tmp->time[ks];
          slotChanged = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Clear"))) slot.flat(1.f), slotChanged = true;
        if (ImGui::MenuItem(i18n::tr("Flip vertically"))) {
          for (int i = 0; i < slot.env.n; ++i) slot.env.y[i] = 1.f - slot.env.y[i], slot.env.t[i] = -slot.env.t[i];
          slotChanged = true;
        }
        if (ImGui::MenuItem(i18n::tr("Reverse"))) {
          tg::Slot r = slot;
          r.env.clear();
          for (int i = slot.env.n - 1; i >= 0; --i) {
            uint8_t m = i + 1 < slot.env.n ? slot.env.mode[i + 1] : static_cast<uint8_t>(menv::SingleCurve);
            float tt = i + 1 < slot.env.n ? -slot.env.t[i + 1] : 0.f;
            r.env.add(slot.len - slot.env.x[i], slot.env.y[i], i == slot.env.n - 1 ? 0.f : tt,
                      i == slot.env.n - 1 ? static_cast<uint8_t>(menv::SingleCurve) : m);
          }
          slot = r;
          slotChanged = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Factory content"))) {
          PluginState ns = st;
          ns.data = tg::resetSlot(st.data, vol ? 'V' : 'T', ks);
          setPluginState(a, t, ns, "reset slot");
        }
        ImGui::EndPopup();
      }
    }
    // Envelope + smoothing rail + slot footer.
    const float footH = c.px(54);
    const float railW = narrow ? 0.f : c.px(84);
    const float gutter = c.px(38);
    const float envTop = tbY + rowB + c.px(10);
    const float envBot = r1.y - footH - c.px(10);
    const ImVec2 e0(r0.x + gutter, envTop), e1(r1.x - (railW > 0 ? railW + c.px(10) : 0), envBot);
    {
      envui::Style es;
      es.bg = theme::mix(P.field, ln, P.dark ? 0.05f : 0.035f);
      es.grid = theme::withAlpha(P.text, P.dark ? 0.05f : 0.07f);
      es.gridStrong = theme::withAlpha(P.text, P.dark ? 0.14f : 0.18f);
      es.line = ln;
      es.fill = theme::withAlpha(ln, P.dark ? 0.20f : 0.24f);
      es.point = P.dark ? IM_COL32(0xF2, 0xF4, 0xF6, 255) : P.paper;
      es.pointHot = ln;
      es.handle = theme::withAlpha(ln, 0.85f);
      es.text = P.text;
      envui::Opts eo;
      eo.xLabels = true;
      // Readout while hovering / dragging (precise editing): position in beats and the lane's value.
      eo.readout = [vol](double x, double y) {
        char b[64];
        const double d = tg::delayBeats(y);
        if (vol) std::snprintf(b, sizeof b, "%s %.2f  ·  %d%%", i18n::tr("beat"), x + 1.0, static_cast<int>(std::lround(y * 100.0)));
        else if (d < 0.005) std::snprintf(b, sizeof b, "%s %.2f  ·  %s", i18n::tr("beat"), x + 1.0, i18n::tr("live"));
        else std::snprintf(b, sizeof b, "%s %.2f  ·  -%.2f %s", i18n::tr("beat"), x + 1.0, d, i18n::tr("beats"));
        return std::string(b);
      };
      eo.xMax = slot.len;
      eo.gridDiv = ui.snap > 0 ? static_cast<int>(kSnapDiv[ui.snap]) : 4;
      eo.gridEvery = eo.gridDiv;
      eo.snapDiv = ui.snap > 0 ? static_cast<int>(kSnapDiv[ui.snap]) : 0;
      eo.yLines = vol ? 4 : 8;
      eo.freeze = ui.freeze, eo.step = ui.step, eo.slide = ui.slide;
      const int play = vol ? playV : playT;
      const float pos = vol ? vPos : tPos;
      eo.playX = play == ks + 1 && pos >= 0 ? std::fmod(static_cast<double>(pos), static_cast<double>(slot.len)) : -1;
      if (!vol)
        eo.extraMenu = [&](int i) {
          if (i <= 0) return false;
          ImGui::TextDisabled("%s", i18n::tr("Pitch"));
          int dir = 0;
          if (ImGui::MenuItem(i18n::tr("1 semitone up"))) dir = 1;
          if (ImGui::MenuItem(i18n::tr("1 semitone down"))) dir = -1;
          ImGui::Separator();
          if (!dir) return false;
          // Re-slope the segment ending at the point: speed = 2^(±1/12) → delay changes by (1 - speed) per beat.
          double speed = std::pow(2.0, dir / 12.0);
          double dx = slot.env.x[i] - slot.env.x[i - 1];
          double d0 = tg::delayBeats(slot.env.y[i - 1]);
          slot.env.y[i] = static_cast<float>(tg::timeY(d0 + (1.0 - speed) * dx));
          slot.env.mode[i] = menv::SingleCurve, slot.env.t[i] = 0;
          return true;
        };
      audit::frame(e0, e1, "graph");
      ImGui::PushFont(fMono(), std::clamp(11.0f * k, 10.0f, 22.0f));  // beat numbers / readout
      const int r = envui::edit("##gbenv", e0, e1, slot.env, ui.env, eo, es);
      ImGui::PopFont();
      if (r & envui::Changed) slotChanged = true;
      if (r & envui::Released) a.doc.endGesture();
      // Axis labels in the gutter: delay (time page) or level (volume page).
      const float Hh = e1.y - e0.y;
      const float lfs = std::clamp(10.0f * k, 9.5f, 20.0f);
      for (int i = 0; i <= 8; i += 2) {
        float y = e1.y - Hh * (1.0f - i / 8.0f);
        char lb[16];
        if (vol) std::snprintf(lb, sizeof lb, "%d%%", 100 - i * 25 / 2);
        else if (i == 0) std::snprintf(lb, sizeof lb, "%s", i18n::tr("live"));
        else std::snprintf(lb, sizeof lb, "-%d", i);
        if (i == 0) y += lfs * 0.55f;
        if (i == 8) y -= lfs * 0.55f;
        text(c, fMono(), lfs, ImVec2(r0.x, y - lfs * 0.7f), ImVec2(e0.x - c.px(6), y + lfs * 0.7f), P.textDim, lb, 1);
      }
    }
    if (railW > 0) {
      const ImVec2 q0(r1.x - railW, envTop), q1(r1.x, envBot);
      audit::frame(q0, q1, "rail");
      sk::noteFrame(q0, q1);
      if (P.dark) {
        sk::insetField(dl, q0, q1, theme::mix(c.card, P.field, 0.5f), c.px(5));
      } else {
        dl->AddRectFilled(q0, q1, P.surface800, c.px(5));
        dl->AddRect(q0, q1, P.line, c.px(5), 0, 1.0f);
      }
      dl->AddRectFilled(ImVec2(q0.x + c.px(8), q0.y + c.px(10)), ImVec2(q1.x - c.px(8), q0.y + c.px(12)), c.laneV, c.px(1));
      caps(c, ImVec2(q0.x + c.px(4), q0.y + c.px(14)), ImVec2(q1.x - c.px(4), q0.y + c.px(30)), i18n::tr("SMOOTHING"), 0, 0);
      const float span = std::min(c.px(118), (q1.y - q0.y - c.px(44)) / 3.0f);
      smoothingDials(ImVec2((q0.x + q1.x) * 0.5f, q0.y + c.px(40)), true, span);
      if (ImGui::IsMouseHoveringRect(q0, ImVec2(q1.x, q0.y + c.px(32))))
        a.setHint(i18n::tr("Volume smoothing: attack, release and tension of the volume envelope"));
    }
    // Slot footer: playback behaviour of the slot being edited.
    {
      const float fy0 = r1.y - footH, fy1 = r1.y;
      dl->AddLine(ImVec2(r0.x, fy0 - c.px(5)), ImVec2(r1.x, fy0 - c.px(5)), theme::withAlpha(P.textDim, 0.2f), 1.0f);
      const float capY1 = fy0 + c.px(16);
      const float cy0 = capY1 + c.px(3), cy1 = fy1 - c.px(1);
      float x = r0.x;
      caps(c, ImVec2(x, fy0), ImVec2(x + c.px(260), capY1), vol ? i18n::tr("VOLUME SLOT") : i18n::tr("TIME SLOT"), ln);
      {
        bool lk = slot.link;
        const float w = narrow ? c.px(40) : c.px(70);
        if (button(c, "link", ImVec2(x, cy0), ImVec2(x + w, cy1), lk && !vol, ln, ui::Icon::LinkSlots, true, narrow ? nullptr : i18n::tr("Link"),
                   vol ? i18n::tr("Link: set on time slots (a time slot can pick the volume slot with the same number)")
                       : i18n::tr("Link: this time slot also picks the volume slot with the same number"),
                   !vol))
          slot.link = !slot.link, slotChanged = true;
        x += w + c.px(6);
      }
      {
        const float w = narrow ? c.px(40) : c.px(92);
        if (button(c, "keyh", ImVec2(x, cy0), ImVec2(x + w, cy1), slot.keyHeld, ln, ui::Icon::Keyboard, true, narrow ? nullptr : i18n::tr("Key-held"),
                   i18n::tr("Key-held: back to the previous slot when the note is released")))
          slot.keyHeld = !slot.keyHeld, slotChanged = true;
        x += w + c.px(6);
      }
      {
        const float w = narrow ? c.px(40) : c.px(94);
        if (button(c, "oneshot", ImVec2(x, cy0), ImVec2(x + w, cy1), slot.oneShot, ln, ui::Icon::ToolPlay, true, narrow ? nullptr : i18n::tr("One-shot"),
                   i18n::tr("One-shot: back to the first slot once the envelope has played")))
          slot.oneShot = !slot.oneShot, slotChanged = true;
        x += w + c.px(14);
      }
      static const char* fh[3] = {i18n::N_("Off"), i18n::N_("Last bar"), i18n::N_("Next bar")};
      static const char* sy[7] = {i18n::N_("Default"), i18n::N_("Off"), i18n::N_("1/4 beat"), i18n::N_("1/2 beat"),
                                  i18n::N_("1 beat"),  i18n::N_("2 beats"), i18n::N_("4 beats")};
      static const char* ps[6] = {i18n::N_("Off"), i18n::N_("1/4 beat"), i18n::N_("1/2 beat"), i18n::N_("1 beat"), i18n::N_("2 beats"), i18n::N_("4 beats")};
      const float cw = std::max(c.px(70), (r1.x - x - 2 * c.px(8)) / 3.0f);
      int f = slot.force, tr = slot.trig + 1, po = slot.pos;
      caps(c, ImVec2(x + c.px(2), fy0), ImVec2(x + cw, capY1), i18n::tr("FORCE HOLD"));
      if (dropFree(c, "force", ImVec2(x, cy0), ImVec2(x + cw, cy1), fh, 3, f, i18n::tr("Force hold: start the bar repeat when this slot starts")))
        slot.force = f, slotChanged = true;
      x += cw + c.px(8);
      caps(c, ImVec2(x + c.px(2), fy0), ImVec2(x + cw, capY1), i18n::tr("TRIGGER SYNC"));
      if (dropFree(c, "trig", ImVec2(x, cy0), ImVec2(x + cw, cy1), sy, 7, tr, i18n::tr("Trigger sync: when a newly picked slot starts")))
        slot.trig = tr - 1, slotChanged = true;
      x += cw + c.px(8);
      caps(c, ImVec2(x + c.px(2), fy0), ImVec2(x + cw, capY1), i18n::tr("POSITION SYNC"));
      if (dropFree(c, "pos", ImVec2(x, cy0), ImVec2(std::min(x + cw, r1.x), cy1), ps, 6, po,
                   i18n::tr("Position sync: where in its envelope a newly started slot begins")))
        slot.pos = po, slotChanged = true;
    }
    if (slotChanged) commitSlot(a, t, st, vol, ks, slot, "edit shuttle slot");
  }

  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(width, H));
}

}  // namespace gl
