// Gridline — K-85: warp markers in the playlist. See PlaylistWarp.h.
#include "PlaylistWarp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../core/I18n.h"
#include "../engine/WarpOps.h"
#include "App.h"
#include "GrooveUi.h"
#include "Widgets.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "WarpWave.h"

namespace gl::plwarp {

namespace {

struct Hover {
  ObjectId clip = kNoId;
  uint32_t id = 0;
  int frame = -1;
};
Hover hover;

struct Drag {
  ObjectId clip = kNoId;
  uint32_t id = 0;
  bool slide = false;
  uint64_t key = 0;
  double tick0 = 0;    // the marker at the grab
  Tick grab = 0;       // the pointer at the grab
  bool moved = false;  // the pointer left the grab tick (until then nothing moves)
  double target = 0;   // where the document has the marker now
};
Drag dragging;

}  // namespace

void drawMarkers(ImDrawList* dl, const Song& s, const Clip& c, const TempoMap& tm, ImVec2 b0, ImVec2 b1, float clipX0, float pxPerTick,
                 float visX0, float visX1, bool tool, ImU32 ink) {
  if (c.kind != ClipKind::Audio || c.warp.empty() || b1.y - b0.y < 4) return;
  const std::vector<WarpMarkerView> v = warpMarkerViews(s, c, tm);
  if (v.empty()) return;
  // K-248: drawn by the shared ui::WarpWave (the clip editor, the sampler, Kerf and Loupe draw theirs the same way).
  std::vector<warpwave::Mark> marks;
  marks.reserve(v.size());
  for (const WarpMarkerView& k : v)
    marks.push_back({k.id, clipX0 + static_cast<float>((k.tick - static_cast<double>(c.start)) * pxPerTick), k.speed});
  const uint32_t held = dragging.clip == c.id ? dragging.id : 0;
  const uint32_t hot = warpwave::draw(dl, marks, b0, b1, visX0, visX1, tool, ink, held);
  if (tool && hot) hover = {c.id, hot, ImGui::GetFrameCount()};
}

uint32_t hovered(ObjectId clip) {
  return hover.clip == clip && hover.frame >= ImGui::GetFrameCount() - 1 ? hover.id : 0;
}

bool begin(App& a, const Clip& c, const TempoMap& tm, double tick, bool slide, uint64_t key, double snapTicks) {
  dragging = Drag{};
  if (c.kind != ClipKind::Audio) return false;
  uint32_t id = hovered(c.id);
  if (!id) {
    Clip after = c;
    id = addWarpMarker(a.doc.song(), after, tm, tick, a.settings.getBool("playlist.warpSnapTransients"), snapTicks);
    if (!id) return false;
    a.doc.editClip(c.id, "add warp marker", [&after](Clip& k) { k.warp = after.warp; }, key);
  }
  dragging = Drag{c.id, id, slide, key};
  dragging.grab = static_cast<Tick>(std::llround(tick));
  dragging.tick0 = tick;
  if (const Clip* live = a.doc.song().clip(c.id))
    for (const WarpMarkerView& v : warpMarkerViews(a.doc.song(), *live, tm))
      if (v.id == id) dragging.tick0 = v.tick;
  dragging.target = dragging.tick0;
  return true;
}

void drag(App& a, const TempoMap& tm, Tick pointer, Tick grid) {
  if (dragging.clip == kNoId) return;
  const Clip* live = a.doc.song().clip(dragging.clip);
  if (!live) {
    end();
    return;
  }
  // A grab never jumps: the marker stays where it was clicked until the pointer really drags (past
  // the usual drag threshold), so a pixel of hand movement while clicking can't snap it to a grid line.
  if (!dragging.moved && (pointer == dragging.grab || !ImGui::IsMouseDragging(ImGuiMouseButton_Left))) return;
  dragging.moved = true;
  const double tick = warpDragTick(dragging.tick0, dragging.grab, pointer, grid);
  Clip after = *live;
  if (tick != dragging.target) {  // (the same place: no edit, no rebuild)
    dragging.target = tick;
    const bool changed = dragging.slide ? slideWarpMarker(a.doc.song(), after, tm, dragging.id, tick)
                                        : moveWarpMarker(a.doc.song(), after, tm, dragging.id, tick);
    if (changed)
      a.doc.editClip(dragging.clip, dragging.slide ? "slide warp marker" : "move warp marker", [&after](Clip& k) { k.warp = after.warp; }, dragging.key);
  }
  for (const WarpMarkerView& v : warpMarkerViews(a.doc.song(), after, tm))
    if (v.id == dragging.id) {
      if (!dragging.slide) dragging.target = v.tick;  // (held at a speed limit: the drawing glides to where it stopped)
      char b[96];
      std::snprintf(b, sizeof b, "%s %.0f%%", i18n::tr("Section after the marker:"), 100.0 / std::max(1e-6, v.speed));
      a.setHint(b);
    }
}

void end() { dragging = Drag{}; }

const Clip& shown(const Song& s, const Clip& c, const TempoMap& tm, bool glide, double eps) {
  if (dragging.clip != c.id || dragging.slide || !dragging.moved) return c;
  const uint64_t key = 0x5741525000000000ull ^ (static_cast<uint64_t>(c.id) << 24) ^ dragging.id;
  const double g = scrollui::glide(key, dragging.target, glide, eps);
  if (std::fabs(g - dragging.target) <= eps) return c;
  static Clip drawn;  // (one marker is dragged at a time)
  drawn = c;
  moveWarpMarker(s, drawn, tm, dragging.id, g);
  return drawn;
}

bool removeHovered(App& a, const Clip& c) {
  const uint32_t id = hovered(c.id);
  if (!id) return false;
  Clip after = c;
  if (!deleteWarpMarker(after, id)) return false;
  a.doc.editClip(c.id, "remove warp marker", [&after](Clip& k) { k.warp = after.warp; });
  hover = Hover{};
  return true;
}

void clipMenu(App& a, ObjectId clip, const TempoMap& tm, Tick grid) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c || c->kind != ClipKind::Audio) return;
  if (!ImGui::BeginMenu(i18n::tr("Warp markers"))) return;
  if (ImGui::MenuItem(i18n::tr("Add at transients"))) {
    Clip after = *c;
    const int n = warpMarkersAtTransients(a.doc.song(), after, tm);
    if (n > 0) a.doc.editClip(clip, "warp markers at transients", [&after](Clip& k) { k.warp = after.warp; });
    a.setHint(n > 0 ? std::to_string(n) + " " + i18n::tr("warp markers added") : std::string(i18n::tr("No transients found")));
  }
  const double strength = std::clamp(a.settings.getNumber("playlist.quantizeStrength"), 0.0, 1.0);
  char q[96];
  std::snprintf(q, sizeof q, "%s (%.0f%%)", i18n::tr("Quantize to the grid"), strength * 100.0);
  if (ImGui::MenuItem(q, nullptr, false, grid > 0)) {
    Clip after = *c;
    const int n = quantizeAudioClip(a.doc.song(), after, tm, grid, strength);
    if (!(after.warp == c->warp)) a.doc.editClip(clip, "quantize audio", [&after](Clip& k) { k.warp = after.warp; });
    a.setHint(std::to_string(n) + " " + i18n::tr("warp markers quantized"));
  }
  if (ImGui::BeginMenu(i18n::tr("Quantize strength"))) {
    for (int pct : {100, 75, 50, 25})
      if (ImGui::MenuItem((std::to_string(pct) + "%").c_str(), nullptr, std::fabs(strength * 100.0 - pct) < 0.5))
        a.settings.set("playlist.quantizeStrength", pct / 100.0);
    ImGui::EndMenu();
  }
  // K-151: the song's swing (its preset or groove) on the audio, and the audio's groove as the song's swing.
  {
    char sw[128];
    std::snprintf(sw, sizeof sw, "%s (%.0f%%)", i18n::tr("Swing to the song's groove"), strength * 100.0);
    if (ImGui::MenuItem(sw)) grooveui::swingClip(a, clip, tm, strength);
    ui::Hint("Warp markers on the transients, moved onto the song's swung grid (its swing preset or groove)");
    if (ImGui::MenuItem(i18n::tr("Take its groove as the song's swing"))) {
      if (grooveui::fromClip(a, clip, tm)) a.showGroove = true;
    }
    ui::Hint("The timing and accents of the clip's transients become the song's swing (save it as a preset in the groove editor)");
  }
  bool snap = a.settings.getBool("playlist.warpSnapTransients");
  if (ImGui::MenuItem(i18n::tr("New markers snap to transients"), nullptr, &snap)) a.settings.set("playlist.warpSnapTransients", snap);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Clear warp markers"), nullptr, false, !c->warp.empty()))
    a.doc.editClip(clip, "clear warp markers", [](Clip& k) { k.warp.clear(); });
  ImGui::EndMenu();
}

}  // namespace gl::plwarp
