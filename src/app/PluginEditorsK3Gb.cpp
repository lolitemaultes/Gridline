// Gridline — Time Gate editor at the reference geometry (824 × 535 form):
// left column (time mix, options, HOLD pad, 36-cell time and volume slot
// grids, volume smoothing / tension / mix wheels), a tool strip (envelope
// menu, snap, step / slide / freeze, time / volume page) and the envelope
// editor with its slot bar (name, link, key-held, one-shot, force hold,
// trigger and position sync).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include "../core/I18n.h"
#include "../core/PluginBrand.h"
#include "../engine/Program.h"
#include "../engine/TimeGateSlots.h"
#include "EnvEditor.h"
#include "LayoutAudit.h"
#include "PluginEditorsCommon.h"
#include "SkinForms.h"
#include "PluginSkin.h"

namespace gl {

namespace {

using namespace ed;
namespace sk = skin;
namespace fm = skin::forms;

struct GbUi {
  bool volPage = false;
  int edit[2] = {0, 0};  // slot shown per page (0-based)
  int snap = 1;          // 0 off, 1 1/16, 2 1/12, 3 1/8, 4 1/6, 5 1/4, 6 1/3 beat
  bool step = false, slide = false, freeze = false, previews = true;
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

const double kSnapDiv[7] = {0, 16, 12, 8, 6, 4, 3};  // cells per beat... (1/16 beat = 16 per beat)

void commitSlot(App& a, const PluginTarget& t, const PluginState& st, bool vol, int k, const tg::Slot& s, const char* what) {
  PluginState ns = st;
  ns.data = tg::replaceSlot(st.data, vol ? 'V' : 'T', k, s);
  setPluginState(a, t, ns, what, 0x4742000000000000ull ^ t.key() ^ static_cast<uint64_t>(k + (vol ? 64 : 0)));
}

void glyphClock(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  float r = std::min(b.x - a.x, b.y - a.y) * 0.42f;
  dl->AddCircle(m, r, c, 16, 1.4f);
  dl->AddLine(m, ImVec2(m.x, m.y - r * 0.7f), c, 1.4f);
  dl->AddLine(m, ImVec2(m.x + r * 0.55f, m.y + r * 0.2f), c, 1.4f);
}
void glyphVolume(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float w = b.x - a.x, h = b.y - a.y;
  dl->AddTriangleFilled(ImVec2(a.x + w * 0.1f, b.y - h * 0.2f), ImVec2(b.x - w * 0.1f, b.y - h * 0.2f), ImVec2(b.x - w * 0.1f, a.y + h * 0.2f), c);
}
void glyphLink(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  float r = std::min(b.x - a.x, b.y - a.y) * 0.22f;
  dl->AddCircle(ImVec2(m.x - r * 0.9f, m.y), r * 1.2f, c, 12, 1.4f);
  dl->AddCircle(ImVec2(m.x + r * 0.9f, m.y), r * 1.2f, c, 12, 1.4f);
}
void glyphKey(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float w = b.x - a.x, h = b.y - a.y;
  dl->AddRect(ImVec2(a.x + w * 0.25f, a.y + h * 0.1f), ImVec2(b.x - w * 0.25f, b.y - h * 0.1f), c, 1.5f, 0, 1.4f);
  dl->AddRectFilled(ImVec2(a.x + w * 0.25f, a.y + h * 0.1f), ImVec2(a.x + w * 0.5f, a.y + h * 0.55f), c);
}
void glyphOneShot(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c) {
  float w = b.x - a.x, h = b.y - a.y;
  float y = (a.y + b.y) * 0.5f;
  dl->AddLine(ImVec2(a.x + w * 0.15f, y), ImVec2(b.x - w * 0.3f, y), c, 1.5f);
  dl->AddTriangleFilled(ImVec2(b.x - w * 0.1f, y), ImVec2(b.x - w * 0.38f, y - h * 0.2f), ImVec2(b.x - w * 0.38f, y + h * 0.2f), c);
  dl->AddLine(ImVec2(a.x + w * 0.15f, y - h * 0.25f), ImVec2(a.x + w * 0.15f, y + h * 0.25f), c, 1.5f);
}

}  // namespace

void editorGrossBeat(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  const PluginState st = *t.state(a.doc.song());
  GbUi& ui = gbUi[t.key()];
  tg::Bank& bank = bankFor(t.key(), st.data);
  sk::Palette pal = sk::paletteFrom(fm::kGrossbeat);
  pal.raised = IM_COL32(0x2A, 0x3A, 0x46, 255);
  pal.fill = IM_COL32(0xFF, 0xB8, 0x70, 255);
  pal.accent2 = IM_COL32(0x86, 0xBF, 0x69, 255);
  sk::View v = sk::begin(fm::kGrossbeat, width, 2.0f, pal, 28.f);  // taller than the design size: bottom controls clear the grids
  const float G = v.growH;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = std::clamp(v.px(9.f), 10.f, 14.f);
  // Live state from the engine.
  float tPos = -1, vPos = -1;
  int playT = 0, playV = 0;
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
      tPos = rt->bands[0].load(), playT = static_cast<int>(rt->bands[1].load()), playV = static_cast<int>(rt->bands[2].load());
      vPos = rt->bands[3].load();
    }
  const int selT = ival(st, TgTimeSlot), selV = ival(st, TgVolSlot);

  // ---------------------------------------------------------------- left column
  // J-93: the header / footer plates are raised slate (were flat light grey), the
  // lanes carry their colour (time green, volume orange) through the whole editor.
  const ImU32 laneT = pal.accent2, laneV = pal.fill;
  {
    const sk::FormCtl& top = v.c("TopBgPaintBox");
    sk::gradientRect(dl, v.p0(top), v.p1(top), sk::col(0x34434E), sk::col(0x263139), 0);
    dl->AddLine(ImVec2(v.p0(top).x, v.p1(top).y - 0.5f), ImVec2(v.p1(top).x, v.p1(top).y - 0.5f), IM_COL32(0, 0, 0, 120), 1.0f);
    const sk::FormCtl& bot = v.c("BottomBgPaintBox");
    sk::gradientRect(dl, v.p0(bot), v.p1(bot), sk::col(0x263139), sk::col(0x34434E), 0);
    dl->AddLine(ImVec2(v.p0(bot).x, v.p0(bot).y + 0.5f), ImVec2(v.p1(bot).x, v.p0(bot).y + 0.5f), IM_COL32(255, 255, 255, 22), 1.0f);
    sk::bevel(v, "SlotsBevel", pal);
    // Title slot: the catalog / brand title (J-94), lane legend underneath.
    const std::string titleStr = pluginTitle("gl.timegate");
    const char* title = titleStr.c_str();
    {
      ImGui::PushFont(app().fontBold ? app().fontBold : app().fontRegular, std::clamp(v.px(15.f), 13.f, 30.f));
      const ImVec2 ts = ImGui::CalcTextSize(title);
      const ImVec2 c0 = v.p(60, 8), c1 = v.p(262, 30);
      dl->AddText(ImVec2(std::round((c0.x + c1.x - ts.x) * 0.5f), std::round((c0.y + c1.y - ts.y) * 0.5f)), IM_COL32(0xEE, 0xF2, 0xF4, 255), title);
      audit::control(ImVec2((c0.x + c1.x - ts.x) * 0.5f, (c0.y + c1.y - ts.y) * 0.5f), ImVec2((c0.x + c1.x + ts.x) * 0.5f, (c0.y + c1.y + ts.y) * 0.5f),
                     "title");
      ImGui::PopFont();
    }
    {  // legend: ● TIME   ● VOLUME
      const float fsz = std::clamp(v.px(7.5f), 9.f, 14.f);
      ImGui::PushFont(app().fontMonoBold, fsz);
      const char* lt = i18n::tr("TIME");
      const char* lv = i18n::tr("VOLUME");
      const float wt = ImGui::CalcTextSize(lt).x, wv = ImGui::CalcTextSize(lv).x, dot = fsz * 0.7f, gap = fsz * 1.4f;
      const float total = dot + 4 + wt + gap + dot + 4 + wv;
      const ImVec2 mid((v.p(60, 0).x + v.p(262, 0).x) * 0.5f, v.p(0, 38).y);
      float x = std::round(mid.x - total * 0.5f);
      dl->AddCircleFilled(ImVec2(x + dot * 0.5f, mid.y), dot * 0.5f, laneT, 12);
      dl->AddText(ImVec2(x + dot + 4, std::round(mid.y - fsz * 0.55f)), IM_COL32(0xA8, 0xB4, 0xBC, 255), lt);
      x += dot + 4 + wt + gap;
      dl->AddCircleFilled(ImVec2(x + dot * 0.5f, mid.y), dot * 0.5f, laneV, 12);
      dl->AddText(ImVec2(x + dot + 4, std::round(mid.y - fsz * 0.55f)), IM_COL32(0xA8, 0xB4, 0xBC, 255), lv);
      ImGui::PopFont();
    }
  }
  sk::Palette dark = pal;
  dark.text = IM_COL32(0xA8, 0xB4, 0xBC, 255);
  sk::wheel(a, t, v, "TimeMixWheel", TgTimeMix, pal);
  sk::textAt(dl, v.p(5, 52), v.p(57, 62), dark.text, i18n::tr("TIME MIX"), std::clamp(v.px(7.f), 9.f, 12.f), 0, true);
  if (sk::button(v, "OptionsBtn", pal, false, nullptr, sk::glyphMenu, "Options")) ImGui::OpenPopup("##gbopts");
  if (ImGui::BeginPopup("##gbopts")) {
    auto tog = [&](const char* label, int p) {
      bool on = bval(st, p);
      if (ImGui::MenuItem(label, nullptr, on)) a.doc.setParam(t.pid(p), on ? 0 : 1, false);
    };
    tog("Trigger slots from notes", TgMidiTrigger);
    if (ImGui::BeginMenu(i18n::tr("Click reduction"))) {
      static const char* dc[3] = {"Off", "Low (drums)", "High (melodic)"};
      for (int k = 0; k < 3; ++k)
        if (ImGui::MenuItem(dc[k], nullptr, ival(st, TgDeclick, 1) == k)) a.doc.setParam(t.pid(TgDeclick), k, false);
      ImGui::EndMenu();
    }
    tog("Smoothing attack compensation", TgCompensate);
    tog("Remove DC offset", TgDcRemove);
    tog("HQ resampling", TgHq);
    ImGui::Separator();
    tog("Run when stopped", TgFreeRun);
    if (ImGui::BeginMenu(i18n::tr("Default trigger sync"))) {
      static const char* sy[6] = {"Off", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats"};
      for (int k = 0; k < 6; ++k)
        if (ImGui::MenuItem(sy[k], nullptr, ival(st, TgTrigSync) == k)) a.doc.setParam(t.pid(TgTrigSync), k, false);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Quick effect (no time slot)"))) {
      static const char* modes[8] = {"Off", "Repeat 1/4", "Repeat 1/8", "Repeat 1/16", "Repeat 1/32", "Half speed", "Tape stop", "Reverse"};
      for (int k = 0; k < 8; ++k)
        if (ImGui::MenuItem(modes[k], nullptr, ival(st, TgMode) == k)) a.doc.setParam(t.pid(TgMode), k, false);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Step gate (no volume slot)"))) {
      tog("Enabled", TgGateOn);
      for (int k = 0; k < kGateSteps; ++k) {
        char lb[24];
        std::snprintf(lb, sizeof lb, "Step %d", k + 1);
        bool on = bval(st, TgGate + k, true);
        if (ImGui::MenuItem(lb, nullptr, on)) a.doc.setParam(t.pid(TgGate + k), on ? 0 : 1, false);
      }
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  {  // HOLD pad (bar repeat)
    bool on = bval(st, TgHold);
    const sk::FormCtl& hb = v.c("HoldBtn");
    ImVec2 p0 = v.p0(hb), p1 = v.p1(hb);
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID("hold");
    if (ImGui::InvisibleButton("##h", ImVec2(p1.x - p0.x, p1.y - p0.y))) a.doc.setParam(t.pid(TgHold), on ? 0 : 1, false);
    if (ImGui::IsItemHovered()) a.setHint("Hold: repeat the last bar while on");
    ImGui::PopID();
    // J-93: a lit pad (glow + bright face) when on, a dark recessed pad with a green rim when off.
    ImU32 g = sk::col(0x83E32B);
    const bool hovH = ImGui::IsMouseHoveringRect(p0, p1);
    if (on) {
      dl->AddRectFilled(ImVec2(p0.x - 3, p0.y - 3), ImVec2(p1.x + 3, p1.y + 3), theme::withAlpha(g, 0.22f), v.px(6));
      sk::gradientRect(dl, p0, p1, sk::lighten(g, 0.25f), sk::darken(g, 0.12f), v.px(4));
      dl->AddLine(ImVec2(p0.x + v.px(4), p0.y + 1), ImVec2(p1.x - v.px(4), p0.y + 1), IM_COL32(255, 255, 255, 110), 1.0f);
    } else {
      sk::insetField(dl, p0, p1, theme::mix(g, IM_COL32(14, 20, 16, 255), hovH ? 0.78f : 0.84f), v.px(4));
      dl->AddRect(p0, p1, theme::withAlpha(g, hovH ? 0.7f : 0.45f), v.px(4), 0, 1.0f);
    }
    sk::textAt(dl, p0, p1, on ? IM_COL32(0x10, 0x20, 0x08, 255) : theme::mix(g, IM_COL32(255, 255, 255, 255), 0.35f), i18n::tr("HOLD"), v.px(11), 0,
               true);
  }
  // Slot grids.
  auto grid = [&](bool vol) {
    const sk::FormCtl& c = v.c(vol ? "VolSlotSel" : "TimeSlotSel");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    const float cw = (p1.x - p0.x) / 4, ch = (p1.y - p0.y) / 9;
    const int sel = vol ? selV : selT, play = vol ? playV : playT;
    {  // section header: lane-coloured tick, title, hairline
      sk::Skin hk = pal;
      hk.fill = vol ? laneV : laneT;
      hk.pillText = IM_COL32(0xC8, 0xD2, 0xD8, 255);
      hk.textDim = pal.textDim;
      sk::sectionHeader(dl, ImVec2(p0.x, p0.y - v.px(11)), ImVec2(p1.x, p0.y - v.px(1)), hk, vol ? i18n::tr("VOLUME") : i18n::tr("TIME"),
                        std::clamp(v.px(7.5f), 9.f, 13.f));
    }
    ImGui::PushID(vol ? "vgrid" : "tgrid");
    for (int k = 0; k < tg::kSlots; ++k) {
      int cx = k % 4, cy = k / 4;
      ImVec2 a0(p0.x + cx * cw, p0.y + cy * ch), a1(a0.x + cw - 1, a0.y + ch - 1);
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(a0);
      bool clicked = ImGui::InvisibleButton("##s", ImVec2(cw - 1, ch - 1));
      bool hov = ImGui::IsItemHovered();
      if (clicked) {
        a.doc.setParam(t.pid(vol ? TgVolSlot : TgTimeSlot), sel == k + 1 ? 0 : k + 1, false);
        ui.volPage = vol, ui.edit[vol ? 1 : 0] = k;
      }
      if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ui.volPage = vol, ui.edit[vol ? 1 : 0] = k;
        ImGui::OpenPopup("##slotm");
      }
      if (ImGui::BeginPopup("##slotm")) {
        const tg::Slot& s = vol ? bank.vol[k] : bank.time[k];
        if (ImGui::MenuItem(i18n::tr("Reset"))) {
          PluginState ns = st;
          ns.data = tg::resetSlot(st.data, vol ? 'V' : 'T', k);
          setPluginState(a, t, ns, "reset slot");
        }
        if (ImGui::MenuItem(i18n::tr("Edit slot name..."))) {
          std::snprintf(ui.rename, sizeof ui.rename, "%s", s.name);
          ui.renameBank = vol ? 1 : 0, ui.renameSlot = k;
        }
        auto move = [&](int to) {
          const tg::Slot& o = vol ? bank.vol[to] : bank.time[to];
          PluginState ns = st;
          ns.data = tg::replaceSlot(tg::replaceSlot(st.data, vol ? 'V' : 'T', to, s), vol ? 'V' : 'T', k, o);
          setPluginState(a, t, ns, "move slot");
          ui.edit[vol ? 1 : 0] = to;
        };
        if (ImGui::MenuItem(i18n::tr("Move slot left"), nullptr, false, k > 0)) move(k - 1);
        if (ImGui::MenuItem(i18n::tr("Move slot right"), nullptr, false, k + 1 < tg::kSlots)) move(k + 1);
        ImGui::EndPopup();
      }
      const tg::Slot& s = vol ? bank.vol[k] : bank.time[k];
      bool on = sel == k + 1, playing = play == k + 1, shown = ui.volPage == vol && ui.edit[vol ? 1 : 0] == k;
      // J-93 cell: shaded face; selected = lane colour face + left bar; playing = lane outline;
      // being edited = white inner ring. The preview sits in the lower part under the name.
      const ImU32 lane = vol ? laneV : laneT;
      const ImU32 face = on ? theme::mix(sk::col(0x1E282F), lane, 0.42f) : hov ? sk::col(0x26323B) : sk::col(0x1C252C);
      sk::gradientRect(dl, a0, a1, sk::lighten(face, 0.05f), sk::darken(face, 0.06f), v.px(2));
      if (on) dl->AddRectFilled(a0, ImVec2(a0.x + std::max(2.0f, v.px(2)), a1.y), lane, v.px(2), ImDrawFlags_RoundCornersLeft);
      if (playing) dl->AddRect(a0, a1, theme::withAlpha(lane, on ? 1.0f : 0.85f), v.px(2), 0, 1.5f);
      if (shown) dl->AddRect(ImVec2(a0.x + 1.5f, a0.y + 1.5f), ImVec2(a1.x - 1.5f, a1.y - 1.5f), IM_COL32(0xF4, 0xF6, 0xF8, 210), v.px(1.5f), 0, 1.0f);
      const float nameH = (a1.y - a0.y) * 0.56f;
      if (ui.previews) {
        ImVec2 pts[32];
        const float py0 = a0.y + nameH, py1 = a1.y - 2.5f, px0 = a0.x + v.px(4), px1 = a1.x - 3;
        for (int i = 0; i < 32; ++i) {
          double x = s.len * i / 31.0;
          double y = std::clamp(s.eval(x), 0.0, 1.0);
          pts[i] = ImVec2(px0 + (px1 - px0) * i / 31.0f, py1 - static_cast<float>(y) * (py1 - py0));
        }
        dl->AddPolyline(pts, 32, theme::withAlpha(on ? IM_COL32(255, 255, 255, 255) : lane, on ? 0.75f : 0.5f), 0, 1.1f);
      }
      char lb[40];
      if (s.name[0]) std::snprintf(lb, sizeof lb, "%s", s.name);
      else std::snprintf(lb, sizeof lb, "%d", k + 1);
      sk::textAt(dl, ImVec2(a0.x + v.px(3.5f), a0.y), ImVec2(a1.x - 2, ui.previews ? a0.y + nameH : a1.y),
                 on ? IM_COL32(255, 255, 255, 255) : s.name[0] ? IM_COL32(0xC4, 0xCE, 0xD4, 255) : IM_COL32(0x6E, 0x7C, 0x86, 255), lb,
                 std::clamp(v.px(7.0f), 9.f, 12.f), -1);
      if (hov) a.setHint(std::string(vol ? "Volume" : "Time") + " slot " + std::to_string(k + 1) + (s.name[0] ? std::string(": ") + s.name : "") +
                         "   (click: select · right-click: menu)");
      ImGui::PopID();
    }
    ImGui::PopID();
  };
  grid(false);
  grid(true);
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
  if (sk::button(v, "SlotZoom", pal, ui.previews, nullptr,
                 [](ImDrawList* d, ImVec2 p0, ImVec2 p1, ImU32 c) { sk::glyphWave(d, 0, p0, p1, c, 1.2f); }, "Toggle envelope previews"))
    ui.previews = !ui.previews;
  sk::wheel(a, t, v, "VolAttWheel", TgVolAttack, pal);
  sk::wheel(a, t, v, "VolRelWheel", TgVolRelease, pal);
  sk::wheel(a, t, v, "VolTnsWheel", TgTension, pal);
  sk::label(v, "VolAttWheelLabel", pal, "ATT");
  sk::label(v, "VolRelWheelLabel", pal, "REL");
  sk::label(v, "VolTnsWheelLabel", pal, "TNS");
  sk::wheel(a, t, v, "VolMixWheel", TgVolMix, pal);
  sk::textAt(dl, v.p(58, 494 + G), v.p(140, 514 + G), dark.text, i18n::tr("VOLUME MIX"), std::clamp(v.px(7.f), 9.f, 12.f), -1, true);

  // ---------------------------------------------------------------- tool strip
  const bool vol = ui.volPage;
  const int k = std::clamp(ui.edit[vol ? 1 : 0], 0, tg::kSlots - 1);
  tg::Slot slot = vol ? bank.vol[k] : bank.time[k];
  bool slotChanged = false;
  if (sk::button(v, "MenuBtn", pal, false, nullptr, sk::glyphMenu, "Envelope options")) ImGui::OpenPopup("##gbenv");
  if (ImGui::BeginPopup("##gbenv")) {
    if (ImGui::MenuItem(i18n::tr("Copy slot"))) ui.clip = tg::serialize(vol ? 'V' : 'T', k, slot);
    if (ImGui::MenuItem(i18n::tr("Paste slot"), nullptr, false, !ui.clip.empty())) {
      auto tmp = std::make_unique<tg::Bank>();
      std::string line = "GB2\n" + std::string(1, vol ? 'V' : 'T') + std::to_string(k + 1) + ui.clip.substr(ui.clip.find(' '));
      tg::factory(*tmp);
      tg::parse(line.c_str(), *tmp);
      slot = vol ? tmp->vol[k] : tmp->time[k];
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
        r.env.add(slot.len - slot.env.x[i], slot.env.y[i], i == slot.env.n - 1 ? 0.f : tt, i == slot.env.n - 1 ? static_cast<uint8_t>(menv::SingleCurve) : m);
      }
      slot = r;
      slotChanged = true;
    }
    if (ImGui::BeginMenu(i18n::tr("Length"))) {
      static const float lens[5] = {1, 2, 4, 8, 16};
      static const char* names[5] = {"1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
      for (int i = 0; i < 5; ++i)
        if (ImGui::MenuItem(names[i], nullptr, std::fabs(slot.len - lens[i]) < 1e-3f)) {
          float sc = lens[i] / slot.len;
          for (int j = 0; j < slot.env.n; ++j) slot.env.x[j] *= sc;
          slot.len = lens[i];
          slotChanged = true;
        }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Factory content"))) {
      PluginState ns = st;
      ns.data = tg::resetSlot(st.data, vol ? 'V' : 'T', k);
      setPluginState(a, t, ns, "reset slot");
    }
    ImGui::EndPopup();
  }
  {
    static const char* snaps[7] = {"-", "16", "12", "8", "6", "4", "3"};
    const sk::FormCtl& c = v.c("SnapSel");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    float ch = (p1.y - p0.y) / 7;
    sk::insetField(dl, p0, p1, sk::col(0x1A2228), v.px(3));
    const ImU32 lane = ui.volPage ? laneV : laneT;
    for (int i = 0; i < 7; ++i) {
      ImVec2 a0(p0.x, p0.y + i * ch), a1(p1.x, a0.y + ch);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(400 + i);
      if (ImGui::InvisibleButton("##sn", ImVec2(p1.x - p0.x, ch))) ui.snap = i;
      const bool hovS = ImGui::IsItemHovered();
      if (hovS) a.setHint(i == 0 ? "Snap off" : std::string("Snap to 1/") + snaps[i] + " beat");
      ImGui::PopID();
      if (ui.snap == i) {
        sk::gradientRect(dl, ImVec2(a0.x + 2, a0.y + 2), ImVec2(a1.x - 2, a1.y - 2), theme::withAlpha(lane, 0.42f), theme::withAlpha(lane, 0.26f), v.px(2));
        dl->AddRect(ImVec2(a0.x + 2, a0.y + 2), ImVec2(a1.x - 2, a1.y - 2), theme::withAlpha(lane, 0.7f), v.px(2), 0, 1.0f);
      } else if (hovS) {
        dl->AddRectFilled(ImVec2(a0.x + 2, a0.y + 2), ImVec2(a1.x - 2, a1.y - 2), IM_COL32(255, 255, 255, 16), v.px(2));
      }
      sk::textAt(dl, a0, a1, ui.snap == i ? IM_COL32(255, 255, 255, 255) : pal.textDim, snaps[i], std::clamp(v.px(8.f), 9.f, 12.f), 0, true);
    }
  }
  if (sk::button(v, "StepBtn", pal, ui.step, nullptr, sk::glyphSteps, "Step editing")) ui.step = !ui.step;
  if (sk::button(v, "SlideBtn", pal, ui.slide, nullptr, sk::glyphSlide, "Slide remaining points")) ui.slide = !ui.slide;
  if (sk::button(v, "FreezeBtn", pal, ui.freeze, nullptr, sk::glyphSnow, "Freeze editing")) ui.freeze = !ui.freeze;
  {
    const sk::FormCtl& c = v.c("MapTypeSel");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    float ch = (p1.y - p0.y) / 2;
    for (int i = 0; i < 2; ++i) {
      ImVec2 a0(p0.x, p0.y + i * ch), a1(p1.x, a0.y + ch - 1);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(450 + i);
      if (ImGui::InvisibleButton("##mt", ImVec2(p1.x - p0.x, ch))) ui.volPage = i == 1;
      if (ImGui::IsItemHovered()) a.setHint(i == 0 ? "Edit the time envelope" : "Edit the volume envelope");
      ImGui::PopID();
      bool on = ui.volPage == (i == 1);
      const ImU32 lane = i ? laneV : laneT;
      if (on) {
        sk::gradientRect(dl, a0, a1, theme::mix(pal.raised, lane, 0.62f), theme::mix(pal.raised, lane, 0.42f), v.px(3));
        dl->AddRect(a0, a1, theme::withAlpha(sk::lighten(lane, 0.2f), 0.9f), v.px(3), 0, 1.0f);
      } else {
        sk::raisedFace(dl, a0, a1, pal.raised, v.px(3));
      }
      ImU32 gc = on ? IM_COL32(255, 255, 255, 255) : theme::mix(pal.textDim, lane, 0.35f);
      if (i == 0) glyphClock(dl, ImVec2(a0.x + v.px(4), a0.y + v.px(4)), ImVec2(a1.x - v.px(4), a1.y - v.px(4)), gc);
      else glyphVolume(dl, ImVec2(a0.x + v.px(4), a0.y + v.px(5)), ImVec2(a1.x - v.px(4), a1.y - v.px(5)), gc);
    }
  }

  // ---------------------------------------------------------------- envelope editor
  {
    ImVec2 e0 = v.p(298, 5), e1 = v.p(818, 489 + G);
    envui::Style es;
    es.bg = sk::col(vol ? 0x221F27 : 0x1B252C);
    es.grid = theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.04f);
    es.gridStrong = theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.12f);
    es.line = vol ? laneV : laneT;
    es.fill = theme::withAlpha(es.line, 0.20f);
    es.point = IM_COL32(0xF2, 0xF4, 0xF6, 255);
    es.pointHot = vol ? laneV : laneT;
    es.handle = theme::withAlpha(es.line, 0.8f);
    es.text = es.point;
    envui::Opts o;
    o.xLabels = true;
    // Readout while hovering / dragging (precise editing): position in beats and the lane's value.
    o.readout = [vol](double x, double y) {
      char b[64];
      const double d = tg::delayBeats(y);
      if (vol) std::snprintf(b, sizeof b, "%s %.2f  ·  %d%%", i18n::tr("beat"), x + 1.0, static_cast<int>(std::lround(y * 100.0)));
      else if (d < 0.005) std::snprintf(b, sizeof b, "%s %.2f  ·  %s", i18n::tr("beat"), x + 1.0, i18n::tr("live"));
      else std::snprintf(b, sizeof b, "%s %.2f  ·  -%.2f %s", i18n::tr("beat"), x + 1.0, d, i18n::tr("beats"));
      return std::string(b);
    };
    o.xMax = slot.len;
    o.gridDiv = ui.snap > 0 ? static_cast<int>(kSnapDiv[ui.snap]) : 4;
    o.gridEvery = o.gridDiv;
    o.snapDiv = ui.snap > 0 ? static_cast<int>(kSnapDiv[ui.snap]) : 0;
    o.yLines = vol ? 4 : 8;
    o.freeze = ui.freeze, o.step = ui.step, o.slide = ui.slide;
    int play = vol ? playV : playT;
    float pos = vol ? vPos : tPos;
    o.playX = play == k + 1 && pos >= 0 ? std::fmod(static_cast<double>(pos), static_cast<double>(slot.len)) : -1;
    if (!vol)
      o.extraMenu = [&](int i) {
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
    int r = envui::edit("##gbenv", e0, e1, slot.env, ui.env, o, es);
    if (r & envui::Changed) slotChanged = true;
    if (r & envui::Released) a.doc.endGesture();
    // Axis labels: delay scale on the time page, level on the volume page (J-93: readable, mono).
    {
      const float H = e1.y - e0.y;
      const float lfs = std::clamp(v.px(8.5f), 10.f, 15.f);
      const ImU32 lc = theme::withAlpha(es.text, 0.62f);
      for (int i = 0; i <= 8; i += 2) {
        if (vol && i % 2) continue;
        float y = e1.y - H * (1.0f - i / 8.0f);
        char lb[16];
        if (vol) std::snprintf(lb, sizeof lb, "%d%%", 100 - i * 25 / 2);
        else if (i == 0) std::snprintf(lb, sizeof lb, "%s", i18n::tr("live"));
        else std::snprintf(lb, sizeof lb, "-%d", i);
        if (i == 0) y += lfs * 0.6f;  // keep the top label inside
        if (i == 8) y -= lfs * 0.6f;  // and the bottom one
        sk::textAt(dl, ImVec2(e0.x + 4, y - v.px(7)), ImVec2(e0.x + v.px(44), y + v.px(7)), lc, lb, lfs, -1, true);
      }
    }
    // Time scroller strip (shows the visible range: the whole slot).
    ImVec2 s0 = v.p(298, 493 + G), s1 = v.p(653, 504 + G);
    sk::insetField(dl, s0, s1, sk::col(0x1A2228), v.px(3));
    sk::gradientRect(dl, ImVec2(s0.x + 2, s0.y + 2), ImVec2(s1.x - 2, s1.y - 2), theme::withAlpha(es.line, 0.55f), theme::withAlpha(es.line, 0.32f),
                     v.px(2));
    char lenTxt[32];
    std::snprintf(lenTxt, sizeof lenTxt, "%g %s", static_cast<double>(slot.len), i18n::tr("beats"));
    sk::textAt(dl, v.p(660, 491 + G), v.p(818, 506 + G), pal.textDim, lenTxt, std::clamp(v.px(8.f), 9.f, 12.f), 1, true);
  }

  // ---------------------------------------------------------------- slot bar
  {
    const sk::FormCtl& nl = v.c("NameLabel");
    ImVec2 p0 = v.p0(nl), p1 = v.p1(nl);
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID("name");
    if (ImGui::InvisibleButton("##n", ImVec2(p1.x - p0.x, p1.y - p0.y))) {
      std::snprintf(ui.rename, sizeof ui.rename, "%s", slot.name);
      ui.renameBank = vol ? 1 : 0, ui.renameSlot = k;
    }
    if (ImGui::IsItemHovered()) a.setHint("Click to rename the slot");
    ImGui::PopID();
    char lb[48];
    std::snprintf(lb, sizeof lb, "%s %d%s%s", vol ? "V" : "T", k + 1, slot.name[0] ? "  " : "", slot.name);
    sk::lcd(dl, p0, p1, pal, lb, -1);
  }
  if (sk::button(v, "LinkOtherBtn", pal, slot.link, nullptr, glyphLink, "Link volume slot (the time slot also selects the same volume slot)") && !vol)
    slot.link = !slot.link, slotChanged = true;
  if (sk::button(v, "KeyHoldBtn", pal, slot.keyHeld, nullptr, glyphKey, "Key-held (back to the previous slot on key release)"))
    slot.keyHeld = !slot.keyHeld, slotChanged = true;
  if (sk::button(v, "OneShotBtn", pal, slot.oneShot, nullptr, glyphOneShot, "One-shot (back to the first slot once done)"))
    slot.oneShot = !slot.oneShot, slotChanged = true;
  {
    static const char* fh[3] = {"Hold: off", "Hold: last", "Hold: next"};
    static const char* sy[7] = {"Trig: default", "Trig: off", "Trig: 1/4", "Trig: 1/2", "Trig: 1", "Trig: 2", "Trig: 4"};
    static const char* ps[6] = {"Pos: off", "Pos: 1/4", "Pos: 1/2", "Pos: 1", "Pos: 2", "Pos: 4"};
    int f = slot.force, tr = slot.trig + 1, po = slot.pos;
    if (sk::comboFree(v, "ForceHoldSel", pal, fh, 3, f, "Force hold (bar repeat) when this slot starts")) slot.force = f, slotChanged = true;
    if (sk::comboFree(v, "TrigSyncSel", pal, sy, 7, tr, "Slot trigger synchronization")) slot.trig = tr - 1, slotChanged = true;
    if (sk::comboFree(v, "PosSyncSel", pal, ps, 6, po, "Slot position synchronization")) slot.pos = po, slotChanged = true;
  }
  if (slotChanged) commitSlot(a, t, st, vol, k, slot, "edit shuttle slot");
  (void)fs;
  sk::end(v);
}

}  // namespace gl
