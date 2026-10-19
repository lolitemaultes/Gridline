// Gridline — plugin picker (F8): a full-window grid of every instrument and
// effect (built-in and hosted), grouped by category with a category rail,
// search (same smart syntax as the browser), kind filter, favourites, and
// editor thumbnails (or in-house category glyphs). Click adds the plugin,
// drag drops it onto the channel rack, a mixer strip / slot or the playlist.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "BrowserUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::browserui {

using theme::pal;
namespace role = theme::role;

namespace {

constexpr float kPi = 3.14159265f;

bool pickerOpen = false;
bool pickerFocusSearch = false;
bool hiddenForDrag = false;  // the picker steps aside while one of its tiles is dragged
std::string dragName;

struct PickerState {
  char search[96] = "";
  int filter = 0;  // 0 all, 1 instruments, 2 effects, 3 note effects, 4 favourites
  bool thumbs = true;
  std::string jumpTo;  // category section to scroll to
};
PickerState& st() {
  static PickerState s = [] {
    PickerState p;
    // Dev (screenshots): glyphs instead of thumbnails, a search, a kind filter.
    if (std::getenv("GRIDLINE_PICKER_GLYPHS")) p.thumbs = false;
    if (const char* q = std::getenv("GRIDLINE_PICKER_SEARCH")) std::snprintf(p.search, sizeof p.search, "%s", q);
    if (const char* f = std::getenv("GRIDLINE_PICKER_FILTER")) p.filter = std::clamp(std::atoi(f), 0, 4);
    return p;
  }();
  return s;
}

std::string lowerStr(std::string s) { return browse::lower(std::move(s)); }

bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

enum class Glyph { Synth, Drums, Sampler, Controller, Midi, Delay, Dynamics, Filter, Eq, Distortion, Modulation, Spectral, Visual, Stereo, Pitch, Utility, Plug };

Glyph glyphOf(const std::string& category, int kind) {
  const std::string c = lowerStr(category);
  if (has(c, "drum") || has(c, "percuss")) return Glyph::Drums;
  if (has(c, "sampl") || has(c, "slic")) return Glyph::Sampler;
  if (has(c, "control")) return Glyph::Controller;
  if (has(c, "midi") || has(c, "note") || has(c, "arp") || has(c, "chord")) return Glyph::Midi;
  if (has(c, "synth") || has(c, "oscil")) return Glyph::Synth;
  if (has(c, "delay") || has(c, "reverb") || has(c, "space") || has(c, "echo")) return Glyph::Delay;
  if (has(c, "dynamic") || has(c, "compress") || has(c, "limit")) return Glyph::Dynamics;
  if (has(c, "eq")) return Glyph::Eq;
  if (has(c, "filter")) return Glyph::Filter;
  if (has(c, "distort") || has(c, "saturat") || has(c, "drive")) return Glyph::Distortion;
  if (has(c, "modul") || has(c, "chorus") || has(c, "flang") || has(c, "phas")) return Glyph::Modulation;
  if (has(c, "spectr") || has(c, "vocod")) return Glyph::Spectral;
  if (has(c, "visual") || has(c, "analy") || has(c, "meter")) return Glyph::Visual;
  if (has(c, "stereo") || has(c, "spatial") || has(c, "pan")) return Glyph::Stereo;
  if (has(c, "pitch") || has(c, "tun")) return Glyph::Pitch;
  if (has(c, "util") || has(c, "misc") || has(c, "gain") || has(c, "tool")) return Glyph::Utility;
  if (kind == static_cast<int>(PluginKind::Generator)) return Glyph::Synth;
  if (kind == static_cast<int>(PluginKind::NoteFx)) return Glyph::Midi;
  return Glyph::Plug;
}

}  // namespace

uint32_t categoryRole(const std::string& category, int kind) {
  switch (glyphOf(category, kind)) {
    case Glyph::Synth: return role::kDraw;
    case Glyph::Drums: return role::kDelete;
    case Glyph::Sampler: return role::kPaint;
    case Glyph::Controller: return role::kStamp;
    case Glyph::Midi: return role::kSnap;
    case Glyph::Delay: return role::kSlice;
    case Glyph::Dynamics: return role::kPlay;
    case Glyph::Filter: return role::kSelect;
    case Glyph::Eq: return role::kGraph;
    case Glyph::Distortion: return role::kSlip;
    case Glyph::Modulation: return role::kMute;
    case Glyph::Spectral: return role::kGhost;
    case Glyph::Visual: return role::kLoop;
    case Glyph::Stereo: return role::kZoom;
    case Glyph::Pitch: return role::kMenu;
    case Glyph::Utility: return role::kView;
    case Glyph::Plug: break;
  }
  return role::kView;
}

void drawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
  ImVec2 pts[10];
  for (int i = 0; i < 10; ++i) {
    const float ang = -kPi / 2 + static_cast<float>(i) * kPi / 5;
    const float rr = i % 2 ? r * 0.45f : r;
    pts[i] = ImVec2(c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr);
  }
  if (filled) {
    // Concave: fan of triangles from the centre.
    for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
  } else {
    dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
  }
}

void drawCategoryGlyph(ImDrawList* dl, const std::string& category, int kind, ImVec2 c, float size, ImU32 col) {
  const float h = size * 0.5f, th = std::max(1.2f, size / 17.0f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
  auto curve = [&](auto fn, float x0, float x1, int n = 40) {
    ImVec2 pts[64];
    n = std::min(n, 64);
    for (int i = 0; i < n; ++i) {
      float x = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(n - 1);
      pts[i] = P(x, fn(x));
    }
    dl->AddPolyline(pts, n, col, 0, th);
  };
  switch (glyphOf(category, kind)) {
    case Glyph::Synth:  // saw wave
      dl->AddLine(P(-0.8f, 0.4f), P(-0.25f, -0.45f), col, th);
      dl->AddLine(P(-0.25f, -0.45f), P(-0.25f, 0.4f), col, th);
      dl->AddLine(P(-0.25f, 0.4f), P(0.3f, -0.45f), col, th);
      dl->AddLine(P(0.3f, -0.45f), P(0.3f, 0.4f), col, th);
      dl->AddLine(P(0.3f, 0.4f), P(0.8f, -0.35f), col, th);
      break;
    case Glyph::Drums:  // pads
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
          ImVec2 a = P(-0.7f + x * 0.75f, -0.7f + y * 0.75f), b = P(-0.05f + x * 0.75f, -0.05f + y * 0.75f);
          if (x == y) dl->AddRectFilled(a, b, col, size * 0.04f);
          else dl->AddRect(a, b, col, size * 0.04f, 0, th);
        }
      break;
    case Glyph::Sampler:  // waveform
      for (int i = 0; i < 13; ++i) {
        float x = -0.75f + i * 0.125f;
        float a = (0.15f + 0.65f * std::fabs(std::sin(static_cast<float>(i) * 1.7f + 0.4f))) * (1.0f - std::fabs(x) * 0.6f);
        dl->AddLine(P(x, -a), P(x, a), col, th);
      }
      break;
    case Glyph::Controller:  // knob
      dl->PathArcTo(c, h * 0.62f, kPi * 0.75f, kPi * 2.25f, 32);
      dl->PathStroke(col, 0, th);
      dl->AddCircleFilled(c, h * 0.38f, col);
      dl->AddLine(c, P(0.3f, -0.42f), pal().surface900, th * 1.4f);
      break;
    case Glyph::Midi:  // notes
      dl->AddCircleFilled(P(-0.35f, 0.45f), h * 0.2f, col);
      dl->AddCircleFilled(P(0.4f, 0.3f), h * 0.2f, col);
      dl->AddLine(P(-0.17f, 0.45f), P(-0.17f, -0.55f), col, th);
      dl->AddLine(P(0.58f, 0.3f), P(0.58f, -0.7f), col, th);
      dl->AddLine(P(-0.17f, -0.55f), P(0.58f, -0.7f), col, th * 2.2f);
      break;
    case Glyph::Delay:  // decaying taps
      for (int i = 0; i < 5; ++i) {
        float x = -0.7f + i * 0.35f, a = 0.75f * std::pow(0.68f, static_cast<float>(i));
        dl->AddRectFilled(P(x - 0.09f, 0.6f - a * 1.6f), P(x + 0.09f, 0.6f), theme::withAlpha(col, 1.0f - 0.15f * static_cast<float>(i)));
      }
      break;
    case Glyph::Dynamics:  // transfer curve with knee
      dl->AddRect(P(-0.75f, -0.75f), P(0.75f, 0.75f), theme::withAlpha(col, 0.35f), 0, 0, th * 0.7f);
      dl->AddLine(P(-0.75f, 0.75f), P(0.05f, -0.05f), col, th);
      dl->AddLine(P(0.05f, -0.05f), P(0.75f, -0.3f), col, th);
      break;
    case Glyph::Filter:  // low-pass
      curve([](float x) { return x < 0.1f ? -0.3f : -0.3f + (x - 0.1f) * (x - 0.1f) * 2.4f; }, -0.8f, 0.75f);
      break;
    case Glyph::Eq:  // bell
      curve([](float x) { return 0.25f - 0.8f * std::exp(-x * x * 9.0f); }, -0.85f, 0.85f);
      dl->AddCircleFilled(P(0, -0.55f), th * 1.8f, col);
      break;
    case Glyph::Distortion:  // clipped sine
      curve([](float x) { return std::clamp(-0.9f * std::sin(x * kPi * 1.25f), -0.45f, 0.45f); }, -0.8f, 0.8f);
      break;
    case Glyph::Modulation:  // two detuned sines
      curve([](float x) { return 0.4f * std::sin(x * kPi * 1.5f); }, -0.8f, 0.8f);
      curve([](float x) { return 0.4f * std::sin(x * kPi * 1.5f + 1.2f); }, -0.8f, 0.8f);
      break;
    case Glyph::Spectral:
    case Glyph::Visual: {  // spectrum bars
      const float hs[7] = {0.35f, 0.7f, 1.0f, 0.8f, 0.55f, 0.65f, 0.3f};
      for (int i = 0; i < 7; ++i) {
        float x = -0.72f + i * 0.24f;
        dl->AddRectFilled(P(x - 0.07f, 0.65f - hs[i] * 1.2f), P(x + 0.07f, 0.65f), col, 1);
      }
      break;
    }
    case Glyph::Stereo:
      dl->AddCircle(P(-0.25f, 0), h * 0.5f, col, 28, th);
      dl->AddCircle(P(0.25f, 0), h * 0.5f, col, 28, th);
      break;
    case Glyph::Pitch:
      dl->AddTriangleFilled(P(-0.35f, -0.15f), P(0.0f, -0.7f), P(0.35f, -0.15f), col);
      dl->AddTriangleFilled(P(-0.35f, 0.15f), P(0.0f, 0.7f), P(0.35f, 0.15f), theme::withAlpha(col, 0.6f));
      break;
    case Glyph::Utility:  // fader
      dl->AddLine(P(-0.3f, -0.75f), P(-0.3f, 0.75f), theme::withAlpha(col, 0.5f), th);
      dl->AddLine(P(0.3f, -0.75f), P(0.3f, 0.75f), theme::withAlpha(col, 0.5f), th);
      dl->AddRectFilled(P(-0.5f, 0.05f), P(-0.1f, 0.3f), col, 1);
      dl->AddRectFilled(P(0.1f, -0.45f), P(0.5f, -0.2f), col, 1);
      break;
    case Glyph::Plug:
      ui::drawIcon(dl, ui::Icon::Plug, c, size * 0.7f, col);
      break;
  }
}

void togglePluginPicker() {
  pickerOpen = !pickerOpen;
  pickerFocusSearch = pickerOpen;
  hiddenForDrag = false;
}

bool pluginPickerOpen() { return pickerOpen; }

namespace {

// Adds the plugin the usual way (new channel / free slot on the selected
// insert / note effect on the selected channel).
void addPlugin(App& a, const PluginInfo& p) {
  if (p.kind == PluginKind::Generator) {
    a.settingsChannel = a.addInstrument(p.type);
    return;
  }
  if (p.kind == PluginKind::NoteFx) {
    ObjectId ch = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
    if (ch != kNoId && a.doc.addNoteFx(ch, p.type)) a.settingsChannel = ch;
    return;
  }
  const Song& s = a.doc.song();
  ObjectId mid = a.selMixer != kNoId ? a.selMixer : (s.mixer.size() > 1 ? s.mixer[1].id : kNoId);
  if (const MixerTrack* t = s.mixerTrack(mid)) {
    for (int k = 0; k < kEffectSlots; ++k)
      if (t->slots[static_cast<size_t>(k)].plugin.empty()) {
        a.doc.setSlotPlugin(t->id, k, p.type);
        a.fxEditor = {t->id, k};
        a.selMixer = t->id;
        return;
      }
    a.notify("No free effect slot on " + mixerTrackName(s, static_cast<size_t>(s.mixerIndex(t->id))), true);
  }
}

struct Group {
  std::string key;    // "<kind>:<category>"
  std::string title;  // category
  PluginKind kind;
  std::vector<const PluginInfo*> items;
};

constexpr float kTileW = 172, kTileH = 136, kThumbH = 100, kGap = 10;

void tile(App& a, const PluginInfo& p, bool fav, ImVec2 pos) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(pos);
  ImGui::PushID(p.type.c_str());
  ImGui::SetNextItemAllowOverlap();
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
  const bool hov = ImGui::IsItemHovered();
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("GL_PLUGIN", p.type.data(), p.type.size());
    ui::DragPreview(p.kind == PluginKind::Generator ? ui::Icon::Plug : ui::Icon::Mixer, p.name.c_str(),
                    theme::rgb(categoryRole(p.category, static_cast<int>(p.kind))));
    ImGui::EndDragDropSource();
    hiddenForDrag = true;
    dragName = p.name;
  }
  const ImVec2 b(pos.x + kTileW, pos.y + kTileH);
  const uint32_t rc = categoryRole(p.category, static_cast<int>(p.kind));
  dl->AddRectFilled(pos, b, hov ? P.surface700 : P.surface800, theme::kRadiusLg);
  // Image: editor thumbnail or the category glyph on a tinted panel.
  const ImVec2 i0(pos.x + 4, pos.y + 4), i1(b.x - 4, pos.y + kThumbH);
  ImVec2 tsz;
  ImTextureID tex = st().thumbs ? pluginThumb(a, p.type, &tsz) : ImTextureID_Invalid;
  if (tex != ImTextureID_Invalid && tsz.x > 0 && tsz.y > 0) {
    // Cover the box, top-aligned (editor heads are the recognisable part).
    const float bw = i1.x - i0.x, bh = i1.y - i0.y;
    float u1 = 1, v1 = 1;
    if (tsz.x / tsz.y > bw / bh) u1 = (bw / bh) / (tsz.x / tsz.y);
    else v1 = (tsz.x / tsz.y) / (bw / bh);
    dl->AddImageRounded(tex, i0, i1, ImVec2((1 - u1) * 0.5f, 0), ImVec2((1 + u1) * 0.5f, v1), IM_COL32_WHITE, theme::kRadius);
    if (!hov) dl->AddRectFilled(i0, i1, theme::withAlpha(P.surface950, 0.12f), theme::kRadius);
  } else {
    dl->AddRectFilledMultiColor(i0, i1, theme::mix(P.surface700, theme::rgb(rc), 0.10f), theme::mix(P.surface700, theme::rgb(rc), 0.10f),
                                P.surface900, P.surface900);
    // Faint grid (rack-panel feel).
    for (float x = i0.x + 12; x < i1.x; x += 12) dl->AddLine(ImVec2(x, i0.y), ImVec2(x, i1.y), theme::withAlpha(P.line, 0.35f));
    drawCategoryGlyph(dl, p.category, static_cast<int>(p.kind), ImVec2((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f), 62,
                      theme::litRole(rc, hov ? 1.0f : 0.85f));
  }
  dl->AddRect(i0, i1, theme::withAlpha(P.line, 0.8f), theme::kRadius);
  // Kind / format badge (top-left).
  {
    std::string badge;
    std::string fmtId, uid;
    if (host::splitType(p.type, fmtId, uid)) {
      const host::Format* f = host::findFormat(fmtId);
      badge = f ? f->label : fmtId;
    }
    if (!badge.empty()) {
      float bw = a.fontMono->CalcTextSizeA(10, 1e9f, 0, badge.c_str()).x + 8;
      dl->AddRectFilled(ImVec2(i0.x + 4, i0.y + 4), ImVec2(i0.x + 4 + bw, i0.y + 18), theme::withAlpha(P.surface950, 0.85f), 2);
      dl->AddText(a.fontMono, 10, ImVec2(i0.x + 8, i0.y + 6), P.textDim, badge.c_str());
    }
  }
  // Favourite star (top-right): its own button.
  {
    const ImVec2 sc(i1.x - 12, i0.y + 12);
    ImGui::SetCursorScreenPos(ImVec2(sc.x - 10, sc.y - 10));
    if (ImGui::InvisibleButton("##fav", ImVec2(20, 20))) {
      host::PluginPrefs prefs = host::pluginPrefs();
      if (fav) prefs.favourites.erase(p.type);
      else prefs.favourites.insert(p.type);
      host::setPluginPrefs(prefs);
    }
    const bool sh = ImGui::IsItemHovered();
    if (sh) ui::Hint(fav ? "Remove from favourites" : "Add to favourites");
    if (fav || hov) {
      dl->AddCircleFilled(sc, 9, theme::withAlpha(P.surface950, 0.75f));
      drawStar(dl, sc, 6, fav ? P.hot : sh ? P.text : P.textDim, fav);
    }
  }
  // Name + category.
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption + 1, ImVec2(pos.x + 8, pos.y + kThumbH + 6), kTileW - 16, P.text, p.name.c_str());
  dl->AddRectFilled(ImVec2(pos.x + 8, pos.y + kThumbH + 25), ImVec2(pos.x + 11, pos.y + kThumbH + 31), theme::rgb(rc));
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption - 1, ImVec2(pos.x + 15, pos.y + kThumbH + 21), kTileW - 24, P.textFaint,
                      p.category.c_str());
  if (hov) dl->AddRect(pos, b, P.hot, theme::kRadiusLg, 0, 1.5f);
  if (hov)
    ui::Hint(p.name + " \xC2\xB7 " + p.category + "   (click: " +
             (p.kind == PluginKind::Generator ? "new channel" : p.kind == PluginKind::Effect ? "insert on the selected mixer track" : "add to the selected channel") +
             " \xC2\xB7 drag onto the rack, a mixer strip / slot or the playlist \xC2\xB7 Shift+click keeps the picker open)");
  if (clicked) {
    addPlugin(a, p);
    if (!ImGui::GetIO().KeyShift) pickerOpen = false;
  }
  ImGui::PopID();
}

}  // namespace

void drawPluginPicker(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && !io.KeyAlt && !io.KeyCtrl && !io.KeySuper && !io.KeyShift) togglePluginPicker();
  if (!pickerOpen) return;
  // While a tile is dragged the picker steps aside so the targets show.
  if (hiddenForDrag) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("GL_PLUGIN")) {
      ImGui::SetTooltip("%s", dragName.c_str());
      return;
    }
    hiddenForDrag = false;
    pickerOpen = false;  // dropped (or cancelled): done
    return;
  }
  const auto& P = pal();
  PickerState& S = st();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::withAlpha(P.surface950, 0.98f)));
  ImGui::Begin("Plugin picker##overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) pickerOpen = false;

  // ---- header ----
  const float headH = 52;
  dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + headH), P.surface900);
  dl->AddLine(ImVec2(wp.x, wp.y + headH), ImVec2(wp.x + ws.x, wp.y + headH), P.line);
  dl->AddRectFilled(ImVec2(wp.x + 16, wp.y + 16), ImVec2(wp.x + 19, wp.y + 36), P.hot);
  dl->AddText(a.fontBold, theme::kFontTitle, ImVec2(wp.x + 28, wp.y + 11), P.text, "Plugin picker");
  // Collect.
  const host::PluginPrefs prefs = host::pluginPrefs();
  const browse::Query q = browse::parseQuery(S.search);
  std::vector<const PluginInfo*> all;
  for (const auto& p : pluginCatalog()) all.push_back(&p);
  for (const PluginInfo* p : dynamicPlugins()) {
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    all.push_back(p);
  }
  std::vector<Group> groups;
  int total = 0;
  for (const PluginInfo* p : all) {
    const bool fav = prefs.favourites.count(p->type) > 0;
    if (S.filter == 1 && p->kind != PluginKind::Generator) continue;
    if (S.filter == 2 && p->kind != PluginKind::Effect) continue;
    if (S.filter == 3 && p->kind != PluginKind::NoteFx) continue;
    if (S.filter == 4 && !fav) continue;
    if (!q.empty() && !browse::matchesPlugin(*p, q, fav)) continue;
    std::string cat = p->category.empty() ? "Other" : p->category;
    if (auto it = prefs.categories.find(p->type); it != prefs.categories.end()) cat = it->second;
    const std::string key = std::to_string(static_cast<int>(p->kind)) + ":" + cat;
    auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.key == key; });
    if (g == groups.end()) {
      groups.push_back(Group{key, cat, p->kind, {}});
      g = groups.end() - 1;
    }
    g->items.push_back(p);
    ++total;
  }
  std::sort(groups.begin(), groups.end(), [](const Group& x, const Group& y) {
    return x.kind != y.kind ? static_cast<int>(x.kind) < static_cast<int>(y.kind) : browse::lower(x.title) < browse::lower(y.title);
  });
  for (auto& g : groups)
    std::sort(g.items.begin(), g.items.end(), [&prefs](const PluginInfo* x, const PluginInfo* y) {
      const bool fx = prefs.favourites.count(x->type) > 0, fy = prefs.favourites.count(y->type) > 0;
      return fx != fy ? fx : browse::lower(x->name) < browse::lower(y->name);
    });
  {
    char b[48];
    std::snprintf(b, sizeof b, "%d plugins", total);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(wp.x + 150, wp.y + 16), P.textFaint, b);
  }
  // Search.
  ImGui::SetCursorScreenPos(ImVec2(wp.x + 236, wp.y + 14));
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(300);
    if (pickerFocusSearch) {
      ImGui::SetKeyboardFocusHere();
      pickerFocusSearch = false;
    }
    ImGui::InputTextWithHint("##psearch", "Search plugins  (type:effect  tag:delay  tag:fav)", S.search, sizeof S.search);
    ImGui::PopStyleVar();
    ui::FocusRing();
    ui::drawIcon(dl, ui::Icon::Search, ImVec2(p.x + 14, p.y + ImGui::GetFrameHeight() * 0.5f), 12, P.textFaint);
  }
  ImGui::SameLine(0, 12);
  {
    static const char* labels[] = {"All", "Instruments", "Effects", "Note effects", "Favourites"};
    ui::Segmented("##pfilter", labels, 5, &S.filter, 0, 0);
  }
  ImGui::SameLine(0, 12);
  if (ui::TextButton("Thumbnails", S.thumbs, 0, "Show editor thumbnails (off: category glyphs)")) S.thumbs = !S.thumbs;
  ImGui::SameLine(0, 6);
  if (thumbnailRunActive()) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textFaint), "Rendering thumbnails\xE2\x80\xA6");
  } else if (ui::TextButton("Render thumbnails", false, 0, "Render every built-in editor once (in the background) and cache the images")) {
    startThumbnailRun(a);
  }
  ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - 40, wp.y + 14));
  if (ui::IconButton("closepicker", ui::Icon::Close, false, 24, "Close (Esc / F8)")) pickerOpen = false;

  // ---- category rail ----
  const float railW = 196;
  const ImVec2 r0(wp.x, wp.y + headH + 1), r1(wp.x + railW, wp.y + ws.y);
  dl->AddRectFilled(r0, r1, P.surface900);
  dl->AddLine(ImVec2(r1.x, r0.y), ImVec2(r1.x, r1.y), P.line);
  ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + 8));
  ImGui::BeginChild("##rail", ImVec2(railW - 1, r1.y - r0.y - 8), 0);
  {
    int lastKind = -1;
    for (const auto& g : groups) {
      if (static_cast<int>(g.kind) != lastKind) {
        lastKind = static_cast<int>(g.kind);
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::SetCursorPosX(16);
        ui::Label(g.kind == PluginKind::Generator ? "INSTRUMENTS" : g.kind == PluginKind::Effect ? "EFFECTS" : "NOTE EFFECTS");
        ImGui::Dummy(ImVec2(0, 2));
      }
      ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::PushID(g.key.c_str());
      if (ImGui::InvisibleButton("##cat", ImVec2(railW - 9, 24))) S.jumpTo = g.key;
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      ImDrawList* rl = ImGui::GetWindowDrawList();
      if (hov) rl->AddRectFilled(q, ImVec2(q.x + railW - 9, q.y + 24), P.surface800, theme::kRadiusSm);
      const ImU32 rc = theme::litRole(categoryRole(g.title, static_cast<int>(g.kind)));
      drawCategoryGlyph(rl, g.title, static_cast<int>(g.kind), ImVec2(q.x + 24, q.y + 12), 14, rc);
      theme::textEllipsis(rl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 40, q.y + 5), railW - 80, hov ? P.text : P.textDim, g.title.c_str());
      std::string n = std::to_string(g.items.size());
      float nw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, n.c_str()).x;
      rl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q.x + railW - 18 - nw, q.y + 6), P.textFaint, n.c_str());
    }
  }
  ImGui::EndChild();

  // ---- grid ----
  ImGui::SetCursorScreenPos(ImVec2(r1.x + 1, r0.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 14));
  ImGui::BeginChild("##grid", ImVec2(ws.x - railW - 1, r1.y - r0.y), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImDrawList* gl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + kGap) / (kTileW + kGap)));
    for (const auto& g : groups) {
      ImVec2 h = ImGui::GetCursorScreenPos();
      if (S.jumpTo == g.key) {
        ImGui::SetScrollHereY(0.0f);
        S.jumpTo.clear();
      }
      const ImU32 rc = theme::litRole(categoryRole(g.title, static_cast<int>(g.kind)));
      gl->AddRectFilled(ImVec2(h.x, h.y + 4), ImVec2(h.x + 3, h.y + 18), rc);
      std::string title = g.title;
      for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      const char* kindTxt = g.kind == PluginKind::Generator ? "  \xC2\xB7  INSTRUMENTS" : g.kind == PluginKind::Effect ? "  \xC2\xB7  EFFECTS" : "  \xC2\xB7  NOTE EFFECTS";
      gl->AddText(a.fontMonoBold ? a.fontMonoBold : a.fontMono, theme::kFontLabel + 1, ImVec2(h.x + 10, h.y + 4), P.text, title.c_str());
      float tw = (a.fontMonoBold ? a.fontMonoBold : a.fontMono)->CalcTextSizeA(theme::kFontLabel + 1, 1e9f, 0, title.c_str()).x;
      gl->AddText(a.fontMono, theme::kFontLabel, ImVec2(h.x + 10 + tw, h.y + 5), P.textFaint, kindTxt);
      gl->AddLine(ImVec2(h.x, h.y + 24), ImVec2(h.x + avail, h.y + 24), P.line);
      ImGui::Dummy(ImVec2(avail, 32));
      ImVec2 start = ImGui::GetCursorScreenPos();
      const int n = static_cast<int>(g.items.size());
      const int rows = (n + cols - 1) / cols;
      for (int i = 0; i < n; ++i) {
        const ImVec2 pos(start.x + static_cast<float>(i % cols) * (kTileW + kGap), start.y + static_cast<float>(i / cols) * (kTileH + kGap));
        // Only tiles in view are drawn (hundreds of hosted plugins stay cheap).
        if (ImGui::IsRectVisible(pos, ImVec2(pos.x + kTileW, pos.y + kTileH)))
          tile(a, *g.items[static_cast<size_t>(i)], prefs.favourites.count(g.items[static_cast<size_t>(i)]->type) > 0, pos);
      }
      ImGui::SetCursorScreenPos(start);
      ImGui::Dummy(ImVec2(avail, static_cast<float>(rows) * (kTileH + kGap) + 10));
    }
    if (groups.empty()) {
      ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
      ui::EmptyState(gl, mn, ImVec2(mn.x + sz.x, mn.y + sz.y * 0.8f), "No plugins match", "Try another word or filter");
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::End();
}

}  // namespace gl::browserui
