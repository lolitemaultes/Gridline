// Gridline: ui::WarpWave, warp markers on any waveform. See WarpWave.h.
#include "WarpWave.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../core/I18n.h"
#include "../engine/Sample.h"
#include "../engine/SampleWarpRender.h"
#include "../packs/edison/edison.h"
#include "App.h"
#include "LayoutPresets.h"
#include "../core/LayoutPreset.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::warpwave {

namespace {
// The lanes edited lately (dev checks).
struct DevLane {
  std::string id;
  ImVec2 a, b;
  bool active = false;
  std::vector<WarpMarker> m;
  std::vector<float> xs;  // where each marker is drawn
  int frame = 0;
};
std::vector<DevLane> gDevLanes;
struct DevButton {
  std::string id;
  ImVec2 a, b;
  bool on = false;
  int frame = 0;
};
std::vector<DevButton> gDevButtons;
void noteLane(const char* id, ImVec2 a, ImVec2 b, bool active, const std::vector<WarpMarker>& m, const std::function<float(double)>& xOf) {
  const int f = ImGui::GetFrameCount();
  gDevLanes.erase(std::remove_if(gDevLanes.begin(), gDevLanes.end(), [&](const DevLane& d) { return d.frame < f - 2 || d.id == id; }), gDevLanes.end());
  std::vector<float> xs;
  for (const WarpMarkerAt& k : sampleWarpViews(m)) xs.push_back(xOf(k.w));
  gDevLanes.push_back({id, a, b, active, m, xs, f});
}
}  // namespace

uint32_t draw(ImDrawList* dl, const std::vector<Mark>& v, ImVec2 b0, ImVec2 b1, float visX0, float visX1, bool active, ImU32 ink, uint32_t held) {
  if (v.empty() || b1.y - b0.y < 4) return 0;
  const theme::Palette& P = theme::pal();
  const ImU32 warpCol = theme::rgb(theme::role::kWarp);
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool inBody = mouse.y >= b0.y && mouse.y <= b1.y;
  if (active) {
    // The sections that play faster or slower: a faint tint and their speed.
    for (size_t i = 0; i + 1 < v.size(); ++i) {
      if (!sectionChanged(v[i].speed)) continue;
      const float xa = std::max(v[i].x, visX0), xb = std::min(v[i + 1].x, visX1);
      if (xb <= xa) continue;
      dl->AddRectFilled(ImVec2(xa, b0.y), ImVec2(xb, b1.y), theme::withAlpha(warpCol, P.dark ? 0.07f : 0.10f));
      char lab[24];
      std::snprintf(lab, sizeof lab, "%.0f%%", sectionPercent(v[i].speed));  // (100%: its own length)
      const ImVec2 ts = ImGui::CalcTextSize(lab);
      if (labelFits(xb - xa, ts.x, b1.y - b0.y, ts.y)) dl->AddText(ImVec2(xa + 4, b1.y - ts.y - 2), theme::withAlpha(P.dark ? P.paper : P.ink, 0.6f), lab);
    }
  }
  const uint32_t hot = active && inBody ? hitTest(v, mouse.x, visX0, visX1) : 0;
  for (const Mark& k : v) {
    const float x = std::floor(k.x) + 0.5f;
    if (x < visX0 - kGrab || x > visX1 + kGrab) continue;
    if (!active) {  // warp mode off: a faint line, so warped audio is recognisable
      dl->AddLine(ImVec2(x, b0.y + 1), ImVec2(x, b1.y - 1), theme::withAlpha(ink, 0.30f));
      dl->AddTriangleFilled(ImVec2(x - 2.5f, b0.y), ImVec2(x + 2.5f, b0.y), ImVec2(x, b0.y + 3.0f), theme::withAlpha(ink, 0.55f));
      continue;
    }
    const bool on = k.id == hot || (held && held == k.id);
    const ImU32 col = on ? warpCol : theme::withAlpha(warpCol, 0.8f);
    dl->AddLine(ImVec2(x, b0.y), ImVec2(x, b1.y), theme::withAlpha(P.dark ? P.surface950 : P.paper, 0.5f), on ? 3.5f : 2.5f);
    dl->AddLine(ImVec2(x, b0.y), ImVec2(x, b1.y), col, on ? 2.0f : 1.0f);
    const float w = on ? 4.5f : 3.5f;  // a tab at the top of the body
    dl->AddTriangleFilled(ImVec2(x - w, b0.y), ImVec2(x + w, b0.y), ImVec2(x, b0.y + w * 1.3f), col);
  }
  return hot;
}

void glyph(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
  const float h = std::max(6.0f, size), w = h * 0.36f;
  const float top = std::round(c.y - h * 0.5f) + 0.5f, x = std::round(c.x) + 0.5f;
  dl->AddTriangleFilled(ImVec2(x - w, top), ImVec2(x + w, top), ImVec2(x, top + w * 1.5f), col);
  dl->AddLine(ImVec2(x, top + w), ImVec2(x, top + h), col, 1.5f);
}

const char* modeWord() { return layoutui::active() == layout::Preset::Logic ? i18n::N_("Flex") : i18n::N_("Warp"); }

bool modeButton(const char* id, bool on, const char* label, float height, const char* hint) {
  const theme::Palette& P = theme::pal();
  const char* text = label ? i18n::tr(label) : nullptr;
  const float h = height > 0 ? height : ImGui::GetFrameHeight();
  const float tw = text ? ImGui::CalcTextSize(text).x : 0.0f;
  const float w = std::round(h * 0.9f + (text ? tw + 10.0f : 0.0f));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  {  // (dev checks: where it is)
    const int f = ImGui::GetFrameCount();
    gDevButtons.erase(std::remove_if(gDevButtons.begin(), gDevButtons.end(), [&](const DevButton& d) { return d.frame < f - 2 || d.id == id; }),
                      gDevButtons.end());
    gDevButtons.push_back({id, p0, ImVec2(p0.x + w, p0.y + h), on, f});
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 teal = theme::rgb(theme::role::kWarp);
  const ImVec2 p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, on ? theme::withAlpha(teal, P.dark ? 0.16f : 0.22f) : hov ? theme::withAlpha(P.text, 0.08f) : P.field, 3.0f);
  dl->AddRect(p0, p1, on ? teal : theme::withAlpha(P.text, hov ? 0.30f : 0.14f), 3.0f);
  const ImU32 ink = on ? teal : hov ? P.text : P.textDim;
  glyph(dl, ImVec2(p0.x + h * 0.45f + (text ? 2.0f : 0.0f), (p0.y + p1.y) * 0.5f), h * 0.55f, ink);
  if (text) dl->AddText(ImVec2(p0.x + h * 0.9f, std::round(p0.y + (h - ImGui::GetFontSize()) * 0.5f)), ink, text);
  if (hint) ui::Hint(hint);
  return clicked;
}

float badgeWidth(int count) {
  char b[24];
  std::snprintf(b, sizeof b, "W %d", count);
  return std::round(ImGui::CalcTextSize(b).x * 0.85f + 12.0f);
}

float badge(ImDrawList* dl, ImVec2 pos, int count, float height) {
  if (count <= 0) return 0.0f;
  const theme::Palette& P = theme::pal();
  const ImU32 teal = theme::rgb(theme::role::kWarp);
  char b[24];
  std::snprintf(b, sizeof b, "W %d", count);
  const float fs = ImGui::GetFontSize() * 0.85f, w = badgeWidth(count);
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), theme::withAlpha(teal, P.dark ? 0.14f : 0.2f), height * 0.5f);
  dl->AddRect(pos, ImVec2(pos.x + w, pos.y + height), theme::withAlpha(teal, 0.8f), height * 0.5f);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(std::round(pos.x + 6), std::round(pos.y + (height - fs) * 0.5f)), teal, b);
  return w;
}

std::vector<Mark> marks(const std::vector<WarpMarker>& m, const std::function<float(double)>& xOf) {
  std::vector<Mark> v;
  for (const WarpMarkerAt& k : sampleWarpViews(m)) v.push_back({k.id, xOf(k.w), k.speed});
  return v;
}

namespace {
struct Drag {
  ImGuiID owner = 0;
  uint32_t id = 0;
  bool slide = false;
  double markerW = 0, grabW = 0;
  bool moved = false;
};
Drag gDrag;
ImGuiID gRightRemoved = 0;  // the lane whose marker the right button's press removed (no menu on its release)
}  // namespace


void devCommand(App& a, const std::vector<std::string>& args) {
  (void)a;
  const std::string tag = args.empty() ? std::string("-") : args[0];  // (the script's own tag for the lines)
  for (const DevButton& d : gDevButtons)
    std::printf("warpwave button %s tag %s rect %.0f %.0f %.0f %.0f on %d\n", d.id.c_str(), tag.c_str(), static_cast<double>(d.a.x),
                static_cast<double>(d.a.y), static_cast<double>(d.b.x), static_cast<double>(d.b.y), d.on ? 1 : 0);
  for (const DevLane& d : gDevLanes) {
    std::string at;
    char b[48];
    for (const WarpMarkerAt& k : sampleWarpViews(d.m)) {
      std::snprintf(b, sizeof b, "%s%.4f", at.empty() ? "" : ",", k.w);
      at += b;
    }
    std::string xs;
    for (float x : d.xs) {
      std::snprintf(b, sizeof b, "%s%.0f", xs.empty() ? "" : ",", static_cast<double>(x));
      xs += b;
    }
    std::printf("warpwave lane %s tag %s rect %.0f %.0f %.0f %.0f active %d markers %zu at %s x %s\n", d.id.c_str(), tag.c_str(),
                static_cast<double>(d.a.x), static_cast<double>(d.a.y), static_cast<double>(d.b.x), static_cast<double>(d.b.y), d.active ? 1 : 0, d.m.size(),
                at.empty() ? "-" : at.c_str(), xs.empty() ? "-" : xs.c_str());
  }
  std::fflush(stdout);
}

EditResult edit(const char* id, std::vector<WarpMarker>& m, const Map& map, ImVec2 b0, ImVec2 b1, bool active, ImU32 ink) {
  struct Note {  // (the lane and its markers as this frame leaves them)
    const char* id;
    ImVec2 a, b;
    bool on;
    const std::vector<WarpMarker>& m;
    const std::function<float(double)>& xOf;
    ~Note() { noteLane(id, a, b, on, m, xOf); }
  } note{id, b0, b1, active, m, map.xOf};
  EditResult r;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiID me = ImGui::GetID(id);
  const std::vector<Mark> v = marks(m, map.xOf);
  r.hot = draw(dl, v, b0, b1, b0.x, b1.x, active, ink, gDrag.owner == me ? gDrag.id : 0);
  if (!active) {
    if (gDrag.owner == me) gDrag = Drag{};
    return r;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse = io.MousePos;
  const bool inside = mouse.x >= b0.x && mouse.x < b1.x && mouse.y >= b0.y && mouse.y < b1.y &&
                      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    r.menuClick = inside && gRightRemoved != me && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16;
    if (gRightRemoved == me) gRightRemoved = 0;
  }
  if (gDrag.owner == me) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const double pw = map.wOf(mouse.x);
      if (!gDrag.moved && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) gDrag.moved = true;
      if (gDrag.moved) {
        const double target = sampleWarpDragTarget(gDrag.markerW, gDrag.grabW, pw, io.KeyAlt ? 0.0 : map.step);
        std::vector<WarpMarker> next = m;
        const bool ok = gDrag.slide ? sampleWarpSlide(next, gDrag.id, target, map.fileSec) : sampleWarpMove(next, gDrag.id, target, map.fileSec);
        if (ok && next != m) {
          m = std::move(next);
          r.changed = true;
          r.label = gDrag.slide ? "slide warp marker" : "move warp marker";
        }
        for (const WarpMarkerAt& k : sampleWarpViews(m))
          if (k.id == gDrag.id) {
            char b[96];
            std::snprintf(b, sizeof b, "%s %.0f%%", i18n::tr("Section after the marker:"), sectionPercent(k.speed));
            ImGui::SetTooltip("%s", b);
          }
      }
    } else {
      gDrag = Drag{};
      r.released = true;
    }
    return r;
  }
  if (!inside) return r;
  if (r.hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  if ((dbl || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) && r.hot) {
    if (sampleWarpRemove(m, r.hot)) {
      r.changed = r.released = true, r.label = "remove warp marker";
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) gRightRemoved = me;  // (its release opens no menu)
    }
    return r;
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !dbl) {
    uint32_t idHit = r.hot;
    if (!idHit) {
      idHit = sampleWarpAdd(m, map.wOf(mouse.x), map.fileSec, map.onsets, 0.04);
      if (idHit) r.changed = true, r.label = "add warp marker";
    }
    if (idHit) {
      gDrag = Drag{me, idHit, io.KeyShift, 0, map.wOf(mouse.x), false};
      for (const WarpMarkerAt& k : sampleWarpViews(m))
        if (k.id == idHit) gDrag.markerW = k.w;
    }
  }
  return r;
}

bool sampleMenu(App& a, SampleWarp& w, const Sample* smp, const MenuOpts& o) {
  bool changed = false;
  const double fileSec = smp ? smp->seconds() : 0.0;
  if (ImGui::MenuItem(i18n::tr("Warp mode"), keys::label("wave.warpMode"), w.on)) w.on = !w.on, changed = true;
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Add at transients"), nullptr, false, smp != nullptr)) {
    const int n = sampleWarpAtOnsets(w.markers, sampleOnsetsSec(*smp), fileSec);
    if (n > 0) changed = true;
    a.setHint(n > 0 ? std::to_string(n) + " " + i18n::tr("warp markers added") : std::string(i18n::tr("No transients found")));
  }
  const double strength = std::clamp(a.settings.getNumber("playlist.quantizeStrength"), 0.0, 1.0);
  const double beat = o.gridStep > 0 ? o.gridStep : w.bpm > 0 ? 60.0 / w.bpm : 0.0;
  if (ImGui::MenuItem(i18n::tr("Quantize to the grid"), nullptr, false, beat > 0 && !w.markers.empty())) {
    const int n = sampleWarpQuantize(w.markers, beat / 4.0, strength, fileSec);
    if (n > 0) changed = true;
    a.setHint(std::to_string(n) + " " + i18n::tr("warp markers quantized"));
  }
  if (ImGui::BeginMenu(i18n::tr("Strength"))) {
    for (int pct : {100, 75, 50, 25})
      if (ImGui::MenuItem((std::to_string(pct) + "%").c_str(), nullptr, std::fabs(strength * 100.0 - pct) < 0.5))
        a.settings.set("playlist.quantizeStrength", pct / 100.0);
    ImGui::EndMenu();
  }
  bool snap = a.settings.getBool("playlist.warpSnapTransients");
  if (ImGui::MenuItem(i18n::tr("New markers snap to transients"), nullptr, &snap)) a.settings.set("playlist.warpSnapTransients", snap);
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Set the loop's BPM"))) {
    float bpm = static_cast<float>(w.bpm > 0 ? w.bpm : 120.0);
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("##loopbpm", &bpm, 0.1f, 20.0f, 999.0f, "%.1f BPM")) w.bpm = bpm, changed = true;
    if (smp && ImGui::MenuItem(i18n::tr("Detect"))) {
      const std::vector<double> on = sampleOnsetsSec(*smp);
      std::vector<int64_t> f;
      for (double s : on) f.push_back(static_cast<int64_t>(std::llround(s * smp->sampleRate)));
      const double t = packs::edison::estimateTempo(f, smp->sampleRate);
      if (t > 0) w.bpm = t, changed = true;
    }
    ImGui::EndMenu();
  }
  if (o.slicer && o.sliceAtMarkers && ImGui::MenuItem(i18n::tr("Slice at warp markers"), nullptr, false, !w.markers.empty())) *o.sliceAtMarkers = true;
  if (o.applyRow && o.apply && ImGui::MenuItem(i18n::tr("Apply warp to a new file"), nullptr, false, w.active() || (!w.markers.empty() && !warpIsIdentity(w.markers))))
    *o.apply = true;
  if (ImGui::MenuItem(i18n::tr("Clear markers"), nullptr, false, !w.markers.empty())) w.markers.clear(), changed = true;
  return changed;
}

namespace {
// Onsets per sample (file seconds), for new markers to snap to.
const std::vector<double>* onsetsOf(const std::shared_ptr<const Sample>& smp) {
  static std::weak_ptr<const Sample> key;
  static std::vector<double> on;
  if (!smp) return nullptr;
  if (key.lock() != smp) {
    key = smp;
    on = sampleOnsetsSec(*smp);
  }
  return &on;
}
}  // namespace

std::shared_ptr<const Sample> shownSample(const SampleWarp& w, const std::shared_ptr<const Sample>& smp) {
  return w.active() && smp ? warpedSample(smp, w) : smp;
}

bool sampleWell(App& a, ObjectId channel, const char* id, ImVec2 p0, ImVec2 p1, const std::shared_ptr<const Sample>& smp, ImU32 ink, const WellOpts& o) {
  const Channel* ch = a.doc.song().channel(channel);
  if (!ch || !sampleWarpSupported(ch->plugin.type)) return false;
  SampleWarp w = sampleWarpOf(ch->plugin);
  const SampleWarp before = w;
  ImGui::PushID(id);
  const uint64_t key = (0x5357415250ull << 24) ^ static_cast<uint64_t>(channel);
  bool changed = false;
  std::string label = "warp mode";
  // The mode button, at the well's top right.
  const float bh = std::max(16.0f, std::min(20.0f, (p1.y - p0.y) * 0.18f));
  const float bw = std::round(bh * 0.9f + ImGui::CalcTextSize(i18n::tr(modeWord())).x + 10.0f);
  const ImVec2 cur = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(p1.x - bw - o.buttonRight, p0.y + 4));
  if (modeButton("##warpbtn", w.on, modeWord(), bh, "Warp mode: markers on the sample stretch it, every note plays the warped sample")) {
    w.on = !w.on;
    changed = true;
  }
  const bool overButton = ImGui::IsItemHovered();
  const bool hovered = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  if (hovered && keys::pressed("wave.warpMode")) w.on = !w.on, changed = true;
  const bool overMenuArea = hovered && (o.laneH <= 0 || ImGui::GetIO().MousePos.y >= p1.y - o.laneH);  // Kerf: its own menus above the lane
  // The markers: on the warped time line of the sample.
  const double fileSec = smp ? smp->seconds() : 0.0;
  const double len = std::max(1e-6, w.markers.size() >= 2 ? sampleWarpLength(w.markers, fileSec) : fileSec);
  const float sw = std::max(1.0f, p1.x - p0.x);
  uint32_t hot = 0;
  bool edited = false, menuClick = false;
  if (smp && fileSec > 0 && (w.on || (!w.markers.empty() && o.laneH <= 0))) {
    Map m;
    m.xOf = o.xOf ? o.xOf : std::function<float(double)>([&](double t) { return p0.x + static_cast<float>(t / len) * sw; });
    m.wOf = o.wOf ? o.wOf : std::function<double(float)>([&](float x) { return static_cast<double>((x - p0.x) / sw) * len; });
    m.step = w.bpm > 0 ? 60.0 / w.bpm / 4.0 : 0.0;
    m.fileSec = fileSec;
    m.onsets = a.settings.getBool("playlist.warpSnapTransients") ? onsetsOf(smp) : nullptr;
    ImVec2 e0 = p0;
    if (o.laneH > 0) {  // Kerf: a lane of flags and section speeds under the wave (its slice flags stay above)
      e0.y = p1.y - o.laneH;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 teal = theme::rgb(theme::role::kWarp);
      dl->AddRectFilled(e0, p1, theme::withAlpha(teal, 0.10f));
      dl->AddLine(e0, ImVec2(p1.x, e0.y), theme::withAlpha(teal, 0.55f));
    }
    const EditResult r = edit("##warpedit", w.markers, m, e0, p1, w.on && !overButton, ink);
    hot = r.hot;
    edited = w.on && !overButton;
    menuClick = r.menuClick;
    if (r.changed) changed = true, label = r.label;
    if (r.released) a.doc.endGesture();
  }
  // The right-click menu (a right-click on a marker removes it instead).
  bool slice = false;
  // (in warp mode the markers say: a right-click that removed one opens no menu on its release)
  const bool rightUp = edited ? menuClick : ImGui::IsMouseReleased(ImGuiMouseButton_Right);
  if (o.menu && overMenuArea && !overButton && !hot && rightUp && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16)
    ImGui::OpenPopup("##warpwell");
  if (ImGui::BeginPopup("##warpwell")) {
    if (ImGui::BeginMenu(i18n::tr("Warp"))) {
      MenuOpts mo;
      mo.slicer = o.slicer;
      mo.sliceAtMarkers = &slice;
      if (sampleMenu(a, w, smp.get(), mo)) changed = true, label = "sample warp";
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  if (slice && o.sliceAtMarkers) *o.sliceAtMarkers = true;
  if (changed && w != before) {
    const bool drag = label == "add warp marker" || label == "move warp marker" || label == "slide warp marker";  // (a click that adds and drags: one step)
    a.doc.editChannel(channel, label, [&w](Channel& c) { setSampleWarp(c.plugin, w); }, drag ? key : 0);
  }
  ImGui::SetCursorScreenPos(cur);
  ImGui::PopID();
  return w.on;
}

}  // namespace gl::warpwave
