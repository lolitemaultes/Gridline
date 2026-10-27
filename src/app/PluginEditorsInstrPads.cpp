// Gridline — Drum Pads editor in the reference pad-sampler layout (J-2).
// Reference form 790 × 367:
//   title (-1, 1, 128×32) · view / bank switch (125, 7)
//   pad strip y 37..61: prev / "Pad n/16" / next / options (8..132, 41), pad name
//     (202, 43), volume (301, 37, 24) · pan (335) · tune (375) wheels, mute (415, 40)
//     · solo (447) · velocity scaling (479), note (594, 44), cut (648, 42),
//     cut by (702, 42), output (754, 42)
//   pads 4 × 4 (60 × 56) at x 14 / 81 / 148 / 215, rows y 79 / 144 / 209 / 274
//     (pads 13..16 on top, 1..4 at the bottom), bank selector (14, 334, 261×19)
//   layer tools y 73: create (299) · delete (357) · up / down (417) · spread even
//     (440) · lock (522) · test range (547, 213×18) · trigger mode (762)
//   layer list (300, 99, 479×118), rows of 29: select, volume / pan / tune wheels
//     (37 / 72 / 107), waveform (140, 101×20), velocity range (247, 215×24)
//   sample view (301, 220, 190×117) + name / load / reverse (303..490, 341)
//   envelope editor (495, 220, 269×137) with volume / pan tabs (766, 220 / 288)
#include "KeyLight.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../core/I18n.h"
#include "../core/PluginParams.h"
#include "../engine/Slicing.h"
#include "InstrumentSkin.h"
#include "PluginEditorsCommon.h"
#include "../core/Platform.h"

namespace gl {

bool drawPadsEditorJ2(App& a, const PluginTarget& t, float width);

namespace {

using namespace pp;
using iskin::lerp;
using iskin::rgb;
namespace fs = std::filesystem;

constexpr float kFpcW = 790, kFpcH = 367;

iskin::Palette fpcPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x3A4146);
  p.bgBottom = rgb(0x2A3034);
  p.panel = rgb(0x262F34);
  p.panelLine = rgb(0x161B1E);
  p.rail = rgb(0x3A4247);
  p.fill = rgb(0xB2B2B2);
  p.fill2 = rgb(0xFFA346);   // pad lit / selection
  p.fill3 = rgb(0xFFBC40);   // velocity ranges
  p.cap = rgb(0x343D41);
  p.handle = rgb(0x81888D);
  p.label = rgb(0x8A9296);
  p.text = rgb(0xE6E8E9);
  p.textDim = rgb(0xA3A4A4);
  p.lcdBg = rgb(0x1A1F22);
  p.lcdText = rgb(0xFFFFFF);
  p.led = rgb(0xFFA346);
  p.ledOff = rgb(0x343D41);
  return p;
}

struct FpcUi {
  int pad = 0, layer = 0, envTab = 0, bank = 0, scroll = 0;
  int dragPoint = -1, dragRange = -1;
  bool lockLayers = false;
};
std::map<uint64_t, FpcUi> fpcUi;

// ---- layer list of a pad (layers 1..4 in parameters, 5.. in the data text)
struct LayerInfo {
  std::string path;
  int lo = 0, hi = 127;
  float level = 1, pan = 0, tune = 0, start = 0, end = 1;
  bool reverse = false;
};

std::vector<LayerInfo> readLayers(const PluginState& st, int pad) {
  std::vector<LayerInfo> out;
  const int n = std::max(kPadLayers, padLayerCount(st.data, pad));
  int last = -1;
  std::vector<LayerInfo> all(static_cast<size_t>(n));
  for (int l = 0; l < n; ++l) {
    LayerInfo& L = all[static_cast<size_t>(l)];
    L.path = padSamplePath(st.data, pad, l);
    bool on;
    if (l < kPadLayers) {
      on = ed::bval(st, padL(pad, l, LayEnabled), l == 0);
      L.lo = ed::ival(st, padL(pad, l, LayVelLow)), L.hi = ed::ival(st, padL(pad, l, LayVelHigh), 127);
      L.level = ed::val(st, padL(pad, l, LayLevel), 1), L.pan = ed::val(st, padL(pad, l, LayPan)), L.tune = ed::val(st, padL(pad, l, LayTune));
      L.reverse = ed::bval(st, padL(pad, l, LayReverse));
      L.start = ed::val(st, padL(pad, l, LayStart)), L.end = ed::val(st, padL(pad, l, LayEnd), 1);
    } else {
      PadLayerX x;
      on = parsePadLayer(padDataGet(st.data, "S" + std::to_string(pad + 1) + "." + std::to_string(l + 1)), x);
      L.lo = x.lo, L.hi = x.hi, L.level = x.level, L.pan = x.pan, L.tune = x.tune, L.reverse = x.reverse;
    }
    const bool hasSample = L.path != "none" && !(L.path.empty() && (l > 0 || pad >= kPads));
    if (on && hasSample) last = l;
    if (!on) L.path = "none";
  }
  for (int l = 0; l <= last; ++l) out.push_back(all[static_cast<size_t>(l)]);
  return out;
}

void writeLayers(PluginState& st, int pad, const std::vector<LayerInfo>& ls) {
  const int before = std::max(kPadLayers, padLayerCount(st.data, pad));
  const int n = std::max(before, static_cast<int>(ls.size()));
  for (int l = 0; l < n; ++l) {
    const bool have = l < static_cast<int>(ls.size());
    const LayerInfo L = have ? ls[static_cast<size_t>(l)] : LayerInfo{"none"};
    const std::string sKey = "S" + std::to_string(pad + 1) + "." + std::to_string(l + 1);
    if (l < kPadLayers) {
      auto setp = [&](int k, double v) {
        if (static_cast<size_t>(k) < st.params.size()) st.params[static_cast<size_t>(k)] = static_cast<float>(v);
      };
      setp(padL(pad, l, LayEnabled), have ? 1 : 0);
      setp(padL(pad, l, LayVelLow), L.lo), setp(padL(pad, l, LayVelHigh), L.hi);
      setp(padL(pad, l, LayLevel), L.level), setp(padL(pad, l, LayPan), L.pan), setp(padL(pad, l, LayTune), L.tune);
      setp(padL(pad, l, LayReverse), L.reverse ? 1 : 0), setp(padL(pad, l, LayStart), L.start), setp(padL(pad, l, LayEnd), L.end);
      st.data = setPadSamplePath(st.data, pad, l, have ? L.path : "none");
    } else if (have) {
      PadLayerX x;
      x.lo = L.lo, x.hi = L.hi, x.level = L.level, x.pan = L.pan, x.tune = L.tune, x.reverse = L.reverse;
      st.data = padDataSet(st.data, sKey, formatPadLayer(x));
      st.data = setPadSamplePath(st.data, pad, l, L.path);
    } else {
      st.data = padDataSet(st.data, sKey, "");
      st.data = padDataSet(st.data, "L" + std::to_string(pad + 1) + "." + std::to_string(l + 1), "");
    }
  }
}

std::string padName(const PluginState& st, int pad) {
  std::string n = padDataGet(st.data, "N" + std::to_string(pad + 1));
  if (!n.empty()) return n;
  std::string path = padSamplePath(st.data, pad, 0);
  if (path == "none") return std::string();
  if (path.empty()) return pad < kPads && static_cast<size_t>(pad) < builtinKit().size() ? builtinKit()[static_cast<size_t>(pad)] : std::string();
  return fs::path(path).stem().string();
}

SamplePtr layerSample(const PluginState& st, int pad, int layer) {
  std::string path = padSamplePath(st.data, pad, layer);
  if (path == "none") return nullptr;
  if (path.empty()) {
    if (layer != 0 || pad >= kPads || static_cast<size_t>(pad) >= builtinKit().size()) return nullptr;
    path = "builtin:" + builtinKit()[static_cast<size_t>(pad)];
  }
  return sampleCache().get(path);
}

ImU32 padColour(const PluginState& st, int pad, ImU32 dflt) {
  std::string c = padDataGet(st.data, "C" + std::to_string(pad + 1));
  if (c.size() == 6) return rgb(static_cast<uint32_t>(std::strtoul(c.c_str(), nullptr, 16)));
  return dflt;
}

int padNoteOf(const PluginState& st, int k) {
  int n = ed::ival(st, padE(k, PadNote), -1);
  return n >= 0 ? n : ed::ival(st, PadsBaseNote, 60) + k;
}

void openSample(App& a, const char* title, std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = title;
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = h ? h : ".";
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv"};
  a.fileDialog.onOk = std::move(ok);
}

// ---- envelope view / editor --------------------------------------------------
void drawEnvelope(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, FpcUi& ui, int pad) {
  const float ex = 495, ey = 220, ew = 269, eh = 103;
  iskin::bevel(k, ex, ey, ew, 137, rgb(0x50575A), k.c.panelLine, 2);
  iskin::bevel(k, ex, ey, ew, eh, rgb(0x5F686D), 0, 2);
  const char* key = ui.envTab == 0 ? ".v" : ".p";
  const std::string envKey = "E" + std::to_string(pad + 1) + key;
  PadEnvelope e;
  const bool has = parsePadEnvelope(padDataGet(st.data, envKey), e);
  ImDrawList* dl = k.dl();
  const ImVec2 p0 = k.at(ex + 6, ey + 6), p1 = k.at(ex + ew - 6, ey + eh - 6);
  // Grid.
  for (int i = 1; i < 4; ++i) {
    float y = p0.y + (p1.y - p0.y) * i / 4.0f;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), theme::withAlpha(IM_COL32_BLACK, i == 2 ? 0.22f : 0.12f));
  }
  auto toggleEnv = [&](bool on) {
    PluginState ns = st;
    PadEnvelope ne = e;
    if (!has) {  // a fresh envelope: volume decay / centred pan
      ne = PadEnvelope{};
      ne.n = 2;
      ne.v[0] = ui.envTab == 0 ? 1.0f : 0.5f, ne.t[1] = 1.0f, ne.v[1] = ui.envTab == 0 ? 0.0f : 0.5f, ne.ten[1] = 0.2f;
    }
    ne.on = on;
    ns.data = padDataSet(st.data, envKey, formatPadEnvelope(ne));
    setPluginState(a, t, ns, on ? "pad envelope on" : "pad envelope off");
  };
  const bool on = has && e.on;
  if (!on && ui.envTab == 0) {
    // The pad's AHDSR (used while the envelope is off): curve + wheels.
    const double at = ed::val(st, padE(pad, PadAttack)), ho = std::min(2.0, static_cast<double>(ed::val(st, padE(pad, PadHold), 10))),
                 de = std::min(4.0, static_cast<double>(ed::val(st, padE(pad, PadDecay), 10))), su = ed::val(st, padE(pad, PadSustain), 1),
                 re = ed::val(st, padE(pad, PadRelease), 0.05f);
    const double span = at + ho + de + 0.3 + re;
    auto X = [&](double s) { return p0.x + static_cast<float>(s / span) * (p1.x - p0.x); };
    auto Y = [&](double v) { return p1.y - static_cast<float>(v) * (p1.y - p0.y); };
    ImVec2 pts[6] = {ImVec2(X(0), Y(0)), ImVec2(X(at), Y(1)), ImVec2(X(at + ho), Y(1)), ImVec2(X(at + ho + de), Y(su)),
                     ImVec2(X(at + ho + de + 0.3), Y(su)), ImVec2(X(span), Y(0))};
    dl->AddPolyline(pts, 6, theme::withAlpha(IM_COL32_WHITE, 0.55f), 0, std::max(1.0f, k.px(1.4f)));
    iskin::text(k, ex + 8, ey + 4, 160, 12, "AHDSR (envelope off)", 8.5f, rgb(0x2C3338), 0, false, true);
    const char* caps[5] = {"ATT", "HOLD", "DEC", "SUS", "REL"};
    const int ps[5] = {padE(pad, PadAttack), padE(pad, PadHold), padE(pad, PadDecay), padE(pad, PadSustain), padE(pad, PadRelease)};
    for (int i = 0; i < 5; ++i) {
      iskin::wheel(a, t, k, ps[i], ex + 60 + i * 30, ey + 106, 18, k.c.fill2);
      iskin::text(k, ex + 52 + i * 30, ey + 125, 34, 10, caps[i], 7.5f, k.c.textDim, 1, false, true);
    }
  } else if (has) {
    // Points: drag to move (time / value), double-click empty space to add,
    // right-click a point for sustain / delete.
    double total = 0;
    for (int i = 1; i < e.n; ++i) total += e.t[i];
    const double span = std::max(0.25, total * 1.1);
    auto X = [&](double s) { return p0.x + static_cast<float>(s / span) * (p1.x - p0.x); };
    auto Y = [&](double v) { return p1.y - static_cast<float>(v) * (p1.y - p0.y); };
    ImVec2 pts[PadEnvelope::kMax];
    double acc = 0;
    for (int i = 0; i < e.n; ++i) {
      acc += i ? e.t[i] : 0.0;
      pts[i] = ImVec2(X(acc), Y(e.v[i]));
    }
    const ImU32 lc = on ? IM_COL32_WHITE : theme::withAlpha(IM_COL32_WHITE, 0.35f);
    for (int i = 1; i < e.n; ++i) {
      ImVec2 prev = pts[i - 1];
      for (int s = 1; s <= 16; ++s) {
        double x = s / 16.0;
        ImVec2 q(prev.x + (pts[i].x - prev.x) * static_cast<float>(x),
                 Y(e.v[i - 1] + (e.v[i] - e.v[i - 1]) * padEnvCurve(e.ten[i], x)));
        dl->AddLine(prev, q, lc, std::max(1.0f, k.px(1.5f)));
        prev = q;
      }
    }
    if (e.sustain >= 0) dl->AddLine(ImVec2(pts[e.sustain].x, p0.y), ImVec2(pts[e.sustain].x, p1.y), theme::withAlpha(k.c.fill2, 0.8f));
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##envarea", ImVec2(p1.x - p0.x, p1.y - p0.y));
    const bool hovArea = ImGui::IsItemHovered();
    ImVec2 m = ImGui::GetIO().MousePos;
    int hovPt = -1;
    for (int i = 0; i < e.n; ++i) {
      float d2 = (m.x - pts[i].x) * (m.x - pts[i].x) + (m.y - pts[i].y) * (m.y - pts[i].y);
      if (d2 < 49 && hovArea) hovPt = i;
      dl->AddCircleFilled(pts[i], k.px(3.2f), i == ui.dragPoint || i == hovPt ? k.c.fill2 : lc);
    }
    if (ImGui::IsItemActivated()) ui.dragPoint = hovPt;
    if (ImGui::IsItemActive() && ui.dragPoint >= 0 && on) {
      PadEnvelope ne = e;
      const int i = ui.dragPoint;
      double before = 0;
      for (int j = 1; j < i; ++j) before += e.t[j];
      double tm = std::max(0.0, (m.x - p0.x) / (p1.x - p0.x) * span);
      if (i > 0) ne.t[i] = static_cast<float>(std::max(0.0, tm - before));
      ne.v[i] = static_cast<float>(std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.0f, 1.0f));
      PluginState ns = st;
      ns.data = padDataSet(st.data, envKey, formatPadEnvelope(ne));
      setPluginState(a, t, ns, "edit pad envelope", t.key() * 31 + 7);
    }
    if (ImGui::IsItemDeactivated()) ui.dragPoint = -1;
    if (hovArea && hovPt < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && on && e.n < PadEnvelope::kMax) {
      double tm = (m.x - p0.x) / (p1.x - p0.x) * span, a0 = 0;
      int at = e.n;
      for (int i = 1; i < e.n; ++i) {
        if (a0 + e.t[i] > tm) {
          at = i;
          break;
        }
        a0 += e.t[i];
      }
      PadEnvelope ne = e;
      for (int i = e.n; i > at; --i) ne.t[i] = ne.t[i - 1], ne.v[i] = ne.v[i - 1], ne.ten[i] = ne.ten[i - 1];
      ne.t[at] = static_cast<float>(tm - a0);
      if (at < e.n) ne.t[at + 1] = std::max(0.0f, e.t[at] - ne.t[at]);
      ne.v[at] = static_cast<float>(std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.0f, 1.0f));
      ne.ten[at] = 0;
      ++ne.n;
      if (ne.sustain >= at) ++ne.sustain;
      PluginState ns = st;
      ns.data = padDataSet(st.data, envKey, formatPadEnvelope(ne));
      setPluginState(a, t, ns, "add envelope point");
    }
    static int menuPt = -1;
    if (hovPt >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      menuPt = hovPt;
      ImGui::OpenPopup("##envpt");
    }
    if (ImGui::BeginPopup("##envpt")) {
      PadEnvelope ne = e;
      bool changed = false;
      if (ImGui::MenuItem(i18n::tr("Sustain at this point"), nullptr, e.sustain == menuPt)) ne.sustain = e.sustain == menuPt ? -1 : menuPt, changed = true;
      if (menuPt > 0 && ImGui::MenuItem(i18n::tr("Delete point"))) {
        for (int i = menuPt; i + 1 < e.n; ++i) ne.t[i] = ne.t[i + 1], ne.v[i] = ne.v[i + 1], ne.ten[i] = ne.ten[i + 1];
        if (menuPt + 1 < e.n) ne.t[menuPt] = e.t[menuPt] + e.t[menuPt + 1];
        --ne.n;
        if (ne.sustain == menuPt) ne.sustain = -1;
        else if (ne.sustain > menuPt) --ne.sustain;
        changed = true;
      }
      if (changed) {
        PluginState ns = st;
        ns.data = padDataSet(st.data, envKey, formatPadEnvelope(ne));
        setPluginState(a, t, ns, "edit pad envelope");
      }
      ImGui::EndPopup();
    }
    if (hovArea) a.setHint("Envelope: drag points · double-click adds a point · right-click a point: sustain / delete");
    char info[48];
    std::snprintf(info, sizeof info, "%.2f s", total);
    iskin::text(k, ex + ew - 60, ey + 4, 54, 12, info, 8.5f, rgb(0x2C3338), 2, true, false);
  } else {
    iskin::text(k, ex, ey, ew, eh, "pan envelope off", 9, rgb(0x2C3338), 1, false, true);
  }
  // Bottom strip: on / off.
  if (iskin::button(k, "##envon", ex + 4, ey + 108, 50, 16, on ? "ON" : "OFF", on, 8.5f)) toggleEnv(!on);
  if (ImGui::IsItemHovered()) a.setHint(ui.envTab == 0 ? "Volume envelope on/off (off = the pad's AHDSR)" : "Pan envelope on/off");
  // Tabs (vertical): VOLUME / PAN.
  for (int tab = 0; tab < 2; ++tab) {
    const float ty = tab == 0 ? 220 : 288, th = tab == 0 ? 67 : 68;
    ImGui::SetCursorScreenPos(k.at(766, ty));
    ImGui::PushID(tab);
    if (ImGui::InvisibleButton("##etab", ImVec2(k.px(13), k.px(th)))) ui.envTab = tab;
    ImGui::PopID();
    const bool sel = ui.envTab == tab;
    iskin::bevel(k, 766, ty, 13, th, sel ? rgb(0x8A9094) : rgb(0x575757), k.c.panelLine, 2);
    // Vertical caption: one letter per line.
    const char* cap = tab == 0 ? "VOLUME" : "PAN";
    const int n = static_cast<int>(std::strlen(cap));
    for (int i = 0; i < n; ++i) {
      char ch[2] = {cap[i], 0};
      iskin::text(k, 766, ty + th * 0.5f - n * 4.5f + i * 9, 13, 9, ch, 7.5f, rgb(0x191919), 1, false, true);
    }
  }
}

// ---- layer list --------------------------------------------------------------
void drawLayers(App& a, const PluginTarget& t, const iskin::Skin& k, const PluginState& st, FpcUi& ui, int pad) {
  std::vector<LayerInfo> ls = readLayers(st, pad);
  ui.layer = std::clamp(ui.layer, 0, std::max(0, static_cast<int>(ls.size()) - 1));
  auto commit = [&](const std::vector<LayerInfo>& nl, const char* label) {
    PluginState ns = st;
    writeLayers(ns, pad, nl);
    setPluginState(a, t, ns, label);
  };
  // Tools row.
  if (iskin::button(k, "##lcreate", 299, 73, 54, 19, "CREATE", false, 8.5f)) {
    std::vector<LayerInfo> nl = ls;
    LayerInfo L;
    L.path = ls.empty() ? std::string("none") : ls[static_cast<size_t>(ui.layer)].path;
    if (static_cast<int>(nl.size()) < kPadMaxLayers) nl.push_back(L), ui.layer = static_cast<int>(nl.size()) - 1;
    commit(nl, "create layer");
  }
  if (iskin::button(k, "##ldelete", 357, 73, 54, 19, "DELETE", false, 8.5f) && !ls.empty()) {
    std::vector<LayerInfo> nl = ls;
    nl.erase(nl.begin() + ui.layer);
    ui.layer = std::max(0, ui.layer - 1);
    commit(nl, "delete layer");
  }
  if (iskin::button(k, "##lup", 417, 73, 13, 10, "", false) && ui.layer > 0) {
    std::vector<LayerInfo> nl = ls;
    std::swap(nl[static_cast<size_t>(ui.layer)], nl[static_cast<size_t>(ui.layer - 1)]);
    --ui.layer;
    commit(nl, "move layer up");
  }
  iskin::glyphArrow(k.dl(), k.at(423.5f, 78), k.px(3.5f), k.c.textDim, 2);
  if (iskin::button(k, "##ldown", 417, 82, 13, 10, "", false) && ui.layer + 1 < static_cast<int>(ls.size())) {
    std::vector<LayerInfo> nl = ls;
    std::swap(nl[static_cast<size_t>(ui.layer)], nl[static_cast<size_t>(ui.layer + 1)]);
    ++ui.layer;
    commit(nl, "move layer down");
  }
  iskin::glyphArrow(k.dl(), k.at(423.5f, 87), k.px(3.5f), k.c.textDim, 3);
  if (iskin::button(k, "##lspread", 440, 74, 79, 18, "SPREAD EVEN", false, 8.5f) && !ls.empty()) {
    std::vector<LayerInfo> nl = ls;
    const int n = static_cast<int>(nl.size());
    for (int i = 0; i < n; ++i) nl[static_cast<size_t>(i)].lo = i * 128 / n, nl[static_cast<size_t>(i)].hi = (i + 1) * 128 / n - 1;
    commit(nl, "spread layers");
  }
  if (iskin::button(k, "##llock", 522, 74, 19, 18, "", ui.lockLayers)) ui.lockLayers = !ui.lockLayers;
  {  // padlock glyph
    ImVec2 c = k.at(531.5f, 84);
    float r = k.px(4);
    // The SVG lock in the old padlock's ink (body to r, shackle ring of 0.65 r above it).
    ui::drawIconFit(k.dl(), ui::Icon::Lock, ImVec2(c.x - r, c.y - r * 1.05f - 0.6f), ImVec2(c.x + r, c.y + r), ui.lockLayers ? rgb(0x262F34) : k.c.textDim);
  }
  if (ImGui::IsItemHovered()) a.setHint("Lock layers: velocity ranges move together");
  // Test strip: click a velocity to audition the pad at it.
  {
    iskin::bevel(k, 547, 74, 213, 18, rgb(0x1F2529), k.c.panelLine, 2);
    ImDrawList* dl = k.dl();
    for (size_t i = 0; i < ls.size(); ++i) {
      float x0 = 548 + ls[i].lo / 127.0f * 211, x1 = 548 + (ls[i].hi + 1) / 128.0f * 211;
      dl->AddRectFilled(k.at(x0, 76 + i % 4 * 3.5f), k.at(x1, 79 + i % 4 * 3.5f), theme::withAlpha(k.c.fill3, i == static_cast<size_t>(ui.layer) ? 0.95f : 0.45f));
    }
    ImGui::SetCursorScreenPos(k.at(547, 74));
    ImGui::InvisibleButton("##ltest", ImVec2(k.px(213), k.px(18)));
    static int testNote = -1;
    if (ImGui::IsItemActivated() && t.isChannel()) {
      float v = std::clamp((ImGui::GetIO().MousePos.x - k.at(548, 0).x) / k.px(211), 0.01f, 1.0f);
      testNote = padNoteOf(st, pad);
      a.engine.noteOn(t.owner, testNote, v);
    }
    if (ImGui::IsItemDeactivated() && testNote >= 0) a.engine.noteOff(t.owner, testNote), testNote = -1;
    if (ImGui::IsItemHovered()) a.setHint("Test the pad: click at a velocity (left = soft, right = hard)");
  }
  // Trigger mode (overlapping layers): stack / cycle / random.
  {
    const int mode = ed::ival(st, padE(pad, PadLayerMode));
    if (iskin::button(k, "##ltrig", 762, 74, 19, 18, "", mode != LayerStack)) ImGui::OpenPopup("##trigmode");
    ImVec2 c = k.at(771.5f, 83);
    const float r = k.px(4.5f);
    const ImU32 gc = mode != LayerStack ? rgb(0x262F34) : k.c.textDim;
    ui::drawIconGlyph(k.dl(), ui::Icon::DiceTwo, c, r, 1.2f, gc);
    if (ImGui::IsItemHovered()) a.setHint("Overlapping layers: all at once, or one at a time (cycle / random)");
    if (ImGui::BeginPopup("##trigmode")) {
      static const char* names[5] = {"All layers", "Cycle", "Cycle backward", "Random", "Random (avoid previous)"};
      for (int i = 0; i < 5; ++i)
        if (ImGui::MenuItem(names[i], nullptr, mode == i)) a.doc.setParam(t.pid(padE(pad, PadLayerMode)), i, false);
      ImGui::EndPopup();
    }
  }
  // Rows.
  iskin::bevel(k, 300, 99, 479, 118, rgb(0x2C3338), k.c.panelLine, 0);
  const int rows = 4;
  const int maxScroll = std::max(0, static_cast<int>(ls.size()) - rows);
  ui.scroll = std::clamp(ui.scroll, 0, maxScroll);
  ImGui::SetCursorScreenPos(k.at(300, 99));
  ImGui::InvisibleButton("##lscroll", ImVec2(k.px(465), k.px(118)), ImGuiButtonFlags_AllowOverlap);
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && ImGui::GetIO().MouseWheel != 0)
    ui.scroll = std::clamp(ui.scroll - static_cast<int>(ImGui::GetIO().MouseWheel), 0, maxScroll);
  for (int r = 0; r < rows; ++r) {
    const int l = ui.scroll + r;
    if (l >= static_cast<int>(ls.size())) break;
    const LayerInfo& L = ls[static_cast<size_t>(l)];
    const float y = 99 + r * 29.0f;
    ImGui::PushID(l);
    iskin::bevel(k, 300, y, 465, 29, l == ui.layer ? rgb(0x4A5256) : rgb(0x3F4649), k.c.panelLine, 0);
    // Select button with the layer number.
    char num[32];
    std::snprintf(num, sizeof num, "%d", l + 1);
    if (iskin::button(k, "##lsel", 303, y + 3, 21, 22, num, l == ui.layer, 9)) ui.layer = l;
    // Volume / pan / tune (parameters for layers 1..4; data text for 5..).
    if (l < kPadLayers) {
      iskin::wheel(a, t, k, padL(pad, l, LayLevel), 337, y + 2, 24, rgb(0xCECECE), 0, rgb(0x30373A));
      iskin::wheel(a, t, k, padL(pad, l, LayPan), 372, y + 2, 24, rgb(0xCECECE), 0, rgb(0x30373A));
      iskin::wheel(a, t, k, padL(pad, l, LayTune), 407, y + 2, 24, rgb(0xCECECE), 0, rgb(0x30373A));
    } else {
      // Plain value readouts (drag to change) for the data-text layers.
      auto dragVal = [&](const char* id, float x, float v, float lo, float hi, int field) {
        ImGui::SetCursorScreenPos(k.at(x, y + 4));
        ImGui::InvisibleButton(id, ImVec2(k.px(28), k.px(20)));
        char b[16];
        std::snprintf(b, sizeof b, field == 2 ? "%+.1f" : "%.0f%%", static_cast<double>(field == 2 ? v : v * 100));
        iskin::lcd(k, x, y + 4, 28, 20, b, k.c.lcdBg, ImGui::IsItemActive() ? k.c.fill2 : k.c.text, 8);
        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0) {
          std::vector<LayerInfo> nl = ls;
          LayerInfo& M = nl[static_cast<size_t>(l)];
          float* f = field == 0 ? &M.level : field == 1 ? &M.pan : &M.tune;
          *f = std::clamp(*f - ImGui::GetIO().MouseDelta.y * (hi - lo) / 200.0f, lo, hi);
          PluginState ns = st;
          writeLayers(ns, pad, nl);
          setPluginState(a, t, ns, "edit layer", t.key() * 131 + static_cast<uint64_t>(l * 3 + field));
        }
      };
      dragVal("##lv", 335, L.level, 0, 1, 0);
      dragVal("##lp", 370, L.pan, -1, 1, 1);
      dragVal("##lt", 405, L.tune, -24, 24, 2);
    }
    // Waveform.
    iskin::bevel(k, 440, y + 4, 101, 20, rgb(0x2C3338), k.c.panelLine, 1);
    SamplePtr smp = layerSample(st, pad, l);
    iskin::wave(k.dl(), smp.get(), k.at(441, y + 5), k.at(540, y + 23), rgb(0x6FA8D6), 0, 1, theme::withAlpha(IM_COL32_WHITE, 0.1f));
    ImGui::SetCursorScreenPos(k.at(440, y + 4));
    ImGui::InvisibleButton("##lwave", ImVec2(k.px(101), k.px(20)));
    if (ImGui::IsItemClicked()) ui.layer = l;
    if (ImGui::IsItemHovered()) a.setHint((L.path == "none" ? std::string("no sample") : fs::path(L.path).filename().string()) + "   (drop a sample to replace)");
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          std::vector<LayerInfo> nl = ls;
          nl[static_cast<size_t>(l)].path = path;
          commit(nl, "load layer sample");
        }
      }
      ImGui::EndDragDropTarget();
    }
    // Velocity range: bar with draggable ends.
    {
      const float rx = 547, rw = 215;
      iskin::bevel(k, rx, y + 2, rw, 24, rgb(0x262C30), k.c.panelLine, 2);
      float x0 = rx + 2 + L.lo / 127.0f * (rw - 4), x1 = rx + 2 + L.hi / 127.0f * (rw - 4);
      k.dl()->AddRectFilled(k.at(x0, y + 11), k.at(x1, y + 17), k.c.fill3, k.px(2));
      k.dl()->AddRectFilled(k.at(x0 - 2, y + 6), k.at(x0 + 2, y + 22), k.c.handle, 1);
      k.dl()->AddRectFilled(k.at(x1 - 2, y + 6), k.at(x1 + 2, y + 22), k.c.handle, 1);
      char rb[24];
      std::snprintf(rb, sizeof rb, "%d-%d", L.lo, L.hi);
      iskin::text(k, rx + rw - 46, y + 2, 44, 10, rb, 7.5f, k.c.textDim, 2, true, false);
      ImGui::SetCursorScreenPos(k.at(rx, y + 2));
      ImGui::InvisibleButton("##lrange", ImVec2(k.px(rw), k.px(24)));
      if (ImGui::IsItemActivated()) {
        float mx = ImGui::GetIO().MousePos.x;
        ui.dragRange = l * 2 + (std::fabs(mx - k.at(x1, 0).x) < std::fabs(mx - k.at(x0, 0).x) ? 1 : 0);
        ui.layer = l;
      }
      if (ImGui::IsItemActive() && ui.dragRange / 2 == l) {
        int v = static_cast<int>(std::lround((ImGui::GetIO().MousePos.x - k.at(rx + 2, 0).x) / k.px(rw - 4) * 127));
        v = std::clamp(v, 0, 127);
        std::vector<LayerInfo> nl = ls;
        LayerInfo& M = nl[static_cast<size_t>(l)];
        if (ui.dragRange % 2 == 0) {
          int d = std::min(v, M.hi) - M.lo;
          M.lo = std::min(v, M.hi);
          if (ui.lockLayers && l > 0) nl[static_cast<size_t>(l - 1)].hi = std::clamp(nl[static_cast<size_t>(l - 1)].hi + d, 0, 127);
        } else {
          int d = std::max(v, M.lo) - M.hi;
          M.hi = std::max(v, M.lo);
          if (ui.lockLayers && l + 1 < static_cast<int>(nl.size())) nl[static_cast<size_t>(l + 1)].lo = std::clamp(nl[static_cast<size_t>(l + 1)].lo + d, 0, 127);
        }
        PluginState ns = st;
        writeLayers(ns, pad, nl);
        setPluginState(a, t, ns, "layer velocity range", t.key() * 17 + static_cast<uint64_t>(l));
      }
      if (ImGui::IsItemDeactivated()) ui.dragRange = -1;
      if (ImGui::IsItemHovered()) a.setHint("Velocity range of the layer: drag either end");
    }
    ImGui::PopID();
  }
  // Scroll bar.
  if (maxScroll > 0) {
    iskin::bevel(k, 767, 99, 10, 118, rgb(0x2C3338), k.c.panelLine, 2);
    float h = 118.0f * rows / static_cast<float>(ls.size()), y = 99 + (118 - h) * ui.scroll / static_cast<float>(maxScroll);
    iskin::bevel(k, 768, y + 1, 8, h - 2, rgb(0x5F686D), 0, 2);
  }
  if (ls.empty()) iskin::text(k, 300, 99, 465, 118, "No layers: drop a sample on the pad or press CREATE", 9, k.c.textDim, 1);
  // Sample view of the selected layer.
  {
    iskin::bevel(k, 301, 220, 190, 117, rgb(0xE9EAEA), k.c.panelLine, 2);
    SamplePtr smp = ls.empty() ? nullptr : layerSample(st, pad, ui.layer);
    double from = 0, to = 1;
    if (!ls.empty() && ui.layer < kPadLayers) from = ls[static_cast<size_t>(ui.layer)].start, to = ls[static_cast<size_t>(ui.layer)].end;
    iskin::wave(k.dl(), smp.get(), k.at(303, 222), k.at(489, 335), rgb(0x7A7A79), 0, 1, rgb(0xC4C5C5));
    if (smp && (from > 0 || to < 1)) {  // region outside the play range shaded
      k.dl()->AddRectFilled(k.at(303, 222), k.at(303 + 186 * static_cast<float>(from), 335), theme::withAlpha(IM_COL32_BLACK, 0.18f));
      k.dl()->AddRectFilled(k.at(303 + 186 * static_cast<float>(to), 222), k.at(489, 335), theme::withAlpha(IM_COL32_BLACK, 0.18f));
    }
    ImGui::SetCursorScreenPos(k.at(301, 220));
    ImGui::InvisibleButton("##swave", ImVec2(k.px(190), k.px(117)));
    if (ImGui::IsItemClicked() && smp) a.previewSample(smp->path);
    if (ImGui::IsItemHovered()) a.setHint("Click to preview the layer's sample · drop a sample here");
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          std::vector<LayerInfo> nl = ls;
          if (nl.empty()) nl.push_back(LayerInfo{});
          nl[static_cast<size_t>(std::min<int>(ui.layer, static_cast<int>(nl.size()) - 1))].path = path;
          commit(nl, "load layer sample");
        }
      }
      ImGui::EndDragDropTarget();
    }
    // Load / name / reverse.
    if (iskin::button(k, "##sload", 302, 341, 18, 15, "", false)) {
      PluginTarget tt = t;
      const int pk = pad, lk = ls.empty() ? 0 : ui.layer;
      openSample(a, "Load pad sample", [&a, tt, pk, lk](const std::string& p) {
        if (const PluginState* cur = tt.state(a.doc.song())) {
          PluginState ns = *cur;
          std::vector<LayerInfo> nl = readLayers(*cur, pk);
          if (nl.empty()) nl.push_back(LayerInfo{});
          nl[static_cast<size_t>(std::min<int>(lk, static_cast<int>(nl.size()) - 1))].path = p;
          writeLayers(ns, pk, nl);
          setPluginState(a, tt, ns, "load pad sample");
        }
      });
    }
    iskin::glyphFolder(k.dl(), k.at(311, 348.5f), k.px(5), k.c.textDim);
    std::string nm = ls.empty() || ls[static_cast<size_t>(ui.layer)].path == "none" ? std::string(i18n::tr("(empty)")) : fs::path(ls[static_cast<size_t>(ui.layer)].path).filename().string();
    if (!ls.empty() && ls[static_cast<size_t>(ui.layer)].path.empty()) nm = padName(st, pad) + " (built-in)";
    iskin::text(k, 324, 341, 145, 15, nm.c_str(), 9, k.c.textDim, 0);
    if (ui.layer < kPadLayers && !ls.empty()) {
      iskin::switchBtn(a, t, k, padL(pad, ui.layer, LayReverse), 472, 341, 18, 15, [&](ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool on) {
        dl->AddRectFilled(p0, p1, on ? k.c.fill2 : rgb(0x1D1D1D), 2);
        iskin::glyphArrow(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), (p1.y - p0.y) * 0.4f, on ? rgb(0x1D1D1D) : rgb(0xB0B1B2), 0);
      });
    }
  }
}

void editorFpc(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  FpcUi& ui = fpcUi[t.key()];
  ui.pad = std::clamp(ui.pad, 0, kPadsAll - 1);
  const int pad = ui.pad;
  iskin::Skin k = iskin::begin(width, kFpcW, kFpcH, fpcPalette());
  iskin::background(k, 0, 0, kFpcW, kFpcH, k.c.bgTop, k.c.bgBottom, 4);
  ImDrawList* dl = k.dl();
  // Title + view (bank) switch.
  if (!pui::titleWordmarkIn(k.dl(), "gl.pads", k.at(6, 2), k.at(124, 34), k.px(28), 0, k.c.text))  // J-136: the wordmark
    iskin::text(k, 6, 2, 118, 32, "PADS", 28, rgb(0x5B6164), 0, false, true);
  // Pad strip.
  iskin::bevel(k, 4, 34, 782, 30, rgb(0x2C3338), k.c.panelLine, 3);
  if (iskin::button(k, "##pprev", 8, 41, 15, 17, "", false)) ui.pad = (pad + kPadsAll - 1) % kPadsAll;
  iskin::glyphArrow(dl, k.at(15.5f, 49.5f), k.px(5), rgb(0xF0F0F0), 0);
  {
    char cur[24];
    std::snprintf(cur, sizeof cur, "Pad %d/%d", pad % kPads + 1, kPads);
    iskin::text(k, 23, 41, 78, 17, cur, 10.5f, k.c.textDim, 1, false, true);
  }
  if (iskin::button(k, "##pnext", 101, 41, 15, 17, "", false)) ui.pad = (pad + 1) % kPadsAll;
  iskin::glyphArrow(dl, k.at(108.5f, 49.5f), k.px(5), rgb(0xF0F0F0), 1);
  iskin::text(k, 134, 41, 64, 17, pad < kPads ? "BANK A" : "BANK B", 9, k.c.label, 1, false, true);
  {
    std::string nm = padName(st, pad);
    iskin::text(k, 202, 43, 92, 15, nm.empty() ? i18n::tr("(empty)") : nm.c_str(), 10, rgb(0xBDBDBD), 0);
  }
  auto cap = [&](float x, float w, const char* s) { iskin::text(k, x, 26, w, 9, s, 7.5f, k.c.label, 1, false, true); };
  iskin::wheel(a, t, k, padP(pad, PadLevel), 301, 37, 24, k.c.fill);
  cap(295, 36, "VOL");
  iskin::wheel(a, t, k, padP(pad, PadPan), 335, 37, 24, k.c.fill);
  cap(329, 36, "PAN");
  iskin::wheel(a, t, k, padP(pad, PadTune), 375, 37, 24, k.c.fill);
  cap(369, 36, "TUNE");
  auto sw = [&](int param, float x, const char* s, ImU32 col) {
    iskin::switchBtn(a, t, k, param, x, 40, 19, 18, [&, col](ImDrawList* l, ImVec2 p0, ImVec2 p1, bool on) {
      l->AddRectFilled(p0, p1, on ? col : rgb(0x343D41), 2);
      l->AddRect(p0, p1, rgb(0x1A1F22), 2);
      ImFont* f = a.fontBold;
      float fs = std::max(7.0f, k.px(8));
      ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, s);
      l->AddText(f, fs, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), on ? rgb(0x1A1F22) : rgb(0x6D6D6E), s);
    });
  };
  sw(padE(pad, PadMute), 415, "M", rgb(0xE5484D));
  cap(409, 31, "MUTE");
  sw(padE(pad, PadSolo), 447, "S", rgb(0x7FF0AA));
  cap(441, 31, "SOLO");
  sw(padE(pad, PadVelScale), 479, "V", k.c.fill2);
  cap(470, 37, "VEL>VOL");
  sw(padE(pad, PadOneShot), 511, "1", k.c.fill2);
  cap(503, 35, "1-SHOT");
  // Note: click → menu (learn from the keyboard strip / base note mapping).
  {
    iskin::text(k, 556, 42, 36, 13, "NOTE", 7.5f, k.c.label, 2, false, true);  // J-128: on the switch captions line
    iskin::digi(a, t, k, padE(pad, PadNote), 594, 42, 46, 16, [&](double v) {
      int n = static_cast<int>(std::lround(v));
      return n < 0 ? ed::noteName(padNoteOf(st, pad)) + "*" : ed::noteName(n);
    }, 9);
  }
  iskin::text(k, 642, 26, 30, 9, "CUT", 7.5f, k.c.label, 1, false, true);
  iskin::digi(a, t, k, padE(pad, PadCutBy), 643, 42, 30, 16, {}, 9);
  iskin::text(k, 692, 26, 44, 9, "CUT BY", 7.5f, k.c.label, 1, false, true);
  iskin::digi(a, t, k, padP(pad, PadCut), 697, 42, 30, 16, {}, 9);
  iskin::text(k, 734, 26, 50, 9, "START", 7.5f, k.c.label, 1, false, true);
  iskin::digi(a, t, k, padE(pad, PadStart), 743, 42, 38, 16, [](double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", v * 100);
    return std::string(b);
  }, 9);
  // Global strip (top right): base note, velocity → level, volume.
  iskin::text(k, 150, 8, 60, 10, "VOLUME", 7.5f, k.c.label, 1, false, true);
  iskin::wheel(a, t, k, PadsVolume, 214, 4, 24, k.c.fill2);
  iskin::text(k, 244, 8, 60, 10, "VEL>LEVEL", 7.5f, k.c.label, 1, false, true);
  iskin::wheel(a, t, k, PadsVelocity, 306, 4, 24, k.c.fill2);
  iskin::text(k, 340, 8, 60, 10, "BASE NOTE", 7.5f, k.c.label, 1, false, true);
  iskin::digi(a, t, k, PadsBaseNote, 402, 7, 40, 17, [](double v) { return ed::noteName(static_cast<int>(std::lround(v))); }, 9);
  // Pads (4 × 4, pad 1 bottom-left) of the shown bank.
  const int bank = pad < kPads ? 0 : 1;
  iskin::bevel(k, 9, 74, 271, 260, rgb(0x232A2E), k.c.panelLine, 4);
  static const float colX[4] = {14, 81, 148, 215}, rowY[4] = {274, 209, 144, 79};
  for (int i = 0; i < kPads; ++i) {
    const int pk = bank * kPads + i;
    const float x = colX[i % 4], y = rowY[i / 4];
    std::string nm = padName(st, pk);
    const bool muted = ed::bval(st, padE(pk, PadMute));
    ImGui::PushID(pk);
    float vel = 0.8f;
    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "%s", nm.c_str());
    iskin::Skin kp = k;
    kp.c.panel = lerp(padColour(st, pk, rgb(0x5C6A70)), rgb(0x232A2E), nm.empty() ? 0.75f : muted ? 0.6f : 0.25f);
    const int note = padNoteOf(st, pk);
    // J-77: every hit of the pad's note (playback, MIDI, keys, clicks) flashes it, scaled by velocity.
    const float flash = t.isChannel() ? keylight::flash(a, t.owner, note) : 0.0f;
    int r = iskin::pad(kp, "##pad", x, y, 60, 56, lbl, pk == pad, flash, &vel);
    static int playing = -1;
    if (r == 1) {
      ui.pad = pk;
      if (t.isChannel()) a.engine.noteOn(t.owner, note, vel), playing = note;
    }
    if (r == 2 && playing >= 0 && t.isChannel()) a.engine.noteOff(t.owner, playing), playing = -1;
    char num[8];
    std::snprintf(num, sizeof num, "%d", pk + 1);
    iskin::text(k, x + 4, y + 3, 20, 10, num, 7.5f, theme::withAlpha(IM_COL32_WHITE, 0.45f), 0, true, false);
    iskin::text(k, x + 26, y + 3, 30, 10, ed::noteName(note).c_str(), 7.5f, theme::withAlpha(IM_COL32_WHITE, 0.45f), 2, true, false);
    const int nl = static_cast<int>(readLayers(st, pk).size());
    for (int l = 0; l < std::min(nl, 8); ++l) dl->AddCircleFilled(k.at(x + 54 - l * 4.5f, y + 50), k.px(1.4f), theme::withAlpha(IM_COL32_WHITE, 0.5f));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) {
          PluginState ns = st;
          std::vector<LayerInfo> ls = readLayers(st, pk);
          if (ls.empty()) ls.push_back(LayerInfo{});
          ls[0].path = path;
          writeLayers(ns, pk, ls);
          ns.data = padDataSet(ns.data, "N" + std::to_string(pk + 1), "");
          setPluginState(a, t, ns, "load pad sample");
          ui.pad = pk;
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered()) a.setHint("Pad " + std::to_string(pk + 1) + " (" + ed::noteName(note) + ")   click: play (top = loud) · drop a sample");
    ImGui::PopID();
  }
  // Bank selector.
  for (int b = 0; b < 2; ++b)
    if (iskin::button(k, b == 0 ? "##bankA" : "##bankB", 14 + b * 131.5f, 334, 129.5f, 19, b == 0 ? "BANK A" : "BANK B", bank == b, 9))
      ui.pad = b * kPads + pad % kPads;
  // Layers, sample, envelope.
  drawLayers(a, t, k, st, ui, pad);
  drawEnvelope(a, t, k, st, ui, pad);
  iskin::end(k, kFpcH);
}

}  // namespace

bool drawPadsEditorJ2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != "gl.pads") return false;
  editorFpc(a, t, width);
  return true;
}

}  // namespace gl
