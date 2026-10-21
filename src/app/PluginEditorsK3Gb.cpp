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
#include "../engine/Program.h"
#include "../engine/TimeGateSlots.h"
#include "EnvEditor.h"
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
  {
    const sk::FormCtl& top = v.c("TopBgPaintBox");
    dl->AddRectFilledMultiColor(v.p0(top), v.p1(top), sk::col(0x8A9298), sk::col(0x8A9298), sk::col(0x6E767C), sk::col(0x6E767C));
    const sk::FormCtl& bot = v.c("BottomBgPaintBox");
    dl->AddRectFilledMultiColor(v.p0(bot), v.p1(bot), sk::col(0x6E767C), sk::col(0x6E767C), sk::col(0x8A9298), sk::col(0x8A9298));
    sk::bevel(v, "SlotsBevel", pal);
    sk::textAt(dl, v.p(60, 6), v.p(262, 30), IM_COL32(0x1E, 0x26, 0x2C, 255), "TIME GATE", v.px(15), 0);
    sk::textAt(dl, v.p(60, 30), v.p(262, 46), IM_COL32(0x2E, 0x36, 0x3C, 255), "time  ·  volume", v.px(9), 0);
  }
  sk::Palette dark = pal;
  dark.text = IM_COL32(0x16, 0x1C, 0x20, 255);
  sk::wheel(a, t, v, "TimeMixWheel", TgTimeMix, pal);
  sk::textAt(dl, v.p(5, 52), v.p(57, 62), dark.text, "TIME MIX", std::clamp(v.px(7.f), 9.f, 12.f));
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
    ImU32 g = sk::col(0x83E32B);
    dl->AddRectFilled(p0, p1, on ? g : theme::mix(g, IM_COL32(20, 30, 20, 255), 0.7f), v.px(4));
    if (on) dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), theme::withAlpha(g, 0.5f), v.px(5), 0, 2.f);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 150), v.px(4));
    sk::textAt(dl, p0, p1, on ? IM_COL32(0x10, 0x20, 0x08, 255) : IM_COL32(0xB0, 0xC8, 0xA0, 255), "HOLD", v.px(12), 0);
  }
  // Slot grids.
  auto grid = [&](bool vol) {
    const sk::FormCtl& c = v.c(vol ? "VolSlotSel" : "TimeSlotSel");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    const float cw = (p1.x - p0.x) / 4, ch = (p1.y - p0.y) / 9;
    const int sel = vol ? selV : selT, play = vol ? playV : playT;
    sk::textAt(dl, ImVec2(p0.x, p0.y - v.px(11)), ImVec2(p1.x, p0.y - v.px(1)), pal.textDim, vol ? "VOLUME" : "TIME", std::clamp(v.px(7.5f), 9.f, 12.f), -1);
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
      ImU32 base = sk::col(0x1E282F);
      if (on) base = theme::mix(base, vol ? pal.fill : pal.accent2, 0.45f);
      else if (hov) base = theme::mix(base, IM_COL32(255, 255, 255, 255), 0.08f);
      dl->AddRectFilled(a0, a1, base, v.px(1.5f));
      if (playing && !on) dl->AddRect(a0, a1, theme::withAlpha(vol ? pal.fill : pal.accent2, 0.8f), v.px(1.5f), 0, 1.5f);
      if (shown) dl->AddRect(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1), IM_COL32(0xF0, 0xF0, 0xF0, 200), v.px(1.5f), 0, 1.0f);
      if (ui.previews) {
        ImVec2 pts[24];
        for (int i = 0; i < 24; ++i) {
          double x = s.len * i / 23.0;
          double y = std::clamp(s.eval(x), 0.0, 1.0);
          pts[i] = ImVec2(a0.x + 2 + (a1.x - a0.x - 4) * i / 23.0f, a1.y - 2 - static_cast<float>(y) * (a1.y - a0.y - 4));
        }
        dl->AddPolyline(pts, 24, theme::withAlpha(on ? IM_COL32(255, 255, 255, 255) : pal.textDim, 0.45f), 0, 1.0f);
      }
      char lb[40];
      if (s.name[0]) std::snprintf(lb, sizeof lb, "%s", s.name);
      else std::snprintf(lb, sizeof lb, "%d", k + 1);
      sk::textAt(dl, ImVec2(a0.x + 2, a0.y), a1, on ? IM_COL32(255, 255, 255, 255) : pal.text, lb, std::clamp(v.px(7.5f), 9.f, 12.f), -1);
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
  sk::textAt(dl, v.p(58, 494 + G), v.p(140, 514 + G), dark.text, "VOLUME MIX", std::clamp(v.px(7.f), 9.f, 12.f), -1);

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
    dl->AddRectFilled(p0, p1, pal.well, v.px(2));
    for (int i = 0; i < 7; ++i) {
      ImVec2 a0(p0.x, p0.y + i * ch), a1(p1.x, a0.y + ch);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(400 + i);
      if (ImGui::InvisibleButton("##sn", ImVec2(p1.x - p0.x, ch))) ui.snap = i;
      if (ImGui::IsItemHovered()) a.setHint(i == 0 ? "Snap off" : std::string("Snap to 1/") + snaps[i] + " beat");
      ImGui::PopID();
      if (ui.snap == i) dl->AddRectFilled(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1), theme::withAlpha(pal.fill, 0.35f), v.px(2));
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
      dl->AddRectFilled(a0, a1, on ? theme::mix(pal.raised, i ? pal.fill : pal.accent2, 0.5f) : pal.raised, v.px(2));
      ImU32 gc = on ? IM_COL32(255, 255, 255, 255) : pal.textDim;
      if (i == 0) glyphClock(dl, ImVec2(a0.x + v.px(4), a0.y + v.px(4)), ImVec2(a1.x - v.px(4), a1.y - v.px(4)), gc);
      else glyphVolume(dl, ImVec2(a0.x + v.px(4), a0.y + v.px(5)), ImVec2(a1.x - v.px(4), a1.y - v.px(5)), gc);
    }
  }

  // ---------------------------------------------------------------- envelope editor
  {
    ImVec2 e0 = v.p(298, 5), e1 = v.p(818, 489 + G);
    envui::Style es;
    es.bg = sk::col(vol ? 0x2B2A33 : 0x24333C);
    es.grid = theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.05f);
    es.gridStrong = theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.14f);
    es.line = vol ? pal.fill : pal.accent2;
    es.fill = theme::withAlpha(es.line, 0.13f);
    es.point = IM_COL32(0xF0, 0xF0, 0xF0, 255);
    es.pointHot = vol ? pal.fill : pal.accent2;
    es.handle = theme::withAlpha(es.point, 0.7f);
    es.text = es.point;
    envui::Opts o;
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
    // Labels: delay scale on the time page.
    if (!vol) {
      for (int i = 0; i <= 8; i += 2) {
        float y = e1.y - (e1.y - e0.y) * (1.0f - i / 8.0f);
        char lb[16];
        std::snprintf(lb, sizeof lb, i == 0 ? "live" : "-%d", i);
        sk::textAt(dl, ImVec2(e0.x + 3, y - v.px(7)), ImVec2(e0.x + v.px(40), y + v.px(7)), theme::withAlpha(es.text, 0.35f), lb,
                   std::clamp(v.px(7.f), 9.f, 11.f), -1, true);
      }
    }
    // Time scroller strip (shows the visible range: the whole slot).
    ImVec2 s0 = v.p(298, 492 + G), s1 = v.p(653, 505 + G);
    dl->AddRectFilled(s0, s1, pal.well, v.px(2));
    dl->AddRectFilled(ImVec2(s0.x + 2, s0.y + 2), ImVec2(s1.x - 2, s1.y - 2), theme::withAlpha(es.line, 0.35f), v.px(2));
    char lenTxt[32];
    std::snprintf(lenTxt, sizeof lenTxt, "%g beats", static_cast<double>(slot.len));
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
  if (slotChanged) commitSlot(a, t, st, vol, k, slot, "edit time gate slot");
  (void)fs;
  sk::end(v);
}

}  // namespace gl
