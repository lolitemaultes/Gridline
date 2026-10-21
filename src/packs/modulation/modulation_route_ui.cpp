// Gridline — "modulation" pack: control surface, mixer send, bus send /
// return and sidechain ducker editors in the reference family's look
// (send: the reference send panel's dry | target | pan | volume row).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../core/I18n.h"
#include "../../core/PluginParams.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_skin.h"

namespace gl::packs::modulation {

namespace {

struct SurfaceItem {
  int kind = SkKnob;
  std::string label;
};
std::vector<SurfaceItem> parseSurface(const std::string& data) {
  const int lines = surfaceControlCount(data);
  const int count = std::clamp(((lines + 7) / 8) * 8, kSurfaceControls, kSurfaceMax);  // J-41: up to 64
  std::vector<SurfaceItem> v(static_cast<size_t>(count));
  for (int k = 0; k < count; ++k) {
    v[static_cast<size_t>(k)].kind = k < 8 ? SkKnob : (k < 12 ? SkSlider : (k < 14 ? SkButton : (k == 14 ? SkPadX : (k == 15 ? SkPadY : SkKnob))));
    v[static_cast<size_t>(k)].label = k == 14 ? "Pad" : "Control " + std::to_string(k + 1);
  }
  size_t p = 0;
  for (int k = 0; k < count && p < data.size(); ++k) {
    size_t e = data.find('\n', p);
    std::string line = data.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? data.size() : e + 1;
    if (!line.empty() && line[0] == '#') {  // kept FL surface (export)
      --k;
      continue;
    }
    if (line.empty()) continue;
    v[static_cast<size_t>(k)].kind = std::clamp(line[0] - '0', 0, 5);
    v[static_cast<size_t>(k)].label = line.size() > 2 ? line.substr(2) : "";
  }
  return v;
}
std::string formatSurface(const std::vector<SurfaceItem>& v, const std::string& old = {}) {
  std::string s;
  for (const auto& it : v) s += std::to_string(it.kind) + " " + it.label + "\n";
  const size_t fl = old.find("#fl ");  // keep an imported FL surface
  if (fl != std::string::npos && (fl == 0 || old[fl - 1] == '\n')) s += old.substr(fl);
  return s;
}

}  // namespace

// ----------------------------------------------------------- Control Surface
// Reference panel 540 × 454: edit / add buttons along the top, the control
// canvas underneath (here a two-row grid of the 16 controls).
void surfaceEditor(EditTarget& e) {
  const skin::Skin& K = sk::surface();
  if (!e.st) return;
  std::vector<SurfaceItem> items = parseSurface(e.st->data);
  static std::map<uint64_t, bool> editing;
  bool& edit = editing[e.key];
  const int count = static_cast<int>(items.size()), rows = (count + 7) / 8;
  constexpr float kW = 540;
  const float kH = 52 + 134.0f * static_cast<float>(rows);
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  if (sk::button("##edit", F.p(3, 3), F.sz(76, 38), K, edit ? "DONE" : "EDIT", edit)) edit = !edit;
  sk::text(dl, F, 90, 16, K, edit ? "Choose a type and name per control" : "Each control is an output you can link to any knob", 8.0f);
  sk::text(dl, F, 384, 16, K, "SMOOTH", 8.0f);
  sk::digi(e, SfSmooth, F.p(432, 10), F.sz(104, 24), K);
  const float cx0 = 3, cy0 = 46, cw = 534.0f / 8, ch = 132;
  skin::graphWell(dl, F.p(cx0, cy0), F.p(cx0 + 534, cy0 + (ch + 2) * static_cast<float>(rows)), K);
  for (int g = 1; g < 8; ++g) dl->AddLine(F.p(cx0 + g * cw, cy0 + 4), F.p(cx0 + g * cw, cy0 + (ch + 2) * static_cast<float>(rows) - 4), K.graphGrid);
  for (int r = 1; r < rows; ++r) dl->AddLine(F.p(cx0 + 4, cy0 + (ch + 2) * static_cast<float>(r) - 1), F.p(cx0 + 530, cy0 + (ch + 2) * static_cast<float>(r) - 1), K.graphGrid);
  for (int k = 0; k < count; ++k) {
    ImGui::PushID(k);
    const int pk = surfaceParam(k);
    SurfaceItem& it = items[static_cast<size_t>(k)];
    const float x = cx0 + (k % 8) * cw, y = cy0 + (k / 8) * (ch + 2);
    if (edit) {
      static const char* kinds[] = {"Knob", "Slider", "Button", "Toggle", "Pad X", "Pad Y"};
      ImGui::SetCursorScreenPos(F.p(x + 4, y + 10));
      ImGui::SetNextItemWidth((cw - 8) * F.s);
      int kind = it.kind;
      if (ImGui::Combo("##kind", &kind, kinds, 6)) {
        it.kind = kind;
        PluginState st = *e.st;
        st.data = formatSurface(items, e.st->data);
        e.set(st, "edit surface", 0);
      }
      char buf[48];
      std::snprintf(buf, sizeof buf, "%s", it.label.c_str());
      ImGui::SetCursorScreenPos(F.p(x + 4, y + 44));
      ImGui::SetNextItemWidth((cw - 8) * F.s);
      if (ImGui::InputText("##lbl", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        it.label = buf;
        PluginState st = *e.st;
        st.data = formatSurface(items, e.st->data);
        e.set(st, "rename control", 0);
      }
    } else {
      ParamId pid = e.pid(pk);
      bool padY = it.kind == SkPadY && k > 0 && items[static_cast<size_t>(k - 1)].kind == SkPadX;
      switch (it.kind) {
        case SkSlider: sk::vslider(e, pk, F.p(x + cw * 0.5f - 13, y + 8), F.sz(26, 96), K); break;
        case SkButton:
        case SkToggle: {
          bool on = e.p(pk) >= 0.5f;
          sk::button("##b", F.p(x + 7, y + 36), F.sz(cw - 14, 40), K, it.kind == SkButton ? "PRESS" : (on ? "ON" : "OFF"), on);
          if (it.kind == SkButton) {
            if (ImGui::IsItemActivated()) e.a->doc.setParam(pid, 1, false);
            if (ImGui::IsItemDeactivated()) e.a->doc.setParam(pid, 0, false);
          } else if (ImGui::IsItemClicked()) {
            e.a->doc.setParam(pid, on ? 0 : 1, false);
          }
          break;
        }
        case SkPadX:
        case SkPadY: {
          if (it.kind == SkPadX && k + 1 < count) {
            const int pk2 = surfaceParam(k + 1);
            ImVec2 p0 = F.p(x + 6, y + 8), sz = F.sz(cw - 12, 96);
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("##pad", sz);
            dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), skin::lighten(K.graphBg, 0.05f), 3);
            dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), K.graphGridStrong, 3);
            float px = e.p(pk), py = e.p(pk2);
            ImVec2 c(p0.x + px * sz.x, p0.y + (1 - py) * sz.y);
            dl->AddCircleFilled(c, 6, theme::withAlpha(K.fill, 0.3f), 16);
            dl->AddCircle(c, 6, K.fill, 16, 1.6f);
            if (ImGui::IsItemActive()) {
              PluginState st = *e.st;
              ImGuiIO& io = ImGui::GetIO();
              st.params[static_cast<size_t>(pk)] = std::clamp((io.MousePos.x - p0.x) / sz.x, 0.0f, 1.0f);
              st.params[static_cast<size_t>(pk2)] = std::clamp(1 - (io.MousePos.y - p0.y) / sz.y, 0.0f, 1.0f);
              e.set(st, "move pad", e.key ^ 0x53504144ull ^ static_cast<uint64_t>(k));
            }
            if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
          }
          break;
        }
        default: sk::wheel(e, pk, F.p(x + cw * 0.5f - 22, y + 30), 44 * F.s, K); break;
      }
      if (!padY) {
        ImGui::PushFont(e.a->fontMonoBold, F.font(8.0f));
        ImVec2 ts = ImGui::CalcTextSize(it.label.c_str());
        ImVec2 l0 = F.p(x + 2, y + 110), l1 = F.p(x + cw - 2, y + 128);
        dl->PushClipRect(l0, l1, true);
        dl->AddText(ImVec2(std::round(std::max(l0.x, (l0.x + l1.x - ts.x) * 0.5f)), std::round((l0.y + l1.y - ts.y) * 0.5f)), K.textDim,
                    it.label.c_str());
        dl->PopClipRect();
        ImGui::PopFont();
      }
    }
    ImGui::PopID();
  }
  F.end(0, kH);
}

// ------------------------------------------------------------- routing plugins
// Reference panel 258 × 84: dry wheel | target selector | pan | volume.
void sendEditor(EditTarget& e) {
  const Song& s = e.a->doc.song();
  const skin::Skin& K = sk::send();
  constexpr float kW = 258, kH = 84;
  skin::Form F = sk::openForm(e, K, kW, kH, 300);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::wheel(e, SnDry, F.p(6, 6), 56 * F.s, K);
  sk::text(dl, F, 34, 66, K, "DRY", 7.5f, true);
  int target = e.ip(SnTarget, 1);
  // J-45 route numbering: the target is the n-th route of this track.
  const bool routed = e.p(SnRoute, 0) >= 0.5f;
  const int self = s.mixerIndex(e.owner);
  const int dest = routed ? flRouteOutput(s, self, target) : target;
  std::string name = dest >= 0 && static_cast<size_t>(dest) < s.mixer.size() ? mixerTrackName(s, static_cast<size_t>(dest)) : "(none)";
  {
    char num[8];
    std::snprintf(num, sizeof num, "%d", target);
    ImVec2 t0 = F.p(70, 12), t1 = F.p(126, 44);
    ImGui::SetCursorScreenPos(t0);
    if (ImGui::InvisibleButton("##target", ImVec2(t1.x - t0.x, t1.y - t0.y))) ImGui::OpenPopup("##targets");
    bool hov = ImGui::IsItemHovered();
    if (hov) e.a->setHint("Mixer track that receives the copy of the audio here: " + name + "   (click to choose)");
    skin::Skin kk = K;
    if (hov) kk.lcdText = skin::lighten(K.lcdText, 0.3f);
    skin::lcd(dl, t0, t1, kk, num, F.font(13.0f));
    if (ImGui::BeginPopup("##targets")) {
      if (routed) {
        const std::vector<int> outs = flRouteOutputs(s, self);
        for (size_t k = 1; k < outs.size(); ++k) {
          std::string l = "Route " + std::to_string(k) + "  " + mixerTrackName(s, static_cast<size_t>(outs[k]));
          if (ImGui::MenuItem(l.c_str(), nullptr, static_cast<int>(k) == target)) e.setParam(SnTarget, static_cast<double>(k), false);
        }
        if (ImGui::MenuItem(i18n::tr("Number by mixer track"))) {
          e.setParam(SnTarget, dest >= 0 ? dest : 0, false);
          e.setParam(SnRoute, 0, true);
        }
      } else {
        for (size_t i = 0; i < s.mixer.size(); ++i) {
          if (s.mixer[i].id == e.owner) continue;
          std::string l = std::to_string(i) + "  " + mixerTrackName(s, i);
          if (ImGui::MenuItem(l.c_str(), nullptr, static_cast<int>(i) == target)) e.setParam(SnTarget, static_cast<double>(i), false);
        }
      }
      ImGui::EndPopup();
    }
    dl->AddRect(F.p(68, 48), F.p(128, 76), K.select, 3, 0, 1.0f);
    ImGui::PushFont(e.a->fontMonoBold, F.font(7.0f));
    dl->PushClipRect(F.p(68, 48), F.p(128, 76), true);
    std::string shown = "TO " + name;
    ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    ImVec2 c = F.p(98, 62);
    dl->AddText(ImVec2(std::round(std::max(F.p(70, 0).x, c.x - ts.x * 0.5f)), std::round(c.y - ts.y * 0.5f)), K.text, shown.c_str());
    dl->PopClipRect();
    ImGui::PopFont();
  }
  sk::wheel(e, SnPan, F.p(134, 6), 56 * F.s, K, skin::WheelBigLine, K.accent2C());
  sk::text(dl, F, 162, 66, K, "PAN", 7.5f, true);
  sk::wheel(e, SnLevel, F.p(196, 6), 56 * F.s, K);
  sk::text(dl, F, 224, 66, K, "VOLUME", 7.5f, true);
  F.end(0, kH);
}

// Bus send / return: the send panel's layout with a bus selector.
void busEditor(EditTarget& e, bool send) {
  const Song& s = e.a->doc.song();
  const skin::Skin& K = sk::send();
  int bus = e.ip(BsBus);
  constexpr float kW = 258, kH = 132;
  skin::Form F = sk::openForm(e, K, kW, kH, 300);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::wheel(e, send ? static_cast<int>(BsDry) : static_cast<int>(BrDry), F.p(6, 6), 56 * F.s, K);
  sk::text(dl, F, 34, 66, K, send ? "DRY" : "OWN", 7.5f, true);
  sk::text(dl, F, 98, 8, K, "BUS", 7.5f, true);
  sk::menuBox(e, BsBus, F.p(68, 20), F.sz(60, 26), K);
  sk::text(dl, F, 98, 52, K, send ? "SEND" : "RETURN", 7.5f, true);
  sk::wheel(e, send ? static_cast<int>(BsLevel) : static_cast<int>(BrLevel), F.p(158, 6), 56 * F.s, K);
  sk::text(dl, F, 186, 66, K, send ? "LEVEL" : "RETURN", 7.5f, true);
  sk::panelBox(dl, F, 6, 80, 246, 46, K, send ? "RECEIVED BY" : "SENT FROM");
  std::string list;
  int found = 0;
  for (size_t i = 0; i < s.mixer.size(); ++i)
    for (size_t k = 0; k < s.mixer[i].slots.size(); ++k) {
      const PluginState& ps = s.mixer[i].slots[k].plugin;
      if (ps.type != (send ? "gl.busreturn" : "gl.bussend") || ps.params.empty() || static_cast<int>(ps.params[0] + 0.5f) != bus) continue;
      list += (found ? ", " : "") + mixerTrackName(s, i) + " #" + std::to_string(k + 1);
      ++found;
    }
  if (!found) list = send ? "No return on this bus yet" : "Nothing sends to this bus yet";
  ImGui::PushFont(e.a->fontMono, F.font(7.5f));
  dl->PushClipRect(F.p(10, 96), F.p(248, 124), true);
  dl->AddText(F.p(12, 102), found ? K.text : K.textDim, list.c_str());
  dl->PopClipRect();
  ImGui::PopFont();
  F.end(0, kH);
}

// Sidechain ducker (no reference counterpart): controller-family panel with a
// ducking curve and a reduction meter.
void duckEditor(EditTarget& e) {
  const Song& s = e.a->doc.song();
  const skin::Skin& K = sk::controller();
  auto rt = e.a->builder.effectRuntime(e.owner, e.slot);
  float gr = rt ? rt->meter.load() : 0.0f;
  float in = rt ? rt->input.load() : -120.0f;
  constexpr float kW = 560, kH = 176;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 7, 7, 380, 162, K, "DUCKING");
  const int ps[5] = {SdThreshold, SdRatio, SdAttack, SdRelease, SdDepth};
  const char* nm[5] = {"THRES", "RATIO", "ATTACK", "RELEASE", "RANGE"};
  for (int i = 0; i < 5; ++i) sk::wheelL(e, ps[i], F, 18 + i * 73, 26, 42, K, nm[i]);
  sk::text(dl, F, 16, 92, K, "KEY", 7.5f);
  sk::selector(e, SdSource, F.p(44, 88), F.sz(220, 20), K);
  sk::check(e, SdListen, F.p(276, 88), F.sz(100, 20), K, "LISTEN");
  std::string keys;
  for (size_t i = 0; i < s.mixer.size(); ++i)
    for (const auto& sd : s.mixer[i].sends)
      if (sd.to == e.owner && sd.sidechain) keys += (keys.empty() ? "" : ", ") + mixerTrackName(s, i);
  ImGui::PushFont(e.a->fontMono, F.font(7.5f));
  dl->PushClipRect(F.p(14, 114), F.p(380, 132), true);
  dl->AddText(F.p(16, 117), keys.empty() ? K.textDim : K.text, keys.empty() ? "No sidechain route into this track yet." : ("Keyed by: " + keys).c_str());
  dl->PopClipRect();
  ImGui::PopFont();
  if (sk::button("##addkey", F.p(14, 138), F.sz(180, 24), K, "ADD SIDECHAIN FROM...")) ImGui::OpenPopup("##addkeys");
  if (ImGui::IsItemHovered()) e.a->setHint("Routes a track to this one as a sidechain-only send (it is not mixed in)");
  if (ImGui::BeginPopup("##addkeys")) {
    for (size_t i = 1; i < s.mixer.size(); ++i) {
      if (s.mixer[i].id == e.owner) continue;
      if (ImGui::MenuItem(mixerTrackName(s, i).c_str())) {
        e.a->doc.beginGroup("sidechain route");
        bool exists = false;
        for (const auto& sd : s.mixer[i].sends) exists = exists || sd.to == e.owner;
        if (exists || e.a->doc.setSend(s.mixer[i].id, e.owner, 1.0f)) e.a->doc.setSendSidechain(s.mixer[i].id, e.owner, true);
        e.a->doc.endGroup();
      }
    }
    ImGui::EndPopup();
  }
  sk::panelBox(dl, F, 395, 7, 158, 162, K, "CURVE");
  ImVec2 p0 = F.p(403, 26), p1 = F.p(515, 138);
  skin::graphWell(dl, p0, p1, K);
  const float sx = p1.x - p0.x, sy = p1.y - p0.y;
  for (int g = 1; g < 4; ++g) {
    dl->AddLine(ImVec2(p0.x + sx * g / 4, p0.y + 1), ImVec2(p0.x + sx * g / 4, p1.y - 1), K.graphGrid);
    dl->AddLine(ImVec2(p0.x + 1, p0.y + sy * g / 4), ImVec2(p1.x - 1, p0.y + sy * g / 4), K.graphGrid);
  }
  double thr = e.p(SdThreshold, -24), ratio = std::max(1.0f, e.p(SdRatio, 4)), range = e.p(SdDepth, 60);
  ImVec2 pts[40];
  for (int i = 0; i < 40; ++i) {
    double x = -60.0 + 60.0 * i / 39.0;
    double over = x - thr;
    double y = x - (over > 0 ? std::min(range, over * (1 - 1 / ratio)) : 0);
    pts[i] = ImVec2(p0.x + static_cast<float>((x + 60) / 60) * sx, p1.y - static_cast<float>((y + 60) / 60) * sy);
  }
  dl->AddPolyline(pts, 40, K.graphCurve, 0, 1.8f);
  if (in > -60) {
    double over = in - thr;
    double y = in - (over > 0 ? std::min(range, over * (1 - 1 / ratio)) : 0);
    dl->AddCircleFilled(ImVec2(p0.x + static_cast<float>((in + 60) / 60) * sx, p1.y - static_cast<float>((y + 60) / 60) * sy), 3.5f, K.text);
  }
  skin::meter(dl, F.p(522, 26), F.p(544, 138), K, gr, 24.0f, true);
  char buf[32];
  std::snprintf(buf, sizeof buf, "GR %.1f dB", static_cast<double>(gr));
  skin::lcd(dl, F.p(403, 144), F.p(544, 162), K, buf, F.font(8.0f));
  F.end(0, kH);
}

}  // namespace gl::packs::modulation
