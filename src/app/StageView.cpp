// Gridline - the Stage (see StageView.h): the stage, its tokens and groups, the gestures and the toolbar.
// The control panel, Arrange and the popovers are in StagePanel.cpp.
#include "StageView.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/MixerOps.h"
#include "../core/StageArrange.h"
#include "../core/StageMap.h"
#include "../core/StageRoomOps.h"
#include "../core/StageSelect.h"
#include "../core/StageRooms.h"
#include "App.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "CursorAnchor.h"
#include "Keys.h"
#include "StageViewInternal.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::stageui {

using theme::pal;
using namespace detail;

namespace detail {

namespace {
View gView;
}
View& view() { return gView; }


std::vector<ObjectId> selection(App& a) { return mixerSelection(a); }

bool isSelected(App& a, ObjectId id) { return id == a.selMixer || a.mixerView.sel.count(id) > 0; }

void selectOnly(App& a, ObjectId id) {
  a.mixerView.sel = {id};
  a.selMixer = id;
  a.editFocus = App::EditFocus::Mixer;
}

const Tok* tokOf(ObjectId id) {
  for (const Tok& t : gView.toks)
    if (t.id == id) return &t;
  return nullptr;
}

std::vector<size_t> shownTracks(App& a) { return shownTracksOf(a, shownRoom(a)); }

std::vector<size_t> shownTracksOf(App& a, int room) {
  const Song& s = a.doc.song();
  std::vector<size_t> out;
  const bool all = a.settings.getNumber("ui.stage.tracks") >= 0.5;
  std::vector<bool> used(s.mixer.size(), all);
  if (!all)  // only tracks in use (core/StageRoomOps.h: a channel, effects, an input, a name, routing or a place)
    for (size_t i = 1; i < s.mixer.size(); ++i) used[i] = roomops::trackInUse(s, s.mixer[i]);
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (used[i] && !roomops::isSpaceTrack(s, s.mixer[i].id) && roomops::trackRoom(s, s.mixer[i]) == room) out.push_back(i);
  return out;
}

int shownRoom(App& a) {
  const Song& s = a.doc.song();
  const int i = roomops::roomIndex(s, gView.room);
  if (i < 0) gView.room = kNoId;  // that room is gone (deleted here or by a collaborator): the first
  const int r = std::max(0, i);
  // New tracks stand in the room shown (session state, set each frame: undo and collaborators replace the song).
  const ObjectId want = roomops::roomCount(s) > 1 ? roomops::roomIdAt(s, r) : kNoId;
  if (s.newTrackRoom != want) a.doc.setNewTrackRoom(want);
  return r;
}

const StageSettings& shownStage(App& a) { return roomops::roomAt(a.doc.song(), shownRoom(a)); }

void showRoom(App& a, int room) {
  const Song& s = a.doc.song();
  room = std::clamp(room, 0, roomops::roomCount(s) - 1);
  if (room == shownRoom(a)) return;
  gView.room = roomops::roomIdAt(s, room);
  a.mixerView.sel.clear();  // the choice was on the other room's stage
  a.selMixer = s.masterId();
  gView.preview = Preview{};
  shownRoom(a);
}

bool moveRoomKeepShown(App& a, int from, int to) {
  int cur = shownRoom(a);
  if (!roomops::moveRoom(a.doc, from, to)) return false;
  if (cur == from) cur = to;  // where the shown room is now
  else if (from < cur && cur <= to) --cur;
  else if (to <= cur && cur < from) ++cur;
  gView.room = roomops::roomIdAt(a.doc.song(), cur);  // by its id: the first room changes with a move
  shownRoom(a);
  return true;
}

void dashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thick, float dash, float gap) {
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float len = std::hypot(q.x - p.x, q.y - p.y);
    if (len <= 0) return;
    const ImVec2 u((q.x - p.x) / len, (q.y - p.y) / len);
    for (float t = 0; t < len; t += dash + gap) {
      const float e = std::min(len, t + dash);
      dl->AddLine(ImVec2(p.x + u.x * t, p.y + u.y * t), ImVec2(p.x + u.x * e, p.y + u.y * e), col, thick);
    }
  };
  const float r = std::min(4.0f, (mx.x - mn.x) * 0.2f);
  line(ImVec2(mn.x + r, mn.y), ImVec2(mx.x - r, mn.y));
  line(ImVec2(mx.x, mn.y + r), ImVec2(mx.x, mx.y - r));
  line(ImVec2(mx.x - r, mx.y), ImVec2(mn.x + r, mx.y));
  line(ImVec2(mn.x, mx.y - r), ImVec2(mn.x, mn.y + r));
}

ImU32 faceOf(const MixerTrack& t, double depth, ImU32 stageCol) {
  const ImU32 slate = IM_COL32(0x2A, 0x31, 0x37, 0xFF);
  const ImU32 f = theme::mix(theme::content(t.color), slate, 0.30f);
  return theme::mix(f, stageCol, 0.22f * static_cast<float>(std::clamp(depth, 0.0, 1.0)));
}

void drawFace(App& a, ImDrawList* dl, const Tok& t, ImVec2 mn, float side, float alpha) {
  const auto& P = pal();
  const View& v = gView;
  const MixerTrack& m = a.doc.song().mixer[t.index];
  const ImVec2 mx(mn.x + side, mn.y + side), c(mn.x + side * 0.5f, mn.y + side * 0.5f);
  const float rad = side * 0.25f;
  const float back = static_cast<float>(std::clamp(t.depth, 0.0, 1.0));
  if (t.muted) {
    dl->AddRect(mn, mx, theme::withAlpha(P.textFaint, alpha), rad, 0, 1.5f);
  } else {
    ImU32 f = faceOf(m, t.depth, v.stage);
    if (t.dim) f = theme::mix(f, v.stage, 0.62f);
    dl->AddRectFilled(mn, mx, theme::withAlpha(f, alpha), rad);
  }
  float ia = (1.0f - 0.25f * back) * alpha;
  if (t.dim) ia *= 0.4f;
  const ImU32 ink = t.muted ? theme::withAlpha(P.textFaint, alpha) : IM_COL32(255, 255, 255, static_cast<int>(255.0f * std::clamp(ia, 0.0f, 1.0f)));
  if (t.icon > 0) {
    trackicons::draw(dl, t.icon, c, std::round(side * 24.0f / 52.0f), ink);
  } else {
    const std::string ini = st::initials(t.name);
    const float fs = std::round(side * 0.34f);
    const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, ini.c_str());
    dl->AddText(a.fontBold, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), ink, ini.c_str());
  }
}

}  // namespace detail

namespace {

using Clock = std::chrono::steady_clock;
double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

constexpr float kHandleR = 5.0f;
constexpr double kEase = 0.15;  // seconds a nudged drop eases into its place

struct Drag {
  enum Mode { None, Press, Move, Width, Band } mode = None;
  ObjectId anchor = kNoId;
  ImVec2 press{0, 0}, grab{0, 0};  // the pointer at the press; the pointer minus the token's centre
  ImVec2 virt{0, 0};               // the pointer's travel (slower with Shift)
  bool moved = false;
  std::vector<ObjectId> ids;       // the tracks it moves
  std::vector<double> pan0, depth0, sep0;
  double aPan0 = 0, aDepth0 = 0, aSep0 = 0;
  st::Rect from;                   // the anchor's token at the press ("was here")
  bool fromTray = false;
  // While moving: the anchor's place (before the drop's rule), the join target, outside the stage.
  double pan = 0, depth = 0;
  int target = -1;
  bool outside = false;
  std::vector<st::Obstacle> obs;
  std::vector<std::string> obsName;  // a token's track name, "" for a tray
  std::vector<int> obsCount;         // tracks behind the obstacle
  std::vector<st::Rect> obsRing;     // a tray's ring (to light up)
  bool toggleOnRelease = false;
  ObjectId widthTrack = kNoId;
  // K-224: a box on the empty stage (Band): the choice before it, how it combines, the tracks it holds now.
  std::vector<ObjectId> bandBefore;
  stagesel::BandMode bandMode = stagesel::BandMode::Replace;
  bool bandLive = false;  // the box is big enough to choose with
  // K-224: the room in the rooms list under the pointer while moving (-1: none): a drop moves the tracks there.
  int dropRoom = -1;
};

struct Ease {
  ImVec2 off{0, 0};
  double t0 = 0;
};

struct Local {
  Drag drag;
  std::map<ObjectId, Ease> ease;
  ObjectId menuTrack = kNoId;
  int menuGroup = -1;   // the "+N" list
  uint64_t keySerial = 0;
  bool focused = false;
  int hoverHandle = 0;  // 1 left, 2 right
  int hoverGroup = -1;  // the tray under the pointer
  bool openPresets = false, openRoom = false;  // dev: open a toolbar popover
  int escFrame = -1;  // K-224: Esc ended a drag or a box this frame (it does not also clear the selection)
  double roomHintUntil = 0;  // "Pick a room to hear depth" under the Room button until then
};
Local L;

const Song& song(App& a) { return a.doc.song(); }

double liveOr(App& a, ParamId pid, double stored, bool& live) {
  double v = 0;
  if (a.engine.playing() && a.builder.modulatedValue(pid, v)) {
    live = live || std::fabs(v - stored) > 1e-6;
    return v;
  }
  return stored;
}

bool locked(App& a, ObjectId id) {
  const MixerTrack* t = song(a).mixerTrack(id);
  return !t || t->locked;
}

bool dragging(ObjectId id) {
  const Drag& d = L.drag;
  return (d.mode == Drag::Move) && std::find(d.ids.begin(), d.ids.end(), id) != d.ids.end();
}

void setIfChanged(App& a, ParamId pid, double v) {
  const auto cur = getParam(a.doc.song(), pid);
  if (cur && std::fabs(*cur - v) > 1e-9) a.doc.setParam(pid, v, true);
}

std::vector<ObjectId> scopeOf(App& a, ObjectId id) {
  std::vector<ObjectId> out;
  if (isSelected(a, id))
    for (ObjectId t : selection(a))
      if (tokOf(t)) out.push_back(t);
  if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  return out;
}

// K-224: the choice is exactly `ids` (the box while it is dragged): the tokens show it at once.
void setChoice(App& a, const std::vector<ObjectId>& ids) {
  a.mixerView.sel = std::set<ObjectId>(ids.begin(), ids.end());
  if (ids.empty()) a.selMixer = song(a).masterId();
  else if (!a.mixerView.sel.count(a.selMixer)) a.selMixer = ids.front();
  for (Tok& t : gView.toks) t.selected = a.mixerView.sel.count(t.id) > 0;
}

void clickSelect(App& a, ObjectId id) {
  const ImGuiIO& io = ImGui::GetIO();
  MixerViewState& v = a.mixerView;
  if (io.KeyCtrl || io.KeySuper) {
    if (v.sel.empty() && a.selMixer != kNoId) v.sel.insert(a.selMixer);
    if (!v.sel.erase(id)) v.sel.insert(id);
    a.selMixer = id;
  } else if (io.KeyShift && a.selMixer != kNoId) {
    const Song& s = song(a);
    const int i0 = s.mixerIndex(a.selMixer), i1 = s.mixerIndex(id);
    if (i0 >= 0 && i1 >= 0)
      for (const Tok& t : gView.toks)
        if (static_cast<int>(t.index) >= std::min(i0, i1) && static_cast<int>(t.index) <= std::max(i0, i1)) v.sel.insert(t.id);
    v.sel.insert(a.selMixer);
    a.selMixer = id;
  } else if (!isSelected(a, id)) {
    selectOnly(a, id);
  } else {
    a.selMixer = id;
  }
  a.editFocus = App::EditFocus::Mixer;
}

// ---------------------------------------------------------------- the frame's model
void buildToks(App& a, const std::vector<size_t>& shown) {
  View& v = gView;
  const Song& s = song(a);
  bool anySolo = false;
  for (size_t i = 1; i < s.mixer.size(); ++i) anySolo = anySolo || s.mixer[i].solo;
  std::map<std::string, int> names;
  v.toks.clear();
  for (size_t i : shown) {
    const MixerTrack& m = s.mixer[i];
    Tok t;
    t.id = m.id;
    t.index = i;
    t.sPan = m.pan, t.sDepth = m.depth;
    t.pan = liveOr(a, mixerParam(m.id, MixerParam::Pan), m.pan, t.live);
    t.depth = liveOr(a, mixerParam(m.id, MixerParam::Depth), m.depth, t.live);
    t.sep = liveOr(a, mixerParam(m.id, MixerParam::StereoSep), m.stereoSep, t.live);
    t.focus = liveOr(a, mixerParam(m.id, MixerParam::Focus), m.focus, t.live);
    t.volume = liveOr(a, mixerParam(m.id, MixerParam::Volume), m.volume, t.live);
    if (dragging(m.id)) {  // a dragged track keeps its place in the groups until the drop
      const Drag& d = L.drag;
      const size_t k = static_cast<size_t>(std::find(d.ids.begin(), d.ids.end(), m.id) - d.ids.begin());
      t.pan = d.pan0[k], t.depth = d.depth0[k];
    }
    t.icon = m.icon > 0 ? m.icon : autoIcon(a, i);
    t.name = mixerTrackName(s, i);
    ++names[t.name];
    t.muted = m.muted;
    t.solo = m.solo;
    t.dim = anySolo && !m.solo;
    t.selected = isSelected(a, m.id);
    v.toks.push_back(std::move(t));
  }
}

float stageMinX(const st::Geo& g, float y) { return g.cx - g.halfAt(std::min(y, g.frontY)); }
float stageMaxX(const st::Geo& g, float y) { return g.cx + g.halfAt(std::min(y, g.frontY)); }

st::Pt ringOf(const st::Geo& g, double pan, double depth) {
  const st::Pt p = g.at(pan, depth);
  return {p.x, p.y + g.side(depth) * 0.5f};
}

// A tray keeps clear of the tracks standing alone near it (their tokens and name labels): of the
// places that still point at its spot (above or below it, slid sideways while the stem reaches the
// spot), the one covering the least of them, the nearest when several are clear.
void clearTray(st::Tray& tr, st::Pt ring, const std::vector<size_t>& members, float minX, float maxX, float minY, float maxY) {
  const View& v = gView;
  const st::Geo& g = v.g;
  const float s = g.scale;
  std::vector<st::Rect> obs;
  for (size_t i = 0; i < v.toks.size(); ++i) {
    if (std::find(members.begin(), members.end(), i) != members.end()) continue;
    const st::Rect r = g.token(v.toks[i].pan, v.toks[i].depth);
    const float lw = std::max(r.w(), 70.0f * s);  // the name under it
    obs.push_back({std::min(r.x0, (r.x0 + r.x1 - lw) * 0.5f), r.y0, std::max(r.x1, (r.x0 + r.x1 + lw) * 0.5f), r.y1 + 22.0f * s});
  }
  if (obs.empty()) return;
  const float w = tr.box.w(), h = tr.box.h();
  auto cost = [&](float x0, float y0) {
    const st::Rect b{x0, y0, x0 + w, y0 + h};
    float c = 0;
    for (const st::Rect& o : obs) c += st::overlapArea(b, o);
    return c;
  };
  const float bestY0 = tr.box.y0;
  float bx = tr.box.x0, by = tr.box.y0, best = cost(bx, by), dist = 0;
  if (best <= 0) return;
  std::vector<float> ys{tr.box.y0};
  const float above = ring.y - 22.0f * s - h, below = ring.y + 22.0f * s;
  if (above >= minY) ys.push_back(std::round(above));
  if (below + h <= maxY) ys.push_back(std::round(below));
  // Sideways: the stem stays inside the tray (20 px from its ends), the tray inside the stage.
  const float lo = std::max(minX, ring.x - w + 20.0f * s), hi = std::min(maxX - w, ring.x - 20.0f * s);
  std::vector<float> xs{tr.box.x0};
  if (lo <= hi) {
    xs.push_back(lo), xs.push_back(hi);
    for (const st::Rect& o : obs) xs.push_back(o.x0 - w - 2), xs.push_back(o.x1 + 2);
  }
  for (float y : ys)
    for (float x : xs) {
      if (lo <= hi) x = std::clamp(x, lo, hi);
      x = std::round(x);
      const float c = cost(x, y), d = std::fabs(x - tr.box.x0) + std::fabs(y - bestY0);
      if (c < best - 1.0f || (std::fabs(c - best) <= 1.0f && d < dist)) best = c, dist = d, bx = x, by = y;
    }
  const float dx = bx - tr.box.x0, dy = by - tr.box.y0;
  if (dx == 0 && dy == 0) return;
  tr.box = {tr.box.x0 + dx, tr.box.y0 + dy, tr.box.x1 + dx, tr.box.y1 + dy};
  for (st::Rect& c : tr.cells) c = {c.x0 + dx, c.y0 + dy, c.x1 + dx, c.y1 + dy};
  tr.above = tr.box.y1 <= ring.y;
}

// The listener on screen (the head, its nose and the cups), for keeping trays off it.
st::Rect listenerRect(const st::Geo& g) {
  const float s = g.scale, r = 15.0f * s;
  return {g.cx - r - 9.0f * s, g.listenerY - r - 7.0f * s, g.cx + r + 9.0f * s, g.listenerY + r + 1.0f * s};
}

// K-270: a tray that hangs over the floor's front edge or onto the listener (a short stage, where the
// room above a front-row group is taken by the next row): the stage is laid out a little smaller.
bool traysCrowdListener() {
  const View& v = gView;
  const st::Geo& g = v.g;
  const st::Rect l = listenerRect(g).grown(4.0f * g.scale);
  for (const Grp& gr : v.groups)
    if (gr.tray.box.y1 > g.frontY + st::kBulge * g.scale * 0.5f || st::overlapArea(gr.tray.box, l) > 0) return true;
  return false;
}

// The lowest a tray may reach: the panel's top, or on a stage too short even laid out smaller, the
// floor's front edge (so it never hangs over the listener).
float trayMaxY() {
  const View& v = gView;
  return v.trayOnFloor ? std::min(v.panel0.y - 4, v.g.frontY + st::kBulge * v.g.scale * 0.5f) : v.panel0.y - 4;
}

// K-270: a tray over another track's tile or its name (the stage too short for the tray to find a clear
// place): the stage is laid out smaller too.
bool traysCoverTiles() {
  const View& v = gView;
  const st::Geo& g = v.g;
  const float s = g.scale;
  for (size_t gi = 0; gi < v.groups.size(); ++gi) {
    const st::Rect& box = v.groups[gi].tray.box;
    for (const Tok& t : v.toks) {
      if (t.group == static_cast<int>(gi) || dragging(t.id)) continue;
      const st::Rect r = g.token(t.pan, t.depth);
      const float lw = std::max(r.w(), 70.0f * s);  // the name under it
      const st::Rect tile{std::min(r.x0, (r.x0 + r.x1 - lw) * 0.5f), r.y0, std::max(r.x1, (r.x0 + r.x1 + lw) * 0.5f), r.y1 + 18.0f * s};
      if (st::overlapArea(box, tile) > 0) return true;
    }
  }
  return false;
}

void buildGroups(App& a) {
  (void)a;
  View& v = gView;
  std::vector<st::Pos> pts;
  for (const Tok& t : v.toks) pts.push_back({t.id, t.pan, t.depth});
  const auto gs = st::groups(pts, &v.prevGroup);
  v.groups.clear();
  v.prevGroup.clear();
  for (const auto& members : gs) {
    Grp gr;
    gr.toks = members;
    gr.spot = st::spotOf(pts, members);
    gr.ring = ringOf(v.g, gr.spot.pan, gr.spot.depth);
    const st::Geo& g = v.g;
    gr.tray = st::trayLayout(g, gr.ring, static_cast<int>(members.size()), stageMinX(g, g.backY) + 4, stageMaxX(g, g.backY) - 4, v.body0.y + 4,
                             trayMaxY());
    clearTray(gr.tray, gr.ring, members, stageMinX(g, g.backY) + 4, stageMaxX(g, g.backY) - 4, v.body0.y + 4, trayMaxY());
    const int gi = static_cast<int>(v.groups.size());
    for (size_t k = 0; k < members.size(); ++k) {
      Tok& t = v.toks[members[k]];
      t.group = gi;
      t.cell = static_cast<int>(k) < gr.tray.shown ? static_cast<int>(k) : -1;
      if (t.cell >= 0) {
        const st::Rect& c = gr.tray.cells[static_cast<size_t>(t.cell)];
        const float ts = gr.tray.token, cx = (c.x0 + c.x1) * 0.5f, y0 = c.y0 + 6.0f * g.scale;
        t.r = {std::round(cx - ts * 0.5f), std::round(y0), std::round(cx - ts * 0.5f) + ts, std::round(y0) + ts};
      }
      v.prevGroup[t.id] = v.toks[members.front()].id;
    }
    v.groups.push_back(std::move(gr));
  }
  // Tracks on their own: their tokens, kept apart on screen when their places are close.
  std::vector<size_t> singles;
  std::vector<st::Rect> rects;
  for (size_t i = 0; i < v.toks.size(); ++i)
    if (v.toks[i].group < 0) singles.push_back(i), rects.push_back(v.g.token(v.toks[i].pan, v.toks[i].depth));
  const std::vector<float> off = st::separate(rects, st::kGap * v.g.scale);
  const double t = now();
  for (size_t k = 0; k < singles.size(); ++k) {
    Tok& tk = v.toks[singles[k]];
    st::Rect r = rects[k];
    float dx = off[k], dy = 0;
    if (auto e = L.ease.find(tk.id); e != L.ease.end()) {
      const double f = std::clamp((t - e->second.t0) / kEase, 0.0, 1.0);
      const float k2 = static_cast<float>((1.0 - f) * (1.0 - f));
      dx += e->second.off.x * k2, dy += e->second.off.y * k2;
      if (f >= 1.0) L.ease.erase(e);
    }
    tk.r = {std::round(r.x0 + dx), std::round(r.y0 + dy), std::round(r.x1 + dx), std::round(r.y1 + dy)};
  }
}

// ---------------------------------------------------------------- drawing
void drawFloor(App& a, ImDrawList* dl) {
  const auto& P = pal();
  const View& v = gView;
  const st::Geo& g = v.g;
  const float s = g.scale, bulge = st::kBulge * s;
  const ImVec2 tl(g.cx - g.backHalf, g.backY), trr(g.cx + g.backHalf, g.backY), br(g.cx + g.frontHalf, g.frontY), bl(g.cx - g.frontHalf, g.frontY);
  const ImVec2 ctrl(g.cx, g.frontY + 2.0f * bulge);
  const bool open = !shownStage(a).hasRoom();  // No space: a faint open floor, no edge
  dl->PathLineTo(tl);
  dl->PathLineTo(trr);
  dl->PathLineTo(br);
  dl->PathBezierQuadraticCurveTo(ctrl, bl, 32);
  dl->PathFillConvex(open ? theme::mix(v.stage, v.bg, 0.55f) : v.stage);
  if (!open) {
    dl->PathLineTo(br);
    dl->PathBezierQuadraticCurveTo(ctrl, bl, 32);
    dl->PathStroke(P.lineStrong, 0, 1.5f);
  }
  dl->AddLine(ImVec2(g.cx + 0.5f, g.backY + 4 * s), ImVec2(g.cx + 0.5f, g.frontY + bulge), theme::withAlpha(P.text, 0.07f), 1.0f);
  if (v.roomOpen) {  // the room popover is open: the room's outline around the stage
    const float o = 10.0f * s;
    dl->PathLineTo(ImVec2(tl.x - o * 0.6f, tl.y - o));
    dl->PathLineTo(ImVec2(trr.x + o * 0.6f, trr.y - o));
    dl->PathLineTo(ImVec2(br.x + o, br.y + o));
    dl->PathBezierQuadraticCurveTo(ImVec2(ctrl.x, ctrl.y + o), ImVec2(bl.x - o, bl.y + o), 32);
    dl->PathStroke(P.hot, ImDrawFlags_Closed, 1.5f);
  }
}

void drawListener(App& a, ImDrawList* dl) {
  const auto& P = pal();
  const st::Geo& g = gView.g;
  const float s = g.scale;
  const ImVec2 c(g.cx, g.listenerY);
  const float r = 15.0f * s;
  const ImU32 cup = theme::mix(P.text, gView.bg, 0.12f);
  // The headset seen from above: the head with the nose toward the stage, a cup on each side.
  dl->AddTriangleFilled(ImVec2(c.x, c.y - r - 6 * s), ImVec2(c.x - 6 * s, c.y - r + 3 * s), ImVec2(c.x + 6 * s, c.y - r + 3 * s), P.accent);
  dl->AddCircleFilled(c, r, P.accent, 32);
  for (float sx : {-1.0f, 1.0f}) {
    const ImVec2 mn(c.x + sx * (r + 1 * s) - (sx < 0 ? 7 * s : 0), c.y - 8 * s);
    dl->AddRectFilled(mn, ImVec2(mn.x + 7 * s, mn.y + 16 * s), cup, 3 * s);
  }
  (void)a;
}


// Where a track's footprint (and width handles) is: under its token, or at its group's spot.
struct Foot {
  float cx = 0, y = 0, rx = 0;
  double depth = 0;
};
Foot footOf(const Tok& t) {
  const View& v = gView;
  const st::Geo& g = v.g;
  Foot f;
  if (t.group >= 0) {
    const Grp& gr = v.groups[static_cast<size_t>(t.group)];
    f.cx = gr.ring.x, f.y = gr.ring.y, f.depth = gr.spot.depth;
  } else {
    f.cx = (t.r.x0 + t.r.x1) * 0.5f, f.y = t.r.y1, f.depth = t.depth;
  }
  const float half = g.halfAt(std::min(f.y, g.frontY));
  f.rx = std::min(g.footRx(f.depth, t.sep), std::max(g.side(f.depth) * 0.5f + 10 * g.scale, std::min(g.cx + half - f.cx, f.cx - (g.cx - half))));
  return f;
}

bool onlyOneSelected(App& a) { return selection(a).size() <= 1; }

// A flat tooltip beside a point (no delay: it follows a drag or a hover on the stage).
void floatTip(App& a, ImVec2 at, const std::string& l1, const std::string& l2) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float fs = theme::kFontCaption, pad = 10;
  const ImVec2 s1 = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, l1.c_str());
  const ImVec2 s2 = l2.empty() ? ImVec2(0, 0) : a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, l2.c_str());
  const float w = std::max(s1.x, s2.x) + pad * 2, h = s1.y + (l2.empty() ? 0 : s2.y + 3) + 14;
  ImVec2 mn(std::round(at.x), std::round(at.y));
  const ImVec2 vp = ImGui::GetMainViewport()->Pos, vs = ImGui::GetMainViewport()->Size;
  if (mn.x + w > vp.x + vs.x - 4) mn.x = std::round(vp.x + vs.x - 4 - w);
  if (mn.y + h > vp.y + vs.y - 4) mn.y = std::round(vp.y + vs.y - 4 - h);
  dl->AddRectFilled(mn, ImVec2(mn.x + w, mn.y + h), theme::popupBg(P), 6);
  dl->AddRect(mn, ImVec2(mn.x + w, mn.y + h), P.lineStrong, 6);
  dl->AddText(a.fontRegular, fs, ImVec2(mn.x + pad, mn.y + 7), P.text, l1.c_str());
  if (!l2.empty()) dl->AddText(a.fontRegular, fs, ImVec2(mn.x + pad, mn.y + 10 + s1.y), P.textDim, l2.c_str());
}

// Lower-case place words for the join tooltip: "centre, near", "40% left, far".
std::string placeWords(double pan, double depth) {
  static const char* kD[] = {i18n::N_("front"), i18n::N_("near"), i18n::N_("middle"), i18n::N_("far"), i18n::N_("back")};
  const int k = std::clamp(static_cast<int>(std::lround(depth * 4.0)), 0, 4);
  std::string p;
  const int pc = static_cast<int>(std::lround(std::fabs(pan) * 100.0));
  char b[48];
  if (pc == 0) p = i18n::tr("centre");
  else std::snprintf(b, sizeof b, pan < 0 ? i18n::tr("%d%% left") : i18n::tr("%d%% right"), pc), p = b;
  return p + ", " + i18n::tr(kD[k]);
}

void drawTile(App& a, ImDrawList* dl, const Tok& t, const st::Rect& r, bool hovered, bool focused) {
  const auto& P = pal();
  const MixerTrack& m = song(a).mixer[t.index];
  const float side = r.w();
  const ImVec2 mn(r.x0, r.y0), mx(r.x1, r.y1);
  drawFace(a, dl, t, mn, side, 1.0f);
  const float rad = side * 0.25f;
  if (hovered && !t.muted) dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 18), rad);
  if (t.selected) dl->AddRect(ImVec2(mn.x - 3, mn.y - 3), ImVec2(mx.x + 3, mx.y + 3), theme::withAlpha(P.hot, focused || !t.dim ? 1.0f : 0.8f), rad + 3, 0, 2.0f);
  if (t.solo) {
    const ImVec2 c(mx.x - 1, mn.y + 1);
    dl->AddCircleFilled(c, 7.5f, P.solo, 16);
    const float fs = 9.0f;
    const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, "S");
    dl->AddText(a.fontBold, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), theme::contrastText(P.solo), "S");
  }
  if (t.live) dl->AddCircleFilled(ImVec2(mn.x + 4, mn.y + 4), 2.5f, P.accent, 10);  // automation plays it
  if (m.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(mn.x + 7, mx.y - 7), 9, P.textDim);
  if (ImU32 fc = 0; const float fl = collabui::flash(a, "mixer/#" + std::to_string(m.id), &fc))  // a collaborator's change
    dl->AddRect(ImVec2(mn.x - 4, mn.y - 4), ImVec2(mx.x + 4, mx.y + 4), theme::withAlpha(fc, fl), rad + 4, 0, 2.0f);
  canchor::publishRect(canchor::StageTrack, m.id, 0, mn, mx);  // collaborators' pointers over the stage
}

// A label chip: the full name, boxed (hovered, selected or dragged).
void drawChip(App& a, ImDrawList* dl, const std::string& name, float cx, float y, float fs, float minX, float maxX) {
  const auto& P = pal();
  const View& v = gView;
  const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, name.c_str());
  const float w = ts.x + 12, h = ts.y + 4;
  float x = std::round(cx - w * 0.5f);
  x = std::clamp(x, minX, std::max(minX, maxX - w));
  const ImVec2 mn(x, std::round(y - 2)), mx(x + w, std::round(y - 2) + h);
  dl->AddRectFilled(mn, mx, v.bg, 3);
  dl->AddRect(mn, mx, P.lineStrong, 3);
  dl->AddText(a.fontBold, fs, ImVec2(mn.x + 6, mn.y + 2), P.text, name.c_str());
}

float labelSize(double depth) { return 12.0f - 0.5f * static_cast<float>(std::clamp(depth, 0.0, 1.0)); }

void drawSingles(App& a, ImDrawList* dl) {
  const auto& P = pal();
  View& v = gView;
  const st::Geo& g = v.g;
  const Drag& d = L.drag;
  const bool one = onlyOneSelected(a);
  std::vector<size_t> order;
  for (size_t i = 0; i < v.toks.size(); ++i)
    if (v.toks[i].group < 0) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return v.toks[x].depth > v.toks[y].depth; });  // far first
  for (size_t i : order) {
    const Tok& t = v.toks[i];
    if (dragging(t.id) && d.moved) continue;
    const Foot f = footOf(t);
    const ImU32 col = t.muted ? theme::withAlpha(P.textFaint, 0.07f)
                              : theme::withAlpha(theme::content(song(a).mixer[t.index].color), (P.dark ? 0.20f : 0.24f) * (t.dim ? 0.5f : 1.0f));
    dl->AddEllipseFilled(ImVec2(f.cx, f.y), ImVec2(f.rx, g.footRy(f.depth)), col);
  }
  for (size_t i : order) {
    const Tok& t = v.toks[i];
    if (dragging(t.id) && d.moved) {  // "was here"
      dl->AddRect(ImVec2(t.r.x0, t.r.y0), ImVec2(t.r.x1, t.r.y1), P.lineStrong, t.r.w() * 0.25f, 0, 1.5f);
      const char* w = i18n::tr("was here");
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, w);
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((t.r.x0 + t.r.x1 - ts.x) * 0.5f), t.r.y1 + 7), P.textFaint, w);
      continue;
    }
    drawTile(a, dl, t, t.r, v.hovered == t.id, t.id == a.selMixer);
  }
  // The selected track's width handles (one track selected).
  if (one && d.mode != Drag::Move)
    if (const Tok* t = tokOf(a.selMixer); t && t->group < 0) {
      const Foot f = footOf(*t);
      for (float sx : {-1.0f, 1.0f}) {
        const bool hot = L.hoverHandle == (sx < 0 ? 1 : 2) || (d.mode == Drag::Width);
        dl->AddCircleFilled(ImVec2(f.cx + sx * f.rx, f.y), kHandleR * g.scale + 2, v.stage, 16);
        dl->AddCircleFilled(ImVec2(f.cx + sx * f.rx, f.y), (kHandleR + (hot ? 1.0f : 0.0f)) * g.scale, P.hot, 16);
      }
    }
}

void drawLabels(App& a, ImDrawList* dl) {
  if (!a.settings.getBool("ui.stage.labels")) return;
  const auto& P = pal();
  View& v = gView;
  const st::Geo& g = v.g;
  const Drag& d = L.drag;
  std::vector<st::LabelIn> in;
  std::vector<size_t> who;
  std::map<std::string, int> count;
  for (const Tok& t : v.toks)
    if (t.group < 0) ++count[t.name];
  for (size_t i = 0; i < v.toks.size(); ++i) {
    const Tok& t = v.toks[i];
    if (t.group >= 0 || (dragging(t.id) && d.moved)) continue;
    st::LabelIn l;
    l.name = count[t.name] > 1 ? t.name + " " + std::to_string(t.index) : t.name;  // twins: the faint mixer number
    l.cx = (t.r.x0 + t.r.x1) * 0.5f;
    l.y = t.r.y1 + 7.0f * g.scale;
    l.token = t.r.w();
    l.minX = stageMinX(g, l.y) + 8, l.maxX = stageMaxX(g, l.y) - 8;
    l.chip = t.selected || v.hovered == t.id;
    in.push_back(l);
    who.push_back(i);
  }
  const float fs = 12.0f;
  auto measure = [&](const std::string& s) { return a.fontBold->CalcTextSizeA(fs, 1e9f, 0, s.c_str()).x; };
  const std::vector<st::LabelOut> out = st::layoutLabels(in, measure, g.scale);
  for (size_t k = 0; k < out.size(); ++k) {
    const Tok& t = v.toks[who[k]];
    const float lfs = labelSize(t.depth);
    if (in[k].chip) continue;  // drawn after, over the others
    if (!out[k].visible) continue;
    const ImU32 col = t.muted || (t.dim && !t.selected) ? P.textFaint : P.text;
    const std::string& text = out[k].text;
    const std::string suffix = " " + std::to_string(t.index);
    if (in[k].name != t.name && text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
      const std::string head = text.substr(0, text.size() - suffix.size());
      dl->AddText(a.fontBold, lfs, ImVec2(out[k].x, out[k].y), col, head.c_str());
      const float hw = a.fontBold->CalcTextSizeA(lfs, 1e9f, 0, head.c_str()).x;
      dl->AddText(a.fontRegular, lfs, ImVec2(out[k].x + hw, out[k].y), P.textFaint, suffix.c_str());
    } else {
      dl->AddText(a.fontBold, lfs, ImVec2(out[k].x, out[k].y), col, text.c_str());
    }
  }
  for (size_t k = 0; k < in.size(); ++k)
    if (in[k].chip) drawChip(a, dl, v.toks[who[k]].name, in[k].cx, in[k].y, labelSize(v.toks[who[k]].depth), in[k].minX - 8, in[k].maxX + 8);
}

void drawTrays(App& a, ImDrawList* dl) {
  const auto& P = pal();
  View& v = gView;
  const st::Geo& g = v.g;
  const Drag& d = L.drag;
  const float s = g.scale;
  const bool one = onlyOneSelected(a);
  std::vector<size_t> order(v.groups.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return v.groups[x].ring.y < v.groups[y].ring.y; });
  auto measure = [&](const std::string& t) { return a.fontBold->CalcTextSizeA(12.0f, 1e9f, 0, t.c_str()).x; };
  for (size_t gi : order) {
    const Grp& gr = v.groups[gi];
    const st::Tray& tr = gr.tray;
    // The footprint at the real spot: the selected member's, else the first one's.
    const Tok* shown = &v.toks[gr.toks.front()];
    for (size_t k : gr.toks)
      if (v.toks[k].selected) {
        shown = &v.toks[k];
        break;
      }
    {
      const Foot f = footOf(*shown);
      const ImU32 col = theme::withAlpha(theme::content(song(a).mixer[shown->index].color), P.dark ? 0.20f : 0.24f);
      dl->AddEllipseFilled(ImVec2(f.cx, f.y), ImVec2(f.rx, g.footRy(f.depth)), col);
      if (one && shown->id == a.selMixer && d.mode != Drag::Move)
        for (float sx : {-1.0f, 1.0f}) {
          dl->AddCircleFilled(ImVec2(f.cx + sx * f.rx, f.y), kHandleR * s + 2, v.stage, 16);
          dl->AddCircleFilled(ImVec2(f.cx + sx * f.rx, f.y), kHandleR * s, P.hot, 16);
        }
    }
    // The ring with its dot: the only thing at the true place.
    const ImVec2 rc(gr.ring.x, gr.ring.y);
    dl->AddEllipse(rc, ImVec2(16 * s, 5 * s), P.textDim, 0, 24, 1.5f);
    dl->AddCircleFilled(rc, 2.0f * s, P.textDim, 8);
    // The tray and its stem.
    const ImVec2 b0(tr.box.x0, tr.box.y0), b1(tr.box.x1, tr.box.y1);
    const float sx = std::clamp(gr.ring.x, b0.x + 20 * s, b1.x - 20 * s);
    const float rad = 14 * s;
    dl->AddRectFilled(b0, b1, v.tray, rad);
    dl->AddRect(b0, b1, P.lineStrong, rad);
    if (tr.above) {
      dl->AddTriangleFilled(ImVec2(sx - 7 * s, b1.y - 1), ImVec2(sx + 7 * s, b1.y - 1), ImVec2(sx, b1.y + 8 * s), v.tray);
      dl->AddLine(ImVec2(sx - 7 * s, b1.y), ImVec2(sx, b1.y + 8 * s), P.lineStrong);
      dl->AddLine(ImVec2(sx + 7 * s, b1.y), ImVec2(sx, b1.y + 8 * s), P.lineStrong);
    } else {
      dl->AddTriangleFilled(ImVec2(sx - 7 * s, b0.y + 1), ImVec2(sx + 7 * s, b0.y + 1), ImVec2(sx, b0.y - 8 * s), v.tray);
      dl->AddLine(ImVec2(sx - 7 * s, b0.y), ImVec2(sx, b0.y - 8 * s), P.lineStrong);
      dl->AddLine(ImVec2(sx + 7 * s, b0.y), ImVec2(sx, b0.y - 8 * s), P.lineStrong);
    }
    std::vector<std::string> names;
    for (size_t k : gr.toks) names.push_back(v.toks[k].name);
    const Tok* chip = nullptr;
    for (size_t k : gr.toks) {
      const Tok& t = v.toks[k];
      if (t.cell < 0) continue;
      const st::Rect& c = tr.cells[static_cast<size_t>(t.cell)];
      if (dragging(t.id) && d.moved) {  // lifted out: its cell stays until the drop
        dashedRect(dl, ImVec2(t.r.x0, t.r.y0), ImVec2(t.r.x1, t.r.y1), P.textFaint, 1.2f);
        continue;
      }
      drawTile(a, dl, t, t.r, v.hovered == t.id, t.id == a.selMixer);
      if (!a.settings.getBool("ui.stage.labels")) continue;
      if (t.selected || v.hovered == t.id) {
        if (!chip || v.hovered == t.id) chip = &t;
        continue;
      }
      std::vector<std::string> others;
      for (const std::string& n : names)
        if (n != t.name) others.push_back(n);
      const std::string lab = st::cutLabel(t.name, 86.0f * s, measure, others);
      const float lw = measure(lab);
      dl->AddText(a.fontBold, 12.0f, ImVec2(std::round((c.x0 + c.x1 - lw) * 0.5f), std::round(t.r.y1 + 4 * s)),
                  t.muted || (t.dim && !t.selected) ? P.textFaint : P.text, lab.c_str());
    }
    for (size_t k : gr.toks) {  // every selected member's full name (the hovered one last)
      const Tok& t = v.toks[k];
      if (t.cell < 0 || &t == chip || !(t.selected || v.hovered == t.id) || (dragging(t.id) && d.moved) || !a.settings.getBool("ui.stage.labels")) continue;
      drawChip(a, dl, t.name, (t.r.x0 + t.r.x1) * 0.5f, t.r.y1 + 4 * s, 12.0f, b0.x + 2, b1.x - 2);
    }
    if (chip) drawChip(a, dl, chip->name, (chip->r.x0 + chip->r.x1) * 0.5f, chip->r.y1 + 4 * s, 12.0f, b0.x + 2, b1.x - 2);
    if (tr.more > 0) {  // "+N": the rest in a list
      const st::Rect& c = tr.cells.back();
      const float ts = tr.token, cx = (c.x0 + c.x1) * 0.5f;
      const ImVec2 mn(std::round(cx - ts * 0.5f), std::round(c.y0 + 6 * s));
      dl->AddRectFilled(mn, ImVec2(mn.x + ts, mn.y + ts), L.menuGroup == static_cast<int>(gi) ? P.surface600 : P.surface700, ts * 0.25f);
      char b[16];
      std::snprintf(b, sizeof b, "+%d", tr.more);
      const ImVec2 tsz = a.fontBold->CalcTextSizeA(13.0f, 1e9f, 0, b);
      dl->AddText(a.fontBold, 13.0f, ImVec2(std::round(cx - tsz.x * 0.5f), std::round(mn.y + (ts - tsz.y) * 0.5f)), P.text, b);
    }
    // The count chip on the tray's corner.
    char b[16];
    std::snprintf(b, sizeof b, "%zu", gr.toks.size());
    const ImVec2 tsz = a.fontRegular->CalcTextSizeA(10.0f, 1e9f, 0, b);
    const float cw = std::max(16.0f, tsz.x + 9), cx = b1.x - 22 * s;
    dl->AddRectFilled(ImVec2(cx - cw * 0.5f, b0.y - 8), ImVec2(cx + cw * 0.5f, b0.y + 8), P.surface700, 8);
    dl->AddText(a.fontRegular, 10.0f, ImVec2(std::round(cx - tsz.x * 0.5f), std::round(b0.y - tsz.y * 0.5f)), P.textDim, b);
  }
}

// The drag on the stage: the target lit, the place it will take, the lifted tokens and the tooltip.
void drawDrag(App& a, ImDrawList* dl) {
  const auto& P = pal();
  View& v = gView;
  const st::Geo& g = v.g;
  const Drag& d = L.drag;
  if (d.mode != Drag::Move || !d.moved) return;
  const float s = g.scale;
  const ImVec2 m = ImGui::GetIO().MousePos;
  v.dropRoom = d.dropRoom;
  v.dragCount = static_cast<int>(d.ids.size());
  if (d.dropRoom >= 0) {  // K-224: over a room in the rooms list: the tracks ride along under the pointer
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const int n = static_cast<int>(std::min<size_t>(d.ids.size(), 4));
    const float side = 30.0f;
    for (int k = n; k-- > 0;) {
      const Tok* t = tokOf(d.ids[static_cast<size_t>(k)]);
      if (!t) continue;
      const ImVec2 mn(std::round(m.x + 14 + 5.0f * static_cast<float>(k)), std::round(m.y + 8 + 4.0f * static_cast<float>(k)));
      fg->AddRectFilled(ImVec2(mn.x + 2, mn.y + 4), ImVec2(mn.x + side + 2, mn.y + side + 4), IM_COL32(0, 0, 0, 60), side * 0.25f);
      Tok lifted = *t;
      lifted.dim = false;
      drawFace(a, fg, lifted, mn, side, 1.0f);
    }
    if (d.ids.size() > 1) {  // the count on the stack
      char b[16];
      std::snprintf(b, sizeof b, "%zu", d.ids.size());
      const ImVec2 tsz = a.fontBold->CalcTextSizeA(10.0f, 1e9f, 0, b);
      const float cw = std::max(16.0f, tsz.x + 9), cx = m.x + 14 + side + 5.0f * static_cast<float>(n - 1), cy = m.y + 8;
      fg->AddRectFilled(ImVec2(cx - cw * 0.5f, cy - 8), ImVec2(cx + cw * 0.5f, cy + 8), P.hot, 8);
      fg->AddText(a.fontBold, 10.0f, ImVec2(std::round(cx - tsz.x * 0.5f), std::round(cy - tsz.y * 0.5f)), P.surface950, b);
    }
    const Song& sg = song(a);
    char b[200];
    if (d.dropRoom == shownRoom(a)) {
      const float tipX = railRows().empty() ? m.x + 28 : std::max(m.x + 28, railRows().front().x1 + 10);
      floatTip(a, ImVec2(tipX, m.y + 8), i18n::tr("Already in this room"), i18n::tr("Drop on another room to move them there"));
    } else {
      if (d.ids.size() == 1) std::snprintf(b, sizeof b, i18n::tr("Move 1 track to %s"), roomops::roomName(sg, d.dropRoom).c_str());
      else std::snprintf(b, sizeof b, i18n::tr("Move %zu tracks to %s"), d.ids.size(), roomops::roomName(sg, d.dropRoom).c_str());
      const float tipX = railRows().empty() ? m.x + 28 : std::max(m.x + 28, railRows().front().x1 + 10);  // beside the list, not over the next room
      floatTip(a, ImVec2(tipX, m.y + 8), b, i18n::tr("They keep their places and take on its sound"));
    }
    return;
  }
  // The join target.
  if (d.target >= 0) {
    const st::Obstacle& o = d.obs[static_cast<size_t>(d.target)];
    if (d.obsName[static_cast<size_t>(d.target)].empty()) {  // a tray
      dl->AddRect(ImVec2(o.r.x0, o.r.y0), ImVec2(o.r.x1, o.r.y1), P.hot, 14 * s, 0, 1.5f);
      {  // its count chip lights up too
        char b[16];
        std::snprintf(b, sizeof b, "%d", d.obsCount[static_cast<size_t>(d.target)]);
        const ImVec2 tsz = a.fontBold->CalcTextSizeA(10.0f, 1e9f, 0, b);
        const float cw = std::max(16.0f, tsz.x + 9), cx = o.r.x1 - 22 * s;
        dl->AddRectFilled(ImVec2(cx - cw * 0.5f, o.r.y0 - 8), ImVec2(cx + cw * 0.5f, o.r.y0 + 8), P.hot, 8);
        dl->AddText(a.fontBold, 10.0f, ImVec2(std::round(cx - tsz.x * 0.5f), std::round(o.r.y0 - tsz.y * 0.5f)), P.surface950, b);
      }
      const st::Rect& rg = d.obsRing[static_cast<size_t>(d.target)];
      dl->AddEllipse(ImVec2((rg.x0 + rg.x1) * 0.5f, (rg.y0 + rg.y1) * 0.5f), ImVec2(16 * s, 5 * s), P.hot, 0, 24, 1.5f);
      const st::Pt ring{(rg.x0 + rg.x1) * 0.5f, (rg.y0 + rg.y1) * 0.5f};
      const int n = d.obsCount[static_cast<size_t>(d.target)];
      const st::Tray nt = st::trayLayout(g, ring, n + 1, stageMinX(g, g.backY) + 4, stageMaxX(g, g.backY) - 4, v.body0.y + 4, trayMaxY());
      const st::Rect& c = nt.cells[std::min(static_cast<size_t>(n), nt.cells.size() - 1)];
      const float ts = nt.token, cx = (c.x0 + c.x1) * 0.5f;
      dashedRect(dl, ImVec2(cx - ts * 0.5f, c.y0 + 6 * s), ImVec2(cx + ts * 0.5f, c.y0 + 6 * s + ts), P.hot, 1.5f);
    } else {
      dl->AddRect(ImVec2(o.r.x0 - 3, o.r.y0 - 3), ImVec2(o.r.x1 + 3, o.r.y1 + 3), P.hot, o.r.w() * 0.25f + 3, 0, 2.0f);
    }
  } else if (!d.outside) {  // the place it will take, and how wide it is
    if (const Tok* t = tokOf(d.anchor)) {
      Tok f = *t;
      const st::Rect r = g.token(d.pan, d.depth);
      f.r = r, f.group = -1, f.depth = d.depth;
      const Foot ft = footOf(f);
      const int n = 40;
      for (int k = 0; k < n; k += 2) {
        const float a0 = 6.2831853f * static_cast<float>(k) / n, a1 = 6.2831853f * static_cast<float>(k + 1) / n;
        dl->AddLine(ImVec2(ft.cx + ft.rx * std::cos(a0), ft.y + g.footRy(d.depth) * std::sin(a0)),
                    ImVec2(ft.cx + ft.rx * std::cos(a1), ft.y + g.footRy(d.depth) * std::sin(a1)), theme::withAlpha(P.text, 0.45f), 1.2f);
      }
    }
  }
  // The lifted tokens: the anchor under the pointer, the others keeping their offsets.
  const ImVec2 ac(m.x - d.grab.x, m.y - d.grab.y);
  const st::Pt ap = g.at(d.pan, d.depth);
  for (size_t k = 0; k < d.ids.size(); ++k) {
    const Tok* t = tokOf(d.ids[k]);
    if (!t) continue;
    const double pk = std::clamp(d.pan0[k] + (d.pan - d.aPan0), -1.0, 1.0), dk = std::clamp(d.depth0[k] + (d.depth - d.aDepth0), 0.0, 1.0);
    const st::Pt pp = g.at(pk, dk);
    const ImVec2 c = d.ids[k] == d.anchor ? ac : ImVec2(ac.x + pp.x - ap.x, ac.y + pp.y - ap.y);
    const float side = g.side(dk);
    const ImVec2 mn(std::round(c.x - side * 0.5f), std::round(c.y - side * 0.5f - 8 * s));
    for (int sh = 0; sh < 3; ++sh) {  // one soft shadow: three stacked translucent shapes
      const float gw = 3.0f + 4.0f * static_cast<float>(sh);
      dl->AddRectFilled(ImVec2(mn.x - gw * 0.5f, mn.y + 14 * s - gw * 0.3f), ImVec2(mn.x + side + gw * 0.5f, mn.y + side + 14 * s + gw * 0.5f),
                        IM_COL32(0, 0, 0, 30), side * 0.25f + gw * 0.5f);
    }
    Tok lifted = *t;
    lifted.depth = dk;
    lifted.dim = false;
    drawTile(a, dl, lifted, st::Rect{mn.x, mn.y, mn.x + side, mn.y + side}, false, true);
    if (d.ids[k] == d.anchor) drawChip(a, dl, t->name, c.x, mn.y + side + 6, 12.0f, v.body0.x + 4, v.body1.x - 4);
  }
  // The tooltip: what the drop does.
  std::string l1, l2;
  const Tok* at = tokOf(d.anchor);
  if (!d.outside) {
    gView.dragId = d.anchor;
    gView.dragPan = d.target >= 0 ? d.obs[static_cast<size_t>(d.target)].pan : d.pan;
    gView.dragDepth = d.target >= 0 ? d.obs[static_cast<size_t>(d.target)].depth : d.depth;
  }
  if (d.outside) {
    l1 = i18n::tr("Off the stage");
    l2 = i18n::tr("Let go here and it goes back");
  } else if (d.target >= 0) {
    const size_t k = static_cast<size_t>(d.target);
    const st::Obstacle& o = d.obs[k];
    char b[160];
    if (d.obsName[k].empty()) {
      std::snprintf(b, sizeof b, i18n::tr("Join the group: %s"), placeWords(o.pan, o.depth).c_str());
      l1 = b;
      std::snprintf(b, sizeof b, i18n::tr("Same spot as the other %d. Drop to join."), d.obsCount[k]);
      l2 = b;
    } else {
      std::snprintf(b, sizeof b, i18n::tr("Join %s: %s"), d.obsName[k].c_str(), placeWords(o.pan, o.depth).c_str());
      l1 = b;
      l2 = i18n::tr("Takes its exact spot. Drop to join.");
    }
  } else if (at && d.fromTray) {
    // Out of a group: where it goes, and that only this track moves.
    const char* back = d.depth > d.aDepth0 + 0.02 ? i18n::tr("further back") : d.depth < d.aDepth0 - 0.02 ? i18n::tr("nearer") : "";
    const char* side = d.pan > d.aPan0 + 0.02 ? i18n::tr("more to the right") : d.pan < d.aPan0 - 0.02 ? i18n::tr("more to the left") : "";
    std::string where = std::string(back) + (*back && *side ? ", " : "") + side;
    l1 = where.empty() ? std::string(i18n::tr("Out of the group")) : std::string(i18n::tr("Out of the group:")) + " " + where;
    char b[160];
    std::snprintf(b, sizeof b, i18n::tr("Only %s moves. Drop it back on the group to rejoin."), at->name.c_str());
    l2 = b;
  } else if (at) {
    l1 = i18n::tr(st::moveDepthLine(d.aDepth0, d.depth));
    const char* side = st::moveSideLine(d.aPan0, d.pan);
    l2 = side && *side ? i18n::tr(side) : "";
  }
  if (!l1.empty()) floatTip(a, ImVec2(m.x + 28, m.y + 6), l1, l2);
}

// K-224: the box being dragged on the empty stage, over the tokens: a clear outline, a faint fill and how
// many tracks it holds.
void drawBand(App& a, ImDrawList* dl) {
  const Drag& d = L.drag;
  if (d.mode != Drag::Band || !d.bandLive || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
  const auto& P = pal();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const stagesel::Box b = stagesel::Box::of(d.press.x, d.press.y, m.x, m.y);
  const ImVec2 b0(std::floor(b.x0) + 0.5f, std::floor(b.y0) + 0.5f), b1(std::floor(b.x1) + 0.5f, std::floor(b.y1) + 0.5f);
  dl->AddRectFilled(b0, b1, theme::withAlpha(P.accent, 0.13f), 2.0f);
  dl->AddRect(b0, b1, P.accent, 2.0f, 0, 1.5f);
  size_t n = 0;
  for (const Tok& t : gView.toks) n += t.selected ? 1 : 0;
  char c[48];
  const char* how = d.bandMode == stagesel::BandMode::Add ? "+ " : d.bandMode == stagesel::BandMode::Toggle ? "\xC2\xB1 " : "";
  if (n == 1) std::snprintf(c, sizeof c, "%s%s", how, i18n::tr("1 track"));
  else std::snprintf(c, sizeof c, i18n::tr("%s%zu tracks"), how, n);
  const float fs = theme::kFontCaption;
  const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, c);
  // The count sits by the pointer's corner, inside the stage.
  float x = m.x + 12, y = m.y + 14;
  if (x + ts.x + 12 > gView.body1.x - 4) x = m.x - ts.x - 20;
  if (y + ts.y + 6 > gView.panel0.y - 4) y = m.y - ts.y - 18;
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ts.x + 12, y + ts.y + 6), P.accent, 4.0f);
  dl->AddText(a.fontBold, fs, ImVec2(std::round(x + 6), std::round(y + 3)), theme::contrastText(P.accent), c);
}

// Spread out, Arrange by sound or a preset, before Apply: dashed outlines where every track would go.
std::vector<st::Rect> gPreviewRects;
void drawPreview(App& a, ImDrawList* dl, const Preview& pv, bool hoverOnly) {
  const auto& P = pal();
  View& v = gView;
  const st::Geo& g = v.g;
  std::vector<st::Rect> rects;
  for (size_t k = 0; k < pv.ids.size(); ++k) rects.push_back(g.token(pv.to[k].pan, pv.to[k].depth));
  const std::vector<float> off = st::separate(rects, st::kGap * g.scale);
  for (size_t k = 0; k < rects.size(); ++k) rects[k].x0 += off[k], rects[k].x1 += off[k];
  if (!hoverOnly) gPreviewRects = rects;
  auto measure = [&](const std::string& t) { return a.fontBold->CalcTextSizeA(12.0f, 1e9f, 0, t.c_str()).x; };
  for (size_t k = 0; k < pv.ids.size(); ++k) {
    const Tok* t = tokOf(pv.ids[k]);
    if (!t) continue;
    const st::Rect& r = rects[k];
    const ImU32 col = theme::content(song(a).mixer[t->index].color);
    dashedRect(dl, ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), theme::withAlpha(col, hoverOnly ? 0.7f : 0.9f), 1.2f);
    if (!hoverOnly) {
      const ImVec2 c((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f);
      if (t->icon > 0) trackicons::draw(dl, t->icon, c, std::round(r.w() * 0.4f), col);
      const std::string lab = st::cutLabel(t->name, r.w() * 2.6f, measure);
      const float lw = measure(lab);
      dl->AddText(a.fontBold, 12.0f, ImVec2(std::round(c.x - lw * 0.5f), std::round(r.y1 + 7)), P.textDim, lab.c_str());
      if (k < pv.why.size() && pv.why[k].source == 0) {  // not sure: "?" on the tile's corner
        const ImVec2 q(r.x1 - 1, r.y0 + 1);
        dl->AddCircleFilled(q, 7.0f, P.surface600, 16);
        const ImVec2 ts = a.fontBold->CalcTextSizeA(10.0f, 1e9f, 0, "?");
        dl->AddText(a.fontBold, 10.0f, ImVec2(std::round(q.x - ts.x * 0.5f), std::round(q.y - ts.y * 0.5f)), P.text, "?");
      }
    }
  }
}

// ---------------------------------------------------------------- dragging
// What a dropped token must not rest on: the tokens and trays of every track it does not move, at the
// places they will have once it is gone (a group it leaves may become a single token).
void buildObstacles(App& a) {
  (void)a;
  Drag& d = L.drag;
  View& v = gView;
  const st::Geo& g = v.g;
  d.obs.clear(), d.obsName.clear(), d.obsCount.clear(), d.obsRing.clear();
  std::vector<st::Pos> pts;
  std::vector<size_t> who;
  for (size_t i = 0; i < v.toks.size(); ++i)
    if (!dragging(v.toks[i].id)) pts.push_back({v.toks[i].id, v.toks[i].pan, v.toks[i].depth}), who.push_back(i);
  std::vector<bool> grouped(pts.size(), false);
  for (const auto& mem : st::groups(pts)) {
    const st::Pos sp = st::spotOf(pts, mem);
    const st::Pt ring = ringOf(g, sp.pan, sp.depth);
    const st::Tray tr = st::trayLayout(g, ring, static_cast<int>(mem.size()), stageMinX(g, g.backY) + 4, stageMaxX(g, g.backY) - 4, v.body0.y + 4,
                                       trayMaxY());
    st::Obstacle o;
    o.r = tr.box;
    o.pan = sp.pan, o.depth = sp.depth, o.key = static_cast<int>(d.obs.size());
    d.obs.push_back(o);
    d.obsName.push_back("");
    d.obsCount.push_back(static_cast<int>(mem.size()));
    d.obsRing.push_back({ring.x - 16 * g.scale, ring.y - 5 * g.scale, ring.x + 16 * g.scale, ring.y + 5 * g.scale});
    for (size_t k : mem) grouped[k] = true;
  }
  for (size_t k = 0; k < pts.size(); ++k) {
    if (grouped[k]) continue;
    const Tok& t = v.toks[who[k]];
    st::Obstacle o;
    o.r = g.token(t.pan, t.depth);
    o.pan = t.pan, o.depth = t.depth, o.key = static_cast<int>(d.obs.size());
    d.obs.push_back(o);
    d.obsName.push_back(t.name);
    d.obsCount.push_back(1);
    d.obsRing.push_back({});
  }
}

bool snapOn(App& a) { return a.settings.getBool("ui.stage.snap") && !(ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper); }

void beginPress(App& a, ObjectId anchor, ImVec2 m, const st::Rect& r, bool fromTray) {
  Drag& d = L.drag;
  d = Drag{};
  d.mode = Drag::Press;
  d.anchor = anchor;
  d.press = m;
  d.grab = ImVec2(m.x - (r.x0 + r.x1) * 0.5f, m.y - (r.y0 + r.y1) * 0.5f);
  d.from = r;
  d.fromTray = fromTray;
  for (ObjectId id : scopeOf(a, anchor)) {
    if (locked(a, id)) continue;
    const MixerTrack* t = song(a).mixerTrack(id);
    if (!t) continue;
    d.ids.push_back(id);
    d.pan0.push_back(t->pan);
    d.depth0.push_back(t->depth);
    d.sep0.push_back(t->stereoSep);
    if (id == anchor) d.aPan0 = t->pan, d.aDepth0 = t->depth, d.aSep0 = t->stereoSep;
  }
  d.pan = d.aPan0, d.depth = d.aDepth0;
  // The grab point on the floor: a token lifted from a tray keeps the pointer at the tray token's centre.
  if (fromTray) d.grab = ImVec2(0, 0);
}

void moveFrame(App& a) {
  Drag& d = L.drag;
  View& v = gView;
  const st::Geo& g = v.g;
  const ImGuiIO& io = ImGui::GetIO();
  const float speed = io.KeyShift ? 0.25f : 1.0f;
  d.virt.x += io.MouseDelta.x * speed;
  d.virt.y += io.MouseDelta.y * speed;
  if (d.mode == Drag::Press) {
    if (std::fabs(io.MousePos.x - d.press.x) + std::fabs(io.MousePos.y - d.press.y) < 4.0f || d.ids.empty()) return;
    d.mode = Drag::Move;
    d.moved = true;
    buildObstacles(a);
  }
  // The token's centre: where it was grabbed plus the pointer's (fine) travel.
  const ImVec2 c((d.from.x0 + d.from.x1) * 0.5f + d.virt.x, (d.from.y0 + d.from.y1) * 0.5f + d.virt.y);
  const ImVec2 cc = d.fromTray ? ImVec2(io.MousePos.x, io.MousePos.y) : c;
  double dep = std::clamp(g.dOfY(cc.y), 0.0, 1.0);
  const bool snap = snapOn(a);
  if (snap) dep = st::snapDepth(dep);
  double pan = g.panOfX(cc.x, dep);
  if (snap) pan = st::snapPan(pan, 0.03);
  d.pan = pan, d.depth = dep;
  const ImVec2 m = io.MousePos;
  bool overTray = false;
  for (size_t k = 0; k < d.obs.size(); ++k) overTray = overTray || (d.obsName[k].empty() && d.obs[k].r.contains(m.x, m.y));
  d.outside = !g.inside(m.x, m.y) && !overTray;
  // K-224: over a room in the rooms list: the tracks go back to their places and move there on the drop.
  d.dropRoom = railRowAt(m);
  if (d.dropRoom >= 0) d.outside = true;
  d.target = d.outside ? -1 : st::joinTarget(g, pan, dep, {m.x, m.y}, d.obs, io.KeyAlt);
  // The sound follows the drag.
  for (size_t k = 0; k < d.ids.size(); ++k) {
    const double pk = d.outside ? d.pan0[k] : std::clamp(d.pan0[k] + (pan - d.aPan0), -1.0, 1.0);
    const double dk = d.outside ? d.depth0[k] : std::clamp(d.depth0[k] + (dep - d.aDepth0), 0.0, 1.0);
    setIfChanged(a, mixerParam(d.ids[k], MixerParam::Pan), pk);
    setIfChanged(a, mixerParam(d.ids[k], MixerParam::Depth), dk);
  }
}

// The drop: join the target, stay where it was dropped when nothing is in the way, else ease to the
// nearest free place. Off the stage (or Esc) puts everything back. The whole drag is one undo step.
void finishMove(App& a, bool cancel) {
  Drag& d = L.drag;
  View& v = gView;
  const st::Geo& g = v.g;
  if (cancel || d.outside) {
    for (size_t k = 0; k < d.ids.size(); ++k) {
      setIfChanged(a, mixerParam(d.ids[k], MixerParam::Pan), d.pan0[k]);
      setIfChanged(a, mixerParam(d.ids[k], MixerParam::Depth), d.depth0[k]);
    }
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const st::Pt pointer{io.MousePos.x, io.MousePos.y};
  std::vector<st::Obstacle> obs = d.obs;
  const st::Drop dr = st::resolveDrop(g, d.pan, d.depth, pointer, obs, io.KeyAlt);
  std::map<ObjectId, st::Pos> to;
  to[d.anchor] = {d.anchor, dr.pan, dr.depth};
  if (dr.kind != st::Drop::Join) {
    st::Obstacle o;
    o.r = g.token(dr.pan, dr.depth), o.pan = dr.pan, o.depth = dr.depth, o.key = static_cast<int>(obs.size());
    obs.push_back(o);
  }
  const double t0 = now();
  auto ease = [&](ObjectId id, double fromPan, double fromDepth, double toPan, double toDepth) {
    const st::Pt p = g.at(fromPan, fromDepth), q = g.at(toPan, toDepth);
    if (std::fabs(p.x - q.x) + std::fabs(p.y - q.y) > 1.0f) L.ease[id] = Ease{ImVec2(p.x - q.x, p.y - q.y), t0};
  };
  if (dr.kind == st::Drop::Nudged) ease(d.anchor, d.pan, d.depth, dr.pan, dr.depth);
  for (size_t k = 0; k < d.ids.size(); ++k) {
    const ObjectId id = d.ids[k];
    if (id == d.anchor) continue;
    if (std::fabs(d.pan0[k] - d.aPan0) <= st::kJoin && std::fabs(d.depth0[k] - d.aDepth0) <= st::kJoin) {  // it shared the anchor's spot: it still does
      to[id] = {id, dr.pan, dr.depth};
      continue;
    }
    const double pk = std::clamp(d.pan0[k] + (dr.pan - d.aPan0), -1.0, 1.0), dk = std::clamp(d.depth0[k] + (dr.depth - d.aDepth0), 0.0, 1.0);
    const st::Drop o = st::resolveDrop(g, pk, dk, {-1e9f, -1e9f}, obs, false);
    to[id] = {id, o.pan, o.depth};
    if (o.kind == st::Drop::Nudged) ease(id, pk, dk, o.pan, o.depth);
    if (o.kind != st::Drop::Join) {
      st::Obstacle ob;
      ob.r = g.token(o.pan, o.depth), ob.pan = o.pan, ob.depth = o.depth, ob.key = static_cast<int>(obs.size());
      obs.push_back(ob);
    }
  }
  bool back = false;
  for (const auto& [id, p] : to) {
    if (const MixerTrack* m = a.doc.song().mixerTrack(id); m && p.depth > m->depth + 0.01) back = true;
    setIfChanged(a, mixerParam(id, MixerParam::Pan), p.pan);
    setIfChanged(a, mixerParam(id, MixerParam::Depth), p.depth);
  }
  if (back && !shownStage(a).hasRoom() && !a.settings.getBool("ui.stage.roomHinted")) {
    a.settings.set("ui.stage.roomHinted", true);
    L.roomHintUntil = now() + 6.0;
  }
}

void widthFrame(App& a) {
  Drag& d = L.drag;
  const Tok* t = tokOf(d.anchor);
  if (!t) return;
  const ImGuiIO& io = ImGui::GetIO();
  if (!d.moved && std::fabs(io.MousePos.x - d.press.x) < 2.0f) return;
  d.moved = true;
  const Foot f = footOf(*t);
  const double sep = gView.g.sepOfFootRx(f.depth, std::fabs(io.MousePos.x - f.cx));
  const double delta = sep - d.aSep0;
  for (size_t k = 0; k < d.ids.size(); ++k) setIfChanged(a, mixerParam(d.ids[k], MixerParam::StereoSep), std::clamp(d.sep0[k] + delta, -1.0, 1.0));
}

// ---------------------------------------------------------------- edits
void keyEdit(App& a, const char* label, const std::function<void()>& fn) {
  a.doc.amendStep(L.keySerial, [&] {
    a.doc.beginGroup(label);
    fn();
    a.doc.endGroup(false);
  });
  L.keySerial = a.doc.history().stateSerial();
}

void nudge(App& a, const std::vector<ObjectId>& ids, MixerParam p, const std::function<double(double)>& f, const char* label) {
  keyEdit(a, label, [&] {
    for (ObjectId id : ids) {
      if (locked(a, id)) continue;
      const ParamId pid = mixerParam(id, p);
      if (const auto cur = getParam(a.doc.song(), pid)) {
        const double v = f(*cur);
        if (std::fabs(v - *cur) > 1e-9) a.doc.setParam(pid, v, false);
      }
    }
  });
}

void levelStep(App& a, const std::vector<ObjectId>& ids, double notches) {
  nudge(a, ids, MixerParam::Volume, [&](double v) {
    const auto dsc = describeParam(a.doc.song(), mixerParam(ids.front(), MixerParam::Volume));
    if (!dsc) return v;
    return dsc->fromNormalized(std::clamp(dsc->toNormalized(v) + 0.02 * notches, 0.0, 1.0));
  }, "level on stage");
}

void toggleMute(App& a, const std::vector<ObjectId>& ids) {
  const MixerTrack* first = ids.empty() ? nullptr : song(a).mixerTrack(ids.front());
  if (!first) return;
  const bool mute = !first->muted;
  a.doc.beginGroup(mute ? "mute mixer tracks" : "unmute mixer tracks");
  for (ObjectId id : ids) a.doc.setParam(mixerParam(id, MixerParam::Mute), mute ? 1.0 : 0.0, false);
  a.doc.endGroup();
}

void toggleSolo(App& a, const std::vector<ObjectId>& ids) {
  const MixerTrack* first = ids.empty() ? nullptr : song(a).mixerTrack(ids.front());
  if (!first) return;
  const bool solo = !first->solo;
  a.doc.beginGroup("solo mixer track");
  for (ObjectId id : ids) a.doc.editMixerTrack(id, "solo mixer track", [solo](MixerTrack& m) { m.solo = solo; });
  a.doc.endGroup();
}

void renameTrack(App& a, ObjectId id) {
  const Song& s = song(a);
  const int i = s.mixerIndex(id);
  if (i <= 0) return;
  menus::openPrompt(i18n::tr("Rename mixer track"), mixerTrackName(s, static_cast<size_t>(i)), [&a, id](const std::string& n) {
    a.doc.editMixerTrack(id, "rename mixer track", [&](MixerTrack& m) { m.name = n; });
  });
}

void setPlace(App& a, const std::vector<ObjectId>& ids, const std::function<void(double&, double&)>& f, const char* label) {
  a.doc.endGesture();
  a.doc.beginGroup(label);
  for (ObjectId id : ids) {
    const MixerTrack* t = song(a).mixerTrack(id);
    if (!t || t->locked) continue;
    double p = t->pan, d = t->depth;
    f(p, d);
    a.doc.setParam(mixerParam(id, MixerParam::Pan), p, false);
    a.doc.setParam(mixerParam(id, MixerParam::Depth), d, false);
  }
  a.doc.endGroup();
  a.doc.endGesture();
}

// ---------------------------------------------------------------- menus
void tokenMenu(App& a, ObjectId id) {
  const Song& s = song(a);
  const int i = s.mixerIndex(id);
  if (i <= 0) return;
  const MixerTrack& t = s.mixer[static_cast<size_t>(i)];
  const std::vector<ObjectId> scope = scopeOf(a, id);
  ImGui::TextDisabled("%s", mixerTrackName(s, static_cast<size_t>(i)).c_str());
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Effects..."))) openEffects(a, id);
  if (ImGui::MenuItem(i18n::tr("Rename..."), keys::label("stage.rename"))) renameTrack(a, id);
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t c = 0;
    if (menus::colorGrid(&c)) mixops::setColor(a.doc, scope, c);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Icon"))) {
    const int autoId = st::roleIcon(guessOf(a, static_cast<size_t>(i)).role);
    char b[96];
    std::snprintf(b, sizeof b, i18n::tr("Automatic (%s)"), autoId > 0 ? trackicons::name(autoId) : i18n::tr("initials"));
    if (ImGui::MenuItem(b, nullptr, t.icon == 0)) mixops::setIcon(a.doc, scope, 0);
    ImGui::Separator();
    int pick = 0;
    if (trackicons::picker(t.icon, &pick)) mixops::setIcon(a.doc, scope, pick);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Mute"), keys::label("stage.mute"), t.muted)) toggleMute(a, scope);
  if (ImGui::MenuItem(i18n::tr("Solo"), keys::label("stage.solo"), t.solo)) toggleSolo(a, scope);
  {
    const int r = shownRoom(a);
    const MixerTrack* room = mixops::stageRoom(s, r);
    bool inRoom = false;
    if (room)
      for (const auto& sd : t.sends) inRoom = inRoom || (sd.to == room->id && sd.stageRoom);
    if (ImGui::MenuItem(i18n::tr("Send to the space"), nullptr, inRoom)) {
      if (!room) mixops::ensureStageRoom(a.doc, scope, "add the room", r);
      else if (!inRoom) {
        a.doc.beginGroup("send to the room");
        mixops::addRoomSends(a.doc, scope, r);
        a.doc.endGroup();
      } else {
        a.doc.beginGroup("leave the room");
        for (ObjectId x : scope) mixops::setSendRoom(a.doc, x, room->id, false);
        a.doc.endGroup();
      }
    }
    ui::Hint("A send into this room's space track (made the first time): further back sounds further away");
  }
  // K-224: the same as dragging them onto a room in the rooms list.
  if (roomops::roomCount(s) > 1 && ImGui::BeginMenu(i18n::tr("Move to room"))) {
    for (int k = 0; k < roomops::roomCount(s); ++k) {
      if (k == shownRoom(a)) continue;
      ImGui::PushID(k);
      if (ImGui::MenuItem(roomops::roomName(s, k).c_str())) moveToRoom(a, scope, k);
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Reset"))) {
    using P = MixerParam;
    if (ImGui::MenuItem(i18n::tr("Position"), keys::label("stage.reset"))) mixops::resetParams(a.doc, scope, {P::Pan, P::Depth});
    if (ImGui::MenuItem(i18n::tr("Width"))) mixops::resetParams(a.doc, scope, {P::StereoSep});
    if (ImGui::MenuItem(i18n::tr("Focus"))) mixops::resetParams(a.doc, scope, {P::Focus});
    if (ImGui::MenuItem(i18n::tr("All of these"))) mixops::resetParams(a.doc, scope, {P::Pan, P::Depth, P::StereoSep, P::Focus});
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Move to the front"))) setPlace(a, scope, [](double&, double& d) { d = 0; }, "move on stage");
  if (ImGui::MenuItem(i18n::tr("Centre"))) setPlace(a, scope, [](double& p, double&) { p = 0; }, "move on stage");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Show in the mix"))) {
    selectOnly(a, id);
    setView(a, false);
  }
  if (ImGui::BeginMenu(i18n::tr("Mixer track"))) {
    mixerTrackMenu(a, static_cast<size_t>(i));
    ImGui::EndMenu();
  }
}

void viewItems(App& a) {
  auto toggleSetting = [&](const char* label, const char* key, const char* hint) {
    if (ImGui::MenuItem(i18n::tr(label), nullptr, a.settings.getBool(key))) a.settings.set(key, !a.settings.getBool(key));
    if (hint) ui::Hint(hint);
  };
  toggleSetting(i18n::N_("Snap"), "ui.stage.snap", "Drags land on five distances and stick to the centre (hold Ctrl to place freely)");
  toggleSetting(i18n::N_("Names"), "ui.stage.labels", "Track names under the tokens");
  const bool all = a.settings.getNumber("ui.stage.tracks") >= 0.5;
  if (ImGui::MenuItem(i18n::tr("Every insert track"), nullptr, all)) a.settings.set("ui.stage.tracks", all ? 0 : 1);
  ui::Hint("Off: only tracks in use (with a channel, effects, an input, a name or a place)");
  if (ImGui::MenuItem(i18n::tr("Rooms column"), keys::label("stage.roomsPanel"), roomsPanelOpen(a))) setRoomsPanelOpen(a, !roomsPanelOpen(a));
  ui::Hint("The rooms beside the stage: show, add, solo and manage them (also the tab on its edge)");
}

void floorMenu(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Presets"))) {
    presetsMenu(a);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Spread out"))) startPreview(a, PreviewKind::Spread);
  if (ImGui::MenuItem(i18n::tr("Arrange by sound..."))) startPreview(a, PreviewKind::Sound);
  ImGui::Separator();
  viewItems(a);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Select all"), keys::label("stage.selectAll"))) {
    a.mixerView.sel.clear();
    for (const Tok& t : gView.toks) a.mixerView.sel.insert(t.id);
    if (!gView.toks.empty() && !a.mixerView.sel.count(a.selMixer)) a.selMixer = gView.toks.front().id;
  }
}

// The ≡ menu's Stage part.
void stageMenu(App& a) {
  if (!ImGui::BeginMenu(i18n::tr("Stage"))) return;
  viewItems(a);
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Presets"))) {  // also on the toolbar when the mixer is wide
    presetsMenu(a);
    ImGui::EndMenu();
  }
  const Song& s = song(a);
  if (ImGui::BeginMenu(i18n::tr("Pan law"))) {
    static const char* names[] = {i18n::N_("Balance"), i18n::N_("Constant power"), i18n::N_("Stage placement")};
    static const char* hints[] = {"Turning the pan quietens the other side (older projects)", "Equal loudness across the stage",
                                  "A stereo track keeps both channels as it moves to the side (new projects)"};
    for (int k = 0; k < 3; ++k) {
      if (ImGui::MenuItem(i18n::tr(names[k]), nullptr, static_cast<int>(s.panLaw) == k)) mixops::setPanLaw(a.doc, static_cast<PanLaw>(k));
      ui::Hint(hints[k]);
    }
    ImGui::EndMenu();
  }
  auto stageFlag = [&](const char* label, bool StageSettings::*f, const char* hint) {
    const int r = shownRoom(a);
    if (ImGui::MenuItem(i18n::tr(label), nullptr, roomops::roomAt(s, r).*f)) {
      const bool on = !(roomops::roomAt(s, r).*f);
      a.doc.editSong(label, [&](Song& x) {
        if (StageSettings* st = roomops::roomAtMut(x, r)) st->*f = on;
      });
    }
    ui::Hint(hint);
  };
  stageFlag(i18n::N_("Space pre-delay"), &StageSettings::preDelay, "Tracks at the front reach the space a little later, as on a real stage");
  stageFlag(i18n::N_("Early reflections"), &StageSettings::early, "The first echoes from the space's walls, by where each track stands");
  stageFlag(i18n::N_("Less space up front"), &StageSettings::frontQuiet, "Tracks at the front send less to the space");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Show the welcome card"))) a.settings.set("ui.stage.welcomed", false);
  ImGui::EndMenu();
}

// ---------------------------------------------------------------- input
void stageInput(App& a) {
  View& v = gView;
  const st::Geo& g = v.g;
  Drag& d = L.drag;
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mn = v.body0, mx(v.body1.x, v.panel0.y - 2);
  ImGui::SetCursorScreenPos(mn);
  ImGui::InvisibleButton("##stage", ImVec2(std::max(1.0f, mx.x - mn.x), std::max(1.0f, mx.y - mn.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  // The stage's own button is active while it is pressed: the window still counts as hovered then.
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                   ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !v.cardOpen;
  const bool pressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImVec2 m = io.MousePos;
  // Arrange's preview: only its outlines answer the pointer (why each track went where it did).
  if (v.preview.active()) {
    v.hovered = kNoId;
    if (hov)
      for (size_t k = 0; k < gPreviewRects.size() && k < v.preview.ids.size(); ++k)
        if (gPreviewRects[k].contains(m.x, m.y)) {
          std::string l1, l2;
          if (v.preview.kind == PreviewKind::Sound && k < v.preview.why.size()) {
            const st::Guess& gs = v.preview.why[k];
            char b[160];
            if (gs.source == 0) {
              l1 = i18n::tr("Not sure what this is");
              l2 = i18n::tr("It stays in the middle for you to place");
            } else {
              std::snprintf(b, sizeof b, i18n::tr("Placed as %s: %s"), i18n::tr(st::roleName(gs.role)), i18n::tr(gs.why.c_str()));
              l1 = b;
              l2 = gs.source == 1 ? i18n::tr("From what plays on it") : gs.source == 2 ? i18n::tr("From its sound while the song played") : i18n::tr("From its name only");
            }
          } else {
            l1 = tokOf(v.preview.ids[k]) ? tokOf(v.preview.ids[k])->name : "";
          }
          floatTip(a, ImVec2(gPreviewRects[k].x0, gPreviewRects[k].y1 + 22), l1, l2);
          break;
        }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) cancelPreview();
    return;
  }
  // What is under the pointer: the selected track's width handles, a tray's cells, the tokens on their own.
  ObjectId hit = kNoId;
  st::Rect hitRect;
  bool hitTray = false;
  int handle = 0, moreGroup = -1, trayHover = -1;
  if (hov && (d.mode == Drag::None || d.mode == Drag::Press)) {
    if (onlyOneSelected(a))
      if (const Tok* t = tokOf(a.selMixer)) {
        const Foot f = footOf(*t);
        for (int k = 0; k < 2; ++k) {
          const float hx = f.cx + (k ? 1.0f : -1.0f) * f.rx;
          if (std::fabs(m.x - hx) <= kHandleR * g.scale + 3 && std::fabs(m.y - f.y) <= kHandleR * g.scale + 3) handle = k + 1;
        }
      }
    if (!handle)
      for (size_t gi = v.groups.size(); gi-- > 0;) {
        const Grp& gr = v.groups[gi];
        if (!gr.tray.box.contains(m.x, m.y)) continue;
        trayHover = static_cast<int>(gi);
        const int c = st::trayCellAt(gr.tray, m.x, m.y);
        if (c >= 0 && c == gr.tray.shown && gr.tray.more > 0) moreGroup = static_cast<int>(gi);
        else if (c >= 0)
          for (size_t k : gr.toks)
            if (v.toks[k].cell == c) hit = v.toks[k].id, hitRect = v.toks[k].r, hitTray = true;
        break;
      }
    if (!handle && trayHover < 0) {
      std::vector<size_t> order;
      for (size_t i = 0; i < v.toks.size(); ++i)
        if (v.toks[i].group < 0) order.push_back(i);
      std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return v.toks[x].depth < v.toks[y].depth; });  // near first
      for (size_t i : order)
        if (v.toks[i].r.grown(2).contains(m.x, m.y)) {
          hit = v.toks[i].id, hitRect = v.toks[i].r;
          break;
        }
    }
  }
  L.hoverHandle = handle;
  L.hoverGroup = trayHover;
  v.hovered = d.mode == Drag::Move ? d.anchor : hit;
  // The listener.
  if (hov && hit == kNoId && std::hypot(m.x - g.cx, m.y - g.listenerY) < 20 * g.scale) ui::Tooltip(i18n::N_("You: you hear the stage from here"));
  if (hov && d.mode == Drag::None) {
    if (handle) ui::Hint("Drag: stereo width (both sides move) \xC2\xB7 double-click: reset the width");
    else if (moreGroup >= 0) ui::Hint("The rest of the tracks at this spot");
    else if (hit != kNoId)
      ui::Hint("Drag: move it (onto another track: share its spot) \xC2\xB7 Shift: fine \xC2\xB7 Alt: exact \xC2\xB7 wheel: level \xC2\xB7 double-click: its effects");
    else ui::Hint("Click a track to select it \xC2\xB7 drag on the floor to select several \xC2\xB7 right-click: stage menu");
  }
  if (hov && hit != kNoId && d.mode == Drag::None && io.MouseWheel != 0) {  // the wheel: level (Shift: width)
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const std::vector<ObjectId> scope = scopeOf(a, hit);
    if (io.KeyShift) nudge(a, scope, MixerParam::StereoSep, [&](double x) { return std::clamp(x + io.MouseWheel * 0.05, -1.0, 1.0); }, "width on stage");
    else levelStep(a, scope, io.MouseWheel * ((io.KeyCtrl || io.KeySuper) ? 0.25 : 1.0));
    L.keySerial = 0;
  }
  // Press.
  if (pressed && hov) {
    const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (handle) {
      if (dbl) {
        mixops::resetParams(a.doc, scopeOf(a, a.selMixer), {MixerParam::StereoSep});
        d = Drag{};
      } else {
        beginPress(a, a.selMixer, m, tokOf(a.selMixer)->r, false);
        d.mode = Drag::Width;
      }
    } else if (moreGroup >= 0) {
      L.menuGroup = moreGroup;
      ImGui::OpenPopup("##stagemore");
      d = Drag{};
    } else if (hit != kNoId) {
      if (dbl) {  // its effects, staying on the stage (Ctrl/Cmd: its strip in the mix)
        selectOnly(a, hit);
        if (io.KeyCtrl || io.KeySuper) setView(a, false);
        else openEffects(a, hit);
        d = Drag{};
      } else {
        const bool plain = !(io.KeyCtrl || io.KeySuper || io.KeyShift);
        const bool wasSel = isSelected(a, hit) && selection(a).size() > 1;
        clickSelect(a, hit);
        beginPress(a, hit, m, hitRect, hitTray);
        d.toggleOnRelease = plain && wasSel;
      }
    } else {
      d = Drag{};
      d.mode = Drag::Band;
      d.press = m;
      d.bandBefore = selection(a);
      d.bandBefore.erase(std::remove_if(d.bandBefore.begin(), d.bandBefore.end(), [](ObjectId id) { return !tokOf(id); }), d.bandBefore.end());
      d.bandMode = (io.KeyCtrl || io.KeySuper) ? stagesel::BandMode::Toggle : io.KeyShift ? stagesel::BandMode::Add : stagesel::BandMode::Replace;
    }
  }
  const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (held && (d.mode == Drag::Press || d.mode == Drag::Move)) moveFrame(a);
  if (held && d.mode == Drag::Width) widthFrame(a);
  if (d.mode == Drag::Move && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    finishMove(a, true);
    d = Drag{};
    L.escFrame = ImGui::GetFrameCount();
  }
  // K-224: the box: the tracks it touches are chosen as it grows (Shift adds, Ctrl / Cmd flips), drawn over
  // the tokens (drawBand). Esc puts the choice back as it was.
  if (d.mode == Drag::Band && held) {
    const stagesel::Box box = stagesel::Box::of(d.press.x, d.press.y, m.x, m.y);
    d.bandLive = d.bandLive || box.width() >= 3 || box.height() >= 3;
    if (d.bandLive) {
      std::vector<ObjectId> caught;
      const st::Rect band{box.x0, box.y0, box.x1, box.y1};
      for (const Tok& t : v.toks)
        if ((t.group < 0 || t.cell >= 0) && st::overlapArea(band, t.r) > 0) caught.push_back(t.id);
      setChoice(a, stagesel::applyBand(d.bandBefore, caught, d.bandMode));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      setChoice(a, d.bandBefore);
      d = Drag{};
      L.escFrame = ImGui::GetFrameCount();
    }
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && d.mode != Drag::None) {
    if (d.mode == Drag::Band) {
      if (!d.bandLive && d.bandMode == stagesel::BandMode::Replace) {  // a click on the floor: nothing chosen (the panel shows the room)
        a.mixerView.sel.clear();
        a.selMixer = song(a).masterId();
      }
      a.editFocus = App::EditFocus::Mixer;
    } else if (d.mode == Drag::Move) {
      const int to = d.dropRoom;
      const std::vector<ObjectId> ids = d.ids;
      finishMove(a, false);  // over the rooms list: every track back where it was
      if (to >= 0 && to != shownRoom(a)) moveToRoom(a, ids, to);  // joins the drag's undo step
    } else if (d.mode == Drag::Press && d.toggleOnRelease) {
      selectOnly(a, d.anchor);
    }
    d = Drag{};
  }
  // Right-click: the token's menu or the floor's.
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && d.mode == Drag::None) {
    if (hit != kNoId) {
      L.menuTrack = hit;
      if (!isSelected(a, hit)) selectOnly(a, hit);
      a.selMixer = hit;
      ImGui::OpenPopup("##stagetoken");
    } else {
      ImGui::OpenPopup("##stagefloor");
    }
  }
  if (ImGui::BeginPopup("##stagetoken")) {
    tokenMenu(a, L.menuTrack);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##stagefloor")) {
    floorMenu(a);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##stagemore")) {  // a big group's "+N": the rest, and Arrange
    if (L.menuGroup >= 0 && static_cast<size_t>(L.menuGroup) < v.groups.size()) {
      const Grp& gr = v.groups[static_cast<size_t>(L.menuGroup)];
      for (size_t k : gr.toks) {
        const Tok& t = v.toks[k];
        if (t.cell >= 0) continue;
        if (ImGui::MenuItem(t.name.c_str(), nullptr, t.selected)) selectOnly(a, t.id);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Arrange by sound..."))) startPreview(a, PreviewKind::Sound);
      if (ImGui::MenuItem(i18n::tr("Spread out"))) startPreview(a, PreviewKind::Spread);
    }
    ImGui::EndPopup();
  } else {
    L.menuGroup = -1;
  }
}

void keyboard(App& a) {
  if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
  View& v = gView;
  if (v.preview.active()) return;
  const ImGuiIO& io = ImGui::GetIO();
  std::vector<ObjectId> ids;
  for (ObjectId id : selection(a))
    if (tokOf(id)) ids.push_back(id);
  const bool fine = io.KeyShift, coarse = io.KeyCtrl || io.KeySuper;
  bool any = false;
  if (!ids.empty()) {
    // The level keys share the arrows (with Alt): pressedHeld ignores modifiers, so Alt tells them apart.
    if (io.KeyAlt && (keys::pressedHeld("stage.levelUp") || keys::pressedHeld("stage.levelDown"))) {
      levelStep(a, ids, keys::pressedHeld("stage.levelUp") ? 1.0 : -1.0);
      any = true;
    } else if (!io.KeyAlt && (keys::pressedHeld("stage.up") || keys::pressedHeld("stage.down"))) {
      const double dir = keys::pressedHeld("stage.up") ? 1.0 : -1.0;
      nudge(a, ids, MixerParam::Depth, [&](double x) { return fine ? std::clamp(x + dir * 0.01, 0.0, 1.0) : st::snapDepth(x + dir * 0.25); }, "move on stage");
      any = true;
    }
    if (!io.KeyAlt && (keys::pressedHeld("stage.left") || keys::pressedHeld("stage.right"))) {
      const double dir = keys::pressedHeld("stage.right") ? 1.0 : -1.0, step = fine ? 0.01 : coarse ? 0.25 : 0.05;
      nudge(a, ids, MixerParam::Pan, [&](double x) { return std::clamp(x + dir * step, -1.0, 1.0); }, "move on stage");
      any = true;
    }
    if (keys::pressedHeld("stage.narrower") || keys::pressedHeld("stage.wider")) {
      const double dir = keys::pressedHeld("stage.wider") ? 1.0 : -1.0;
      nudge(a, ids, MixerParam::StereoSep, [&](double x) { return std::clamp(x + dir * 0.05, -1.0, 1.0); }, "width on stage");
      any = true;
    }
    if (keys::pressed("stage.mute")) toggleMute(a, ids);
    if (keys::pressed("stage.solo")) toggleSolo(a, ids);
    if (keys::pressed("stage.reset")) mixops::resetParams(a.doc, ids, {MixerParam::Pan, MixerParam::Depth});
  }
  if (keys::pressed("stage.rename") && a.selMixer != kNoId && tokOf(a.selMixer)) renameTrack(a, a.selMixer);
  if (keys::pressed("stage.roomsPanel")) setRoomsPanelOpen(a, !roomsPanelOpen(a));  // K-270
  if ((keys::pressed("stage.next") || keys::pressed("stage.previous")) && !v.toks.empty()) {
    const bool next = keys::pressed("stage.next");
    int at = -1;
    for (size_t k = 0; k < v.toks.size(); ++k)
      if (v.toks[k].id == a.selMixer) at = static_cast<int>(k);
    const int n = static_cast<int>(v.toks.size());
    const int to = at < 0 ? (next ? 0 : n - 1) : ((at + (next ? 1 : -1)) % n + n) % n;
    selectOnly(a, v.toks[static_cast<size_t>(to)].id);
  }
  if (keys::pressed("stage.selectAll")) {
    a.mixerView.sel.clear();
    for (const Tok& t : v.toks) a.mixerView.sel.insert(t.id);
    if (!v.toks.empty() && !a.mixerView.sel.count(a.selMixer)) a.selMixer = v.toks.front().id;
  }
  if (keys::pressed("stage.deselect")) mixerDeselect(a);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && L.drag.mode == Drag::None && L.escFrame != ImGui::GetFrameCount() && !a.mixerView.sel.empty())
    setChoice(a, {});  // K-224
  if (!any && !keys::keyDown("stage.left") && !keys::keyDown("stage.right") && !keys::keyDown("stage.up") && !keys::keyDown("stage.down") &&
      !keys::keyDown("stage.narrower") && !keys::keyDown("stage.wider") && !keys::keyDown("stage.levelUp") && !keys::keyDown("stage.levelDown"))
    L.keySerial = 0;
}

// ---------------------------------------------------------------- the toolbar
// A toolbar button drawn by hand: an optional icon or lamp, a dim caption, a value, a chevron.
bool toolButton(App& a, const char* id, ui::Icon icon, bool useIcon, int lamp, const char* caption, const char* value, bool chevron, bool on,
                const char* hint) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight(), pad = 8, gap = 6;
  const float fs = ImGui::GetFontSize();
  ImFont* f = ImGui::GetFont();
  float w = pad * 2;
  if (useIcon || lamp >= 0) w += 14 + gap;
  const float cw = caption ? f->CalcTextSizeA(fs, 1e9f, 0, caption).x : 0, vw = value ? f->CalcTextSizeA(fs, 1e9f, 0, value).x : 0;
  w += cw + (caption && value ? gap : 0) + vw;
  if (chevron) w += gap + 8;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on || hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? theme::mix(P.dark ? P.surface700 : P.surface800, P.hot, 0.14f) : P.surface700, theme::kRadius);
  float x = p.x + pad;
  const float cy = p.y + h * 0.5f;
  const ImU32 fg = on ? P.hot : P.text;
  if (useIcon) {
    ui::drawIcon(dl, icon, ImVec2(x + 7, cy), 14, fg);
    x += 14 + gap;
  } else if (lamp >= 0) {
    dl->AddCircleFilled(ImVec2(x + 7, cy), 3.5f, lamp ? P.led : P.textFaint, 12);
    x += 14 + gap;
  }
  if (caption) {
    dl->AddText(ImVec2(x, std::round(cy - fs * 0.5f)), value ? (on ? P.hot : P.textDim) : fg, caption);
    x += cw + (value ? gap : 0);
  }
  if (value) {
    dl->AddText(ImVec2(x, std::round(cy - fs * 0.5f)), fg, value);
    x += vw;
  }
  if (chevron) {
    x += gap;
    dl->AddTriangleFilled(ImVec2(x, cy - 2.5f), ImVec2(x + 8, cy - 2.5f), ImVec2(x + 4, cy + 2.5f), on ? P.hot : P.textDim);
  }
  if (hint) ui::Hint(hint);
  (void)a;
  return clicked;
}

void toolbar(App& a) {
  const auto& P = pal();
  View& v = gView;
  ui::BeginToolbar("##stagetb");
  menuButton("stagemenu", "Mixer menu: the stage's options, pan law, room extras, tracks", [&a]() {
    stageMenu(a);
    ImGui::Separator();
    mixerOptionsMenu(a);
  });
  ImGui::SameLine(0, 6);
  viewSwitch(a);
  // A narrow mixer (docked as a side column): the presets stay in the menu, the room shows its name only.
  const bool narrow = ImGui::GetWindowWidth() < 46.0f * ImGui::GetFontSize();
  ImGui::SameLine(0, 8);
  ImVec2 pp = ImGui::GetCursorScreenPos();
  if (!narrow) {
    ui::ToolbarDivider();
    ImGui::SameLine(0, 8);
    const bool presetsOpen = ImGui::IsPopupOpen("##stagepresets");
    pp = ImGui::GetCursorScreenPos();
    if (toolButton(a, "##presetsbtn", ui::Icon::Stage, true, -1, i18n::tr("Presets"), nullptr, true, presetsOpen, "Stage presets, Spread out and Arrange by sound") ||
        L.openPresets)
      ImGui::OpenPopup("##stagepresets"), L.openPresets = false;
  } else if (L.openPresets) {
    ImGui::OpenPopup("##stagepresets"), L.openPresets = false;
  }
  ImGui::SetNextWindowPos(ImVec2(std::max(ImGui::GetWindowPos().x, pp.x - 46), pp.y + ImGui::GetFrameHeight() + 4));
  if (ImGui::BeginPopup("##stagepresets")) {
    presetsMenu(a);
    ImGui::EndPopup();
  }
  if (!narrow) ImGui::SameLine(0, 10);
  const MixerTrack* room = mixops::stageRoom(song(a), shownRoom(a));
  const bool roomOn = shownStage(a).hasRoom() && room && !room->muted;
  const std::string type = i18n::tr(mixops::roomTypeName(shownStage(a).roomType));
  pp = ImGui::GetCursorScreenPos();
  if (toolButton(a, "##roombtn", ui::Icon::Stage, false, roomOn ? 1 : 0, narrow ? nullptr : i18n::tr("Space"), type.c_str(), true,
                 ImGui::IsPopupOpen("##stageroom"), "The space this room plays in: distance and focus are heard once a space is picked") ||
      L.openRoom)
    ImGui::OpenPopup("##stageroom"), L.openRoom = false;
  const float usedX = ImGui::GetItemRectMax().x;
  if (L.roomHintUntil > now()) {
    if (ImGui::IsPopupOpen("##stageroom") || roomOn) L.roomHintUntil = 0;
    else floatTip(a, ImVec2(pp.x, pp.y + ImGui::GetFrameHeight() + 8), i18n::tr("Pick a space to hear depth"),
                  i18n::tr("Distance is kept, but with no space it changes nothing yet"));
  }
  ImGui::SetNextWindowPos(ImVec2(std::max(ImGui::GetWindowPos().x, pp.x - 50), pp.y + ImGui::GetFrameHeight() + 4));
  v.roomOpen = false;
  if (ImGui::BeginPopup("##stageroom")) {
    v.roomOpen = true;
    roomPopover(a);
    ImGui::EndPopup();
  }
  // Right: the solo chip, Routing.
  const Song& s = song(a);
  std::vector<ObjectId> solos;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (s.mixer[i].solo) solos.push_back(s.mixer[i].id);
  const ImGuiStyle& sty = ImGui::GetStyle();
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - sty.WindowPadding.x;
  const float routeW = ImGui::CalcTextSize(i18n::tr("Routing")).x + sty.FramePadding.x * 2;
  std::string chip;
  if (!solos.empty()) {
    if (solos.size() == 1) chip = mixerTrackName(s, static_cast<size_t>(s.mixerIndex(solos.front())));
    else {
      char b[48];
      std::snprintf(b, sizeof b, i18n::tr("%zu tracks"), solos.size());
      chip = b;
    }
  }
  const float chipW = chip.empty() ? 0 : ImGui::CalcTextSize(chip.c_str()).x + 46;
  float x = right - routeW - (chip.empty() ? 0 : chipW + 6);
  if (x > usedX + 12) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
    if (!chip.empty()) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetFrameHeight();
      ImGui::InvisibleButton("##solochip", ImVec2(chipW, h));
      const bool hovc = ImGui::IsItemHovered();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p, ImVec2(p.x + chipW, p.y + h), theme::mix(P.dark ? P.surface700 : P.surface800, P.solo, hovc ? 0.22f : 0.14f), theme::kRadius);
      const float fs = ImGui::GetFontSize(), cy = p.y + (h - fs) * 0.5f;
      dl->AddText(a.fontBold, fs, ImVec2(p.x + 9, cy), P.solo, "S");
      dl->AddText(ImVec2(p.x + 24, cy), P.solo, chip.c_str());
      ui::drawIcon(dl, ui::Icon::Close, ImVec2(p.x + chipW - 12, p.y + h * 0.5f), 10, P.solo);
      ui::Hint("Solo is on: click to turn it off for every track");
      if (ImGui::IsItemClicked()) {
        a.doc.beginGroup("solo mixer track");
        for (ObjectId id : solos) a.doc.editMixerTrack(id, "solo mixer track", [](MixerTrack& m) { m.solo = false; });
        a.doc.endGroup();
      }
      ImGui::SameLine(0, 6);
    }
    if (ui::TextButton("Routing", a.mixerView.showRouting, 0, "Routing overview: every route between the tracks"))
      a.mixerView.showRouting = !a.mixerView.showRouting;
  }
  ui::EndToolbar();
}

}  // namespace

// ---------------------------------------------------------------- public
bool active(App& a) { return a.showMixer && a.showStage; }

void setView(App& a, bool stage) {
  a.showStage = stage;
  a.settings.set("ui.mixerView", stage ? 1 : 0);
  if (stage) {
    a.showMixer = true;
    a.focusWindow = "Mixer";
  }
  if (!stage) {
    cancelPreview();
    L.drag = Drag{};
  }
}

void toggle(App& a) { setView(a, !a.showStage); }

int currentRoom(App& a) { return shownRoom(a); }

void sync(App& a) {
  static bool loaded = false, last = false;
  if (!loaded) {
    loaded = true;
    const bool asked = a.showStage;  // --show stage, a layout
    if (a.settings.getNumber("ui.mixerView") >= 0.5) a.showStage = true;
    if (asked) a.showMixer = true;
    last = a.showStage;
  }
  if (a.showStage != last) {  // asked for elsewhere (View menu, palette, keys): the mixer shows it
    last = a.showStage;
    a.settings.set("ui.mixerView", a.showStage ? 1 : 0);
    if (a.showStage) {
      a.showMixer = true;
      a.focusWindow = "Mixer";
    } else {
      cancelPreview();
    }
  }
  if (gView.frame < ImGui::GetFrameCount() - 1) gView.open = false;
}

void viewSwitch(App& a) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight(), fs = ImGui::GetFontSize();
  const char* labels[2] = {i18n::tr("Mix"), i18n::tr("Stage")};
  const ui::Icon icons[2] = {ui::Icon::Mixer, ui::Icon::Stage};
  const char* hints[2] = {"Mix: the mixer's strips", "Stage: place the tracks on a stage (left and right, near and far)"};
  float segW = 0;
  for (const char* l : labels) segW = std::max(segW, ImGui::CalcTextSize(l).x + 14 + 6 + 20);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + segW * 2 + 4, p.y + h), P.field, theme::kRadius);
  if (!P.dark) dl->AddRect(p, ImVec2(p.x + segW * 2 + 4, p.y + h), P.line, theme::kRadius);
  const int cur = a.showStage ? 1 : 0;
  for (int i = 0; i < 2; ++i) {
    ImGui::PushID(i);
    const ImVec2 q(p.x + 2 + segW * static_cast<float>(i), p.y + 2);
    ImGui::SetCursorScreenPos(q);
    if (ImGui::InvisibleButton("##view", ImVec2(segW, h - 4)) && cur != i) setView(a, i == 1);
    const bool hov = ImGui::IsItemHovered(), on = cur == i;
    ui::Hint(hints[i]);
    if (on) dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h - 4), theme::mix(P.dark ? P.surface700 : P.surface800, P.hot, 0.14f), std::max(1.0f, theme::kRadius - 1));
    else if (hov) dl->AddRectFilled(q, ImVec2(q.x + segW, q.y + h - 4), P.surface700, std::max(1.0f, theme::kRadius - 1));
    const ImU32 fg = on ? P.hot : hov ? P.text : P.textDim;
    const float tw = ImGui::CalcTextSize(labels[i]).x, x0 = std::round(q.x + (segW - (14 + 6 + tw)) * 0.5f);
    ui::drawIcon(dl, icons[i], ImVec2(x0 + 7, q.y + (h - 4) * 0.5f), 14, fg);
    dl->AddText(ImVec2(x0 + 20, std::round(q.y + (h - 4 - fs) * 0.5f)), fg, labels[i]);
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(segW * 2 + 4, h));
}

void drawBody(App& a) {
  const auto& P = pal();
  View& v = gView;
  v.open = true;
  v.frame = ImGui::GetFrameCount();
  v.dragId = kNoId;
  const Song& s = song(a);
  if (!s.mixerTrack(a.selMixer) && !s.mixer.empty()) a.selMixer = s.masterId();
  L.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  toolbar(a);
  const ImVec2 c0 = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
  v.body0 = c0;
  v.body1 = ImVec2(c0.x + avail.x, c0.y + std::max(1.0f, avail.y));
  const bool panel = avail.y >= 380;
  const float gut = theme::kS4;
  v.panel1 = ImVec2(v.body1.x - gut, v.body1.y - gut);
  v.panel0 = panel ? ImVec2(v.body0.x + gut, v.panel1.y - 136.0f) : ImVec2(v.body0.x + gut, v.body1.y);
  v.bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
  // The floor: a little lighter than the window, in dark themes toward the text, in light ones toward white.
  v.stage = P.dark ? theme::mix(v.bg, P.text, 0.045f) : theme::mix(v.bg, P.paper, 0.6f);
  v.tray = theme::mix(v.stage, P.text, 0.05f);
  // K-224: the rooms list down the left side; the stage and its panel take the rest.
  v.fullW = v.body1.x - v.body0.x, v.fullH = v.body1.y - v.body0.y, v.fullX = v.body0.x;
  if (L.drag.mode != Drag::Move) v.dropRoom = -1;
  // K-270: closed it keeps a thin edge with its tab, half way down the stage (clear of the panel under it).
  if (const float rw = railWidth(a); rw > 0) {
    drawRail(a, v.body0, ImVec2(v.body0.x + rw, v.body1.y), std::round((v.body0.y + (panel ? v.panel0.y : v.body1.y)) * 0.5f));
    v.body0.x += rw;
    v.panel0.x += rw;
  }
  roomPopups(a);
  const std::vector<size_t> shown = shownTracks(a);
  // While a track is dragged the stage keeps the size it had when the drag began (it never jumps
  // under the pointer).
  const bool held = L.drag.mode != Drag::None;
  if (!held) v.trayOnFloor = false;
  const float stageY1 = panel ? v.panel0.y - 12 : v.body1.y - 8;
  v.g = st::layout(v.body0.x, v.body0.y, v.body1.x, stageY1, held ? v.g.scale : 1.0f, static_cast<int>(shown.size()));
  buildToks(a, shown);
  feedAnalysis(a);
  buildGroups(a);
  // K-270: on a short stage a group's tray can find no clear place: under its spot it hangs over the
  // front edge onto the listener, above it covers the next row's tiles and names. Each step tries the
  // trays kept on the floor, then lays the stage out a little smaller (down to 55 %), until nothing
  // overlaps; a stage with room for its trays (1920x1080) is never touched.
  auto crowded = [] { return traysCrowdListener() || traysCoverTiles(); };
  for (float sc = 1.0f; !held && crowded();) {
    if (!v.trayOnFloor) {
      v.trayOnFloor = true;  // first: the trays on the floor, above its front edge
      buildGroups(a);
      if (!crowded()) break;
    }
    if (sc * 0.9f < 0.55f) break;
    sc *= 0.9f;
    v.trayOnFloor = false;
    v.g = st::layout(v.body0.x, v.body0.y, v.body1.x, stageY1, sc, static_cast<int>(shown.size()));
    buildToks(a, shown);
    buildGroups(a);
  }
  if (!held && traysCrowdListener() && !v.trayOnFloor) {  // never over the listener, whatever else
    v.trayOnFloor = true;
    buildGroups(a);
  }
  // Hover and gestures first (the drawing shows this frame's state), then the picture.
  stageInput(a);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(v.body0, ImVec2(v.body1.x, v.panel0.y - 1), true);
  drawFloor(a, dl);
  if (v.preview.active()) {
    drawPreview(a, dl, v.preview, false);
  } else {
    drawSingles(a, dl);
    drawLabels(a, dl);
    drawTrays(a, dl);
    if (v.hoverPreview.active() && v.hoverPresetFrame >= ImGui::GetFrameCount() - 1) drawPreview(a, dl, v.hoverPreview, true);
    drawDrag(a, dl);
    drawBand(a, dl);
  }
  drawListener(a, dl);
  if (v.toks.empty())
    ui::EmptyState(dl, ImVec2(v.body0.x, v.body0.y + 40), ImVec2(v.body1.x, v.g.frontY - 40), i18n::tr("No tracks on the stage yet"),
                   i18n::tr("Route a channel to a mixer track, or show every insert track from the stage menu."));
  dl->PopClipRect();
  drawRailTab(a);
  if (panel) drawPanel(a);
  if (!v.preview.active() && !a.settings.getBool("ui.stage.welcomed") && !v.toks.empty()) firstRunCard(a);
  else v.cardOpen = false;
  if (v.wantFx) {
    ImGui::OpenPopup("##stagefx");
    v.wantFx = false;
  }
  if (ImGui::BeginPopup("##stagefx")) {
    effectsPopover(a, v.fxTrack);
    ImGui::EndPopup();
  }
  if (L.focused) keyboard(a);
  ImGui::SetCursorScreenPos(ImVec2(c0.x, v.body1.y - 1));
  ImGui::Dummy(ImVec2(1, 1));
}

void devCommand(App& a, int frame, const std::vector<std::string>& args) {
  const Song& s = a.doc.song();
  View& v = gView;
  if (!args.empty()) {
    const std::string& c = args[0];
    const std::string a1 = args.size() > 1 ? args[1] : "";
    if (c == "select") {
      a.mixerView.sel.clear();
      size_t at = 0;
      while (at <= a1.size()) {
        const size_t comma = a1.find(',', at);
        const int i = std::atoi(a1.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str());
        if (i >= 0 && static_cast<size_t>(i) < s.mixer.size()) {
          a.mixerView.sel.insert(s.mixer[static_cast<size_t>(i)].id);
          if (at == 0) a.selMixer = s.mixer[static_cast<size_t>(i)].id;
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
    } else if (c == "none") {
      a.mixerView.sel.clear();
      a.selMixer = s.masterId();
    } else if (c == "preview") {
      startPreview(a, a1 == "spread" ? PreviewKind::Spread : a1 == "sound" ? PreviewKind::Sound : PreviewKind::Preset,
                   args.size() > 2 ? std::atoi(args[2].c_str()) : 0);
    } else if (c == "apply") {
      applyPreview(a);
    } else if (c == "room") {
      std::vector<ObjectId> ids;
      for (const Tok& t : v.toks) ids.push_back(t.id);
      mixops::setRoomType(a.doc, std::atoi(a1.c_str()), ids, shownRoom(a));
    } else if (c == "solo") {
      const int i = std::atoi(a1.c_str());
      if (i > 0 && static_cast<size_t>(i) < s.mixer.size()) a.doc.editMixerTrack(s.mixer[static_cast<size_t>(i)].id, "solo mixer track", [](MixerTrack& m) { m.solo = true; });
    } else if (c == "mute") {
      const int i = std::atoi(a1.c_str());
      if (i > 0 && static_cast<size_t>(i) < s.mixer.size()) a.doc.setParam(mixerParam(s.mixer[static_cast<size_t>(i)].id, MixerParam::Mute), 1.0, false);
    } else if (c == "roomnew") {  // K-224: a new room from the shown one ("roomnew [name]"), shown
      const int i = roomops::addRoom(a.doc, shownRoom(a), nullptr, a1);
      if (i >= 0) showRoom(a, i);
    } else if (c == "roomorder" && args.size() > 2) {  // "roomorder FROM TO": as the room menu's Move up / down
      moveRoomKeepShown(a, std::atoi(a1.c_str()), std::atoi(args[2].c_str()));
    } else if (c == "roomshow") {
      showRoom(a, std::atoi(a1.c_str()));
    } else if (c == "roommove" && args.size() > 2) {  // "roommove <room> <mixer indexes, commas>"
      std::vector<ObjectId> ids;
      for (size_t at = 0; at <= args[2].size();) {
        const size_t comma = args[2].find(',', at);
        const int i = std::atoi(args[2].substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str());
        if (i > 0 && static_cast<size_t>(i) < s.mixer.size()) ids.push_back(s.mixer[static_cast<size_t>(i)].id);
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
      a.doc.endGesture();
      roomops::moveTracksToRoom(a.doc, ids, std::atoi(a1.c_str()));
      a.doc.endGesture();
    } else if (c == "roomtemplate" && args.size() > 2) {  // "roomtemplate <room> <factory index>"
      const auto& f = roomops::factoryTemplates();
      const int k = std::atoi(args[2].c_str());
      if (k >= 0 && static_cast<size_t>(k) < f.size()) roomops::applyTemplate(a.doc, std::atoi(a1.c_str()), f[static_cast<size_t>(k)]);
    } else if (c == "bring") {
      openBring(a, shownRoom(a));
    } else if (c == "listen" || c == "roomsolo") {  // "roomsolo <room> [only]": its headphones (click, right-click); "listen off": none
      if (a1 == "off" || a1.empty()) {
        v.soloRooms.clear();
        a.forceRebuild();
      } else if (const int r = std::atoi(a1.c_str()); r >= 0 && r < roomops::roomCount(s)) {
        const ObjectId id = roomops::roomIdAt(s, r);
        const auto at = std::find(v.soloRooms.begin(), v.soloRooms.end(), id);
        if (args.size() > 2 && args[2] == "only") {
          if (v.soloRooms.size() == 1 && at != v.soloRooms.end()) v.soloRooms.clear();
          else v.soloRooms.assign(1, id);
        } else if (at != v.soloRooms.end()) {
          v.soloRooms.erase(at);
        } else {
          v.soloRooms.push_back(id);
        }
        a.forceRebuild();
      }
    } else if (c == "roomdelete") {  // as the room menu's Delete room...: refused with the reason, or the confirm opens
      requestDeleteRoom(a, std::atoi(a1.c_str()));
    } else if (c == "rooms") {  // "rooms open|close" (at once, no slide), "rooms width <px>": the rooms column
      if (a1 == "width" && args.size() > 2) setRailWidth(a, static_cast<float>(std::atof(args[2].c_str())));
      else setRoomsPanelOpen(a, a1 != "close", false);
    } else if (c == "demorooms") {  // screenshots: 14 named, placed tracks in three rooms, one per song
      static const char* names[] = {"Kick", "Snare", "Hats", "Bass", "Keys", "Pad", "Lead", "Vocal", "Choir", "Strings", "Guitar", "Perc", "Brass", "FX"};
      static const Rgb cols[] = {0xD9695F, 0xE0A458, 0xD8C25A, 0x7FB069, 0x5FA8D3, 0x8E7DD6, 0xC77DBA, 0xE07FA2, 0x6DBBA6, 0xA3B565, 0xD08C60, 0x9AA5B1, 0xCFA15B, 0x7A9CC6};
      static const float pans[] = {0, 0.1f, 0.35f, 0, -0.45f, 0.5f, -0.25f, 0, -0.6f, 0.6f, -0.35f, 0.4f, 0.2f, -0.15f};
      static const float deps[] = {0, 0.25f, 0.25f, 0, 0.5f, 0.75f, 0.25f, 0, 0.75f, 0.5f, 0.5f, 0.5f, 0.75f, 1.0f};
      a.doc.beginGroup("demo");
      std::vector<ObjectId> all;
      for (size_t k = 0; k < 14 && k + 1 < s.mixer.size(); ++k) {
        const ObjectId id = a.doc.song().mixer[k + 1].id;
        all.push_back(id);
        a.doc.editMixerTrack(id, "demo", [k](MixerTrack& m) {
          m.name = names[k];
          m.color = cols[k];
        });
        a.doc.setParam(mixerParam(id, MixerParam::Pan), pans[k], false);
        a.doc.setParam(mixerParam(id, MixerParam::Depth), deps[k], false);
      }
      a.doc.endGroup();
      mixops::setRoomType(a.doc, rooms::kStudio, all, 0);
      roomops::renameRoom(a.doc, 0, "Opening");
      const int r1 = roomops::addRoom(a.doc, 0, &roomops::factoryTemplates()[3], "Glass Coast", 0xE07FA2);
      const int r2 = roomops::addRoom(a.doc, 0, &roomops::factoryTemplates()[2], "Night Drive", 0x6DBBA6);
      roomops::moveTracksToRoom(a.doc, {all.begin() + 8, all.begin() + 12}, r1);
      roomops::moveTracksToRoom(a.doc, {all.begin() + 12, all.end()}, r2);
      a.doc.endGesture();
      a.settings.set("ui.stage.welcomed", true);
    } else if (c == "open") {
      if (a1 == "presets") L.openPresets = true;
      else if (a1 == "room") L.openRoom = true;
      else if (a1 == "fx") openEffects(a, a.selMixer);
    }
    return;
  }
  std::string sel;
  for (ObjectId id : mixerSelection(a)) sel += (sel.empty() ? "" : ",") + std::to_string(s.mixerIndex(id));
  std::printf("stage frame %d: open %d undo %zu sel %s focus %d shown %zu groups %zu preview %d stage %.1f %.1f %.1f %.1f panel %.1f hover %d drag %d mouse %.0f %.0f active %u\n", frame,
              v.open && v.frame >= ImGui::GetFrameCount() - 2 ? 1 : 0, a.doc.history().position(), sel.empty() ? "-" : sel.c_str(),
              s.mixerIndex(a.selMixer), v.toks.size(), v.groups.size(), static_cast<int>(v.preview.kind), static_cast<double>(v.g.cx - v.g.frontHalf),
              static_cast<double>(v.g.backY), static_cast<double>(v.g.cx + v.g.frontHalf), static_cast<double>(v.g.frontY), static_cast<double>(v.panel0.y),
              v.hovered == kNoId ? -1 : s.mixerIndex(v.hovered), static_cast<int>(L.drag.mode), static_cast<double>(ImGui::GetIO().MousePos.x),
              static_cast<double>(ImGui::GetIO().MousePos.y), static_cast<unsigned>(ImGui::GetActiveID()));
  for (const Tok& t : v.toks) {
    const MixerTrack* m = s.mixerTrack(t.id);
    std::printf("  token %zu x %.2f y %.2f side %.2f pan %.4f depth %.4f volume %.4f sep %.4f focus %.4f muted %d solo %d group %d cell %d\n", t.index,
                static_cast<double>((t.r.x0 + t.r.x1) * 0.5f), static_cast<double>((t.r.y0 + t.r.y1) * 0.5f), static_cast<double>(t.r.w()),
                m ? static_cast<double>(m->pan) : 0.0, m ? static_cast<double>(m->depth) : 0.0, m ? static_cast<double>(m->volume) : 0.0,
                m ? static_cast<double>(m->stereoSep) : 0.0, m ? static_cast<double>(m->focus) : 0.0, m && m->muted ? 1 : 0, m && m->solo ? 1 : 0, t.group, t.cell);
  }
  for (size_t gi = 0; gi < v.groups.size(); ++gi) {
    const Grp& gr = v.groups[gi];
    std::printf("  group %zu count %zu box %.1f %.1f %.1f %.1f ring %.1f %.1f more %d\n", gi, gr.toks.size(), static_cast<double>(gr.tray.box.x0),
                static_cast<double>(gr.tray.box.y0), static_cast<double>(gr.tray.box.x1), static_cast<double>(gr.tray.box.y1),
                static_cast<double>(gr.ring.x), static_cast<double>(gr.ring.y), gr.tray.more);
  }
  const st::Rect tab = railTab();
  std::printf("  rooms %d shown %d listen %zu drop %d band %d rail %d menu %d slim %d width %.0f tab %.0f %.0f anim %.2f\n", roomops::roomCount(s), shownRoom(a),
              v.soloRooms.size(), v.dropRoom, L.drag.mode == Drag::Band && L.drag.bandLive ? 1 : 0, railShown(a) ? 1 : 0, railShown(a) ? railMenuRoom() : -1,
              railSlim(a) ? 1 : 0, static_cast<double>(railFits(a) ? v.body0.x - v.fullX : 0.0f), static_cast<double>((tab.x0 + tab.x1) * 0.5f),
              static_cast<double>((tab.y0 + tab.y1) * 0.5f), static_cast<double>(railAnim()));
  const st::Rect lis = listenerRect(v.g);
  std::printf("  listener %.0f %.0f %.0f %.0f scale %.2f front %.0f\n", static_cast<double>(lis.x0), static_cast<double>(lis.y0), static_cast<double>(lis.x1),
              static_cast<double>(lis.y1), static_cast<double>(v.g.scale), static_cast<double>(v.g.frontY));
  for (int r = 0; r < roomops::roomCount(s); ++r) {
    std::string ids;
    for (ObjectId id : roomops::roomTracks(s, r)) ids += (ids.empty() ? "" : ",") + std::to_string(s.mixerIndex(id));
    const MixerTrack* sp = mixops::stageRoom(s, r);
    const st::Rect row = static_cast<size_t>(r) < railRows().size() ? railRows()[static_cast<size_t>(r)] : st::Rect{0, 0, 0, 0};
    const st::Rect ph = static_cast<size_t>(r) < railPhones().size() ? railPhones()[static_cast<size_t>(r)] : st::Rect{0, 0, 0, 0};
    const ObjectId rid = roomops::roomIdAt(s, r);
    const bool solo = std::find(v.soloRooms.begin(), v.soloRooms.end(), rid) != v.soloRooms.end();
    std::printf("  room %d [%s] tracks %s space %d row %.0f %.0f %.0f %.0f solo %d used %zu phones %.0f %.0f\n", r, roomops::roomName(s, r).c_str(),
                ids.empty() ? "-" : ids.c_str(), sp ? s.mixerIndex(sp->id) : -1, static_cast<double>(row.x0), static_cast<double>(row.y0),
                static_cast<double>(row.x1), static_cast<double>(row.y1), solo ? 1 : 0, roomops::roomUsedTracks(s, r).size(),
                static_cast<double>((ph.x0 + ph.x1) * 0.5f), static_cast<double>((ph.y0 + ph.y1) * 0.5f));
  }
  std::fflush(stdout);
}

}  // namespace gl::stageui
