// Gridline — playlist: pattern, audio and automation clips arranged on tracks
// (song mode), song markers in the ruler.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include "ExportUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/MenuOpsExtra.h"
#include "../core/PluginParams.h"
#include "../engine/Sample.h"
#include "App.h"
#include "../core/NoteTools.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"

namespace gl {

using theme::pal;

namespace {

struct ClipGesture {
  enum Kind { None, Move, Resize, Trim, Select, Paint, Erase, Scrub, TimeSel, AutoPoint, AutoTension, Mute, Slip, Play, Zoom } kind = None;
  int muteTo = -1;     // mute tool: state given to every touched clip
  Tick slipBase = 0;   // slip tool: the clip's offset at the start
  ImVec2 start;
  Tick startTick = 0;
  int startTrack = 0;
  std::vector<Clip> base;
  ObjectId anchor = kNoId;
  uint64_t key = 0;
  bool moved = false;
  Tick lastScrub = -1;  // last position sent while scrubbing (only send changes)
  // Automation point editing.
  ObjectId autoId = kNoId;
  int point = -1;
  float baseTension = 0;
  bool grouped = false;
};
ClipGesture cg;
uint64_t clipGestureCounter = 1;

void drawClipPreview(ImDrawList* dl, const Song& s, const Pattern& p, ImVec2 a, ImVec2 b, Tick clipLen, Tick offset, ImU32 col) {
  Tick patLen = std::max<Tick>(1, s.patternContentLength(p));
  float w = b.x - a.x;
  if (w < 6) return;
  // Notes (pitch-scaled) from every channel, then steps as ticks at the bottom.
  // The clip shows pattern ticks [offset, offset + clipLen).
  int lo = 127, hi = 0;
  for (const auto& [ch, ns] : p.notes)
    for (const auto& n : ns) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  float h = b.y - a.y;
  float span = static_cast<float>(std::max(hi - lo, 8));
  auto xAt = [&](Tick t) { return a.x + w * static_cast<float>(t - offset) / static_cast<float>(clipLen); };
  for (Tick rep = 0; rep < offset + clipLen; rep += patLen) {
    for (const auto& [ch, ns] : p.notes)
      for (const auto& n : ns) {
        Tick t = rep + n.start;
        if (t >= offset + clipLen || t + n.length <= offset) continue;
        float x0 = xAt(std::max(t, offset)), x1 = xAt(std::min(offset + clipLen, t + n.length));
        float y = a.y + 2 + (h - 6) * (1.0f - (n.pitch - lo) / span);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x0 + 1.5f, x1), y + 2), col);
      }
    Tick stepT = s.ticksPerStep(p);
    if (stepT > 0) {
      int rowIdx = 0;
      for (const auto& [ch, row] : p.steps) {
        for (size_t k = 0; k < row.size(); ++k) {
          if (!row[k]) continue;
          Tick t = rep + static_cast<Tick>(k) * stepT;
          if (t >= offset + clipLen) break;
          if (t < offset) continue;
          float x = xAt(t);
          float y = b.y - 3 - (rowIdx % 4) * 3.0f;
          dl->AddRectFilled(ImVec2(x, y - 2), ImVec2(x + 2, y), col);
        }
        ++rowIdx;
      }
    }
  }
}

// ---- audio clip waveforms: min/max per block of frames, cached per sample ----
struct Peaks {
  static constexpr size_t kBin = 128;
  std::vector<float> lo, hi;
};
const Peaks& peaksFor(const SamplePtr& s) {
  static std::map<const Sample*, std::pair<SamplePtr, Peaks>> cache;
  auto it = cache.find(s.get());
  if (it != cache.end()) return it->second.second;
  if (cache.size() > 256) cache.clear();
  Peaks p;
  size_t n = s->frames(), bins = (n + Peaks::kBin - 1) / Peaks::kBin;
  p.lo.assign(bins, 0.f);
  p.hi.assign(bins, 0.f);
  for (size_t b = 0; b < bins; ++b) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = b * Peaks::kBin; i < std::min(n, (b + 1) * Peaks::kBin); ++i) {
      float v = 0.5f * (s->left[i] + s->right[i]);
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    p.lo[b] = lo, p.hi[b] = hi;
  }
  return (cache[s.get()] = {s, std::move(p)}).second;
}

// Source seconds an audio clip plays at song tick t (display approximation of
// the engine's mapping: resample / stretch / tempo sync).
double audioSourceAt(const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, double t) {
  double tf = std::max(0.01, c.stretch);
  bool sampler = ch.plugin.type == "gl.sampler" && ch.plugin.params.size() > static_cast<size_t>(pp::SmpTimeStretch);
  if (sampler) tf *= ch.plugin.params[pp::SmpTimeStretch];
  if (c.syncBpm > 0) return c.audioStart + (t - static_cast<double>(c.start)) * 60.0 / (c.syncBpm * s.ppq) / tf;
  double speed = 1.0 / tf;
  if (sampler && ch.plugin.params[pp::SmpStretchMode] < 0.5f)
    speed *= std::pow(2.0, (ch.plugin.params[pp::SmpPitchShift] + c.pitch) / 1200.0);
  return c.audioStart + (tm.secondsAt(t) - tm.secondsAt(static_cast<double>(c.start))) * speed;
}

void drawWaveform(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                  float pxPerTick, float visX0, float visX1, ImU32 col) {
  SamplePtr smp = sampleCache().get(ch.plugin.data);
  if (!smp || smp->frames() < 2) return;
  const Peaks& pk = peaksFor(smp);
  const bool rev = c.reverse != (ch.plugin.type == "gl.sampler" && ch.plugin.params.size() > pp::SmpReverse &&
                                 ch.plugin.params[pp::SmpReverse] >= 0.5f);
  const double srcEnd = c.audioEnd > c.audioStart ? std::min(c.audioEnd, smp->seconds()) : smp->seconds();
  const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f;
  const float x0 = std::max(a.x, visX0), x1 = std::min(b.x, visX1);
  for (float x = std::floor(x0); x < x1; x += 1.0f) {
    double t0 = static_cast<double>(c.start) + (x - clipX0) / pxPerTick, t1 = t0 + 1.0 / pxPerTick;
    double s0 = audioSourceAt(s, ch, c, tm, t0), s1 = audioSourceAt(s, ch, c, tm, t1);
    if (s0 >= srcEnd) break;
    s1 = std::min(s1, srcEnd);
    double f0 = s0 * smp->sampleRate, f1 = s1 * smp->sampleRate;
    if (rev) {
      double last = static_cast<double>(smp->frames());
      std::swap(f0, f1);
      f0 = last - f0, f1 = last - f1;
    }
    long b0 = static_cast<long>(std::floor(std::min(f0, f1) / Peaks::kBin)), b1 = static_cast<long>(std::ceil(std::max(f0, f1) / Peaks::kBin));
    b0 = std::clamp<long>(b0, 0, static_cast<long>(pk.lo.size()) - 1);
    b1 = std::clamp<long>(std::max(b1, b0 + 1), 1, static_cast<long>(pk.lo.size()));
    float lo = 0, hi = 0;
    for (long k = b0; k < b1; ++k) lo = std::min(lo, pk.lo[static_cast<size_t>(k)]), hi = std::max(hi, pk.hi[static_cast<size_t>(k)]);
    float g = std::min(1.0f, c.gain);
    dl->AddLine(ImVec2(x + 0.5f, mid - std::clamp(hi * g, -1.f, 1.f) * half), ImVec2(x + 0.5f, mid - std::clamp(lo * g, -1.f, 1.f) * half + 1), col);
  }
}

// Automation curve geometry inside a clip body.
struct AutoGeom {
  ImVec2 a, b;  // body rect
  float clipX0 = 0, pxPerTick = 1;
  Tick start = 0, offset = 0, len = 0;
  float x(Tick content) const { return clipX0 + static_cast<float>(content - offset) * pxPerTick; }
  float y(double v) const { return b.y - static_cast<float>(v) * (b.y - a.y); }
  double valueAtY(float yy) const { return std::clamp(static_cast<double>((b.y - yy) / std::max(1.0f, b.y - a.y)), 0.0, 1.0); }
  bool visible(Tick content) const { return content >= offset && content <= offset + len; }
};

void drawAutomation(ImDrawList* dl, const AutomationClip& ac, const AutoGeom& g, float visX0, float visX1, ImU32 line, ImU32 fill,
                    bool handles, int hotPoint, int hotHandle, ImU32 hot) {
  float x0 = std::max(g.a.x, visX0), x1 = std::min(g.b.x, visX1);
  if (x1 <= x0) return;
  float prevY = 0;
  for (float x = std::floor(x0); x <= x1; x += 1.0f) {
    double content = static_cast<double>(g.offset) + (x - g.clipX0) / g.pxPerTick;
    float y = g.y(automationValue(ac, content));
    dl->AddLine(ImVec2(x, y), ImVec2(x, g.b.y), fill);
    if (x > std::floor(x0)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), line, 1.5f);
    prevY = y;
  }
  if (!handles) return;
  const auto& p = ac.points;
  for (size_t i = 0; i < p.size(); ++i) {
    if (!g.visible(p[i].pos)) continue;
    ImVec2 c(g.x(p[i].pos), g.y(p[i].value));
    bool hp = static_cast<int>(i) == hotPoint;
    dl->AddCircleFilled(c, hp ? 4.5f : 3.5f, hp ? hot : line);
    if (i == 0) continue;
    Tick mid = (p[i - 1].pos + p[i].pos) / 2;
    if (!g.visible(mid) || p[i].shape == AutoShape::Hold || g.x(p[i].pos) - g.x(p[i - 1].pos) < 14) continue;
    ImVec2 m(g.x(mid), g.y(automationValue(ac, static_cast<double>(mid))));
    dl->AddCircle(m, static_cast<int>(i) == hotHandle ? 4.0f : 3.0f, static_cast<int>(i) == hotHandle ? hot : line, 12, 1.2f);
  }
}

// Hit test: point index or tension handle (segment ending at point i).
void hitAutomation(const AutomationClip& ac, const AutoGeom& g, ImVec2 m, int& point, int& handle) {
  point = handle = -1;
  float best = 7.0f * 7.0f;
  const auto& p = ac.points;
  for (size_t i = 0; i < p.size(); ++i) {
    if (!g.visible(p[i].pos)) continue;
    float dx = m.x - g.x(p[i].pos), dy = m.y - g.y(p[i].value);
    if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, point = static_cast<int>(i);
  }
  if (point >= 0) return;
  for (size_t i = 1; i < p.size(); ++i) {
    Tick mid = (p[i - 1].pos + p[i].pos) / 2;
    if (!g.visible(mid) || p[i].shape == AutoShape::Hold || g.x(p[i].pos) - g.x(p[i - 1].pos) < 14) continue;
    float dx = m.x - g.x(mid), dy = m.y - g.y(automationValue(ac, static_cast<double>(mid)));
    if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, handle = static_cast<int>(i);
  }
}

std::string clipName(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->name : "Audio";
  }
  if (c.kind == ClipKind::Automation) {
    const AutomationClip* a = s.automation(c.source);
    return a ? a->name : "Automation";
  }
  const Pattern* p = s.pattern(c.pattern);
  return p ? p->name : "?";
}

Rgb clipColor(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->color : 0x7A8696;
  }
  if (c.kind == ClipKind::Automation) {
    const AutomationClip* a = s.automation(c.source);
    return a ? a->color : 0x7A8696;
  }
  const Pattern* p = s.pattern(c.pattern);
  return p ? p->color : 0x7A8696;
}

bool clipValid(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) return s.channel(c.source) != nullptr;
  if (c.kind == ClipKind::Automation) return s.automation(c.source) != nullptr;
  return s.pattern(c.pattern) != nullptr;
}

// Right-click menu of a clip: arrangement settings, then the pattern clip menu.
void arrangementClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return;
  if (c->kind != ClipKind::Pattern) ImGui::TextDisabled("%s", clipName(s, *c).c_str());
  if (ImGui::MenuItem("Mute clip", nullptr, c->muted)) a.doc.editClip(id, "mute clip", [](Clip& x) { x.muted = !x.muted; });
  if (c->kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c->source); ch && ImGui::MenuItem("Edit in audio editor", nullptr, false, !ch->plugin.data.empty()))
      packs::openAudioEditor(a, ch->plugin.data, ch->id);  // J-7
    if (ImGui::MenuItem("Export clip as WAV...")) exportui::exportClipAudio(a, id);  // J-20
    static uint64_t key = 0x41430000;
    float gainDb = c->gain > 0 ? 20.0f * std::log10(c->gain) : -60.0f;
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat("Gain", &gainDb, 0.1f, -60.0f, 12.0f, "%.1f dB"))
      a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f); }, key);
    float cents = c->pitch;
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat("Pitch", &cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct"))
      a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = cents; }, key + 1);
    float fi = static_cast<float>(c->fadeIn * 1000.0), fo = static_cast<float>(c->fadeOut * 1000.0);
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat("Fade in", &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0; }, key + 2);
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat("Fade out", &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0; }, key + 3);
    if (ImGui::MenuItem("Reverse", nullptr, c->reverse)) a.doc.editClip(id, "reverse clip", [](Clip& x) { x.reverse = !x.reverse; });
    if (ImGui::MenuItem("Follow tempo", nullptr, c->syncBpm > 0)) {
      TempoMap tm;
      tm.build(s);
      double bpm = tm.bpmAt(static_cast<double>(c->start));
      a.doc.editClip(id, "follow tempo", [bpm](Clip& x) { x.syncBpm = x.syncBpm > 0 ? 0.0 : bpm; });
    }
    if (ImGui::MenuItem("Channel settings")) a.settingsChannel = c->source;
    a.doc.endGesture();
    ImGui::Separator();
    audioClipMenu(a, id);  // J-8: properties, takes, stretch, fit, normalize, slice, consolidate, bounce
  } else if (c->kind == ClipKind::Automation) {
    const AutomationClip* ac = s.automation(c->source);
    if (ac) {
      std::string target = ac->owner == kNoId ? std::string("no target") : paramDisplayName(s, {ac->owner, ac->index});
      ImGui::TextDisabled("Drives: %s", target.c_str());
      if (ImGui::MenuItem("Edit automation...")) openAutomationEditor(a, ac->id, id);
      if (ImGui::BeginMenu("Curve of every segment")) {
        for (int k = 0; k < static_cast<int>(AutoShape::Count); ++k)
          if (ImGui::MenuItem(autoShapeName(static_cast<AutoShape>(k)))) {
            ObjectId aid = ac->id;
            a.doc.editSong("automation curve", [aid, k](Song& x) {
              if (AutomationClip* q = x.automation(aid))
                for (auto& p : q->points) p.shape = static_cast<AutoShape>(k);
            });
          }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Reset tensions")) {
        ObjectId aid = ac->id;
        a.doc.editSong("reset tensions", [aid](Song& x) {
          if (AutomationClip* q = x.automation(aid))
            for (auto& p : q->points) p.tension = 0;
        });
      }
      if (ImGui::MenuItem("Select all clips of this automation")) {
        a.playlist.selected.clear();
        for (const auto& k : s.playlist)
          if (k.kind == ClipKind::Automation && k.source == ac->id) a.playlist.selected.insert(k.id);
      }
    }
  } else {
    ImGui::Separator();
    clipContextMenu(a, id);
  }
}

}  // namespace

void drawPlaylist(App& a) {
  if (!a.showPlaylist) return;
  if (!ui::BeginDockWindow("Playlist", &a.showPlaylist, ImVec2(1000, 480))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  PlaylistState& st = a.playlist;
  ImGuiIO& io = ImGui::GetIO();
  const float winW = ImGui::GetContentRegionAvail().x;
  // Tempo map for audio clip drawing (rebuilt when the song changes).
  static TempoMap tempoMap;
  static uint64_t tempoRev = ~0ull;
  if (tempoRev != a.doc.revision()) {
    tempoMap.build(s);
    tempoRev = a.doc.revision();
  }

  // ---------------- toolbar ----------------
  ui::BeginToolbar("##pltb");
  {
    const float fh = ImGui::GetFrameHeight();
    menuButton("plmenu", "Playlist menu: edit, select, snap, view, tracks", [&a]() { playlistOptionsMenu(a); });
    ImGui::SameLine(0, 4);
    struct T { ui::Icon icon; Tool tool; const char* hint; uint32_t role; };
    const T tools[] = {{ui::Icon::Draw, Tool::Draw, "Draw (P): click to place the selected pattern, drag to move, drag the ends to resize or trim, right-click deletes", theme::role::kDraw},
                       {ui::Icon::Paint, Tool::Paint, "Paint (B): drag to place clips one after another", theme::role::kPaint},
                       {ui::Icon::Erase, Tool::Erase, "Delete (D): click or drag over clips", theme::role::kDelete},
                       {ui::Icon::Mute, Tool::Mute, "Mute (T): click or drag over clips to mute / unmute them", theme::role::kMute},
                       {ui::Icon::Slip, Tool::Slip, "Slip (S): drag inside a clip to move its content without moving the clip", theme::role::kSlip},
                       {ui::Icon::Slice, Tool::Slice, "Slice (C): click a clip to cut it there (Shift: every track)", theme::role::kSlice},
                       {ui::Icon::Select, Tool::Select, "Select (E): drag a box (Ctrl+drag works with any tool)", theme::role::kSelect},
                       {ui::Icon::Zoom, Tool::Zoom, "Zoom (Z): drag a range to fill the view, click to zoom in, right-click to zoom out", theme::role::kZoom},
                       {ui::Icon::Play, Tool::Play, "Play (Y): hold to play the song from the pointer", theme::role::kPlay}};
    for (const auto& t : tools) {
      if (ui::IconButton(t.hint, t.icon, st.tool == t.tool, fh, t.hint, theme::rgb(t.role))) st.tool = t.tool;
      ImGui::SameLine(0, 2);
    }
    // Groups right of the tools appear only when they fit (never clipped).
    auto roomLeft = []() { return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetItemRectMax().x; };
    if (roomLeft() > fh + 2 + 128 + 18) {
      ui::ToolbarDivider();
      ui::IconButton("snapicon", ui::Icon::Magnet, st.snap != Snap::None, fh, "Snap to grid (hold Alt while dragging to bypass)",
                     theme::rgb(theme::role::kSnap));
      ImGui::SameLine(0, 2);
      // Line / Cell show the grid they currently snap to, which follows the zoom.
      std::string snapLabel = snapName(st.snap);
      if ((st.snap == Snap::Line || st.snap == Snap::Cell) && st.effGrid > 0)
        snapLabel += " \xC2\xB7 " + timeline::gridName(s, st.effGrid);
      if (ui::BeginCombo("##snap", snapLabel.c_str(), 128)) {
        for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
          if (ImGui::Selectable(snapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) st.snap = static_cast<Snap>(i);
        ImGui::EndCombo();
      }
      ui::Hint("Snap: Line and Cell follow the zoom (free at the deepest zoom); hold Alt while dragging to invert snapping");
    }
    if (roomLeft() > fh * 3 + 4 + 18) {
      ui::ToolbarDivider();
      if (ui::IconButton("zout", ui::Icon::Minus, false, fh, "Zoom out (PgDn, Ctrl+wheel)")) st.zoomRequest /= 1.5f;
      ImGui::SameLine(0, 2);
      if (ui::IconButton("zin", ui::Icon::Plus, false, fh, "Zoom in (PgUp, Ctrl+wheel)")) st.zoomRequest *= 1.5f;
      ImGui::SameLine(0, 2);
      if (ui::IconButton("zfit", ui::Icon::Zoom, false, fh, "Zoom to fit the song (Home)", 0, theme::rgb(theme::role::kZoom, 0.85f))) st.fitRequest = true;
    }
    // Arrangement selector + live toggle when there is room (also in the playlist menu).
    ImGui::SameLine(0, 0);
    if (roomLeft() > 230) {
      ui::ToolbarDivider();
      arrangementSelector(a, winW >= 900 ? 150.0f : 110.0f);
      ImGui::SameLine(0, 2);
      const bool liveOn = a.engine.mode() == PlayMode::Live;
      if (ui::TextButton("LIVE", liveOn, 0, "Live mode: click clips in the live area to launch them (see also View \xE2\x86\x92 Live pads)"))
        a.engine.setMode(liveOn ? PlayMode::Song : PlayMode::Live);
    }
    // Right: the pattern that clicks place (colour chip + name), when there is room.
    const Pattern* cur = s.pattern(a.curPattern);
    ImGui::SameLine(0, theme::kS4);
    float room = ImGui::GetContentRegionAvail().x;
    if (room > 90) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      float labelW = room > 190 ? a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, "Placing").x + 8 : 0;
      float ty = std::round(p.y + (fh - theme::kFontCaption) * 0.5f) - 1;
      if (labelW > 0) tdl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x, ty), P.textFaint, "Placing");
      if (cur) tdl->AddRectFilled(ImVec2(p.x + labelW, p.y + 6), ImVec2(p.x + labelW + 4, p.y + fh - 6), theme::rgb(cur->color), 2);
      theme::textEllipsis(tdl, a.fontRegular, theme::kFontBody, ImVec2(p.x + labelW + 10, std::round(p.y + (fh - theme::kFontBody) * 0.5f) - 1),
                          room - labelW - 12, P.text, cur ? cur->name.c_str() : "No pattern");
      ImGui::Dummy(ImVec2(room - 1, fh));
      ui::Hint("Clicking in the playlist places this pattern");
    }
  }
  ui::EndToolbar();

  // ---------------- pattern picker (hidden when the window is narrow) ----------------
  const bool showPicker = winW >= 560;
  if (showPicker) {
    const float pickerW = std::clamp(winW * 0.15f, 128.0f, 180.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS2, 2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    ImGui::BeginChild("##picker", ImVec2(pickerW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ui::Label("PATTERNS");
    ImGui::Dummy(ImVec2(0, 2));
    const float rowH = 22.0f;
    for (const auto& p : s.patterns) {
      ImGui::PushID(static_cast<int>(p.id));
      ImVec2 q = ImGui::GetCursorScreenPos();
      bool sel = p.id == a.curPattern;
      float w = ImGui::GetContentRegionAvail().x;
      if (ImGui::InvisibleButton("##pp", ImVec2(w, rowH))) a.selectPattern(p.id);
      bool hov = ImGui::IsItemHovered();
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("GL_PATTERN", &p.id, sizeof p.id);
        ui::DragPreview(ui::Icon::Pattern, p.name.c_str(), theme::content(p.color));
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginPopupContextItem("##pmenu")) {
        patternContextMenu(a, p.id);
        ImGui::EndPopup();
      }
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      // Picker rows: a mini clip (colour strip + name) like the clips they place.
      const ImU32 pc = theme::rgb(p.color);
      pdl->AddRectFilled(q, ImVec2(q.x + w, q.y + rowH), sel ? theme::mix(P.surface700, pc, 0.18f) : hov ? P.surface700 : theme::withAlpha(P.surface800, 0.55f),
                         theme::kRadiusSm);
      pdl->AddRectFilled(q, ImVec2(q.x + 4, q.y + rowH), pc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      if (sel) pdl->AddRect(q, ImVec2(q.x + w, q.y + rowH), theme::withAlpha(P.hot, 0.55f), theme::kRadiusSm);
      theme::textEllipsis(pdl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 12, std::round(q.y + (rowH - 13) * 0.5f) - 1),
                          w - 18, sel && P.dark ? P.paper : P.text, p.name.c_str());
      ui::Hint("Click to select \xC2\xB7 drag into the playlist \xC2\xB7 right-click to rename, recolour, clone or delete");
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    // Divider between picker and arrangement.
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(r1.x, r0.y), ImVec2(r1.x, r1.y), P.line);
    ImGui::SameLine(0, 1);
  }

  // ---------------- arrangement canvas ----------------
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 120.0f);
  avail.y = std::max(avail.y, 100.0f);
  ImGui::InvisibleButton("##arr", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float headW = std::clamp(avail.x * 0.16f, 104.0f, 168.0f), rulerH = 26.0f;
  const ImVec2 gMin(origin.x + headW, origin.y + rulerH), gMax(origin.x + avail.x, origin.y + avail.y);
  const float trackH = st.trackHeight;
  const int nTracks = static_cast<int>(s.tracks.size());
  // Per-track heights (relative to the zoomable track height): prefix offsets.
  // Tracks inside a folded group take no room.
  std::vector<char> folded(static_cast<size_t>(nTracks), 0);
  std::vector<int> groupDepth(static_cast<size_t>(nTracks), 0);
  for (int i = 0; i < nTracks; ++i) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    for (int k = 1; k <= t.groupSize && i + k < nTracks; ++k) {
      if (t.folded || folded[static_cast<size_t>(i)]) folded[static_cast<size_t>(i + k)] = 1;
      groupDepth[static_cast<size_t>(i + k)] = groupDepth[static_cast<size_t>(i)] + 1;
    }
  }
  std::vector<float> trackY(static_cast<size_t>(nTracks) + 1, 0.0f);
  for (int i = 0; i < nTracks; ++i)
    trackY[static_cast<size_t>(i) + 1] = trackY[static_cast<size_t>(i)] +
        (folded[static_cast<size_t>(i)] ? 0.0f : std::round(trackH * std::clamp(s.tracks[static_cast<size_t>(i)].height, 0.25f, 4.0f)));
  const float totalH = trackY.back();
  st.scrollY = std::clamp(st.scrollY, 0.0f, std::max(0.0f, totalH - (gMax.y - gMin.y)));
  st.scrollX = std::max(0.0f, st.scrollX);
  auto xOf = [&](Tick t) { return gMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto yOf = [&](int tr) { return gMin.y + trackY[static_cast<size_t>(std::clamp(tr, 0, nTracks))] - st.scrollY; };
  auto hOf = [&](int tr) {
    tr = std::clamp(tr, 0, std::max(0, nTracks - 1));
    return nTracks ? trackY[static_cast<size_t>(tr) + 1] - trackY[static_cast<size_t>(tr)] : trackH;
  };
  auto tickAt = [&](float x) { return static_cast<Tick>(std::floor((x - gMin.x + st.scrollX) / st.pxPerTick)); };
  auto trackAt = [&](float y) {
    float yy = y - gMin.y + st.scrollY;
    auto it = std::upper_bound(trackY.begin(), trackY.end(), yy);
    return std::clamp(static_cast<int>(it - trackY.begin()) - 1, 0, std::max(0, nTracks - 1));
  };
  // ---- zoom: Ctrl+wheel at the pointer, PgUp/PgDn, Home = fit, toolbar buttons ----
  const float gridW = gMax.x - gMin.x;
  const float playX = static_cast<float>(a.engine.position()) * st.pxPerTick - st.scrollX;
  const float keyAnchor = hovered && io.MousePos.x >= gMin.x ? io.MousePos.x - gMin.x
                          : (playX >= 0 && playX <= gridW ? playX : gridW * 0.5f);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) st.zoomRequest *= 1.5f;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) st.zoomRequest /= 1.5f;
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) st.fitRequest = true;
  }
  if (st.fitRequest) {
    Tick len = std::max<Tick>(s.lengthTicks(), 4 * s.barTicks());
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, len + s.barTicks(), gridW));
    st.scrollX = 0;
    st.fitRequest = false;
  }
  if (st.zoomRequest != 1.0f) {
    timeline::zoomAt(s, st.pxPerTick, st.scrollX, st.zoomRequest, keyAnchor);
    st.zoomRequest = 1.0f;
  }
  st.pxPerTick = static_cast<float>(timeline::clampZoom(s, st.pxPerTick));
  // Auto scrolling: page along with the song position while the song plays
  // (not while the user drags or scrolls the view).
  if (a.engine.playing() && a.engine.mode() == PlayMode::Song && a.settings.getBool("ui.autoScroll") && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    st.scrollX = ops::followScroll(st.scrollX, static_cast<float>(a.engine.position()) * st.pxPerTick, gridW);

  {  // the engine loops playback inside the time selection
    static Tick sentA = -1, sentB = -1;
    Tick la = st.selEnd > st.selStart ? st.selStart : 0, lb = st.selEnd > st.selStart ? st.selEnd : 0;
    if (la != sentA || lb != sentB) {
      a.engine.setLoopRange(la, lb);
      sentA = la;
      sentB = lb;
    }
  }
  // ---- snapping: the visible grid (Line/Cell) or a fixed size; Alt inverts ----
  const Tick visGrid = timeline::visibleGrid(s, st.pxPerTick);
  const Tick snapG = timeline::snapGrid(s, st.snap, st.pxPerTick, io.KeyAlt);
  st.effGrid = timeline::snapGrid(s, st.snap, st.pxPerTick, false);
  auto snapPos = [&](Tick t) { return timeline::snapTick(t, snapG, st.snap); };

  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) {
      timeline::zoomAt(s, st.pxPerTick, st.scrollX, std::pow(1.2, static_cast<double>(io.MouseWheel)), io.MousePos.x - gMin.x);
    } else if (io.KeyAlt) {
      st.trackHeight = std::clamp(st.trackHeight + io.MouseWheel * 4, 24.0f, 120.0f);
    } else if (io.KeyShift) {
      st.scrollX = std::max(0.0f, st.scrollX - io.MouseWheel * 80);
    } else {
      st.scrollY -= io.MouseWheel * trackH;
    }
  }
  if (hovered && io.MouseWheelH != 0) st.scrollX = std::max(0.0f, st.scrollX - io.MouseWheelH * 80);
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
    st.scrollX = std::max(0.0f, st.scrollX - io.MouseDelta.x);
    st.scrollY -= io.MouseDelta.y;
  }

  // Background & grid: alternate bar shading, then track separators and bar/beat lines.
  dl->PushClipRect(gMin, gMax, true);
  dl->AddRectFilled(gMin, gMax, P.well);
  const Tick barT = std::max<Tick>(1, s.barTicks()), beatT = std::max<Tick>(1, s.beatTicks());
  // Alternate bar shading (every other bar a step lighter), like the reference grid.
  for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), barT); xOf(t) < gMax.x; t += barT)
    if ((t / barT) % 2 == 1) dl->AddRectFilled(ImVec2(xOf(t), gMin.y), ImVec2(xOf(t + barT), gMax.y), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    float y = yOf(tr);
    dl->AddLine(ImVec2(gMin.x, y + hOf(tr)), ImVec2(gMax.x, y + hOf(tr)), P.gridBar);
  }
  {
    // Lines at the visible grid: bars strongest, then beats, then finer subdivisions.
    const ImU32 fine = theme::withAlpha(P.gridBeat, 0.45f);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), visGrid); xOf(t) < gMax.x; t += visGrid) {
      ImU32 c = t % barT == 0 ? P.gridBar : t % beatT == 0 ? P.gridBeat : fine;
      dl->AddLine(ImVec2(std::round(xOf(t)), gMin.y), ImVec2(std::round(xOf(t)), gMax.y), c);
    }
  }
  // Markers: faint full-height lines (labels in the ruler).
  for (const auto& mk : s.markers) {
    float x = std::round(xOf(mk.pos));
    if (x >= gMin.x && x <= gMax.x) dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), theme::withAlpha(P.textDim, 0.35f));
  }
  // Live area: clips before its end are launched by hand (live mode).
  const bool liveMode = a.engine.mode() == PlayMode::Live;
  if (s.live.areaEnd > 0 && xOf(0) < gMax.x) {
    float lx1 = std::min(gMax.x, xOf(s.live.areaEnd));
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(0)), gMin.y), ImVec2(lx1, gMax.y), theme::withAlpha(P.paper, liveMode ? 0.05f : 0.03f));
    if (lx1 < gMax.x) dl->AddLine(ImVec2(std::round(lx1), gMin.y), ImVec2(std::round(lx1), gMax.y), P.lineStrong, 2.0f);
  }
  // Time selection: tinted band across the tracks.
  if (st.selEnd > st.selStart)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(st.selStart)), gMin.y), ImVec2(std::min(gMax.x, xOf(st.selEnd)), gMax.y),
                      theme::withAlpha(P.accent, 0.07f));
  // Song end shading.
  if (s.lengthTicks() > 0 && xOf(s.lengthTicks()) < gMax.x)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(s.lengthTicks())), gMin.y), gMax, theme::withAlpha(P.surface950, 0.35f));
  // Clips.
  ObjectId hoverClip = kNoId;
  bool hoverEdge = false, hoverLeftEdge = false, hoverBody = false;
  // Audio clip fade handles (J-8): 1 = fade in, 2 = fade out.
  static ObjectId fadeDragClip = kNoId;
  static int fadeDragKind = 0;
  ObjectId fadeHoverClip = kNoId;
  int fadeHoverKind = 0;
  int hoverPoint = -1, hoverHandle = -1;
  // Automation clip being edited: drawn from the live document (edits apply as they happen).
  for (const auto& c : s.playlist) {
    if (!clipValid(s, c)) continue;
    Tick len = s.clipLength(c);
    float x0 = xOf(c.start), x1 = xOf(c.start + len), y = yOf(c.track), th = hOf(c.track);
    if (x1 < gMin.x || x0 > gMax.x || y + th < gMin.y || y > gMax.y || th < 1) continue;
    bool sel = st.selected.count(c.id) > 0;
    const bool livePlaying = liveMode && a.engine.liveActive(c.track) == c.id;
    const bool liveWaiting = liveMode && a.engine.liveQueued(c.track) == c.id;
    bool muted = c.muted || (c.track < nTracks && s.tracks[static_cast<size_t>(c.track)].muted);
    ImU32 base = theme::rgb(clipColor(s, c));
    // Clip: translucent colour body, solid title bar with the name, darker rim;
    // selected clips get a lit rim.
    ImU32 body = theme::mix(base, P.well, muted ? 0.85f : 0.58f);
    ImU32 head = theme::mix(base, P.well, muted ? 0.65f : 0.08f);
    ImVec2 a0(std::round(x0) + 1, y + 1), a1(std::max(std::round(x0) + 5, std::round(x1) - 1), y + th - 1);
    const float headH = std::min(15.0f, (a1.y - a0.y) * 0.45f);
    dl->AddRectFilled(a0, a1, body, theme::kRadiusSm);
    dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + headH), head, theme::kRadiusSm, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(a0.x + 1, a0.y + 0.5f), ImVec2(a1.x - 1, a0.y + 0.5f), theme::withAlpha(P.paper, muted ? 0.08f : 0.28f));
    dl->AddLine(ImVec2(a0.x, a0.y + headH), ImVec2(a1.x, a0.y + headH), theme::withAlpha(P.ink, 0.35f));
    dl->AddRect(a0, a1, sel ? P.hot : theme::mix(base, P.ink, muted ? 0.7f : 0.45f), theme::kRadiusSm, 0, sel ? 2.0f : 1.0f);
    if (livePlaying) dl->AddRect(ImVec2(a0.x - 1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1), P.accent, theme::kRadiusSm, 0, 2.0f);
    if (liveWaiting)
      dl->AddRect(ImVec2(a0.x - 1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1),
                  theme::withAlpha(P.accent, 0.35f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f))), theme::kRadiusSm, 0, 2.0f);
    dl->PushClipRect(ImVec2(std::max(a0.x, gMin.x), std::max(a0.y, gMin.y)), ImVec2(std::min(a1.x, gMax.x), std::min(a1.y, gMax.y)), true);
    std::string name = clipName(s, c);
    if (c.muted) name += "  (muted)";
    theme::textEllipsis(dl, a.fontBold, std::min(theme::kFontCaption, headH - 2), ImVec2(std::max(a0.x, gMin.x) + 6, a0.y + std::max(0.0f, (headH - theme::kFontCaption) * 0.5f)),
                        a1.x - std::max(a0.x, gMin.x) - 10, theme::contrastText(head), name.c_str());
    const ImVec2 b0(a0.x + 3, a0.y + headH + 2), b1(a1.x - 3, a1.y - 3);
    // Clip content (notes, waveform, curve): lighter than the colour on dark
    // slate, darker than it on the light variant's pastel bodies.
    const ImU32 ink = P.dark ? theme::mix(base, P.paper, muted ? 0.0f : 0.45f) : theme::mix(base, P.ink, muted ? 0.15f : 0.42f);
    const ImU32 mark = P.dark ? P.paper : theme::mix(base, P.ink, 0.6f);
    bool inside = hovered && io.MousePos.x >= a0.x && io.MousePos.x < a1.x && io.MousePos.y >= a0.y && io.MousePos.y < a1.y && io.MousePos.y >= gMin.y;
    if (c.kind == ClipKind::Pattern) {
      drawClipPreview(dl, s, *s.pattern(c.pattern), b0, b1, len, c.offset, ink);
    } else if (c.kind == ClipKind::Audio) {
      if (b1.y - b0.y > 4) drawWaveform(dl, s, *s.channel(c.source), c, tempoMap, b0, b1, x0, st.pxPerTick, gMin.x, gMax.x, ink);
      double sec0 = tempoMap.secondsAt(static_cast<double>(c.start));
      double dur = tempoMap.secondsAt(static_cast<double>(c.start + len)) - sec0;
      auto xAtSec = [&](double sec) { return xOf(static_cast<Tick>(std::llround(tempoMap.tickAtSeconds(sec0 + sec)))); };
      if (c.fadeIn > 0 || c.fadeOut > 0) {  // fade curves (shaded outside)
        const ImU32 shade = theme::withAlpha(P.ink, 0.28f), line = theme::withAlpha(P.paper, 0.6f);
        if (c.fadeIn > 0) {
          float xf = xAtSec(c.fadeIn);
          dl->AddTriangleFilled(ImVec2(a0.x, b0.y), ImVec2(xf, b0.y), ImVec2(a0.x, b1.y), shade);
          dl->AddLine(ImVec2(a0.x, b1.y), ImVec2(xf, b0.y), line, 1.2f);
        }
        if (c.fadeOut > 0) {
          float xf = xAtSec(dur - c.fadeOut);
          dl->AddTriangleFilled(ImVec2(xf, b0.y), ImVec2(a1.x, b0.y), ImVec2(a1.x, b1.y), shade);
          dl->AddLine(ImVec2(xf, b0.y), ImVec2(a1.x, b1.y), line, 1.2f);
        }
      }
      // Fade handles at the top corners of the body (drag sideways).
      if ((inside || fadeDragClip == c.id) && b1.y - b0.y > 8 && a1.x - a0.x > 24) {
        const float hin = std::max(a0.x + 4, std::min(xAtSec(c.fadeIn), a1.x - 8));
        const float hout = std::min(a1.x - 4, std::max(xAtSec(dur - c.fadeOut), a0.x + 8));
        for (int k = 1; k <= 2; ++k) {
          const float hx = k == 1 ? hin : hout;
          const bool hot = (fadeDragClip == c.id && fadeDragKind == k) ||
                           (fadeDragClip == kNoId && std::fabs(io.MousePos.x - hx) < 5 && std::fabs(io.MousePos.y - b0.y) < 6);
          if (hot && fadeDragClip == kNoId) fadeHoverClip = c.id, fadeHoverKind = k;
          dl->AddRectFilled(ImVec2(hx - 3, b0.y - 3), ImVec2(hx + 3, b0.y + 3), hot ? P.hot : theme::withAlpha(P.paper, 0.75f), 1.0f);
        }
      }
    } else if (c.kind == ClipKind::Automation && b1.y - b0.y > 6) {
      const AutomationClip& ac = *s.automation(c.source);
      AutoGeom g{b0, b1, x0, st.pxPerTick, c.start, c.offset, len};
      int hp = -1, hh = -1;
      if (inside && io.MousePos.y > a0.y + headH) hitAutomation(ac, g, io.MousePos, hp, hh);
      if (cg.kind == ClipGesture::AutoPoint && cg.anchor == c.id) hp = cg.point;
      if (cg.kind == ClipGesture::AutoTension && cg.anchor == c.id) hh = cg.point;
      drawAutomation(dl, ac, g, gMin.x, gMax.x, ink, theme::withAlpha(base, muted ? 0.08f : 0.18f), inside || sel, hp, hh, mark);
      if (inside) hoverPoint = hp, hoverHandle = hh;
    }
    dl->PopClipRect();
    if (inside) {
      hoverClip = c.id;
      float edge = std::min(8.0f, (a1.x - a0.x) * 0.3f);
      hoverEdge = io.MousePos.x > a1.x - edge;
      hoverLeftEdge = !hoverEdge && io.MousePos.x < a0.x + edge;
      hoverBody = io.MousePos.y > a0.y + headH;
      if (c.kind != ClipKind::Automation) hoverPoint = hoverHandle = -1;
    }
  }
  // Empty arrangement: muted mark (guide §08 C).
  if (s.playlist.empty())
    ui::EmptyState(dl, gMin, gMax, "Empty arrangement", "Click to place the selected pattern, or drag one in from the list");
  // Playhead.
  if (a.engine.mode() == PlayMode::Song) {
    float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
    dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), P.playhead, 1.5f);
  }
  if (cg.kind == ClipGesture::Select && active) {
    dl->AddRectFilled(cg.start, io.MousePos, theme::withAlpha(P.accent, 0.12f));
    dl->AddRect(cg.start, io.MousePos, P.accent, 0, 0, 1.5f);
  }
  if (cg.kind == ClipGesture::Zoom && active) {
    const ImU32 zc = theme::rgb(theme::role::kZoom);
    dl->AddRectFilled(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), theme::withAlpha(zc, 0.10f));
    dl->AddRect(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), zc, 0, 0, 1.2f);
  }
  if (st.tool == Tool::Slice && hovered && io.MousePos.x >= gMin.x && io.MousePos.y >= gMin.y && hoverClip != kNoId) {
    const Tick cut = timeline::snapTick(std::max<Tick>(0, tickAt(io.MousePos.x)), snapG, Snap::Line);
    const float x = std::round(xOf(cut));
    const Clip* sc = s.clip(hoverClip);
    float y0 = io.KeyShift ? gMin.y : yOf(sc->track), y1 = io.KeyShift ? gMax.y : yOf(sc->track) + hOf(sc->track);
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), theme::rgb(theme::role::kSlice), 1.5f);
  }
  dl->PopClipRect();

  // Ruler: bar numbers, beat ticks, markers, song end marker, playhead cap.
  dl->AddRectFilled(origin, ImVec2(gMax.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(gMax.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  dl->PushClipRect(ImVec2(gMin.x, origin.y), ImVec2(gMax.x, gMin.y), true);
  int hoverMarker = -1;
  {
    // Time selection in the ruler.
    if (st.selEnd > st.selStart) {
      float sx0 = xOf(st.selStart), sx1 = xOf(st.selEnd);
      dl->AddRectFilled(ImVec2(sx0, origin.y + 2), ImVec2(sx1, gMin.y - 1), theme::withAlpha(P.accent, 0.38f), 2);
      dl->AddLine(ImVec2(sx0, origin.y + 2), ImVec2(sx0, gMin.y - 1), P.accent, 1.5f);
      dl->AddLine(ImVec2(sx1, origin.y + 2), ImVec2(sx1, gMin.y - 1), P.accent, 1.5f);
    }
    // Minor ticks on the visible grid; labels on a coarser grid that leaves room for text.
    const Tick labelGrid = timeline::visibleGrid(s, st.pxPerTick, 64.0);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), visGrid); xOf(t) < gMax.x; t += visGrid) {
      float x = std::round(xOf(t));
      bool major = t % labelGrid == 0;
      float len = major ? 10.0f : t % beatT == 0 ? 6.0f : 3.0f;
      dl->AddLine(ImVec2(x, gMin.y - len), ImVec2(x, gMin.y - 1), major ? P.lineStrong : P.line);
      if (major) {
        std::string lbl = timeline::rulerLabel(s, t, labelGrid);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 4, origin.y + 4), t % barT == 0 ? P.text : P.textFaint, lbl.c_str());
      }
    }
    // Markers: a flag with the name (time signature markers show the meter).
    for (size_t i = 0; i < s.markers.size(); ++i) {
      const Marker& mk = s.markers[i];
      float x = std::round(xOf(mk.pos));
      if (x < gMin.x - 200 || x > gMax.x) continue;
      std::string lbl = mk.timeSigNum > 0 ? std::to_string(mk.timeSigNum) + "/" + std::to_string(mk.timeSigDen) : mk.name;
      if (mk.timeSigNum > 0 && !mk.name.empty() && mk.name != lbl) lbl += "  " + mk.name;
      if (lbl.empty()) lbl = "\xE2\x80\xA2";  // bullet for unnamed markers
      static const char* const acts[] = {"", "loop", "skip", "pause", "stop"};
      if (mk.action != MarkerAction::None) lbl += std::string("  \xC2\xB7 ") + acts[static_cast<int>(mk.action)];
      float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str()).x;
      ImVec2 f0(x, origin.y + 13), f1(x + tw + 8, gMin.y - 2);
      bool hov = hovered && io.MousePos.x >= f0.x && io.MousePos.x < f1.x && io.MousePos.y >= origin.y && io.MousePos.y < gMin.y;
      if (hov) hoverMarker = static_cast<int>(i);
      dl->AddRectFilled(f0, f1, hov ? P.surface600 : P.surface700, 2, ImDrawFlags_RoundCornersRight);
      dl->AddLine(ImVec2(x, origin.y + 2), ImVec2(x, gMin.y - 1), mk.timeSigNum > 0 ? P.textDim : P.text, 1.5f);
      dl->AddText(mk.timeSigNum > 0 ? a.fontMono : a.fontRegular, theme::kFontLabel, ImVec2(x + 4, f0.y), mk.timeSigNum > 0 ? P.textDim : P.text,
                  lbl.c_str());
    }
    if (s.live.areaEnd > 0) {
      float lx = std::round(xOf(s.live.areaEnd));
      if (lx > gMin.x - 60 && lx < gMax.x + 60) {
        dl->AddLine(ImVec2(lx, origin.y + 2), ImVec2(lx, gMin.y - 1), P.lineStrong, 2.0f);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(lx - 34, origin.y + 13), liveMode ? P.text : P.textFaint, "LIVE");
      }
    }
    if (s.lengthTicks() > 0) {
      float xe = std::round(xOf(s.lengthTicks()));
      dl->AddTriangleFilled(ImVec2(xe - 5, gMin.y - 9), ImVec2(xe + 5, gMin.y - 9), ImVec2(xe, gMin.y - 2), P.textFaint);
    }
    if (a.engine.mode() == PlayMode::Song) {
      float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
      dl->AddTriangleFilled(ImVec2(x - 5, gMin.y - 9), ImVec2(x + 5, gMin.y - 9), ImVec2(x, gMin.y - 1), P.playhead);
    }
  }
  dl->PopClipRect();

  // Track headers.
  dl->PushClipRect(ImVec2(origin.x, gMin.y), ImVec2(gMin.x, gMax.y), true);
  dl->AddRectFilled(ImVec2(origin.x, gMin.y), ImVec2(gMin.x, gMax.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(gMin.x - 1, gMin.y), ImVec2(gMin.x - 1, gMax.y), P.dark ? P.ink : P.line);
  int headResize = -1;  // track whose header's bottom edge is under the pointer
  static int renaming = -1;
  static ObjectId menuClip = kNoId;
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    float y = yOf(tr), th = hOf(tr);
    if (th < 1) continue;  // inside a folded group
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    const float indent = 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    bool hovHead = hovered && io.MousePos.x < gMin.x && io.MousePos.y >= y && io.MousePos.y < y + th && io.MousePos.y >= gMin.y;
    // Header: raised slate face tinted by the track colour, colour strip, name,
    // mute lamp (top right), resize grip along the bottom edge.
    ImVec2 h0(origin.x + 2, y + 1), h1(gMin.x - 3, y + th - 1);
    const ImU32 tc = theme::content(t.color);
    ImU32 face = theme::mix(hovHead ? P.surface700 : P.surface800, tc, t.muted ? 0.04f : 0.12f);
    dl->AddRectFilled(h0, h1, theme::mix(face, P.ink, 0.35f), theme::kRadiusSm);
    dl->AddRectFilled(h0, ImVec2(h1.x, h1.y - 1), face, theme::kRadiusSm);
    dl->AddLine(ImVec2(h0.x + 6, h0.y + 0.5f), ImVec2(h1.x - 2, h0.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.10f : 0.5f));
    dl->AddRectFilled(h0, ImVec2(h0.x + 5, h1.y), tc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    const bool nearEdge = hovered && io.MousePos.x < gMin.x && std::fabs(io.MousePos.y - (y + th)) < 3.5f && io.MousePos.y >= gMin.y;
    if (nearEdge) headResize = tr;
    if (hovHead || nearEdge) {
      const float gx = (h0.x + h1.x) * 0.5f;
      for (int d = -2; d <= 2; ++d)
        dl->AddCircleFilled(ImVec2(gx + static_cast<float>(d) * 4.0f, h1.y - 3), 1.0f, nearEdge ? P.hot : P.textFaint, 6);
    }
    float nameY = std::min(y + 7.0f, y + (th - theme::kFontCaption) * 0.5f - 1);
    float nameX = h0.x + 12 + indent;
    if (t.groupSize > 0) {  // group: fold arrow
      ImVec2 c(nameX + 4, nameY + 7);
      if (t.folded)
        dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 4), ImVec2(c.x - 3, c.y + 4), ImVec2(c.x + 3, c.y), P.textDim);
      else
        dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x, c.y + 3), P.textDim);
      nameX += 12;
    }
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(nameX, nameY), h1.x - nameX - (liveMode ? 36 : 22),
                        t.muted ? P.textFaint : P.text, playlistTrackName(s, static_cast<size_t>(tr)).c_str());
    if (liveMode && a.engine.liveActive(tr) != kNoId)  // live: a clip plays on this track
      dl->AddTriangleFilled(ImVec2(h1.x - 30, nameY + 3), ImVec2(h1.x - 30, nameY + 11), ImVec2(h1.x - 24, nameY + 7), P.accent);
    ui::Led(dl, ImVec2(h1.x - 11, nameY + 7), 4.5f, !t.muted);
    if (t.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(h1.x - (liveMode ? 40 : 26), nameY + 7), 11, P.textDim);
  }
  dl->PopClipRect();
  // Corner above the headers.
  dl->AddRectFilled(origin, ImVec2(gMin.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(gMin.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(origin.x + 10, origin.y + 7), P.textFaint, "TRACKS");

  // ---------------- interaction ----------------
  const ImVec2 m = io.MousePos;
  const bool inGrid = m.x >= gMin.x && m.x < gMax.x && m.y >= gMin.y && m.y < gMax.y;
  const bool inRuler = m.y >= origin.y && m.y < gMin.y && m.x >= gMin.x;
  const bool inHead = m.x >= origin.x && m.x < gMin.x && m.y >= gMin.y;
  const Clip* hc = hoverClip != kNoId ? s.clip(hoverClip) : nullptr;
  const bool hoverAuto = hc && hc->kind == ClipKind::Automation && hoverBody;
  if (hovered && inGrid) {
    if (hoverPoint >= 0 || hoverHandle >= 0)
      ImGui::SetMouseCursor(hoverHandle >= 0 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Hand);
    else if (hoverClip != kNoId && st.tool == Tool::Draw)
      ImGui::SetMouseCursor(hoverEdge || hoverLeftEdge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
    std::string where = "Bar " + std::to_string(tickAt(m.x) / std::max<Tick>(1, s.barTicks()) + 1) + " · " +
                        playlistTrackName(s, static_cast<size_t>(trackAt(m.y)));
    if (liveMode && hc && inLiveArea(s, *hc))
      a.setHint(where + "   (live: click to launch on the track's launch grid \xC2\xB7 right-click to stop the track)");
    else if (hoverAuto)
      a.setHint(where + "   (drag a point · drag a ring: curve tension · double-click: add point · right-click a point: delete · double-click the title: editor)");
    else
      a.setHint(where + "   (wheel: scroll · Shift+wheel: horizontal · Ctrl+wheel: zoom · Alt+wheel: track height · drag a clip's left edge: trim · double-click clip: edit)");
  } else if (hovered && inRuler) {
    ImGui::SetMouseCursor(hoverMarker >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
    if (hoverMarker >= 0)
      a.setHint("Marker \"" + s.markers[static_cast<size_t>(hoverMarker)].name + "\": click to jump here \xC2\xB7 right-click: rename, time signature, action, loop");
    else
      a.setHint("Click or drag to move the song position (snaps to the grid, Alt = free)  \xC2\xB7  Ctrl+drag or right-drag: "
                "time selection (loops playback)  \xC2\xB7  double-click: clear the selection");
  } else if (hovered && inHead) {
    a.setHint("Click the dot to mute · double-click to rename · right-click for colour");
  }

  // Automation point edits: one undo step per gesture.
  auto editAuto = [&](ObjectId aid, const std::function<void(AutomationClip&)>& fn) {
    if (!cg.grouped) {
      a.doc.beginGroup("edit automation");
      cg.grouped = true;
    }
    a.doc.editSong("edit automation", [&](Song& x) {
      if (AutomationClip* q = x.automation(aid)) fn(*q);
    });
  };

  bool lClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool rClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  {  // Track resize: drag the bottom edge of a track header (one undo step per drag).
    static int resizing = -1;
    static float resizeY0 = 0, resizeH0 = 1;
    if (resizing >= 0 && resizing < nTracks) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float nh = std::clamp(resizeH0 + (m.y - resizeY0) / std::max(1.0f, trackH), 0.25f, 4.0f);
        if (std::fabs(nh - s.tracks[static_cast<size_t>(resizing)].height) > 0.01f)
          a.doc.editPlaylistTrack(resizing, "resize playlist track", [nh](PlaylistTrack& t) { t.height = nh; });
      } else {
        a.doc.endGroup();
        resizing = -1;
      }
      lClick = false;
    } else if (headResize >= 0) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      a.setHint("Drag to resize the track");
      if (lClick) {
        resizing = headResize;
        resizeY0 = m.y;
        resizeH0 = s.tracks[static_cast<size_t>(headResize)].height;
        a.doc.beginGroup("resize playlist track");
        lClick = false;
      }
    }
  }
  // Live mode: clicking a clip of the live area launches it (held = gate pads).
  static ObjectId liveHeld = kNoId;
  if (liveMode && lClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.launchClip(hc->id);
    liveHeld = hc->id;
    lClick = false;
  } else if (liveMode && rClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.stopLiveTrack(hc->track);
    rClick = false;
  }
  if (liveHeld != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(liveHeld);
    liveHeld = kNoId;
  }
  // Group fold arrow in the track header.
  if (lClick && inHead) {
    int tr = trackAt(m.y);
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    float ax = origin.x + 4 + 12 + 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    if (t.groupSize > 0 && m.x >= ax - 4 && m.x < ax + 12) {
      a.doc.editPlaylistTrack(tr, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
      lClick = false;
    }
  }
  // J-11: locked tracks keep their clips (only selection, play, zoom and menus work there).
  auto trackLocked = [&](int tr) { return tr >= 0 && tr < nTracks && s.tracks[static_cast<size_t>(tr)].locked; };
  const bool editTool = st.tool != Tool::Select && st.tool != Tool::Play && st.tool != Tool::Zoom && !io.KeyCtrl;
  if (inGrid && editTool && trackLocked(trackAt(m.y))) {
    if (hovered) a.setHint("This track is locked (track menu \xE2\x86\x92 Lock)");
    if (lClick) lClick = false;
    if (rClick && !(hc && m.y < yOf(hc->track) + std::min(16.0f, (hOf(hc->track) - 4) * 0.45f) + 2)) rClick = false;
  }
  // Fade handle drags (J-8): one undo step per drag.
  if (fadeDragClip != kNoId) {
    const Clip* fc = s.clip(fadeDragClip);
    if (fc && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      const double sec0 = tempoMap.secondsAt(static_cast<double>(fc->start));
      const double dur = tempoMap.secondsAt(static_cast<double>(fc->start + s.clipLength(*fc))) - sec0;
      const double at = std::clamp(tempoMap.secondsAt(static_cast<double>(std::max<Tick>(0, tickAt(m.x)))) - sec0, 0.0, dur);
      const int kind = fadeDragKind;
      const double v = kind == 1 ? std::min(at, dur - fc->fadeOut) : std::min(dur - at, dur - fc->fadeIn);
      a.doc.editClip(fadeDragClip, kind == 1 ? "clip fade in" : "clip fade out", [kind, v](Clip& x) {
        (kind == 1 ? x.fadeIn : x.fadeOut) = std::max(0.0, v);
      }, 0x46414445ull ^ (static_cast<uint64_t>(fadeDragClip) << 8));
      char b[48];
      std::snprintf(b, sizeof b, "Fade %s: %.0f ms", kind == 1 ? "in" : "out", std::max(0.0, v) * 1000.0);
      a.setHint(b);
    } else {
      a.doc.endGesture();
      fadeDragClip = kNoId;
    }
    lClick = false;
  } else if (fadeHoverClip != kNoId) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    a.setHint(fadeHoverKind == 1 ? "Drag: fade in" : "Drag: fade out");
    if (lClick) {
      fadeDragClip = fadeHoverClip;
      fadeDragKind = fadeHoverKind;
      lClick = false;
    }
  }
  static int menuMarker = -1;
  if (inRuler && rClick && hoverMarker >= 0) {  // right-click a marker: its menu
    menuMarker = hoverMarker;
    ImGui::OpenPopup("##markmenu");
    rClick = false;
  }
  if (lClick || rClick) {
    cg = ClipGesture{};
    cg.start = m;
    cg.startTick = tickAt(m.x);
    cg.startTrack = trackAt(m.y);
    cg.base = s.playlist;
    cg.anchor = hoverClip;
    cg.key = 0x504C000000000000ull ^ (clipGestureCounter++ << 8);
    if (inRuler && lClick && hoverMarker >= 0) {
      a.engine.setMode(PlayMode::Song);
      a.engine.setPosition(s.markers[static_cast<size_t>(hoverMarker)].pos);  // jump to the marker
    } else if (inRuler && lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      st.selStart = st.selEnd = 0;  // double-click clears the time selection
    } else if (inRuler && (rClick || (lClick && io.KeyCtrl))) {
      cg.kind = ClipGesture::TimeSel;
    } else if (inRuler && lClick) {
      a.engine.setMode(PlayMode::Song);
      cg.kind = ClipGesture::Scrub;  // the position follows the pointer until release
    } else if (inHead) {
      int tr = trackAt(m.y);
      float y = yOf(tr);
      if (lClick && m.x > gMin.x - 26 && m.y < y + 26) {
        a.doc.editPlaylistTrack(tr, "mute playlist track", [](PlaylistTrack& t) { t.muted = !t.muted; });
      } else if (lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        renaming = tr;
        ImGui::OpenPopup("##trname");
      } else if (rClick) {
        renaming = tr;
        ImGui::OpenPopup("##trmenu");
      }
    } else if (inGrid && hoverAuto && rClick && hoverPoint >= 0) {
      const AutomationClip* ac = s.automation(hc->source);
      if (ac && ac->points.size() > 1) {  // right-click a point: delete it
        int k = hoverPoint;
        ObjectId aid = ac->id;
        cg.anchor = kNoId;
        a.doc.editSong("delete automation point", [aid, k](Song& x) {
          AutomationClip* q = x.automation(aid);
          if (q && k >= 0 && static_cast<size_t>(k) < q->points.size()) q->points.erase(q->points.begin() + k);
        });
      }
    } else if (inGrid && rClick && hc && m.y < yOf(hc->track) + std::min(16.0f, (hOf(hc->track) - 4) * 0.45f) + 2) {
      menuClip = hoverClip;  // right-click on the clip's title bar: clip menu
      ImGui::OpenPopup("##clipmenu");
    } else if (inGrid && rClick && st.tool == Tool::Zoom) {
      st.zoomRequest /= 1.5f;
    } else if (inGrid && rClick) {
      cg.kind = ClipGesture::Erase;
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Mute) {
      cg.kind = ClipGesture::Mute;
      if (hc) cg.muteTo = hc->muted ? 0 : 1;
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Slip) {
      if (hc && hc->kind != ClipKind::Audio) {
        cg.kind = ClipGesture::Slip;
        cg.slipBase = hc->offset;
      } else if (hc) {
        a.notify("Audio clips slide their content in the audio clip settings");
      }
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Slice) {
      const Tick cut = timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line);
      if (io.KeyShift) {
        ops::sliceAt(a.doc, cut, -1);
      } else if (hc && hc->kind == ClipKind::Audio) {
        a.notify("Audio clips are cut in the audio clip tools");
      } else if (hc) {
        if (ObjectId r = ops::sliceClip(a.doc, hc->id, cut)) st.selected = {r};
      }
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Play) {
      a.engine.setMode(PlayMode::Song);
      a.engine.setPosition(std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line)));
      if (!a.engine.playing()) a.engine.play();
      cg.kind = ClipGesture::Play;
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Zoom) {
      cg.kind = ClipGesture::Zoom;
    } else if (inGrid && lClick && hoverAuto && st.tool == Tool::Draw && !io.KeyCtrl && (hoverPoint >= 0 || hoverHandle >= 0)) {
      const AutomationClip* ac = s.automation(hc->source);
      cg.autoId = ac->id;
      if (hoverPoint >= 0) {
        cg.kind = ClipGesture::AutoPoint;
        cg.point = hoverPoint;
      } else {
        cg.kind = ClipGesture::AutoTension;
        cg.point = hoverHandle;
        cg.baseTension = ac->points[static_cast<size_t>(hoverHandle)].tension;
      }
    } else if (inGrid && lClick && hoverAuto && st.tool == Tool::Draw && !io.KeyCtrl && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      // Double-click on the curve: add a point there and keep dragging it.
      const AutomationClip* ac = s.automation(hc->source);
      const float headH = std::min(16.0f, (hOf(hc->track) - 4) * 0.45f);
      AutoGeom g{ImVec2(xOf(hc->start) + 4, yOf(hc->track) + 2 + headH + 2), ImVec2(xOf(hc->start + s.clipLength(*hc)) - 4, yOf(hc->track) + hOf(hc->track) - 5),
                 xOf(hc->start), st.pxPerTick, hc->start, hc->offset, s.clipLength(*hc)};
      Tick pos = hc->offset + std::max<Tick>(0, snapPos(tickAt(m.x)) - hc->start);
      AutoPoint np{pos, static_cast<float>(g.valueAtY(m.y)), 0.0f, AutoShape::Curve};
      ObjectId aid = ac->id;
      int idx = 0;
      cg.autoId = aid;
      editAuto(aid, [&](AutomationClip& q) { idx = insertAutoPoint(q, np); });
      cg.kind = ClipGesture::AutoPoint;
      cg.point = idx;
    } else if (inGrid && lClick) {
      if (hoverClip != kNoId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (hc->kind == ClipKind::Pattern) {
          a.selectPattern(hc->pattern);
          a.showRack = true;
          a.focusWindow = "Channel rack";
        } else if (hc->kind == ClipKind::Audio) {
          a.settingsChannel = hc->source;  // the channel that plays the clip
        } else if (hc->kind == ClipKind::Automation) {
          openAutomationEditor(a, hc->source, hc->id);  // J-5: automation clip editor
        }
      } else if (io.KeyCtrl || st.tool == Tool::Select) {
        cg.kind = ClipGesture::Select;
        if (!io.KeyShift) st.selected.clear();
      } else if (st.tool == Tool::Erase) {
        cg.kind = ClipGesture::Erase;
      } else if (st.tool == Tool::Paint) {
        cg.kind = ClipGesture::Paint;
      } else if (hoverClip != kNoId) {
        if (!st.selected.count(hoverClip)) {
          if (!io.KeyShift) st.selected.clear();
          st.selected.insert(hoverClip);
        }
        if (hc->kind == ClipKind::Pattern) a.selectPattern(hc->pattern);
        cg.kind = hoverEdge ? ClipGesture::Resize : hoverLeftEdge ? ClipGesture::Trim : ClipGesture::Move;
      } else if (s.pattern(a.curPattern)) {
        ObjectId id = a.doc.addClip(a.curPattern, trackAt(m.y), std::max<Tick>(0, snapDown(tickAt(m.x), snapG)));
        if (id) {
          st.selected = {id};
          cg.base = a.doc.song().playlist;
          cg.anchor = id;
          cg.startTick = a.doc.song().clip(id)->start;
          cg.kind = ClipGesture::Move;
        }
      }
    }
  }

  if (active && cg.kind != ClipGesture::None) {
    // Dragging past the left/right edge scrolls the view (scrub, time selection, clips).
    if (cg.kind != ClipGesture::Select && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) + ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f)) {
      const float edge = 24.0f;
      if (m.x > gMax.x - edge) st.scrollX += (m.x - (gMax.x - edge)) * 0.35f;
      if (m.x < gMin.x + edge) st.scrollX = std::max(0.0f, st.scrollX - ((gMin.x + edge) - m.x) * 0.35f);
    }
    Tick curTick = std::max<Tick>(0, tickAt(std::clamp(m.x, gMin.x, gMax.x)));
    int curTrack = trackAt(m.y);
    auto baseClip = [&]() -> const Clip* {
      for (const auto& c : cg.base)
        if (c.id == cg.anchor) return &c;
      return nullptr;
    };
    switch (cg.kind) {
      case ClipGesture::Scrub: {
        // Smooth: follows the pointer every frame, landing on the snap grid (Alt = free).
        Tick t = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
        if (t != cg.lastScrub) {
          a.engine.setPosition(t);
          cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::TimeSel: {
        Tick t0 = std::max<Tick>(0, timeline::snapTick(cg.startTick, snapG, Snap::Line));
        Tick t1 = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
        st.selStart = std::min(t0, t1);
        st.selEnd = std::max(t0, t1);
        break;
      }
      case ClipGesture::Move: {
        const Clip* an = baseClip();
        if (!an) break;
        Tick target = snapPos(an->start + (curTick - cg.startTick));
        Tick dt = target - an->start;
        int dtr = curTrack - cg.startTrack;
        for (const auto& c : cg.base)
          if (st.selected.count(c.id)) {
            dt = std::max<Tick>(dt, -c.start);
            dtr = std::clamp(dtr, -c.track, nTracks - 1 - c.track);
          }
        if (dt == 0 && dtr == 0 && !cg.moved) break;
        {
          bool blocked = false;  // locked tracks neither give nor take clips
          for (const auto& c : cg.base)
            if (st.selected.count(c.id)) blocked = blocked || trackLocked(c.track) || trackLocked(c.track + dtr);
          if (blocked) break;
        }
        cg.moved = true;
        auto sel = st.selected;
        auto base = cg.base;
        a.doc.editClips("move clips", [&](std::vector<Clip>& cs) {
          for (size_t i = 0; i < cs.size() && i < base.size(); ++i)
            if (sel.count(cs[i].id)) {
              cs[i].start = base[i].start + dt;
              cs[i].track = base[i].track + dtr;
            }
        }, cg.key);
        break;
      }
      case ClipGesture::Resize: {
        const Clip* an = baseClip();
        if (!an) break;
        Tick oldLen = s.clipLength(*an);
        Tick end = std::max(timeline::snapTick(curTick, snapG, Snap::Line), an->start + std::max<Tick>(snapG, 1));
        Tick d = end - (an->start + oldLen);
        auto sel = st.selected;
        auto base = cg.base;
        const Song& song = a.doc.song();
        a.doc.editClips("resize clips", [&](std::vector<Clip>& cs) {
          for (size_t i = 0; i < cs.size() && i < base.size(); ++i)
            if (sel.count(cs[i].id)) cs[i].length = std::max<Tick>(1, song.clipLength(base[i]) + d);
        }, cg.key);
        break;
      }
      case ClipGesture::Trim: {  // left edge: later start, content shifts with it (offset)
        const Clip* an = baseClip();
        if (!an) break;
        Tick newStart = timeline::snapTick(curTick, snapG, Snap::Line);
        Tick d = newStart - an->start;
        auto sel = st.selected;
        auto base = cg.base;
        const Song& song = a.doc.song();
        // Every selected clip moves its start by d, limited by its content start and a minimum length.
        for (const auto& c : base)
          if (sel.count(c.id)) {
            Tick len = song.clipLength(c);
            d = std::min(d, len - 1);
            d = std::max(d, -c.start);
            if (c.kind != ClipKind::Audio) d = std::max(d, -c.offset);
          }
        if (d == 0 && !cg.moved) break;
        cg.moved = true;
        a.doc.editClips("trim clips", [&](std::vector<Clip>& cs) {
          for (size_t i = 0; i < cs.size() && i < base.size(); ++i) {
            if (!sel.count(cs[i].id)) continue;
            const Clip& b = base[i];
            Tick len = song.clipLength(b);
            cs[i].start = b.start + d;
            cs[i].length = len - d;
            if (b.kind == ClipKind::Audio) {
              const Channel* ch = song.channel(b.source);
              double src = ch ? audioSourceAt(song, *ch, b, tempoMap, static_cast<double>(b.start + d)) : b.audioStart;
              cs[i].audioStart = std::max(0.0, src);
            } else {
              cs[i].offset = std::max<Tick>(0, b.offset + d);
            }
          }
        }, cg.key);
        break;
      }
      case ClipGesture::AutoPoint: {
        const Clip* an = baseClip();
        const AutomationClip* ac = s.automation(cg.autoId);
        if (!an || !ac || cg.point < 0 || cg.point >= static_cast<int>(ac->points.size())) break;
        const Clip* live = s.clip(an->id);
        if (!live) break;
        Tick len = s.clipLength(*live);
        const float headH = std::min(16.0f, (hOf(live->track) - 4) * 0.45f);
        AutoGeom g{ImVec2(0, yOf(live->track) + 2 + headH + 2), ImVec2(0, yOf(live->track) + hOf(live->track) - 5), xOf(live->start),
                   st.pxPerTick, live->start, live->offset, len};
        int k = cg.point;
        Tick lo = k > 0 ? ac->points[static_cast<size_t>(k) - 1].pos : 0;
        Tick hi = k + 1 < static_cast<int>(ac->points.size()) ? ac->points[static_cast<size_t>(k) + 1].pos : std::numeric_limits<Tick>::max() / 4;
        Tick pos = std::clamp(live->offset + snapPos(curTick) - live->start, lo, hi);
        if (k == 0 && ac->points.size() > 1) pos = std::min<Tick>(pos, ac->points[1].pos);
        float val = static_cast<float>(g.valueAtY(m.y));
        const AutoPoint& p = ac->points[static_cast<size_t>(k)];
        if (pos == p.pos && val == p.value) break;
        editAuto(cg.autoId, [&](AutomationClip& q) {
          q.points[static_cast<size_t>(k)].pos = pos;
          q.points[static_cast<size_t>(k)].value = val;
        });
        break;
      }
      case ClipGesture::AutoTension: {
        const AutomationClip* ac = s.automation(cg.autoId);
        int k = cg.point;
        if (!ac || k < 1 || k >= static_cast<int>(ac->points.size())) break;
        float rise = ac->points[static_cast<size_t>(k)].value >= ac->points[static_cast<size_t>(k) - 1].value ? 1.0f : -1.0f;
        float t = std::clamp(cg.baseTension + (m.y - cg.start.y) / 120.0f * rise, -1.0f, 1.0f);
        if (t == ac->points[static_cast<size_t>(k)].tension) break;
        editAuto(cg.autoId, [&](AutomationClip& q) { q.points[static_cast<size_t>(k)].tension = t; });
        break;
      }
      case ClipGesture::Select: {
        ImVec2 lo(std::min(cg.start.x, m.x), std::min(cg.start.y, m.y)), hi(std::max(cg.start.x, m.x), std::max(cg.start.y, m.y));
        std::set<ObjectId> sel;
        for (const auto& c : s.playlist) {
          float x0 = xOf(c.start), x1 = xOf(c.start + s.clipLength(c)), y = yOf(c.track);
          if (x1 >= lo.x && x0 <= hi.x && y + hOf(c.track) >= lo.y && y <= hi.y) sel.insert(c.id);
        }
        st.selected = sel;
        break;
      }
      case ClipGesture::Paint: {
        const Pattern* p = s.pattern(a.curPattern);
        if (!p) break;
        Tick len = s.patternContentLength(*p);
        Tick t = std::max<Tick>(0, snapDown(curTick, std::max(snapG, len > 0 ? std::min(len, s.barTicks()) : snapG)));
        bool occupied = false;
        for (const auto& c : s.playlist)
          occupied = occupied || (c.track == curTrack && t < c.start + s.clipLength(c) && t + len > c.start);
        if (!occupied) a.doc.addClip(p->id, curTrack, t);
        break;
      }
      case ClipGesture::Erase:
        if (hoverClip != kNoId && !trackLocked(s.clip(hoverClip)->track)) a.doc.removeClip(hoverClip);
        break;
      case ClipGesture::Mute: {  // every touched clip gets the first clip's new state
        if (hoverClip == kNoId || trackLocked(s.clip(hoverClip)->track)) break;
        const Clip* c = s.clip(hoverClip);
        if (cg.muteTo < 0) cg.muteTo = c->muted ? 0 : 1;
        if (c->muted == (cg.muteTo == 1)) break;
        const bool to = cg.muteTo == 1;
        a.doc.editClip(hoverClip, to ? "mute clips" : "unmute clips", [to](Clip& k) { k.muted = to; }, cg.key);
        break;
      }
      case ClipGesture::Slip: {  // content moves inside the clip: later content = smaller offset
        const Clip* an = baseClip();
        if (!an) break;
        Tick d = timeline::snapTick(curTick, snapG, Snap::Line) - timeline::snapTick(cg.startTick, snapG, Snap::Line);
        Tick off = std::max<Tick>(0, cg.slipBase - d);
        if (const Clip* live = s.clip(an->id); live && live->offset != off) {
          a.doc.editClip(an->id, "slip clip content", [off](Clip& k) { k.offset = off; }, cg.key);
          a.setHint("Content offset " + timeline::rulerLabel(s, off, std::max<Tick>(1, s.ppq / 4)));
        }
        break;
      }
      case ClipGesture::Play: {  // dragging moves the playback start
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
          Tick t = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
          if (t != cg.lastScrub) a.engine.setPosition(t), cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::Zoom: break;  // applied on release
      default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (cg.kind == ClipGesture::Play) a.engine.pause();  // the play tool plays while held
    if (cg.kind == ClipGesture::Zoom) {
      if (std::fabs(m.x - cg.start.x) < 4) {
        st.zoomRequest *= 1.5f;
      } else {
        Tick t0 = std::max<Tick>(0, tickAt(std::min(cg.start.x, m.x))), t1 = tickAt(std::max(cg.start.x, m.x));
        if (t1 > t0) {
          st.pxPerTick = static_cast<float>(timeline::clampZoom(s, gridW / static_cast<double>(t1 - t0)));
          st.scrollX = static_cast<float>(t0) * st.pxPerTick;
        }
      }
    }
    if (cg.kind == ClipGesture::Move && !cg.moved && cg.anchor != kNoId && !io.KeyShift) st.selected = {cg.anchor};
    if (cg.kind == ClipGesture::TimeSel) {
      if (st.selEnd > st.selStart) {
        a.engine.setMode(PlayMode::Song);
        a.engine.setPosition(st.selStart);  // playback starts at the selection, like FL
      } else {
        st.selStart = st.selEnd = 0;
      }
    }
    if (cg.grouped) a.doc.endGroup();
    a.doc.endGesture();
    cg = ClipGesture{};
  }

  // Drop a pattern from the picker.
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PATTERN")) {
      ObjectId pid = *static_cast<const ObjectId*>(pl->Data);
      ObjectId id = a.doc.addClip(pid, trackAt(m.y), std::max<Tick>(0, snapDown(tickAt(m.x), snapG)));
      if (id) st.selected = {id};
    }
    // Browser files (J-16): samples become audio clips at the drop point.
    browserui::DropPlace place;
    place.track = trackAt(m.y);
    place.tick = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
    browserui::acceptFileDrop(a, browse::DropTarget::Playlist, place);
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PLUGIN")) {  // instruments: a new channel
      std::string type(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (const PluginInfo* info = findPlugin(type); info && info->kind == PluginKind::Generator) a.settingsChannel = a.addInstrument(type);
    }
    ImGui::EndDragDropTarget();
  }

  // Track popups.
  if (ImGui::BeginPopup("##trname")) {
    static char buf[128];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof buf, "%s", renaming >= 0 && renaming < nTracks ? s.tracks[static_cast<size_t>(renaming)].name.c_str() : "");
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputText("##n", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::string n = buf;
      a.doc.editPlaylistTrack(renaming, "rename playlist track", [&](PlaylistTrack& t) { t.name = n; });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trmenu")) {
    playlistTrackMenu(a, renaming);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##clipmenu")) {
    arrangementClipMenu(a, menuClip);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##markmenu")) {
    markerMenu(a, menuMarker);
    ImGui::EndPopup();
  }

  // ---------------- shortcuts ----------------
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && !st.selected.empty()) {
      a.doc.beginGroup("delete clips");
      for (ObjectId id : std::set<ObjectId>(st.selected))
        if (const Clip* c = s.clip(id); c && !trackLocked(c->track)) a.doc.removeClip(id);
      a.doc.endGroup();
      st.selected.clear();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
      st.selected.clear();
      for (const auto& c : s.playlist) st.selected.insert(c.id);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) st.selected.clear();
    // Copies keep every clip setting (kind, source, offsets, audio settings).
    auto duplicate = [&](const std::vector<Clip>& src, const char* label) {
      if (src.empty()) return;
      Tick lo = src[0].start, hi = 0;
      for (const auto& c : src) lo = std::min(lo, c.start), hi = std::max(hi, c.start + s.clipLength(c));
      Tick shift = snapNearest(hi - lo, std::max<Tick>(s.barTicks(), 1));
      if (shift <= 0) shift = s.barTicks();
      std::set<ObjectId> added;
      a.doc.editSong(label, [&](Song& x) {
        for (Clip c : src) {
          if (!clipValid(x, c)) continue;
          c.id = x.allocateId();
          c.start += shift;
          x.playlist.push_back(c);
          added.insert(c.id);
        }
      });
      st.selected = added;
    };
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !st.selected.empty()) {
      st.clipboard.clear();
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
      a.notify(std::to_string(st.clipboard.size()) + " clips copied");
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !st.clipboard.empty()) {
      duplicate(st.clipboard, "paste clips");
      st.clipboard.clear();
      for (const auto& c : a.doc.song().playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B) && !st.selected.empty()) {
      std::vector<Clip> src;
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) src.push_back(c);
      duplicate(src, "duplicate clips");
    }
    if (!io.KeyCtrl && !io.KeyAlt && !a.typingKeyboard) {  // tool keys
      struct K { ImGuiKey key; Tool tool; };
      const K keys[] = {{ImGuiKey_P, Tool::Draw}, {ImGuiKey_B, Tool::Paint}, {ImGuiKey_D, Tool::Erase}, {ImGuiKey_T, Tool::Mute},
                        {ImGuiKey_S, Tool::Slip},  {ImGuiKey_C, Tool::Slice}, {ImGuiKey_E, Tool::Select}, {ImGuiKey_Z, Tool::Zoom},
                        {ImGuiKey_Y, Tool::Play}};
      for (const auto& k : keys)
        if (ImGui::IsKeyPressed(k.key, false)) st.tool = k.tool;
    }
    if (io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T, false)) {  // marker at the song position
      ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
    }
    // M: mute / unmute the selected clips.
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M) && !st.selected.empty()) {
      auto sel = st.selected;
      a.doc.editClips("mute clips", [&](std::vector<Clip>& cs) {
        for (auto& c : cs)
          if (sel.count(c.id)) c.muted = !c.muted;
      });
    }
  }
  ui::EndDockWindow();
  drawClipProperties(a);  // J-8: audio clip properties window
}

}  // namespace gl
