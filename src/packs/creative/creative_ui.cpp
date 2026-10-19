// Gridline — "creative" pack: Octa Filter editor at the reference geometry
// (670 x 400 form): unit strip (enable / solo boxes, routing arrows, unit
// tabs), the unit page (IN, FILTER, SHAPER, OUT boxes), the envelope editor
// (9 targets x 5 parts, pattern number) and the main panel (title, XY pad,
// master / LFO / follower sliders, follower box, options, info text).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include "../../app/App.h"
#include "../../app/EnvEditor.h"
#include "../../app/PluginEditorsCommon.h"
#include "../../app/SkinForms.h"
#include "../../app/PluginSkin.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"
#include "creative.h"

namespace gl::packs::creative {

namespace {

namespace sk = skin;
namespace fm = skin::forms;

struct OfUi {
  int unit = 0;
  int target = 2;  // 0..7 targets, 8 = shaper curve
  int part = 0;    // 0 pattern, 1 LFO, 2 X map, 3 Y map, 4 follower map
  bool hideLeft = false, snap = true, step = false, slide = false, freeze = false;
  envui::State env;
  std::string clip;
  char info[1024] = {};
  size_t infoHash = 0;
};
std::map<uint64_t, OfUi> ofUi;

struct OfCache {
  std::string data;
  std::unique_ptr<OctaBank> bank;
};
std::map<uint64_t, OfCache> ofCache;
const OctaBank& bankFor(uint64_t key, const std::string& data) {
  OfCache& c = ofCache[key];
  if (!c.bank || c.data != data) {
    if (!c.bank) c.bank = std::make_unique<OctaBank>();
    parse(data.c_str(), *c.bank);
    c.data = data;
  }
  return *c.bank;
}

const char* kTypeNames[FilterTypeCount] = {"Multimode (L / B / H)", "Low pass 12", "High pass 12", "Low pass", "Band pass", "Band stop",
                                           "High pass", "All pass", "Low shelf", "Peak", "High shelf", "Low pass 24 drive"};

void setState(App& a, const PluginTarget& t, const PluginState& st, const std::string& data, const char* what, uint64_t merge = 0) {
  PluginState ns = st;
  ns.data = data;
  setPluginState(a, t, ns, what, merge);
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != "gl.octafilter") return false;
  const PluginState st = *sp;
  OfUi& ui = ofUi[t.key()];
  const OctaBank& bank = bankFor(t.key(), st.data);
  const int u = std::clamp(ui.unit, 0, kUnits - 1);
  auto P = [&](int k) { return ed::val(st, k); };
  sk::Palette pal = sk::paletteFrom(fm::kLovephilter);
  pal.fill = sk::col(0xFDA73F);
  pal.text = IM_COL32(0xEC, 0xEF, 0xF2, 255);
  pal.textDim = IM_COL32(0x9A, 0xA4, 0xAE, 255);
  // Hidden left panel: only the main panel is shown (form shifted left).
  const float fullW = static_cast<float>(fm::kLovephilter.w);
  const float shownW = ui.hideLeft ? 227.f : fullW;
  const float scale = std::max(0.5f, std::min(2.0f, width / shownW));
  sk::View v;
  v.f = &fm::kLovephilter;
  v.s = scale;
  ImVec2 cur = ImGui::GetCursorScreenPos();
  v.o = ImVec2(cur.x - (ui.hideLeft ? 443.f * scale : 0.f), cur.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(cur, ImVec2(cur.x + shownW * scale, cur.y + 400 * scale), true);
  dl->AddRectFilled(cur, ImVec2(cur.x + shownW * scale, cur.y + 400 * scale), pal.panel, v.px(3));
  const float fs = std::clamp(v.px(8.f), 9.f, 13.f);
  auto lbl = [&](const char* ctl, const char* text) {
    const sk::FormCtl& c = v.c(ctl);
    sk::textAt(dl, v.p(static_cast<float>(c.x - 4), static_cast<float>(c.y - 1)), v.p(static_cast<float>(c.x + c.w + 4), static_cast<float>(c.y + c.h + 2)),
               pal.text, text, fs);
  };

  if (!ui.hideLeft) {
    // ------------------------------------------------------------ unit strip
    {
      const sk::FormCtl& g = v.c("VectorGradient1");
      dl->AddRectFilledMultiColor(v.p0(g), v.p1(g), sk::col(0x7A848E), sk::col(0x7A848E), sk::col(0x5C656E), sk::col(0x5C656E));
    }
    for (int k = 0; k < kUnits; ++k) {
      char nm[16];
      std::snprintf(nm, sizeof nm, "EnableBank%d", k + 1);
      const sk::FormCtl& c = v.c(nm);
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID(700 + k);
      bool on = ed::bval(st, unitParam(k, UEnabled), k == 0);
      bool solo = ed::ival(st, OfSolo) == k + 1;
      ImGui::InvisibleButton("##en", ImVec2(p1.x - p0.x, p1.y - p0.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) a.doc.setParam(t.pid(unitParam(k, UEnabled)), on ? 0 : 1, false);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.doc.setParam(t.pid(OfSolo), solo ? 0 : k + 1, false);
      if (ImGui::IsItemHovered()) a.setHint("Unit " + std::to_string(k + 1) + ": click to enable · right-click to solo");
      ImGui::PopID();
      ImU32 box = on ? sk::col(0xFFA830) : sk::col(0x464E56);
      dl->AddRectFilled(ImVec2(p0.x + v.px(3), p0.y + v.px(3)), ImVec2(p1.x - v.px(3), p1.y - v.px(3)), box, v.px(2));
      dl->AddRect(ImVec2(p0.x + v.px(3), p0.y + v.px(3)), ImVec2(p1.x - v.px(3), p1.y - v.px(3)), IM_COL32(0, 0, 0, 150), v.px(2));
      if (solo) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 220), v.px(3), 0, 1.5f);
      // Routing arrow to the next unit when its to-next level is not zero.
      if (k + 1 < kUnits && std::fabs(P(unitParam(k, UNext))) > 1e-4) {
        float x0 = p1.x - v.px(1), x1 = v.p(static_cast<float>(c.x + 30 + 1), 0).x, y = (p0.y + p1.y) * 0.5f;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1 - v.px(3), y), pal.text, std::max(1.f, v.px(1.5f)));
        dl->AddTriangleFilled(ImVec2(x1, y), ImVec2(x1 - v.px(5), y - v.px(3.5f)), ImVec2(x1 - v.px(5), y + v.px(3.5f)), pal.text);
      }
    }
    {  // unit tabs
      const sk::FormCtl& c = v.c("TabSelect");
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      float cw = (p1.x - p0.x) / kUnits;
      for (int k = 0; k < kUnits; ++k) {
        ImVec2 a0(p0.x + k * cw, p0.y), a1(a0.x + cw - v.px(2), p1.y);
        ImGui::SetCursorScreenPos(a0);
        ImGui::PushID(720 + k);
        if (ImGui::InvisibleButton("##tab", ImVec2(cw, p1.y - p0.y)) || (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left))) ui.unit = k;
        ImGui::PopID();
        bool sel = ui.unit == k, on = ed::bval(st, unitParam(k, UEnabled), k == 0);
        dl->AddRectFilled(a0, a1, sel ? sk::col(0x8A949E) : sk::col(0x56606A), v.px(3), ImDrawFlags_RoundCornersTop);
        char b[4];
        std::snprintf(b, sizeof b, "%d", k + 1);
        sk::textAt(dl, a0, a1, sel ? IM_COL32(0x16, 0x1A, 0x1E, 255) : (on ? pal.text : pal.textDim), b, v.px(13), 0);
      }
    }
    sk::bevel(v, "LineBevel", pal);
    // ------------------------------------------------------------ unit page
    for (const char* b : {"InBevel", "FiltBevel", "WSBevel", "OutBevel"}) sk::bevel(v, b, pal);
    sk::label(v, "InBevelLabel", pal, "IN", 0, true);
    sk::label(v, "FiltBevelLabel", pal, "FILTER", 0, true);
    sk::label(v, "WsBevelLabel", pal, "SHAPER", 0, true);
    sk::label(v, "OutBevelLabel", pal, "OUT", 0, true);
    sk::check(a, t, v, "FFlatBtn", unitParam(u, UAlt), pal, "ALT");
    sk::wheel(a, t, v, "InVolWheel", unitParam(u, UIn), pal);
    lbl("InVolWheelLabel", "VOL");
    sk::bevel(v, "FilterSelectorBevel", pal);
    const int type = ed::ival(st, unitParam(u, UType));
    sk::digi(a, t, v, "FTypeSelect", unitParam(u, UType), pal, [](double x) {
      char b[8];
      std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(x)));
      return std::string(b);
    });
    {
      const sk::FormCtl& c = v.c("FTypeSelect");
      if (ImGui::IsMouseHoveringRect(v.p0(c), v.p1(c)) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##ftype");
      if (ImGui::BeginPopup("##ftype")) {
        for (int k = 0; k < FilterTypeCount; ++k) {
          if (k == 1 || k == 3 || k == 11) ImGui::Separator();
          if (ImGui::MenuItem(kTypeNames[k], nullptr, type == k)) a.doc.setParam(t.pid(unitParam(u, UType)), k, false);
        }
        ImGui::EndPopup();
      }
      sk::textAt(dl, v.p(70, 82), v.p(102, 94), pal.textDim, "TYPE", std::clamp(v.px(6.5f), 8.f, 11.f));
      static const char* shortNames[FilterTypeCount] = {"MULTI", "LP 12", "HP 12", "LP", "BP", "STOP", "HP", "AP", "LSHF", "PEAK", "HSHF", "LP 24"};
      sk::textAt(dl, v.p(68, 117), v.p(104, 131), pal.text, shortNames[std::clamp(type, 0, FilterTypeCount - 1)], std::clamp(v.px(7.f), 9.f, 12.f));
    }
    static const char* orders[6] = {"OFF", "SINGLE", "DUAL", "TRIPLE", "STG DUAL", "STG TRIPLE"};
    sk::textSelector(a, t, v, "FOrderSelect", unitParam(u, UOrder), pal, orders, 6);
    sk::wheel(a, t, v, "FEnvWheel", unitParam(u, UEnv), pal);
    sk::wheel(a, t, v, "FCutWheel", unitParam(u, UCut), pal);
    sk::wheel(a, t, v, "FResWheel", unitParam(u, URes), pal);
    lbl("FEnvWheelLabel", "ENV");
    lbl("FCutWheelLabel", "CUT");
    lbl("FResWheelLabel", "RES");
    if (filterUsesGain(type)) {
      sk::wheel(a, t, v, "FGainWheel", unitParam(u, ULow), pal);
      lbl("FGainWheelLabel", type == FtLadder ? "DRIVE" : "GAIN");
    } else {
      sk::slider(a, t, v, "FLowSlider", unitParam(u, ULow), pal);
      sk::slider(a, t, v, "FBandSlider", unitParam(u, UBand), pal);
      sk::slider(a, t, v, "FHighSlider", unitParam(u, UHigh), pal);
      lbl("FLowSliderLabel", "L");
      lbl("FBandSliderLabel", "B");
      lbl("FHighSliderLabel", "H");
      if (type != FtSvf) {  // only the multimode type mixes L / B / H
        const sk::FormCtl& c0 = v.c("FLowSlider");
        const sk::FormCtl& c1 = v.c("FHighSlider");
        dl->AddRectFilled(v.p0(c0), v.p1(c1), IM_COL32(0x40, 0x48, 0x50, 150), v.px(2));
      }
    }
    {  // shaper ON (tall toggle)
      bool on = ed::bval(st, unitParam(u, UWsOn));
      if (sk::button(v, "WSEnableBtn", pal, on, "ON", {}, "Enable the shaper")) a.doc.setParam(t.pid(unitParam(u, UWsOn)), on ? 0 : 1, false);
      bool bip = ed::bval(st, unitParam(u, UWsBipolar));
      if (sk::button(v, "WSBipolarBtn", pal, bip, nullptr,
                     [bip](ImDrawList* d, ImVec2 p0, ImVec2 p1, ImU32 c) {
                       float m = (p0.y + p1.y) * 0.5f;
                       if (bip) d->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), c, 1.5f);
                       else d->AddLine(ImVec2(p0.x, m), ImVec2(p1.x, p0.y), c, 1.5f), d->AddLine(ImVec2(p0.x, m), ImVec2(p1.x, p1.y), c, 1.0f);
                     },
                     "Unipolar / bipolar shaper"))
        a.doc.setParam(t.pid(unitParam(u, UWsBipolar)), bip ? 0 : 1, false);
    }
    sk::wheel(a, t, v, "WSPreAmpWheel", unitParam(u, UWsPre), pal);
    sk::wheel(a, t, v, "WSMixWheel", unitParam(u, UWsMix), pal);
    lbl("WSPreAmpWheelLabel", "AMP");
    lbl("WSMixWheelLabel", "MIX");
    lbl("QuickLabel9", "MODE");
    sk::wheel(a, t, v, "OutPanWheel", unitParam(u, UPan), pal);
    sk::wheel(a, t, v, "OutVolWheel", unitParam(u, UOut), pal);
    sk::slider(a, t, v, "ToNextFWheel", unitParam(u, UNext), pal);
    lbl("OutPanWheelLabel", "PAN");
    lbl("OutVolWheelLabel", "VOL");
    lbl("ToNextFWheelLabel", "NEXT");

    // ------------------------------------------------------------ envelope editor
    sk::bevel(v, "LeftPanelBottomBevel", pal);
    sk::bevel(v, "MEnvEditorBevel", pal);
    {
      static const char* targets[9] = {"PAN", "VOL", "CUT", "RES", "LOW", "BAND", "HIGH", "SHAPE", "SH MIX"};
      const sk::FormCtl& c = v.c("EnvTargetSelect");
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      float cw = (p1.x - p0.x) / 9;
      for (int k = 0; k < 9; ++k) {
        ImVec2 a0(p0.x + k * cw, p0.y), a1(a0.x + cw - 1, p1.y);
        ImGui::SetCursorScreenPos(a0);
        ImGui::PushID(760 + k);
        if (ImGui::InvisibleButton("##tg", ImVec2(cw, p1.y - p0.y))) ui.target = k;
        ImGui::PopID();
        int tgt = k == 8 ? TWsMix : k;
        bool used = k == 7 ? bank.wsCurve(u) != nullptr : [&] {
          for (int e = 0; e < EnvKinds; ++e)
            if (bank.env(u, tgt, e)) return true;
          return false;
        }();
        bool sel = ui.target == k;
        dl->AddRectFilled(a0, a1, sel ? sk::col(0xFDA73F) : sk::col(0x3A444C), v.px(2));
        sk::textAt(dl, a0, a1, sel ? IM_COL32(0x20, 0x16, 0x08, 255) : (used ? pal.text : pal.textDim), targets[k], fs);
      }
    }
    const bool wsPage = ui.target == 7;
    const int tgt = ui.target == 8 ? TWsMix : std::min(ui.target, 7);
    if (!wsPage) {
      static const char* parts[5] = {"PATTERN", "LFO", "X MAP", "Y MAP", "FOLLOWER MAP"};
      const sk::FormCtl& c = v.c("EnvSelect");
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      float cw = (p1.x - p0.x) / 5;
      for (int k = 0; k < 5; ++k) {
        ImVec2 a0(p0.x + k * cw, p0.y), a1(a0.x + cw - 1, p1.y);
        ImGui::SetCursorScreenPos(a0);
        ImGui::PushID(780 + k);
        if (ImGui::InvisibleButton("##pt", ImVec2(cw, p1.y - p0.y))) ui.part = k;
        ImGui::PopID();
        int kind = k == 0 ? KPattern0 + ed::ival(st, OfPattern) : k == 1 ? KLfo : KMapX + (k - 2);
        bool used = bank.env(u, tgt, kind) != nullptr, sel = ui.part == k;
        dl->AddRectFilled(a0, a1, sel ? sk::col(0x8A949E) : sk::col(0x3A444C), v.px(2));
        sk::textAt(dl, a0, a1, sel ? IM_COL32(0x16, 0x1A, 0x1E, 255) : (used ? pal.text : pal.textDim), parts[k], fs);
      }
      sk::digi(a, t, v, "PatNumSelect", OfPattern, pal, [](double x) { return std::to_string(static_cast<int>(std::lround(x)) + 1); });
    }
    // The edited envelope (a copy; committed on change).
    const int kind = wsPage ? -1 : ui.part == 0 ? KPattern0 + ed::ival(st, OfPattern) : ui.part == 1 ? KLfo : KMapX + (ui.part - 2);
    const bool add = additiveTarget(tgt);
    OEnv e;
    int ls = -1, le = -1;
    bool exists = false;
    if (wsPage) {
      if (const OEnv* w = bank.wsCurve(u)) e = *w, exists = true;
      else defaultWs(e);
    } else if (const OEnv* src = bank.env(u, tgt, kind)) {
      e = *src, exists = true;
      if (kind < kPatterns) ls = bank.loopS[u][tgt][kind], le = bank.loopE[u][tgt][kind];
    } else {
      float neutral = add || kind >= KMapX ? 0.5f : 1.0f;
      if (kind == KLfo) neutral = 0.5f;
      if (!add && kind >= KMapX) neutral = 1.0f;
      double len = kind < kPatterns ? 4.0 : 1.0;
      e.add(0, neutral), e.add(len, neutral, 0, menv::SingleCurve);
      e.prepare();
      if (kind < kPatterns) ls = 0, le = 1;
    }
    const bool beats = !wsPage && kind < kPatterns;
    double xMax = beats ? std::max(4.0, std::ceil(e.length() / 4.0 - 1e-9) * 4.0) : 1.0;
    envui::Style es;
    es.bg = sk::col(0x232A2E);
    es.grid = IM_COL32(255, 255, 255, 14);
    es.gridStrong = IM_COL32(255, 255, 255, 34);
    es.line = sk::col(0xFDA73F);
    es.fill = theme::withAlpha(es.line, 0.12f);
    es.point = IM_COL32(0xF0, 0xF0, 0xF0, 255);
    es.pointHot = sk::col(0xFDA73F);
    es.handle = IM_COL32(0xF0, 0xF0, 0xF0, 170);
    es.text = es.point;
    envui::Opts o;
    o.xMax = xMax;
    o.gridDiv = beats ? 4 : 8;
    o.gridEvery = 4;
    o.snapDiv = ui.snap ? (beats ? 4 : 16) : 0;
    o.yLines = 4;
    o.centreLine = add || wsPage;
    o.freeze = ui.freeze, o.step = ui.step && beats, o.slide = ui.slide;
    o.lockEnds = !beats;
    if (beats)
      o.extraMenu = [&](int i) {
        ImGui::TextDisabled("Loop");
        bool ch = false;
        if (ImGui::MenuItem("Set loop start here", nullptr, ls == i)) ls = i, ch = true;
        if (ImGui::MenuItem("Set loop end here", nullptr, le == i)) le = i, ch = true;
        if (ImGui::MenuItem("No loop", nullptr, ls < 0)) ls = le = -1, ch = true;
        ImGui::Separator();
        return ch;
      };
    ImVec2 e0 = v.p(4, 235), e1 = v.p(439, 364);
    int r = envui::edit("##ofenv", e0, e1, e, ui.env, o, es);
    if (beats && ls >= 0 && le > ls && le < e.n) {  // loop markers
      float xs = e0.x + static_cast<float>(e.x[ls] / xMax) * (e1.x - e0.x), xe = e0.x + static_cast<float>(e.x[le] / xMax) * (e1.x - e0.x);
      dl->AddRectFilled(ImVec2(xs, e0.y), ImVec2(xe, e0.y + v.px(3)), theme::withAlpha(es.line, 0.6f));
    }
    if (!exists) sk::textAt(dl, ImVec2(e0.x, e0.y), ImVec2(e1.x, e0.y + v.px(16)), theme::withAlpha(pal.text, 0.4f), "(unused: edit to enable)", fs, 0);
    if (r & envui::Changed) {
      ls = std::min(ls, e.n - 1), le = std::min(le, e.n - 1);
      std::string data = wsPage ? replaceLine(st.data, wsKey(u), wsLine(u, e)) : replaceLine(st.data, envKey(u, tgt, kind), envLine(u, tgt, kind, e, ls, le));
      setState(a, t, st, data, "edit envelope", 0x4F46000000000000ull ^ t.key() ^ static_cast<uint64_t>(u * 1000 + tgt * 20 + (kind + 1)));
    }
    if (r & envui::Released) a.doc.endGesture();
    // Time scroller + toolbar.
    {
      ImVec2 s0 = v.p(4, 365), s1 = v.p(285, 379);
      dl->AddRectFilled(s0, s1, sk::col(0x1C2226), v.px(2));
      dl->AddRectFilled(ImVec2(s0.x + 2, s0.y + 2), ImVec2(s1.x - 2, s1.y - 2), theme::withAlpha(es.line, 0.3f), v.px(2));
      char lb[40];
      if (beats) std::snprintf(lb, sizeof lb, "%g beats", xMax);
      else std::snprintf(lb, sizeof lb, "%s", wsPage ? "input -> output" : kind == KLfo ? "one cycle" : "source 0..1");
      sk::textAt(dl, s0, s1, pal.text, lb, fs, 0, true);
      auto tbtn = [&](const char* id, float x, bool on, void (*g)(ImDrawList*, ImVec2, ImVec2, ImU32), const char* hint) {
        return sk::buttonAt(v, id, x, 381, 22, 16, pal, on, nullptr, g, hint);
      };
      if (tbtn("ofmenu", 4, false, sk::glyphMenu, "Envelope options")) ImGui::OpenPopup("##ofenvm");
      if (tbtn("offreeze", 96, ui.freeze, sk::glyphSnow, "Freeze editing")) ui.freeze = !ui.freeze;
      if (tbtn("ofstep", 120, ui.step, sk::glyphSteps, "Step editing")) ui.step = !ui.step;
      if (tbtn("ofsnap", 144, ui.snap, sk::glyphMagnet, "Snap to grid")) ui.snap = !ui.snap;
      if (tbtn("ofslide", 168, ui.slide, sk::glyphSlide, "Slide remaining points")) ui.slide = !ui.slide;
      if (ImGui::BeginPopup("##ofenvm")) {
        std::string key = wsPage ? wsKey(u) : envKey(u, tgt, kind);
        if (ImGui::MenuItem("Copy")) ui.clip = pointsText(e);
        if (ImGui::MenuItem("Paste", nullptr, false, !ui.clip.empty())) {
          std::string line = (wsPage ? wsKey(u) + "|" : envKey(u, tgt, kind) + " 0 -1|") + ui.clip;
          setState(a, t, st, replaceLine(st.data, key, line), "paste envelope");
        }
        if (ImGui::MenuItem("Reset (unused)", nullptr, false, exists)) setState(a, t, st, replaceLine(st.data, key, ""), "reset envelope");
        if (beats && ImGui::BeginMenu("Length")) {
          for (double len : {1.0, 2.0, 4.0, 8.0, 16.0}) {
            char b[16];
            std::snprintf(b, sizeof b, "%g beats", len);
            if (ImGui::MenuItem(b)) {
              double sc = len / std::max(1e-6, e.length());
              for (int i = 0; i < e.n; ++i) e.x[i] *= sc;
              setState(a, t, st, replaceLine(st.data, key, envLine(u, tgt, kind, e, ls, le)), "envelope length");
            }
          }
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      if (ui.part == 1 && !wsPage) {  // LFO panel knobs
        static const char* nm[LfoFields] = {"SPD", "TNS", "SKW", "PW", "PHS"};
        for (int k = 0; k < LfoFields; ++k) {
          float x = 292 + k * 29.f;
          sk::wheelAt(a, t, v, "FEnvWheel", x, 364, 20, 20, lfoParam(u, tgt, k), pal);
          sk::textAt(dl, v.p(x - 4, 384), v.p(x + 24, 396), pal.textDim, nm[k], std::clamp(v.px(6.5f), 8.f, 11.f));
        }
      }
    }
  }

  // ------------------------------------------------------------ main panel
  {
    ImVec2 m0 = v.p(443, 0), m1 = v.p(670, 400);
    dl->AddRectFilled(m0, m1, sk::col(0x30383D));
    sk::textAt(dl, v.p(448, 2), v.p(510, 28), sk::col(0xB6C0C7), "OCTA", v.px(17), -1);
    sk::textAt(dl, v.p(448, 26), v.p(530, 52), sk::col(0xFDA73F), "FILTER", v.px(17), -1);
    {  // decorative octagon (stands where the reference shows its logo wheel)
      ImVec2 c = v.p(531, 20);
      ImVec2 pts[8];
      for (int k = 0; k < 8; ++k) {
        float an = 3.14159265f * (0.125f + k * 0.25f);
        pts[k] = ImVec2(c.x + std::cos(an) * v.px(14), c.y + std::sin(an) * v.px(14));
      }
      dl->AddPolyline(pts, 8, sk::col(0xFDA73F), ImDrawFlags_Closed, std::max(1.f, v.px(2)));
      int en = 0;
      for (int k = 0; k < kUnits; ++k) en += ed::bval(st, unitParam(k, UEnabled), k == 0) ? 1 : 0;
      char b[4];
      std::snprintf(b, sizeof b, "%d", en);
      sk::textAt(dl, ImVec2(c.x - v.px(10), c.y - v.px(10)), ImVec2(c.x + v.px(10), c.y + v.px(10)), pal.text, b, v.px(11), 0, true);
    }
    if (sk::button(v, "KbBtn", pal, !ui.hideLeft, nullptr,
                   [](ImDrawList* d, ImVec2 p0, ImVec2 p1, ImU32 c) {
                     d->AddRect(p0, p1, c, 1.f, 0, 1.2f);
                     d->AddRectFilled(p0, ImVec2((p0.x + p1.x) * 0.5f, p1.y), c);
                   },
                   "Show / hide the unit panel"))
      ui.hideLeft = !ui.hideLeft;
    if (sk::button(v, "MenuBtn", pal, false, nullptr, sk::glyphMenu, "Options")) ImGui::OpenPopup("##ofopts");
    if (ImGui::BeginPopup("##ofopts")) {
      if (ImGui::MenuItem("Copy unit")) {
        ui.clip.clear();
        for (int f = 0; f < UFields; ++f) ui.clip += std::to_string(P(unitParam(u, f))) + " ";
      }
      if (ImGui::MenuItem("Paste unit", nullptr, false, !ui.clip.empty() && ui.clip.find('|') == std::string::npos)) {
        PluginState ns = st;
        const char* p = ui.clip.c_str();
        char* end = nullptr;
        for (int f = 0; f < UFields; ++f) {
          double x = std::strtod(p, &end);
          if (end == p) break;
          ns.params[static_cast<size_t>(unitParam(u, f))] = static_cast<float>(x);
          p = end;
        }
        setPluginState(a, t, ns, "paste unit");
      }
      ImGui::Separator();
      auto tog = [&](const char* label, int k) {
        bool on = ed::bval(st, k);
        if (ImGui::MenuItem(label, nullptr, on)) a.doc.setParam(t.pid(k), on ? 0 : 1, false);
      };
      tog("Sync pattern changes", OfSyncPatterns);
      tog("HQ shaper", OfHqShaper);
      ImGui::EndPopup();
    }
    if (sk::button(v, "InfoBtn", pal, false, "i", {}, "About")) ImGui::OpenPopup("##ofinfo");
    if (ImGui::BeginPopup("##ofinfo")) {
      ImGui::TextUnformatted("Octa Filter: eight filter units with envelopes, LFOs and X / Y / follower maps.");
      ImGui::EndPopup();
    }
    sk::bevel(v, "FeedbackBevel", pal);
    {  // XY pad
      const sk::FormCtl& c = v.c("ModVisual");
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      dl->AddRectFilled(p0, p1, sk::col(0x232A2E), v.px(2));
      for (int k = 1; k < 4; ++k) {
        float x = p0.x + (p1.x - p0.x) * k / 4, y = p0.y + (p1.y - p0.y) * k / 4;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 18));
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 18));
      }
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushID("xy");
      ImGui::InvisibleButton("##xy", ImVec2(p1.x - p0.x, p1.y - p0.y));
      if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        double x = std::clamp((m.x - p0.x) / (p1.x - p0.x), 0.f, 1.f), y = std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.f, 1.f);
        a.doc.setParam(t.pid(OfModX), x, true);
        a.doc.setParam(t.pid(OfModY), y, true);
      }
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      if (ImGui::IsItemHovered()) a.setHint("Modulation X / Y: drag");
      ImGui::PopID();
      float x = p0.x + static_cast<float>(P(OfModX)) * (p1.x - p0.x), y = p1.y - static_cast<float>(P(OfModY)) * (p1.y - p0.y);
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), theme::withAlpha(pal.fill, 0.4f));
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), theme::withAlpha(pal.fill, 0.4f));
      dl->AddCircleFilled(ImVec2(x, y), v.px(5), pal.fill);
      dl->AddCircle(ImVec2(x, y), v.px(5), IM_COL32(0, 0, 0, 180));
      if (!t.isChannel())
        if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {  // follower level
          float f = std::clamp(rt->bands[0].load(), 0.f, 1.f);
          dl->AddRectFilled(ImVec2(p1.x - v.px(4), p1.y - f * (p1.y - p0.y)), p1, theme::withAlpha(pal.text, 0.5f));
        }
    }
    sk::check(a, t, v, "ModSmoothBtn", OfSmooth, pal, "SMOOTH");
    sk::wheel(a, t, v, "ModXWheel", OfModX, pal);
    sk::wheel(a, t, v, "ModYWheel", OfModY, pal);
    lbl("ModXWheelLabel", "X");
    lbl("ModYWheelLabel", "Y");
    lbl("ModulationLabel", "MOD");
    sk::slider(a, t, v, "MainVolSlider", OfMaster, pal);
    sk::slider(a, t, v, "MainLFOVolSlider", OfMasterLfo, pal);
    sk::slider(a, t, v, "IEFVolSlider", OfIefLevel, pal);
    lbl("MainVolSliderLabel", "VOL");
    lbl("MainLFOVolSliderLabel", "LFO");
    lbl("IEFVolSliderLabel", "FOL");
    sk::label(v, "IEFBevelLabel", pal, "FOLLOW", 0, true);
    sk::bevel(v, "IEFBevel", pal);
    sk::check(a, t, v, "IEFUseF1Btn", OfIefUse1, pal, "USE1");
    sk::wheel(a, t, v, "IEFAttWheel", OfIefAttack, pal);
    sk::wheel(a, t, v, "IEFRelWheel", OfIefRelease, pal);
    lbl("IEFAttWheelLabel", "ATT");
    lbl("IEFRelWheelLabel", "REL");
    sk::check(a, t, v, "GlobalCenterBtn", OfCenter, pal, "CENTER");
    sk::check(a, t, v, "DQEnvBtn", OfHqEnvelopes, pal, "HQ ENVELOPES");
    sk::digi(a, t, v, "DQOverSelect", OfOversample, pal, [](double x) { return std::to_string(static_cast<int>(std::lround(x))); });
    lbl("DQOverSelectLabel", "OVERSAMPLING");
    {  // info text
      sk::bevel(v, "InfoEditBevel", pal);
      const sk::FormCtl& c = v.c("InfoEdit");
      ImVec2 p0 = v.p0(c), p1 = v.p1(c);
      size_t hh = std::hash<std::string>{}(st.data);
      if (hh != ui.infoHash && !ImGui::IsAnyItemActive()) {
        std::snprintf(ui.info, sizeof ui.info, "%s", infoText(st.data).c_str());
        ui.infoHash = hh;
      }
      ImGui::SetCursorScreenPos(p0);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(sk::col(0x374146)));
      ImGui::PushFont(nullptr, std::clamp(v.px(8.f), 10.f, 13.f));
      if (ImGui::InputTextMultiline("##ofinfo", ui.info, sizeof ui.info, ImVec2(p1.x - p0.x, p1.y - p0.y)))
        setState(a, t, st, withInfo(st.data, ui.info), "edit info", 0x4F49000000000000ull ^ t.key());
      if (ImGui::IsItemDeactivatedAfterEdit()) a.doc.endGesture();
      ImGui::PopFont();
      ImGui::PopStyleColor();
    }
  }
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(cur);
  ImGui::Dummy(ImVec2(shownW * scale, 400 * scale));
  return true;
}

}  // namespace gl::packs::creative
