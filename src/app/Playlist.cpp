// Gridline — playlist: pattern, audio and automation clips arranged on tracks
// (song mode), song markers in the ruler.
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include "LoopNotch.h"
#include "CollabUi.h"
#include "CursorAnchor.h"
#include "../core/I18n.h"
#include "Keys.h"
#include "ExportUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"  // GetActiveID (K-112: the audio clip name field)
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/ClipGain.h"
#include "../core/ClipMultiEdit.h"
#include "../core/ClipRate.h"
#include "ClipHeaderHit.h"
#include "ClipMultiUi.h"
#include "../core/Fades.h"
#include "../core/ClipPlayback.h"
#include "../core/TrackIconSet.h"
#include "../core/TrackModes.h"
#include "../core/TrackOrder.h"
#include "../core/MenuOps.h"
#include "../core/MenuOpsExtra.h"
#include "../core/MenuParity.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "../core/SmoothSnap.h"
#include "../engine/ClipOps.h"
#include "../engine/ClipSource.h"
#include "../engine/PlaylistTools.h"
#include "../engine/Sample.h"
#include "FreezeUi.h"
#include "MixerMeters.h"
#include "App.h"
#include "SmoothScrollUi.h"
#include "../core/Grouping.h"
#include "J31Ui.h"
#include "ExternalDragUi.h"
#include "LiveLook.h"
#include "LiveTakes.h"
#include "PlaylistWarp.h"
#include "PixelGrid.h"
#include "../core/NoteTools.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "../packs/PacksUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LogicNewTracks.h"
#include "LogicPlugins.h"
#include "Theme.h"
#include "TrackerPages.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"
#include "TrackIcons.h"
#include "TrackModeUi.h"
#include "ChopUi.h"
#include "ContextMenus.h"
#include "../engine/Chop.h"
#include "../engine/TakeLanes.h"
#include "../engine/Dsp.h"
#include "../core/ClipLoop.h"
#include "../core/ClipSources.h"
#include "../core/PatternAutoOps.h"
#include "../core/PatternAutomation.h"
#include "PatternAutoUi.h"
#include "RecordRowsUi.h"
#include "MeterUi.h"
#include "../core/Meter.h"
#include "../core/TrackPlus.h"
#include "TrackPlusUi.h"

namespace gl {

using theme::pal;

namespace {
// A clip's title face: regular in Live, as the refs' arrangement clips (019, 141, 093); bold elsewhere.
ImFont* clipTitleFont(App& a) {
  ImFont* f = livelook::active(a) ? a.fontRegular : a.fontBold;
  return f ? f : ImGui::GetFont();
}
}  // namespace

namespace {

// K-24: the vertical zoom: the base track height's range, one Alt+wheel
// notch's factor, and the scroller thumbs' shortest lengths (their ends are
// zoom handles).
constexpr float kTrackHMin = 24.0f, kTrackHMax = 120.0f;
constexpr double kRowZoomStep = 1.1;
constexpr float kThumbMinV = 18.0f, kThumbMinH = 24.0f;

struct ClipGesture {
  enum Kind { None, Move, Resize, Trim, Select, Paint, Erase, Scrub, TimeSel, AutoPoint, AutoTension, Mute, Slip, Play, Zoom, SliceLine, MarkerDrag, Warp, Marquee } kind = None;
  Tick insertMarker = -1;  // N47-08: Clips: a click in a clip's body put the insert marker here
  // J-85: a marker dragged in the ruler (moved on release; a click jumps to it),
  // and a right-click in the ruler that did not drag (opens the ruler menu).
  int marker = -1;
  Tick markerTo = 0;
  bool rulerMenu = false;
  // R02-12: the Tracks layout's cycle area in the ruler: -1 none, 0 a new one dragged out, 1 the
  // area moved, 2 / 3 its left / right edge dragged. cycleA / cycleB: the area at the start.
  int cycleMode = -1;
  Tick cycleA = 0, cycleB = 0;
  int muteTo = -1;     // mute tool: state given to every touched clip
  bool soloHeld = false;  // R05-17: the Solo tool: the track's strip soloed while held (its group is dropped on release)
  Tick slipBase = 0;   // slip tool: the clip's offset at the start
  // Paint: clips of `paintSrc` laid back to back from `paintAnchor`.
  Tick paintAnchor = 0, paintLen = 0;
  ClipKind paintKind = ClipKind::Pattern;
  ObjectId paintId = kNoId;
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
  // Q-3: the selection before the gesture (a cancelled drag restores it);
  // cancelled: the drag was cancelled, the button is still held.
  std::set<ObjectId> selBefore;
  bool cancelled = false;
  bool copyPending = false;  // Q-3: Shift+drag — copies are made when the pointer first moves
  bool placed = false;       // the click just placed this clip: it follows the pointer at once
  bool copied = false;       // the dragged clips are copies
  bool autoLoops = false;    // K-114: the loop drag gave automation curves a loop length
  std::vector<ObjectId> autoLooped;  // K-140: those curves (back at the cut point, they lose it again)
  bool unique = false;       // Q-3: the copies were made unique (Shift+V)
  pltools::EraseSweep sweep;  // J-79: delete sweep: only the topmost clip at each point
};
ClipGesture cg;
Tick rulerMenuAt = 0;

// K-114: the key that makes an edge drag loop the clip (Ctrl; Cmd on macOS,
// where Ctrl+click is the right-click).
#ifdef __APPLE__
const char* const kLoopKey = "Cmd";
#else
const char* const kLoopKey = "Ctrl";
#endif

// K-140: during a loop-key edge drag the edge's cut point (where it was when the
// drag started) is a snap target: within a few pixels of the pointer, or nearer
// than the grid line it would snap to, the edge lands exactly on it (the clip is
// then exactly as it was).
Tick snapToCutPoint(Tick pointer, Tick snapped, Tick cut, float pxPerTick) {
  const Tick far = std::llabs(pointer - cut);
  const float magnet = std::max(6.0f, ImGui::GetFontSize() * 0.6f);
  if (static_cast<float>(far) * pxPerTick <= magnet || far <= std::llabs(pointer - snapped)) return cut;
  return snapped;
}

// K-140: whether a loop-key drag of a clip's right (or left) edge loops it: its
// content is there (a pattern, audio that has loaded, a curve with points), and
// a left edge can move out (the clip loops already, or does not start the song).
bool edgeCanLoop(const Song& s, const Clip& c, bool right, const TempoMap& tm) {
  if (c.kind == ClipKind::Pattern && !s.pattern(c.pattern)) return false;
  if (c.kind == ClipKind::Automation) {
    const AutomationClip* ac = s.automation(c.source);
    if (!ac || ac->points.empty()) return false;
  }
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    if (!ch || !sampleCache().request(ch->plugin.data) || audioClipNaturalLength(s, c) <= 0) return false;
    if (c.loop && audioLoopLength(s, c, tm) <= 0) return false;
  }
  return right || c.loop || c.start > 0;
}
// K-140: during a loop-key edge drag, whether the anchor clip loops now: it did
// already, or its edge is out past the cut point. Before the pointer moves, as
// over the edge.
bool edgeDragLoops(const Song& s, const std::vector<Clip>& base, ObjectId anchor, bool right, bool moved, const TempoMap& tm) {
  const Clip* b = nullptr;
  for (const Clip& c : base)
    if (c.id == anchor) b = &c;
  const Clip* now = s.clip(anchor);
  if (!b || !now) return false;
  if (!moved) return edgeCanLoop(s, *b, right, tm);
  if (!now->loop) return false;
  if (b->loop) return true;
  return right ? now->start + s.clipLength(*now) > b->start + s.clipLength(*b) : now->start < b->start;
}
// K-140: the loop badge at the pointer: a round accent tag below and right of
// the pointer, clear of the resize cursor's arrows, with the loop arrows large
// enough to read (they blurred into a ring at 8 px). Whole-pixel geometry at
// every UI scale; the accent fill reads on the dark and the light grid alike.
ImVec2 drawLoopBadge(ImDrawList* fg, ImVec2 m, const theme::Palette& P) {
  const float fs = ImGui::GetFontSize();
  const float R = std::round(std::max(9.0f, fs * 0.72f));  // badge radius (10 px at the default size)
  const ImVec2 at(std::round(m.x + fs * 0.85f + R), std::round(m.y + fs * 0.6f + R));  // on a pixel corner: the glyph box lands on whole pixels
  fg->AddCircleFilled(ImVec2(at.x, at.y + 1.0f), R + 1.0f, IM_COL32(0, 0, 0, P.dark ? 90 : 50), 40);  // soft drop shadow
  fg->AddCircleFilled(at, R, P.hot, 40);
  fg->AddCircle(at, R - 0.5f, theme::withAlpha(theme::contrastText(P.hot), 0.25f), 40, 1.0f);
  ui::drawIconGlyph(fg, ui::Icon::LoopArrows, at, std::round(R * 0.7f), 1.5f, theme::contrastText(P.hot));
  return at;
}

// K-114: the clips an edge drag works from this frame: as they were when it
// started, or with the loop key held, the selected ones looping (automation
// curves without a loop length get one, once per drag, in its undo step).
// K-140: `loop` is false while the edge is at or inside its cut point (where the
// drag started): the clips are plain there, as they were, with no loop length
// left on their curves.
std::vector<Clip> loopDragBase(App& a, const std::vector<Clip>& base, const std::set<ObjectId>& sel, bool loop, const TempoMap& tm) {
  if (!loop) {
    if (cg.autoLoops && !cg.autoLooped.empty())
      a.doc.editSong("loop automation", [&](Song& x) {
        for (ObjectId id : cg.autoLooped)
          if (AutomationClip* ac = x.automation(id)) ac->loop = 0;
      });
    cg.autoLoops = false;
    cg.autoLooped.clear();
    return base;
  }
  if (!cg.autoLoops) {
    cg.autoLoops = true;
    std::vector<ObjectId> before;
    for (const AutomationClip& ac : a.doc.song().automations)
      if (ac.loop <= 0) before.push_back(ac.id);
    bool need = false;
    for (const Clip& c : base)
      if (sel.count(c.id) && c.kind == ClipKind::Automation)
        if (const AutomationClip* ac = a.doc.song().automation(c.source); ac && ac->loop <= 0) need = true;
    if (need) a.doc.editSong("loop automation", [&](Song& x) { prepareAutomationLoops(x, base, sel); });
    for (ObjectId id : before)
      if (const AutomationClip* ac = a.doc.song().automation(id); ac && ac->loop > 0) cg.autoLooped.push_back(id);
  }
  std::vector<Clip> out = base;
  const Song& s = a.doc.song();
  for (Clip& c : out)
    if (sel.count(c.id)) c = withClipLoop(s, tm, c, true);
  return out;
}

// K-114: the status bar while the right edge is dragged.
std::string resizeHint(const Song& s, const std::vector<Clip>& from, const std::vector<Clip>& after, ObjectId anchor, Tick d, bool loopKey) {
  const Clip* f = nullptr;
  const Clip* t = nullptr;
  for (const Clip& c : from)
    if (c.id == anchor) f = &c;
  for (const Clip& c : after)
    if (c.id == anchor) t = &c;
  const std::string key(kLoopKey);
  if (!f || !t) return {};
  if (loopKey && !f->loop && d <= 0)  // K-140: at or inside the cut point the clip is plain
    return d == 0 ? std::string(i18n::tr("At the cut point: the clip is as it was, it does not loop (drag further out to loop it)"))
                  : std::string(i18n::tr("Shorter than the cut point: the clip does not loop (drag past it to loop)"));
  if (loopKey && !f->loop) return i18n::tr("Looping from the cut point: the part the clip showed repeats as far as you drag") + std::string("  (") + i18n::tr("back to the cut point: no loop") + ")";
  if (loopKey) return i18n::tr("Looping: the content repeats as far as you drag") + std::string("  (") + i18n::tr("release ") + key + i18n::tr(" to extend instead)");
  if (f->loop && !t->loop) return i18n::tr("Back to its own length: the clip stops looping (drag further out to keep the loop)");
  if (f->loop) return i18n::tr("The clip loops: dragging adds or removes repeats (back to its own length, it stops looping)");
  if (t->kind == ClipKind::Audio && d > 0 && s.clipLength(*t) < s.clipLength(*f) + d)
    return i18n::tr("The audio ends here") + std::string(": ") + i18n::tr("hold ") + key + i18n::tr(" to loop it");
  return i18n::tr("Extending") + std::string("  (") + i18n::tr("hold ") + key + i18n::tr(" to loop instead)");
}

// K-114: the status bar over a clip's edge (Draw tool).
std::string loopEdgeHint(const Clip& c, bool right, bool loopKey, bool stretch) {
  const std::string key(kLoopKey);
  if (!right)
    return loopKey ? std::string(i18n::tr("(drag out: loop the part the clip shows, the edge sets where the loop starts · back to the cut point: no loop)"))
                   : std::string(i18n::tr("(drag: trim the start · ")) + key + i18n::tr("+drag: loop the clip and set where its loop starts)");
  if (loopKey) return std::string(i18n::tr("(drag out: loop the part the clip shows as far as you drag · back to the cut point: no loop · release ")) + key + i18n::tr(" to extend instead)");
  if (c.loop) return std::string(i18n::tr("(drag: more or fewer repeats, the clip loops · back to its own length, it stops looping)"));
  const char* plain = c.kind == ClipKind::Audio ? (stretch ? i18n::tr("(drag: stretch the audio · ") : i18n::tr("(drag: extend, up to the end of the audio · "))
                      : c.kind == ClipKind::Automation ? i18n::tr("(drag: extend, the last value holds · ")
                                                       : i18n::tr("(drag: extend, empty past the pattern's end · ");
  return std::string(plain) + key + i18n::tr("+drag: loop instead)");
}  // J-85: position of the ruler menu (snapped)
uint64_t clipGestureCounter = 1;

// J-37: smooth snapping. Drawn geometry of clips under a snapped drag:
// start tick, end tick, track (fractional while gliding between rows) and
// content position (offset in ticks; audio clips: source start in seconds).
// The document keeps the exact snapped values; only the drawing eases.
smooth::Tracker<4> clipAnim;
// PL-13: the slice tool's cut line(s) glide between grid lines the same way
// (key 1: the hover / start line, key 2: the far end of a region cut).
smooth::Tracker<1> sliceAnim;
smooth::Tracker<4>::Vec clipAnimValues(const Clip& c, Tick len) {
  return {static_cast<double>(c.start), static_cast<double>(c.start + len), static_cast<double>(c.track),
          c.kind == ClipKind::Audio && !c.loop ? c.audioStart : static_cast<double>(c.offset)};  // (K-114: a loop's start glides)
}
// J-79: stretch mode resizes audio clips by time-stretching them with the
// pitch kept: channels of the dragged audio clips that resample (pitch
// follows speed) switch to their pitch-keeping stretch (part of the drag's
// undo step).
void keepPitchWhenStretching(App& a, const std::vector<Clip>& base, const std::set<ObjectId>& sel) {
  const std::set<ObjectId> chans = pltools::resamplingChannels(a.doc.song(), base, sel);
  if (chans.empty()) return;
  a.doc.editSong("stretch keeps the pitch", [&](Song& x) { pltools::keepPitch(x, chans); });
  a.setHint(i18n::tr("Stretching keeps the pitch: the clip's channel now time-stretches"));
}

// J-79: the audio clip editing toggles are global settings (FL keeps its
// toolbar switches between projects): loaded once, written when any path
// (corner cells, shortcuts, menus) changes them.
void syncEditToggles(App& a) {
  static bool loaded = false;
  static bool last[3] = {false, true, false};
  PlaylistState& st = a.playlist;
  static const char* const kKeys[3] = {"playlist.stretchResize", "playlist.showFades", "playlist.zeroCross"};
  bool* vals[3] = {&st.stretchResize, &st.showFades, &st.zeroCross};
  for (int i = 0; i < 3; ++i) {
    if (!loaded) *vals[i] = a.settings.getBool(kKeys[i]);
    else if (*vals[i] != last[i]) a.settings.set(kKeys[i], *vals[i]);
    last[i] = *vals[i];
  }
  // J-173: what pattern clips show (notes and automation / notes / automation)
  static int lastPreview = 0;
  if (!loaded) st.patternPreview = std::clamp(static_cast<int>(a.settings.getNumber("playlist.patternPreview")), 0, 2);
  else if (st.patternPreview != lastPreview) a.settings.set("playlist.patternPreview", st.patternPreview);
  lastPreview = st.patternPreview;
  loaded = true;
}

bool gestureAnimates(ClipGesture::Kind k) {
  return k == ClipGesture::Move || k == ClipGesture::Resize || k == ClipGesture::Trim || k == ClipGesture::Slip || k == ClipGesture::Paint;
}

// J-118: an audio clip's fades along x as drawn (each fade's span in screen
// px, the curve of fadePath below).
struct FadeGeom {
  float inXa = 0, inXb = 0, outXa = 0, outXb = 0;
  EdgeFade in, out;
  // Level (0..1) at x from the edges that crossfade with another clip (other
  // edges and the rest of the clip: 1), the curve the clip plays.
  float xfGain(float x) const {
    double g = 1.0;
    if (in.crossfade && in.len > 0 && inXb - inXa > 0.5f && x < inXb)
      g *= fadeGain(std::clamp(static_cast<double>((x - inXa) / (inXb - inXa)), 0.0, 1.0), in.shape, in.tension);
    if (out.crossfade && out.len > 0 && outXb - outXa > 0.5f && x > outXa)
      g *= fadeGain(std::clamp(static_cast<double>((outXb - x) / (outXb - outXa)), 0.0, 1.0), out.shape, out.tension);
    return static_cast<float>(std::clamp(g, 0.0, 1.0));
  }
};

// Clip rectangles drawn this frame, in drawing (stacking) order (J-37 / J-38).
struct DrawnClip {
  int track = 0;
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0, body = 0;  // body: top of the body (below the title bar)
  size_t ci = 0;                                  // playlist index
  ImU32 rim = 0;
  bool sel = false;
  // J-118: what a crossfade partner lying on this clip needs to redraw its
  // part of the overlap (the clip as drawn, its colours and fades).
  Clip clip;
  float clipX0 = 0;  // x of the clip start (waveform origin)
  ImU32 bodyCol = 0, headCol = 0, ink = 0;
  FadeGeom fade;
  bool audio = false, glide = false;
  int xfBelow = -1;  // index (drawn list) of the clip this one crossfades on, blended (-1: none)
  // K-26: what a clip lying on this one needs to show it through its body
  // (its length and pattern loop as drawn).
  Tick len = 0, loop = 0, loopFrom = 0;  // (K-140: a cut clip's own loop starts at loopFrom)
};

// Notes (pitch-scaled) from every channel, then steps as ticks at the bottom,
// drawn pass by pass: the clip shows pattern ticks [offset, offset + clipLen),
// and a looping pattern (loop > 0, Q-1) repeats them. Returns the content end
// (clip ticks) of a pattern that does not loop and ends inside the clip, else -1.
Tick drawClipPreview(ImDrawList* dl, const Song& s, const Pattern& p, ImVec2 a, ImVec2 b, Tick clipLen, Tick offset, Tick loop, Tick loopFrom,
                     ImU32 col, float visX0, float visX1, bool notes = true) {
  const Tick patLen = std::max<Tick>(1, s.patternContentLength(p));
  const float w = b.x - a.x;
  if (w < 6 || clipLen <= 0) return -1;
  int lo = 127, hi = 0;
  for (const auto& [ch, ns] : p.notes)
    for (const auto& n : ns) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  const float h = b.y - a.y;
  const float span = static_cast<float>(std::max(hi - lo, 8));
  auto xAt = [&](Tick t) { return a.x + w * static_cast<float>(t) / static_cast<float>(clipLen); };  // clip ticks
  const Tick stepT = s.ticksPerStep(p);
  for (const ClipPass& ps : loopPasses(offset, clipLen, loop, loopFrom)) {
    if (!notes) break;  // J-173: automation only (the content end still counts)
    if (xAt(ps.at) > visX1) break;                                          // later passes start right of the view
    if (xAt(ps.at + ps.len) < visX0) continue;                              // this one ends left of it
    const Tick c0 = ps.content, c1 = ps.content + ps.len;                   // content shown by the pass
    auto xc = [&](Tick t) { return xAt(ps.at + (t - c0)); };
    for (const auto& [ch, ns] : p.notes)
      for (const auto& n : ns) {
        if (n.start >= c1 || n.start + n.length <= c0) continue;
        const float x0 = xc(std::max(n.start, c0)), x1 = xc(std::min(c1, n.start + n.length));
        if (std::max(x0 + 1.5f, x1) < visX0 || x0 > visX1) continue;  // outside the view
        const float y = a.y + 2 + (h - 6) * (1.0f - (n.pitch - lo) / span);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x0 + 1.5f, x1), y + 2), col);
      }
    if (stepT > 0) {
      int rowIdx = 0;
      for (const auto& [ch, row] : p.steps) {
        for (size_t k = 0; k < row.size(); ++k) {
          if (!row[k]) continue;
          const Tick t = static_cast<Tick>(k) * stepT;
          if (t >= c1) break;
          if (t < c0) continue;
          const float x = xc(t);
          if (x > visX1) break;
          if (x + 2 < visX0) continue;
          const float y = b.y - 3 - (rowIdx % 4) * 3.0f;
          dl->AddRectFilled(ImVec2(x, y - 2), ImVec2(x + 2, y), col);
        }
        ++rowIdx;
      }
    }
  }
  if (loop <= 0 && offset + clipLen > patLen) return std::max<Tick>(0, patLen - offset);
  return -1;
}

// J-173: the automation clips a pattern holds, drawn over its playlist clip's
// body as they play there (the clip's passes: offset, trim, loop repeats),
// one value per pixel column. Values are in the clip's own range (0..1 of its
// height), like playlist automation clips. `mouse` / `hitR`: the curve passing
// nearest the pointer within hitR px is returned in `hit` (-1: none); `sample`
// gets a point on each drawn curve (dev geometry).
struct PatCurveHit {
  int hit = -1;
  std::vector<std::pair<int, ImVec2>> sample;  // (clip index, point on its curve)
};
// K-112c (Live): a clip's content drawn dark on its full-colour body.
ImU32 liveClipInk(ImU32 base, bool muted) { return theme::withAlpha(theme::mix(base, IM_COL32(16, 18, 22, 255), muted ? 0.55f : 0.72f), 0.95f); }

ImU32 patternCurveColor(const AutomationClip& c, size_t k, ImU32 body) {
  return theme::mix(theme::rgb(patautoui::curveRgb(c, k)), theme::contrastText(body), 0.22f);
}
void drawPatternCurves(ImDrawList* dl, const Pattern& p, ImVec2 a, ImVec2 b, Tick clipLen, Tick offset, Tick loop, Tick loopFrom, ImU32 body, float visX0,
                       float visX1, ImVec2 mouse, bool mouseIn, float hitR, PatCurveHit* out) {
  const float w = b.x - a.x, h = b.y - a.y;
  if (p.automation.empty() || w < 4 || h < 4 || clipLen <= 0) return;
  const std::vector<ClipPass> passes = loopPasses(offset, clipLen, loop, loopFrom);
  const float x0 = std::floor(std::max(a.x, visX0)), x1 = std::min(b.x, visX1);
  if (x1 <= x0 || passes.empty()) return;
  const float sx = std::round((x0 + x1) * 0.5f);
  const ImU32 shadow = IM_COL32(0, 0, 0, 90);
  float best = hitR;
  static std::vector<ImVec2> pts;
  for (size_t k = 0; k < p.automation.size(); ++k) {
    const AutomationClip& ac = p.automation[k];
    if (ac.points.empty() || ac.owner == kNoId) continue;
    const ImU32 col = patternCurveColor(ac, k, body);
    size_t pi = 0;
    pts.clear();
    auto flush = [&]() {
      if (pts.size() >= 2) {
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), shadow, ImDrawFlags_None, 3.0f);
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, 1.5f);
      }
      pts.clear();
    };
    for (float x = x0; x <= x1; x += 1.0f) {
      const double t = static_cast<double>(x - a.x) / static_cast<double>(w) * static_cast<double>(clipLen);  // clip ticks
      size_t q = pi;
      while (q + 1 < passes.size() && t >= static_cast<double>(passes[q + 1].at)) ++q;
      if (q != pi) {  // a new pass: the curve restarts (a loop point)
        flush();
        pi = q;
      }
      const ClipPass& ps = passes[pi];
      const double content = static_cast<double>(ps.content) + (t - static_cast<double>(ps.at));
      const double v = std::clamp(automationValue(ac, content), 0.0, 1.0);
      const float y = b.y - 1.0f - static_cast<float>(v) * (h - 2.0f);
      pts.emplace_back(x + 0.5f, y);
      if (out && mouseIn && std::fabs(mouse.x - (x + 0.5f)) <= 0.5f && std::fabs(mouse.y - y) < best) best = std::fabs(mouse.y - y), out->hit = static_cast<int>(k);
      if (out && x == sx) out->sample.push_back({static_cast<int>(k), ImVec2(x + 0.5f, y)});
    }
    flush();
  }
}

// ---- audio clip waveforms: min/max per block of frames, cached per sample ----
// (J-217: LiveTakes.h samplePeaks: a PeakSummary per sample; a take just
// recorded keeps the summary it was drawn with while recording.)

// Source seconds an audio clip plays at song tick t (display approximation of
// the engine's mapping: resample / stretch / tempo sync).
// The mapping's per-clip constants are computed once (the waveform evaluates
// it for every pixel column).
struct AudioMap {
  const TempoMap& tm;
  double audioStart = 0, start = 0, startSec = 0, syncScale = 0, speed = 1;
  bool sync = false;
  AudioMap(const Song& s, const Channel& ch, const Clip& c, const TempoMap& map, double from) : tm(map) {
    const ClipRate r = clipRate(ch, c);  // J-164: the engine's mapping (pitch never moves the waveform)
    audioStart = from;  // K-85: where its audio starts (inside the channel's start / end handles)
    start = static_cast<double>(c.start);
    sync = c.syncBpm > 0;
    if (sync) {
      syncScale = 60.0 / (c.syncBpm * s.ppq) / r.timeFactor;
      return;
    }
    speed = r.speed;
    startSec = tm.secondsAt(start);
  }
  // K-85: through the clip's warp markers (ClipSource.h clipWarp; empty: none).
  std::vector<WarpPoint> warp;
  double warpBase = 0;
  void setWarp(std::vector<WarpPoint> w) {
    warp = std::move(w);
    warpBase = warp.empty() ? 0.0 : warpTimeAt(warp, audioStart);
  }
  double at(double t) const {
    const double p = sync ? (t - start) * syncScale : (tm.secondsAt(t) - startSec) * speed;
    return warp.empty() ? audioStart + p : warpSourceAt(warp, warpBase + p);
  }
};

// J-118: with `xf`, each column's opacity follows the clip's crossfade level
// there (a waveform fading out under, or into, its crossfade partner).
// J-217: drawn from a summary of `frames` frames at `rate` (a decoded sample,
// a committed take whose file is still decoding, or a take still recording).
void drawWaveformFrom(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                      float pxPerTick, float visX0, float visX1, ImU32 col, const FadeGeom* xf, const PeakSummary& pk, size_t frames, double rate,
                      const ClipSource* src) {
  if (frames < 2 || pk.bins() == 0 || rate <= 0) return;
  theme::devNote("wave", col);  // K-177 dev: the waveform colour (from the theme's clip ink)
  const bool rev = clipPlaysReversed(ch, c);
  const double seconds = static_cast<double>(frames) / rate;
  // K-85: the window inside the channel's start / end handles (the sound its sample page makes).
  double srcStart = std::max(0.0, c.audioStart), srcEnd = c.audioEnd > c.audioStart ? std::min(c.audioEnd, seconds) : seconds;
  if (src && !clipWindow(*src, rev, c, srcStart, srcEnd)) return;
  const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f;
  const float x0 = std::max(a.x, visX0), x1 = std::min(b.x, visX1);
  AudioMap map(s, ch, c, tm, srcStart);
  if (src) map.setWarp(clipWarp(*src, rev, c));
  const double bin = static_cast<double>(pk.binFrames());
  for (float x = std::floor(x0); x < x1; x += 1.0f) {
    double t0 = static_cast<double>(c.start) + (x - clipX0) / pxPerTick, t1 = t0 + 1.0 / pxPerTick;
    double s0 = map.at(t0), s1 = map.at(t1);
    if (s0 >= srcEnd) break;
    s1 = std::min(s1, srcEnd);
    double f0 = s0 * rate, f1 = s1 * rate;
    if (rev) {
      double last = static_cast<double>(frames);
      std::swap(f0, f1);
      f0 = last - f0, f1 = last - f1;
    }
    long b0 = static_cast<long>(std::floor(std::min(f0, f1) / bin)), b1 = static_cast<long>(std::ceil(std::max(f0, f1) / bin));
    b0 = std::clamp<long>(b0, 0, static_cast<long>(pk.bins()) - 1);
    b1 = std::clamp<long>(std::max(b1, b0 + 1), 1, static_cast<long>(pk.bins()));
    float lo = 0, hi = 0;
    pk.range(static_cast<size_t>(b0), static_cast<size_t>(b1), lo, hi);
    const float g = c.gain;  // J-79: the waveform follows the clip's gain (louder clips draw taller, clipped at the body)
    const ImU32 cc = xf ? theme::withAlpha(col, 0.1f + 0.9f * xf->xfGain(x + 0.5f)) : col;
    dl->AddLine(ImVec2(x + 0.5f, mid - std::clamp(hi * g, -1.f, 1.f) * half), ImVec2(x + 0.5f, mid - std::clamp(lo * g, -1.f, 1.f) * half + 1), cc);
  }
}

void drawWaveform(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                  float pxPerTick, float visX0, float visX1, ImU32 col, const FadeGeom* xf = nullptr) {
  // J-30: never waits for a decode (drawn once ready). K-85: the sound the channel's sample page makes.
  const ClipSource src = clipSourceIfReady(ch);
  const SamplePtr& smp = src.sample;
  if (smp && smp->frames() >= 2) {
    drawWaveformFrom(dl, s, ch, c, tm, a, b, clipX0, pxPerTick, visX0, visX1, col, xf, livetake::samplePeaks(smp), smp->frames(), smp->sampleRate,
                     &src);
  } else if (!smp) {
    // J-217: a take just recorded draws from its live summary until its file has decoded.
    if (const livetake::Handoff* h = livetake::pending(ch.plugin.data))
      drawWaveformFrom(dl, s, ch, c, tm, a, b, clipX0, pxPerTick, visX0, visX1, col, xf, h->peaks, static_cast<size_t>(h->frames), h->sampleRate,
                       nullptr);
  }
}

// K-114: an audio clip's waveform; a looping clip pass by pass (each pass
// from its loop's start; drawn once its audio has decoded).
void drawClipWave(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                  float pxPerTick, float visX0, float visX1, ImU32 col, const FadeGeom* xf = nullptr) {
  if (!c.loop || !sampleCache().request(ch.plugin.data)) {
    drawWaveform(dl, s, ch, c, tm, a, b, clipX0, pxPerTick, visX0, visX1, col, xf);
    return;
  }
  for (const Clip& p : audioLoopPasses(s, c, tm)) {
    const float px0 = clipX0 + static_cast<float>(p.start - c.start) * pxPerTick, px1 = px0 + static_cast<float>(p.length) * pxPerTick;
    if (px0 > visX1) break;
    if (px1 < visX0) continue;
    drawWaveform(dl, s, ch, p, tm, a, b, px0, pxPerTick, std::max(visX0, px0), std::min(visX1, px1), col, xf);
  }
}

// K-114: the curve as a placement shows it (a placement that does not loop holds its last value).
// K-140: a cut clip that loops repeats its own part (AutoGeom's window), not the curve's loop.
const AutomationClip& shownCurve(const AutomationClip& ac, const Clip& c, AutomationClip& scratch) {
  if ((c.loop && c.loopLength <= 0) || ac.loop <= 0) return ac;
  scratch = ac;
  scratch.loop = 0;
  return scratch;
}

// Automation curve geometry inside a clip body.
struct AutoGeom {
  ImVec2 a, b;  // body rect
  float clipX0 = 0, pxPerTick = 1;
  Tick start = 0, offset = 0, len = 0;
  // K-140: a cut clip that loops repeats content [wFrom, wFrom + wLen) (wLen 0: none).
  Tick wFrom = 0, wLen = 0;
  float x(Tick content) const { return clipX0 + static_cast<float>(content - offset) * pxPerTick; }
  float y(double v) const { return b.y - static_cast<float>(v) * (b.y - a.y); }
  double valueAtY(float yy) const { return std::clamp(static_cast<double>((b.y - yy) / std::max(1.0f, b.y - a.y)), 0.0, 1.0); }
  bool visible(Tick content) const { return content >= offset && content <= offset + len && (wLen <= 0 || content <= wFrom + wLen); }
  double content(double rel) const {  // content shown `rel` ticks into the clip
    double t = static_cast<double>(offset) + rel;
    const double e = static_cast<double>(wFrom + wLen);
    if (wLen > 0 && t >= e) t = static_cast<double>(wFrom) + std::fmod(t - static_cast<double>(wFrom), static_cast<double>(wLen));
    return t;
  }
};
// K-140: the geometry's loop window for a placement.
AutoGeom withOwnLoop(AutoGeom g, const Clip& c) {
  if (c.loop && c.loopLength > 0) g.wFrom = c.loopStart, g.wLen = c.loopLength;
  return g;
}

void drawAutomation(ImDrawList* dl, const AutomationClip& ac, const AutoGeom& g, float visX0, float visX1, ImU32 line, ImU32 fill,
                    bool handles, int hotPoint, int hotHandle, ImU32 hot) {
  float x0 = std::max(g.a.x, visX0), x1 = std::min(g.b.x, visX1);
  if (x1 <= x0) return;
  // One value per pixel column; the fill is one triangle strip and the curve
  // one polyline (J-25: was two line primitives per column).
  static std::vector<ImVec2> pts;
  pts.clear();
  for (float x = std::floor(x0); x <= x1; x += 1.0f) {
    const double content = g.content((x - g.clipX0) / g.pxPerTick);
    pts.emplace_back(x, g.y(automationValue(ac, content)));
  }
  const int n = static_cast<int>(pts.size());
  if (n >= 2 && (fill & IM_COL32_A_MASK)) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6 * (n - 1), 2 * n);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int i = 0; i < n; ++i) {
      dl->PrimWriteVtx(ImVec2(pts[static_cast<size_t>(i)].x + 0.5f, pts[static_cast<size_t>(i)].y), uv, fill);
      dl->PrimWriteVtx(ImVec2(pts[static_cast<size_t>(i)].x + 0.5f, g.b.y), uv, fill);
    }
    for (int i = 0; i + 1 < n; ++i) {
      const ImDrawIdx k = static_cast<ImDrawIdx>(base + 2 * i);
      dl->PrimWriteIdx(k);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 2));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 3));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 2));
    }
  }
  if (n >= 2) dl->AddPolyline(pts.data(), n, line, ImDrawFlags_None, 1.5f);
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

bool clipValid(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) return s.channel(c.source) != nullptr;
  if (c.kind == ClipKind::Automation) return s.automation(c.source) != nullptr;
  return s.pattern(c.pattern) != nullptr;
}

// Per-clip data the canvas needs every frame (validity, length, colour, name),
// rebuilt only when the document changes: Song::clipLength / lookups by id are
// linear scans (pattern length walks every note), far too slow for thousands
// of clips per frame (J-25).
struct ClipCache {
  uint64_t rev = ~0ull;
  const Clip* data = nullptr;
  size_t count = 0;
  std::vector<Tick> len;
  std::vector<Tick> loop;  // Q-1 / K-114: loop length of a looping pattern or automation clip's content (0 = does not loop)
  std::vector<Tick> loopFrom;  // K-140: where that loop starts in the content (a cut clip's own loop)
  std::vector<uint8_t> valid;
  std::vector<Rgb> color;
  std::vector<const std::string*> name;
  Tick songLen = 0;
  // J-38: stacking and fades. Clips draw by track, then start (a later clip
  // lies on top of the one it overlaps); `cover` is the clip lying directly on
  // top of each clip (-1 = none), `fades` the resolved audio fades.
  std::vector<uint32_t> order;
  std::vector<int32_t> cover;
  // R11-10: per clip, how many clips lie wholly under its start (the same track and start: their
  // names hidden by it), so Smooth can say so on the one on top.
  std::vector<uint16_t> under;
  std::vector<ClipFades> fades;
};
const ClipCache& clipCache(const App& a) {
  static ClipCache cc;
  const Song& s = a.doc.song();
  if (cc.rev == a.doc.revision() && cc.data == s.playlist.data() && cc.count == s.playlist.size()) return cc;
  cc.rev = a.doc.revision();
  cc.data = s.playlist.data();
  cc.count = s.playlist.size();
  std::unordered_map<ObjectId, size_t> pat, chan, aut;
  pat.reserve(s.patterns.size());
  for (size_t i = 0; i < s.patterns.size(); ++i) pat.emplace(s.patterns[i].id, i);
  for (size_t i = 0; i < s.channels.size(); ++i) chan.emplace(s.channels[i].id, i);
  for (size_t i = 0; i < s.automations.size(); ++i) aut.emplace(s.automations[i].id, i);
  std::vector<Tick> patLen(s.patterns.size(), -1);
  static const std::string kAudio = "Audio", kAuto = "Automation", kUnknown = "?";
  cc.len.assign(cc.count, 0);
  cc.loop.assign(cc.count, 0);
  cc.loopFrom.assign(cc.count, 0);
  cc.valid.assign(cc.count, 0);
  cc.color.assign(cc.count, 0x7A8696);
  cc.name.assign(cc.count, &kUnknown);
  cc.songLen = 0;
  for (size_t i = 0; i < cc.count; ++i) {
    const Clip& c = s.playlist[i];
    if (c.kind == ClipKind::Audio) {
      auto it = chan.find(c.source);
      cc.valid[i] = it != chan.end();
      if (cc.valid[i]) cc.color[i] = s.channels[it->second].color, cc.name[i] = &s.channels[it->second].name;
      else cc.name[i] = &kAudio;
      cc.len[i] = s.clipLength(c);
    } else if (c.kind == ClipKind::Automation) {
      auto it = aut.find(c.source);
      cc.valid[i] = it != aut.end();
      if (cc.valid[i]) cc.color[i] = s.automations[it->second].color, cc.name[i] = &s.automations[it->second].name;
      else cc.name[i] = &kAuto;
      if (cc.valid[i] && c.loop && s.automations[it->second].loop > 0) cc.loop[i] = s.automations[it->second].loop;  // K-114
      if (cc.valid[i] && c.loop && c.loopLength > 0) cc.loop[i] = c.loopLength, cc.loopFrom[i] = c.loopStart;  // K-140
      cc.len[i] = s.clipLength(c);
    } else {
      auto it = pat.find(c.pattern);
      cc.valid[i] = it != pat.end();
      if (cc.valid[i]) {
        const Pattern& pp = s.patterns[it->second];
        cc.color[i] = pp.color, cc.name[i] = &pp.name;
        Tick& pl = patLen[it->second];
        if (pl < 0) pl = s.patternContentLength(pp);
        cc.len[i] = c.length > 0 ? c.length : pl;
        if (c.loop) cc.loop[i] = std::max<Tick>(1, pl);  // K-114: per clip
        if (c.loop && c.loopLength > 0) cc.loop[i] = c.loopLength, cc.loopFrom[i] = c.loopStart;  // K-140: a cut clip's own loop
      } else {
        cc.len[i] = c.length > 0 ? c.length : 0;
      }
    }
    cc.songLen = std::max(cc.songLen, c.start + cc.len[i]);
  }
  // J-38: stacking order and the clip covering each clip.
  cc.order.resize(cc.count);
  for (size_t i = 0; i < cc.count; ++i) cc.order[i] = static_cast<uint32_t>(i);
  std::sort(cc.order.begin(), cc.order.end(), [&](uint32_t x, uint32_t y) {
    const Clip &p = s.playlist[x], &q = s.playlist[y];
    // R04-02: a Tracks-layout lane's curve (an automation clip on a track with a lane) lies under the
    // track's clips in every layout, so it never hides them or takes their clicks.
    const bool pl = logicplug::laneClip(s, p), ql = logicplug::laneClip(s, q);
    return p.track != q.track ? p.track < q.track : pl != ql ? pl : p.start != q.start ? p.start < q.start : x < y;
  });
  cc.cover.assign(cc.count, -1);
  for (size_t k = 0; k + 1 < cc.count; ++k) {
    const uint32_t i = cc.order[k], j = cc.order[k + 1];
    const Clip &p = s.playlist[i], &q = s.playlist[j];
    if (cc.valid[i] && cc.valid[j] && p.track == q.track && q.start < p.start + cc.len[i]) cc.cover[i] = static_cast<int32_t>(j);
  }
  cc.under.assign(cc.count, 0);
  for (size_t k = 0; k + 1 < cc.count; ++k) {
    const uint32_t i = cc.order[k], j = cc.order[k + 1];
    const Clip &p = s.playlist[i], &q = s.playlist[j];
    if (cc.valid[i] && cc.valid[j] && p.track == q.track && p.start == q.start && !logicplug::laneClip(s, p) && !logicplug::laneClip(s, q))
      cc.under[j] = static_cast<uint16_t>(std::min<int>(cc.under[i] + 1, 999));
  }
  TempoMap tm;
  tm.build(s);
  cc.fades = resolveAllFades(s, tm);
  return cc;
}

// The rows every clip kind has (mute, meter, loop): true when an edit replaced the song.
static bool clipOwnRows(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return true;
  // R05-18: the Tracks layout shows the region mute's key, as the reference's menu does
  if (ImGui::MenuItem(regionWord("Mute clip"), layoutui::active() == layout::Preset::Logic ? keys::label("playlist.mute") : nullptr, c->muted)) a.doc.editClip(id, "mute clip", [](Clip& x) { x.muted = !x.muted; });
  meterui::clipMeterItems(a, id);  // K-179: a pattern clip in a section of another meter
  {  // K-114: the clip's loop (every selected clip with it, when it is selected)
    std::set<ObjectId> sel = a.playlist.selected;
    if (!sel.count(id)) sel = {id};
    if (ImGui::MenuItem(i18n::tr("Loop"), nullptr, c->loop)) {
      const bool on = !c->loop;
      a.doc.editSong(on ? "loop clip" : "stop looping clip", [&](Song& x) { setClipsLoop(x, sel, on); });
      return true;  // the edit replaced the song: `s` and `c` point into the old one
    }
    ui::Hint(c->kind == ClipKind::Audio ? "Longer than its audio, the clip repeats it (else it stops at the end of the audio)"
             : c->kind == ClipKind::Automation ? "Longer than its curve, the clip repeats it (else the last value holds)"
                                               : "Longer than its pattern, the clip repeats it (else the rest stays empty)");
    if (c->loop && c->kind != ClipKind::Pattern && ImGui::MenuItem(i18n::tr("Reset loop start"), nullptr, false, c->offset != 0))
      a.doc.editClip(id, "reset loop start", [](Clip& x) { x.offset = 0; });
  }
  return false;
}

// K-112b3: a pattern clip's Playback submenu: how this clip plays its pattern.
static void clipPlaybackMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return;
  if (c->kind == ClipKind::Pattern && ImGui::BeginMenu(i18n::tr("Playback"))) {  // K-112b3: how this clip plays its pattern
    auto drag = [&](const char* wid, const char* label, int v, int lo, int hi, const char* fmt, uint64_t key, auto set) {
      ImGui::SetNextItemWidth(170);
      if (ImGui::DragInt(wid, &v, 0.25f, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) a.doc.editClip(id, label, [v, set](Clip& k) { set(k, v); }, key);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    };
    drag("##ptrans", "clip transpose", c->playTranspose, -48, 48, i18n::tr("Transpose %+d st"), 0x112B0321, [](Clip& k, int v) { k.playTranspose = v; });
    drag("##pvel", "clip velocity", c->playVelocity, -127, 127, i18n::tr("Velocity %+d"), 0x112B0322, [](Clip& k, int v) { k.playVelocity = v; });
    const int q = static_cast<int>(std::max<Tick>(1, s.ppq));
    drag("##pdelay", "clip delay", static_cast<int>(c->playDelay), -q * 4, q * 4, i18n::tr("Delay %+d ticks"), 0x112B0323, [](Clip& k, int v) { k.playDelay = v; });
    if (ImGui::BeginMenu(i18n::tr("Quantize"))) {
      const Tick qs[] = {0, q, q / 2, q / 4, q / 8, q / 3, q / 6};
      const char* qn[] = {i18n::N_("Off"), "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T"};
      for (int i = 0; i < 7; ++i)
        if (ImGui::MenuItem(i18n::tr(qn[i]), nullptr, c->playQuantize == qs[i])) {
          const Tick to = qs[i];
          a.doc.editClip(id, "clip playback quantise", [to](Clip& k) { k.playQuantize = to; });
        }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Reset"), nullptr, false, clipplay::active(*c)))
      a.doc.editClip(id, "reset clip playback", [](Clip& k) { k.playTranspose = 0, k.playVelocity = 0, k.playDelay = 0, k.playQuantize = 0; });
    ImGui::EndMenu();
    ui::Hint("How this clip plays its pattern: transpose, velocity, delay and quantise (the pattern stays as written)");
  }
}

// A pattern clip's rows: its loop start, the pattern's automation.
static void patternClipRows(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return;
  // Q-1: the clip's loop start (K-114: its loop is the clip's own, above).
  if (const Pattern* p = s.pattern(c->pattern)) {
    const ObjectId pid = p->id;
    if (ImGui::MenuItem(i18n::tr("Reset loop start"), nullptr, false, c->offset != 0))
      a.doc.editClip(id, "reset loop start", [](Clip& x) { x.offset = 0; });
    if (ImGui::BeginMenu(regionWord("Automation in this pattern"), !p->automation.empty())) {  // J-173
      ParamId pick{kNoId, 0};
      for (const auto& pa : p->automation) {
        const std::string l = patternAutoName(s, pa) + "##pa" + std::to_string(pa.owner) + "_" + std::to_string(pa.index);
        if (ImGui::MenuItem(l.c_str())) pick = ParamId{pa.owner, pa.index};
      }
      ImGui::EndMenu();
      if (pick.owner != kNoId) openPatternAutomation(a, pid, pick);
    }
  }
}

// Right-click menu of a clip: arrangement settings, then the pattern clip menu.
void arrangementClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return;
  if (livelook::active(a) && c->kind == ClipKind::Pattern) {
    // K-112 (Live): the clip's own rows sit in the clip menu's rename section (the object menu skeleton).
    // N47-12: only the reference's own row here: Deactivate (Studio's loop, playback and pattern
    // automation rows are not in its clip menu; the clip band holds the loop).
    clipContextMenu(a, id, [&a, id] {
      const Clip* c = a.doc.song().clip(id);
      if (!c) return true;
      if (ImGui::MenuItem(i18n::tr("Deactivate"), keys::label("playlist.mute"), c->muted)) {
        a.doc.editClip(id, c->muted ? "activate clip" : "deactivate clip", [](Clip& x) { x.muted = !x.muted; });
        return true;
      }
      return false;
    });
    return;
  }
  if (livelook::active(a) && c->kind == ClipKind::Audio) {  // checkpoint 49: the reference's audio clip menu
    clipsAudioClipMenu(a, id);
    return;
  }
  if (c->kind != ClipKind::Pattern) ImGui::TextDisabled("%s", clipName(s, *c).c_str());
  if (clipOwnRows(a, id)) return;
  c = a.doc.song().clip(id);
  if (!c) return;
  clipPlaybackMenu(a, id);
  if (c->kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c->source); ch && ImGui::BeginMenu(i18n::tr("Rename..."))) {  // J-28: the clip's title (its channel's name)
      static char buf[512];  // R02-20: long names are kept (a 128-byte field cut them without a word)
      if (ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", ch->name.c_str());
      ImGui::SetNextItemWidth(180);
      if (ImGui::InputText("##rn", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        a.doc.renameChannel(ch->id, buf);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }
    if (const Channel* ch = s.channel(c->source); ch && ImGui::MenuItem(i18n::tr("Edit in Loupe"), nullptr, false, !ch->plugin.data.empty()))
    {
      if (logicplug::active(a)) {  // R01-12: the Tracks layout edits audio in the lower pane (in place of the piano roll it opens with)
        a.settings.set("ui.loupePinned", true), layoutui::setRegionVisible(a, "logic.lower", true);
        a.showPiano = a.showRack = false;
      }
      packs::openAudioEditor(a, ch->plugin.data, ch->id);  // J-7
    }
    if (ImGui::MenuItem(i18n::tr("Export clip as WAV..."))) exportui::exportClipAudio(a, id);  // J-20
    static uint64_t key = 0x41430000;
    // J-79: FL's clip gain range (-96 .. +36 dB, the bottom is silence), same value as the volume line.
    float gainDb = static_cast<float>(std::max(clipgain::kMinDb, clipgain::toDb(c->gain)));
    // J-142: with the clip in a multi-selection each field moves every selected audio clip by the same amount.
    using clipmulti::Param;
    ImGui::SetNextItemWidth(160);
    if (!clipui::multiDrag(a, id, Param::GainDb, i18n::tr("Gain"), gainDb, 0.1f, static_cast<float>(clipgain::kMinDb),
                           static_cast<float>(clipgain::kMaxDb), "%.1f dB", 1.0, "clip gain", key) &&
        ImGui::DragFloat(i18n::tr("Gain"), &gainDb, 0.1f, static_cast<float>(clipgain::kMinDb), static_cast<float>(clipgain::kMaxDb), "%.1f dB"))
      a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = clipgain::fromDb(gainDb <= clipgain::kMinDb ? clipgain::kMinDb - 0.1 : gainDb); }, key);
    float cents = c->pitch;
    ImGui::SetNextItemWidth(160);
    if (!clipui::multiDrag(a, id, Param::Pitch, i18n::tr("Pitch"), cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct", 1.0, "clip pitch", key + 1) &&
        ImGui::DragFloat(i18n::tr("Pitch"), &cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct"))
      a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = cents; }, key + 1);
    float fi = static_cast<float>(c->fadeIn * 1000.0), fo = static_cast<float>(c->fadeOut * 1000.0);
    ImGui::SetNextItemWidth(160);
    if (!clipui::multiDrag(a, id, Param::FadeIn, i18n::tr("Fade in"), fi, 1.0f, 0.0f, 60000.0f, "%.0f ms", 1000.0, "clip fade in", key + 2) &&
        ImGui::DragFloat(i18n::tr("Fade in"), &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0, x.fadeInManual = true; }, key + 2);
    ImGui::SetNextItemWidth(160);
    if (!clipui::multiDrag(a, id, Param::FadeOut, i18n::tr("Fade out"), fo, 1.0f, 0.0f, 60000.0f, "%.0f ms", 1000.0, "clip fade out", key + 3) &&
        ImGui::DragFloat(i18n::tr("Fade out"), &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0, x.fadeOutManual = true; }, key + 3);
    if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, c->reverse)) a.doc.editClip(id, "reverse clip", [&a](Clip& x) { toggleClipReverse(a.doc.song(), x); });  // K-85: keeps its audio
    {  // K-85: warp markers (transients, audio quantize, clear)
      TempoMap wtm;
      wtm.build(s);
      plwarp::clipMenu(a, id, wtm, timeline::snapGrid(s, a.playlist.snap, a.playlist.pxPerTick, false));
    }
    if (ImGui::MenuItem(i18n::tr("Channel settings"))) a.settingsChannel = c->source;
    {  // K-158: the clip's audio tools in one submenu (J-193 tempo follow, J-28 operations)
      const uint64_t rev = a.doc.revision();
      const bool openAudio = ImGui::BeginMenu(i18n::tr("Audio"));
      devRackItem("clipmenu audio");  // K-158: scripted screenshots open it
      if (openAudio) {
        // J-193: on, the clip keeps its bars when the tempo changes (a stretch turns it on); off, it keeps its audio and seconds.
        if (ImGui::MenuItem(i18n::tr("Follow the project tempo"), nullptr, c->syncBpm > 0)) {
          const bool on = !(c->syncBpm > 0);
          a.doc.editSong("follow tempo", [&](Song& x) { setClipFollowsTempo(x, id, on); });
        }
        audioClipTempoItems(a, id);  // J-8: fit to tempo, normalize
        ImGui::Separator();
        std::set<ObjectId> sel = a.playlist.selected;
        if (!sel.count(id)) sel = {id};
        if (ImGui::MenuItem(i18n::tr("Make unique as sample"))) {
          if (ObjectId nch = ops::makeAudioClipUnique(a.doc, id)) a.settingsChannel = nch;
        }
        if (ImGui::MenuItem(i18n::tr("Pitch a semitone up"))) ops::pitchAudioClips(a.doc, sel, 100.0f);
        if (ImGui::MenuItem(i18n::tr("Pitch a semitone down"))) ops::pitchAudioClips(a.doc, sel, -100.0f);
        ImGui::EndMenu();
      }
      if (a.doc.revision() != rev) {  // the edit replaced the song: `s` and `c` point into the old one
        a.doc.endGesture();
        return;
      }
    }
    a.doc.endGesture();
    ImGui::Separator();
    audioClipMenu(a, id);  // J-8: properties, takes, stretch, fit, normalize, slice, consolidate, bounce
  } else if (c->kind == ClipKind::Automation) {
    const AutomationClip* ac = s.automation(c->source);
    if (ac) {
      std::string target = ac->owner == kNoId ? std::string("no target") : paramDisplayName(s, {ac->owner, ac->index});
      ImGui::TextDisabled(i18n::tr("Drives: %s"), target.c_str());
      if (ImGui::MenuItem(i18n::tr("Edit automation..."))) openAutomationEditor(a, ac->id, id);
      if (ImGui::BeginMenu(i18n::tr("Curve of every segment"))) {
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
      if (ImGui::MenuItem(i18n::tr("Reset tensions"))) {
        ObjectId aid = ac->id;
        a.doc.editSong("reset tensions", [aid](Song& x) {
          if (AutomationClip* q = x.automation(aid))
            for (auto& p : q->points) p.tension = 0;
        });
      }
      if (ImGui::MenuItem(i18n::tr("Merge selected automation clips"), nullptr, false, a.playlist.selected.size() >= 2)) {  // J-28 (op: J-27)
        ObjectId nc = ops::mergeAutomationClips(a.doc, a.playlist.selected);
        if (nc) a.playlist.selected = {nc};
        else a.notify("Merging needs two automation clips of the same parameter");
        return;  // the clip under the menu may be gone
      }
      if (ImGui::MenuItem(i18n::tr("Select all clips of this automation"))) {
        a.playlist.selected.clear();
        for (const auto& k : s.playlist)
          if (k.kind == ClipKind::Automation && k.source == ac->id) a.playlist.selected.insert(k.id);
      }
      // J-173: the clip into the current pattern (what this placement plays).
      if (const Pattern* cp = s.pattern(a.curPattern)) {
        ImGui::Separator();
        const std::string mv = std::string(i18n::tr("Move into pattern")) + " \"" + cp->name + "\"";
        const std::string cpy = std::string(i18n::tr("Copy into pattern")) + " \"" + cp->name + "\"";
        const ObjectId pid = cp->id;
        int act = 0;
        if (ImGui::MenuItem(mv.c_str())) act = 1;
        ui::Hint("The pattern holds the automation from now on: it plays wherever the pattern plays and shows on its clips");
        if (ImGui::MenuItem(cpy.c_str())) act = 2;
        if (act) {
          const patauto::MoveResult r = patauto::placementIntoPattern(a.doc, id, pid, act == 2);
          if (r != patauto::MoveResult::Done) a.notify(patauto::moveResultText(r), true);
          else a.notify(act == 1 ? "Automation moved into the pattern" : "Automation copied into the pattern");
          return;  // the clip under the menu may be gone
        }
      }
    }
  } else {
    patternClipRows(a, id);
    ImGui::Separator();
    clipContextMenu(a, id);
  }
}

PlaylistGeometry devGeom;  // J-118 dev check

// ---- current clip source (J-28) ----
using browserui::ClipSrcPayload;

bool isAudioSource(const Channel& ch) { return ops::isAudioClipSource(ch); }

// K-278: the channel the piano roll shows for a pattern clip: the selected channel when it has
// notes in the pattern, else the clip track's linked instrument, else the first channel with
// notes there (kNoId: the pattern has no notes).
ObjectId patternNoteChannel(const App& a, const Song& s, const Clip& c) {
  const Pattern* p = c.kind == ClipKind::Pattern ? s.pattern(c.pattern) : nullptr;
  if (!p) return kNoId;
  auto has = [&](ObjectId ch) {
    const auto it = p->notes.find(ch);
    return it != p->notes.end() && !it->second.empty();
  };
  if (has(a.selChannel)) return a.selChannel;
  if (const Channel* lc = trackmodes::linkedChannel(s, c.track); lc && has(lc->id)) return lc->id;
  for (const auto& [cid, ns] : p->notes)
    if (!ns.empty()) return cid;
  return kNoId;
}


// K-235: audition from the clip sources list (setting playlist.pickerAudition).
namespace pickeraudition {
struct State {
  bool on = false;
  ClipKind kind = ClipKind::Pattern;
  bool transport = false;  // a pattern: the transport was started for it
  PlayMode mode = PlayMode::Pattern;
  double seek = 0;
};
State& state() {
  static State s;
  return s;
}
void stop(App& a) {
  State& au = state();
  if (!au.on) return;
  if (au.kind == ClipKind::Audio) a.stopPreview();
  else if (au.transport) {
    a.engine.stop();
    a.engine.setPattern(a.curPattern);
    if (a.engine.mode() != au.mode) a.engine.setMode(au.mode);
    if (au.seek > 0) a.engine.setPosition(static_cast<Tick>(std::llround(au.seek)), true);
  }
  au = {};
}
// The switch itself, shown in the picker's menus (a source's menu and the tab band's).
void toggleItem(App& a) {
  const bool on = a.settings.getBool("playlist.pickerAudition");
  if (ImGui::MenuItem(i18n::tr("Play sources while pressed"), nullptr, on)) a.settings.set("playlist.pickerAudition", !on);
  ui::Hint(i18n::tr("Pressing a pattern or audio source in this list plays it until the button is let go"));
}
void start(App& a, ClipKind kind, ObjectId id) {
  stop(a);
  if (!a.settings.getBool("playlist.pickerAudition")) return;
  const Song& s = a.doc.song();
  State& au = state();
  if (kind == ClipKind::Audio) {
    const Channel* ch = s.channel(id);
    if (!ch || !isAudioSource(*ch)) return;
    a.previewSample(ch->plugin.data);
    au.on = true, au.kind = kind;
  } else if (kind == ClipKind::Pattern && s.pattern(id) && !a.engine.playing()) {
    au.on = true, au.kind = kind, au.transport = true, au.mode = a.engine.mode(), au.seek = a.engine.seekPosition();
    if (a.engine.mode() != PlayMode::Pattern) a.engine.setMode(PlayMode::Pattern);
    a.engine.setPattern(id);
    a.engine.setPosition(0, false);
    a.engine.play();
  }
}
}  // namespace pickeraudition


pltools::Source currentSource(App& a) {
  const PlaylistState& st = a.playlist;
  if (st.srcKind != ClipKind::Pattern) {
    pltools::Source s{st.srcKind, st.srcId};
    if (pltools::sourceValid(a.doc.song(), s)) return s;
  }
  return {ClipKind::Pattern, a.curPattern};
}

void setSource(App& a, const pltools::Source& src) {
  a.playlist.srcKind = src.kind;
  a.playlist.srcId = src.id;
  a.playlist.brushId = kNoId;  // J-118: a source picked by itself places its natural clip
  if (src.kind == ClipKind::Pattern) a.selectPattern(src.id);
  if (src.kind == ClipKind::Audio) a.selectChannel(src.id);
}

// J-118: clicking a clip makes it the brush: draw / paint place copies of it.
void setSourceFromClip(App& a, const Clip& c) {
  setSource(a, pltools::sourceOf(c));
  a.playlist.brushId = c.id;
  a.playlist.brush = c;
}

// The brush for `src` (the clicked clip as it is now, else as it was when
// clicked), nullptr when draw / paint place the source's natural clip.
const Clip* currentBrush(App& a, const pltools::Source& src) {
  PlaylistState& st = a.playlist;
  if (st.brushId == kNoId) return nullptr;
  if (const Clip* live = a.doc.song().clip(st.brushId); live && pltools::sourceOf(*live) == pltools::sourceOf(st.brush)) st.brush = *live;
  return pltools::sourceOf(st.brush) == src ? &st.brush : nullptr;
}

void openSource(App& a, const pltools::Source& src) {
  if (livelook::active(a) && src.kind != ClipKind::Automation) {  // Clips: the clip band shows it (no rack)
    if (src.kind == ClipKind::Pattern) a.selectPattern(src.id);
    layoutui::setRegionVisible(a, "live.clip", true);
    return;
  }
  if (src.kind == ClipKind::Pattern) {
    a.selectPattern(src.id);
    a.showRack = true;
    a.focusWindow = "Channel rack";
  } else if (src.kind == ClipKind::Audio) {
    a.settingsChannel = src.id;
  } else {
    openAutomationEditor(a, src.id);
  }
}

std::string sourceName(const Song& s, const pltools::Source& src) {
  if (src.kind == ClipKind::Audio) return s.channel(src.id) ? s.channel(src.id)->name : "Audio";
  if (src.kind == ClipKind::Automation) return s.automation(src.id) ? s.automation(src.id)->name : "Automation";
  return s.pattern(src.id) ? s.pattern(src.id)->name : "(none)";
}

// Pattern: step bars; audio: a wave; automation: a ramp with points. Each fitted by its ink into
// the same `size` px square, centred, so the three read as one set whatever their SVG geometry.
void drawKindGlyph(ImDrawList* dl, ClipKind kind, ImVec2 c, float size, ImU32 col) {
  const ui::Icon icon = kind == ClipKind::Audio ? ui::Icon::Wave : kind == ClipKind::Automation ? ui::Icon::ClipAutomation : ui::Icon::ClipPattern;
  const float h = size * 0.5f;
  ui::drawIconFit(dl, icon, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col);
}

// Breadcrumb menu: every clip source by kind.
void sourceMenu(App& a) {
  const Song& s = a.doc.song();
  const pltools::Source cur = currentSource(a);
  ImGui::SeparatorText(i18n::tr("Patterns"));
  for (const auto& p : s.patterns) {
    ImGui::PushID(static_cast<int>(p.id));
    if (ImGui::MenuItem(p.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Pattern, p.id})) setSource(a, {ClipKind::Pattern, p.id});
    ImGui::PopID();
  }
  bool any = false;
  for (const auto& ch : s.channels) {
    if (!isAudioSource(ch)) continue;
    if (!any) ImGui::SeparatorText(i18n::tr("Audio"));
    any = true;
    ImGui::PushID(static_cast<int>(ch.id));
    if (ImGui::MenuItem(ch.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Audio, ch.id})) setSource(a, {ClipKind::Audio, ch.id});
    ImGui::PopID();
  }
  if (!s.automations.empty()) ImGui::SeparatorText(i18n::tr("Automation"));
  for (const auto& ac : s.automations) {
    ImGui::PushID(static_cast<int>(ac.id));
    if (ImGui::MenuItem(ac.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Automation, ac.id})) setSource(a, {ClipKind::Automation, ac.id});
    ImGui::PopID();
  }
}

// J-82: the picker's selection when it holds `id` (several rows), else just `id`.
std::vector<ObjectId> pickerTargets(const App& a, ObjectId id) {
  const auto& sel = a.playlist.pickerSel;
  if (sel.size() > 1 && sel.count(id)) return std::vector<ObjectId>(sel.begin(), sel.end());
  return {id};
}

ClipKind pickerKind(int tab) { return tab == 1 ? ClipKind::Audio : tab == 2 ? ClipKind::Automation : ClipKind::Pattern; }

// The ids of the picker tab's rows, in list order.
std::vector<ObjectId> pickerRows(const Song& s, int tab) {
  std::vector<ObjectId> out;
  if (tab == 0) {
    for (const auto& p : s.patterns) out.push_back(p.id);
  } else if (tab == 1) {
    for (const auto& ch : s.channels)
      if (isAudioSource(ch)) out.push_back(ch.id);
  } else {
    for (const auto& ac : s.automations) out.push_back(ac.id);
  }
  return out;
}

// Delete items shared by the three source menus: the row (or the selection
// holding it), selecting / deleting the unused sources of the kind.
void deleteSourceItems(App& a, ClipKind kind, ObjectId id) {
  if (kind != ClipKind::Pattern) {  // patterns: the pattern menu's own Delete (same request)
    const auto targets = pickerTargets(a, id);
    char label[96];
    if (targets.size() > 1)
      std::snprintf(label, sizeof label, i18n::tr("Delete %d selected..."), static_cast<int>(targets.size()));
    else
      std::snprintf(label, sizeof label, "%s", i18n::tr("Delete..."));
    if (ImGui::MenuItem(label, keys::label("playlist.delete")))  // next frame: the picker draws this in its source loop
      a.later([&a, kind, targets] { requestDeleteSources(a, kind, targets); });
    ui::Hint(kind == ClipKind::Audio ? "Removes the audio clip channel and every clip of it from the project (the sample file stays on disk)"
                                     : "Removes the automation clip and every placement of it from the playlist");
  }
  const auto unused = ops::unusedClipSources(a.doc.song(), kind);
  const char* selUnused = kind == ClipKind::Audio        ? i18n::N_("Select unused audio clips")
                          : kind == ClipKind::Automation ? i18n::N_("Select unused automation clips")
                                                         : i18n::N_("Select unused patterns");
  if (ImGui::MenuItem(i18n::tr(selUnused), nullptr, false, !unused.empty())) {
    a.playlist.pickerSel = std::set<ObjectId>(unused.begin(), unused.end());
    a.playlist.pickerFocus = true;
  }
  if (kind == ClipKind::Pattern) return;  // patterns: the project keeps one
  const char* delUnused = kind == ClipKind::Audio ? i18n::N_("Delete unused audio clips") : i18n::N_("Delete unused automation clips");
  if (ImGui::MenuItem(i18n::tr(delUnused), nullptr, false, !unused.empty())) a.later([&a, kind, unused] { requestDeleteSources(a, kind, unused); });
  ui::Hint("Sources no clip in any arrangement uses (audio: channels that play no pattern either)");
}

// K-112c (C44-02): in Clips one track plays one clip at a time, so clips placed by a move, a
// duplicate or a paste overwrite what they cover on their track (the covered clips are cut at the
// edges and the covered pieces removed), as the reference's arrangement does. Inside one song edit.
void overwriteCovered(Song& x, const std::set<ObjectId>& placed, Tick origin) {
  struct R {
    Tick a, b;
    int t;
  };
  std::vector<R> ranges;
  for (const Clip& c : x.playlist)
    if (placed.count(c.id) && c.start >= origin) ranges.push_back({c.start, c.start + x.clipLength(c), c.track});
  for (const R& r : ranges) {
    if (r.b <= r.a) continue;
    bool other = false;  // only cut when something else is there
    for (const Clip& c : x.playlist)
      other = other || (!placed.count(c.id) && c.track == r.t && c.start >= origin && c.start < r.b && c.start + x.clipLength(c) > r.a);
    if (!other) continue;
    const std::set<ObjectId> in = chop::isolateRange(x, r.a, r.b, {r.t});
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                    [&](const Clip& c) { return in.count(c.id) && !placed.count(c.id) && c.start >= origin; }),
                     x.playlist.end());
  }
}

}  // namespace

const PlaylistGeometry& playlistGeometry() { return devGeom; }
void notePatternMenuClone(float x, float y) { devGeom.cloneX = x, devGeom.cloneY = y; }

// J-82: deleting clip sources (picker menus, Delete in the picker). Sources
// the arrangement uses ask first; unused ones go at once. One undo step.
void requestDeleteSources(App& a, ClipKind kind, std::vector<ObjectId> ids) {
  const Song& s = a.doc.song();
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](ObjectId id) {
                             return kind == ClipKind::Pattern ? !s.pattern(id) : kind == ClipKind::Audio ? !s.channel(id) : !s.automation(id);
                           }),
            ids.end());
  if (ids.empty()) return;
  if (kind == ClipKind::Pattern && ids.size() >= s.patterns.size()) {
    a.notify("The project keeps at least one pattern");
    return;
  }
  int clips = 0;
  bool playsNotes = false;
  for (ObjectId id : ids) {
    clips += ops::clipSourceUses(s, kind, id);
    if (kind == ClipKind::Audio) playsNotes = playsNotes || ops::channelHasPatternData(s, id);
  }
  const std::string name = sourceName(s, {kind, ids[0]});
  auto run = [&a, kind, ids, name]() {
    const int n = ops::deleteClipSources(a.doc, kind, ids);
    PlaylistState& st = a.playlist;
    for (auto it = st.selected.begin(); it != st.selected.end();) it = a.doc.song().clip(*it) ? std::next(it) : st.selected.erase(it);
    st.pickerSel.clear();
    if (n == 1) a.notify("Deleted " + name);
    else if (n > 1) a.notify("Deleted clip sources: " + std::to_string(n));
  };
  if (clips == 0 && !playsNotes) {
    run();
    return;
  }
  const int count = static_cast<int>(ids.size());
  char title[256];
  if (count == 1 && clips == 1)
    std::snprintf(title, sizeof title, i18n::tr("Delete '%s' and its clip?"), name.c_str());
  else if (count == 1 && clips > 1)
    std::snprintf(title, sizeof title, i18n::tr("Delete '%s' and its %d clips?"), name.c_str(), clips);
  else if (count == 1)
    std::snprintf(title, sizeof title, i18n::tr("Delete '%s'?"), name.c_str());
  else
    std::snprintf(title, sizeof title, i18n::tr("Delete %d clip sources and their %d clips?"), count, clips);
  a.confirm.title = title;
  if (kind == ClipKind::Audio)
    a.confirm.subtitle = playsNotes ? i18n::tr("Its notes and steps in patterns go too. The sample file stays on disk.")
                                    : i18n::tr("The channel leaves the channel rack. The sample file stays on disk.");
  else
    a.confirm.subtitle = i18n::tr("Every clip of it leaves the playlist, in every arrangement.");
  a.confirm.okLabel = i18n::N_("Delete");
  a.confirm.action = run;
  a.confirm.open = true;
}

void audioSourceMenu(App& a, ObjectId id) {
  if (!a.doc.song().channel(id)) return;
  if (ImGui::MenuItem(i18n::tr("Channel settings"))) a.settingsChannel = id;
  if (ImGui::MenuItem(i18n::tr("Select its clips"))) {
    a.playlist.selected.clear();
    for (const auto& k : a.doc.song().playlist)
      if (k.kind == ClipKind::Audio && k.source == id) a.playlist.selected.insert(k.id);
  }
  ImGui::Separator();
  deleteSourceItems(a, ClipKind::Audio, id);
}

void automationSourceMenu(App& a, ObjectId id) {
  if (!a.doc.song().automation(id)) return;
  if (ImGui::MenuItem(i18n::tr("Edit automation..."))) openAutomationEditor(a, id, kNoId);
  if (ImGui::MenuItem(i18n::tr("Select its clips"))) {
    a.playlist.selected.clear();
    for (const auto& k : a.doc.song().playlist)
      if (k.kind == ClipKind::Automation && k.source == id) a.playlist.selected.insert(k.id);
  }
  ImGui::Separator();
  deleteSourceItems(a, ClipKind::Automation, id);
}

void patternSourceMenu(App& a, ObjectId id) {
  patternContextMenu(a, id);
  ImGui::Separator();
  deleteSourceItems(a, ClipKind::Pattern, id);
}

// GitHub #46: the tracks a header gesture moves (Windows.h).
std::vector<int> playlistTrackSelection(App& a, int track) {
  const std::set<int>& sel = a.playlist.trackSel;
  if (sel.count(track)) return std::vector<int>(sel.begin(), sel.end());
  return {track};
}

bool stepPlaylistSelection(App& a, const std::vector<int>& tracks, int dir) {
  std::vector<int> now;
  // N53-03: in Clips a track's lanes move with it.
  const std::vector<int> mv = livelook::active(a) ? trackmodes::withLanes(a.doc.song(), tracks) : tracks;
  if (!trackorder::stepPlaylistTracks(a.doc, mv, dir, &now)) return false;
  if (livelook::active(a))
    now.erase(std::remove_if(now.begin(), now.end(), [&a](int t) { return trackmodes::isAutomationLane(a.doc.song(), t); }), now.end());
  a.playlist.trackSel = std::set<int>(now.begin(), now.end());
  return true;
}

namespace {
int gDevLane = -1;  // K-112b: a lane asked for by the dev command
bool gEndDrag = false;  // checkpoint 47: the Project End marker is being dragged
// cp54 (polish): Smooth's tracks toolbar, planned once a frame to fit its width (see the toolbar).
struct ToolbarPlan {
  bool sliders = true, dragPrefix = true, snapLabel = true;
  float snapW = 112, dragW = 150, groupX = 0;
};
ToolbarPlan gTb;
ObjectId gRegionRename = kNoId;  // R07-01: the region the Text tool renames in place
ImVec2 gRegionRenameAt;
float gRegionRenameW = 160;
std::string gRegionRenameText;
}
void devShowAutomationLane(int track) { gDevLane = track; }

// Checkpoint 47: the reference's most used tracks-area keys. Up / Down select the previous / next
// track; Left / Right the previous / next region on the focused track (from the selected region, else
// the playhead); Option-Left / Right nudge the selected regions by the snap grid; L loops them.
void tracksNavKeys(App& a) {
  if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
  PlaylistState& st = a.playlist;
  const Song& s = a.doc.song();
  const int n = static_cast<int>(s.tracks.size());
  const logicplug::AutoFold& fold = logicplug::autoFold(a);
  auto region = [&](const Clip& c) { return !logicplug::laneClip(s, c); };
  const int prevT = keys::pressed("playlist.prevTrack") ? -1 : keys::pressed("playlist.nextTrack") ? 1 : 0;
  if (prevT != 0 && n > 0) {
    int t = std::clamp(logicplug::focusTrack(a), 0, n - 1);
    for (int k = t + prevT; k >= 0 && k < n; k += prevT)
      if (!fold.takesNoRoom(k)) {
        t = k;
        break;
      }
    st.trackSel = {t};
  }
  const int dirR = keys::pressed("playlist.prevRegion") ? -1 : keys::pressed("playlist.nextRegion") ? 1 : 0;
  if (dirR != 0) {
    int tr = logicplug::focusTrack(a);
    Tick from = static_cast<Tick>(a.engine.position());
    ObjectId fromId = kNoId;
    for (const Clip& c : s.playlist)  // from the selected region on that track (the last for Right, the first for Left)
      if (st.selected.count(c.id) && region(c) && (tr < 0 || c.track == tr) &&
          (fromId == kNoId || (dirR > 0 ? c.start > from : c.start < from)))
        tr = c.track, from = c.start, fromId = c.id;
    const Clip* best = nullptr;
    for (const Clip& c : s.playlist) {
      if (c.track != tr || !region(c) || c.id == fromId) continue;
      const bool after = c.start > from || (c.start == from && fromId != kNoId && c.id > fromId);
      const bool before = c.start < from || (c.start == from && fromId != kNoId && c.id < fromId);
      if (dirR > 0 ? !(after || (fromId == kNoId && c.start == from)) : !before) continue;
      if (!best || (dirR > 0 ? c.start < best->start : c.start > best->start)) best = &c;
    }
    if (best) {
      st.selected = {best->id};
      st.trackSel = {best->track};
    }
  }
  const int dirN = keys::pressed("playlist.nudgeLeft") ? -1 : keys::pressed("playlist.nudgeRight") ? 1 : 0;
  if (dirN != 0 && !st.selected.empty()) {
    const MeterMap mm(s);
    const Tick step = std::max<Tick>(1, timeline::snapGrid(mm, st.snap == Snap::None ? Snap::Line : st.snap, st.pxPerTick, false));
    Tick lo = -1;
    for (const Clip& c : s.playlist)
      if (st.selected.count(c.id) && region(c) && !s.tracks[static_cast<size_t>(std::clamp(c.track, 0, std::max(0, n - 1)))].locked)
        lo = lo < 0 ? c.start : std::min(lo, c.start);
    const Tick d = dirN < 0 ? -std::min(step, std::max<Tick>(0, lo)) : step;
    if (lo >= 0 && d != 0) {
      const std::set<ObjectId> sel = st.selected;
      a.doc.editSong("nudge regions", [&](Song& x) {
        for (Clip& c : x.playlist)
          if (sel.count(c.id) && !logicplug::laneClip(x, c) && c.track >= 0 && c.track < static_cast<int>(x.tracks.size()) && !x.tracks[static_cast<size_t>(c.track)].locked)
            c.start += d;
      });
    }
  }
  if (keys::pressed("playlist.loopRegion") && !st.selected.empty()) {
    bool allOn = true;
    for (const Clip& c : s.playlist)
      if (st.selected.count(c.id) && region(c)) allOn = allOn && c.loop;
    const std::set<ObjectId> sel = st.selected;
    a.doc.editSong(allOn ? "loop off" : "loop on", [&](Song& x) {
      for (Clip& c : x.playlist)
        if (sel.count(c.id) && !logicplug::laneClip(x, c)) c.loop = !allOn;
    });
  }
}

// R07-03 / R07-10 / checkpoint 47: the Tracks layout's Join (the Join tool, Edit > Join, its key and
// the toolbar's button): the selected regions of one kind (`kind`; ClipKind::Automation: the kind of
// the first selected region).
void tracksJoin(App& a, ClipKind kind) {
  PlaylistState& st = a.playlist;
  const Song& s = a.doc.song();
  if (kind == ClipKind::Automation)
    for (const Clip& c : s.playlist)
      if (st.selected.count(c.id) && c.kind != ClipKind::Automation && !logicplug::laneClip(s, c)) {
        kind = c.kind;
        break;
      }
  auto trackLocked = [&](int tr) { return tr >= 0 && tr < static_cast<int>(s.tracks.size()) && s.tracks[static_cast<size_t>(tr)].locked; };
  std::vector<Clip> picked;
  for (const Clip& c : s.playlist)
    if (st.selected.count(c.id) && c.kind == kind && c.kind != ClipKind::Automation && !trackLocked(c.track)) picked.push_back(c);
  std::sort(picked.begin(), picked.end(), [](const Clip& x, const Clip& y) { return x.track != y.track ? x.track < y.track : x.start < y.start; });
  bool pieces = picked.size() >= 2;
  for (size_t i = 1; pieces && i < picked.size(); ++i) {
    const Clip &p0 = picked[i - 1], &p1 = picked[i];
    const Tick l0 = s.clipLength(p0);
    pieces = p1.track == p0.track && p1.kind == p0.kind && p1.pattern == p0.pattern && p1.source == p0.source && !p0.loop && !p1.loop &&
             p1.start == p0.start + l0 &&
             (p0.kind == ClipKind::Audio ? (p0.audioEnd <= 0 || std::fabs(p1.audioStart - p0.audioEnd) < 1e-6) && p1.audioStart > p0.audioStart && p1.gain == p0.gain
                                         : p1.offset == p0.offset + l0);
  }
  if (pieces) {
    const ObjectId first = picked.front().id;
    const Tick len = picked.back().start + s.clipLength(picked.back()) - picked.front().start;
    std::set<ObjectId> rest;
    for (size_t i = 1; i < picked.size(); ++i) rest.insert(picked[i].id);
    const Clip last = picked.back();
    a.doc.editSong("join regions", [&](Song& x) {
      for (Clip& c : x.playlist)
        if (c.id == first) {
          c.length = len;
          if (c.kind == ClipKind::Audio) c.fadeOut = last.fadeOut, c.fadeOutManual = last.fadeOutManual, c.audioEnd = last.audioEnd;
        }
      x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [&](const Clip& c) { return rest.count(c.id) > 0; }), x.playlist.end());
    });
    st.selected = {first};
  } else if (picked.size() >= 2 && kind == ClipKind::Pattern) {
    std::set<ObjectId> join;
    for (const Clip& c : picked) join.insert(c.id);
    const std::string name = s.pattern(picked.front().pattern) ? s.pattern(picked.front().pattern)->name : std::string();
    a.doc.beginGroup("join regions");
    const ObjectId nid = ops::mergeClips(a.doc, join);
    if (const Clip* nc = a.doc.song().clip(nid); nc && !name.empty()) a.doc.renamePattern(nc->pattern, name);  // R07-10: the first one's name
    a.doc.endGroup();
    if (nid != kNoId) st.selected = {nid};
  } else if (picked.size() >= 2 && kind == ClipKind::Audio) {
    // Checkpoint 47: other audio regions join as the reference's do: one new file per track,
    // the gaps silent, named after the first region (one undo step).
    std::map<int, std::set<ObjectId>> perTrack;
    for (const Clip& c : picked) perTrack[c.track].insert(c.id);
    const std::string dir = renderedDir(a);
    const double sr = a.engine.sampleRate();
    a.later([&a, perTrack, dir, sr] {
      std::set<ObjectId> made;
      a.doc.beginGroup("join regions");
      for (const auto& entry : perTrack) {
        const std::set<ObjectId>& ids = entry.second;
        if (ids.size() < 2) continue;
        ObjectId nid = kNoId;
        a.doc.editSong("join regions", [&](Song& x) { nid = logicplug::joinAudioRegions(x, ids, dir, sr); });
        if (nid != kNoId) made.insert(nid);
      }
      a.doc.endGroup();
      if (!made.empty()) a.playlist.selected = made;
      else a.notify(i18n::tr("Join: nothing to join (the regions are muted or silent)"), true);
    });
  } else {
    a.setHint(i18n::tr("Join: select two or more regions, then click one of them"));
  }
}

namespace { bool gLaneToggle = false; }
void toggleAutomationLanes() { gLaneToggle = true; }
namespace {
bool gToolMenu = false;
// R01-18: while Cmd is held the Tracks layout's tracks area works with its Command-click tool
// (PlaylistState::cmdTool); the tool chosen in the menu comes back when Cmd is let go.
bool gCmdSwap = false;
Tool gCmdSaved = Tool::Select;
// The reference's tool menu picks a tool by a letter while it is open.
char toolLetter(Tool t) {
  switch (t) {
    case Tool::Select: return 'T';
    case Tool::Draw: return 'P';
    case Tool::Erase: return 'E';
    case Tool::Slice: return 'I';
    case Tool::Mute: return 'M';
    case Tool::Zoom: return 'Z';
    case Tool::Fade: return 'A';
    case Tool::Marquee: return 'R';
    case Tool::Text: return 'X';
    case Tool::Glue: return 'Y';
    case Tool::Solo: return 'S';
    case Tool::AutoSelect: return 'U';
    case Tool::AutoCurve: return 'W';
    case Tool::Warp: return 'F';  // R05-17: the Flex tool
    default: return 0;
  }
}
}  // namespace
void openToolMenu() { gToolMenu = true; }
namespace {
// R05-17: the Automation Select tool's points (of one automation clip), and the range being dragged.
ObjectId gAutoSelClip = kNoId;
std::set<int> gAutoSelPts;
int gAutoSelTrack = -1;
float gAutoSelX0 = 0, gAutoSelX1 = 0;
bool gAutoSelDragging = false;
}  // namespace
namespace {
// K-112B: a lane's value as the reference's lanes show a volume: in dB on the fader law (other
// parameters in their own units).
std::string laneValueText(const Song& s, ParamId pid, double v, const ParamDesc& d) {
  double unity = -1;
  if (s.channel(pid.owner) && pid.index == static_cast<uint32_t>(ChannelParam::Volume)) unity = 0.78;
  else if (s.mixerTrack(pid.owner) && pid.index == static_cast<uint32_t>(MixerParam::Volume)) unity = 0.8;
  if (unity < 0) return d.format(v);
  const double g = dsp::faderGain(v, unity);
  if (g < 1e-5) return "-inf dB";
  double db = dsp::gainToDb(g);
  if (std::fabs(db) < 0.05) db = 0.0;
  char b[24];
  std::snprintf(b, sizeof b, "%.1f dB", db);
  return b;
}
// K-112B: the automation modes, as the reference's track header lists them, with its colours.
const char* const kAutoModeNames[5] = {i18n::N_("Off"), i18n::N_("Read"), i18n::N_("Touch"), i18n::N_("Latch"), i18n::N_("Write")};
const uint32_t kAutoModeColors[5] = {0x8A8A8A, 0x5CC26A, 0xE0B33A, 0xE08A3A, 0xA77BE0};  // R05-15: Latch orange, Write purple
const char* const kAutoModeHints[5] = {
    i18n::N_("Off: the track's automation does not play (it is kept)"),
    i18n::N_("Read: the automation plays; moving a control writes nothing"),
    i18n::N_("Touch: while playing, moving a control writes its values; let go and the curve plays on as it was"),
    i18n::N_("Latch: while playing, the first move starts writing, and the last value goes on until playback stops"),
    i18n::N_("Write: from the start of playback the control's value replaces the curve, until playback stops")};
}  // namespace
void toggleTracksAudioEditor(App& a) {
  const layout::Preset lp = layout::Preset::Logic;
  const bool lowerShown = layout::visible(layout::shell(lp), layoutui::regionState(a, lp), "logic.lower");
  if (lowerShown && a.settings.getBool("ui.loupePinned") && !a.showPiano && !a.showRack) {
    layoutui::setRegionVisible(a, "logic.lower", false);
    a.settings.set("ui.loupePinned", false);  // (pinned, it went on drawing as a window over the tracks)
    return;
  }
  const Song& s = a.doc.song();
  const Channel* ach = nullptr;
  for (const Clip& c : s.playlist)
    if (!ach && c.kind == ClipKind::Audio && a.playlist.selected.count(c.id)) ach = s.channel(c.source);
  const int ft = logicplug::focusTrack(a);
  for (const Clip& c : s.playlist)
    if (!ach && c.kind == ClipKind::Audio && c.track == ft) ach = s.channel(c.source);
  layoutui::setRegionVisible(a, "logic.lower", true);
  a.showPiano = a.showRack = a.showMixer = false;  // the audio editor takes the pane, not the piano roll
  a.settings.set("ui.loupePinned", true);
  if (ach && !ach->plugin.data.empty()) packs::openAudioEditor(a, ach->plugin.data, ach->id);
}
bool automationLanesShown(const App& a) {  // R04-08: Show Automation is for every track
  for (const PlaylistTrack& t : a.doc.song().tracks)
    if (t.laneShown) return true;
  return false;
}
namespace {
bool gDevRowsAsked = false;
// Checkpoint 48 (audit D1): Smooth's regions in the reference's saturated colours: the track colour's
// hue at 70 % saturation and about 52 % lightness (greys stay grey), so names read white on them.
ImU32 smoothRegionColor(ImU32 c) {
  float r = static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, g = static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
        b = static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
  float h = 0, sat = 0, v = 0;
  ImGui::ColorConvertRGBtoHSV(r, g, b, h, sat, v);
  if (sat < 0.08f) return IM_COL32(118, 118, 118, 255);
  // HSV for 70 % HSL saturation at 52 % lightness: v = l + s * min(l, 1 - l), s_v = 2 (1 - l / v).
  const float l = (h > 0.11f && h < 0.20f) ? 0.56f : 0.50f, sl = 0.68f;
  const float vv = l + sl * std::min(l, 1.0f - l), sv = vv > 0 ? 2.0f * (1.0f - l / vv) : 0.0f;
  ImGui::ColorConvertHSVtoRGB(h, sv, vv, r, g, b);
  return IM_COL32(static_cast<int>(r * 255.0f + 0.5f), static_cast<int>(g * 255.0f + 0.5f), static_cast<int>(b * 255.0f + 0.5f), 255);
}
// R09-05: a Smooth track's one colour, as the reference's (the number cell, the regions and the
// curves share it): the colour the person gave the track, else the colour of the sound it plays,
// else its first region's; 0 when it has none (its regions keep their own). Worked out once per document revision.
const std::vector<Rgb>& smoothTrackColors(const App& a) {
  static std::vector<Rgb> cols;
  static uint64_t rev = ~0ull;
  static const Song* song = nullptr;
  const Song& s = a.doc.song();
  if (rev == a.doc.revision() && song == &s && cols.size() == s.tracks.size()) return cols;
  rev = a.doc.revision(), song = &s;
  cols.assign(s.tracks.size(), 0);
  const Rgb plain = PlaylistTrack{}.color;
  for (size_t i = 0; i < s.tracks.size(); ++i) {
    Rgb c = 0;
    bool has = false;
    if (s.tracks[i].color != plain) c = s.tracks[i].color, has = true;
    else if (const Channel* ch = logicplug::trackSound(a, static_cast<int>(i))) c = ch->color, has = true;
    // R10 gap 8: a track with neither (a generic track) takes its first region's colour, so its
    // number cell and its regions agree (a Pencil region was green on a grey track).
    for (size_t k = 0; !has && k < s.playlist.size(); ++k) {
      const Clip& r = s.playlist[k];
      if (r.track != static_cast<int>(i)) continue;
      if (r.kind == ClipKind::Pattern) {
        if (const Pattern* p = s.pattern(r.pattern)) c = p->color, has = true;
      } else if (r.kind == ClipKind::Audio) {
        if (const Channel* ch = s.channel(r.source)) c = ch->color, has = true;
      }
    }
    cols[i] = has ? std::max<Rgb>(c, 1) : 0;  // (black itself is still a colour: 0 means none)
  }
  return cols;
}
// R08-01 (checkpoint 49): the song's automation lanes for the arrange (the lane curves and the
// header readouts), built once per document revision: automationLanes copies every curve, and the
// arrange asked for it twice a frame while lanes were shown (bounces and playback paid for it).
const std::vector<AutomationLane>& arrangeLanes(const App& a) {
  static std::vector<AutomationLane> lanes;
  static uint64_t rev = ~0ull;
  static const Song* song = nullptr;
  const Song& s = a.doc.song();
  if (rev != a.doc.revision() || song != &s) rev = a.doc.revision(), song = &s, lanes = automationLanes(s);
  return lanes;
}
// The name on a Smooth region: white, but near black on the yellow family (the reference's).
bool smoothDarkName(ImU32 c) {
  float h = 0, sat = 0, v = 0;
  ImGui::ColorConvertRGBtoHSV(static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                              static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, h, sat, v);
  return sat > 0.2f && h > 0.11f && h < 0.20f;
}
// R08-04 / R08-05 (Smooth): a new audio track for dropped files, as New tracks > Audio makes one
// (its own strip), named after the first file, track and strip alike; the files as regions at `at`.
void newAudioTrackFor(App& a, const std::vector<std::string>& files, Tick at) {
  const int t = trackmodes::addAudioTrack(a.doc);
  if (t < 0 || files.empty()) return;
  browserui::DropPlace pl;
  pl.track = t, pl.tick = at;
  browserui::dropFiles(a, files, browse::DropTarget::Playlist, pl);
  const std::string nm = std::filesystem::path(files.front()).stem().string();
  if (!nm.empty()) {
    a.doc.editPlaylistTrack(t, "name track", [&nm](PlaylistTrack& k) { k.name = nm; });
    if (const MixerTrack* mt = trackmodes::linkedMixer(a.doc.song(), t))
      a.doc.editMixerTrack(mt->id, "name track", [&nm](MixerTrack& x) { x.name = nm; });
  }
  a.playlist.trackSel = {t};
}
}  // namespace
void devPlaylistRowsAsk() { gDevRowsAsked = true; }
namespace { std::string gDevMenu; }
void devMenuOpen(const std::string& id) { gDevMenu = id; }
// K-112b: the Tracks layout's references draw lower rows (125, 216, 195); the others keep 46.
float playlistDefaultTrackHeight(bool tracksLayout) { return tracksLayout ? 36.0f : 46.0f; }
bool devMenuTake(const char* id) {
  if (gDevMenu.empty() || gDevMenu != id) return false;
  gDevMenu.clear();
  return true;
}

void drawPlaylist(App& a) {
  logicplug::autoFold(a);  // K-112B (checkpoint 46): the fold follows the layout every frame
  // C44-01: a selection never names a clip that is gone (an undone duplicate's id is handed out again
  // by the next new clip, which then started out selected).
  for (auto it = a.playlist.selected.begin(); it != a.playlist.selected.end();) it = a.doc.song().clip(*it) ? std::next(it) : a.playlist.selected.erase(it);
  // K-141: a drag whose window closed or hid before the button came up keeps its undo step open
  // (and undo waiting): close it once the button is up.
  if (cg.grouped && !ImGui::IsAnyMouseDown() && !ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    if (livelook::active(a) && cg.kind == ClipGesture::Move && cg.moved) {  // C44-02 (as the release below)
      const std::set<ObjectId> placed = a.playlist.selected;
      const Tick o = livelook::arrangementOrigin(a);
      a.doc.editSong("move clips", [&](Song& x) { overwriteCovered(x, placed, o); });
    }
    a.doc.endGroup();
    a.doc.endGesture();
    cg = ClipGesture{};
  }
  // R02-06: the Tracks layout's tracks area is its main window: it cannot be closed (View > Playlist,
  // its tab's close box and Close all windows leave it in place).
  if (layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic)) a.showPlaylist = true;
  if (!a.showPlaylist) return;
  if (!ui::BeginDockWindow("Playlist", &a.showPlaylist, ImVec2(1000, 480))) {
    ui::EndDockWindow();
    return;
  }
  if (trackerpages::active(a)) {  // K-106: the Tracker's Song page (the same clips, its own look)
    trackerpages::song(a);
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  PlaylistState& st = a.playlist;
  // K-112c: in the Live layout the arrangement has no toolbar or picker (their commands are in the
  // menus, the keys and the browser), and the track headers sit right of the lanes.
  const bool liveLook = livelook::active(a);
  syncEditToggles(a);  // J-79: stretch / fade handles / zero crossings persist globally
  // K-112b: the Logic layout draws the tracks area in its own look (headers, ruler with a markers
  // lane, no picker column, no scrollers above and beside the headers); the same data and gestures.
  const bool lg = layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic);
  ImGuiIO& io = ImGui::GetIO();
  {  // R01-18: the Command-click tool, switched in between gestures (a drag keeps its tool)
    const bool idle = cg.kind == ClipGesture::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (gCmdSwap && (!lg || (!io.KeyCtrl && idle))) {
      st.tool = gCmdSaved;
      gCmdSwap = false;
    } else if (lg && !gCmdSwap && io.KeyCtrl && idle && st.cmdTool != st.tool && !io.WantTextInput) {
      gCmdSaved = st.tool;
      st.tool = st.cmdTool;
      gCmdSwap = true;
    }
  }
  if (!lg) st.mqA = st.mqB = 0;
  // K-112B (checkpoint 46): automation-only tracks as curves on their tracks' lanes (a view, see AutoFold).
  const logicplug::AutoFold& fold = logicplug::autoFold(a);
  // R09-01: where a region may go: PL-08's track modes, and in this layout never on a bus track or an
  // automation-only track (automation regions only there).
  auto clipAllowed = [&](ClipKind k, int tr) {
    return trackmodes::clipAllowed(s, k, tr) && !(lg && k != ClipKind::Automation && fold.takesNoRegions(tr));
  };
  auto clipRefused = [&](int tr) {
    return lg && fold.takesNoRegions(tr) ? i18n::tr("A bus track holds its automation only, no regions") : i18n::tr("Instrument tracks take pattern clips only");
  };
  if (lg)  // R04-05: a track's own automation is never part of the region selection (Select all, Delete, drags)
    for (auto it = st.selected.begin(); it != st.selected.end();) {
      const Clip* sc = a.doc.song().clip(*it);
      it = sc && logicplug::laneClip(a.doc.song(), *sc) ? st.selected.erase(it) : std::next(it);
    }
  // K-112b (b4, R02-01): automation lanes in the Logic look, one under a track (its menu or A shows
  // or hides it). The lane is part of the track (PlaylistTrack::lane*), so it moves with the track
  // and is saved with the project. A lane shows one parameter across the song: the automation clips
  // that drive it, wherever they lie, as one curve. Its edits go to those clips (R02-03: a first
  // click where none plays starts one curve over the whole song, on the lane's own track).
  constexpr float kAutoLaneH = 44.0f;
  // K-112B (checkpoint 46, ref 254): the track number column at the left of each header, in the track's colour.
  const float kHeadNumW = lg ? 22.0f : 0.0f;
  // What a new lane shows (R02-02): what the track's own automation clips drive, else the volume
  // of the sound the track plays (its channel, a free track's regions' sound), else its mixer
  // track's. Never the master: a lane on one track must not fade the whole mix.
  auto laneDefault = [&](int tr) -> std::optional<ParamId> {
    std::optional<ParamId> pick;
    for (const Clip& c : s.playlist)
      if (!pick && c.track == tr && c.kind == ClipKind::Automation)
        if (const AutomationClip* ac = s.automation(c.source)) pick = ParamId{ac->owner, ac->index};
    // K-112B (checkpoint 46): else the first curve an automation-only track shows on this one.
    for (size_t ci = 0; !pick && lg && ci < s.playlist.size() && ci < fold.clipTarget.size(); ++ci)
      if (fold.clipTarget[ci] == tr)
        if (const AutomationClip* ac = s.automation(s.playlist[ci].source)) pick = ParamId{ac->owner, ac->index};
    if (!pick) {
      // R04-03: the fader the inspector's channel strip shows: the track's mixer track (its own, else
      // its instrument's), the reference's track Volume; the instrument's own volume when it plays
      // straight into the master.
      // R05-13: a kit on several mixer tracks gets their bus (trackStrip), not its first sound's.
      const Channel* lc = logicplug::trackSound(a, tr);
      const MixerTrack* mt = logicplug::trackStrip(a, tr);
      if (mt) pick = mixerParam(mt->id, MixerParam::Volume);
      else if (lc) pick = channelParam(lc->id, ChannelParam::Volume);
    }
    return pick;
  };
  // R10 CPU: a track's default lane parameter, worked out once per document revision (and layout):
  // it walked every region for each shown lane each frame.
  auto laneDefaultOnce = [&](int tr) -> std::optional<ParamId> {
    static std::vector<std::optional<ParamId>> memo;
    static std::vector<char> done;
    static uint64_t rev = ~0ull;
    static const Song* song = nullptr;
    static bool memoLg = false, memoFold = false, memoOpen = false;
    if (rev != a.doc.revision() || song != &s || memoLg != lg || memoFold != fold.on || memoOpen != fold.open || memo.size() != s.tracks.size()) {
      rev = a.doc.revision(), song = &s, memoLg = lg, memoFold = fold.on, memoOpen = fold.open;
      memo.assign(s.tracks.size(), std::nullopt), done.assign(s.tracks.size(), 0);
    }
    if (tr < 0 || tr >= static_cast<int>(memo.size())) return laneDefault(tr);
    if (!done[static_cast<size_t>(tr)]) memo[static_cast<size_t>(tr)] = laneDefault(tr), done[static_cast<size_t>(tr)] = 1;
    return memo[static_cast<size_t>(tr)];
  };
  std::map<int, ParamId> autoLanes;  // playlist track -> the parameter its lane shows (this frame)
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    if (!t.laneShown) continue;
    // R05-01: a track whose parameter nobody chose shows its default (worked out each frame), so
    // showing automation writes nothing into the track: its automation regions stay regions.
    if (t.laneSet && describeParam(s, ParamId{t.laneOwner, t.laneIndex})) autoLanes[i] = ParamId{t.laneOwner, t.laneIndex};
    else if (const auto d = laneDefaultOnce(i)) autoLanes[i] = *d;
  }
  // Show or hide a track's lane. R04-10: showing and hiding is a view change (saved with the
  // project, never an undo step and never "changed"); choosing another parameter is an edit.
  auto setLane = [&](int tr, bool show, std::optional<ParamId> pid, bool quietNoSound = false) {
    if (tr < 0 || tr >= static_cast<int>(s.tracks.size())) return;
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    const bool viewOnly = !pid;
    if (show && !pid) {
      if (t.laneSet && describeParam(s, ParamId{t.laneOwner, t.laneIndex})) pid = ParamId{t.laneOwner, t.laneIndex};
      else pid = laneDefault(tr);
    }
    if (show && !pid) {
      if (!quietNoSound) a.setHint(i18n::tr("This track plays no sound yet, so it has nothing to automate"));
      return;
    }
    auto apply = [&](PlaylistTrack& x) {
      x.laneShown = show;
      // R05-01: only a parameter the person chose makes the track's automation its own (laneSet);
      // A and the header's Show automation are views and leave it as it was.
      if (pid && !viewOnly) x.laneSet = true, x.laneOwner = pid->owner, x.laneIndex = pid->index;
    };
    if (viewOnly) {
      if (t.laneShown == show) return;
      a.doc.patchSong([&](Song& x) { apply(x.tracks[static_cast<size_t>(tr)]); });
    } else {
      a.doc.editPlaylistTrack(tr, "automation parameter", apply);
    }
  };
  if (gLaneToggle) {  // R02-15
    gLaneToggle = false;
    if (lg) {  // R04-08: as the reference's Show Automation (A): every track shows its automation
      const bool show = !automationLanesShown(a);
      for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) setLane(t, show, std::nullopt, true);
    }
  }
  if (gDevLane >= 0 && gDevLane < static_cast<int>(s.tracks.size())) {
    setLane(gDevLane, true, std::nullopt);
    gDevLane = -1;
  }
  {  // Logic: the editors follow the selected region: its pattern, and the channel that plays it.
    // Only a selection made here moves them; one carried over from another layout keeps its channel.
    static ObjectId followed = kNoId;
    const ObjectId first = st.selected.empty() ? kNoId : *st.selected.begin();
    const bool changed = first != followed;
    followed = first;
    if (lg && changed) {
      const bool lowerShown = layout::visible(layout::shell(layout::Preset::Logic), layoutui::regionState(a, layout::Preset::Logic), "logic.lower");
      const Clip* c = first != kNoId ? s.clip(first) : nullptr;
      if (c && c->kind == ClipKind::Pattern) {
        if (c->pattern != a.curPattern) a.selectPattern(c->pattern);
        ObjectId ch = kNoId;
        if (const Channel* lc = trackmodes::linkedChannel(s, c->track)) ch = lc->id;
        if (const Pattern* pt = s.pattern(c->pattern); pt && (ch == kNoId || !pt->notes.count(ch)))
          for (const auto& [cid, ns] : pt->notes)
            if (!ns.empty()) {
              ch = cid;
              break;
            }
        if (ch != kNoId && ch != a.selChannel) a.selectChannel(ch);
        // R01-12: the lower pane's audio editor gives way to the piano roll for a pattern region.
        if (a.settings.getBool("ui.loupePinned") && lowerShown) {
          a.settings.set("ui.loupePinned", false);
          a.showPiano = true;
          packs::closeAudioEditorSampleWindows();  // R04-04: unpinned, it went on as a big window over the tracks
        }
      } else if (c && c->kind == ClipKind::Audio && lowerShown && (a.showPiano || a.showRack || a.settings.getBool("ui.loupePinned"))) {
        // R01-12: an audio region shows the audio editor in the lower pane, never a piano roll of
        // some other pattern (the reference's editor follows the selected region's kind).
        if (const Channel* ach = s.channel(c->source); ach && !ach->plugin.data.empty()) {
          a.showPiano = a.showRack = false;
          a.settings.set("ui.loupePinned", true);
          packs::openAudioEditor(a, ach->plugin.data, ach->id);
        }
      }
    }
  }
  const float winW = ImGui::GetContentRegionAvail().x;
  // Tempo map for audio clip drawing (rebuilt when the song changes).
  static TempoMap tempoMap;
  static uint64_t tempoRev = ~0ull;
  if (tempoRev != a.doc.revision()) {
    tempoMap.build(s);
    tempoRev = a.doc.revision();
  }

  // ---------------- toolbar (FL order: options, snap, tools, breadcrumb) ----------------
  // (Logic: a calm bar of its own: the options as a text menu, the tool in use as one button, snap,
  // follow and zoom. The control bar's toolbar switch shows the full bar with every tool instead.)
  if (!liveLook) {
  // R01-27: the Toolbar button shows the toolbar strip under the control bar now; the tracks
  // area keeps its own short bar.
  const bool calm = lg;
  ui::BeginToolbar("##pltb");
  {
    const float fh = ImGui::GetFrameHeight();
    if (calm) {  // three text menus: Edit, Functions, View
      struct M {
        const char* name;
        const char* id;
        const char* hint;
      };
      const M ms[] = {{i18n::N_("Edit"), "##plcalm1", "Edit and select clips"},
                      {i18n::N_("Functions"), "##plcalm2", "Time selection, markers, arrangements, tracks and clean-up"},
                      {i18n::N_("View"), "##plcalm3", "Tools, snap, zoom and what the tracks area shows"}};
      static int openPart = 0;
      int k = 0;
      bool anyOpen = false;
      // K-112b (checkpoint 20): one group of flat menu labels, each with a down chevron, hairlines
      // between them, as 216, 125 and 164 draw the tracks area's local menus (they were three
      // separate raised buttons with no chevron).
      const auto& PP = pal();
      const float padX = 9, chevS = 7, chevGap = 4;
      float segW[3] = {};
      for (int i = 0; i < 3; ++i) segW[i] = std::round(ImGui::CalcTextSize(i18n::tr(ms[i].name)).x + padX * 2 + chevGap + chevS);
      ImDrawList* gdl = ImGui::GetWindowDrawList();
      const float gr = ImGui::GetStyle().FrameRounding;
      const ImU32 groupBg = ImGui::GetColorU32(ImGuiCol_FrameBg);
      // B5 / K-112D note 8: plain text menus with a chevron, no box (the hover lights one).
      for (const M& mm : ms) {
        ++k;
        const std::string pid = std::string(mm.id) + "pop";
        const bool isOpen = ImGui::IsPopupOpen(pid.c_str());
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        const float sw = segW[k - 1];
        const bool clicked = ImGui::InvisibleButton(mm.id, ImVec2(sw, fh));
        const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (hov) ui::Hint(mm.hint);
        if (isOpen || hov)
          gdl->AddRectFilled(bp, ImVec2(bp.x + sw, bp.y + fh), theme::mix(groupBg, PP.text, isOpen ? 0.14f : 0.08f), gr);  // K-112b: the quiet hover
        const char* nm = i18n::tr(mm.name);
        const ImVec2 ts = ImGui::CalcTextSize(nm);
        const float ty = std::round(bp.y + (fh - ts.y) * 0.5f);
        gdl->AddText(ImVec2(bp.x + padX, ty), PP.text, nm);
        ui::drawIcon(gdl, ui::Icon::ChevronDown, ImVec2(std::round(bp.x + padX + ts.x + chevGap + chevS * 0.5f), std::round(bp.y + fh * 0.5f)), chevS,
                     PP.textDim);
        if (clicked && !isOpen) ImGui::OpenPopup(pid.c_str());
        else if (openPart && openPart != k && hov && !isOpen) ImGui::OpenPopup(pid.c_str());
        ImGui::SetNextWindowPos(ImVec2(bp.x, bp.y + fh + 2));
        if (ImGui::BeginPopup(pid.c_str())) {
          anyOpen = true, openPart = k;
          playlistOptionsMenu(a, k);
          ImGui::EndPopup();
        }
        ImGui::SameLine(0, 0);
      }
      if (!anyOpen) openPart = 0;
      ImGui::SameLine(0, 4);
      if (lg) {  // B1/B3: the view toggles after View: catch (the view follows the playhead), blue when on
        ImGui::SameLine(0, 12);
        const bool follow = a.settings.getBool("ui.autoScroll");
        if (ui::IconButton("##plfollow", ui::Icon::ScrollTo, follow, fh,
                           follow ? "Catch is on: the view follows the playhead while the song plays (click to keep the view still)"
                                  : "Catch is off: the view stays where you put it while the song plays (click to follow the playhead)",
                           PP.accent))
          a.settings.set("ui.autoScroll", !follow);
        // B1: the tool menus, Snap and Drag sit together, right-aligned before the zoom.
        // cp54 (polish): one plan for the whole right side, on one spacing scale (4 px inside a group,
        // 12 between groups), so the group butts against the zoom it is aligned to (at 1920 the plan
        // assumed the zoom sliders while the buttons were drawn: a 143 px hole) and a narrow tracks
        // area compacts in steps instead of clipping (at 1024 the Drag menu ran off the edge and the
        // pads and zoom buttons went): the zoom sliders give way to buttons, then Drag drops its
        // "Drag:" prefix, then the "Snap:" label goes, then both menus narrow.
        const float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        ImGui::SameLine(0, 12);
        const float startX = ImGui::GetCursorScreenPos().x;
        const float snapLblW = ImGui::CalcTextSize(i18n::tr("Snap:")).x;
        auto plan = [&](int level) {
          gTb.sliders = level < 1;
          gTb.dragPrefix = level < 2;
          gTb.snapLabel = level < 3;
          gTb.snapW = level < 4 ? 112.0f : 84.0f;
          gTb.dragW = level < 2 ? 150.0f : level < 4 ? 112.0f : 96.0f;
          const float group = 2 * (fh + 16) + 4 + 12 + (gTb.snapLabel ? snapLblW + 4 : 0.0f) + fh + 4 + gTb.snapW + 4 + gTb.dragW;
          const float zoom = gTb.sliders ? 2 * (14 + 64.0f) + 10 : fh * 2 + 2;
          gTb.groupX = rightX - zoom - 12 - fh - 12 - group;  // the group, the pads button, the zoom
          return gTb.groupX >= startX;
        };
        int level = 0;
        while (!plan(level) && level < 4) {  // (the last level is planned too, fitting or not)
          ++level;
        }
        if (gTb.groupX > startX) {
          ImGui::SetCursorScreenPos(ImVec2(gTb.groupX, ImGui::GetCursorScreenPos().y));
        }
      }
    } else {
    menuButton("plmenu", "Playlist options: edit, select, snap, view, picker panel, tracks", [&a]() { playlistOptionsMenu(a); });
    ImGui::SameLine(0, 4);
    // Magnet: the snap menu (right-click toggles snapping off / back on).
    if (ui::ToolButton("snapicon", ui::Icon::ToolSnap, st.snap != Snap::None, fh, i18n::N_("Snap"),
                       i18n::N_("click for the grid menu, right-click turns snapping off / on (Alt while dragging bypasses it)"), nullptr,
                       theme::rgb(theme::role::kSnap)))
      ImGui::OpenPopup("##snapmenu");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      if (st.snap != Snap::None) st.lastSnap = st.snap, st.snap = Snap::None;
      else st.snap = st.lastSnap == Snap::None ? Snap::Line : st.lastSnap;
    }
    if (ImGui::BeginPopup("##snapmenu")) {
      ImGui::TextDisabled("%s", i18n::tr("Snap to"));
      for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i) {
        std::string l = snapName(static_cast<Snap>(i));
        if ((static_cast<Snap>(i) == Snap::Line || static_cast<Snap>(i) == Snap::Cell) && st.effGrid > 0 && st.snap == static_cast<Snap>(i))
          l += "  (" + timeline::gridName(s, st.effGrid) + ")";
        if (ImGui::MenuItem(l.c_str(), nullptr, st.snap == static_cast<Snap>(i))) {
          st.snap = static_cast<Snap>(i);
          if (st.snap != Snap::None) st.lastSnap = st.snap;
        }
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 4);
    }
    // Q-12: one tool family, tooltips "Name (key): what it does" from the keymap.
    struct T { ui::Icon icon; Tool tool; const char* name; const char* desc; const char* key; uint32_t role; };
    const T tools[] = {
        {ui::Icon::ToolDraw, Tool::Draw, i18n::N_("Draw"), i18n::N_("click places the current clip source, drag moves, drag an edge to resize / trim, Shift+drag copies, right-click deletes"), "playlist.toolDraw", theme::role::kDraw},
        {ui::Icon::ToolPaint, Tool::Paint, i18n::N_("Paint"), i18n::N_("paint clips while dragging: the current source, one after another"), "playlist.toolPaint", theme::role::kPaint},
        {ui::Icon::ToolDelete, Tool::Erase, i18n::N_("Delete"), i18n::N_("click or drag over clips to delete them"), "playlist.toolDelete", theme::role::kDelete},
        {ui::Icon::ToolMute, Tool::Mute, i18n::N_("Mute"), i18n::N_("click or drag over clips to mute / unmute them"), "playlist.toolMute", theme::role::kMute},
        {ui::Icon::ToolSlip, Tool::Slip, i18n::N_("Slip"), i18n::N_("drag inside a clip to move its content without moving the clip"), "playlist.toolSlip", theme::role::kSlip},
        {ui::Icon::Stretch, Tool::Warp, i18n::N_("Warp"), i18n::N_("click an audio clip to add a warp marker, drag a marker to stretch the audio around it (Alt: off the grid, Shift: slide the audio under it), double-click or right-click removes it"), "playlist.toolWarp", theme::role::kWarp},
        {ui::Icon::ToolSlice, Tool::Slice, i18n::N_("Slice"), i18n::N_("click a clip to cut it, drag up / down to cut every clip the line crosses, drag sideways to cut out a region (Shift: all tracks)"), "playlist.toolSlice", theme::role::kSlice},
        {ui::Icon::ToolSelect, Tool::Select, i18n::N_("Select"), i18n::N_("drag a box (Ctrl+drag selects with any tool, Shift adds)"), "playlist.toolSelect", theme::role::kSelect},
        {ui::Icon::ToolZoom, Tool::Zoom, i18n::N_("Zoom"), i18n::N_("drag a range to fill the view, click zooms in, right-click zooms out"), "playlist.toolZoom", theme::role::kZoom},
        {ui::Icon::ToolPlay, Tool::Play, i18n::N_("Playback"), i18n::N_("hold to play from the pointer, drag to scrub"), "playlist.toolPlay", theme::role::kPlay},
        // R01-18: the Tracks layout's Fade and Marquee tools (its tool menu only).
        {ui::Icon::FadeIn, Tool::Fade, i18n::N_("Fade"), i18n::N_("drag from inside an audio region over its start or end: a fade in or out that long; Alt+click removes the fade"), "playlist.toolFade", theme::role::kWarp},
        {ui::Icon::Select, Tool::Marquee, i18n::N_("Marquee"), i18n::N_("drag across regions and tracks to mark a range: Delete removes it, a Pointer click inside cuts it out as regions of its own, a click elsewhere clears it"), "playlist.toolMarquee", theme::role::kSelect},
        // R05-17: the rest of the Tracks layout's tool menu.
        {ui::Icon::ListBullets, Tool::Text, i18n::N_("Text"), i18n::N_("click a region to rename it"), "playlist.toolText", theme::role::kSelect},
        {ui::Icon::CirclesMerged, Tool::Glue, i18n::N_("Join"), i18n::N_("click one of the selected regions: they join into one region"), "playlist.toolGlue", theme::role::kSlice},
        {ui::Icon::Headphones, Tool::Solo, i18n::N_("Solo"), i18n::N_("hold on a region: it plays from there with its track soloed, drag to scrub; let go and it stops"), "playlist.toolSolo", theme::role::kPlay},
        {ui::Icon::EnvelopePoints, Tool::AutoSelect, i18n::N_("Automation Select"), i18n::N_("drag in a track's automation to select its points: Delete removes them, a drag on a selected point moves them all"), "playlist.toolAutoSelect", theme::role::kSelect},
        {ui::Icon::CurveSmooth, Tool::AutoCurve, i18n::N_("Automation Curve"), i18n::N_("drag a line between two automation points up or down to bend it"), "playlist.toolAutoCurve", theme::role::kDraw}};
    // R01-18: the Tracks layout names and orders its tools as the reference's tool menu does: the
    // pointer first (its default), then the pencil, eraser and scissors.
    auto toolName = [&](const T& t) -> const char* {
      if (!lg) return t.name;
      switch (t.tool) {
        case Tool::Select: return i18n::N_("Pointer");
        case Tool::Draw: return i18n::N_("Pencil");
        case Tool::Erase: return i18n::N_("Eraser");
        case Tool::Slice: return i18n::N_("Scissors");
        case Tool::Warp: return i18n::N_("Flex");
        default: return t.name;
      }
    };
    auto toolDesc = [&](const T& t) -> const char* {
      if (lg && t.tool == Tool::Select)
        return i18n::N_("click selects a region, drag moves it, its lower corners resize it, the upper corners of an audio region fade it, a drag on empty space selects with a box, a click there deselects");
      if (lg && t.tool == Tool::Draw) return i18n::N_("click on empty space draws a region of the current pattern, drag moves");
      if (lg && t.tool == Tool::Warp)
        return i18n::N_("click an audio region to add a flex marker, drag a marker to stretch the audio around it, double-click removes it");
      return t.desc;
    };
    // R05-17: the reference's tool menu, in its order; Studio's Paint, Playback and Slip are not in it
    // (a Pencil Ctrl+Shift drag still slips a region's content).
    static const Tool lgOrder[] = {Tool::Select, Tool::Draw, Tool::Erase, Tool::Text, Tool::Slice, Tool::Glue, Tool::Solo,
                                   Tool::Mute, Tool::Zoom, Tool::Fade, Tool::AutoSelect, Tool::AutoCurve, Tool::Marquee, Tool::Warp};
    std::vector<const T*> shownTools;
    if (lg) {
      for (Tool want : lgOrder)
        for (const auto& t : tools)
          if (t.tool == want) shownTools.push_back(&t);
    } else {
      for (const auto& t : tools)
        if (t.tool != Tool::Fade && t.tool != Tool::Marquee && t.tool < Tool::Text) shownTools.push_back(&t);
    }
    const Tool leftTool = gCmdSwap ? gCmdSaved : st.tool;  // the toolbar shows the chosen tools, not Cmd's swap
    if (calm) {  // the tool in use; a click lists every tool with its key
      // R01-18: two menus, as the reference's: the Left-click tool, then (Tracks layout) the
      // Command-click tool, the one a click with Cmd held uses.
      auto toolMenu = [&](const char* id, const char* pop, Tool& which, bool second, bool openNow) {
        const T* cur = shownTools.front();
        for (const T* t : shownTools)
          if (which == t->tool) cur = t;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = fh + 16;
        ImGui::InvisibleButton(id, ImVec2(bw, fh));
        const bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen(pop);
        static int openFrame = -1;  // the key that opened the menu (T) picks nothing
        if (openNow) openFrame = ImGui::GetFrameCount();
        if (ImGui::IsItemClicked() || openNow) ImGui::OpenPopup(pop);
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        tdl->AddRectFilled(p, ImVec2(p.x + bw, p.y + fh), open || hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, theme::kRadius);
        ui::drawIcon(tdl, cur->icon, ImVec2(p.x + fh * 0.5f + 1, p.y + fh * 0.5f), fh - 10, lg ? P.text : theme::rgb(cur->role));  // X2: no role colours in Smooth
        ui::drawIcon(tdl, ui::Icon::ChevronDown, ImVec2(p.x + bw - 9, p.y + fh * 0.5f), 10, P.textDim);
        const char* k = second ? nullptr : keys::label(cur->key);
        if (second)
          ui::Hint(std::string(i18n::tr("Command-click tool")) + ": " + i18n::tr(toolName(*cur)) + "  " + i18n::tr("(hold Cmd to use it; click: all tools)"));
        else
          ui::Hint(std::string(i18n::tr(toolName(*cur))) + (k ? std::string(" (") + k + ")" : std::string()) + ": " + i18n::tr(toolDesc(*cur)) + "  " + i18n::tr("(click: all tools)"));
        if (!second) ui::noteActiveTool("playlist", static_cast<int>(cur->tool), toolName(*cur), cur->key);
        ImGui::SetNextWindowPos(ImVec2(p.x, p.y + fh + 2));
        if (ImGui::BeginPopup(pop)) {
          if (lg) keys::setCapturing(true);  // the letters pick tools here, not their commands
          for (const T* tp : shownTools) {
            const T& t = *tp;
            ImGui::PushID(t.key);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float lh = ImGui::GetTextLineHeight();
            if (ImGui::Selectable("##t", which == t.tool, 0, ImVec2(200, 0))) which = t.tool;
            if (ImGui::IsItemHovered()) ui::Hint(i18n::tr(toolDesc(t)));
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            ui::drawIcon(pdl, t.icon, ImVec2(q.x + 8, q.y + lh * 0.5f), lh - 2, lg ? (which == t.tool ? P.accent : P.textDim) : theme::rgb(t.role));  // R09-09
            pdl->AddText(ImVec2(q.x + 22, q.y), P.text, i18n::tr(toolName(t)));
            char letter[2] = {lg ? toolLetter(t.tool) : '\0', '\0'};
            const char* kk = lg ? (letter[0] ? letter : nullptr) : keys::label(t.key);
            if (kk) pdl->AddText(ImVec2(q.x + 200 - ImGui::CalcTextSize(kk).x, q.y), P.textDim, kk);
            if (lg && letter[0] && ImGui::GetFrameCount() != openFrame && ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + (letter[0] - 'A')), false)) {
              which = t.tool;
              ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
          }
          ImGui::EndPopup();
        }
      };
      Tool chosen = leftTool;
      toolMenu("##pltoolmenu", "##pltoolpop", chosen, false, gToolMenu);
      gToolMenu = false;
      if (chosen != leftTool) {
        if (gCmdSwap) gCmdSaved = chosen;
        else st.tool = chosen;
      }
      if (lg) {
        ImGui::SameLine(0, 4);
        Tool second = st.cmdTool;
        toolMenu("##pltoolmenu2", "##pltoolpop2", second, true, false);
        if (second != st.cmdTool) {
          if (gCmdSwap) st.tool = second;
          st.cmdTool = second;
        }
      }
      ImGui::SameLine(0, 12);
      // Snap: its grid as a short text menu. R01-26 (Tracks layout): a switch before it turns snapping
      // off and back on to the grid it had, as the reference's snap switch does. B2 (checkpoint 48): the
      // label "Snap:", then a power switch in the accent blue, as the reference's.
      if (lg) {
        const bool snapOn = st.snap != Snap::None;
        if (gTb.snapLabel) {  // (cp54: the first part a narrow toolbar drops)
          const char* lbl = i18n::tr("Snap:");
          const ImVec2 lp = ImGui::GetCursorScreenPos();
          ImGui::GetWindowDrawList()->AddText(ImVec2(lp.x, std::round(lp.y + (fh - ImGui::GetFontSize()) * 0.5f)), P.textDim, lbl);
          ImGui::Dummy(ImVec2(ImGui::CalcTextSize(lbl).x, fh));
          ImGui::SameLine(0, 4);
        }
        if (ui::IconButton("##plsnapon", ui::Icon::Power, snapOn, fh, snapOn ? "Snap is on: click to place freely" : "Snap is off: click to snap to the grid again",
                           P.accent)) {
          if (snapOn) st.snap = Snap::None;
          else st.snap = st.lastSnap != Snap::None ? st.lastSnap : Snap::Line;
        }
        ImGui::SameLine(0, 4);  // (cp54: 4 px inside a group; was 2)
      }
      // B5 (checkpoint 49): Smooth's pop-ups flat, as the reference's: a subtle grey fill with 5 px
      // corners and a small up-and-down chevron at the right (they were heavy dark fields).
      auto calmCombo = [&](const char* id, const char* preview, float width) {
        if (!lg) return ui::BeginCombo(id, preview, width);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const ImU32 face = theme::mix(P.surface700, P.text, 0.06f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, face);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::mix(face, P.text, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme::mix(face, P.text, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_Border, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(width);
        const std::string pv = theme::fitText(preview, width - 16 - 14);
        const bool open = ImGui::BeginCombo(id, pv.c_str(), ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        const float cx = std::round(p.x + width - 10), cy = std::round(p.y + h * 0.5f);
        cdl->AddTriangleFilled(ImVec2(cx - 3, cy - 1.5f), ImVec2(cx + 3, cy - 1.5f), ImVec2(cx, cy - 5), P.textDim);
        cdl->AddTriangleFilled(ImVec2(cx + 3, cy + 1.5f), ImVec2(cx - 3, cy + 1.5f), ImVec2(cx, cy + 5), P.textDim);
        return open;
      };
      // B4: in Smooth the adaptive grid reads "Smart" (the reference's word for it is its own).
      auto calmSnapName = [&](Snap sn) -> const char* { return lg && sn == Snap::Line ? i18n::tr("Smart") : snapName(sn); };
      std::string snapLabel = lg ? std::string(calmSnapName(st.snap)) : std::string(i18n::tr("Snap")) + ": " + snapName(st.snap);
      if (calmCombo("##plcalmsnap", snapLabel.c_str(), lg ? gTb.snapW : 128)) {
        for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
          if (ImGui::Selectable(calmSnapName(static_cast<Snap>(i)), st.snap == static_cast<Snap>(i))) {
            st.snap = static_cast<Snap>(i);
            if (st.snap != Snap::None) st.lastSnap = st.snap;
          }
        ImGui::EndCombo();
      }
      ui::Hint("Snap: where clips land while dragging (hold Alt while dragging to bypass it)");
      ImGui::SameLine(0, 2);
      if (lg) {  // R01-26: Drag: what a region dropped over another does (the reference's drag modes)
        // (Overlapping audio regions crossfade by themselves, so Overlap is the reference's X-Fade too.)
        static const char* const kDragNames[2] = {i18n::N_("Overlap"), i18n::N_("No overlap")};
        static const char* const kDragHints[2] = {
            i18n::N_("A region dropped over another lies on it: the later one plays, and audio crossfades where they meet"),
            i18n::N_("A region dropped over another shortens the earlier one, so none overlap")};
        const int dm = std::clamp(static_cast<int>(a.settings.getNumber("ui.logicDragMode")), 0, 1);
        const std::string dragLabel = gTb.dragPrefix ? std::string(i18n::tr("Drag")) + ": " + i18n::tr(kDragNames[dm]) : std::string(i18n::tr(kDragNames[dm]));
        ImGui::SameLine(0, 4);
        if (calmCombo("##plcalmdrag", dragLabel.c_str(), gTb.dragW)) {
          for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(i18n::tr(kDragNames[i]), dm == i)) a.settings.set("ui.logicDragMode", i);
            if (ImGui::IsItemHovered()) ui::Hint(i18n::tr(kDragHints[i]));
          }
          ImGui::EndCombo();
        }
        ui::Hint(i18n::tr(kDragHints[dm]));
        ImGui::SameLine(0, 2);
      }
    }
    for (const T* tp : shownTools) {
      const T& t = *tp;
      if (calm) break;
      if (ui::ToolButton(t.key, t.icon, leftTool == t.tool, fh, toolName(t), toolDesc(t), t.key, theme::rgb(t.role))) st.tool = t.tool;
      if (leftTool == t.tool) ui::noteActiveTool("playlist", static_cast<int>(t.tool), toolName(t), t.key);
      ImGui::SameLine(0, 2);
    }
    if (!lg) {  // K-4: auto scrolling (the view follows the song position), this window's own switch (Smooth: after View)
      ui::ToolbarDivider();
      const bool follow = a.settings.getBool("ui.autoScroll");
      if (ui::IconButton("##plfollow", ui::Icon::ScrollTo, follow, fh,
                         follow ? "Auto scrolling is on: the playlist follows the song position while it plays (click to keep the view still)"
                                : "Auto scrolling is off: the view stays where you put it while the song plays (click to follow the song position)",
                         theme::rgb(theme::role::kView)))
        a.settings.set("ui.autoScroll", !follow);
      ImGui::SameLine(0, 2);
    }
    if (calm) {  // zoom at the bar's right end
      // R01-26: the Tracks layout zooms with two sliders, the track height and the time scale (216,
      // 055), when they fit; else the zoom buttons.
      const float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
      const float slW = 64, slGap = 10, slidersW = 2 * (14 + slW) + slGap;
      const bool sliders = lg && gTb.sliders;  // (cp54: as the toolbar's plan chose)
      const float zx = sliders ? rightX - slidersW : rightX - fh * 2 - 2;
      const float barX = ImGui::GetCursorScreenPos().x;
      auto hintWithKeyPl = [](const char* t, const char* c) {
        const char* k = keys::label(c);
        return std::string(i18n::tr(t)) + (k ? std::string(" (") + k + ")" : std::string());
      };
      if (zx - fh - 12 > barX + 8) {  // the pads grid beside the tracks
        ImGui::SetCursorScreenPos(ImVec2(zx - fh - 12, ImGui::GetCursorScreenPos().y));
        if (ui::IconButton("##plgrid", ui::Icon::Rack, a.showLivePads, fh, hintWithKeyPl("Pads beside the tracks", "view.toggleMain").c_str()))
          layoutui::toggleMainView(a);
        ImGui::SameLine(0, 8);
      }
      if (sliders) {
        // f: where the value sits in its range (0..1); a drag or a click sets it.
        auto zslider = [&](const char* id, bool vertical, float f, const char* hint) -> float {
          const ImVec2 p = ImGui::GetCursorScreenPos();
          ImDrawList* zdl = ImGui::GetWindowDrawList();
          const float cy = std::round(p.y + fh * 0.5f);
          // the glyph: a double arrow up and down, or left and right
          const ImU32 gc = P.textDim;
          if (vertical) {
            zdl->AddLine(ImVec2(p.x + 6, cy - 6), ImVec2(p.x + 6, cy + 6), gc, 1.2f);
            zdl->AddTriangleFilled(ImVec2(p.x + 3, cy - 4), ImVec2(p.x + 9, cy - 4), ImVec2(p.x + 6, cy - 8), gc);
            zdl->AddTriangleFilled(ImVec2(p.x + 3, cy + 4), ImVec2(p.x + 9, cy + 4), ImVec2(p.x + 6, cy + 8), gc);
          } else {
            zdl->AddLine(ImVec2(p.x, cy), ImVec2(p.x + 12, cy), gc, 1.2f);
            zdl->AddTriangleFilled(ImVec2(p.x + 2, cy - 3), ImVec2(p.x + 2, cy + 3), ImVec2(p.x - 2, cy), gc);
            zdl->AddTriangleFilled(ImVec2(p.x + 10, cy - 3), ImVec2(p.x + 10, cy + 3), ImVec2(p.x + 14, cy), gc);
          }
          const ImVec2 t0(p.x + 16, p.y);
          ImGui::SetCursorScreenPos(t0);
          ImGui::InvisibleButton(id, ImVec2(slW - 2, fh));
          const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
          float out = -1;
          if (act) out = std::clamp((io.MousePos.x - t0.x - 6) / std::max(1.0f, slW - 14), 0.0f, 1.0f);
          const float shown = out >= 0 ? out : std::clamp(f, 0.0f, 1.0f);
          const float tx = std::round(t0.x + 6 + (slW - 14) * shown);
          zdl->AddRectFilled(ImVec2(t0.x, cy - 2), ImVec2(t0.x + slW - 2, cy + 2), theme::mix(P.surface800, P.ink, 0.45f), 2.0f);
          zdl->AddRectFilled(ImVec2(t0.x, cy - 2), ImVec2(tx, cy + 2), P.hot, 2.0f);
          zdl->AddCircleFilled(ImVec2(tx, cy), 6.0f, hov || act ? P.text : theme::mix(P.text, P.surface800, 0.15f), 16);
          if (hov || act) ui::Hint(hint);
          return out;
        };
        ImGui::SetCursorScreenPos(ImVec2(zx, ImGui::GetCursorScreenPos().y));
        const float hf = (st.trackHeight - kTrackHMin) / (kTrackHMax - kTrackHMin);
        if (const float v = zslider("##plzv", true, hf, i18n::tr("Track height")); v >= 0) st.trackHeight = kTrackHMin + v * (kTrackHMax - kTrackHMin);
        ImGui::SameLine(0, slGap);
        const double zmin = timeline::clampZoom(s, 1e-12), zmax = timeline::clampZoom(s, 1e12);
        const double lz = std::log(std::max(1e-12, zmax / zmin));
        const float zf = lz > 0 ? static_cast<float>(std::log(std::max(zmin, static_cast<double>(st.pxPerTick)) / zmin) / lz) : 0.5f;
        if (const float v = zslider("##plzh", false, zf, hintWithKeyPl("Zoom", "playlist.zoomIn").c_str()); v >= 0 && lz > 0) {
          const double want = zmin * std::exp(lz * static_cast<double>(v));
          st.zoomRequest = static_cast<float>(want / std::max(1e-12, static_cast<double>(st.pxPerTick)));
        }
      } else if (zx > barX + 8) {
        ImGui::SetCursorScreenPos(ImVec2(zx, ImGui::GetCursorScreenPos().y));
        if (ui::IconButton("##plzout", ui::Icon::Minus, false, fh, hintWithKeyPl("Zoom out", "playlist.zoomOut").c_str())) st.zoomRequest /= 1.5f;
        ImGui::SameLine(0, 2);
        if (ui::IconButton("##plzin", ui::Icon::Plus, false, fh, hintWithKeyPl("Zoom in", "playlist.zoomIn").c_str())) st.zoomRequest *= 1.5f;
      }
    }
    // Breadcrumb: Playlist > arrangement > current clip source (each opens its menu).
    if (!calm) {
    ImGui::SameLine(0, theme::kS3);
    const pltools::Source src = currentSource(a);
    const std::string arrName = arrangementName(s, s.arrangements.empty() ? 0 : s.currentArrangement);
    const std::string srcName = pltools::sourceValid(s, src) ? sourceName(s, src) : std::string("(none)");
    auto crumb = [&](const char* id, const std::string& text, bool clickable, ImU32 col, const char* hint) {
      const float room = ImGui::GetContentRegionAvail().x;
      const float tw = std::min(a.fontRegular->CalcTextSizeA(theme::kFontBody, 1e9f, 0, text.c_str()).x, 220.0f);
      if (room < tw + 24) return false;
      ImVec2 p = ImGui::GetCursorScreenPos();
      bool pressed = clickable ? ImGui::InvisibleButton(id, ImVec2(tw + 4, fh)) : (ImGui::Dummy(ImVec2(tw + 4, fh)), false);
      const bool hov = clickable && ImGui::IsItemHovered();
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      if (hov) tdl->AddRectFilled(p, ImVec2(p.x + tw + 4, p.y + fh), theme::withAlpha(P.paper, 0.06f), theme::kRadiusSm);
      theme::textEllipsis(tdl, a.fontRegular, theme::kFontBody, ImVec2(p.x + 2, std::round(p.y + (fh - theme::kFontBody) * 0.5f)), tw + 1,
                          hov ? P.text : col, text.c_str());
      if (hint) ui::Hint(hint);
      ImGui::SameLine(0, 0);
      // Separator chevron.
      ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(14, fh));
      ui::drawIconFit(tdl, ui::Icon::Right, ImVec2(q.x + 5, q.y + fh * 0.5f - 4), ImVec2(q.x + 9, q.y + fh * 0.5f + 4), P.textFaint);
      ImGui::SameLine(0, 0);
      return pressed;
    };
    crumb("##crPl", "Playlist", false, P.textDim, nullptr);
    if (crumb("##crArr", arrName, true, P.text, "Arrangement: switch, add, duplicate, rename or delete arrangements")) ImGui::OpenPopup("##arrmenu");
    if (ImGui::BeginPopup("##arrmenu")) {
      arrangementMenu(a);
      ImGui::EndPopup();
    }
    if (crumb("##crSrc", srcName, true, pltools::sourceValid(s, src) ? P.text : P.textFaint,
              "Current clip source: what clicks with draw / paint place (pick one here or in the picker panel)"))
      ImGui::OpenPopup("##srcmenu");
    if (ImGui::BeginPopup("##srcmenu")) {
      sourceMenu(a);
      ImGui::EndPopup();
    }
    }
    ImGui::Dummy(ImVec2(0, 0));
  }
  ui::EndToolbar();
  }

  // ---------------- clip source picker (patterns / audio / automation) ----------------
  const bool showPicker = st.pickerOpen && winW >= 560 && !liveLook && !lg;
  if (showPicker) {
    const float pickerW = std::clamp(winW * 0.14f, 124.0f, 172.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, theme::kS1));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS2, 2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    ImGui::BeginChild("##picker", ImVec2(pickerW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    {  // three filter tabs (J-38): one segmented control in the ruler band, so
       // the list starts level with the first track and the corner's divider runs on
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const float bandH = 14.0f + 26.0f + meterui::laneHeight(a.doc.song()) - theme::kS1;  // the ruler band (rulerH below) minus the child padding
      const ImVec2 q0 = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x, th = 22.0f, ty = std::round(q0.y + (bandH - th) * 0.5f) - 1;
      const float tw = std::floor((w - 4) / 3.0f);
      pdl->AddRectFilled(ImVec2(q0.x, ty), ImVec2(q0.x + w, ty + th), P.dark ? P.well : P.surface950, 5.0f);
      pdl->AddRect(ImVec2(q0.x, ty), ImVec2(q0.x + w, ty + th), theme::withAlpha(P.ink, P.dark ? 0.55f : 0.18f), 5.0f);
      static const char* const tabHints[] = {"Pattern clip sources", "Audio clip sources (sample channels)", "Automation clip sources"};
      static const ClipKind tabKinds[] = {ClipKind::Pattern, ClipKind::Audio, ClipKind::Automation};
      bool openOpts = false;
      for (int k = 0; k < 3; ++k) {
        ImGui::PushID(k);
        const ImVec2 c0(q0.x + 2 + static_cast<float>(k) * tw, ty + 2), c1(c0.x + tw, ty + th - 2);
        devGeom.tabX[k] = (c0.x + c1.x) * 0.5f, devGeom.tabY = (c0.y + c1.y) * 0.5f;
        ImGui::SetCursorScreenPos(c0);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, th - 4))) st.pickerTab = k;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) openOpts = true;  // K-235: the list's options
        const bool hov = ImGui::IsItemHovered(), on = st.pickerTab == k;
        ui::Hint(tabHints[k]);
        if (hov) ImGui::SetTooltip("%s", tabHints[k]);
        if (on) pdl->AddRectFilled(c0, c1, P.dark ? P.surface600 : P.surface700, 3.5f);
        else if (hov) pdl->AddRectFilled(c0, c1, theme::withAlpha(P.paper, P.dark ? 0.06f : 0.5f), 3.5f);
        drawKindGlyph(pdl, tabKinds[k], ImVec2(std::round((c0.x + c1.x) * 0.5f), std::round((c0.y + c1.y) * 0.5f)), 12, on ? P.hot : hov ? P.text : P.textDim);
        ImGui::PopID();
      }
      if (openOpts) ImGui::OpenPopup("##pickopts");
      if (ImGui::BeginPopup("##pickopts")) {
        pickeraudition::toggleItem(a);
        ImGui::EndPopup();
      }
      // The corner's bottom rule continues under the tabs.
      pdl->AddLine(ImVec2(q0.x - theme::kS1, q0.y + bandH - 1), ImVec2(q0.x + w + theme::kS1, q0.y + bandH - 1), P.dark ? P.ink : P.lineStrong);
      ImGui::SetCursorScreenPos(q0);
      ImGui::Dummy(ImVec2(w, bandH - ImGui::GetStyle().ItemSpacing.y));
    }
    const float rowH = 22.0f;
    // Rows: a mini clip (colour strip + name) like the clips they place.
    // J-82: the selection belongs to one tab.
    static int selTab = -1;
    if (selTab != st.pickerTab) st.pickerSel.clear(), selTab = st.pickerTab;
    static ObjectId anchor = kNoId;  // Shift+click range start
    const std::vector<ObjectId> rows = pickerRows(s, st.pickerTab);
    devGeom.rows.clear();
    // `name` is a copy: a row's menu may make a whole-song edit that replaces the list it came from.
    auto pickerRow = [&](const pltools::Source& ps, const std::string name, Rgb color, const char* hint, const std::function<void()>& menu) {
      ImVec2 q = ImGui::GetCursorScreenPos();
      const bool sel = currentSource(a) == ps || st.pickerSel.count(ps.id) > 0;
      float w = ImGui::GetContentRegionAvail().x;
      devGeom.rows.push_back({static_cast<unsigned>(ps.id), q.x + w * 0.5f, q.y + rowH * 0.5f});
      if (ImGui::InvisibleButton("##pp", ImVec2(w, rowH))) {
        st.pickerFocus = true;
        if (io.KeyShift && anchor != kNoId) {  // range from the anchor
          auto i0 = std::find(rows.begin(), rows.end(), anchor), i1 = std::find(rows.begin(), rows.end(), ps.id);
          if (i0 != rows.end() && i1 != rows.end()) {
            if (i0 > i1) std::swap(i0, i1);
            st.pickerSel = std::set<ObjectId>(i0, i1 + 1);
          }
        } else if (io.KeyCtrl || io.KeySuper) {  // add / remove one row
          const pltools::Source cur = currentSource(a);
          if (st.pickerSel.empty() && cur.kind == ps.kind) st.pickerSel.insert(cur.id);
          if (!st.pickerSel.erase(ps.id)) st.pickerSel.insert(ps.id);
          anchor = ps.id;
        } else {
          st.pickerSel = {ps.id};
          anchor = ps.id;
          setSource(a, ps);
        }
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        st.pickerFocus = true;
        if (!st.pickerSel.count(ps.id)) st.pickerSel = {ps.id}, anchor = ps.id;
      }
      // K-235: pressing a row auditions its source until the button is let go (or another row
      // is pressed): an audio source plays its sample, a pattern plays from its start while the
      // transport is stopped (the transport's mode and position come back after).
      if (ImGui::IsItemActivated() && !io.KeyShift && !io.KeyCtrl && !io.KeySuper) pickeraudition::start(a, ps.kind, ps.id);
      if (ImGui::IsItemDeactivated()) pickeraudition::stop(a);
      const bool hov = ImGui::IsItemHovered();

      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSource(a, ps);
      if (ImGui::BeginDragDropSource()) {
        const ClipSrcPayload pl{static_cast<int>(ps.kind), ps.id};
        ImGui::SetDragDropPayload("GL_CLIPSRC", &pl, sizeof pl);
        ui::DragPreview(ps.kind == ClipKind::Audio ? ui::Icon::Wave : ui::Icon::Pattern, name.c_str(), theme::content(color));
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginPopupContextItem("##pmenu")) {
        menu();
        if (ps.kind != ClipKind::Automation) ImGui::Separator(), pickeraudition::toggleItem(a);  // K-235
        ImGui::EndPopup();
      }
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const ImU32 pc = theme::rgb(color);
      pdl->AddRectFilled(q, ImVec2(q.x + w, q.y + rowH), sel ? theme::mix(P.surface700, pc, 0.18f) : hov ? P.surface700 : theme::withAlpha(P.surface800, 0.55f),
                         theme::kRadiusSm);
      pdl->AddRectFilled(q, ImVec2(q.x + 4, q.y + rowH), pc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      if (sel) pdl->AddRect(q, ImVec2(q.x + w, q.y + rowH), theme::withAlpha(P.hot, 0.55f), theme::kRadiusSm);
      theme::textEllipsis(pdl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 12, std::round(q.y + (rowH - 13) * 0.5f) - 1), w - 18,
                          sel && P.dark ? P.paper : P.text, name.c_str());
      ui::Hint(hint);
    };
    int shown = 0;
    // J-208: on the pattern tab a centred "+" follows the last pattern and makes a new one,
    // as the New pattern command does. When the rows run past the panel it stays on the
    // panel's bottom edge (submitted first there, so it takes the pointer over the rows).
    devGeom.plusX = devGeom.plusY = 0;
    const float plusS = rowH - 2;
    auto plusButton = [&](ImVec2 at, bool pinned) {
      const float w = ImGui::GetContentRegionAvail().x;
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const ImVec2 back = ImGui::GetCursorScreenPos();
      if (pinned) {  // a footer band in the panel's colour, so the rows pass under it
        pdl->AddRectFilled(ImVec2(at.x - theme::kS1, at.y - 3), ImVec2(at.x + w + theme::kS1, at.y + plusS + theme::kS1), P.surface900);
        pdl->AddLine(ImVec2(at.x, at.y - 3), ImVec2(at.x + w, at.y - 3), theme::withAlpha(P.line, 0.8f));
      }
      const ImVec2 c(std::round(at.x + w * 0.5f), std::round(at.y + plusS * 0.5f));
      devGeom.plusX = c.x, devGeom.plusY = c.y;
      ImGui::SetCursorScreenPos(ImVec2(c.x - plusS * 0.5f, at.y));
      if (ui::IconButton("##newpat", ui::Icon::Plus, false, plusS, "New pattern: adds one after the last and selects it")) {
        const ObjectId id = a.doc.addPattern(ops::nextPatternName(a.doc.song()));
        if (id) {
          setSource(a, {ClipKind::Pattern, id});
          st.pickerSel = {id};
          anchor = id;
          st.pickerFocus = true;
        }
      }
      if (pinned) {  // the band's sides take the clicks too (no row under them)
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y - 3));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##newpatband", ImVec2(w, plusS + 3));
      }
      if (pinned) ImGui::SetCursorScreenPos(back);
    };
    bool plusPinned = false;
    if (st.pickerTab == 0 && !s.patterns.empty()) {
      const float sp = ImGui::GetStyle().ItemSpacing.y;
      const float top = ImGui::GetCursorScreenPos().y;
      const float end = top + static_cast<float>(s.patterns.size()) * (rowH + sp) + 2;
      const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - ImGui::GetStyle().WindowPadding.y;
      if (end + plusS > bottom) {
        plusPinned = true;
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        pdl->ChannelsSplit(2);  // the footer is drawn over the rows that follow
        pdl->ChannelsSetCurrent(1);
        plusButton(ImVec2(ImGui::GetCursorScreenPos().x, std::max(top, bottom - plusS)), true);
        pdl->ChannelsSetCurrent(0);
      }
    }
    const uint64_t pickRev = a.doc.revision();
    if (st.pickerTab == 0) {
      for (size_t i = 0, n = s.patterns.size(); i < n && a.doc.revision() == pickRev; ++i) {  // a menu's edit: the rest next frame
        const auto& p = s.patterns[i];
        ImGui::PushID(static_cast<int>(p.id));
        const ObjectId pid = p.id;
        pickerRow({ClipKind::Pattern, p.id}, p.name, p.color, "Click: current source \xC2\xB7 drag into the playlist \xC2\xB7 double-click: edit \xC2\xB7 right-click: menu",
                  [&a, pid]() { patternSourceMenu(a, pid); });
        ImGui::PopID();
        ++shown;
      }
    } else if (st.pickerTab == 1) {
      for (size_t i = 0, n = s.channels.size(); i < n && a.doc.revision() == pickRev; ++i) {  // a menu's edit: the rest next frame
        const auto& ch = s.channels[i];
        if (!isAudioSource(ch)) continue;
        ImGui::PushID(static_cast<int>(ch.id));
        const ObjectId cid = ch.id;
        pickerRow({ClipKind::Audio, ch.id}, ch.name, ch.color,
                  "Click: current source \xC2\xB7 Ctrl / Shift+click: select several \xC2\xB7 drag into the playlist \xC2\xB7 double-click: channel settings \xC2\xB7 right-click: menu",
                  [&a, cid]() { audioSourceMenu(a, cid); });
        ImGui::PopID();
        ++shown;
      }
    } else {
      for (size_t i = 0, n = s.automations.size(); i < n && a.doc.revision() == pickRev; ++i) {  // a menu's edit: the rest next frame
        const auto& ac = s.automations[i];
        ImGui::PushID(static_cast<int>(ac.id));
        const ObjectId aid = ac.id;
        pickerRow({ClipKind::Automation, ac.id}, ac.name, ac.color,
                  "Click: current source \xC2\xB7 Ctrl / Shift+click: select several \xC2\xB7 drag into the playlist \xC2\xB7 double-click: edit \xC2\xB7 right-click: menu",
                  [&a, aid]() { automationSourceMenu(a, aid); });
        ImGui::PopID();
        ++shown;
      }
    }
    if (shown == 0) {
      ImGui::Dummy(ImVec2(0, 4));
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.textFaint), "%s",
                         st.pickerTab == 1 ? "No audio yet: drop samples from the browser onto the playlist"
                                           : st.pickerTab == 2 ? "No automation yet: right-click a control \xE2\x86\x92 Create automation clip"
                                                               : "No patterns");
      ImGui::PopTextWrapPos();
    }
    if (st.pickerTab == 0) {
      if (plusPinned) {
        ImGui::GetWindowDrawList()->ChannelsMerge();
        ImGui::Dummy(ImVec2(0, plusS + 4));  // scrolled to the end, the last row sits above the "+"
      } else {
        ImGui::Dummy(ImVec2(0, 2));
        plusButton(ImGui::GetCursorScreenPos(), false);
      }
    }
    const bool pickerHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    ImGui::EndChild();
    // J-97: browser items dropped on the picker become clip sources, as on the
    // rack's free area: samples / presets / instruments a new channel, MIDI a pattern.
    if (ImGui::BeginDragDropTarget()) {
      browserui::acceptFileDrop(a, browse::DropTarget::Rack, {});
      browserui::acceptPluginDrop(a, browse::DropTarget::Rack, {});
      ImGui::EndDragDropTarget();
    }
    ImGui::PopStyleVar(2);
    // J-82: a click anywhere else in the playlist takes the focus from the picker.
    if (!pickerHovered && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
      st.pickerFocus = false;
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
  const ImGuiID arrId = ImGui::GetItemID();
  const ImRect arrRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Left to right: a slim vertical scroller, the track headers, the grid. Above
  // the headers: two rows of view buttons (FL's header corner); above the grid:
  // a slim horizontal scroller, then the ruler.
  // K-112c: the Live layout turns it around: the lanes, then the track headers, then the scroller;
  // the horizontal scroller is the song overview strip, and a minutes ruler runs under the lanes.
  const float vbarW = lg ? 0.0f : 10.0f, hbarH = liveLook ? livelook::kOverviewH : lg ? 0.0f : 14.0f;
  const float headW = liveLook ? livelook::headerWidth(a)
                     : lg     ? std::round(std::min(avail.x * 0.45f, avail.x >= 900 ? std::clamp(avail.x * 0.3f, 330.0f, 380.0f) : 260.0f))  // (R10-06: room for R I and a compact slider)
                               : vbarW + (st.wideHeaders ? std::clamp(avail.x * 0.2f, 150.0f, 230.0f) : std::clamp(avail.x * 0.13f, 96.0f, 150.0f));
  // K-179: a thin meter lane on top of the ruler while the song has meter changes.
  const float laneH = meterui::laneHeight(s);
  // K-112c (round 20): Live's ruler stacks three rows, as every ref's arrangement ruler (280, 093,
  // 010, 018): the bar numbers, the loop brace, then the locators (kLiveNumH, kLiveBraceH, the rest).
  constexpr float kLiveNumH = 14.0f, kLiveBraceH = 11.0f;
  const float markLaneH = lg && a.settings.getBool("ui.logicGlobalTracks") ? 24.0f : 0.0f;  // C8: 24 px  // R01-15: G hides it  // K-112b: the markers lane under the bar numbers (Logic)
  const float rulerH = hbarH + (liveLook ? 40.0f : 26.0f) + laneH + markLaneH;  // cp54: Smooth's ruler one 26 px tier (C7's two were 34; the lower one stood empty)
  const float rLaneY = origin.y + hbarH;  // meter lane top
  const float rY = rLaneY + laneH;        // ruler top (bar numbers, markers)
  const float fullX1 = origin.x + avail.x, timeRulerH = liveLook ? livelook::kTimeRulerH : 0.0f;
  const ImVec2 gMin(liveLook ? origin.x : origin.x + headW, origin.y + rulerH), gMax(liveLook ? fullX1 - headW : fullX1, origin.y + avail.y - timeRulerH);
  const float headX0 = liveLook ? gMax.x : origin.x + vbarW;  // the header column
  const float headX1 = liveLook ? fullX1 - vbarW : gMin.x;
  const float vbarX0 = liveLook ? headX1 : origin.x;            // the vertical scroller
  const float rowX0 = std::min(headX0, gMin.x), rowX1 = std::max(headX1, gMax.x);  // a whole track row, header and lane
  // K-112c (round 39): in Clips the rows are the tracks in use, the clip grid's columns one for one
  // (the empty tracks after them stay in the song for Studio's playlist).
  const int nTracks = liveLook ? trackmodes::usedTrackCount(s) : static_cast<int>(s.tracks.size());
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
  if (liveLook && !livelook::automationLanesShown()) {  // N52-10: A hides Clips' automation lanes
    const std::vector<char> lanes = trackmodes::automationLanes(s, nTracks);
    for (size_t i = 0; i < lanes.size(); ++i)
      if (lanes[i]) folded[i] = 1;
  }
  if (liveLook) {  // N53-04: no row for an empty track another layout left between tracks in use
    const std::vector<char> gaps = trackmodes::gapTracks(s, nTracks);
    for (size_t i = 0; i < gaps.size(); ++i)
      if (gaps[i]) folded[i] = 1;
  }
  const int foldHead = lg && fold.on ? fold.head() : -1;  // K-112B (checkpoint 46): the Automation folder's row
  const bool foldShut = foldHead >= 0 && !fold.open;
  const float foldHdrH = foldHead >= 0 && fold.open && foldHead < nTracks ? 24.0f : 0.0f;  // R08-10
  // R08-03: the numbers count the rows on screen (tracks shown as lanes elsewhere take none), so they run on.
  std::vector<int> rowNum(static_cast<size_t>(nTracks), 0);
  for (int i = 0, n = 0; i < nTracks; ++i)
    if (!(lg && fold.on && fold.takesNoRoom(i))) rowNum[static_cast<size_t>(i)] = ++n;
  if (lg && fold.on)
    for (int i = 0; i < nTracks; ++i) {
      if (fold.takesNoRoom(i)) folded[static_cast<size_t>(i)] = 1;
      if (fold.open && fold.isMember(i)) groupDepth[static_cast<size_t>(i)] = std::max(groupDepth[static_cast<size_t>(i)], 1);
    }
  auto rowScale = [&](int i) {
    if (foldShut && i == foldHead) return 0.75f;  // the closed folder: a compact row
    // Checkpoint 55 (gap 5): in Clips the height the person dragged the row to (view only, per person).
    if (liveLook) return std::clamp(livelook::rowScale(a, i, s.tracks[static_cast<size_t>(i)].height), 0.25f, 4.0f);
    return std::clamp(s.tracks[static_cast<size_t>(i)].height, 0.25f, 4.0f);
  };
  // K-123: take lanes under a track that shows them (one per take; not in the Clips look).
  std::vector<int> takeLaneN(static_cast<size_t>(nTracks), 0);
  if (!liveLook)
    for (int i = 0; i < nTracks; ++i)
      if (!folded[static_cast<size_t>(i)] && s.tracks[static_cast<size_t>(i)].takeLanes) takeLaneN[static_cast<size_t>(i)] = takes::laneCount(s, i);
  auto takeLaneH = [&](float h) { return std::round(std::clamp(h * 0.62f, 22.0f, 56.0f)); };
  auto layoutRows = [&](float h, std::vector<float>& ty) {
    ty.assign(static_cast<size_t>(nTracks) + 1, 0.0f);
    for (int i = 0; i < nTracks; ++i)
      ty[static_cast<size_t>(i) + 1] = ty[static_cast<size_t>(i)] +
          (folded[static_cast<size_t>(i)] ? 0.0f : std::max(foldShut && i == foldHead ? 22.0f : 0.0f, std::round(h * rowScale(i)))) +
          (lg && !folded[static_cast<size_t>(i)] && autoLanes.count(i) && !(foldShut && i == foldHead) ? kAutoLaneH : 0.0f) +
          static_cast<float>(takeLaneN[static_cast<size_t>(i)]) * takeLaneH(h);
    // R08-10: the open folder keeps its own header row above its members (it belongs to no track:
    // the room sits at the foot of the row before, which hOf leaves out).
    if (foldHdrH > 0)
      for (int k = foldHead; k <= nTracks; ++k) ty[static_cast<size_t>(k)] += foldHdrH;
  };
  // K-112b (b4): the automation room a track's row gains while it shows its automation (0: none).
  // R04-08: the curve is drawn inside the row, over the track's own regions (299), so hOf below is
  // the whole row (regions included); the header keeps its controls in the upper hOf - laneOf.
  auto laneOf = [&](int tr) {
    return lg && tr >= 0 && tr < nTracks && !folded[static_cast<size_t>(tr)] && autoLanes.count(tr) && !(foldShut && tr == foldHead) ? kAutoLaneH : 0.0f;
  };
  // Scroller drags (thumbs and, K-24, their ends: zoom), kept across frames.
  // 1 / 2: the vertical / horizontal thumb; 3 / 4: the vertical thumb's top /
  // bottom end; 5 / 6: the horizontal thumb's left / right end.
  static int barDrag = 0;
  static float barGrab = 0;     // pointer offset into the thumb (or from the grabbed end)
  static float barFixed = 0;    // ends: the screen position of the end that stays put
  static float barLen0 = 1;     // ends: the thumb's length at the grab
  static double barZ0 = 1;      // ends: the zoom at the grab (track height / px per tick)
  static smoothzoom::RowAnchor barRow;          // vertical ends: the content point held at the opposite edge
  static double barTick = 0;                    // horizontal ends: the tick held at the opposite edge
  static float barEdgePx = 0;                   //   and its offset from the grid's left
  const float gridH0 = gMax.y - gMin.y;
  smoothzoom::Params zp;
  zp.smooth = scrollui::smoothZoom();
  // ---- K-24: vertical zoom (the track height), anchored ----
  // Alt+wheel keeps the track under the pointer and the point inside it still
  // (gliding like J-188's horizontal zoom; a trackpad's fractional Alt+scroll
  // 1:1); dragging an end of the vertical scroller holds the opposite edge;
  // a height written from outside (View menu) glides with the top row held.
  // The rows are laid out at the drawn height and the scroll follows the
  // anchor exactly (mixed heights, folded groups, automation lanes alike).
  // K-112b: until the person sets a height, each layout starts at its own default (the Tracks
  // layout's rows are lower, as its references draw them); applied when the layout changes.
  const bool heightSet = a.settings.getBool("ui.playlistTrackHeightSet");
  const float layoutH = playlistDefaultTrackHeight(lg);
  {
    static int appliedFor = -1;
    if (!heightSet && appliedFor != static_cast<int>(lg)) st.trackHeight = layoutH;
    appliedFor = static_cast<int>(lg);
  }
  {
    smoothzoom::RowGlide& vg = st.rowGlide;
    std::vector<float> ty;
    auto anchorAt = [&](float h, float px) {
      layoutRows(h, ty);
      return smoothzoom::rowAnchorAt(ty.data(), nTracks, h, static_cast<double>(px + st.scrollY), px);
    };
    const float held = st.trackHeight;
    const float drawn = vg.outside(held) ? vg.output() : held;
    if (vg.outside(held)) {
      vg.zoomTo(drawn, std::clamp(held, kTrackHMin, kTrackHMax), anchorAt(drawn, 0.0f), true, zp);
    } else if (vg.active() && st.scrollY != vg.scrollOut) {
      vg.setAnchor(anchorAt(drawn, static_cast<float>(vg.anchor().px)));  // scrolled from outside mid-glide: the glide moves along
    }
    // A plain scroll arriving mid-glide: the zoom is there at once and the scroll goes on from it.
    if (hovered && ((io.MouseWheel != 0 && !io.KeyCtrl && !io.KeyAlt) || io.MouseWheelH != 0 || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
      vg.finish();
    if (hovered && io.MouseWheel != 0 && io.KeyAlt && !io.KeyCtrl) {
      const double to = std::clamp(vg.target(drawn) * std::pow(kRowZoomStep, static_cast<double>(io.MouseWheel)), static_cast<double>(kTrackHMin),
                                   static_cast<double>(kTrackHMax));
      const float py = std::clamp(io.MousePos.y - gMin.y, 0.0f, gridH0);
      vg.zoomTo(drawn, to, anchorAt(drawn, py), smoothscroll::isNotch(io.MouseWheel), zp);
    }
    if ((barDrag == 3 || barDrag == 4) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float end = io.MousePos.y - barGrab;
      const float len = std::clamp(barDrag == 4 ? end - barFixed : barFixed - end, kThumbMinV, std::max(kThumbMinV, gridH0));
      const double to = std::clamp(barZ0 * barLen0 / len, static_cast<double>(kTrackHMin), static_cast<double>(kTrackHMax));
      if (std::fabs(to - vg.target(drawn)) > 1e-4) vg.zoomTo(drawn, to, barRow, false, zp);
    }
    st.trackHeight = static_cast<float>(vg.step(drawn, io.DeltaTime, zp));
    // Remembered with the view (undo-free, like scrolling) once it settles.
    if (!vg.active() && barDrag != 3 && barDrag != 4) {
      if (!heightSet) {  // a height other than the layout's own is the person's from now on
        if (std::fabs(st.trackHeight - layoutH) > 1e-3) a.settings.set("ui.playlistTrackHeightSet", true);
      } else if (std::fabs(a.settings.getNumber("ui.playlistTrackHeight") - static_cast<double>(st.trackHeight)) > 1e-3) {
        a.settings.set("ui.playlistTrackHeight", static_cast<double>(st.trackHeight));
      }
    }
  }
  const float trackH = st.trackHeight;
  std::vector<float> trackY;
  layoutRows(trackH, trackY);
  const float totalH = trackY.back();
  // J-67: the range is enforced here for outside writes; the view's own drawn
  // position (a glide, an elastic overscroll) is left to scrollui::step below.
  // K-112b: the Tracks layout keeps two rows of empty space below the last track (a double-click
  // there opens New tracks), as its main window does.
  const float spaceBelow = logicplug::active(a) ? 2.0f * trackH : 0.0f;
  const float maxScrollY = std::max(0.0f, totalH + spaceBelow - (gMax.y - gMin.y)), maxScrollX = 1e9f;
  if (st.rowGlide.anchored()) {  // K-24: the anchored point back under the pointer (the range's edges hold)
    const smoothzoom::RowAnchor& an = st.rowGlide.anchor();
    st.scrollY = std::clamp(static_cast<float>(smoothzoom::rowAnchorPos(trackY.data(), nTracks, trackH, an) - an.px), 0.0f, maxScrollY);
    st.rowGlide.scrollOut = st.scrollY;
  }
  scrollui::clampOuter(st.axY, st.scrollY, 0.0f, maxScrollY);
  // K-112c (C44-20): in Clips the arrangement starts where the live area (the clip grid's storage) ends.
  const Tick viewO = livelook::arrangementOrigin(a);
  scrollui::clampOuter(st.axX, st.scrollX, static_cast<float>(viewO) * st.pxPerTick, maxScrollX);
  auto xOf = [&](Tick t) { return gMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto yOf = [&](int tr) { return gMin.y + trackY[static_cast<size_t>(std::clamp(tr, 0, nTracks))] - st.scrollY; };
  // K-123: the take lanes' height under a track (after its automation lane), and one lane's.
  const float laneTH = takeLaneH(trackH);
  auto takesOf = [&](int tr) { return tr >= 0 && tr < nTracks ? static_cast<float>(takeLaneN[static_cast<size_t>(tr)]) * laneTH : 0.0f; };
  auto hOf = [&](int tr) {
    tr = std::clamp(tr, 0, std::max(0, nTracks - 1));
    // K-123: less the take lanes; R08-10: less the open folder's header row
    return nTracks ? trackY[static_cast<size_t>(tr) + 1] - trackY[static_cast<size_t>(tr)] - takesOf(tr) - (foldHdrH > 0 && tr + 1 == foldHead ? foldHdrH : 0.0f) : trackH;
  };
  // R04-08: the automation area of a row that shows its automation: under the regions' name bar
  // (it keeps the region gestures, as the reference's) down to the row's foot.
  auto curveTop = [&](int tr) { return yOf(tr) + 1 + std::min(15.0f, (hOf(tr) - 2) * 0.45f); };
  auto curveBot = [&](int tr) { return yOf(tr) + hOf(tr) - 2; };
  auto inCurveArea = [&](int tr, float py) {
    return lg && tr >= 0 && tr < nTracks && laneOf(tr) > 0 && py >= curveTop(tr) && py < curveBot(tr) + 2;
  };
  devGeom.x0 = gMin.x - st.scrollX, devGeom.pxPerTick = st.pxPerTick, devGeom.valid = true;
  devGeom.trackTop.clear(), devGeom.trackH.clear(), devGeom.clips.clear(), devGeom.curves.clear();
  devGeom.dropTrack = -1;
  devGeom.renaming = -1;
  devGeom.dragBefore = -1;
  devGeom.headX0 = headX0, devGeom.headX1 = headX1;
  for (int i = 0; i < nTracks; ++i) devGeom.trackTop.push_back(yOf(i)), devGeom.trackH.push_back(hOf(i));
  auto tickAt = [&](float x) { return timeline::tickAtPx(x - gMin.x + st.scrollX, st.pxPerTick); };
  auto trackAt = [&](float y) {
    float yy = y - gMin.y + st.scrollY;
    auto it = std::upper_bound(trackY.begin(), trackY.end(), yy);
    int tr = std::clamp(static_cast<int>(it - trackY.begin()) - 1, 0, std::max(0, nTracks - 1));
    // R12-05: in Smooth, below the last row, the last row on screen (never a hidden one: spare rows
    // take no room)
    while (lg && tr > 0 && trackY[static_cast<size_t>(tr) + 1] <= trackY[static_cast<size_t>(tr)]) {
      --tr;
    }
    // N52-03: in Clips a point past the last row (the returns below) or on a folded group's hidden
    // track is its folded group's row, never a track out of sight.
    if (liveLook) {
      while (tr > 0 && tr < nTracks && folded[static_cast<size_t>(tr)]) --tr;
    }
    return tr;
  };
  // N52-03: the point is below every track's row (Clips' return lanes, the free area).
  auto pastRows = [&](float y) { return y - gMin.y + st.scrollY >= trackY.back(); };
  // Checkpoint 52: in Clips a group track's lane holds no clips (its tracks' clips show there folded).
  auto groupLane = [&](int tr) { return liveLook && trackmodes::isGroupTrack(a.doc.song(), tr); };
  const char* kGroupLaneHint = "A group track holds no clips: put them on one of its tracks";
  // ---- zoom: Ctrl+wheel at the pointer, PgUp/PgDn, Home = fit, toolbar buttons ----
  // J-188: the code below works on the logical zoom and scroll (where the
  // zooms end up, composing exactly as they always did); zoomEnd turns them
  // into this frame's drawn view, gliding with the point under the pointer
  // (or the anchor) held in place.
  scrollui::zoomBegin(st.zoomGlide, st.pxPerTick, st.scrollX);
  const float gridW = gMax.x - gMin.x;
  const float playX = static_cast<float>(a.engine.position()) * st.pxPerTick - st.scrollX;
  const float keyAnchor = hovered && io.MousePos.x >= gMin.x ? io.MousePos.x - gMin.x
                          : (playX >= 0 && playX <= gridW ? playX : gridW * 0.5f);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (keys::pressed("playlist.zoomIn")) st.zoomRequest *= 1.5f;
    if (keys::pressed("playlist.zoomOut")) st.zoomRequest /= 1.5f;
    if (keys::pressed("playlist.zoomFit")) st.fitRequest = true;
  }
  if (st.fitRequest) {
    Tick len = std::max<Tick>(clipCache(a).songLen - viewO, 4 * s.barTicks());
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, len + s.barTicks(), gridW));
    st.scrollX = static_cast<float>(viewO) * st.pxPerTick;
    st.fitRequest = false;
  }
  if (st.quickZoom > 0) {  // J-28: quick zooms (FL Zoom menu): 4 / 16 / 64 bars across the view
    const Tick bars = st.quickZoom == 1 ? 4 : st.quickZoom == 2 ? 16 : 64;
    const float playTick = static_cast<float>(a.engine.position());
    const Tick from = std::max<Tick>(0, static_cast<Tick>(playTick));  // K-193: bars of the meter map from the song position
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, std::max<Tick>(1, MeterMap(s).addBars(from, bars) - from), gridW));
    st.scrollX = std::max(0.0f, playTick * st.pxPerTick - gridW * 0.25f);
    st.quickZoom = 0;
  }
  if (st.zoomSelRequest) {  // zoom on the time selection, else on the selected clips
    std::pair<Tick, Tick> r{st.selStart, st.selEnd};
    if (r.second <= r.first) r = ops::clipsTimeRange(s, st.selected);
    if (r.second > r.first) {
      st.pxPerTick = static_cast<float>(timeline::fitZoom(s, r.second - r.first, gridW));
      st.scrollX = static_cast<float>(r.first) * st.pxPerTick;
    }
    st.zoomSelRequest = false;
  }
  if (st.zoomRequest != 1.0f) {
    timeline::zoomAt(s, st.pxPerTick, st.scrollX, st.zoomRequest, keyAnchor);
    st.zoomRequest = 1.0f;
  }
  // K-24: over the track headers the zoom holds the grid's left edge (not a point left of it).
  const bool wheelZoom = hovered && io.MouseWheel != 0 && io.KeyCtrl;
  if (wheelZoom)
    timeline::zoomAt(s, st.pxPerTick, st.scrollX, std::pow(1.2, static_cast<double>(io.MouseWheel)), std::clamp(io.MousePos.x - gMin.x, 0.0f, gridW));
  // K-24: an end of the horizontal scroller dragged: the thumb's length sets the zoom, the opposite edge's tick stays put (1:1).
  bool barZoom = false;
  if ((barDrag == 5 || barDrag == 6) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float end = io.MousePos.x - barGrab;
    const float len = std::clamp(barDrag == 6 ? end - barFixed : barFixed - end, kThumbMinH, std::max(kThumbMinH, gridW));
    const double z = timeline::clampZoom(s, barZ0 * barLen0 / len);
    st.pxPerTick = static_cast<float>(z);
    st.scrollX = static_cast<float>(std::max(0.0, barTick * z - barEdgePx));
    barZoom = true;
  }
  st.pxPerTick = static_cast<float>(timeline::clampZoom(s, st.pxPerTick));
  // Auto scrolling: page along with the song position while the song plays
  // (not while the user drags or scrolls the view).
  if (a.engine.playing() && a.engine.mode() == PlayMode::Song && a.settings.getBool("ui.autoScroll") && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    st.scrollX = ops::followScroll(st.scrollX, static_cast<float>(a.engine.position()) * st.pxPerTick, gridW);

  // The engine loops playback inside the time selection (Clips: inside its loop brace): livelook::transportFrame.
  // A trackpad's Ctrl+scroll follows the fingers 1:1; wheel notches and every other zoom glide.
  scrollui::zoomEnd(st.zoomGlide, st.pxPerTick, st.scrollX, (wheelZoom && !smoothscroll::isNotch(io.MouseWheel)) || barZoom);
  if (viewO > 0) st.scrollX = std::max(st.scrollX, static_cast<float>(viewO) * st.pxPerTick);  // C44-20: never left of the arrangement's start
  // ---- snapping: the visible grid (Line/Cell) or a fixed size; Alt inverts ----
  const Tick visGrid = timeline::visibleGrid(s, st.pxPerTick);
  // R01-03: in the Tracks layout Alt (Option) copies a dragged region and the grid still holds.
  const Tick snapG = timeline::snapGrid(s, st.snap, st.pxPerTick, io.KeyAlt && !lg);
  st.effGrid = timeline::snapGrid(s, st.snap, st.pxPerTick, false);
  const MeterMap mm(s);  // K-179: song time follows the meter changes
  auto snapPos = [&](Tick t) { return timeline::snapTick(mm, t, snapG, st.snap); };

  // J-67: wheel / trackpad input goes through the smooth scroller: notches
  // glide a whole track (or 80 px) and land on it, trackpad deltas stay 1:1,
  // the edges stretch a little and spring back instead of jumping.
  float wheelX = 0, wheelY = 0;
  bool notchX = false, notchY = false;
  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) {
      // zoomed above (J-188)
    } else if (io.KeyAlt) {
      // the track height, zoomed above (K-24)
    } else if (io.KeyShift) {
      wheelX -= io.MouseWheel * 80;
      notchX = smoothscroll::isNotch(io.MouseWheel);
    } else {
      wheelY -= io.MouseWheel * trackH;
      notchY = smoothscroll::isNotch(io.MouseWheel);
    }
  }
  if (hovered && io.MouseWheelH != 0) {
    wheelX -= io.MouseWheelH * 80;
    notchX = smoothscroll::isNotch(io.MouseWheelH);
  }
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
    wheelX -= io.MouseDelta.x;
    wheelY -= io.MouseDelta.y;
    notchX = notchY = false;
  }
  scrollui::step(st.axX, st.scrollX, wheelX, notchX, 0.0f, maxScrollX);
  scrollui::step(st.axY, st.scrollY, wheelY, notchY, 0.0f, maxScrollY);
  devGeom.x0 = gMin.x - st.scrollX, devGeom.pxPerTick = st.pxPerTick;  // J-188: as drawn this frame (zoom and scroll settled)
  if (canchor::collecting()) {  // J-111d: collaborators' pointers in song time and tracks
    const float pxT = std::max(1e-6f, st.pxPerTick);
    canchor::publish(canchor::PlaylistRuler, 0, 0, ImVec2(gMin.x - st.scrollX, origin.y), ImVec2(pxT, rulerH), ImVec2(gMin.x, origin.y),
                     ImVec2(gMax.x, gMin.y));
    for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
      const float y = yOf(tr), h = hOf(tr);
      if (h < 1) continue;  // folded into its group
      const uint32_t k = static_cast<uint32_t>(tr);
      canchor::publish(canchor::PlaylistTrack, k, 0, ImVec2(gMin.x - st.scrollX, y), ImVec2(pxT, h), ImVec2(gMin.x, std::max(y, gMin.y)),
                       ImVec2(gMax.x, std::min(y + h, gMax.y)));
      const float hx0 = liveLook ? headX0 : origin.x, hx1 = liveLook ? fullX1 : gMin.x;
      canchor::publish(canchor::PlaylistHeader, k, 0, ImVec2(hx0, y), ImVec2(headW, h), ImVec2(hx0, std::max(y, gMin.y)),
                       ImVec2(hx1, std::min(y + h, gMax.y)));
    }
  }

  // Background & grid: alternate bar shading, then track separators and bar/beat lines.
  // K-276: every time edge (clip, grid line, bar shading) is snapped once to a device pixel
  // (PixelGrid.h), so touching clips show one gap at every zoom and interface scale and meet the lines.
  const float dpp = pixelgrid::frameScale();
  auto snapX = [&](Tick t) { return pixelgrid::edge(xOf(t), dpp); };
  auto vline = [&](float x, float y0, float y1, ImU32 c) {  // a grid line: one crisp device column (or more) at x's edge
    const pixelgrid::Span l = pixelgrid::line(x, dpp);
    dl->AddRectFilled(ImVec2(l.x0, y0), ImVec2(l.x1, y1), c);
  };
  dl->PushClipRect(gMin, gMax, true);
  // K-112b (checkpoint 20): the Tracks layout's tracks area a step darker, its time lines lighter than
  // the ground (216, 125, 164 and 195 all draw a near-black area with light lines, the regions the
  // brightest thing on it). Dark faces only; the light theme keeps its well.
  const bool lgDark = lg && P.dark;
  const ImU32 well = lgDark ? P.surface950 : P.well;
  const ImU32 lineBar = lgDark ? theme::mix(well, P.text, 0.11f) : P.gridBar, lineBeat = lgDark ? theme::mix(well, P.text, 0.045f) : P.gridBeat;
  dl->AddRectFilled(gMin, gMax, well);
  const Tick barT = std::max<Tick>(1, s.barTicks()), beatT = std::max<Tick>(1, s.beatTicks());
  // Alternate bar shading (every other bar a step lighter), like the reference grid. K-179: real
  // bars of the meter map (each section's own bar length).
  if (mm.constant()) {
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), barT); xOf(t) < gMax.x; t += barT)
      if (!lg && (t / barT) % 2 == 1) dl->AddRectFilled(ImVec2(snapX(t), gMin.y), ImVec2(snapX(t + barT), gMax.y), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
  } else {
    for (int64_t b = mm.barIndex(std::max<Tick>(0, tickAt(gMin.x))); xOf(mm.barStart(b)) < gMax.x; ++b)
      if (!lg && b % 2 == 1)
        dl->AddRectFilled(ImVec2(snapX(mm.barStart(b)), gMin.y), ImVec2(snapX(mm.barStart(b + 1)), gMax.y), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
  }
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {  // track separators, on the rows' snapped edges
    const pixelgrid::Span l = pixelgrid::line(yOf(tr) + hOf(tr), dpp);
    dl->AddRectFilled(ImVec2(gMin.x, l.x0), ImVec2(gMax.x, l.x1), lgDark ? theme::mix(well, P.text, 0.05f) : P.gridBar);  // D5: fine in Smooth
  }
  {
    // Lines at the visible grid: bars strongest, then beats, then finer subdivisions.
    const ImU32 fine = theme::withAlpha(lineBeat, 0.45f);
    if (mm.constant()) {
      for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), visGrid); xOf(t) < gMax.x; t += visGrid) {
        ImU32 c = t % barT == 0 ? lineBar : t % beatT == 0 ? lineBeat : fine;
        vline(xOf(t), gMin.y, gMax.y, c);
      }
    } else {  // K-179: lines of the meter map; compound groups (the 4th eighth in 6/8) between bar and beat
      const ImU32 group = theme::mix(lineBeat, lineBar, 0.45f);
      timeline::forEachLine(mm, std::max<Tick>(0, tickAt(gMin.x)), tickAt(gMax.x) + 1, visGrid, [&](Tick t, int k) {
        const ImU32 c = k == timeline::kLineBar ? lineBar : k == timeline::kLineGroup ? group : k == timeline::kLineBeat ? lineBeat : fine;
        vline(xOf(t), gMin.y, gMax.y, c);
      });
    }
  }
  // Markers: faint full-height lines (labels in the ruler).
  for (const auto& mk : s.markers) {
    float x = std::round(xOf(mk.pos));
    if (x >= gMin.x && x <= gMax.x) dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), theme::withAlpha(P.textDim, 0.35f));
  }
  // Live area: clips before its end are launched by hand (live mode).
  const bool liveMode = a.engine.mode() == PlayMode::Live;
  if (viewO == 0 && s.live.areaEnd > 0 && xOf(0) < gMax.x) {  // (in Clips the live area is the clip grid, not shown here)
    float lx1 = std::min(gMax.x, xOf(s.live.areaEnd));
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(0)), gMin.y), ImVec2(lx1, gMax.y), theme::withAlpha(P.paper, liveMode ? 0.05f : 0.03f));
    if (lx1 < gMax.x) dl->AddLine(ImVec2(std::round(lx1), gMin.y), ImVec2(std::round(lx1), gMax.y), P.lineStrong, 2.0f);
  }
  // Time selection: tinted band across the tracks. J-67: its edges glide
  // between grid lines while dragged (smooth motion, like clip moves).
  const bool selGlide = cg.kind == ClipGesture::TimeSel && snapG > 1;
  const float selX0 = gMin.x + static_cast<float>(scrollui::glide(0x504C53454C41ull, static_cast<double>(st.selStart), selGlide, 0.5)) * st.pxPerTick - st.scrollX;
  const float selX1 = gMin.x + static_cast<float>(scrollui::glide(0x504C53454C42ull, static_cast<double>(st.selEnd), selGlide, 0.5)) * st.pxPerTick - st.scrollX;
  if (st.selEnd > st.selStart && liveLook && !st.selTracks.empty()) {
    // N48-05: a time selection drawn in the lanes covers its own tracks only, as the reference's.
    for (int t : st.selTracks) {
      if (t < 0 || t >= nTracks) continue;
      const float y0 = std::max(gMin.y, yOf(t)), y1 = std::min(gMax.y, yOf(t + 1));
      if (y1 > y0) dl->AddRectFilled(ImVec2(std::max(gMin.x, selX0), y0), ImVec2(std::min(gMax.x, selX1), y1), theme::withAlpha(P.paper, 0.16f));
    }
  } else if (st.selEnd > st.selStart)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, selX0), gMin.y), ImVec2(std::min(gMax.x, selX1), gMax.y), theme::withAlpha(P.accent, 0.07f));
  // Song end shading.
  const ClipCache& ccache = clipCache(a);
  const Tick songLen = ccache.songLen;
  if (songLen > 0 && xOf(songLen) < gMax.x)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(songLen)), gMin.y), gMax, theme::withAlpha(P.surface950, 0.35f));
  // Clips.
  ObjectId hoverClip = kNoId, hoverMenuGlyph = kNoId;
  ParamId hoverCurve{kNoId, 0};  // J-173: the pattern automation curve under the pointer on hoverClip
  clipheader::Zone hoverZone = clipheader::Zone::None;  // J-142: where on hoverClip the pointer is
  bool hoverEdge = false, hoverLeftEdge = false, hoverBody = false;
  // Audio clip fade handles (J-8; J-38: tension handles, crossfade partners):
  // 1 = fade-in length, 2 = fade-out length, 3 = fade-in tension, 4 = fade-out tension.
  static ObjectId fadeDragClip = kNoId;
  static int fadeDragKind = 0;
  static Tick fadeToolT0 = -1;  // R07-04: the Fade tool's press (the fade is the span dragged over), -1: a handle drag
  static float fadeDragT0 = 0, fadeDragY0 = 0;
  static ObjectId fadeDragMirror = kNoId;  // tension: the crossfade partner whose opposite edge mirrors it
  ObjectId fadeHoverClip = kNoId;
  int fadeHoverKind = 0;
  ObjectId fadeHoverMirror = kNoId;
  // J-79: the volume line of an audio clip (shown with the fade handles): a
  // bar at the bottom middle, dragged up / down for the clip's gain in dB.
  struct GainLine {
    ObjectId clip = kNoId;
    float x0 = 0, x1 = 0;    // the clip (the faint line runs across it)
    float hx0 = 0, hx1 = 0;  // the handle (bottom middle of the visible part)
    float y = 0;
  };
  GainLine gainLine;
  static ObjectId gainDragClip = kNoId;
  static float gainDragY0 = 0;
  static clipgain::Drag gainDrag;
  // The waveform of a clip whose gain just changed glides to its new height (J-67).
  static ObjectId gainGlideClip = kNoId;
  static double gainGlideDb = 0;
  if (gainGlideClip != kNoId) {
    const Clip* gc = s.clip(gainGlideClip);
    const double target = gc ? std::max(clipgain::kMinDb - 0.1, clipgain::toDb(gc->gain)) : 0.0;
    gainGlideDb = scrollui::smoothSnap() ? smooth::approach(gainGlideDb, target, io.DeltaTime, smooth::kBarTau) : target;
    if (!gc || (std::fabs(gainGlideDb - target) < 0.01 && gainDragClip != gainGlideClip)) gainGlideClip = kNoId;
  }
  int hoverPoint = -1, hoverHandle = -1;
  // Automation clip being edited: drawn from the live document (edits apply as they happen).
  // J-37: clips under a snapped drag glide between grid cells (drawing only);
  // free dragging (snap off, Alt, free grid at max zoom) follows the pointer.
  const double animNow = ImGui::GetTime();
  const bool animSnap = scrollui::smoothSnap() && snapG > 1;
  const double animEpsTick = 0.25 / std::max(1e-6, static_cast<double>(st.pxPerTick));  // a quarter pixel
  auto xOfD = [&](double t) { return gMin.x + static_cast<float>(t * static_cast<double>(st.pxPerTick)) - st.scrollX; };
  static std::vector<DrawnClip> drawn;
  drawn.clear();
  // J-38: the empty grid redrawn under a clip where it lies over another clip
  // (the clip beneath stops showing through; the grid lines stay).
  auto gridPatch = [&](float x0, float x1, float y0, float y1) {
    x0 = std::max(x0, gMin.x), x1 = std::min(x1, gMax.x), y0 = std::max(y0, gMin.y), y1 = std::min(y1, gMax.y);
    if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), well);
    const ImU32 fine = theme::withAlpha(lineBeat, 0.45f);
    if (!mm.constant()) {  // K-193: the meter map's bars and lines, as the grid behind
      for (int64_t b = mm.barIndex(std::max<Tick>(0, tickAt(x0))); xOf(mm.barStart(b)) < x1; ++b)
        if (!lg && b % 2 == 1)
          dl->AddRectFilled(ImVec2(std::max(x0, snapX(mm.barStart(b))), y0), ImVec2(std::min(x1, snapX(mm.barStart(b + 1))), y1),
                            theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
      const ImU32 group = theme::mix(lineBeat, lineBar, 0.45f);
      timeline::forEachLine(mm, std::max<Tick>(0, tickAt(x0)), tickAt(x1) + 1, visGrid, [&](Tick t, int k) {
        const ImU32 c = k == timeline::kLineBar ? lineBar : k == timeline::kLineGroup ? group : k == timeline::kLineBeat ? lineBeat : fine;
        if (snapX(t) >= x0) vline(xOf(t), y0, y1, c);
      });
      return;
    }
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(x0)), barT); xOf(t) < x1; t += barT)
      if (!lg && (t / barT) % 2 == 1)
        dl->AddRectFilled(ImVec2(std::max(x0, snapX(t)), y0), ImVec2(std::min(x1, snapX(t + barT)), y1), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(x0)), visGrid); xOf(t) < x1; t += visGrid)
      if (snapX(t) >= x0) vline(xOf(t), y0, y1, t % barT == 0 ? lineBar : t % beatT == 0 ? lineBeat : fine);
  };
  // J-38: a fade curve over [xa, xb]: full level at yt, silence at yb. Fade-ins
  // rise left to right, fade-outs fall. `pts` receives kFadePts points.
  constexpr int kFadePts = 25;
  auto fadePath = [&](float xa, float xb, float yt, float yb, const EdgeFade& e, bool in, ImVec2* pts) {
    for (int k = 0; k < kFadePts; ++k) {
      const float f = static_cast<float>(k) / static_cast<float>(kFadePts - 1);
      const double g = fadeGain(in ? f : 1.0 - f, e.shape, e.tension);
      pts[k] = ImVec2(xa + (xb - xa) * f, yb - (yb - yt) * static_cast<float>(g));
    }
  };
  // J-118: the faint fade curves of a blended crossfade (in the clip's content colour).
  auto xfCurve = [&](ImU32 ink) { return theme::withAlpha(P.dark ? theme::mix(ink, P.paper, 0.35f) : theme::mix(ink, P.ink, 0.25f), 0.55f); };
  static const ClipFades kNoFades{};
  // Shade above the curve (the part the fade removes), then the curve itself.
  auto drawFade = [&](const ImVec2* pts, float yt, ImU32 shade, ImU32 line, float thick) {
    for (int k = 0; k + 1 < kFadePts; ++k)
      if (pts[k].y > yt + 0.25f || pts[k + 1].y > yt + 0.25f)
        dl->AddQuadFilled(ImVec2(pts[k].x, yt), ImVec2(pts[k + 1].x, yt), pts[k + 1], pts[k], shade);
    dl->AddPolyline(pts, kFadePts, line, ImDrawFlags_None, thick);
  };
  // Fade handles of the hovered clip (and of the crossfade partner under it).
  struct FadeHandle {
    ObjectId clip;
    int kind;
    ImVec2 pos;
    ObjectId mirror;
  };
  std::vector<FadeHandle> fadeHandles;
  // K-26: the parts of clips beneath that show through the clip being drawn
  // (drawn list index, visible x span, depth: 0 the layer right under it,
  // 1 a layer further down), and the spans already given to a layer.
  struct Ghost {
    size_t k;
    float x0, x1;
    int depth;
  };
  static std::vector<Ghost> ghosts;
  static std::vector<std::pair<float, float>> ghostCover;
  // K-26: a clip beneath, seen through the body of the clip lying on it over
  // [x0, x1]: its content in its own ink (notes and steps, waveform,
  // automation curve), dimmed by depth. No wash of its colour: the grid
  // lines seen through the top clip keep one brightness, overlap or not.
  // Drawn on the top clip's body, under the top clip's own content and title.
  auto drawGhost = [&](const DrawnClip& p, float x0, float x1, int depth, float bodyY0, float bodyY1, float dim) {
    const float y0 = std::max({bodyY0, p.body, gMin.y}), y1 = std::min({bodyY1, p.y1, gMax.y});
    if (x1 - x0 < 0.5f || y1 - y0 < 2.0f) return;
    const float k = (depth == 0 ? 1.0f : 0.5f) * dim;
    const float inkA = (P.dark ? 0.50f : 0.46f) * k;
    const ImU32 ghostInk = theme::withAlpha(p.ink, inkA);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    const ImVec2 pb0(p.x0 + 3, p.body + 2), pb1(p.x1 - 3, p.y1 - 3);
    const Clip& c = p.clip;
    if (c.kind == ClipKind::Pattern) {
      if (const Pattern* pat = s.pattern(c.pattern))
        if (st.patternPreview != 2 || pat->automation.empty())
          drawClipPreview(dl, s, *pat, pb0, pb1, p.len, c.offset, p.loop, p.loopFrom, ghostInk, x0, x1, true);
    } else if (c.kind == ClipKind::Audio) {
      if (pb1.y - pb0.y > 4)
        if (const Channel* pch = s.channel(c.source)) drawClipWave(dl, s, *pch, c, tempoMap, pb0, pb1, p.clipX0, st.pxPerTick, x0, x1, ghostInk);
    } else if (c.kind == ClipKind::Automation && pb1.y - pb0.y > 6) {
      if (const AutomationClip* ac = s.automation(c.source)) {
        const AutoGeom g = withOwnLoop(AutoGeom{pb0, pb1, p.clipX0, st.pxPerTick, c.start, c.offset, p.len}, c);
        AutomationClip scratch;
        drawAutomation(dl, shownCurve(*ac, c, scratch), g, x0, x1, ghostInk, theme::withAlpha(p.ink, 0.06f * k), false, -1, -1, ghostInk);
      }
    }
    dl->PopClipRect();
    // (Its edges: the J-38 dashed outline drawn after every clip.)
  };
  // K-123: takes recorded as lanes (a take group on one track): the track's row shows the comp only,
  // the muted takes show in the lanes. A group spread over tracks (the earlier layout) draws as before.
  std::map<ObjectId, int> takeGroupTrack;  // group -> its track (-1: spread)
  for (const Clip& c : s.playlist)
    if (c.takeGroup != kNoId) {
      auto [it, fresh] = takeGroupTrack.emplace(c.takeGroup, c.track);
      if (!fresh && it->second != c.track) it->second = -1;
    }
  auto hiddenTake = [&](const Clip& c) {
    if (c.takeGroup == kNoId || !c.muted) return false;
    const auto it = takeGroupTrack.find(c.takeGroup);
    return it != takeGroupTrack.end() && it->second >= 0;
  };
  auto drawClip = [&](size_t ci) {
    if (!ccache.valid[ci]) return;
    // R02-03: in the Tracks layout a track's own automation is its lane's curve, not a region: the
    // clips on a track that drive the parameter its lane is set to are drawn (and edited) there.
    if (lg && s.playlist[ci].kind == ClipKind::Automation) {
      const Clip& kc = s.playlist[ci];
      // R04-01: every curve of a track with a lane is the track's automation (the lane shows the one
      // chosen, the others wait as data), never a region over the track's regions.
      if (logicplug::laneClip(s, kc)) return;
    }
    const Clip& lc = s.playlist[ci];  // logical clip (what the document holds)
    if (hiddenTake(lc)) return;
    if (viewO > 0 && lc.start < viewO) return;  // N48-06: a slot clip (a long one's tail too) is the grid's, never the arrangement's
    Tick len = ccache.len[ci];
    float x0 = xOf(lc.start), x1 = xOf(lc.start + len), y = yOf(lc.track), th = hOf(lc.track);
    Clip animClip;
    const Clip* cp = &lc;
    const bool gliding = !clipAnim.empty() && clipAnim.contains(lc.id);
    if (gliding) {
      const double secPerTick = 60.0 / (std::max(1.0, tempoMap.bpmAt(static_cast<double>(lc.start))) * std::max(1, s.ppq));
      const smooth::Tracker<4>::Vec eps{animEpsTick, animEpsTick, 0.01, lc.kind == ClipKind::Audio && !lc.loop ? animEpsTick * secPerTick : animEpsTick};
      const auto v = clipAnim.follow(lc.id, clipAnimValues(lc, len), animNow, animSnap, eps);
      animClip = lc;
      animClip.start = static_cast<Tick>(std::llround(v[0]));
      len = std::max<Tick>(1, static_cast<Tick>(std::llround(v[1])) - animClip.start);
      if (lc.kind == ClipKind::Audio && !lc.loop) animClip.audioStart = v[3];
      else animClip.offset = static_cast<Tick>(std::llround(v[3]));
      x0 = xOfD(v[0]), x1 = std::max(x0 + 1.0f, xOfD(v[1]));
      const int tr0 = std::clamp(static_cast<int>(std::floor(v[2])), 0, std::max(0, nTracks - 1));
      const int tr1 = std::min(tr0 + 1, std::max(0, nTracks - 1));
      const float f = std::clamp(static_cast<float>(v[2] - tr0), 0.0f, 1.0f);
      y = yOf(tr0) + (yOf(tr1) - yOf(tr0)) * f;
      th = hOf(tr0) + (hOf(tr1) - hOf(tr0)) * f;
      cp = &animClip;
    }
    const Clip& c = *cp;  // drawn clip
    if (x1 < gMin.x || x0 > gMax.x || y + th < gMin.y || y > gMax.y || th < 1) return;
    bool sel = st.selected.count(c.id) > 0;
    const bool livePlaying = liveMode && a.engine.liveActive(c.track) == c.id;
    const bool liveWaiting = liveMode && a.engine.liveQueued(c.track) == c.id;
    bool muted = c.muted || (c.track < nTracks && s.tracks[static_cast<size_t>(c.track)].muted);
    ImU32 base = theme::rgb(ccache.color[ci]);
    if (lg) {  // checkpoint 48 (D1); R09-05: in the track's colour
      const std::vector<Rgb>& tcols = smoothTrackColors(a);
      if (lc.track >= 0 && lc.track < static_cast<int>(tcols.size()) && tcols[static_cast<size_t>(lc.track)] != 0)
        base = theme::rgb(tcols[static_cast<size_t>(lc.track)]);
      base = smoothRegionColor(base);
    }
    // N52-13: in Clips a track playing a grid clip over the arrangement has its arrangement clips
    // greyed (they are not heard until Back to arrangement), as the reference draws them.
    if (liveLook && liveMode && c.start >= s.live.areaEnd && a.engine.liveActive(c.track) != kNoId) {
      base = theme::mix(base, IM_COL32(128, 130, 134, 255), 0.82f);
      muted = true;
    }
    // Clip: translucent colour body (J-37: see-through, so the grid shows), a
    // more opaque title bar with the name, a crisp rim; selected clips get a lit
    // rim. Over the empty grid the composite matches the J-13 palette (body ~58 %
    // towards the well, title bar ~8 %).
    // K-112c: in Live, as the refs draw an arrangement clip, the body is the track's colour itself (a
    // muted clip dimmed), its notes, waveform and curves drawn dark on it.
    const float bodyA = liveLook ? (muted ? 0.70f : 0.94f) : muted ? 0.60f : 0.74f, headA = muted ? 0.80f : 0.90f;
    const ImU32 headSolid = theme::mix(base, P.well, muted ? 0.56f : 0.0f);  // title text contrast
    // Logic look: regions fill the lane in the track colour itself, their content in a light tint
    // of it. K-112b (second pass): the name sits on the same colour, with no deeper strip or line
    // under it, as 216, 125 and 195 all draw regions.
    const ImU32 body = lg ? (muted ? theme::mix(base, P.well, 0.7f) : sel ? theme::mix(base, IM_COL32(255, 255, 255, 255), 0.2f) : base)
                          : theme::withAlpha(theme::mix(base, P.well, liveLook ? (muted ? 0.56f : 0.0f) : muted ? 0.75f : 0.43f), bodyA);
    const ImU32 head = lg ? body : theme::withAlpha(headSolid, headA);
    // D4: Smooth's regions have no dark outline; a selected one is lighter with a light inner line.
    const ImU32 rim = lg ? (sel ? IM_COL32(255, 255, 255, 140) : theme::withAlpha(base, 0.0f)) : sel ? P.hot : theme::mix(base, P.ink, muted ? 0.7f : 0.45f);
    const float clipR = lg ? 4.0f : theme::kRadiusSm, clipInY = 1.0f;
    // K-276: the clip box from the snapped time edges, 1 px in on every side (PixelGrid.h): touching
    // clips are always the same distance apart, and a tiny clip keeps one pixel without covering the next.
    // K-112b: the Logic layout's regions keep their clipInY rows above and below.
    const pixelgrid::Span spanX = pixelgrid::block(x0, x1, dpp), spanY = lg ? pixelgrid::block(y, y + th, dpp, clipInY, clipInY) : pixelgrid::block(y, y + th, dpp);
    ImVec2 a0(spanX.x0, spanY.x0), a1(spanX.x1, spanY.x1);
    const float headH = std::min(15.0f, (a1.y - a0.y) * 0.45f);
    // J-38: fades as they play (automatic crossfades included).
    const ClipFades& fd = c.kind == ClipKind::Audio && ci < ccache.fades.size() ? ccache.fades[ci] : kNoFades;
    const double sec0 = tempoMap.secondsAt(static_cast<double>(c.start));
    const double dur = tempoMap.secondsAt(static_cast<double>(c.start + len)) - sec0;
    auto xAtSec = [&](double sec) { return xOfD(tempoMap.tickAtSeconds(sec0 + sec)); };
    FadeGeom fg;
    fg.in = fd.in, fg.out = fd.out;
    fg.inXa = a0.x, fg.inXb = std::min(xAtSec(fd.in.len), a1.x);
    fg.outXa = std::max(xAtSec(dur - fd.out.len), a0.x), fg.outXb = a1.x;
    // J-118: a crossfade with the clip beneath (its partner, ending inside
    // this clip): the overlap shows both clips blended by their fade levels,
    // one joined cell.
    int xfBelow = -1;
    if (c.kind == ClipKind::Audio && fd.in.crossfade && fd.in.partner != kNoId && !gliding) {
      int budget = 64;
      for (size_t k = drawn.size(); k-- > 0 && budget-- > 0;) {
        const DrawnClip& p = drawn[k];
        if (p.track != lc.track || p.clip.id != fd.in.partner) continue;
        if (p.audio && !p.glide && p.fade.out.crossfade && p.fade.out.partner == c.id && p.x0 < a0.x && p.x1 > a0.x && p.x1 < a1.x &&
            std::fabs(p.y0 - a0.y) < 0.5f && std::fabs(p.y1 - a1.y) < 0.5f)
          xfBelow = static_cast<int>(k);
        break;
      }
    }
    // J-38: stacking. Where this clip lies over clips drawn before it on the
    // track, the grid is redrawn under it (the two bodies never mix into a
    // muddle of colours). K-26: the clip beneath still shows through, dimmed,
    // as a ghost layer drawn on this clip's body (below); no shadow falls on
    // it (a dark band at the edge read as a smear and hid the see-through
    // look): this clip's own rim is the edge.
    ghosts.clear();
    ghostCover.clear();
    {
      int budget = 64;
      for (size_t k = drawn.size(); k-- > 0 && budget-- > 0;) {
        const DrawnClip& p = drawn[k];
        if (p.track != lc.track || p.x1 <= a0.x || p.x0 >= a1.x) continue;
        const float ox0 = std::max(p.x0, a0.x), ox1 = std::min(p.x1, a1.x), oy0 = std::max(p.y0, a0.y), oy1 = std::min(p.y1, a1.y);
        if (oy1 - oy0 < 1.0f) continue;
        if (static_cast<int>(k) == xfBelow) {  // J-118: cleared to the grid (its rim too), redrawn blended below
          gridPatch(ox0, ox1 + 1.0f, a0.y, a1.y);
          continue;
        }
        gridPatch(ox0, ox1, a0.y, a1.y);
        // K-26: the visible part of the overlap, split into what this clip
        // covers directly (the nearest layer) and what a clip already found
        // above it covers too (a deeper, dimmer layer). Clips are visited top
        // down, so `ghostCover` holds the spans of the layers above.
        {
          const float vx0 = std::max(ox0, gMin.x), vx1 = std::min(ox1, gMax.x);
          if (vx1 - vx0 >= 1.0f && ghosts.size() < 24) {
            float x = vx0;
            for (const auto& [cx0, cx1] : ghostCover) {  // sorted, disjoint
              if (cx1 <= x) continue;
              if (cx0 >= vx1) break;
              if (cx0 > x) ghosts.push_back({k, x, cx0, 0});
              ghosts.push_back({k, std::max(x, cx0), std::min(vx1, cx1), 1});
              x = std::min(vx1, cx1);
              if (x >= vx1) break;
            }
            if (x < vx1) ghosts.push_back({k, x, vx1, 0});
            // Merge [vx0, vx1] into the covered spans.
            float m0 = vx0, m1 = vx1;
            size_t w = 0;
            bool placed = false;
            std::array<std::pair<float, float>, 32> merged{};
            for (const auto& sp : ghostCover) {
              if (sp.second < m0) merged[w++] = sp;
              else if (sp.first > m1) {
                if (!placed) merged[w++] = {m0, m1}, placed = true;
                merged[w++] = sp;
              } else
                m0 = std::min(m0, sp.first), m1 = std::max(m1, sp.second);
              if (w + 1 >= merged.size()) break;
            }
            if (!placed) merged[w++] = {m0, m1};
            ghostCover.assign(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(w));
          }
        }
      }
    }
    // Clip content (notes, waveform, curve): lighter than the colour on dark
    // slate, darker than it on the light variant's pastel bodies.
    const ImU32 ink = liveLook        ? liveClipInk(base, muted)
                      : lg && !muted && c.kind == ClipKind::Audio
                          // Coordinator item: the Tracks layout's audio waveforms read clearly, as the reference's
                          // (a dark wave on a light region, a light one on a dark region); the pale tint was faint.
                          // D8 (checkpoint 49): a filled wave in a light tint of the region's colour, as 296 and 063.
                          // (on the yellow family, where the name is dark, a darker shade instead).
                          ? theme::withAlpha(smoothDarkName(base) ? theme::mix(base, IM_COL32(0, 0, 0, 255), 0.4f) : theme::mix(base, IM_COL32(255, 255, 255, 255), 0.55f), 0.92f)
                      : lg && !muted ? IM_COL32(255, 255, 255, 217)  // D3: notes 85 % white
                                     : theme::clipInk(base, muted);  // K-177: follows the theme (white text pushes it)
    // J-118: this clip's share of a crossfade blend with `p` at x (by the levels playing there).
    auto xfWeight = [&](const DrawnClip& p, float x) {
      const float gp = p.fade.xfGain(x), gq = fg.xfGain(x);
      return gp + gq > 1e-4f ? gq / (gp + gq) : 0.5f;
    };
    // The crossfade's overlap cut into strips that small every blend reads smooth.
    auto xfStrips = [&](const DrawnClip& p, const std::function<void(float, float, float, float)>& fn) {
      const float vx0 = std::max(a0.x, gMin.x - 2), vx1 = std::min(p.x1, gMax.x + 2);
      const int n = std::clamp(static_cast<int>(std::ceil((vx1 - vx0) / 3.0f)), 1, 96);
      float wl = xfWeight(p, vx0);
      for (int k = 0; k < n && vx1 > vx0; ++k) {
        const float xa = vx0 + (vx1 - vx0) * static_cast<float>(k) / static_cast<float>(n);
        const float xb = vx0 + (vx1 - vx0) * static_cast<float>(k + 1) / static_cast<float>(n);
        const float wr = xfWeight(p, xb);
        fn(xa, xb, wl, wr);
        wl = wr;
      }
    };
    if (xfBelow < 0) {
      dl->AddRectFilled(ImVec2(a0.x, a0.y + headH), a1, body, clipR, ImDrawFlags_RoundCornersBottom);
      dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + headH), head, clipR, ImDrawFlags_RoundCornersTop);
    } else {
      // J-118: crossfade. Past the overlap the clip is drawn as usual; across
      // it, body and title bar run on from the clip beneath as one cell, their
      // colour moving from its colour to this clip's as the levels cross. Both
      // waveforms show, each as loud as it plays (opacity follows its fade).
      const DrawnClip& p = drawn[static_cast<size_t>(xfBelow)];
      const float ox0 = a0.x, ox1 = p.x1;
      // (Cut at the overlap's end by the clip rectangle, so no anti-aliased
      // edge leaves a seam where the blend below meets it.)
      dl->PushClipRect(ImVec2(ox1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1), true);
      dl->AddRectFilled(ImVec2(ox1 - 4, a0.y + headH), a1, body, clipR, ImDrawFlags_RoundCornersBottomRight);
      dl->AddRectFilled(ImVec2(ox1 - 4, a0.y), ImVec2(a1.x, a0.y + headH), head, clipR, ImDrawFlags_RoundCornersTopRight);
      dl->PopClipRect();
      xfStrips(p, [&](float xa, float xb, float wl, float wr) {
        const ImU32 bl = theme::mix(p.bodyCol, body, wl), br = theme::mix(p.bodyCol, body, wr);
        const ImU32 hl = theme::mix(p.headCol, head, wl), hr = theme::mix(p.headCol, head, wr);
        dl->AddRectFilledMultiColor(ImVec2(xa, a0.y + headH), ImVec2(xb, a1.y), bl, br, br, bl);
        dl->AddRectFilledMultiColor(ImVec2(xa, a0.y), ImVec2(xb, a0.y + headH), hl, hr, hr, hl);
      });
      // The waveform beneath, fading out under this one.
      const ImVec2 pb0(p.x0 + 3, p.body + 2), pb1(p.x1 - 3, p.y1 - 3);
      if (pb1.y - pb0.y > 4)
        if (const Channel* pch = s.channel(p.clip.source)) {
          dl->PushClipRect(ImVec2(std::max(ox0, gMin.x), std::max(p.body, gMin.y)), ImVec2(std::min(ox1, gMax.x), std::min(a1.y, gMax.y)), true);
          drawWaveform(dl, s, *pch, p.clip, tempoMap, pb0, pb1, p.clipX0, st.pxPerTick, gMin.x, gMax.x, p.ink, &p.fade);
          dl->PopClipRect();
        }
    }
    // K-26: the clips beneath, seen through this clip's body (deepest first).
    // (A muted clip on top keeps its own dim content clear of them: a step fainter.)
    const float ghostDim = muted ? 0.7f : 1.0f;
    for (size_t g = ghosts.size(); g-- > 0;)
      if (ghosts[g].depth == 1) drawGhost(drawn[ghosts[g].k], ghosts[g].x0, ghosts[g].x1, 1, a0.y + headH + 1, a1.y - 1, ghostDim);
    for (size_t g = ghosts.size(); g-- > 0;)
      if (ghosts[g].depth == 0) drawGhost(drawn[ghosts[g].k], ghosts[g].x0, ghosts[g].x1, 0, a0.y + headH + 1, a1.y - 1, ghostDim);
    {
      DrawnClip dc;
      dc.track = lc.track, dc.x0 = a0.x, dc.x1 = a1.x, dc.y0 = a0.y, dc.y1 = a1.y, dc.body = a0.y + headH, dc.ci = ci, dc.rim = rim, dc.sel = sel;
      dc.clip = c, dc.clipX0 = x0, dc.bodyCol = body, dc.headCol = head, dc.ink = ink, dc.fade = fg;
      dc.audio = c.kind == ClipKind::Audio, dc.glide = gliding, dc.xfBelow = xfBelow;
      dc.len = len, dc.loop = c.kind == ClipKind::Pattern ? ccache.loop[ci] : 0, dc.loopFrom = c.kind == ClipKind::Pattern ? ccache.loopFrom[ci] : 0;
      drawn.push_back(dc);
    }
    // K-276: the rim stroked wholly inside the clip (a 2 px selected rim centred on the edge reached into the gap).
    const float rimW = sel && !lg ? 2.0f : 1.0f, rimIn = pixelgrid::strokeIn(rimW);
    if (!lg) dl->AddLine(ImVec2(a0.x + 1, a0.y + 0.5f), ImVec2(a1.x - 1, a0.y + 0.5f), theme::withAlpha(P.paper, muted ? 0.08f : 0.28f));
    if (!lg) dl->AddLine(ImVec2(a0.x, a0.y + headH), ImVec2(a1.x, a0.y + headH), theme::withAlpha(P.ink, 0.35f));
    if (xfBelow < 0) {
      dl->AddRect(ImVec2(a0.x + rimIn, a0.y + rimIn), ImVec2(a1.x - rimIn, a1.y - rimIn), rim, clipR, rimW, 0);
    } else {
      // J-118: the rim past the overlap; across it only its top and bottom
      // lines, blended like the cell (no edge inside it). A selected clip
      // still shows where it starts or ends.
      const DrawnClip& p = drawn[static_cast<size_t>(xfBelow)];
      dl->PushClipRect(ImVec2(p.x1, a0.y - 2), ImVec2(a1.x + 2, a1.y + 2), true);
      dl->AddRect(ImVec2(a0.x + rimIn, a0.y + rimIn), ImVec2(a1.x - rimIn, a1.y - rimIn), rim, clipR, rimW, 0);
      dl->PopClipRect();
      xfStrips(p, [&](float xa, float xb, float wl, float wr) {
        const ImU32 rl = theme::mix(p.rim, rim, wl), rr = theme::mix(p.rim, rim, wr);
        dl->AddRectFilledMultiColor(ImVec2(xa, a0.y), ImVec2(xb, a0.y + 1), rl, rr, rr, rl);
        dl->AddRectFilledMultiColor(ImVec2(xa, a1.y - 1), ImVec2(xb, a1.y), rl, rr, rr, rl);
      });
      auto extent = [&](float edge) {
        dl->AddLine(ImVec2(a0.x, a0.y + 1), ImVec2(p.x1, a0.y + 1), P.hot, 2.0f);
        dl->AddLine(ImVec2(a0.x, a1.y - 1), ImVec2(p.x1, a1.y - 1), P.hot, 2.0f);
        dl->AddLine(ImVec2(edge, a0.y), ImVec2(edge, a1.y), theme::withAlpha(P.hot, 0.8f), 1.5f);
      };
      if (sel) extent(a0.x + 0.75f);
      if (p.sel) extent(p.x1 - 0.75f);
    }
    if (ImU32 fc = 0; const float fl = collabui::flash(a, "playlist/#" + std::to_string(c.id), &fc))  // J-111: a collaborator's change
      dl->AddRect(ImVec2(a0.x + 1.25f, a0.y + 1.25f), ImVec2(a1.x - 1.25f, a1.y - 1.25f), theme::withAlpha(fc, fl), clipR, 2.5f, 0);
    // J-137: the outlines lie inside the clip (a clip on bar 1 touches the grid's clip edge, which cut them).
    if (livePlaying) dl->AddRect(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1), P.accent, clipR, 2.0f, 0);
    if (liveWaiting)
      dl->AddRect(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1),
                  theme::withAlpha(P.accent, 0.35f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f))), clipR, 2.0f, 0);
    dl->PushClipRect(ImVec2(std::max(a0.x, gMin.x), std::max(a0.y, gMin.y)), ImVec2(std::min(a1.x, gMax.x), std::min(a1.y, gMax.y)), true);
    const std::string mutedName = c.muted ? *ccache.name[ci] + "  (muted)" : std::string();
    const char* title = c.muted && !lg ? mutedName.c_str() : lg && !c.name.empty() ? c.name.c_str() : ccache.name[ci]->c_str();  // R07-01: a region's own name  // R03-07: the Tracks layout dims a muted region, no "(muted)" text
    // J-38: the title stays in the part of the clip that shows. A clip lying on
    // top cuts it short (ellipsis before the top clip's edge); when that leaves
    // no room, the title moves past the top clip's end.
    float titleL = std::max(a0.x, gMin.x), titleR = a1.x;
    bool titleGlyph = true;
    // Q-1: a clip that repeats its content shows a loop glyph at the right of
    // its title bar (the title makes room for it). K-114: every clip kind.
    Tick loopLen = c.kind == ClipKind::Audio ? 0 : ccache.loop[ci];
    const Tick loopFrom = c.kind == ClipKind::Audio ? 0 : ccache.loopFrom[ci];  // K-140
    if (c.kind == ClipKind::Audio && c.loop)
      if (const Channel* lch = s.channel(c.source); lch && sampleCache().request(lch->plugin.data)) loopLen = audioLoopLength(s, c, tempoMap);
    const bool repeats = loopRepeatsWithin(c.offset, len, loopLen, loopFrom);
    const bool loopGlyph = repeats && headH >= 9 && a1.x - std::max(a0.x, gMin.x) > 30;
    if (loopGlyph) titleR -= 17;
    // R11-10: regions lying wholly under this one's start (a take over a region, a paste in Overlap)
    // have their names hidden: a small "+N" on the top one's name strip says they are there.
    const int underN = lg && !gliding && ci < ccache.under.size() ? ccache.under[ci] : 0;
    char underTxt[16] = "";
    float underX0 = 0, underX1 = 0;
    if (underN > 0 && headH >= 9) {
      std::snprintf(underTxt, sizeof underTxt, "+%d", std::min(underN, 99));
      const float uw = std::ceil(a.fontBold->CalcTextSizeA(theme::kFontLabel, FLT_MAX, 0.0f, underTxt).x) + 8;
      if (titleR - titleL > uw + 30) {
        underX1 = std::min(titleR, a1.x) - 3, underX0 = underX1 - uw;
        titleR = underX0 - 3;
      }
    }
    if (!gliding && ccache.cover[ci] >= 0) {
      const size_t cv = static_cast<size_t>(ccache.cover[ci]);
      if (!(clipAnim.contains(s.playlist[cv].id))) {
        const float coverX0 = xOf(s.playlist[cv].start), coverX1 = xOf(s.playlist[cv].start + ccache.len[cv]);
        titleR = std::min(titleR, coverX0 - 2);
        if (titleR - titleL < 44 && a1.x - std::max(coverX1, gMin.x) > 52) titleL = std::max(coverX1, gMin.x) + 4, titleR = a1.x, titleGlyph = false;
      }
    }
    // Title bar: menu glyph (click: clip menu, like FL's clip icon), then the name.
    const float tx0 = titleL;
    const bool glyph = !lg && titleGlyph && titleR - tx0 > 34 && headH >= 9;  // Logic: a plain name strip (right-click: the menu)
    // J-142: the title strip's hit geometry, exactly as drawn: the menu arrow
    // and the name box (its text plus a little padding). A right-click there
    // opens the clip menu; anywhere else on the strip it deletes, like the body.
    const bool nameShown = titleR - tx0 > 12;
    const float nameSize = std::min(theme::kFontCaption, headH - 2);
    const ImVec2 namePos(tx0 + (glyph ? 15 : 6), a0.y + std::max(0.0f, (headH - theme::kFontCaption) * 0.5f));
    const float nameMaxW = titleR - tx0 - (glyph ? 19 : 10);
    clipheader::Geom hg;
    hg.x0 = a0.x, hg.y0 = a0.y, hg.x1 = a1.x, hg.y1 = a1.y, hg.headH = headH;
    if (glyph) hg.setMenu(tx0 + 7);
    if (nameShown && nameMaxW > 0) {
      ImFont* nf = clipTitleFont(a);
      hg.setName(namePos.x, nf->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, title).x, nameMaxW);
    }
    devGeom.clips.push_back({static_cast<unsigned>(c.id), hg});
    const clipheader::Zone zone = hovered && io.MousePos.y >= gMin.y ? clipheader::hit(hg, io.MousePos.x, io.MousePos.y) : clipheader::Zone::None;
    if (glyph) {
      const float gx = tx0 + 7, gy = a0.y + headH * 0.5f;
      const bool hotGlyph = zone == clipheader::Zone::Menu;
      if (hotGlyph) hoverMenuGlyph = c.id;
      ui::drawIconFit(dl, ui::Icon::Down, ImVec2(gx - 3.5f, gy - 2), ImVec2(gx + 3.5f, gy + 2.5f),
                      hotGlyph ? P.hot : theme::withAlpha(theme::contrastText(headSolid), 0.8f));
    }
    if (zone == clipheader::Zone::Name && hg.hasName)  // the name box under the pointer (its hit area)
      dl->AddRectFilled(ImVec2(hg.nameX0, a0.y + 1.5f), ImVec2(hg.nameX1, a0.y + headH - 0.5f), theme::withAlpha(theme::contrastText(headSolid), 0.16f),
                        theme::kRadiusSm);
    if (nameShown && lg && !muted) {  // D1: white names (near black on yellow) with a soft shadow; D2: no "…" stubs
      const bool darkName = smoothDarkName(base);
      ImFont* nf = clipTitleFont(a);
      if (nameMaxW >= std::min(28.0f, nf->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, title).x)) {
        if (!darkName) theme::textEllipsis(dl, nf, nameSize, ImVec2(namePos.x, namePos.y + 1), nameMaxW, IM_COL32(0, 0, 0, 102), title);
        theme::textEllipsis(dl, nf, nameSize, namePos, nameMaxW, darkName ? IM_COL32(28, 28, 28, 255) : IM_COL32(255, 255, 255, 250), title);
      }
    } else if (nameShown)
      theme::textEllipsis(dl, clipTitleFont(a), nameSize, namePos, nameMaxW, theme::contrastText(headSolid), title);
    if (underX1 > underX0) {  // R11-10: the "+N" of the regions under this one
      const float uy0 = a0.y + 2, uy1 = a0.y + std::max(10.0f, headH - 1);
      dl->AddRectFilled(ImVec2(underX0, uy0), ImVec2(underX1, uy1), IM_COL32(0, 0, 0, 110), 3.0f);
      const ImVec2 ts = a.fontBold->CalcTextSizeA(theme::kFontLabel, FLT_MAX, 0.0f, underTxt);
      dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(std::round((underX0 + underX1 - ts.x) * 0.5f), std::round((uy0 + uy1 - ts.y) * 0.5f)),
                  IM_COL32(255, 255, 255, 235), underTxt);
      if (hovered && io.MousePos.x >= underX0 && io.MousePos.x < underX1 && io.MousePos.y >= uy0 && io.MousePos.y < uy1 && io.MousePos.y >= gMin.y) {
        std::string names;
        for (size_t k = 0; k < s.playlist.size(); ++k) {
          const Clip& u = s.playlist[k];
          if (k == ci || u.track != c.track || u.start != c.start || !ccache.valid[k] || logicplug::laneClip(s, u)) continue;
          if (ccache.under[k] >= underN) continue;  // (only those under it)
          names += (names.empty() ? "" : ", ") + (u.name.empty() ? *ccache.name[k] : u.name);
        }
        ImGui::SetTooltip("%s %s", i18n::tr("Under this region:"), names.c_str());
      }
    }
    if (nameShown && c.kind == ClipKind::Pattern && headH >= 9) {
      // K-179: the pattern's own meter when it differs from the section the clip starts in (K-236: in a
      // song of one meter too, so a 3/4 or 7/8 pattern in a 4/4 song says so on its clips).
      if (const Pattern* bp = s.pattern(c.pattern)) {
        const MeterSeg& g = mm.segAt(c.start);
        const int bn = s.patternMeterNum(*bp), bd = s.patternMeterDen(*bp);
        if (bn != g.num || bd != g.den) {
          char badge[16];
          std::snprintf(badge, sizeof badge, "%d/%d", bn, bd);
          ImFont* nf = clipTitleFont(a);
          const float nw = std::min(nameMaxW, nf->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, title).x);
          const float bw = a.fontMono->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, badge).x;
          const float bx = namePos.x + nw + 6;
          if (bx + bw + 4 < titleR)
            dl->AddText(a.fontMono, nameSize, ImVec2(bx, namePos.y), theme::withAlpha(theme::contrastText(headSolid), 0.6f), badge);
        }
      }
    }
    if (nameShown && c.kind == ClipKind::Pattern && clipplay::active(c) && headH >= 9) {
      // K-112b3: the clip's playback settings (transpose, velocity, delay, quantise) after its name, in every layout.
      const std::string tg = clipplay::tag(c);
      ImFont* nf = clipTitleFont(a);
      const float nw = std::min(nameMaxW, nf->CalcTextSizeA(nameSize, FLT_MAX, 0.0f, title).x);
      const float tx = namePos.x + nw + 6;
      if (titleR - tx > 24)
        theme::textEllipsis(dl, a.fontRegular, nameSize, ImVec2(tx, namePos.y), titleR - tx - 4, theme::withAlpha(theme::contrastText(headSolid), 0.7f),
                            tg.c_str());
    }
    // R01-14: in the Tracks layout an audio region's waveform fills the region under its name, as
    // the reference draws it (063, 216); the inset rows left it a few pixels tall at the usual height.
    const bool lgWave = lg && c.kind == ClipKind::Audio;
    const ImVec2 b0(a0.x + (lgWave ? 1.0f : 3.0f), a0.y + headH + (lgWave ? 0.0f : 2.0f)), b1(a1.x - (lgWave ? 1.0f : 3.0f), a1.y - (lgWave ? 1.0f : 3.0f));
    const ImU32 mark = P.dark ? P.paper : theme::mix(base, P.ink, 0.6f);
    bool inside = hovered && io.MousePos.x >= a0.x && io.MousePos.x < a1.x && io.MousePos.y >= a0.y && io.MousePos.y < a1.y && io.MousePos.y >= gMin.y;
    // R04-08: while the track shows its automation, its curve takes the clicks under the name bar
    // (points, the curve); the regions keep their name bar for selecting, moving and resizing.
    if (inside && !gliding && inCurveArea(c.track, io.MousePos.y)) inside = false;
    ParamId curveHit{kNoId, 0};  // J-173
    if (loopGlyph)  // K-140: large enough to read as loop arrows (at 4 px it read as a ring), on whole pixels
      ui::drawIconGlyph(dl, ui::Icon::LoopArrows, ImVec2(std::round(std::min(a1.x, gMax.x) - 10), std::round(a0.y + headH * 0.5f)),
                        std::round(std::min(6.0f, headH * 0.42f)), 1.3f, theme::withAlpha(theme::contrastText(headSolid), 0.85f));
    if (c.kind == ClipKind::Pattern) {
      const Pattern& pat = *s.pattern(c.pattern);
      const int pv = st.patternPreview;  // J-173: 0 notes and automation, 1 notes, 2 automation
      const Tick contentEnd = drawClipPreview(dl, s, pat, b0, b1, len, c.offset, loopLen, loopFrom, ink, gMin.x, gMax.x, pv != 2 || pat.automation.empty());
      if (pv != 1 && !pat.automation.empty() && b1.y - b0.y >= (pv == 2 ? 4.0f : 7.0f)) {
        PatCurveHit ph;
        const bool inBody = inside && io.MousePos.y > a0.y + headH;
        drawPatternCurves(dl, pat, b0, b1, len, c.offset, loopLen, loopFrom, theme::mix(base, P.well, 0.43f), gMin.x, gMax.x, io.MousePos, inBody, 5.0f, &ph);
        if (ph.hit >= 0) curveHit = ParamId{pat.automation[static_cast<size_t>(ph.hit)].owner, pat.automation[static_cast<size_t>(ph.hit)].index};
        for (const auto& [k, pt] : ph.sample)
          devGeom.curves.push_back({static_cast<unsigned>(c.id), static_cast<unsigned>(pat.automation[static_cast<size_t>(k)].owner),
                                    pat.automation[static_cast<size_t>(k)].index, pt.x, pt.y});
      }
      const float pxTick = (a1.x - a0.x) / static_cast<float>(std::max<Tick>(1, len));
      if (!repeats && contentEnd >= 0 && contentEnd < len) {  // (loop points: drawn below, for every clip kind)
        // Q-1 / K-114: a clip that does not loop plays nothing past its pattern's
        // end: that part is shaded and its start marked with a dashed line.
        const float x = a0.x + pxTick * static_cast<float>(contentEnd);
        if (x < gMax.x) {
          dl->AddRectFilled(ImVec2(std::max(x, gMin.x), a0.y + headH), a1, theme::withAlpha(P.well, P.dark ? 0.45f : 0.30f));
          const ImU32 dash = theme::withAlpha(P.dark ? P.paper : P.ink, 0.35f);
          for (float yy = a0.y + headH + 1; yy < a1.y - 1; yy += 5) dl->AddLine(ImVec2(x + 0.5f, yy), ImVec2(x + 0.5f, std::min(yy + 2.5f, a1.y - 1)), dash);
        }
      }
    } else if (c.kind == ClipKind::Audio) {
      // D8: in Smooth the wave fills about 80 % of the body, centred.
      const float wInset = lgWave ? std::floor((b1.y - b0.y) * 0.1f) : 0.0f;
      const ImVec2 wa(b0.x, b0.y + wInset), wb(b1.x, b1.y - wInset);
      if (b1.y - b0.y > 4) {
        const Clip& wv = plwarp::shown(s, c, tempoMap, animSnap, animEpsTick);  // K-85: a dragged marker glides as a dragged clip does
        if (c.id == gainGlideClip) {  // J-79: drawn at the gliding gain
          Clip wc = wv;
          wc.gain = clipgain::fromDb(gainGlideDb);
          drawClipWave(dl, s, *s.channel(c.source), wc, tempoMap, wa, wb, x0, st.pxPerTick, gMin.x, gMax.x, ink, xfBelow >= 0 ? &fg : nullptr);
        } else {
          drawClipWave(dl, s, *s.channel(c.source), wv, tempoMap, wa, wb, x0, st.pxPerTick, gMin.x, gMax.x, ink, xfBelow >= 0 ? &fg : nullptr);
        }
        plwarp::drawMarkers(dl, s, wv, tempoMap, b0, b1, x0, st.pxPerTick, gMin.x, gMax.x, st.tool == Tool::Warp, ink);  // K-85
      }
      // J-111: the audio file is still arriving from a collaborator: silent placeholder with progress
      if (float pr = 0; collabui::fileLoading(a, s.channel(c.source)->plugin.data, &pr))
        collabui::drawLoading(dl, ImVec2(a0.x + 1, a0.y + headH), ImVec2(a1.x - 1, a1.y - 1), pr, false);
      // J-38: fades as they play (automatic crossfades included), with their curve shape.
      // R05-17: the Tracks layout shades the faded part darker, as the reference does.
      const ImU32 shade = theme::withAlpha(P.ink, lg ? (P.dark ? 0.55f : 0.32f) : P.dark ? 0.30f : 0.16f);
      const ImU32 fline = P.dark ? theme::withAlpha(P.paper, 0.75f) : theme::mix(base, P.ink, 0.65f);
      ImVec2 pts[kFadePts];
      if (fd.in.len > 0 && b1.y - b0.y > 3) {
        fadePath(a0.x, std::min(xAtSec(fd.in.len), a1.x), b0.y, b1.y, fd.in, true, pts);
        if (xfBelow >= 0)  // J-118: a blended crossfade needs no shade: a faint curve
          dl->AddPolyline(pts, kFadePts, xfCurve(ink), ImDrawFlags_None, 1.25f);
        else
          drawFade(pts, b0.y, shade, fline, fd.in.crossfade ? 1.6f : 1.2f);
      }
      if (fd.out.len > 0 && b1.y - b0.y > 3) {
        fadePath(std::max(xAtSec(dur - fd.out.len), a0.x), a1.x, b0.y, b1.y, fd.out, false, pts);
        drawFade(pts, b0.y, shade, fline, fd.out.crossfade ? 1.6f : 1.2f);
      }
      // Fade handles (drawn after every clip, see below): this clip's own, and
      // the fade-out of the clip it lies on (its crossfade partner).
      if (st.showFades && (inside || fadeDragClip == c.id || gainDragClip == c.id) && (gainDragClip == kNoId || gainDragClip == c.id) &&
          b1.y - b0.y > (lgWave ? 3.0f : 8.0f) && a1.x - a0.x > 24) {
        fadeHandles.clear();
        if (gainDragClip == kNoId || gainDragClip == c.id) {
          // J-79: the volume line: a handle at the bottom middle of the
          // visible part of the clip (FL's place for it).
          const float vx0 = std::max(a0.x, gMin.x), vx1 = std::min(a1.x, gMax.x);
          const float hw = std::clamp((vx1 - vx0) * 0.3f, 12.0f, 44.0f), cx = std::round((vx0 + vx1) * 0.5f);
          gainLine = {c.id, a0.x, a1.x, cx - hw * 0.5f, cx + hw * 0.5f, std::round(a1.y - 4.0f) + 0.5f};
        }
        const float hin = std::max(a0.x + 4, std::min(xAtSec(fd.in.len), a1.x - 8));
        const float hout = std::min(a1.x - 4, std::max(xAtSec(dur - fd.out.len), a0.x + 8));
        // A tied crossfade (both sides follow the overlap): one tension handle steers both.
        auto tiedTo = [&](const EdgeFade& e, bool in) -> ObjectId {
          if (!e.crossfade) return kNoId;
          const int pi = s.clipIndex(e.partner);
          if (pi < 0 || static_cast<size_t>(pi) >= ccache.fades.size()) return kNoId;
          const EdgeFade& o = in ? ccache.fades[static_cast<size_t>(pi)].out : ccache.fades[static_cast<size_t>(pi)].in;
          return o.len > 0 && std::fabs(o.len - e.len) < 1e-6 ? e.partner : kNoId;
        };
        // R01-13: in the Tracks layout the fade handles sit in the region's upper corners (the
        // reference's fade zones; the lower corners resize).
        const float hy = lgWave ? std::round(a0.y + 4.0f) : b0.y;
        fadeHandles.push_back({c.id, 1, ImVec2(hin, hy), kNoId});
        fadeHandles.push_back({c.id, 2, ImVec2(hout, hy), kNoId});
        // R08-08: in the Tracks layout the curve handles come with the Fade tool only, so a Pointer
        // click on a short region's body selects it (the reference bends a fade with the Fade tool).
        const bool curveHandles = !lg || st.tool == Tool::Fade || ((io.KeySuper || io.KeyCtrl) && st.cmdTool == Tool::Fade);
        if (curveHandles && fd.in.len > 0 && hin - a0.x > 14) {
          fadePath(a0.x, std::min(xAtSec(fd.in.len), a1.x), b0.y, b1.y, fd.in, true, pts);
          fadeHandles.push_back({c.id, 3, pts[kFadePts / 2], tiedTo(fd.in, true)});
        }
        if (curveHandles && fd.out.len > 0 && a1.x - hout > 14) {
          fadePath(std::max(xAtSec(dur - fd.out.len), a0.x), a1.x, b0.y, b1.y, fd.out, false, pts);
          fadeHandles.push_back({c.id, 4, pts[kFadePts / 2], tiedTo(fd.out, false)});
        }
        // The clip beneath (crossfading into this one): its fade-out length at the bottom.
        if (fd.in.crossfade) {
          const int pi = s.clipIndex(fd.in.partner);
          if (pi >= 0 && static_cast<size_t>(pi) < ccache.fades.size() && ccache.fades[static_cast<size_t>(pi)].out.crossfade) {
            const Clip& pc = s.playlist[static_cast<size_t>(pi)];
            const double pEnd = tempoMap.secondsAt(static_cast<double>(pc.start + ccache.len[static_cast<size_t>(pi)]));
            const float px = xOfD(tempoMap.tickAtSeconds(pEnd - ccache.fades[static_cast<size_t>(pi)].out.len));
            if (px > a0.x - 2 && px < a1.x) fadeHandles.push_back({pc.id, 2, ImVec2(std::max(a0.x + 4, px), b1.y), kNoId});
          }
        }
      }
    } else if (c.kind == ClipKind::Automation && b1.y - b0.y > 6) {
      AutomationClip shownScratch;
      const AutomationClip& ac = shownCurve(*s.automation(c.source), c, shownScratch);
      const AutoGeom g = withOwnLoop(AutoGeom{b0, b1, x0, st.pxPerTick, c.start, c.offset, len}, c);
      int hp = -1, hh = -1;
      if (inside && io.MousePos.y > a0.y + headH) hitAutomation(ac, g, io.MousePos, hp, hh);
      if (cg.kind == ClipGesture::AutoPoint && cg.anchor == c.id) hp = cg.point;
      if (cg.kind == ClipGesture::AutoTension && cg.anchor == c.id) hh = cg.point;
      drawAutomation(dl, ac, g, gMin.x, gMax.x, ink, theme::withAlpha(base, muted ? 0.08f : 0.18f), inside || sel, hp, hh, mark);
      if (inside) hoverPoint = hp, hoverHandle = hh;
    }
    if (repeats) {  // K-114: the loop points of every looping clip kind
      const float pxTick = (a1.x - a0.x) / static_cast<float>(std::max<Tick>(1, len));
      // K-11: each loop point is a small V cut into the clip's top and bottom
      // edges (grid colour behind, so the clip reads as notched), joined by a
      // faint hairline through the body; repeats are not dimmed. Narrow passes
      // (zoomed far out) show none of it.
      const loopnotch::Geom ng = loopnotch::geom(pxTick * static_cast<float>(loopLen), a1.y - a0.y);
      if (ng.show) {
        // K-26: the hairline is a faint seam in the grid's colour (as the notches
        // are cut to it), one crisp pixel: a light line read as a stray, brighter
        // grid line next to the real ones seen through the body.
        const ImU32 hair = theme::withAlpha(P.well, P.dark ? 0.34f : 0.30f);
        const float yb0 = a0.y + headH, yb1 = a1.y, hw = ng.width * 0.5f;
        for (const ClipPass& ps : loopPasses(c.offset, len, loopLen, loopFrom)) {
          if (ps.index == 0) continue;
          const float x = std::round(a0.x + pxTick * static_cast<float>(ps.at));
          if (x - hw > gMax.x) break;
          if (x + hw < gMin.x || x - a0.x < ng.width || a1.x - x < ng.width) continue;
          // The grid behind (odd bars are a step lighter, as drawn above).
          const ImU32 bg = !lg && (tickAt(x) / barT) % 2 == 1 ? theme::mix(well, P.paper, P.dark ? 0.035f : 0.018f) : well;
          dl->AddLine(ImVec2(x, yb0 + 1), ImVec2(x, yb1 - ng.depth), hair, 1.0f);  // (AddLine centres on pixel x)
          // Slightly past the edge (the clip rectangle cuts it), so no anti-aliased
          // seam is left along the rim; the tip lands on the hairline's pixel centre.
          dl->AddTriangleFilled(ImVec2(x + 0.5f - hw, a0.y - 1), ImVec2(x + 0.5f + hw, a0.y - 1), ImVec2(x + 0.5f, a0.y + ng.depth), bg);
          dl->AddTriangleFilled(ImVec2(x + 0.5f - hw, a1.y + 1), ImVec2(x + 0.5f, a1.y - ng.depth), ImVec2(x + 0.5f + hw, a1.y + 1), bg);
        }
      }
    }
    dl->PopClipRect();
    if (inside) {
      hoverClip = c.id;
      hoverCurve = curveHit;  // J-173: a pattern automation curve under the pointer (owner kNoId: none)
      hoverZone = zone;  // J-142 (the top clip owns the pointer: a covered clip's arrow does not open)
      hoverMenuGlyph = zone == clipheader::Zone::Menu ? c.id : kNoId;
      float edge = std::min(8.0f, (a1.x - a0.x) * 0.3f);
      hoverEdge = io.MousePos.x > a1.x - edge;
      hoverLeftEdge = !hoverEdge && io.MousePos.x < a0.x + edge;
      hoverBody = io.MousePos.y > a0.y + headH;
      if (c.kind != ClipKind::Automation) hoverPoint = hoverHandle = -1;
      if (c.kind != ClipKind::Audio && fadeDragClip == kNoId) fadeHandles.clear();  // the top clip owns the pointer
      if (gainLine.clip != c.id && gainDragClip == kNoId) gainLine = GainLine{};      // (its volume line too)
    }
  };
  // Stacking (J-38): by track, then start (a later clip lies on the one it
  // overlaps); clips under a drag (gliding) draw last, on top of everything.
  const bool anyAnim = !clipAnim.empty();
  // K-276: the gap between two clips that touch in time is one plain fill (the well), drawn before
  // the clips: no bar line, beat line or bar shading shows in it, so every such gap looks the same
  // (a bar line in one gap and none in the next read as uneven spacing). Off-grid gaps match too.
  {
    uint32_t prev = UINT32_MAX;
    for (uint32_t ci : ccache.order) {
      if (prev != UINT32_MAX && ccache.valid[prev] && ccache.valid[ci]) {
        const Clip& p = s.playlist[prev];
        const Clip& c = s.playlist[ci];
        if (p.track == c.track && p.start + ccache.len[prev] == c.start && c.track < nTracks && !hiddenTake(p) && !hiddenTake(c) &&
            !(anyAnim && (clipAnim.contains(p.id) || clipAnim.contains(c.id)))) {
          const float x = xOf(c.start), y = yOf(c.track), th = hOf(c.track);
          const pixelgrid::Span gapL = pixelgrid::block(xOf(p.start), x, dpp), gapR = pixelgrid::block(x, xOf(c.start + ccache.len[ci]), dpp);
          const pixelgrid::Span gapY = pixelgrid::block(y, y + th, dpp);
          if (gapR.x0 > gapL.x1 && gapL.x1 < gMax.x && gapR.x0 > gMin.x && th >= 1 && y + th > gMin.y && y < gMax.y)
            dl->AddRectFilled(ImVec2(gapL.x1, gapY.x0), ImVec2(gapR.x0, gapY.x1), well);
        }
      }
      prev = ci;
    }
  }
  for (uint32_t ci : ccache.order)
    if (!anyAnim || !clipAnim.contains(s.playlist[ci].id)) drawClip(ci);
  if (anyAnim)
    for (uint32_t ci : ccache.order)
      if (clipAnim.contains(s.playlist[ci].id)) drawClip(ci);
  if (liveLook)  // K-112c (checkpoint 51): a folded group's lane shows its tracks' clips as thin bars, one row per track (150)
    for (int g = 0; g < nTracks; ++g) {
      if (!trackmodes::isGroupTrack(s, g) || !s.tracks[static_cast<size_t>(g)].folded || hOf(g) < 1) continue;
      const int gs = s.tracks[static_cast<size_t>(g)].groupSize;
      const float y = yOf(g), th = hOf(g);
      if (y > gMax.y || y + th < gMin.y) continue;
      const float bh = std::max(2.0f, std::floor((th - 6.0f) / static_cast<float>(std::max(1, gs))));
      for (uint32_t ci : ccache.order) {
        const Clip& c = s.playlist[ci];
        if (!ccache.valid[ci] || c.track <= g || c.track > g + gs) continue;
        const float x0 = snapX(c.start), x1 = snapX(c.start + ccache.len[ci]);
        if (x1 < gMin.x || x0 > gMax.x) continue;
        const float by = std::round(y + 3.0f + bh * static_cast<float>(c.track - g - 1));
        dl->AddRectFilled(ImVec2(x0, by), ImVec2(std::max(x0 + 1.0f, x1 - 1.0f), by + std::max(1.0f, bh - 1.0f)), theme::rgb(ccache.color[ci]));
      }
    }
  if (liveLook && st.selEnd > st.selStart && !st.selTracks.empty())  // N50-08: the time selection shows over the clips it covers too
    for (int t : st.selTracks) {
      if (t < 0 || t >= nTracks || hOf(t) < 1) continue;
      const float y0 = std::max(gMin.y, yOf(t)), y1 = std::min(gMax.y, yOf(t + 1));
      const float x0 = std::max(gMin.x, selX0), x1 = std::min(gMax.x, selX1);
      if (y1 <= y0 || x1 <= x0) continue;
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), theme::withAlpha(P.paper, 0.22f));
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), theme::withAlpha(P.paper, 0.55f), 0.0f, 0, 1.0f);
    }
  clipAnim.prune(animNow, gestureAnimates(cg.kind));
  // J-217: takes still recording, each drawn as the clip it will become (the
  // same track, start, colours, title and waveform as the committed clip), its
  // right edge at the newest recorded frame, with a record-red rim, a lit edge
  // at the record head and a pulsing dot in the title bar.
  {
    static const Channel liveCh = [] {
      Channel c;
      c.plugin = defaultPluginState("gl.sampler");  // as commitTakes() makes the take's channel
      return c;
    }();
    dl->PushClipRect(gMin, gMax, true);
    for (livetake::View& v : livetake::views()) {
      v.drawn = false;
      const int tr = v.place.track;
      if (tr < 0 || tr >= nTracks) continue;
      if (v.place.lane && v.place.muted) continue;  // K-123: an earlier pass goes to the lanes; the row shows the one recording
      const float x0 = xOf(v.span.start), x1 = xOf(v.span.start + v.span.length), y = yOf(tr), th = hOf(tr);
      if (x1 < gMin.x || x0 > gMax.x || y + th < gMin.y || y > gMax.y || th < 1) continue;
      const bool muted = v.place.muted || s.tracks[static_cast<size_t>(tr)].muted;
      const ImU32 base = theme::rgb(v.place.color);
      const float bodyA = muted ? 0.60f : 0.74f, headA = muted ? 0.80f : 0.90f;
      const ImU32 headSolid = theme::mix(base, P.well, muted ? 0.56f : 0.0f);
      const ImU32 body = theme::withAlpha(theme::mix(base, P.well, muted ? 0.75f : 0.43f), bodyA);
      const ImU32 head = theme::withAlpha(headSolid, headA);
      const pixelgrid::Span spanX = pixelgrid::block(x0, x1, dpp), spanY = pixelgrid::block(y, y + th, dpp);  // K-276: as clips
      const ImVec2 a0(spanX.x0, spanY.x0), a1(spanX.x1, spanY.x1);
      const float headH = std::min(15.0f, (a1.y - a0.y) * 0.45f);
      v.drawn = true, v.x0 = a0.x, v.x1 = a1.x, v.y0 = a0.y, v.y1 = a1.y;
      // Over clips already on the track the take lies on top (the grid beneath, as stacked clips draw).
      for (const DrawnClip& p : drawn)
        if (p.track == tr && p.x1 > a0.x && p.x0 < a1.x) gridPatch(std::max(p.x0, a0.x), std::min(p.x1, a1.x) + 1.0f, a0.y, a1.y);
      dl->AddRectFilled(ImVec2(a0.x, a0.y + headH), a1, body, theme::kRadiusSm, ImDrawFlags_RoundCornersBottom);
      dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + headH), head, theme::kRadiusSm, ImDrawFlags_RoundCornersTop);
      dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + headH), theme::withAlpha(P.danger, 0.16f), theme::kRadiusSm, ImDrawFlags_RoundCornersTop);
      dl->AddLine(ImVec2(a0.x + 1, a0.y + 0.5f), ImVec2(a1.x - 1, a0.y + 0.5f), theme::withAlpha(P.paper, muted ? 0.08f : 0.28f));
      dl->AddLine(ImVec2(a0.x, a0.y + headH), ImVec2(a1.x, a0.y + headH), theme::withAlpha(P.ink, 0.35f));
      {
        const float in = pixelgrid::strokeIn(1.5f);  // K-276: inside the box
        dl->AddRect(ImVec2(a0.x + in, a0.y + in), ImVec2(a1.x - in, a1.y - in), theme::withAlpha(P.danger, 0.85f), theme::kRadiusSm, 1.5f, 0);
      }
      dl->PushClipRect(ImVec2(std::max(a0.x, gMin.x), std::max(a0.y, gMin.y)), ImVec2(std::min(a1.x, gMax.x), std::min(a1.y, gMax.y)), true);
      // Title bar as the committed clip shows it (menu arrow, name), the record dot at its right.
      const std::string title = muted && v.place.muted ? v.place.name + "  (muted)" : v.place.name;
      const float tx0 = std::max(a0.x, gMin.x);
      const bool dot = headH >= 9 && a1.x - tx0 > 24;
      const float titleR = a1.x - (dot ? 14.0f : 0.0f);
      const bool glyph = titleR - tx0 > 34 && headH >= 9;
      if (glyph) {
        const float gx = tx0 + 7, gy = a0.y + headH * 0.5f;
        ui::drawIconFit(dl, ui::Icon::Down, ImVec2(gx - 3.5f, gy - 2), ImVec2(gx + 3.5f, gy + 2.5f), theme::withAlpha(theme::contrastText(headSolid), 0.8f));
      }
      if (titleR - tx0 > 12) {
        const float nameSize = std::min(theme::kFontCaption, headH - 2);
        const ImVec2 namePos(tx0 + (glyph ? 15 : 6), a0.y + std::max(0.0f, (headH - theme::kFontCaption) * 0.5f));
        theme::textEllipsis(dl, clipTitleFont(a), nameSize, namePos, titleR - tx0 - (glyph ? 19 : 10), theme::contrastText(headSolid), title.c_str());
      }
      if (dot) {
        const float pulse = v.done ? 0.6f : 0.55f + 0.45f * static_cast<float>(std::fabs(std::sin(a.now * 4)));
        dl->AddCircleFilled(ImVec2(std::min(a1.x, gMax.x) - 8, a0.y + headH * 0.5f), std::min(3.5f, headH * 0.26f), theme::withAlpha(P.danger, pulse));
      }
      // The waveform, from the take's live summary, exactly as the committed clip draws it.
      const ImVec2 b0(a0.x + 3, a0.y + headH + 2), b1(a1.x - 3, a1.y - 3);
      if (b1.y - b0.y > 4) {
        Clip lc;
        lc.kind = ClipKind::Audio;
        lc.track = tr;
        lc.start = v.span.start;
        lc.length = v.span.length;
        const ImU32 ink = liveLook ? liveClipInk(base, muted) : theme::clipInk(base, muted);  // K-177: follows the theme (white text pushes it)
        drawWaveformFrom(dl, s, liveCh, lc, tempoMap, b0, b1, x0, st.pxPerTick, gMin.x, gMax.x, ink, nullptr, *v.peaks,
                         static_cast<size_t>(v.fileFrames), v.sampleRate, nullptr);
      }
      dl->PopClipRect();
      // The record head: the take's newest frame.
      const double headSec = tempoMap.secondsAt(static_cast<double>(v.span.start)) + static_cast<double>(v.fileFrames) / v.sampleRate;
      const float hx = xOfD(tempoMap.tickAtSeconds(headSec));
      if (!v.done && hx >= a0.x && hx <= gMax.x) dl->AddLine(ImVec2(hx, a0.y), ImVec2(hx, a1.y), P.danger, 2.0f);
    }
    dl->PopClipRect();
  }
  // J-38: overlaps. The clip beneath keeps its end edge through the clip on
  // top (K-26: a crisp rim-coloured edge; a dashed lit outline of the shared
  // span while it is selected), so both extents read at a glance; an audio
  // crossfade shows both curves crossing (an X).
  if (drawn.size() > 1) {
    std::vector<size_t> byTrack(drawn.size());
    for (size_t k = 0; k < drawn.size(); ++k) byTrack[k] = k;
    std::stable_sort(byTrack.begin(), byTrack.end(), [](size_t p, size_t q) { return drawn[p].track != drawn[q].track ? drawn[p].track < drawn[q].track : drawn[p].x0 < drawn[q].x0; });
    auto dashH = [&](float xa, float xb, float yy, ImU32 col) {
      for (float x = xa; x < xb; x += 7.0f) dl->AddLine(ImVec2(x, yy), ImVec2(std::min(x + 4.0f, xb), yy), col, 1.0f);
    };
    auto dashV = [&](float xx, float ya, float yb, ImU32 col) {
      for (float yy = ya; yy < yb; yy += 6.0f) dl->AddLine(ImVec2(xx, yy), ImVec2(xx, std::min(yy + 3.0f, yb)), col, 1.0f);
    };
    dl->PushClipRect(gMin, gMax, true);
    int budget = 4096;  // pathological stacks: bounded work per frame
    for (size_t u = 0; u < byTrack.size() && budget > 0; ++u)
      for (size_t v = u + 1; v < byTrack.size() && budget > 0; ++v, --budget) {
        const size_t iu = byTrack[u], iv = byTrack[v];
        if (drawn[iv].track != drawn[iu].track || drawn[iv].x0 >= drawn[iu].x1) break;
        const DrawnClip& p = drawn[std::min(iu, iv)];  // drawn first: beneath
        const DrawnClip& q = drawn[std::max(iu, iv)];  // drawn later: on top
        const float ox0 = std::max(p.x0, q.x0), ox1 = std::min(p.x1, q.x1);
        if (ox1 - ox0 < 1.0f) continue;
        const Clip& pc = s.playlist[p.ci];
        if (q.xfBelow >= 0 && &drawn[static_cast<size_t>(q.xfBelow)] == &p) {
          // J-118: a blended crossfade: no outline of the clip beneath; its fade-out as a faint curve.
          const EdgeFade& e = ccache.fades[p.ci].out;
          const double pEnd = tempoMap.secondsAt(static_cast<double>(pc.start + ccache.len[p.ci]));
          const float xa = std::max(p.x0, xOfD(tempoMap.tickAtSeconds(pEnd - e.len)));
          if (p.x1 - xa > 2 && q.body < q.y1 - 6) {
            ImVec2 pts[kFadePts];
            fadePath(xa, p.x1, q.body + 2, std::min(p.y1, q.y1) - 3, e, false, pts);
            dl->AddPolyline(pts, kFadePts, xfCurve(p.ink), ImDrawFlags_None, 1.25f);
          }
          continue;
        }
        // K-26: the clip beneath shows through (its ghost), so its ends inside
        // the top clip are its own rim colour, one crisp pixel down the body:
        // the light dashes read as stray, brighter grid lines. A selected clip
        // beneath keeps its lit dashed outline (its whole extent at a glance).
        const float ey0 = std::max(p.body, q.body), ey1 = std::min(p.y1, q.y1) - 1;
        if (p.sel) {
          dashH(ox0, ox1, std::max(p.y0, q.y0) + 0.5f, P.hot);
          dashH(ox0, ox1, std::min(p.y1, q.y1) - 0.5f, P.hot);
          if (p.x1 > q.x0 + 1 && p.x1 < q.x1 - 1) dashV(p.x1 - 0.5f, std::max(p.y0, q.y0), std::min(p.y1, q.y1), P.hot);
          if (p.x0 > q.x0 + 1 && p.x0 < q.x1 - 1) dashV(p.x0 + 0.5f, std::max(p.y0, q.y0), std::min(p.y1, q.y1), P.hot);
        } else if (ey1 - ey0 > 2) {
          const ImU32 edge = theme::withAlpha(p.rim, 0.75f);
          if (p.x1 > q.x0 + 1 && p.x1 < q.x1 - 1) dl->AddLine(ImVec2(std::floor(p.x1) - 1, ey0), ImVec2(std::floor(p.x1) - 1, ey1), edge, 1.0f);
          if (p.x0 > q.x0 + 1 && p.x0 < q.x1 - 1) dl->AddLine(ImVec2(std::floor(p.x0), ey0), ImVec2(std::floor(p.x0), ey1), edge, 1.0f);
        }
        // Crossfade: the fade-out of the clip beneath, drawn over the clip on top.
        if (pc.kind == ClipKind::Audio && p.ci < ccache.fades.size() && ccache.fades[p.ci].out.len > 0 && q.body < q.y1 - 6 &&
            !clipAnim.contains(pc.id)) {
          const EdgeFade& e = ccache.fades[p.ci].out;
          const double pEnd = tempoMap.secondsAt(static_cast<double>(pc.start + ccache.len[p.ci]));
          const float xa = std::max(p.x0, xOfD(tempoMap.tickAtSeconds(pEnd - e.len)));
          if (p.x1 - xa > 2 && xa < q.x1) {
            ImVec2 pts[kFadePts];
            fadePath(xa, p.x1, q.body + 2, std::min(p.y1, q.y1) - 3, e, false, pts);
            const ImU32 col = P.dark ? theme::mix(theme::rgb(ccache.color[p.ci]), P.paper, 0.55f) : theme::mix(theme::rgb(ccache.color[p.ci]), P.ink, 0.55f);
            dl->AddPolyline(pts, kFadePts, theme::withAlpha(col, 0.95f), ImDrawFlags_None, 1.6f);
          }
        }
      }
    dl->PopClipRect();
  }
  // Fade handles on top of everything (J-38): squares set the length, rings the curve tension.
  if (!fadeHandles.empty() && hovered) {
    // K-112c checkpoint 57 (round 13, part 5): in Clips only the handle itself (its 7 px square and a
    // pixel round it) grabs it, so a click just inside a clip's body selects the clip.
    float best = liveLook ? 4.5f : 7.0f;
    for (const auto& h : fadeHandles) {
      const float d = std::max(std::fabs(io.MousePos.x - h.pos.x), std::fabs(io.MousePos.y - h.pos.y));
      if (fadeDragClip == kNoId && gainDragClip == kNoId && d < best) best = d, fadeHoverClip = h.clip, fadeHoverKind = h.kind, fadeHoverMirror = h.mirror;
    }
  }
  // J-79: the volume line under the fade handles' layer: a faint line across
  // the clip while it is hot, the handle bar, and (dragging) a small readout.
  ObjectId gainHoverClip = kNoId;
  if (gainLine.clip != kNoId) {
    const GainLine& gln = gainLine;
    if (hovered && gainDragClip == kNoId && fadeDragClip == kNoId && fadeHoverClip == kNoId && io.MousePos.x >= gln.hx0 - 3 &&
        io.MousePos.x <= gln.hx1 + 3 && std::fabs(io.MousePos.y - gln.y) <= 4.5f)
      gainHoverClip = gln.clip;
    const bool dragging = gainDragClip == gln.clip, hot = dragging || gainHoverClip == gln.clip;
    const Clip* lc = s.clip(gln.clip);
    if (hot) {
      const float lx0 = std::max(gln.x0 + 3, gMin.x), lx1 = std::min(gln.x1 - 3, gMax.x);
      for (float x = lx0; x < lx1; x += 6.0f)  // dashed, so the waveform still reads through it
        dl->AddLine(ImVec2(x, gln.y), ImVec2(std::min(x + 3.0f, lx1), gln.y), theme::withAlpha(P.dark ? P.paper : P.ink, 0.45f), 1.0f);
    }
    const ImU32 edge = theme::withAlpha(P.ink, 0.7f), fill = hot ? P.hot : theme::withAlpha(P.paper, 0.85f);
    dl->AddRectFilled(ImVec2(gln.hx0 - 1, gln.y - 2.5f), ImVec2(gln.hx1 + 1, gln.y + 2.5f), edge, 2.0f);
    dl->AddRectFilled(ImVec2(gln.hx0, gln.y - 1.5f), ImVec2(gln.hx1, gln.y + 1.5f), fill, 1.5f);
    if (dragging && lc) {  // discreet readout above the handle
      const std::string txt = clipgain::label(lc->gain);
      const float fs = theme::kFontCaption;
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, txt.c_str());
      const float cx = (gln.hx0 + gln.hx1) * 0.5f;
      const ImVec2 p0(std::round(cx - ts.x * 0.5f - 5), std::round(gln.y - 8 - ts.y - 4)), p1(p0.x + ts.x + 10, p0.y + ts.y + 4);
      dl->AddRectFilled(p0, p1, theme::withAlpha(P.dark ? P.surface950 : P.ink, 0.78f), 3.0f);
      dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 5, p0.y + 2), P.dark ? P.text : P.paper, txt.c_str());
    }
  }
  for (const auto& h : fadeHandles) {
    const bool hot = (fadeDragClip == h.clip && fadeDragKind == h.kind) || (fadeHoverClip == h.clip && fadeHoverKind == h.kind);
    const ImU32 fill = hot ? P.hot : lg ? IM_COL32(255, 255, 255, 235) : theme::withAlpha(P.paper, 0.85f), edge = theme::withAlpha(P.ink, 0.7f);  // D9: white in Smooth
    if (h.kind <= 2) {
      dl->AddRectFilled(ImVec2(h.pos.x - 3.5f, h.pos.y - 3.5f), ImVec2(h.pos.x + 3.5f, h.pos.y + 3.5f), edge, 1.0f);
      dl->AddRectFilled(ImVec2(h.pos.x - 2.5f, h.pos.y - 2.5f), ImVec2(h.pos.x + 2.5f, h.pos.y + 2.5f), fill, 1.0f);
    } else {
      dl->AddCircleFilled(h.pos, 4.0f, edge, 12);
      dl->AddCircleFilled(h.pos, 3.0f, hot ? P.hot : P.well, 12);
      dl->AddCircle(h.pos, 3.0f, fill, 12, 1.2f);
    }
  }
  // K-112B (checkpoint 46): the closed Automation folder's region: the folder's curves in their own
  // colours over the time they cover, as the reference's folder region shows its tracks in small.
  if (foldShut && foldHead < nTracks && hOf(foldHead) >= 1 && yOf(foldHead) < gMax.y && yOf(foldHead) + hOf(foldHead) > gMin.y) {
    const float fy0 = yOf(foldHead) + 1, fy1 = yOf(foldHead) + hOf(foldHead) - 2;
    Tick f0 = -1, f1 = 0;
    for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.valid.size(); ++ci)
      if (ccache.valid[ci] && fold.isMember(s.playlist[ci].track)) {
        const Clip& c = s.playlist[ci];
        f0 = f0 < 0 ? c.start : std::min(f0, c.start), f1 = std::max(f1, c.start + ccache.len[ci]);
      }
    const float x0 = xOf(std::max<Tick>(0, f0)), x1 = xOf(f1);
    if (f0 >= 0 && x1 > gMin.x && x0 < gMax.x) {
      dl->PushClipRect(ImVec2(gMin.x, std::max(gMin.y, fy0)), ImVec2(gMax.x, std::min(gMax.y, fy1 + 1)), true);
      // D7 (checkpoint 49): the reference's folder region: one summary region in the folder's colour
      // (its first track's, else a neutral grey) with its tracks' regions as thin bars, one row per
      // track, under a bold white name (it was a grey band of small curves).
      const std::vector<Rgb>& fcols = smoothTrackColors(a);
      const Rgb fc = foldHead < static_cast<int>(fcols.size()) ? fcols[static_cast<size_t>(foldHead)] : 0;
      const ImU32 face = fc != 0 ? smoothRegionColor(theme::rgb(fc)) : IM_COL32(104, 104, 108, 255);
      dl->AddRectFilled(ImVec2(x0 + 1, fy0), ImVec2(x1 - 1, fy1), face, 4.0f);
      std::string title = i18n::tr("Automation");
      int named = 0;
      for (const int tr : fold.folder) {
        const std::string& nm = s.tracks[static_cast<size_t>(tr)].name;
        if (nm.empty()) continue;
        if (named < 3) title += (named ? ", " : ": ") + nm;
        ++named;
      }
      if (named > 3) title += ", +" + std::to_string(named - 3);
      const float tx = std::max(x0 + 5, gMin.x + 4);
      const ImU32 nameCol = smoothDarkName(face) ? IM_COL32(20, 20, 20, 235) : IM_COL32(255, 255, 255, 245);
      const float nameH = fy1 - fy0 >= 20 ? 13.0f : 0.0f;
      if (nameH > 0) dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(tx, fy0 + 1), nameCol, title.c_str());
      const int rows = std::max<int>(1, static_cast<int>(fold.folder.size()));
      const float by0 = fy0 + nameH + 2, by1 = fy1 - 2, rowH = std::max(1.0f, (by1 - by0) / static_cast<float>(rows));
      const ImU32 bar = theme::withAlpha(nameCol, 0.55f);
      if (by1 - by0 >= 3)
        for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.valid.size(); ++ci) {
          const Clip& c = s.playlist[ci];
          if (!ccache.valid[ci] || !fold.isMember(c.track)) continue;
          const auto it = std::find(fold.folder.begin(), fold.folder.end(), c.track);
          const float r = static_cast<float>(it - fold.folder.begin());
          const float yb = std::round(by0 + rowH * r + rowH * 0.5f - std::min(2.0f, rowH * 0.35f));
          const float hb = std::max(1.0f, std::min(3.0f, rowH * 0.7f));
          dl->AddRectFilled(ImVec2(std::max(xOf(c.start), x0 + 3) + 1, yb), ImVec2(std::min(xOf(c.start + ccache.len[ci]), x1 - 3) - 1, yb + hb), bar, 1.0f);
        }
      dl->PopClipRect();
    }
  }
  // Empty arrangement: muted mark (guide §08 C).
  // K-112c (round 42): not in Clips, where a new set is its empty lanes and the drop room, as the refs.
  if (s.playlist.empty() && livetake::views().empty() && !liveLook && lg) {
    // R09-09: Smooth's own empty state: quiet text on the grid, no Studio mark or pattern wording.
    const char* t1 = i18n::tr("No regions yet");
    const char* t2 = i18n::tr("Draw one with the Pencil, record, or drag audio files and patches in from the Browser");
    const ImVec2 s1 = a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, t1), s2 = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, t2);
    const float cx = (gMin.x + gMax.x) * 0.5f, cy = std::round((gMin.y + gMax.y) * 0.5f);
    if (gMax.x - gMin.x > s2.x + 24 && gMax.y - gMin.y > 60) {
      dl->AddText(a.fontBold, theme::kFontBody, ImVec2(std::round(cx - s1.x * 0.5f), cy - s1.y - 3), theme::withAlpha(P.textDim, 0.9f), t1);
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(cx - s2.x * 0.5f), cy + 3), P.textFaint, t2);
    }
  } else if (s.playlist.empty() && livetake::views().empty() && !liveLook)  // (J-217: not under a take being recorded)
    ui::EmptyState(dl, gMin, gMax, "Empty arrangement", "Click to place the selected pattern, or drag one in from the list");
  // K-123: take lanes. One lane per take under the track: every clip of the take, the parts in the
  // comp (playing) lit in the take's colour, the rest dimmed; a swipe in progress shows its range.
  struct TakeSwipe {
    int track = -1, take = -1;
    Tick from = 0, to = 0;
    float x0 = 0;
  };
  static TakeSwipe tswipe;
  auto takeLaneTop = [&](int tr) { return yOf(tr) + hOf(tr); };  // hOf: the row with its automation, less the take lanes
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    const int nl = takeLaneN[static_cast<size_t>(tr)];
    if (nl <= 0) continue;
    const float top = takeLaneTop(tr);
    for (int k = 0; k < nl; ++k) {
      const float ly0 = top + static_cast<float>(k) * laneTH, ly1 = ly0 + laneTH;
      if (ly1 < gMin.y || ly0 > gMax.y) continue;
      dl->AddRectFilled(ImVec2(gMin.x, ly0), ImVec2(gMax.x, ly1), theme::withAlpha(P.well, k % 2 ? 0.30f : 0.45f));
      dl->AddLine(ImVec2(gMin.x, ly1 - 0.5f), ImVec2(gMax.x, ly1 - 0.5f), theme::withAlpha(P.line, 0.6f));
      for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.valid.size(); ++ci) {
        const Clip& c = s.playlist[ci];
        if (c.track != tr || c.takeGroup == kNoId || c.take != k || !ccache.valid[ci]) continue;
        const float cx0 = xOf(c.start), cx1 = xOf(c.start + ccache.len[ci]);
        if (cx1 < gMin.x || cx0 > gMax.x) continue;
        const ImU32 base = theme::rgb(ccache.color[ci]);
        const bool sel = st.selected.count(c.id) > 0;
        const pixelgrid::Span sx = pixelgrid::block(cx0, cx1, dpp), sy = pixelgrid::block(ly0, ly1, dpp, 1.0f, 1.0f);
        const ImVec2 b0(sx.x0, sy.x0), b1(sx.x1, sy.x1);
        dl->AddRectFilled(b0, b1, c.muted ? theme::withAlpha(theme::mix(base, P.well, 0.7f), 0.55f) : theme::withAlpha(base, 0.62f), 2.0f);
        if (const Channel* ch = s.channel(c.source); ch && c.kind == ClipKind::Audio && b1.y - b0.y > 6)
          drawClipWave(dl, s, *ch, c, tempoMap, ImVec2(b0.x, b0.y + 2), ImVec2(b1.x, b1.y - 2), cx0, st.pxPerTick, gMin.x, gMax.x,
                       c.muted ? theme::withAlpha(theme::clipInk(base, true), 0.55f) : theme::clipInk(base, false));
        dl->AddRect(b0, b1, sel ? P.hot : c.muted ? theme::withAlpha(base, 0.35f) : theme::mix(base, P.paper, 0.35f), 2.0f, 0, c.muted ? 1.0f : 1.5f);
      }
      if (tswipe.track == tr && tswipe.take == k && tswipe.to != tswipe.from) {  // the swipe's range
        const float sx0 = xOf(std::min(tswipe.from, tswipe.to)), sx1 = xOf(std::max(tswipe.from, tswipe.to));
        dl->AddRectFilled(ImVec2(sx0, ly0 + 1), ImVec2(sx1, ly1 - 1), theme::withAlpha(P.hot, 0.22f));
        dl->AddRect(ImVec2(sx0, ly0 + 1), ImVec2(sx1, ly1 - 1), theme::withAlpha(P.hot, 0.8f), 0, 0, 1.0f);
      }
    }
  }
  // K-112b (b4): the automation lanes: the parameter's curve over the song, the clips that drive it
  // shaded, their points as dots; where nothing drives it, its present value as a dashed line.
  static const std::vector<AutomationLane> kNoLanes;
  const std::vector<AutomationLane>& songLanes = !autoLanes.empty() && lg ? arrangeLanes(a) : kNoLanes;
  // R04-08: the value's height in a row's automation area (curveTop / curveBot).
  auto laneValueY = [&](int tr, double n) {
    const float t0 = curveTop(tr) + 3, t1 = curveBot(tr) - 3;
    return t0 + static_cast<float>(1.0 - std::clamp(n, 0.0, 1.0)) * std::max(1.0f, t1 - t0);
  };
  // R10 CPU: what the lanes draw is kept between frames while nothing moves: each lane's sampled
  // curve and its points' value labels (a curve was sampled every 3 px and every label formatted
  // again each frame), and the regions under each lane are found in one pass over the song.
  struct LaneDraw {
    uint64_t rev = ~0ull;
    float scrollX = 0, ppt = 0, x0 = 0, x1 = 0, top = 0, bot = 0;
    const AutomationLane* al = nullptr;
    std::vector<ImVec2> pts;
    uint64_t labelRev = ~0ull;
    std::vector<std::string> labels;  // per point, in the order the points are drawn
    std::vector<ImVec2> labelSize;
    ParamId labelPid{};
  };
  static std::map<int, LaneDraw> laneDraw;
  static std::vector<std::vector<uint32_t>> laneRegions;  // per track: its regions (playlist indices) under a lane
  if (!autoLanes.empty() && lg) {
    if (laneRegions.size() < static_cast<size_t>(nTracks)) laneRegions.resize(static_cast<size_t>(nTracks));
    for (auto& v : laneRegions) v.clear();
    for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.valid.size(); ++ci) {
      const Clip& c = s.playlist[ci];
      if (c.track >= 0 && c.track < nTracks && ccache.valid[ci] && autoLanes.count(c.track) && !logicplug::laneClip(s, c))
        laneRegions[static_cast<size_t>(c.track)].push_back(static_cast<uint32_t>(ci));
    }
  }
  for (auto it = laneDraw.begin(); it != laneDraw.end();) it = autoLanes.count(it->first) ? std::next(it) : laneDraw.erase(it);
  for (const auto& [tr, pid] : autoLanes) {
    if (laneOf(tr) <= 0) continue;
    const float top = curveTop(tr), bot = curveBot(tr) + 2;
    if (bot < gMin.y || top > gMax.y) continue;
    // R04-08: the curve lies over the track's own regions inside its row, as the reference draws
    // track automation (299): the regions' bodies are dimmed under it, their name bars stay as they are.
    for (const uint32_t ci : laneRegions[static_cast<size_t>(tr)]) {
      const Clip& c = s.playlist[ci];
      const float x0 = std::max(gMin.x, xOf(c.start)), x1 = std::min(gMax.x, xOf(c.start + ccache.len[ci]));
      if (x1 <= x0) continue;
      dl->AddRectFilled(ImVec2(x0 + 1, top), ImVec2(x1 - 1, bot - 1), theme::withAlpha(P.well, P.dark ? 0.30f : 0.26f), 3.0f, ImDrawFlags_RoundCornersBottom);  // checkpoint 47: lighter, the region keeps its colour (299)
    }
    const AutomationLane* al = nullptr;
    for (const AutomationLane& l : songLanes)
      if (l.targets(pid.owner, pid.index)) al = &l;
    // D6 (checkpoint 48): the curve in a lighter shade of the track's own colour, as the reference's.
    // R09-05: the track's one colour (its sound's when the person gave it none).
    const Rgb ltc = tr < static_cast<int>(smoothTrackColors(a).size()) ? smoothTrackColors(a)[static_cast<size_t>(tr)] : 0;
    const ImU32 curve = ltc != 0 ? theme::mix(smoothRegionColor(theme::rgb(ltc)), IM_COL32(255, 255, 255, 255), 0.35f)
                                 : theme::mix(P.text, P.accent, 0.25f);
    if (!al) {
      const auto d = describeParam(s, pid);
      const auto v = getParam(s, pid);
      const float yv = std::round(laneValueY(tr, d && v ? d->toNormalized(*v) : 0.5));
      for (float x = gMin.x; x < gMax.x; x += 8) dl->AddLine(ImVec2(x, yv), ImVec2(std::min(x + 4, gMax.x), yv), theme::withAlpha(curve, 0.55f), 1.5f);
      continue;
    }
    for (const AutomationLane::Instance& in : al->inst) {
      const float x0 = std::max(gMin.x, xOf(in.start)), x1 = std::min(gMax.x, xOf(in.end));
      if (x1 > x0) dl->AddRectFilled(ImVec2(x0, top + 1), ImVec2(x1, bot - 1), theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.025f));  // D6: no colour tint
    }
    LaneDraw& ld = laneDraw[tr];
    const uint64_t rev = a.doc.revision();
    if (ld.rev != rev || ld.al != al || ld.scrollX != st.scrollX || ld.ppt != st.pxPerTick || ld.x0 != gMin.x || ld.x1 != gMax.x || ld.top != top ||
        ld.bot != bot) {
      ld.rev = rev, ld.al = al, ld.scrollX = st.scrollX, ld.ppt = st.pxPerTick, ld.x0 = gMin.x, ld.x1 = gMax.x, ld.top = top, ld.bot = bot;
      ld.pts.clear();
      for (float x = gMin.x; x <= gMax.x + 2; x += 3) {
        const ImVec2 q(x, laneValueY(tr, al->valueAt(static_cast<double>(tickAt(x)))));
        // (a point on the straight line through the two before it adds nothing: flat and ramped
        // stretches are drawn with their ends only, a fraction of the vertices)
        const size_t n = ld.pts.size();
        if (n >= 2) {
          const ImVec2 p0 = ld.pts[n - 2], p1 = ld.pts[n - 1];
          const float t = (p1.x - p0.x) > 0 ? (q.x - p0.x) / (p1.x - p0.x) : 0.0f;
          if (std::fabs(p0.y + (p1.y - p0.y) * t - q.y) < 0.05f) {
            ld.pts[n - 1] = q;
            continue;
          }
        }
        ld.pts.push_back(q);
      }
    }
    const std::vector<ImVec2>& pts = ld.pts;
    // Checkpoint 47 (showcase): the curve reads over the regions' notes and waveforms: a dark edge
    // under a 2 px line, the points ringed, and each point's value beside it where there is room, as
    // the reference labels its automation points (299).
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::withAlpha(P.ink, P.dark ? 0.55f : 0.30f), 0, 4.0f);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), curve, 0, 2.0f);
    const auto laneDesc = describeParam(s, pid);
    const bool labels = laneDesc && bot - top >= 30;
    float lastLabelX = -1e9f;
    int labelCount = 0;
    const float lfs = theme::kFontCaption;
    if (labels && (ld.labelRev != rev || !(ld.labelPid == pid))) {  // the labels' text and size, once per document revision
      ld.labelRev = rev, ld.labelPid = pid;
      ld.labels.clear(), ld.labelSize.clear();
      for (const AutomationLane::Instance& in : al->inst) {
        if (!in.clip) continue;
        for (const AutoPoint& p : in.clip->points) {
          const double pn = in.clip->min + p.value * (in.clip->max - in.clip->min);
          ld.labels.push_back(laneValueText(s, pid, laneDesc->fromNormalized(std::clamp(pn, 0.0, 1.0)), *laneDesc));
          ld.labelSize.push_back(a.fontRegular->CalcTextSizeA(lfs, 1e9f, 0, ld.labels.back().c_str()));
        }
      }
    }
    size_t pointNo = 0;
    for (const AutomationLane::Instance& in : al->inst) {
      if (!in.clip) continue;
      const AutomationClip& ac = *in.clip;
      for (const AutoPoint& p : ac.points) {
        const size_t pk = pointNo++;
        const Tick at = in.start + p.pos - in.offset;
        if (at < in.start || at > in.end) continue;
        const float x = xOf(at);
        if (x < gMin.x - 4 || x > gMax.x + 4) continue;
        const double pn = ac.min + p.value * (ac.max - ac.min);
        const ImVec2 pc(x, laneValueY(tr, pn));
        dl->AddCircleFilled(pc, 4.0f, theme::withAlpha(P.ink, 0.7f), 12);
        dl->AddCircleFilled(pc, 3.0f, curve, 10);
        if (labels && labelCount < 160 && x >= lastLabelX + 6 && pk < ld.labels.size()) {
          const std::string& txt = ld.labels[pk];
          const ImVec2 ts = ld.labelSize[pk];
          {
            const bool above = pc.y - top >= ts.y + 8;
            const ImVec2 tp(std::round(x + 5), std::round(above ? pc.y - ts.y - 4 : pc.y + 4));
            dl->AddRectFilled(ImVec2(tp.x - 2, tp.y), ImVec2(tp.x + ts.x + 2, tp.y + ts.y), theme::withAlpha(P.ink, P.dark ? 0.55f : 0.35f), 2.0f);
            dl->AddText(a.fontRegular, lfs, tp, P.dark ? P.text : P.paper, txt.c_str());
            lastLabelX = x + 7 + ts.x;
            ++labelCount;
          }
        }
        if (ac.id == gAutoSelClip && gAutoSelPts.count(static_cast<int>(&p - ac.points.data())))  // R05-17: selected
          dl->AddCircle(pc, 5.5f, P.text, 12, 1.5f);
      }
    }
    if (gAutoSelDragging && gAutoSelTrack == tr) {  // R05-17: the Automation Select range
      const float sx0 = std::min(gAutoSelX0, gAutoSelX1), sx1 = std::max(gAutoSelX0, gAutoSelX1);
      dl->AddRectFilled(ImVec2(sx0, top + 1), ImVec2(sx1, bot - 1), theme::withAlpha(curve, 0.18f));
    }
  }
  if (lg) {  // K-112B (checkpoint 46): past the Project End marker the tracks area is shaded
    const float xe = xOf(logicplug::projectEnd(a));
    if (xe < gMax.x) {
      dl->AddRectFilled(ImVec2(std::max(xe, gMin.x), gMin.y), gMax, theme::withAlpha(P.ink, P.dark ? 0.20f : 0.07f));
      if (xe >= gMin.x) dl->AddLine(ImVec2(xe, gMin.y), ImVec2(xe, gMax.y), theme::withAlpha(P.textDim, 0.45f));
    }
  }
  // Playhead.
  if (a.engine.mode() == PlayMode::Song) {
    float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
    if (lg) dl->AddRectFilled(ImVec2(x, gMin.y), ImVec2(x + 1, gMax.y), P.playhead);  // C6: a 1 px light line
    else dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), P.playhead, 1.5f);
  }
  if (cg.kind == ClipGesture::Select && active) {
    dl->AddRectFilled(cg.start, io.MousePos, theme::withAlpha(P.accent, 0.12f));
    dl->AddRect(cg.start, io.MousePos, P.accent, 0, 1.5f, 0);
  }
  if (cg.kind == ClipGesture::Zoom && active) {
    const ImU32 zc = theme::rgb(theme::role::kZoom);
    dl->AddRectFilled(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), theme::withAlpha(zc, 0.10f));
    dl->AddRect(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), zc, 0, 1.2f, 0);
  }
  // PL-13: slice line positions glide between grid lines (J-37 smooth snap).
  auto sliceX = [&](uint64_t key, Tick t) {
    const smooth::Tracker<1>::Vec target{static_cast<double>(t)};
    if (!sliceAnim.contains(key)) sliceAnim.seed(key, target, animNow);
    return std::round(xOfD(sliceAnim.follow(key, target, animNow, animSnap, {animEpsTick})[0]));
  };
  if (cg.kind == ClipGesture::SliceLine && active) {  // the cut line, over the tracks dragged across
    const float x = sliceX(1, cg.startTick);
    const int t0 = std::min(cg.startTrack, trackAt(io.MousePos.y)), t1 = std::max(cg.startTrack, trackAt(io.MousePos.y));
    const float y0 = io.KeyShift ? gMin.y : yOf(t0), y1 = io.KeyShift ? gMax.y : yOf(t1) + hOf(t1);
    const ImU32 sc = theme::rgb(theme::role::kSlice);
    const float dx = std::fabs(io.MousePos.x - cg.start.x), dy = std::fabs(io.MousePos.y - cg.start.y);
    if (dx > 6.0f && dx > dy) {  // horizontal drag: a region cut (both ends), the inside lit
      const float x2 = sliceX(2, std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(io.MousePos.x)), snapG, Snap::Line)));
      dl->AddRectFilled(ImVec2(std::min(x, x2), y0), ImVec2(std::max(x, x2), y1), theme::withAlpha(sc, 0.16f));
      dl->AddLine(ImVec2(x2, y0), ImVec2(x2, y1), sc, 2.0f);
      dl->AddTriangleFilled(ImVec2(x2 - 4, y0), ImVec2(x2 + 4, y0), ImVec2(x2, y0 + 5), sc);
    }
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), sc, 2.0f);
    dl->AddTriangleFilled(ImVec2(x - 4, y0), ImVec2(x + 4, y0), ImVec2(x, y0 + 5), sc);
  } else if (st.tool == Tool::Slice && hovered && io.MousePos.x >= gMin.x && io.MousePos.y >= gMin.y && hoverClip != kNoId) {
    const Tick cut = timeline::snapTick(mm, std::max<Tick>(0, tickAt(io.MousePos.x)), snapG, Snap::Line);
    const float x = sliceX(1, cut);
    const Clip* sc = s.clip(hoverClip);
    float y0 = io.KeyShift ? gMin.y : yOf(sc->track), y1 = io.KeyShift ? gMax.y : yOf(sc->track) + hOf(sc->track);
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), theme::rgb(theme::role::kSlice), 1.5f);
  } else {
    sliceAnim.clear();
  }
  if (lg && st.mqB > st.mqA && st.mqT1 >= st.mqT0 && st.mqT1 < nTracks) {  // R01-18: the marquee, a light veil with a rim
    const float mx0 = std::round(xOf(st.mqA)), mx1 = std::round(xOf(st.mqB)), my0 = yOf(st.mqT0), my1 = yOf(st.mqT1) + hOf(st.mqT1);
    dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx1, my1), theme::withAlpha(P.paper, 0.30f));
    dl->AddRect(ImVec2(mx0 + 0.5f, my0 + 0.5f), ImVec2(mx1 - 0.5f, my1 - 0.5f), theme::withAlpha(P.paper, 0.85f));
  }
  dl->PopClipRect();

  // Ruler: bar numbers, beat ticks, markers, song end marker, playhead cap.
  dl->AddRectFilled(origin, ImVec2(fullX1, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(fullX1, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  const float rulerBot = gMin.y - markLaneH;  // the markers lane (Logic) sits under the bar numbers
  // C7 (checkpoint 48): Smooth's ruler in two tiers, as the reference's: bar numbers with beat ticks
  // above, the cycle and locator band below.
  // cp54 (polish): Smooth's ruler is one tier now (the lower band stood empty once the cycle moved
  // into the number row, R13-10): the numbers on top, the ticks, playhead tab and Project End on its foot.
  const float tierY = lg ? rulerBot : rY + 2;
  dl->PushClipRect(ImVec2(gMin.x, rY), ImVec2(gMax.x, gMin.y), true);
  int hoverMarker = -1;
  float cycX0 = 1e9f, cycX1 = -1e9f;  // R13-10: the lit cycle area across the bar numbers (their ink turns dark)
  {
    // Time selection in the ruler.
    if (liveLook) {
      // K-112c checkpoint 56 (N55-04): the time selection and the loop brace are apart, as the reference's.
      // The time selection: a light band over the bar numbers (it never loops).
      if (st.selEnd > st.selStart) {
        dl->AddRectFilled(ImVec2(std::round(selX0), rY + 1), ImVec2(std::round(selX1), rY + kLiveNumH), theme::withAlpha(P.text, 0.10f));
      }
      // The loop brace (the song's cycle range): an outlined brace in the ruler's lower half, its ends
      // marked by inward triangles; lit while Loop is on, faint while it is off.
      if (s.cycleEnd > s.cycleStart) {
        const float sx0 = std::round(xOf(s.cycleStart)) + 0.5f, sx1 = std::round(xOf(s.cycleEnd)) - 0.5f, by0 = rY + kLiveNumH + 1.5f,
                    by1 = rY + kLiveNumH + kLiveBraceH - 1.5f;
        const bool on = s.cycleOn;
        const ImU32 bc = on ? theme::withAlpha(P.text, 0.75f) : theme::withAlpha(P.text, 0.30f);
        dl->AddRectFilled(ImVec2(sx0, by0), ImVec2(sx1, by1), theme::withAlpha(on ? P.text : P.textFaint, on ? 0.16f : 0.06f));
        dl->AddRect(ImVec2(sx0, by0), ImVec2(sx1, by1), bc);
        const float th = (by1 - by0) * 0.5f - 0.5f, cy = (by0 + by1) * 0.5f;  // round 20: as tall as the brace allows (280, 018)
        if (sx1 - sx0 > 4 * th + 6) {
          dl->AddTriangle(ImVec2(sx0 + 2, cy - th), ImVec2(sx0 + 2 + th * 1.4f, cy), ImVec2(sx0 + 2, cy + th), bc);
          dl->AddTriangle(ImVec2(sx1 - 2, cy - th), ImVec2(sx1 - 2 - th * 1.4f, cy), ImVec2(sx1 - 2, cy + th), bc);
        }
      }
    } else if (st.selEnd > st.selStart && lg) {
      // R02-12: the cycle area as the reference draws it: a yellow band across the bar numbers
      // (its edges a step brighter, as they are handles). R13-10: it fills the bar-number row, as
      // 153's does, the numbers over it in dark ink.
      const float sx0 = std::round(selX0), sx1 = std::round(selX1);
      const ImU32 cyc = theme::rgb(0xD9B13A);
      dl->AddRectFilled(ImVec2(sx0, rY + 1), ImVec2(sx1, tierY), theme::withAlpha(cyc, 0.82f));
      dl->AddLine(ImVec2(sx0 + 1.0f, rY + 1), ImVec2(sx0 + 1.0f, tierY), cyc, 2.0f);
      dl->AddLine(ImVec2(sx1 - 1.0f, rY + 1), ImVec2(sx1 - 1.0f, tierY), cyc, 2.0f);
      cycX0 = sx0, cycX1 = sx1;
    } else if (st.selEnd > st.selStart) {
      const float sx0 = selX0, sx1 = selX1;
      dl->AddRectFilled(ImVec2(sx0, rY + 2), ImVec2(sx1, rulerBot - 1), theme::withAlpha(P.accent, 0.38f), 2);
      dl->AddLine(ImVec2(sx0, rY + 2), ImVec2(sx0, rulerBot - 1), P.accent, 1.5f);
      dl->AddLine(ImVec2(sx1, rY + 2), ImVec2(sx1, rulerBot - 1), P.accent, 1.5f);
    } else if (lg && s.cycleEnd > s.cycleStart) {
      // R04-12: Cycle off: the cycle area stays, grey, as the reference's does (a click turns it back
      // on, a drag moves or resizes it).
      const float sx0 = std::round(xOf(s.cycleStart)), sx1 = std::round(xOf(s.cycleEnd));
      const ImU32 off = P.dark ? theme::rgb(0x6A6A6A) : P.textFaint;  // (light: a theme role, K-177)
      dl->AddRectFilled(ImVec2(sx0, rY + 1), ImVec2(sx1, tierY), theme::withAlpha(off, 0.45f));  // (R13-10: the number row)
      dl->AddLine(ImVec2(sx0 + 1.0f, rY + 1), ImVec2(sx0 + 1.0f, tierY), off, 2.0f);
      dl->AddLine(ImVec2(sx1 - 1.0f, rY + 1), ImVec2(sx1 - 1.0f, tierY), off, 2.0f);
    }
    // K-123: with the record button armed and punching at the selection, the pre-roll before
    // the punch in and the post-roll after the punch out, hatched in the ruler.
    Tick pa = 0, pb = 0;  // checkpoint 56: Clips punches at its loop brace
    if (livelook::recordRange(a, pa, pb) && a.recordArmed && a.recPunch && a.engine.mode() == PlayMode::Song) {
      const float punchX0 = xOf(pa), punchX1 = xOf(pb);
      const double beat = static_cast<double>(s.ppq);
      const double pre = a.settings.getBool("record.preRoll") ? beat * std::max(0.0, a.settings.getNumber("record.preRollBeats")) : 0.0;
      const double post = a.settings.getBool("record.postRoll") ? beat * std::max(0.0, a.settings.getNumber("record.postRollBeats")) : 0.0;
      const float hy0 = rY + 2, hy1 = rulerBot - 1;
      auto hatch = [&](float x0, float x1) {
        if (x1 - x0 < 1) return;
        const ImU32 fill = theme::withAlpha(P.danger, 0.10f), line = theme::withAlpha(P.danger, 0.45f);
        dl->AddRectFilled(ImVec2(x0, hy0), ImVec2(x1, hy1), fill);
        dl->PushClipRect(ImVec2(x0, hy0), ImVec2(x1, hy1), true);
        const float h = hy1 - hy0;
        for (float x = x0 - h; x < x1; x += 6) dl->AddLine(ImVec2(x, hy1), ImVec2(x + h, hy0), line);
        dl->PopClipRect();
      };
      if (pre > 0) hatch(std::max(gMin.x, punchX0 - static_cast<float>(pre) * st.pxPerTick), punchX0);
      if (post > 0) hatch(punchX1, punchX1 + static_cast<float>(post) * st.pxPerTick);
    }
    // Minor ticks on the visible grid; labels on a coarser grid that leaves room for text.
    const Tick labelGrid = timeline::visibleGrid(s, st.pxPerTick, 64.0);
    // K-112b (checkpoint 24): the Tracks layout's numbers in the interface face at caption size, each
    // numbered line drawn down the whole ruler at the number's left, as 216, 125 and 146 draw them.
    ImFont* const numF = lg ? (a.fontBold ? a.fontBold : a.fontRegular) : livelook::numFont(a);  // C7: semibold numbers
    // Gap 9: Smooth's bar numbers at 13 px in the near-white text, as 146's read (they were 11 px).
    const float numS = lg ? theme::kFontCaption + 1 : theme::kFontLabel, numY = lg ? rY + 3 : rY + 4;  // (cp54: where they stood in the upper tier)
    if (mm.constant()) {
      // C44-20: counted from the arrangement's start (Clips: the end of the live area).
      for (Tick t = viewO + snapDown(std::max<Tick>(0, tickAt(gMin.x) - viewO), visGrid); xOf(t) < gMax.x; t += visGrid) {
        float x = std::round(xOf(t));
        bool major = (t - viewO) % labelGrid == 0;
        float len = major ? 10.0f : (t - viewO) % beatT == 0 ? 6.0f : 3.0f;
        if (liveLook && major)  // K-112c: the refs' numbered tick stands at the number's left, from the ruler's top
          vline(xOf(t), rY + 4, rY + kLiveNumH - 1, P.textFaint);
        else if (lg && major)
          vline(xOf(t), rY + 3, tierY, P.lineStrong);
        else if (lg)  // C7: beat ticks 4 px, finer ones 2 px, standing on the tier line
          vline(xOf(t), tierY - (len >= 6.0f ? 4.0f : 2.0f), tierY, P.line);
        else
          vline(xOf(t), rulerBot - len, rulerBot - 1, major ? P.lineStrong : P.line);  // K-276: on the grid line's pixel
        if (major) {
          std::string lbl = timeline::rulerLabel(s, t - viewO, labelGrid);
          const bool onCyc = x + 3 >= cycX0 && x + 4 < cycX1;  // R13-10: dark on the yellow
          // K-112c checkpoint 57 (N56 part 6, 3): Clips leaves out a number that would run past the lanes.
          if (!liveLook || x + 4 + numF->CalcTextSizeA(numS, 1e9f, 0, lbl.c_str()).x <= gMax.x) {
            dl->AddText(numF, numS, ImVec2(x + 4, numY), onCyc ? theme::rgb(0x1E1A10) : (t - viewO) % barT == 0 ? P.text : P.textFaint, lbl.c_str());
          }
        }
      }
    } else {  // K-179: ticks and numbers of the meter map; a short bar's number is dim
      std::vector<Tick> majors;
      timeline::forEachLine(mm, std::max<Tick>(0, tickAt(gMin.x - 40)), tickAt(gMax.x) + 1, labelGrid, [&](Tick t, int) { majors.push_back(t); });
      timeline::forEachLine(mm, std::max<Tick>(0, tickAt(gMin.x)), tickAt(gMax.x) + 1, visGrid, [&](Tick t, int k) {
        const bool major = std::binary_search(majors.begin(), majors.end(), t);
        const float len = major ? 10.0f : k >= timeline::kLineBeat ? 6.0f : 3.0f;
        if (liveLook && major)
          vline(xOf(t), rY + 4, rY + kLiveNumH - 1, P.textFaint);
        else if (lg && major)
          vline(xOf(t), rY + 3, tierY, P.lineStrong);
        else if (lg)  // C7: beat ticks 4 px, finer ones 2 px, standing on the tier line
          vline(xOf(t), tierY - (len >= 6.0f ? 4.0f : 2.0f), tierY, P.line);
        else
          vline(xOf(t), rulerBot - len, rulerBot - 1, major ? P.lineStrong : P.line);
      });
      float lastRight = -1e9f;
      for (Tick t : majors) {
        const float x = std::round(xOf(t));
        if (x + 4 < lastRight) continue;  // bars of a short section crowd: skip numbers that would overlap
        const std::string lbl = timeline::rulerLabel(mm, t, labelGrid);
        const bool bar = timeline::lineStrength(mm, t) == timeline::kLineBar;
        const bool shortBar = bar && mm.shortBar(t);
        if (liveLook && x + 4 + numF->CalcTextSizeA(numS, 1e9f, 0, lbl.c_str()).x > gMax.x) {
          continue;  // checkpoint 57: as above
        }
        const bool onCyc = x + 3 >= cycX0 && x + 4 < cycX1;  // R13-10: dark on the yellow
        dl->AddText(numF, numS, ImVec2(x + 4, numY), onCyc ? theme::rgb(onCyc && shortBar ? 0x4A4230 : 0x1E1A10) : bar && !shortBar ? P.text : P.textFaint, lbl.c_str());
        lastRight = x + 4 + numF->CalcTextSizeA(numS, 1e9f, 0, lbl.c_str()).x + 6;
      }
    }
    const int loopMk = ops::songLoopMarker(s);  // J-85: the active song loop marker (lit glyph)
    // Markers: a flag with the name (time signature markers show the meter).
    for (size_t i = 0; i < s.markers.size(); ++i) {
      const Marker& mk = s.markers[i];
      // K-179: a meter-only marker shows in the meter lane; one with a name or an action keeps its chip.
      if (mk.isMeter() && mk.name.empty() && mk.action == MarkerAction::None) continue;
      const bool dragged = cg.kind == ClipGesture::MarkerDrag && cg.marker == static_cast<int>(i) && cg.moved;
      float x = std::round(xOf(dragged ? cg.markerTo : mk.pos));
      if (x < gMin.x - 200 || x > gMax.x) continue;
      const bool songLoop = mk.action == MarkerAction::SongLoop;
      std::string lbl = mk.name;
      if (lbl.empty()) lbl = "\xE2\x80\xA2";  // bullet for unnamed markers
      static const char* const acts[] = {"", i18n::N_("marker loop"), i18n::N_("skip"), i18n::N_("pause"), i18n::N_("stop"), ""};
      if (mk.action != MarkerAction::None && !songLoop) lbl += std::string("  \xC2\xB7 ") + i18n::tr(acts[static_cast<int>(mk.action)]);
      const bool locTri = liveLook && !songLoop;  // K-112c: Live's locator: an outlined triangle before the name
      const float glyph = songLoop ? 14.0f : locTri ? 9.0f : 0.0f;  // J-85: the loop glyph before the name
      float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str()).x + glyph;
      ImVec2 f0(x, liveLook ? rY + kLiveNumH + kLiveBraceH + 1 : rY + 13), f1(x + tw + 8, gMin.y - 2);
      if (lg) {  // K-112b: a chip in the markers lane, reaching toward the next marker (two bars at most)
        float nextX = 1e9f;
        for (const Marker& o : s.markers)
          if (o.pos > mk.pos && !(o.isMeter() && o.name.empty() && o.action == MarkerAction::None)) nextX = std::min(nextX, std::round(xOf(o.pos)));
        // C8 (checkpoint 48): a band from the marker to the next one (the last to the Project End), as
        // the reference's Marker track draws them.
        if (nextX > 1e8f) nextX = std::round(xOf(std::max(mk.pos + barT, logicplug::projectEnd(a))));
        const float want = nextX - x - 1;
        f0 = ImVec2(x, gMin.y - markLaneH + 2);
        f1 = ImVec2(x + std::max(tw + 12, want), gMin.y - 2);
      }
      bool hov = hovered && io.MousePos.x >= f0.x && io.MousePos.x < f1.x && io.MousePos.y >= (lg ? f0.y - 3 : rY) && io.MousePos.y < gMin.y;
      if (hov) hoverMarker = static_cast<int>(i);
      const bool live = songLoop && static_cast<int>(i) == loopMk;
      const ImU32 ink = live ? P.hot : mk.timeSigNum > 0 ? P.textDim : P.text;
      if (lg) dl->AddRectFilled(f0, f1, dragged ? theme::mix(P.surface700, P.text, 0.14f) : hov ? theme::mix(P.surface700, P.text, 0.08f) : P.surface700, 2.0f);  // K-112b: the quiet hover
      else dl->AddRectFilled(f0, f1, hov || dragged ? P.surface600 : P.surface700, liveLook ? 0.0f : 2.0f, ImDrawFlags_RoundCornersRight);
      // J-139: the marker's tick is the chip's own left edge, only as tall as the chip, in a quiet
      // role colour (the loop marker keeps its lit colour, softer when it is not the active one).
      const ImU32 tick = songLoop ? theme::withAlpha(P.hot, live ? 0.85f : 0.45f)
                                  : theme::withAlpha(mk.timeSigNum > 0 ? P.textFaint : P.textDim, hov || dragged ? 0.8f : 0.55f);
      if (locTri) {
        const float cy = std::round((f0.y + f1.y) * 0.5f) + 0.5f, h = std::floor((f1.y - f0.y) * 0.5f) - 1.0f;
        dl->AddTriangle(ImVec2(x + 1.5f, cy - h), ImVec2(x + 1.5f + h * 1.2f, cy), ImVec2(x + 1.5f, cy + h), mk.timeSigNum > 0 ? P.textFaint : P.text);
      } else if (lg && !songLoop) {
        dl->AddRectFilled(ImVec2(x, f0.y), ImVec2(x + 1, f1.y), theme::withAlpha(P.text, 0.7f));  // C8: a 1 px light start line
      } else {
        dl->AddRectFilled(ImVec2(x, f0.y), ImVec2(x + 2, f1.y), tick);
      }
      if (songLoop) ui::drawIconBox(dl, ui::Icon::Loop, ImVec2(x + 4 + glyph * 0.5f, (f0.y + f1.y) * 0.5f), 12.0f, live ? P.hot : P.textDim);
      ImFont* mf = mk.timeSigNum > 0 ? a.fontMono : a.fontRegular;  // K-112c (round 20): centred on the chip in Live
      dl->AddText(mf, theme::kFontLabel, ImVec2(x + 4 + glyph, liveLook ? theme::textTop(mf, theme::kFontLabel, f0.y, f1.y - f0.y) : lg ? std::round(f0.y + (f1.y - f0.y - theme::kFontLabel) * 0.5f) - 1 : f0.y), live ? P.text : ink,
                  lbl.c_str());
    }
    if (viewO == 0 && s.live.areaEnd > 0) {
      float lx = std::round(xOf(s.live.areaEnd));
      if (lx > gMin.x - 60 && lx < gMax.x + 60) {
        dl->AddLine(ImVec2(lx, rY + 2), ImVec2(lx, gMin.y - 1), P.lineStrong, 2.0f);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(lx - 34, liveLook ? rY + kLiveNumH + kLiveBraceH + 1 : rY + 13), liveMode ? P.text : P.textFaint, "LIVE");
      }
    }
    if (songLen > 0 && !lg) {  // (checkpoint 47: Smooth shows its Project End marker instead)
      float xe = std::round(xOf(songLen));
      dl->AddTriangleFilled(ImVec2(xe - 5, rulerBot - 9), ImVec2(xe + 5, rulerBot - 9), ImVec2(xe, rulerBot - 2), P.textFaint);
    }
    if (a.engine.mode() == PlayMode::Song) {
      float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
      if (lg) {  // C6: the reference's grey rounded tab with a dark notch
        // cp54: a 9 px tab on the ruler's foot (it was 14 px, filling the old lower tier), clear of the numbers.
        dl->AddRectFilled(ImVec2(x - 5, rulerBot - 9), ImVec2(x + 6, rulerBot), theme::mix(P.playhead, P.surface900, 0.22f), 2.5f);
        dl->AddRectFilled(ImVec2(x, rulerBot - 7), ImVec2(x + 1, rulerBot - 2), theme::withAlpha(P.ink, 0.75f));
      } else {
        dl->AddTriangleFilled(ImVec2(x - 5, rulerBot - 9), ImVec2(x + 5, rulerBot - 9), ImVec2(x, rulerBot - 1), P.playhead);
      }
    }
  }
  dl->PopClipRect();
  if (lg) {  // K-112B (checkpoint 46): the Project End marker in the bar ruler
    // Checkpoint 47: dragged along the ruler (to the nearest bar, one undo step on release), as the
    // reference's; the place is saved with the project (Song::projectEnd).
    static Tick endTo = 0;
    const Tick endNow = logicplug::projectEnd(a);
    const Tick endAt = gEndDrag ? endTo : endNow;
    const float xe = std::round(xOf(endAt));
    const float yb = rulerBot - 1;
    const bool nearEnd = hovered && std::fabs(io.MousePos.x - (xe - 4)) <= 6 && io.MousePos.y >= yb - 12 && io.MousePos.y <= yb + 1;
    if (xe >= gMin.x - 1 && xe < gMax.x + 9) {
      dl->PushClipRect(ImVec2(gMin.x - 1, rY), ImVec2(gMax.x, rulerBot), true);
      dl->AddTriangleFilled(ImVec2(xe, yb - 8), ImVec2(xe, yb), ImVec2(xe - 8, yb), gEndDrag || nearEnd ? P.text : P.textDim);
      dl->AddLine(ImVec2(xe, yb - 8), ImVec2(xe, yb), P.text, 1.0f);
      dl->PopClipRect();
      if (nearEnd && !gEndDrag) {
        ImGui::SetTooltip("%s", i18n::tr("Project End"));
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      }
    }
    if (nearEnd && !gEndDrag && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl) gEndDrag = true, endTo = endAt;
    if (gEndDrag) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Tick at = std::max<Tick>(0, tickAt(io.MousePos.x));  // on a bar line, as the reference's (Option: the grid)
        endTo = std::max(mm.nextBar(0), io.KeyAlt ? timeline::snapTick(mm, at, snapG, Snap::Line) : mm.nearestBar(at));
        const MeterMap::Bbt e = mm.toBbt(endTo);
        ImGui::SetTooltip("%s %d %d", i18n::tr("Project End:"), static_cast<int>(e.bar) + 1, e.beat + 1);
      } else {
        gEndDrag = false;
        if (endTo != endNow) a.doc.editSong("move the project end", [to = endTo](Song& x) { x.projectEnd = to; });
      }
    }
  }
  // K-179: the meter lane (its own pointer handling, menu and editor; Delete there removes a change, not clips).
  const bool inMeterLane =
      laneH > 0 && meterui::lane(a, dl, mm, ImVec2(gMin.x, rLaneY), ImVec2(gMax.x, rY), gMin.x - st.scrollX, st.pxPerTick, hovered);

  // Track headers (FL layout): name top left, "..." handle bottom left (drag:
  // reorder, click: track menu), round switch bottom right (mute; right-click:
  // solo), colour strip, resize grip along the bottom edge.
  // K-112c: in Live, Main's row is pinned at the foot of the column; the tracks stop above it.
  // K-112c (round 34): Main is pinned at the foot only while the lanes have room for it and a track; in a
  // squeezed arrangement (an editor opened over it) it covered the one track header left.
  const bool pinMain = liveLook && gMax.y - gMin.y >= livelook::kBusRowH * 3.0f;
  const float headBottom = pinMain ? gMax.y - livelook::kBusRowH - 4 : gMax.y;
  // K-112c (pre-merge): Studio fills its header column inside the clip, as on main (unclipped, a
  // fractional grid edge lost the playhead's left pixel); Live fills under Main's pinned row too.
  if (liveLook) dl->AddRectFilled(ImVec2(headX0, gMin.y), ImVec2(headX1, gMax.y), P.dark ? P.field : P.surface950);
  dl->PushClipRect(ImVec2(headX0, gMin.y), ImVec2(headX1, headBottom), true);
  if (!liveLook) dl->AddRectFilled(ImVec2(headX0, gMin.y), ImVec2(headX1, gMax.y), P.dark ? P.field : P.surface950);
  if (liveLook) dl->AddLine(ImVec2(headX0, gMin.y), ImVec2(headX0, gMax.y), P.dark ? P.ink : P.line);
  else dl->AddLine(ImVec2(gMin.x - 1, gMin.y), ImVec2(gMin.x - 1, gMax.y), P.dark ? P.ink : P.line);
  livelook::HeaderPart liveHeadPart = livelook::HeaderPart::None;  // K-112c: the Live header part under the pointer
  int liveHeadTrack = -1;
  int headResize = -1;  // track whose header's bottom edge is under the pointer
  int headHandle = -1;  // track whose "..." handle is under the pointer
  int headLed = -1;     // track whose mute switch is under the pointer
  bool headLedSolo = false;  // Logic look: the pointer is on the S button (solo), not M
  int headArm = -1, headPan = -1;  // Logic look: the R button / the pan knob under the pointer
  int headMon = -1;                // R10-03: the I button (input monitoring) under the pointer
  int headPower = -1;              // Logic look: the power button (the track's plugins run or sleep)
  int headIcon_ = -1;              // R01-23: the track icon under the pointer (Logic look)
  static int iconMenuTrack = -1;
  // Logic look: the icon a header shows: the track's own, its channel's or mixer track's, else one
  // whose name matches a word of the track's name, else one for its kind.
  auto headIcon = [&](int tr) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    if (lg && fold.isBus(tr)) {
      // R10 gap 10: a bus track's icon says what its automation does on the bus, by the words of
      // its name (a filter, a space, an echo, a level, a duck), else by the bus's first effect
      // (an EQ says little), else the bus strip's own icon, else the send glyph. Never an
      // instrument's: Snare Space Throws showed a red drum, and every bus the same grey glyph.
      static const struct { const char* word; int icon; } kWords[] = {
          {"filter", 73}, {"cutoff", 73}, {"sweep", 74}, {"space", 12}, {"reverb", 12}, {"hall", 12}, {"room", 12},
          {"echo", 13},   {"delay", 13},  {"throw", 13}, {"duck", 15},  {"sidechain", 15}, {"comp", 71}, {"glue", 71},
          {"eq", 72},     {"level", 14},  {"volume", 14}, {"send", 77}};
      auto lower = [](std::string x) {
        for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return x;
      };
      auto mixingIcon = [](int ic) {
        const trackicon::Info* ii = ic > 0 ? trackicon::info(ic) : nullptr;
        return ii && (ii->category == trackicon::Effects || ii->category == trackicon::Mixing);
      };
      if (mixingIcon(t.icon)) return t.icon;  // (one picked for the row from the icon menu)
      const MixerTrack* m = s.mixerTrack(fold.busStrip[static_cast<size_t>(tr)]);
      const std::string nm = lower(playlistTrackName(s, static_cast<size_t>(tr)));
      for (const auto& w : kWords)
        if (nm.find(w.word) != std::string::npos) return w.icon;
      static const struct { const char* type; int icon; } kFx[] = {{"gl.hall", 12}, {"gl.reverb", 12}, {"gl.delay", 13}, {"gl.filter", 73},
                                                                    {"gl.scduck", 15}, {"gl.comp", 71}, {"gl.dist", 11}, {"gl.chorus", 11}};
      if (m)
        for (const EffectSlot& sl : m->slots)
          for (const auto& f : kFx)
            if (sl.plugin.type == f.type) return f.icon;
      if (m && mixingIcon(m->icon)) return m->icon;
      return 77;  // (track-send)
    }
    if (t.icon > 0) return t.icon;
    if (const Channel* c = trackmodes::linkedChannel(s, tr); c && c->icon > 0) return c->icon;
    if (const MixerTrack* m = trackmodes::linkedMixer(s, tr); m && m->icon > 0) return m->icon;
    const TrackMode md = trackmodes::modeOf(s, tr);
    // K-297 round 06: the icon follows what the track plays (its sound, else its strip), never its own
    // name, so renaming a track keeps its icon, as the reference's patch icon does.
    std::string what;
    if (const Channel* c = logicplug::trackSound(a, tr)) {
      if (c->icon > 0) return c->icon;
      what = c->name;
      // Checkpoint 48 (C3): a name that says nothing ("Tercet") is guessed from its instrument's kind.
      if (lg && trackicons::guess(what, nullptr) == 0)
        if (const PluginInfo* pi = findPlugin(c->plugin.type)) what += " " + pi->name + " " + pi->category;
    } else if (const MixerTrack* m = logicplug::trackStrip(a, tr)) {
      if (m->icon > 0) return m->icon;
      what = m->name;
    }
    if (what.empty()) what = playlistTrackName(s, static_cast<size_t>(tr));  // a track with no sound yet
    return trackicons::guess(what, md == TrackMode::Audio ? "Audio" : md == TrackMode::Instrument ? "Synth" : "Melody");
  };
  static int panDrag = -1;         // Logic look: the track whose pan knob is being dragged
  static double panFrom = 0;  // R13-08: the value as it was (a double: a click alone writes nothing)
  static float panY = 0;
  int headVol = -1;                // Logic look: the volume slider under the pointer
  int laneHead = -1;               // K-112b (b4): the automation lane header (its parameter button) under the pointer
  static int laneMenu = -1;        // and the track whose parameter menu is open
  int laneModeHead = -1;           // K-112B: the lane header's automation mode button under the pointer
  int laneRowHead = -1;            // R03-05: the rest of the lane's header row under the pointer
  static int laneModeMenu = -1;    // and the track whose mode menu is open
  static int volDrag = -1;         // and the track whose slider is being dragged
  static double volFrom = 0;  // (normalized)
  static float volX = 0, volW = 1;
  static bool knobMoved = false;  // R13-08: the pointer has moved since the press on a header slider or knob
  int headName = -1;    // track whose name text is under the pointer (double-click renames)
  int headModeTrack = -1, headModeCtl = 0;  // PL-08: audio / instrument track control under the pointer
  static int inputTrack = -1;               // track whose input popup is open
  static int renaming = -1;
  static ObjectId menuClip = kNoId;
  static std::map<int, ImVec4> lgNameAt;  // R01-22: each header's name (left, centre y, right), as drawn
  static int lgBgTrack = -1;  // R01-01: the Tracks layout's background menu: where it opened
  static Tick lgBgTick = 0;
  // GitHub #46: a header pressed (its "..." handle or its body) and, once the pointer moves, dragged
  // with the selected headers to a new place: an insertion line shows where they land, Esc or a
  // right-click cancels, the release moves them (one undo step). A click on the handle opens the
  // track menu; a click on the body selects the track (Ctrl: adds or removes it, Shift: a range).
  struct HeadDrag {
    int track = -1;          // the header pressed (-1: none)
    bool handle = false;     // pressed on the "..." handle
    bool active = false;     // the pointer moved: dragging
    bool cancelled = false;  // Esc / right-click: nothing happens on release
    ImVec2 at;               // where it was pressed
    std::vector<int> tracks; // what moves (the selection, groups included at the drop)
    std::set<int> moving;    // ... with the groups they head (drawn lit)
    int before = -1;         // the insertion point (0 .. nTracks)
  };
  static HeadDrag hd;
  static int selAnchor = -1;  // Shift+click ranges start here
  for (auto it = st.trackSel.begin(); it != st.trackSel.end();) it = *it < nTracks ? std::next(it) : st.trackSel.erase(it);
  const int dragTrack = hd.active && !hd.cancelled ? hd.track : -1;
  float liveBusH = 0;  // K-112c: the return rows' height under the last track (with the gap above them)
  if (liveLook && nTracks > 0 && yOf(nTracks) < gMax.y)  // K-112c: the return tracks under the last track
    liveBusH = livelook::drawBusRows(a, dl, headX0, headX1, yOf(nTracks), headBottom);
  if (gDevRowsAsked) {  // K-112c (dev): the rows as drawn, for the Clips views' track checks
    gDevRowsAsked = false;
    std::printf("plrows n %d\n", nTracks);
    for (int i = 0; i < nTracks; ++i)
      std::printf("plrow %d \"%s\" %.0f %.0f top %.0f h %.0f lane %.0f bus %d grid %.0f %.0f %.0f\n", i, playlistTrackName(s, static_cast<size_t>(i)).c_str(),
                  static_cast<double>(headX0 + 24), static_cast<double>((yOf(i) + yOf(i + 1)) * 0.5f), static_cast<double>(yOf(i)),
                  static_cast<double>(hOf(i)), static_cast<double>(laneOf(i)), lg && fold.isBus(i) ? 1 : 0, static_cast<double>(gMin.x),
                  static_cast<double>(gMin.y), static_cast<double>(gMax.y));  // (R09-01: the row's top, height, lane, bus and the grid)
  }
  auto headLit = [&](int tr) { return hd.active && !hd.cancelled && hd.moving.count(tr) > 0; };
  // K-12: the track "+" (View > Track + button): on the hovered header, left of the mute switch on its line.
  int headPlus = -1;            // track whose "+" is under the pointer
  ImVec2 headPlusAnchor(0, 0);  // its menu's corner (under the button)
  // K-123: the take lanes' headers: "Take N", lit while some part of it is in the comp.
  int takeHeadTrack = -1, takeHeadTake = -1;  // the lane header under the pointer
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    const int nl = takeLaneN[static_cast<size_t>(tr)];
    if (nl <= 0) continue;
    const std::vector<char> inComp = takes::takesInComp(s, tr);
    for (int k = 0; k < nl; ++k) {
      ImU32 tc = P.hot;  // the take's own colour (its clips' channel)
      for (const Clip& c : s.playlist)
        if (c.track == tr && c.takeGroup != kNoId && c.take == k)
          if (const Channel* ch = s.channel(c.source)) {
            tc = theme::rgb(ch->color);
            break;
          }
      const float ly0 = takeLaneTop(tr) + static_cast<float>(k) * laneTH, ly1 = ly0 + laneTH;
      if (ly1 < gMin.y || ly0 > gMax.y) continue;
      const bool lit = static_cast<size_t>(k) < inComp.size() && inComp[static_cast<size_t>(k)];
      const bool hov = hovered && io.MousePos.x >= headX0 && io.MousePos.x < headX1 && io.MousePos.y >= ly0 && io.MousePos.y < ly1 && io.MousePos.y >= gMin.y;
      if (hov) takeHeadTrack = tr, takeHeadTake = k;
      if (lg) {
        // cp54 (polish): Smooth's take headers on the track rows' own grid: the number column carries
        // on (its strip lit while the take is in the comp), the face reaches the rows' right edge, and
        // "Take N" stands on the track names' column (it began 10 px into the number column).
        bool anyGroup = false;
        for (int i = 0; i < nTracks && !anyGroup; ++i) {
          anyGroup = s.tracks[static_cast<size_t>(i)].groupSize > 0 || groupDepth[static_cast<size_t>(i)] > 0;
        }
        const float indent = 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
        const ImVec2 r0(headX0, ly0), r1(gMin.x - 1, ly1);
        dl->AddRectFilled(r0, r1, hov ? (P.dark ? P.surface800 : P.surface700) : theme::mix(P.dark ? P.surface900 : P.surface800, P.ink, 0.2f));
        dl->AddLine(ImVec2(r0.x, r1.y - 0.5f), ImVec2(r1.x, r1.y - 0.5f), P.dark ? P.ink : P.line);
        dl->AddRectFilled(r0, ImVec2(r0.x + kHeadNumW, r1.y - 1), P.surface700);
        dl->AddRectFilled(ImVec2(r0.x + kHeadNumW - 3, r0.y), ImVec2(r0.x + kHeadNumW, r1.y - 1), lit ? tc : P.lineStrong);
        const float tx = r0.x + 12 + kHeadNumW + indent + (anyGroup ? 14.0f : 0.0f) + 32.0f;
        const float fy = std::round((ly0 + ly1 - theme::kFontCaption) * 0.5f) - 1;
        theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(tx, fy), r1.x - tx - 26, lit ? P.text : P.textDim,
                            ("Take " + std::to_string(k + 1)).c_str());
        if (lit) {  // on the pan knobs' centre line
          dl->AddCircleFilled(ImVec2(r1.x - 10 - 7.5f, (ly0 + ly1) * 0.5f), 3.0f, tc, 12);
        }
        continue;
      }
      const ImVec2 h0(headX0 + 2 + 10, ly0 + 1), h1(headX1 - 3, ly1 - 1);
      dl->AddRectFilled(h0, h1, hov ? P.surface700 : theme::mix(P.surface800, P.ink, 0.25f), theme::kRadiusSm);
      dl->AddRectFilled(h0, ImVec2(h0.x + 3, h1.y), lit ? tc : P.lineStrong, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      const std::string label = recordrows::laneLabel(k, h1.x - h0.x);  // K-123: "Take 2", or "2" beside the controls
      const float fy = std::round((ly0 + ly1 - theme::kFontCaption) * 0.5f) - 1;
      const float labelX1 = recordrows::drawLaneHeader(a, dl, tr, k, h0, h1, hov);  // K-123: rating dots, solo
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(h0.x + 10, fy), labelX1 - h0.x - 10, lit ? P.text : P.textDim, label.c_str());
      if (lit) dl->AddCircleFilled(ImVec2(h1.x - 10, (ly0 + ly1) * 0.5f), 3.0f, tc, 12);
    }
  }
  const bool plusOn = trackplusui::enabled(a);
  const int plusMenu = trackplusui::menuTrack();
  static std::vector<float> plusFade;  // per track hover fade of the "+"
  if (plusFade.size() != static_cast<size_t>(nTracks)) plusFade.assign(static_cast<size_t>(nTracks), 0.0f);
  // R05-04: the curves the lane readouts follow while playing (R06-12: stopped too)
  const std::vector<AutomationLane>& headLanes = lg && !autoLanes.empty() ? arrangeLanes(a) : songLanes;
  trackplusui::DevGeom& tpg = trackplusui::devGeom();
  tpg.on = plusOn;
  tpg.track = -1;
  if (foldHdrH > 0 && yOf(foldHead) > gMin.y && yOf(foldHead) - foldHdrH < gMax.y) {  // R08-10: the open folder's own row
    const float y1 = yOf(foldHead), y0 = y1 - foldHdrH;
    dl->PushClipRect(ImVec2(headX0, gMin.y), ImVec2(gMax.x, gMax.y), true);
    const bool hov = hovered && io.MousePos.x >= headX0 && io.MousePos.x < headX1 && io.MousePos.y >= y0 && io.MousePos.y < y1;
    const ImVec2 r0(headX0, y0), r1(gMin.x - 1, y1);
    dl->AddRectFilled(r0, r1, hov ? (P.dark ? P.surface800 : P.surface700) : (P.dark ? P.surface900 : P.surface800));
    dl->AddRectFilled(ImVec2(gMin.x, y0), ImVec2(gMax.x, y1), theme::withAlpha(P.dark ? P.surface900 : P.surface800, 0.9f));
    dl->AddLine(ImVec2(r0.x, y1 - 0.5f), ImVec2(gMax.x, y1 - 0.5f), P.dark ? P.ink : P.line);
    dl->AddRectFilled(r0, ImVec2(r0.x + kHeadNumW, y1 - 1), P.surface700);
    const float cy = std::round((y0 + y1) * 0.5f);
    float nx = r0.x + 12 + kHeadNumW;
    ui::drawIconFit(dl, ui::Icon::Down, ImVec2(nx, cy - 2), ImVec2(nx + 8, cy + 3), P.textDim);
    nx += 14;
    ui::drawIconFit(dl, ui::Icon::Folder, ImVec2(nx + 4, cy - 7), ImVec2(nx + 20, cy + 7), P.textDim);
    nx += 30;
    dl->AddText(ImVec2(nx, std::round(cy - ImGui::GetFontSize() * 0.5f)), P.text, i18n::tr("Automation"));
    char cnt[24];
    std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(fold.folder.size()));
    dl->AddText(ImVec2(r1.x - 12 - ImGui::CalcTextSize(cnt).x, std::round(cy - ImGui::GetFontSize() * 0.5f)), P.textDim, cnt);
    if (hov) ImGui::SetTooltip("%s", i18n::tr("Click to close the folder"));
    dl->PopClipRect();
  }
  bool lgAnyGroup = false;  // R09-06: the group arrow's column only when the song has groups
  for (int i = 0; lg && i < nTracks && !lgAnyGroup; ++i) lgAnyGroup = s.tracks[static_cast<size_t>(i)].groupSize > 0 || groupDepth[static_cast<size_t>(i)] > 0;
  // Checkpoint 54 (N53-03, N53-04): Clips numbers its tracks by what it shows (lanes and gaps take no number).
  const std::vector<int> liveNums = liveLook ? trackmodes::trackNumbers(s, nTracks) : std::vector<int>{};
  const std::vector<char> liveLanes = liveLook ? trackmodes::automationLanes(s, nTracks) : std::vector<char>{};
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    float y = yOf(tr), th = hOf(tr) - laneOf(tr);  // R04-08: the automation line of the header follows
    if (th < 1) continue;  // inside a folded group
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    const float indent = 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    bool hovHead = hovered && io.MousePos.x >= headX0 && io.MousePos.x < headX1 && io.MousePos.y >= y && io.MousePos.y < y + th &&
                   io.MousePos.y >= gMin.y;
    ImVec2 h0(headX0 + 2, y + 1), h1(headX1 - 3, y + th - 1);
    if (liveLook) {  // K-112c: the Live header (name chip, activator, solo, arm, volume)
      const bool edge = hovered && io.MousePos.x >= headX0 && io.MousePos.x < headX1 && std::fabs(io.MousePos.y - (y + th)) < 3.0f && io.MousePos.y >= gMin.y;
      livelook::HeaderDraw lh;
      lh.track = tr;
      lh.h0 = ImVec2(headX0 + 1, y + 1), lh.h1 = ImVec2(headX1, y + th);
      lh.hovered = hovHead && !edge;
      lh.selected = st.trackSel.count(tr) > 0;
      lh.lit = headLit(tr);
      lh.mouse = io.MousePos;
      lh.number = tr < static_cast<int>(liveNums.size()) ? liveNums[static_cast<size_t>(tr)] : tr + 1;
      lh.lane = tr < static_cast<int>(liveLanes.size()) && liveLanes[static_cast<size_t>(tr)];
      const livelook::HeaderPart part = livelook::drawHeader(a, dl, lh);
      if (part != livelook::HeaderPart::None) liveHeadPart = part, liveHeadTrack = tr;
      if (part == livelook::HeaderPart::Name) headName = tr;
      if (edge) {
        headResize = tr;
        const float gx = (lh.h0.x + lh.h1.x) * 0.5f;
        for (int d = -2; d <= 2; ++d) dl->AddCircleFilled(ImVec2(gx + static_cast<float>(d) * 4.0f, lh.h1.y - 1.5f), 1.0f, P.hot, 6);
      }
      continue;
    }
    const bool coloured = t.color != PlaylistTrack{}.color;
    const ImU32 tc = theme::content(t.color);
    const bool selHead = st.trackSel.count(tr) > 0;
    if (lg && foldShut && tr == foldHead) {  // K-112B (checkpoint 46): the closed Automation folder
      const ImVec2 r0(headX0, y), r1(gMin.x - 1, y + th);
      dl->AddRectFilled(r0, r1, hovHead ? (P.dark ? P.surface800 : P.surface700) : (P.dark ? P.surface900 : P.surface800));
      dl->AddLine(ImVec2(r0.x, r1.y - 0.5f), ImVec2(r1.x, r1.y - 0.5f), P.dark ? P.ink : P.line);
      dl->AddRectFilled(r0, ImVec2(r0.x + kHeadNumW, r1.y - 1), P.surface700);
      const float cy = std::round(y + th * 0.5f);
      float nx = r0.x + 12 + kHeadNumW + indent;
      ui::drawIconFit(dl, ui::Icon::Right, ImVec2(nx + 1, cy - 4), ImVec2(nx + 7, cy + 4), P.textDim);
      nx += 14;
      ui::drawIconFit(dl, ui::Icon::Folder, ImVec2(nx + 4, cy - 7), ImVec2(nx + 20, cy + 7), P.textDim);
      nx += 30;
      const char* label = i18n::tr("Automation");
      dl->AddText(ImVec2(nx, std::round(cy - ImGui::GetFontSize() * 0.5f)), P.text, label);
      char cnt[48];
      std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(fold.folder.size()));
      const float cw = ImGui::CalcTextSize(cnt).x;
      dl->AddText(ImVec2(r1.x - 12 - cw, std::round(cy - ImGui::GetFontSize() * 0.5f)), P.textDim, cnt);
      if (hovHead) ImGui::SetTooltip("%s", i18n::tr("Automation with no single track: click to open the folder"));
      continue;
    }
    if (lg) {  // Logic look: colour strip, number, name, then M S R and a pan knob on one row
      const ImVec2 r0(headX0, y), r1(gMin.x - 1, y + th);
      // R09-05: the number cell in the colour the track's regions take (one colour per track).
      const Rgb lgRgb = tr < static_cast<int>(smoothTrackColors(a).size()) ? smoothTrackColors(a)[static_cast<size_t>(tr)] : 0;
      const bool lgColoured = lgRgb != 0;
      const ImU32 lgCol = lgColoured ? smoothRegionColor(theme::rgb(lgRgb)) : P.surface700;
      bool nearEdgeL = hovered && io.MousePos.x >= headX0 && io.MousePos.x < gMin.x && std::fabs(io.MousePos.y - (y + hOf(tr))) < 3.0f && io.MousePos.y >= gMin.y;
      // K-112b (checkpoint 20): a step darker (the references' headers sit just above the dark area).
      const ImU32 restL = P.dark ? P.surface900 : P.surface800, liftL = P.dark ? P.surface800 : P.surface700;
      // Checkpoint 48 (C2): neutral rows with no colour tint (the number cell carries the colour); the
      // selected row a clearly lighter grey, as the reference's.
      const ImU32 faceL = selHead ? (P.dark ? P.surface700 : P.surface600) : hovHead || headLit(tr) ? liftL : restL;
      dl->AddRectFilled(r0, r1, faceL);
      dl->AddLine(ImVec2(r0.x, r1.y - 0.5f), ImVec2(r1.x, r1.y - 0.5f), P.dark ? P.ink : P.line);
      {  // ref 254: the number column (the track's number, white on its colour)
        dl->AddRectFilled(r0, ImVec2(r0.x + kHeadNumW, r1.y - 1), lgColoured ? (selHead ? theme::mix(lgCol, P.text, 0.12f) : lgCol) : P.surface700);
        char num[16];
        std::snprintf(num, sizeof num, "%d", rowNum[static_cast<size_t>(tr)]);
        const float fsz = theme::kFontLabel;
        const ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(fsz, FLT_MAX, 0.0f, num);
        dl->AddText(ImGui::GetFont(), fsz, ImVec2(std::round(r0.x + (kHeadNumW - tsz.x) * 0.5f), std::round(y + (th - tsz.y) * 0.5f)),
                    IM_COL32(255, 255, 255, 235), num);
      }
      if (headLit(tr)) dl->AddRect(r0, ImVec2(r1.x, r1.y - 1), theme::withAlpha(P.hot, 0.7f), 0, 0, 1.5f);
      const float cyMid = std::round(y + th * 0.5f);
      float cy = cyMid;  // the controls' line (R11-07: the lower line of a two-line header)
      // K-112b (second pass): the name in the body size and regular weight, as 125, 216 and 195 set it.
      const float fs = theme::kFontLabel;
      float fn = theme::kFontBody;
      // The row: number, (group chevron), M S R, the name; the volume slider and the pan knob at the right end.
      // Gap 6: a pan knob the size of the reference's (about 18 px), light on the grey row.
      float kr = std::clamp(th * 0.26f, 7.0f, 10.0f);
      ImVec2 kc(std::round(r1.x - 10 - kr), cy);
      float bh = std::min(18.0f, th - 8.0f);
      const float bw = r1.x - r0.x < 340 ? 18.0f : 20.0f, gap = 3.0f;  // (R10-06: narrower buttons in a narrow header)
      float bx1 = kc.x - kr - 8;
      const MixerTrack* mt = trackmodes::linkedMixer(s, tr);
      const Channel* lc = trackmodes::linkedChannel(s, tr);
      // R09-06: the sound the track plays when it has no channel of its own (a MIDI region track):
      // its power button sleeps that sound, so every track with a sound has one.
      const Channel* playCh = !lc && !mt ? logicplug::trackSound(a, tr) : nullptr;
      std::optional<ParamId> panId;
      // R04-09: Configure track header: the parts the person hid are left out (the room stays theirs).
      const bool showIcon = trackHeaderShows(a, kHeadIcon), showPower = trackHeaderShows(a, kHeadPower), showMS = trackHeaderShows(a, kHeadMuteSolo),
                 showRec = trackHeaderShows(a, kHeadRecord), showVol = trackHeaderShows(a, kHeadVolume), showPan = trackHeaderShows(a, kHeadPan);
      // Checkpoint 47: a track with no strip of its own shows the strip its sound plays into (the
      // inspector's strip), so every header has its volume and pan, as the reference's do.
      const MixerTrack* ownStrip = !lc && !mt ? logicplug::trackStrip(a, tr) : nullptr;
      if (lc) panId = channelParam(lc->id, ChannelParam::Pan);
      else if (mt) panId = mixerParam(mt->id, MixerParam::Pan);
      else if (ownStrip) panId = mixerParam(ownStrip->id, MixerParam::Pan);
      if (!showPan) panId.reset();
      bool anyHot = false;
      // A long volume slider between the buttons and the pan knob, when the header has the room.
      std::optional<ParamId> volId;
      if (lc) volId = channelParam(lc->id, ChannelParam::Volume);
      else if (mt) volId = mixerParam(mt->id, MixerParam::Volume);
      else if (ownStrip) volId = mixerParam(ownStrip->id, MixerParam::Volume);
      if (!showVol) volId.reset();
      // R09-06: the left run (number, group arrow, icon, power, M S, R) keeps the same columns on
      // every row, and the name keeps at least about 12 characters: the volume slider gives way
      // first at narrow widths (1440 with the Library and the inspector open), as the reference's.
      const float leftRun = 12 + kHeadNumW + indent + (lgAnyGroup ? 14.0f : 0.0f) + (showIcon && th >= 20 ? 32.0f : 0.0f) +
                            (showPower ? bh + gap : 0.0f) + (showMS ? bw * 2 - 1 + gap : 0.0f) + (showRec ? bw * 2 - 1 + gap : 0.0f) + 5;
      // R10-06: the slider shrinks to a compact one (down to 30 px) before it goes, and the name keeps
      // about 10 characters, as the reference's narrow header keeps both (216 cuts its names). With
      // room, the name grows first (to about 16 characters) and the slider takes the rest.
      const float room = (bx1 - (r0.x + leftRun)) - 8;
      // R11-07: where one line would leave the name under about 100 px (1440 with R and I), the name
      // takes a line of its own above the controls, as 194's narrow header does; the volume slider
      // then has the whole width between I and the pan knob. (The choice is the header's, not the
      // row's, so every row reads the same; rows lower than 30 px keep one line.)
      const bool twoLine = th >= 30 && room + indent - 40.0f < 100.0f;
      const float nameLineX = r0.x + 12 + kHeadNumW + indent + (lgAnyGroup ? 14.0f : 0.0f) + (showIcon && th >= 20 ? 32.0f : 0.0f);
      if (twoLine) {
        bh = std::clamp(std::floor(th - 21.0f), 12.0f, 15.0f);
        cy = std::round(y + th - 3.0f - bh * 0.5f);
        kr = std::min(kr, 7.5f);
        kc = ImVec2(std::round(r1.x - 10 - kr), cy);
        bx1 = kc.x - kr - 8;
        fn = theme::kFontCaption + 1.0f;
      }
      const float nameW = twoLine ? 0.0f : std::clamp(room - 40.0f, 70.0f, 110.0f);
      float slW = std::clamp(room - nameW, 0.0f, twoLine ? 150.0f : 96.0f);
      if (slW < 30) slW = 0;
      if (th >= 20 && volId && slW >= 30) {
        const float sx1 = bx1, sx0 = sx1 - slW;
        // K-112b (checkpoint 24): a rounded dark pill about a third of the row tall with the knob inside
        // it, as 194, 216 and 125 draw the header's volume (it was a 6 px line under a larger knob).
        // Checkpoint 34: the knob a little larger than the pill (216, 125: it stands over the track's edges).
        // Checkpoint 52 (polish): a thin track under a round knob, as 146 and 101 draw it (the 13 px pill
        // read as a switch at 1920); the knob stands well over the track.
        const float ph = twoLine ? 5.0f : std::clamp(std::round(th * 0.16f), 4.0f, 6.0f);
        const float kR = twoLine ? std::max(5.0f, std::floor(bh * 0.5f) - 1.0f) : std::clamp(std::round(th * 0.19f), 5.5f, 7.0f);
        const ImVec2 s0(sx0, std::round(cy - ph * 0.5f)), s1(sx1, std::round(cy - ph * 0.5f) + ph);
        const auto d = describeParam(s, *volId);
        const auto v = getParam(s, *volId);
        const float norm = d && v ? static_cast<float>(d->toNormalized(*v)) : 0.8f;
        // (R13-08: in the two-line header the reach stays off the name line above)
        const bool hot = hovHead && io.MousePos.x >= sx0 - 4 && io.MousePos.x < sx1 + 4 && std::fabs(io.MousePos.y - cy) <= (twoLine ? kR + 1.0f : 8.0f);
        const float tx = std::round(sx0 + 2 + kR + (slW - 4 - kR * 2) * std::clamp(norm, 0.0f, 1.0f));
        const float kcY = (s0.y + s1.y) * 0.5f;
        dl->AddRectFilled(s0, s1, theme::mix(P.surface800, P.ink, 0.55f), ph * 0.5f);
        // A quiet fill up to the knob, and (gap 6) the track's level as a green bar along it, as the
        // reference's slider shows its meter while the song plays.
        // R11-07: a fill that reads (it was 16 % grey, invisible on the dark pill), as 194's light track.
        dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(tx, s1.y - 1), theme::withAlpha(P.textDim, hot || volDrag == tr ? 0.62f : 0.48f), ph * 0.5f - 1);
        if (const MixerTrack* lvlStrip = mt ? mt : logicplug::trackStrip(a, tr); lvlStrip && a.engine.playing()) {
          static std::map<ObjectId, float> shown;  // the drawn level per strip (falls back gently)
          float& lv = shown[lvlStrip->id];
          const float pk = mixmeters::get(a, lvlStrip->id).peak();
          const float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -90.0f;
          const float target = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);  // -60 .. +6 dB across the slider
          lv = target > lv ? target : std::max(target, lv - io.DeltaTime * 1.4f);
          if (lv > 0.01f) {
            const float lx = s0.x + 2 + (slW - 4) * lv;
            dl->AddRectFilled(ImVec2(s0.x + 1, s0.y + 1), ImVec2(lx, s1.y - 1), db > -0.1f ? theme::rgb(0xE0B83A) : theme::rgb(0x3FBF5A), 1.0f);
          }
        }
        // (checkpoint 52: a light knob, as 146's, on the now thin track)
        dl->AddCircleFilled(ImVec2(tx, kcY), kR, hot || volDrag == tr ? theme::mix(P.text, P.surface800, 0.08f) : theme::mix(P.text, P.surface800, 0.24f), 20);
        dl->AddCircle(ImVec2(tx, kcY), kR, theme::withAlpha(P.ink, 0.6f), 20, 1.0f);
        if (hot) headVol = tr, anyHot = true;
        if (volDrag == tr) volW = slW - 10;
        bx1 = sx0 - 8;
      }
      float nx = r0.x + 12 + kHeadNumW + indent;  // left to right: the group arrow, the icon, power, M S R, the name
      if (t.groupSize > 0) {
        if (t.folded) ui::drawIconFit(dl, ui::Icon::Right, ImVec2(nx + 1, cyMid - 4), ImVec2(nx + 7, cyMid + 4), P.textDim);
        else ui::drawIconFit(dl, ui::Icon::Down, ImVec2(nx, cyMid - 2), ImVec2(nx + 8, cyMid + 3), P.textDim);
      }
      if (lgAnyGroup) nx += 14;  // R09-06: the arrow's column only when the song has groups
      if (th >= 20 && showIcon) {  // the track's icon, in a tint of its colour
        const float box = std::min(24.0f, std::floor(th - 12));
        // An icon not drawn before is rasterised on its first draw: a few new ones a frame, so the
        // first frame of a big project (or a layout switch to it) stays quick; the rest follow.
        static std::set<std::pair<int, int>> drawnIcons;
        static int newFrame = -1, newCount = 0;
        if (newFrame != ImGui::GetFrameCount()) newFrame = ImGui::GetFrameCount(), newCount = 0;
        const int ic = headIcon(tr);
        const std::pair<int, int> iconKey(ic, static_cast<int>(box));
        const bool known = ic > 0 && drawnIcons.count(iconKey) > 0;
        if (ic > 0 && box >= 12 && (known || newCount < 3)) {
          if (!known) drawnIcons.insert(iconKey), ++newCount;
          if (lg && box >= 16) {
            // Checkpoint 48 (C3): the reference's filled, two-tone pictograms: the glyph light on a
            // tile in its category's own colour (never the track's), so kinds read at a glance.
            static const uint32_t kCat[trackicon::CategoryCount] = {0xC0503C, 0xC77A2E, 0x3D7FCC, 0x8A5AD0, 0xB0485E, 0x3A9E8C, 0xCC5C9A, 0x5C8A3A,
                                                                   0x6A7A8A, 0x3A8FB0, 0x7A7F86, 0x7A7F86, 0x7A7F86, 0x7A7F86, 0x7A7F86, 0x7A7F86};
            const trackicon::Info* ii = trackicon::info(ic);
            const uint32_t cc = ii && static_cast<int>(ii->category) < static_cast<int>(trackicon::CategoryCount) ? kCat[ii->category] : 0x7A7F86;
            const ImU32 tile = theme::rgb(cc);
            const float tb = std::min(24.0f, box + 2), tx0 = std::round(nx + 13 - tb * 0.5f), ty0 = std::round(cyMid - tb * 0.5f);
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx0 + tb, ty0 + tb), theme::mix(tile, IM_COL32(0, 0, 0, 255), 0.18f), 5.0f);
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx0 + tb, ty0 + tb * 0.5f), theme::withAlpha(IM_COL32(255, 255, 255, 255), 0.10f), 5.0f,
                              ImDrawFlags_RoundCornersTop);
            trackicons::draw(dl, ic, ImVec2(std::round(nx + 13), cyMid), tb - 6, IM_COL32(255, 255, 255, 240));
          } else {
            trackicons::draw(dl, ic, ImVec2(std::round(nx + 13), cyMid), box, coloured ? theme::mix(tc, P.text, 0.45f) : P.textDim);
          }
        }
        // R01-23: a click on the icon opens the icon picker (the reference's icon menu, 042).
        if (hovHead && std::fabs(io.MousePos.x - (nx + 13)) <= box * 0.5f + 2 && std::fabs(io.MousePos.y - cyMid) <= box * 0.5f + 2) headIcon_ = tr, anyHot = true;
        nx += 32;
      }
      // K-112b (close-range pass): M and S share one pill split by a hairline, as 216, 125 and 151 draw
      // them; R stays a button of its own. `corners` picks the pill's rounded ends; `last` ends a run.
      auto button = [&](const char* label, bool on, ImU32 lit, bool& hot, ImDrawFlags corners = ImDrawFlags_RoundCornersAll, bool last = true,
                        ImU32 offText = 0) {
        const ImVec2 b0(nx, std::round(cy - bh * 0.5f)), b1(nx + bw, std::round(cy - bh * 0.5f) + bh);
        hot = hovHead && io.MousePos.x >= b0.x && io.MousePos.x < b1.x && io.MousePos.y >= b0.y && io.MousePos.y < b1.y;
        anyHot = anyHot || hot;
        // K-112b (checkpoint 22): one quiet hover for the Tracks layout's own controls, the rest face 8 % toward the text (14 % open or held)
        const ImU32 rest = theme::mix(P.surface800, P.ink, 0.25f);
        dl->AddRectFilled(b0, b1, on ? lit : hot ? theme::mix(rest, P.text, 0.08f) : rest, 3.0f, corners);
        dl->AddRect(b0, b1, on ? lit : P.line, 3.0f, 1.0f, corners);
        const ImVec2 ts = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, label);
        dl->AddText(a.fontBold, fs, ImVec2(std::round((b0.x + b1.x - ts.x) * 0.5f), std::round((b0.y + b1.y - ts.y) * 0.5f)),
                    on ? P.ink : offText != 0 ? offText : P.textDim, label);
        nx = last ? b1.x + gap : b1.x - 1;  // inside a pill the next half shares this one's edge (one hairline)
      };
      if (th >= 20) {
        bool panHot = hovHead && std::fabs(io.MousePos.x - kc.x) <= kr + 3 && std::fabs(io.MousePos.y - kc.y) <= kr + 3;
        if (panId) {
          const auto d = describeParam(s, *panId);
          const auto v = getParam(s, *panId);
          const float norm = d && v && d->max > d->min ? static_cast<float>((*v - d->min) / (d->max - d->min)) : 0.5f;
          {  // Gap 6: the reference's header pan (R10-06: a dark cap), with a blue arc
             // from the centre to the value (it was the dark FL-style knob, about 14 px).
            const bool held = panDrag == tr;
            constexpr float kPi = 3.14159265f;
            const float a0 = kPi * 0.75f, a1 = kPi * 2.25f, av = a0 + (a1 - a0) * std::clamp(norm, 0.0f, 1.0f), amid = kPi * 1.5f;
            dl->PathArcTo(kc, kr + 1.5f, a0, a1, 24);
            dl->PathStroke(theme::mix(P.surface800, P.ink, 0.45f), 0, 2.0f);
            if (std::fabs(norm - 0.5f) > 0.004f) {
              dl->PathArcTo(kc, kr + 1.5f, std::min(av, amid), std::max(av, amid), 16);
              dl->PathStroke(theme::rgb(0x3A8EE6), 0, 2.0f);
            }
            // R10-06: the reference's knob: a dark cap with a soft top light and a light rim, and a
            // light value tick from the rim inwards (it was a flat light-grey disc).
            const ImU32 cap = theme::mix(P.surface800, P.ink, panHot || held ? 0.05f : 0.35f);
            dl->AddCircleFilled(kc, kr - 0.5f, theme::withAlpha(P.ink, 0.55f), 24);  // a hairline shadow under the cap
            dl->AddCircleFilled(ImVec2(kc.x, kc.y - 0.5f), kr - 1.0f, cap, 24);
            dl->AddCircleFilled(ImVec2(kc.x, kc.y - kr * 0.30f), kr * 0.55f, theme::withAlpha(P.text, 0.06f), 20);
            dl->AddCircle(ImVec2(kc.x, kc.y - 0.5f), kr - 1.0f, theme::mix(P.textDim, cap, panHot || held ? 0.25f : 0.45f), 24, 1.2f);
            dl->AddLine(ImVec2(kc.x + std::cos(av) * (kr * 0.30f), kc.y - 0.5f + std::sin(av) * (kr * 0.30f)),
                        ImVec2(kc.x + std::cos(av) * (kr - 2.2f), kc.y - 0.5f + std::sin(av) * (kr - 2.2f)), theme::mix(P.text, cap, 0.08f), 1.8f);
          }
          if (panHot) headPan = tr, anyHot = true;
        }
        bool rHot = false, sHot = false, mHot = false;
        // R05-03: S is the track's channel strip's solo (its own state, never the other tracks' mutes).
        const MixerTrack* strip = logicplug::trackStrip(a, tr);
        const bool soloed = strip && strip->solo;
        // Power: the track's plugins run (off: they sleep, as Hibernate does; nothing is lost).
        const Channel* powCh = lc ? lc : playCh;
        if ((powCh || mt) && showPower) {
          const bool on = powCh ? !powCh->hibernate : !mt->hibernate;
          const float pr = std::round(bh * 0.5f);
          const ImVec2 pc(nx + pr, cy);
          const bool pHot = hovHead && std::hypot(io.MousePos.x - pc.x, io.MousePos.y - pc.y) <= pr + 1;
          dl->AddCircleFilled(pc, pr, on ? theme::rgb(0x2F7EDB) : pHot ? P.surface600 : theme::mix(P.surface800, P.ink, 0.25f), 24);
          if (!on) dl->AddCircle(pc, pr, P.line, 24);
          ui::drawIcon(dl, ui::Icon::Power, pc, pr * 1.25f, on ? pal().paper : pHot ? P.text : P.textDim);
          if (pHot) headPower = tr, anyHot = true;
          nx += pr * 2 + gap;
        } else if (showPower) {
          nx += std::round(bh * 0.5f) * 2 + gap;  // R09-06: the column stays, so the names line up
        }
        if (showMS) {  // C4: one pill, mute lit blue and solo lit yellow, as the reference's
          button("M", t.muted, theme::rgb(0x3A9BE0), mHot, ImDrawFlags_RoundCornersLeft, false);
          button("S", soloed, theme::rgb(0xE2BE3C), sHot, ImDrawFlags_RoundCornersRight);
        }
        // R10-03: R and I in one pill on every audio and instrument track, as 194 draws them (R in red
        // type, lit red when armed; I lit amber when the input is monitored). An audio track's are its
        // strip's arm and monitoring; an instrument track's arm its MIDI and monitor its sound.
        const bool audioRec = mt && t.mode == TrackMode::Audio;
        const Channel* recCh = !audioRec && logicplug::midiRecordable(a, tr) ? logicplug::trackSound(a, tr) : nullptr;
        if (showRec && (audioRec || recCh)) {
          const bool armed = audioRec ? mt->armed : a.playlist.midiArm.count(recCh->id) > 0;
          const bool mon = audioRec ? mt->monitor == static_cast<uint8_t>(InputMonitor::On) : a.playlist.midiMonitor.count(recCh->id) > 0;
          bool iHot = false;
          button("R", armed, P.danger, rHot, ImDrawFlags_RoundCornersLeft, false, theme::mix(P.danger, P.textDim, 0.25f));
          button("I", mon, theme::rgb(0xE8A23A), iHot, ImDrawFlags_RoundCornersRight);
          if (rHot) headArm = tr;
          if (iHot) headMon = tr;
        } else if (showRec) {
          nx += bw * 2 - 1 + gap;  // R09-06: (the same column on tracks that record nothing)
        }
        if (sHot || mHot) headLed = tr, headLedSolo = sHot;
        nx += 5;
      }
      if (nearEdgeL && !anyHot) headResize = tr;
      // R11-07: a two-line header sets the name on the upper line, from the power button's column to
      // the row's end, so even "Risers + Impacts" reads whole at 1440.
      if (twoLine) nx = nameLineX;
      const float nameCy = twoLine ? std::round(y + 3.0f + fn * 0.5f) : cy;
      const float nameY = std::round(nameCy - fn * 0.5f) - (twoLine ? 0.0f : 1.0f);
      const std::string shownName = playlistTrackName(s, static_cast<size_t>(tr));
      const float nameRight = twoLine ? r1.x - 10 : bx1 - 6;
      theme::textEllipsis(dl, a.fontRegular, fn, ImVec2(nx, nameY), nameRight - nx, t.muted ? P.textFaint : P.text, shownName.c_str());
      const float textW = a.fontRegular->CalcTextSizeA(fn, FLT_MAX, 0.0f, shownName.c_str()).x;
      lgNameAt[tr] = ImVec4(nx, nameCy, std::max(nx + 60, nameRight), 0);  // R01-22: the rename field goes here
      if (hovHead && !anyHot && io.MousePos.x >= nx - 3 && io.MousePos.x < std::min(nameRight, nx + std::max(textW, 28.0f)) + 3 &&
          std::fabs(io.MousePos.y - nameCy) < fn * 0.5f + 4)
        headName = tr;
      if (t.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(r0.x + 24 + indent, r1.y - 9), 9, P.textDim);
      if (const auto lit = autoLanes.find(tr); lit != autoLanes.end() && laneOf(tr) > 0) {  // K-112b (b4): the lane's header line
        const float ly0 = r1.y, ly1 = ly0 + laneOf(tr), lcy = std::round(ly0 + 14);
        // K-112b: one header of two lines (refs 008, 170): the row's face carries on, its colour
        // strip too, and the line between them goes.
        dl->AddRectFilled(ImVec2(r0.x, ly0 - 1), ImVec2(r1.x, ly1), faceL);
        dl->AddRectFilled(ImVec2(r0.x, ly0 - 1), ImVec2(r0.x + 4, ly1 - 1), lgColoured ? lgCol : P.lineStrong);
        dl->AddLine(ImVec2(r0.x, ly1 - 0.5f), ImVec2(r1.x, ly1 - 0.5f), P.dark ? P.ink : P.line);
        const std::string pname = paramDisplayName(s, lit->second);
        const auto d = describeParam(s, lit->second);
        const auto v = getParam(s, lit->second);
        // R05-04: while the song plays and the track reads its automation, the readout follows the curve.
        std::optional<double> shownV = v;
        // R06-12: stopped, a Read track's readout shows the curve at the playhead too. R06-11: while a
        // take writes the parameter, it shows the value written.
        if (d && (a.engine.playing() || t.autoMode == 1) && a.engine.mode() == PlayMode::Song && t.autoMode != 0 &&
            !automationTakeActive(lit->second))
          for (const AutomationLane& l : headLanes)
            if (l.targets(lit->second.owner, lit->second.index)) shownV = d->fromNormalized(l.valueAt(a.engine.position()));
        const std::string val = d && shownV ? laneValueText(s, lit->second, *shownV, *d) : std::string();
        // K-112B: the track's automation mode first, as the reference's header shows it (Read,
        // Touch, Latch, Write, Off: its colour), then the parameter.
        const int amode = std::clamp<int>(t.autoMode, 0, 4);
        const char* mname = i18n::tr(kAutoModeNames[amode]);
        const float mw = std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, mname).x) + 22, mx0 = r0.x + 34 + indent;
        const bool mhot = hovered && io.MousePos.x >= mx0 && io.MousePos.x < mx0 + mw && io.MousePos.y >= lcy - 9 && io.MousePos.y < lcy + 9 && io.MousePos.y >= gMin.y;
        if (mhot) laneModeHead = tr;
        const ImU32 mcol = theme::rgb(kAutoModeColors[amode]);
        // R05-15: Read is white on green only where the track's parameter has a curve; with no
        // data it is green text on grey, as the reference marks a lane with nothing to read.
        bool hasData = false;
        for (const AutomationLane& l : songLanes) hasData = hasData || l.targets(lit->second.owner, lit->second.index);
        const bool readEmpty = amode == 1 && !hasData;
        const ImU32 base = mhot || laneModeMenu == tr ? P.surface600 : P.surface800;
        dl->AddRectFilled(ImVec2(mx0, lcy - 9), ImVec2(mx0 + mw, lcy + 9),
                          readEmpty ? theme::mix(base, P.text, 0.10f) : theme::mix(base, mcol, amode == 1 ? 0.80f : 0.55f), 3.0f);
        dl->AddText(a.fontRegular, fs, ImVec2(mx0 + 6, std::round(lcy - fs * 0.5f) - 1),
                    readEmpty ? mcol : amode == 1 ? P.paper : P.text, mname);
        ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(mx0 + mw - 8, lcy), 8, P.textDim);
        // cp54 (polish): the readout box one width on every lane (room for "-00.0 dB", the value right
        // aligned in it), so the parameter chips and their chevrons end on one column; it followed each
        // value's own width (at 1024 a "-16.0 dB" lane's chip ended 12 px before its neighbours').
        const float textW = std::ceil(a.fontMono->CalcTextSizeA(fs, 1e9f, 0, val.c_str()).x);
        const float bx0 = mx0 + mw + 4, vw = std::max(textW, std::ceil(a.fontMono->CalcTextSizeA(fs, 1e9f, 0, "-00.0 dB").x));
        const float bx1 = std::max(bx0 + 40, r1.x - 22 - vw);
        const bool hot = hovered && io.MousePos.x >= bx0 && io.MousePos.x < bx1 && io.MousePos.y >= lcy - 9 && io.MousePos.y < lcy + 9 && io.MousePos.y >= gMin.y;
        if (hot) laneHead = tr;
        // R03-05: a right-click anywhere on the lane's header row opens its parameter menu too.
        if (hovered && !mhot && !hot && io.MousePos.x >= r0.x && io.MousePos.x < r1.x && io.MousePos.y >= ly0 && io.MousePos.y < ly1 && io.MousePos.y >= gMin.y)
          laneRowHead = tr;
        dl->AddRectFilled(ImVec2(bx0, lcy - 9), ImVec2(bx1, lcy + 9), hot || laneMenu == tr ? P.surface600 : theme::mix(P.surface800, P.ink, 0.25f), 3.0f);
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(bx0 + 6, std::round(lcy - fs * 0.5f) - 1), bx1 - bx0 - 22, P.text, pname.c_str());
        ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(bx1 - 8, lcy), 8, P.textDim);
        dl->AddRectFilled(ImVec2(r1.x - 16 - vw, lcy - 9), ImVec2(r1.x - 6, lcy + 9), theme::mix(P.field, P.ink, 0.4f), 3.0f);
        dl->AddText(a.fontMono, fs, ImVec2(r1.x - 11 - textW, std::round(lcy - fs * 0.5f) - 1), P.text, val.c_str());
      }
      continue;
    }
    ImU32 face = theme::mix(hovHead || headLit(tr) || selHead ? P.surface700 : P.surface800, tc, t.muted ? 0.04f : coloured ? 0.16f : 0.06f);
    dl->AddRectFilled(h0, h1, theme::mix(face, P.ink, 0.35f), theme::kRadiusSm);
    dl->AddRectFilled(h0, ImVec2(h1.x, h1.y - 1), face, theme::kRadiusSm);
    dl->AddLine(ImVec2(h0.x + 6, h0.y + 0.5f), ImVec2(h1.x - 2, h0.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.10f : 0.5f));
    if (coloured) dl->AddRectFilled(h0, ImVec2(h0.x + 4, h1.y), tc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    if (headLit(tr)) dl->AddRect(h0, h1, theme::withAlpha(P.hot, 0.7f), theme::kRadiusSm, 1.5f, 0);
    else if (selHead) dl->AddRect(h0, h1, theme::withAlpha(P.hot, 0.45f), theme::kRadiusSm, 1.0f, 0);
    bool nearEdge = hovered && io.MousePos.x >= headX0 && io.MousePos.x < headX1 && std::fabs(io.MousePos.y - (y + th)) < 3.0f &&
                    io.MousePos.y >= gMin.y;
    const bool roomy = th >= 30;  // two lines: name above, handle + switch below
    float nameY = roomy ? y + 4.0f : y + (th - theme::kFontCaption) * 0.5f - 1;
    float nameX = h0.x + 8 + indent;
    if (t.groupSize > 0) {  // group: fold arrow
      ImVec2 c(nameX + 4, nameY + 7);
      if (t.folded)
        ui::drawIconFit(dl, ui::Icon::Right, ImVec2(c.x - 3, c.y - 4), ImVec2(c.x + 3, c.y + 4), P.textDim);
      else
        ui::drawIconFit(dl, ui::Icon::Down, ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y + 3), P.textDim);
      nameX += 12;
    }
    const float lineY = roomy ? h1.y - 8.0f : nameY + 7.0f;  // handle / switch line
    // Mute switch: lit in the track colour (the accent purple for uncoloured tracks).
    const ImVec2 ledC(h1.x - 10, lineY);
    const bool ledHot = hovHead && std::fabs(io.MousePos.x - ledC.x) < 8 && std::fabs(io.MousePos.y - ledC.y) < 8;
    if (ledHot) headLed = tr;
    const ImU32 ledCol = coloured ? theme::mix(tc, P.paper, 0.25f) : theme::rgb(theme::role::kStamp);
    dl->AddCircleFilled(ledC, 5.0f, theme::mix(P.ink, face, 0.3f));
    dl->AddCircle(ledC, 5.0f, ledHot ? P.hot : theme::withAlpha(ledCol, t.muted ? 0.45f : 0.9f), 16, 1.4f);
    if (!t.muted) dl->AddCircleFilled(ledC, 2.6f, ledCol);
    // "..." handle.
    const ImVec2 hc0(h0.x + 6 + indent, lineY - 5);
    // K-12: the "+" slot sits right of the mode controls (before the lock / live marks and the switch);
    // it is kept free on every header that offers it, so the controls never move when it fades in.
    const float slotX1 = h1.x - 20 - (t.locked ? 16.0f : 0.0f) - (liveMode ? 14.0f : 0.0f);
    const bool plusOffered = plusOn && trackplusui::offeredOn(s, tr) && slotX1 - trackplusui::kSlotW > hc0.x + 22;
    const ImVec2 plusC(std::round(slotX1 - trackplusui::kHalf - 2), lineY);
    float plusA = 0;
    bool plusHot = false;
    if (plusOffered) {
      float& f = plusFade[static_cast<size_t>(tr)];
      const float target = hovHead || plusMenu == tr || dragTrack == tr ? 1.0f : 0.0f;
      f += (target - f) * std::min(1.0f, io.DeltaTime * 16.0f);
      if (std::fabs(target - f) < 0.02f) f = target;
      plusA = f;
      plusHot = hovHead && std::fabs(io.MousePos.x - plusC.x) <= trackplusui::kHalf + 2 && std::fabs(io.MousePos.y - plusC.y) <= trackplusui::kHalf + 2;
      if (plusHot) {
        headPlus = tr;
        headPlusAnchor = ImVec2(plusC.x - trackplusui::kHalf, plusC.y + trackplusui::kHalf + 4);
        nearEdge = false;
      }
    }
    if (nearEdge) headResize = tr;
    const bool handleHot = hovHead && !ledHot && !plusHot && io.MousePos.x >= hc0.x - 2 && io.MousePos.x < hc0.x + 20 && std::fabs(io.MousePos.y - lineY) < 7;
    if (handleHot) headHandle = tr;
    if (roomy || th >= 22)
      ui::drawIconFit(dl, ui::Icon::More, ImVec2(hc0.x + 2.8f, lineY - 0.2f), ImVec2(hc0.x + 14.2f, lineY + 2.2f), handleHot || dragTrack == tr ? P.hot : P.textFaint);
    float nameRight = roomy ? h1.x - 6 : h1.x - 22;
    if (!roomy && plusA > 0.01f) nameRight = std::min(nameRight, plusC.x - trackplusui::kHalf - 4);  // one line: the name gives way
    float nameX0 = 0, iconX1 = 0;
    if (roomy || th >= 18) {
      float nx = roomy ? nameX : nameX + 18;
      // J-211: the track icon before the name, in a box that fits the track's
      // height (left out when the header is too narrow to keep some name).
      if (const float box = trackicons::boxFor(roomy ? 16.0f : th - 2.0f, 14.0f); t.icon > 0 && box > 0 && nameRight - nx >= box + 44) {
        trackicons::draw(dl, t.icon, ImVec2(std::round(nx + box * 0.5f), std::round(nameY + (theme::kFontCaption + 1) * 0.5f + 0.5f)), box,
                         t.muted ? P.textFaint : P.textDim);
        nx += box + 4;
      }
      nx += trackui::drawNameGlyph(a, dl, tr, ImVec2(nx, nameY));  // PL-08: audio / instrument track glyph
      if (freeze::Unit fu; freezeui::trackUnit(s, tr, fu) && freezeui::hasMarker(a, fu)) {  // K-87: its channel / track frozen or asleep
        freezeui::drawMarker(a, dl, fu, ImVec2(nx + 5, nameY + theme::kFontCaption * 0.5f + 1), 10);
        nx += 13;
      }
      iconX1 = nx;
      nameX0 = nx;
      const std::string shownName = playlistTrackName(s, static_cast<size_t>(tr));
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(nx, nameY), nameRight - nx, t.muted ? P.textFaint : P.text,
                          shownName.c_str());
      // The name's own box (the text as drawn, at least a short word wide) is where a double-click renames.
      const float textW = a.fontRegular->CalcTextSizeA(theme::kFontCaption + 1, FLT_MAX, 0.0f, shownName.c_str()).x;
      const float nameHitX1 = std::min(nameRight, nx + std::max(textW, 28.0f)) + 3;
      if (hovHead && !ledHot && !plusHot && !handleHot && io.MousePos.x >= nx - 3 && io.MousePos.x < nameHitX1 &&
          io.MousePos.y >= nameY - 3 && io.MousePos.y < nameY + theme::kFontCaption + 4)
        headName = tr;
    }
    {  // PL-08: input / monitoring / arm (audio tracks), channel (instrument tracks) between the handle and the switch
      const float cx1 = slotX1 - (plusOffered ? trackplusui::kSlotW : 0.0f);
      if (const int ctl = trackui::drawHeader(a, dl, tr, hc0.x + 18, cx1, lineY, hovHead && !ledHot && !handleHot && !plusHot, roomy)) {
        headModeTrack = tr;
        headModeCtl = ctl;
      }
      if (plusOffered) {
        trackplusui::drawButton(a, dl, tr, plusC, plusA, plusHot);
        if (plusA > 0.01f && (hovHead || plusMenu == tr)) {  // K-12 dev geometry: the "+" and its neighbours
          tpg.track = tr;
          tpg.x0 = plusC.x - trackplusui::kHalf, tpg.x1 = plusC.x + trackplusui::kHalf;
          tpg.y0 = plusC.y - trackplusui::kHalf, tpg.y1 = plusC.y + trackplusui::kHalf;
          const bool named = (roomy || th >= 18) && nameX0 > 0;
          tpg.nameX0 = named ? nameX0 : 0, tpg.nameX1 = named ? nameRight : 0;
          tpg.nameY0 = named ? nameY : 0, tpg.nameY1 = named ? nameY + theme::kFontCaption + 1 : 0;
          tpg.handleX0 = hc0.x - 2, tpg.handleX1 = hc0.x + 20;
          tpg.ledX0 = ledC.x - 6, tpg.ledX1 = ledC.x + 6;
          tpg.ctlX0 = hc0.x + 18, tpg.ctlX1 = cx1;
          tpg.iconX1 = iconX1;
        }
      }
    }
    if (liveMode && a.engine.liveActive(tr) != kNoId)  // live: a clip plays on this track
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(h1.x - 30, lineY - 4), ImVec2(h1.x - 24, lineY + 4), P.accent);
    if (t.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(h1.x - (liveMode ? 42 : 26), lineY), 11, P.textDim);
    if (nearEdge) {
      const float gx = (h0.x + h1.x) * 0.5f;
      for (int d = -2; d <= 2; ++d) dl->AddCircleFilled(ImVec2(gx + static_cast<float>(d) * 4.0f, h1.y - 1.5f), 1.0f, P.hot, 6);
    }
  }
  dl->PopClipRect();
  if (pinMain) {  // K-112c: Main, pinned at the foot
    dl->PushClipRect(ImVec2(headX0, headBottom), ImVec2(headX1, gMax.y), true);
    livelook::drawMainRow(a, dl, headX0, headX1, gMax.y - livelook::kBusRowH);
    dl->PopClipRect();
  }

  // Vertical scroller (left of the headers) and horizontal scroller (above the ruler).
  const float gridH = gMax.y - gMin.y;
  // C44-20: the scroller spans the arrangement from its start (in Clips, after the live area).
  const float oPx = static_cast<float>(viewO) * st.pxPerTick;
  const float contentW = std::max(static_cast<float>(std::max<Tick>(0, songLen - viewO) + 8 * std::max<Tick>(1, s.barTicks())) * st.pxPerTick, st.scrollX - oPx + gridW);
  // K-112b: the Logic layout's scrollers lie over the grid's right and bottom edges, thin until hovered.
  const float ovl = 9.0f;
  const ImVec2 vb0 = lg ? ImVec2(gMax.x - ovl, gMin.y) : ImVec2(vbarX0, gMin.y), vb1 = lg ? ImVec2(gMax.x - 1, gMax.y - ovl) : ImVec2(vbarX0 + vbarW - 2, gMax.y);
  const ImVec2 hb0 = lg ? ImVec2(gMin.x, gMax.y - ovl) : ImVec2(gMin.x, origin.y + 1), hb1 = lg ? ImVec2(gMax.x - ovl, gMax.y - 1) : ImVec2(gMax.x, rLaneY - 1);
  const float vThumbH = std::max(kThumbMinV, (vb1.y - vb0.y) * std::min(1.0f, gridH / std::max(1.0f, totalH)));
  const float vThumbY = vb0.y + (totalH > gridH ? (vb1.y - vb0.y - vThumbH) * std::clamp(st.scrollY / (totalH - gridH), 0.0f, 1.0f) : 0.0f);  // J-67: overscroll keeps the thumb in its rail
  const float hThumbW = std::max(kThumbMinH, (hb1.x - hb0.x) * std::min(1.0f, gridW / std::max(1.0f, contentW)));
  const float hThumbX = hb0.x + (contentW > gridW ? (hb1.x - hb0.x - hThumbW) * std::clamp((st.scrollX - oPx) / (contentW - gridW), 0.0f, 1.0f) : 0.0f);
  // K-287: the Logic layout's rails lie over the clips: there a press on a clip is the clip's, and
  // only the thumb itself takes it (a press on the bare rail over a clip paged the view).
  const bool railFree = !lg || hoverClip == kNoId;
  const bool overV = hovered && io.MousePos.x >= vb0.x && io.MousePos.x < vb1.x + 2 && io.MousePos.y >= vb0.y && io.MousePos.y < vb1.y &&
                     (railFree || (io.MousePos.y >= vThumbY && io.MousePos.y < vThumbY + vThumbH && totalH > gridH));
  const bool overH = hovered && io.MousePos.x >= hb0.x && io.MousePos.x < hb1.x && io.MousePos.y >= (lg ? hb0.y : origin.y) && io.MousePos.y < (lg ? hb1.y + 1 : rLaneY) &&
                     (railFree || (io.MousePos.x >= hThumbX && io.MousePos.x < hThumbX + hThumbW && contentW > gridW));
  // K-24: the thumbs' ends are zoom handles (a third of a short thumb at most).
  const float vCap = std::min(7.0f, vThumbH / 3.0f), hCap = std::min(8.0f, hThumbW / 3.0f);
  int overEnd = 0;
  if (overV && io.MousePos.y >= vThumbY && io.MousePos.y < vThumbY + vCap) overEnd = 3;
  else if (overV && io.MousePos.y >= vThumbY + vThumbH - vCap && io.MousePos.y < vThumbY + vThumbH) overEnd = 4;
  else if (overH && io.MousePos.x >= hThumbX && io.MousePos.x < hThumbX + hCap) overEnd = 5;
  else if (overH && io.MousePos.x >= hThumbX + hThumbW - hCap && io.MousePos.x < hThumbX + hThumbW) overEnd = 6;
  const int litEnd = barDrag >= 3 ? barDrag : overEnd;
  const bool vBusy = barDrag == 1 || barDrag == 3 || barDrag == 4, hBusy = barDrag == 2 || barDrag == 5 || barDrag == 6;
  dl->AddRectFilled(ImVec2(vbarX0, gMin.y), ImVec2(vbarX0 + vbarW, gMax.y), P.dark ? P.well : P.surface950);
  if (vbarW > 0) dl->AddRectFilled(ImVec2(vb0.x + 2, vThumbY), ImVec2(vb1.x, vThumbY + vThumbH), vBusy || overV ? P.surface600 : P.surface700, 3);
  dl->AddRectFilled(ImVec2(gMin.x, origin.y), ImVec2(gMax.x, rLaneY), P.dark ? P.well : P.surface950);
  if (!liveLook && hbarH > 0) dl->AddRectFilled(ImVec2(hThumbX, hb0.y + 2), ImVec2(hThumbX + hThumbW, hb1.y - 1), hBusy || overH ? P.surface600 : P.surface700, 3);
  if (lg) {  // overlay thumbs: a slim rounded bar, wider and brighter under the pointer or while dragged
    // D10 (checkpoint 49): as the reference's overlay scrollers, they show while the view scrolls (or
    // the pointer is on them, or near the edge) and fade out a second after.
    static float lastSX = -1, lastSY = -1;
    static double movedAt = -10;
    const double now = ImGui::GetTime();
    if (st.scrollX != lastSX || st.scrollY != lastSY) lastSX = st.scrollX, lastSY = st.scrollY, movedAt = now;
    const bool nearEdge = hovered && ((io.MousePos.x >= gMax.x - 24 && io.MousePos.x < gMax.x && io.MousePos.y >= gMin.y && io.MousePos.y < gMax.y) ||
                                      (io.MousePos.y >= gMax.y - 24 && io.MousePos.y < gMax.y && io.MousePos.x >= gMin.x && io.MousePos.x < gMax.x));
    const double idle = now - movedAt;
    const float show = overV || overH || vBusy || hBusy || nearEdge ? 1.0f : idle < 1.0 ? 1.0f : static_cast<float>(std::max(0.0, 1.0 - (idle - 1.0) / 0.35));
    const ImU32 calm = theme::withAlpha(P.textDim, 0.75f * show), lit = theme::withAlpha(P.text, 0.85f);  // readable over clips of any colour
    if (show > 0.01f && (totalH > gridH || vBusy)) {
      const float inset = overV || vBusy ? 1.0f : 3.0f;
      dl->AddRectFilled(ImVec2(vb0.x + inset, vThumbY + 1), ImVec2(vb1.x - 1, vThumbY + vThumbH - 1), overV || vBusy ? lit : calm, 4);
    }
    if (show > 0.01f && (contentW > gridW || hBusy)) {
      const float inset = overH || hBusy ? 1.0f : 3.0f;
      dl->AddRectFilled(ImVec2(hThumbX + 1, hb0.y + inset), ImVec2(hThumbX + hThumbW - 1, hb1.y - 1), overH || hBusy ? lit : calm, 4);
    }
  }
  {  // K-24: end handles: a grip line at each end while the scroller is hovered, the end under the pointer (or dragged) lit
    const ImU32 grip = P.dark ? P.textFaint : P.textDim;
    if (overV || vBusy) {
      const float x0 = vb0.x + 3, x1 = vb1.x - 1;
      if (litEnd == 3) dl->AddRectFilled(ImVec2(vb0.x + 2, vThumbY), ImVec2(vb1.x, vThumbY + vCap), P.hot, 3, ImDrawFlags_RoundCornersTop);
      else dl->AddLine(ImVec2(x0, vThumbY + vCap - 0.5f), ImVec2(x1, vThumbY + vCap - 0.5f), grip, 1.0f);
      if (litEnd == 4) dl->AddRectFilled(ImVec2(vb0.x + 2, vThumbY + vThumbH - vCap), ImVec2(vb1.x, vThumbY + vThumbH), P.hot, 3, ImDrawFlags_RoundCornersBottom);
      else dl->AddLine(ImVec2(x0, vThumbY + vThumbH - vCap + 0.5f), ImVec2(x1, vThumbY + vThumbH - vCap + 0.5f), grip, 1.0f);
    }
    if (overH || hBusy) {
      const float y0 = hb0.y + 4, y1 = hb1.y - 3;
      if (litEnd == 5) dl->AddRectFilled(ImVec2(hThumbX, hb0.y + 2), ImVec2(hThumbX + hCap, hb1.y - 1), P.hot, 3, ImDrawFlags_RoundCornersLeft);
      else dl->AddLine(ImVec2(hThumbX + hCap - 0.5f, y0), ImVec2(hThumbX + hCap - 0.5f, y1), grip, 1.0f);
      if (litEnd == 6) dl->AddRectFilled(ImVec2(hThumbX + hThumbW - hCap, hb0.y + 2), ImVec2(hThumbX + hThumbW, hb1.y - 1), P.hot, 3, ImDrawFlags_RoundCornersRight);
      else dl->AddLine(ImVec2(hThumbX + hThumbW - hCap + 0.5f, y0), ImVec2(hThumbX + hThumbW - hCap + 0.5f, y1), grip, 1.0f);
    }
    if (litEnd >= 3) ImGui::SetMouseCursor(litEnd <= 4 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    // The hint at once (status bar), a tooltip after the hint delay.
    static double endSince = -1;
    static int endWas = 0;
    if (overEnd != endWas) endWas = overEnd, endSince = ImGui::GetTime();
    if (overEnd >= 3 && barDrag == 0) {
      const bool v = overEnd <= 4;
      a.setHint(v ? i18n::tr("Drag the end to zoom the track height (the other end stays put) \xC2\xB7 Alt+wheel: zoom at the pointer")
                  : i18n::tr("Drag the end to zoom the time (the other end stays put) \xC2\xB7 Ctrl+wheel: zoom at the pointer"));
      if (ImGui::GetTime() - endSince >= static_cast<double>(ImGui::GetStyle().HoverDelayNormal) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(v ? i18n::tr("Drag to zoom the track height") : i18n::tr("Drag to zoom the time"));
        ImGui::EndTooltip();
      }
    }
  }
  devGeom.vThumb0 = vThumbY, devGeom.vThumb1 = vThumbY + vThumbH, devGeom.hThumb0 = hThumbX, devGeom.hThumb1 = hThumbX + hThumbW;
  devGeom.vBarX = (vb0.x + 2 + vb1.x) * 0.5f, devGeom.hBarY = (hb0.y + hb1.y) * 0.5f;
  devGeom.scrollY = st.scrollY, devGeom.trackHeight = st.trackHeight, devGeom.gridX0 = gMin.x, devGeom.gridY0 = gMin.y, devGeom.gridY1 = gMax.y;
  if (liveLook) {  // K-112c: the overview strip: every clip in its track's thin row, the view framed
    const float k = (hb1.x - hb0.x) / std::max(1.0f, contentW);
    const float y0 = hb0.y + 1, y1 = hb1.y - 1;
    const float rowH = std::max(1.0f, (y1 - y0) / static_cast<float>(std::max(1, nTracks)));
    for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.count; ++ci) {
      const Clip& c = s.playlist[ci];
      if (c.start < viewO) continue;  // C44-20: the clip grid's clips are not the arrangement's
      const float x0 = hb0.x + static_cast<float>(c.start - viewO) * st.pxPerTick * k, x1 = hb0.x + static_cast<float>(c.start - viewO + ccache.len[ci]) * st.pxPerTick * k;
      if (x0 > hb1.x || !ccache.valid[ci] || c.track < 0) continue;
      const float ry = std::min(y1 - 1, y0 + rowH * static_cast<float>(c.track));
      dl->AddRectFilled(ImVec2(x0, ry), ImVec2(std::max(x0 + 1, std::min(x1, hb1.x)), std::min(y1, ry + std::max(1.0f, rowH - (rowH > 2 ? 1.0f : 0.0f)))),
                        theme::withAlpha(theme::rgb(ccache.color[ci]), 0.9f));
    }
    dl->AddRect(ImVec2(hThumbX, hb0.y), ImVec2(hThumbX + hThumbW, hb1.y), hBusy || overH ? P.text : theme::withAlpha(P.text, 0.75f), 2.0f, 1.5f, 0);
  } else if (hbarH > 0) {  // song overview in the horizontal scroller: clip extents as thin marks
    const float k = (hb1.x - hb0.x) / std::max(1.0f, contentW);
    for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.count; ++ci) {  // cached lengths / colours (J-25)
      const Clip& c = s.playlist[ci];
      const float x0 = hb0.x + static_cast<float>(c.start) * st.pxPerTick * k, x1 = hb0.x + static_cast<float>(c.start + ccache.len[ci]) * st.pxPerTick * k;
      if (x0 > hb1.x || !ccache.valid[ci]) continue;
      dl->AddRectFilled(ImVec2(x0, hb1.y - 3), ImVec2(std::max(x0 + 1, std::min(x1, hb1.x)), hb1.y - 1), theme::withAlpha(theme::rgb(ccache.color[ci]), 0.8f));
    }
  }

  // Header corner (J-38 layout): the view buttons above the track headers as
  // equal flat cells like the toolbar's: audio editing (stretch, fades, zero
  // crossings) | tracks (add, clear the selection) | view (picker panel, header
  // width), groups split by a thin rule. One row in a wide header column, two
  // rows (view group right-aligned) in a narrow one. A lit (orange) glyph on a
  // raised cell = on; hovering shows what a cell does.
  dl->AddRectFilled(ImVec2(liveLook ? headX0 : origin.x, origin.y), ImVec2(liveLook ? fullX1 : gMin.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(liveLook ? headX0 : origin.x, gMin.y - 1), ImVec2(liveLook ? fullX1 : gMin.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  if (markLaneH > 0) {  // Logic: the markers lane's name under the corner's buttons
    const float laneY = gMin.y - markLaneH;
    dl->AddLine(ImVec2(origin.x, laneY), ImVec2(gMax.x, laneY), P.line);
    // C8: "Marker", in the label size, and a + at the header's right that adds one at the playhead.
    dl->AddText(a.fontRegular, theme::kFontLabel + 1, ImVec2(origin.x + 10, std::round(laneY + (markLaneH - theme::kFontLabel - 1) * 0.5f) - 1), P.textDim,
                i18n::tr("Marker"));
    const ImVec2 pc(std::round(gMin.x - 14), std::round(laneY + markLaneH * 0.5f));
    const bool phov = hovered && std::fabs(io.MousePos.x - pc.x) <= 8 && std::fabs(io.MousePos.y - pc.y) <= 8;
    dl->AddCircle(pc, 6.5f, phov ? P.text : P.textDim, 16, 1.0f);
    dl->AddLine(ImVec2(pc.x - 3, pc.y), ImVec2(pc.x + 4, pc.y), phov ? P.text : P.textDim, 1.0f);
    dl->AddLine(ImVec2(pc.x + 0.5f, pc.y - 3.5f), ImVec2(pc.x + 0.5f, pc.y + 3.5f), phov ? P.text : P.textDim, 1.0f);
    if (phov) {
      ImGui::SetTooltip("%s", i18n::tr("New marker at the playhead"));
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
      }
    }
  }
  struct CornerBtn {
    int id;
    ImVec2 c;
    bool on;
    const char* hint;
    bool shown;
    const char* name = nullptr;  // J-79: toggles: tooltip "Name (key): what it does" from the keymap
    const char* keyId = nullptr;
  };
  constexpr float bw = 20.0f, bh = 16.0f, groupGap = 9.0f;
  // K-112c (Live): the column is titled "Tracks" and the cells sit on its right, in one row.
  const float cr = headX1 - 4, cl = liveLook ? cr - 5 * bw - groupGap - 2 : headX0 + 4;  // usable span
  const bool oneRow = liveLook || cr - cl >= 7 * bw + 2 * groupGap;
  const float ry1 = lg ? std::round(rY + 13.0f)  /* cp54: centred on the 26 px ruler */ : oneRow ? std::round(rY + (gMin.y - rY) * 0.5f) : origin.y + 10.5f, ry2 = oneRow || lg ? ry1 : origin.y + 29.5f;
  const float x0 = cl + bw * 0.5f, xR = cr - bw * 0.5f;
  const float tx = oneRow ? x0 + 3 * bw + groupGap : x0;                // tracks group
  // K-112b (checkpoint 24): the Tracks layout puts the track buttons first, on faces of their own (add
  // tracks, then clear), as 216, 125 and 195 start the track list's header; the audio editing toggles
  // move to the column's right end.
  const float lgGap = 4.0f, lgA = xR - 2 * bw;
  const bool roomR = !liveLook && (oneRow || xR - bw * 0.5f >= x0 + 2.5f * bw + 6);  // two rows: the view cells fit beside the others (Live: no picker, one header width)
  const CornerBtn corner[] = {
      {0, ImVec2(lg ? lgA : x0, ry1), st.stretchResize,
       i18n::N_("resizing an audio clip time-stretches it (the pitch stays) instead of trimming it"), true,
       i18n::N_("Stretch"), "playlist.stretchResize"},
      {1, ImVec2(lg ? lgA + bw : x0 + bw, ry1), st.showFades,
       i18n::N_("audio clips show their fade handles (squares: length, rings: curve) and the volume line (drag up / down for the clip's gain)"),
       true, i18n::N_("Fade and volume handles"), "playlist.fades"},
      {2, ImVec2(lg ? xR : x0 + 2 * bw, ry1), st.zeroCross,
       i18n::N_("audio clip edges, slips and slices land on the nearest zero crossing (no clicks)"), true, i18n::N_("Zero crossings"),
       "playlist.zeroCross"},
      {3, ImVec2(oneRow ? xR - bw : xR, ry1), st.pickerOpen, "Clip source picker panel (Alt+P)", roomR && !lg},
      {4, ImVec2(lg ? x0 + 2 : tx, ry2), false, "Add tracks", true},
      {5, ImVec2(lg ? x0 + 2 + bw + lgGap : tx + bw, ry2), false,
       lg ? "New track with the same settings as the selected one (C9: the reference's duplicate button)" : "Clear the selection (clips and time)", true},
      {6, ImVec2(xR, ry2), st.wideHeaders, "Track header width: narrow / wide", roomR && !lg}};
  {  // group rules
    const ImU32 rule = theme::withAlpha(P.dark ? P.paper : P.ink, 0.14f);
    auto vrule = [&](float gx, float cy) { dl->AddLine(ImVec2(std::round(gx) + 0.5f, cy - 6), ImVec2(std::round(gx) + 0.5f, cy + 6), rule); };
    if (lg) {
      vrule(lgA - bw * 0.5f - 6, ry1);
    } else if (oneRow) {
      vrule(x0 + 2.5f * bw + groupGap * 0.5f, ry1);
      if (roomR) vrule((tx + 1.5f * bw + xR - 1.5f * bw) * 0.5f, ry1);
    } else if (roomR) {
      vrule(xR - bw * 0.5f - 4, ry1);
      vrule(xR - bw * 0.5f - 4, ry2);
    }
  }
  if (liveLook)
    dl->AddText(a.fontRegular, theme::kFontBody - 1, ImVec2(headX0 + 10, std::round(ry1 - (theme::kFontBody - 1) * 0.5f)), P.textDim,
                i18n::tr("Tracks"));
  int hoverCorner = -1;
  for (const auto& b : corner) {
    if (!b.shown) continue;
    const ImVec2 c0(b.c.x - bw * 0.5f, b.c.y - bh * 0.5f), c1(b.c.x + bw * 0.5f, b.c.y + bh * 0.5f);
    const bool hot = hovered && io.MousePos.x >= c0.x && io.MousePos.x < c1.x && io.MousePos.y >= c0.y - 1 && io.MousePos.y < c1.y + 1;
    if (hot) hoverCorner = b.id;
    if (b.on) dl->AddRectFilled(c0, c1, P.dark ? P.surface600 : P.surface700, 3.0f);
    else if (lg && (b.id == 4 || b.id == 5)) {  // the Tracks layout: a quiet face, 8 % lighter on hover
      const ImU32 face = theme::mix(P.dark ? P.surface800 : P.surface700, P.text, 0.04f);
      dl->AddRectFilled(ImVec2(c0.x, c0.y - 1), ImVec2(c1.x, c1.y + 1), hot ? theme::mix(face, P.text, 0.08f) : face, 4.0f);
    } else if (hot) dl->AddRectFilled(c0, c1, theme::withAlpha(P.paper, P.dark ? 0.08f : 0.5f), 3.0f);
    const ImU32 col = b.on ? P.hot : hot ? P.text : P.textDim;
    const float x = b.c.x, y = b.c.y;
    switch (b.id) {
      // J-79: the editing toggles in the J-76 icon family, each fitted by its ink into the same
      // 11 px square: clear of the 16 px cell's edges and one size across the three.
      case 0: case 1: case 2: {
        const ui::Icon icon = b.id == 0 ? ui::Icon::StretchMode : b.id == 1 ? ui::Icon::FadeHandles : ui::Icon::ZeroCross;
        ui::drawIconFit(dl, icon, ImVec2(x - 5.5f, y - 5.5f), ImVec2(x + 5.5f, y + 5.5f), col);
        break;
      }
      case 3: ui::drawIcon(dl, st.pickerOpen ? ui::Icon::Left : ui::Icon::Right, b.c, 10.0f, col); break;
      case 4: ui::drawIcon(dl, ui::Icon::Plus, b.c, 10.0f, col); break;
      case 5:
        if (lg) {  // C9: duplicate: two offset squares
          dl->AddRect(ImVec2(x - 4.5f, y - 2.5f), ImVec2(x + 2.5f, y + 4.5f), col, 1.5f);
          dl->AddRect(ImVec2(x - 2.5f, y - 4.5f), ImVec2(x + 4.5f, y + 2.5f), col, 1.5f);
        } else {
          ui::drawIcon(dl, ui::Icon::Close, b.c, 9.0f, col);
        }
        break;
      case 6: {  // double arrow
        ui::drawIconFit(dl, ui::Icon::StretchMode, ImVec2(x - 7, y - 3), ImVec2(x + 7, y + 3), col);
        break;
      }
      default: break;
    }
    if (hot) a.setHint(b.name ? ui::toolTip(b.name, b.hint, b.keyId) : std::string(i18n::tr(b.hint)));
  }
  if (hoverCorner >= 0) {
    const CornerBtn& hb = corner[hoverCorner];
    ImGui::SetTooltip("%s", hb.name ? ui::toolTip(hb.name, hb.hint, hb.keyId).c_str() : i18n::tr(hb.hint));
  }
  if (liveLook) {
    // K-112c: as the refs draw them, each return track and Main has its own lane, level with its row in
    // the header column: an empty strip across the arrangement (the grid shows through), set off from
    // the tracks by a line.
    const float busTop = yOf(nTracks), mainTop = gMax.y - livelook::kBusRowH - 3;
    const ImU32 laneFill = theme::withAlpha(P.surface800, 0.45f), laneLine = P.dark ? P.ink : P.line;
    dl->PushClipRect(gMin, ImVec2(gMax.x, mainTop), true);
    if (liveBusH > 6 && busTop < mainTop) {
      dl->AddLine(ImVec2(gMin.x, busTop + 2.5f), ImVec2(gMax.x, busTop + 2.5f), laneLine);
      for (float y = busTop + 6; y + livelook::kBusRowH <= busTop + liveBusH + 0.5f; y += livelook::kBusRowH) {
        dl->AddRectFilled(ImVec2(gMin.x, y), ImVec2(gMax.x, y + livelook::kBusRowH - 1), laneFill);
        dl->AddLine(ImVec2(gMin.x, y + livelook::kBusRowH - 0.5f), ImVec2(gMax.x, y + livelook::kBusRowH - 0.5f), laneLine);
      }
    }
    dl->PopClipRect();
    const bool mainLane = busTop + liveBusH <= mainTop;  // never over a track's clips
    if (mainLane) {
      dl->AddRectFilled(ImVec2(gMin.x, mainTop), gMax, laneFill);
      dl->AddLine(ImVec2(gMin.x, mainTop - 0.5f), ImVec2(gMax.x, mainTop - 0.5f), laneLine);
    }
    // Between them, the room where files and devices dropped make a new track.
    const float freeY0 = busTop + std::max(liveBusH, 0.0f) + 8, freeY1 = (mainLane ? mainTop : gMax.y) - 8;
    if (freeY1 - freeY0 >= 44 && gMax.x - gMin.x > 200) {
      const ImVec2 z0(gMin.x + 8, freeY0), z1(gMax.x - 8, freeY1);
      // Checkpoint 55 (gap 8): the room is plain space with a quiet hint, as the reference leaves it; its
      // dashed outline shows only while something is dragged (a file or a device on its way).
      const bool dragging = ImGui::GetDragDropPayload() != nullptr;
      const ImU32 dc = theme::withAlpha(P.textFaint, dragging ? 0.55f : 0.0f);
      for (float x = z0.x; x < z1.x && dragging; x += 8) {
        dl->AddLine(ImVec2(x, z0.y), ImVec2(std::min(x + 4, z1.x), z0.y), dc);
        dl->AddLine(ImVec2(x, z1.y), ImVec2(std::min(x + 4, z1.x), z1.y), dc);
      }
      for (float y = z0.y; y < z1.y && dragging; y += 8) {
        dl->AddLine(ImVec2(z0.x, y), ImVec2(z0.x, std::min(y + 4, z1.y)), dc);
        dl->AddLine(ImVec2(z1.x, y), ImVec2(z1.x, std::min(y + 4, z1.y)), dc);
      }
      const char* dt = i18n::tr("Drop files and instruments here: each makes a new track");
      // K-297 round 06: a plugin dragged over the zone says what the drop does, as a lane does
      // ("Drop to play <plugin> on Track <n>": the new track), and the zone lights.
      std::string planned;
      if (const ImGuiPayload* pl = ImGui::GetDragDropPayload(); pl && pl->IsDataType("GL_PLUGIN") && pl->Data &&
                                                                  ImGui::IsMouseHoveringRect(z0, z1, false) && nTracks < kMaxPlaylistTracks) {
        std::string type(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        while (!type.empty() && type.back() == '\0') type.pop_back();
        browserui::DropPlace zp;
        zp.track = nTracks;
        planned = browserui::pluginDropLabel(a, type, browse::DropTarget::Playlist, zp);
      }
      if (!planned.empty()) {
        const ImU32 hot = lg ? P.accent : theme::rgb(theme::role::kDraw);  // R09-09: Smooth's accent
        dl->AddRectFilled(z0, z1, theme::withAlpha(hot, 0.12f), theme::kRadiusSm);
        dl->AddRect(z0, z1, theme::withAlpha(hot, 0.95f), theme::kRadiusSm, 0, 2.0f);
        dt = planned.c_str();
      }
      if (!dragging && planned.empty()) {
        dt = i18n::tr("Drop files and instruments here");  // (gap 8: shorter and quieter at rest)
      }
      const float hfs = dragging ? theme::kFontCaption + 1 : theme::kFontCaption;
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(hfs, 1e9f, 0, dt);
      dl->AddText(a.fontRegular, hfs, ImVec2(std::round((z0.x + z1.x - ts.x) * 0.5f), std::round((z0.y + z1.y - ts.y) * 0.5f)),
                  !planned.empty() ? P.text : dragging ? P.textFaint : theme::withAlpha(P.textFaint, 0.6f), dt);
    }
    // The minutes ruler under the lanes, and the grid step under the headers.
    const float ry0 = gMax.y, ry1 = gMax.y + timeRulerH;
    dl->AddRectFilled(ImVec2(origin.x, ry0), ImVec2(fullX1, ry1), P.dark ? P.field : P.surface950);
    dl->AddLine(ImVec2(origin.x, ry0), ImVec2(fullX1, ry0), P.dark ? P.ink : P.line);
    dl->PushClipRect(ImVec2(gMin.x, ry0), ImVec2(gMax.x, ry1), true);
    const double secL = tempoMap.secondsAt(std::max(0.0, static_cast<double>(tickAt(gMin.x))));
    const double secR = tempoMap.secondsAt(std::max(0.0, static_cast<double>(tickAt(gMax.x))));
    const double sec0 = tempoMap.secondsAt(static_cast<double>(viewO));  // C44-20: minutes count from the arrangement's start
    const double pxPerSec = (gMax.x - gMin.x) / std::max(1e-6, secR - secL);
    double step = 1;
    for (double c : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0})
      if (c * pxPerSec >= 64) {
        step = c;
        break;
      } else {
        step = c;
      }
    for (double sec = sec0 + std::floor((secL - sec0) / step) * step; sec <= secR + step; sec += step) {
      if (sec < sec0 - 1e-9) continue;
      const float x = std::round(xOf(static_cast<Tick>(tempoMap.tickAtSeconds(sec))));
      if (x < gMin.x - 40 || x > gMax.x) continue;
      dl->AddLine(ImVec2(x + 0.5f, ry0 + 1), ImVec2(x + 0.5f, ry1 - 1), theme::withAlpha(P.textFaint, 0.6f));
      char tb[16];
      const int isec = static_cast<int>(std::lround(sec - sec0));
      std::snprintf(tb, sizeof tb, "%d:%02d", isec / 60, isec % 60);
      // K-112c checkpoint 57 (N56 part 6, 3): a label that would run past the lanes' edge is left out.
      if (x + 4 + livelook::numFont(a)->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, tb).x <= gMax.x) {
        dl->AddText(livelook::numFont(a), theme::kFontLabel, ImVec2(x + 4, ry0 + 3), P.textDim, tb);
      }
    }
    dl->PopClipRect();
    const std::string zl = std::string(i18n::tr("Grid")) + "  " + (st.effGrid > 0 ? timeline::gridName(s, st.effGrid) : std::string("-"));
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(headX0 + 8, ry0 + 3), P.textFaint, zl.c_str());
  }

  // ---------------- interaction ----------------
  const ImVec2 m = io.MousePos;
  const bool inGrid = m.x >= gMin.x && m.x < gMax.x && m.y >= gMin.y && m.y < gMax.y;
  const bool inRuler = m.y >= rY && m.y < gMin.y && m.x >= gMin.x && m.x < gMax.x;
  const bool inHead = m.x >= headX0 && m.x < headX1 && m.y >= gMin.y;
  const Clip* hc = hoverClip != kNoId ? s.clip(hoverClip) : nullptr;
  const bool hoverAuto = hc && hc->kind == ClipKind::Automation && hoverBody;
  // Draw: Shift+drag a clip copies it (R01-03: the Tracks layout copies with Alt, as the reference's
  // Option-drag; Shift there adds to the selection).
  // R01-18: while Cmd gives the Command-click tool it has no meaning of its own in the grid.
  const bool ctlK = io.KeyCtrl && !gCmdSwap;
  const bool shiftDrag = lg ? io.KeyAlt && !ctlK : io.KeyShift && !ctlK;
  const bool slipDrag = ctlK && io.KeyShift;                 // Draw: Ctrl+Shift+drag slips the content
  // K-287: in the Tracks and Live layouts the pointer (Select; Live: Draw mode off) handles a clip as
  // Draw does (move, edges, Shift+drag copy, slip); only a press on empty space drags a box. Studio's
  // Select tool keeps its box everywhere.
  // R05-17: the automation tools work in the automation area; on regions they are the Pointer.
  const Tool baseTool = lg && (st.tool == Tool::AutoSelect || st.tool == Tool::AutoCurve) ? Tool::Select : st.tool;
  const Tool clipTool = (lg || liveLook) && baseTool == Tool::Select && hc ? Tool::Draw : baseTool;
  // K-114: Ctrl (Cmd on macOS) while dragging a clip's edge loops its content
  // (read live during the drag); a plain edge drag extends or trims it. An
  // edge grab wins over Ctrl+click select and Ctrl+Shift slip.
  // io.KeyCtrl is Cmd on macOS (ImGui swaps them): the physical Ctrl key
  // there turns a click into a right-click, so it must not count here.
  const bool loopMod = ctlK;
  const bool onEdge = hc && (hoverEdge || hoverLeftEdge) && hoverZone != clipheader::Zone::Menu;
  const bool loopEdgeGrab = clipTool == Tool::Draw && onEdge && loopMod &&
                            !(hc->track >= 0 && hc->track < nTracks && s.tracks[static_cast<size_t>(hc->track)].locked);
  if (hovered && inGrid) {
    if (hoverPoint >= 0 || hoverHandle >= 0)
      ImGui::SetMouseCursor(hoverHandle >= 0 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Hand);
    else if (hoverMenuGlyph != kNoId)
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (hoverClip != kNoId && clipTool == Tool::Draw)
      ImGui::SetMouseCursor(hoverEdge || hoverLeftEdge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
    else if (st.tool == Tool::Slip && hoverClip != kNoId)
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (st.tool == Tool::Warp && hoverClip != kNoId && plwarp::hovered(hoverClip))  // K-85
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    // N47-07: in Clips' arrangement the bar the ruler shows (counted from the arrangement's start, after
    // the live area), not the stored one.
    const Tick whereT = std::max<Tick>(0, tickAt(m.x)), whereO = liveLook ? livelook::arrangementOrigin(a) : 0;
    std::string where = std::string(i18n::tr("Bar")) + " " +
                        std::to_string(whereT >= whereO ? mm.barIndex(whereT) - mm.barIndex(whereO) + 1 : mm.barIndex(whereT) + 1) + " · " +
                        playlistTrackName(s, static_cast<size_t>(trackAt(m.y)));
    if (lg && hc) where += " \xC2\xB7 " + clipName(s, *hc);  // R03-07: Quick help names the region under the pointer
    if (liveMode && !liveLook && hc && inLiveArea(s, *hc))
      a.setHint(where + "   (live: click to launch on the track's launch grid \xC2\xB7 right-click to stop the track)");
    else if (hc && hc->kind == ClipKind::Pattern && hoverCurve.owner != kNoId && hoverBody) {  // J-173
      const Pattern* hp = s.pattern(hc->pattern);
      const AutomationClip* hcv = hp ? findPatternLane(*hp, hoverCurve.owner, hoverCurve.index) : nullptr;
      a.setHint(where + "   " + (hcv ? patternAutoName(s, *hcv) : std::string()) + "   " +
                i18n::tr("(automation in this pattern \xC2\xB7 double-click: edit it in the automation editor)"));
    } else if (hoverAuto)
      a.setHint(where + "   (drag a point · drag a ring: curve tension · double-click: add point · right-click a point: delete · double-click the title: editor)");
    else if (hoverMenuGlyph != kNoId)
      a.setHint(where + "   (click: clip menu)");
    else if (hc && lg && clipTool == Tool::Draw)  // R01-01 / R01-03: the Tracks layout's own gestures
      a.setHint(where + "   " + i18n::tr("(drag: move \xC2\xB7 lower corners: resize \xC2\xB7 upper corners of an audio region: fade \xC2\xB7 Alt+drag: copy \xC2\xB7 Shift+click: add to the selection \xC2\xB7 double-click: open in the editor \xC2\xB7 right-click: region menu)"));
    else if (hc && hoverZone == clipheader::Zone::Name)  // J-142
      a.setHint(where + "   " + (livelook::active(a) ? i18n::tr("(right-click: clip menu)")  // N46-07
                                                       : i18n::tr("(right-click the name: clip menu \xC2\xB7 right-click elsewhere on the clip: delete)")));
    else if (hc && liveLook && hoverZone == clipheader::Zone::Body && !onEdge && st.tool != Tool::Draw)  // N47-08
      a.setHint(where + "   " + i18n::tr("(click: insert marker here \xC2\xB7 drag: select time \xC2\xB7 drag the title bar: move \xC2\xB7 edges: resize \xC2\xB7 right-click: clip menu)"));
    else if (hc && liveLook && clipTool == Tool::Draw && !onEdge)  // N47-08: the title bar
      a.setHint(where + "   " + i18n::tr("(drag: move \xC2\xB7 Shift+drag: copy \xC2\xB7 edges: resize \xC2\xB7 double-click: open in the clip band \xC2\xB7 right-click: clip menu)"));
    else if (hc && clipTool == Tool::Draw && onEdge)  // K-114
      a.setHint(where + "   " + loopEdgeHint(*hc, hoverEdge, loopMod, st.stretchResize));
    else if (hc && clipTool == Tool::Draw)
      a.setHint(where + std::string("   (drag: move · edges: resize / trim") + (hc->kind == ClipKind::Audio ? (st.stretchResize ? " (stretching)" : "") : "") +
                " · " + kLoopKey + "+drag an edge: loop · Shift+drag: copy · Ctrl+Shift+drag: slip content · Esc / right-click while dragging: cancel · double-click: " +
                (hc->kind == ClipKind::Audio ? "channel settings" : hc->kind == ClipKind::Automation ? "edit" : "open") +
                (livelook::active(a) ? " · right-click: clip menu)" : " · right-click: delete)"));  // N46-07: Clips' right-click opens the menu
    else
      a.setHint(where + "   (wheel: scroll · Shift+wheel: horizontal · Ctrl+wheel: zoom · Alt+wheel: track height)");
  } else if (hovered && inRuler) {
    ImGui::SetMouseCursor(hoverMarker >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
    if (hoverMarker >= 0)
      a.setHint("Marker \"" + s.markers[static_cast<size_t>(hoverMarker)].name + "\": click to jump here \xC2\xB7 right-click: rename, time signature, action, loop");
    else
      a.setHint(liveLook ? i18n::tr("Click or drag to move the song position (snaps to the grid, Alt = free)  \xC2\xB7  Ctrl+drag or right-drag: "
                                    "time selection  \xC2\xB7  the loop brace below the numbers: drag it, drag its ends, click it to switch Loop  \xC2\xB7  Cmd+L: loop the selection")
                         : "Click or drag to move the song position (snaps to the grid, Alt = free)  \xC2\xB7  Ctrl+drag or right-drag: "
                           "time selection (loops playback)  \xC2\xB7  double-click: clear the selection");
  } else if (hovered && inHead) {
    const char* modeHint = trackui::headerHint(a, headModeCtl ? headModeTrack : trackAt(m.y), headModeCtl);
    if (liveHeadPart == livelook::HeaderPart::Io) {  // checkpoint 55: the In/Out column says its own
    } else if (liveHeadPart != livelook::HeaderPart::None && liveHeadPart != livelook::HeaderPart::Name) {  // K-112c
      a.setHint(livelook::headerHint(liveHeadPart));
      if (liveHeadPart == livelook::HeaderPart::Volume || liveHeadPart == livelook::HeaderPart::Pan) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if (headPlus >= 0) {  // K-12
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      a.setHint(i18n::tr("Click: give the track an instrument, or a new pattern for its instrument (opened in the piano roll)"));
    } else if (headLed >= 0 && headLedSolo) a.setHint(lg ? i18n::tr("Click: solo the track's channel strip (the others go quiet, their mutes stay) or end the solo")
                                                             : i18n::tr("Click: solo the track (mute every other track) or end the solo"));
    else if (headLed >= 0) a.setHint("Click: mute / unmute the track \xC2\xB7 right-click: solo (mute every other track)");
    else if (headArm >= 0) a.setHint(i18n::tr("Click: arm the track for recording"));
    else if (headMon >= 0) a.setHint(i18n::tr("Click: monitor the track's input (hear what is played through it)"));
    else if (headPower >= 0) a.setHint(i18n::tr("Click: turn the track's plugins off to save processing, or back on"));
    else if (headVol >= 0 || volDrag >= 0) a.setHint(i18n::tr("Drag left / right: volume \xC2\xB7 double-click: reset"));
    else if (headPan >= 0 || panDrag >= 0) a.setHint(i18n::tr("Drag up / down: pan \xC2\xB7 double-click: centre"));
    else if (headHandle >= 0) a.setHint("Drag up / down to move the track \xC2\xB7 click: track menu");
    else if (*modeHint) a.setHint(modeHint);  // PL-08
    else if (headName >= 0) a.setHint(i18n::tr("Double-click the name to rename the track \xC2\xB7 right-click: track menu"));
    else if (const int ht = trackAt(m.y); ht >= 0 && ht < nTracks && trackmodes::modeOf(s, ht) == TrackMode::Instrument)
      a.setHint(i18n::tr("Double-click: show the track's instrument \xC2\xB7 double-click the name: rename \xC2\xB7 right-click: track menu"));
    else a.setHint(i18n::tr("Double-click the name to rename the track \xC2\xB7 right-click: track menu \xC2\xB7 drag the bottom edge to resize"));
  }

  // K-114 / K-140: the loop key over a clip's edge, or during an edge drag: a loop
  // badge by the pointer, so the drag's mode shows before and while it happens.
  // Only where a loop-key drag of that edge loops (edgeCanLoop), and in a drag
  // only while it loops (past the cut point, or a clip that already looped).
  bool loopBadge = false;
  if (loopMod && cg.kind == ClipGesture::None && hovered && inGrid && loopEdgeGrab && hoverPoint < 0 && hoverHandle < 0)
    loopBadge = edgeCanLoop(s, *hc, hoverEdge, tempoMap);
  if (loopMod && (cg.kind == ClipGesture::Resize || cg.kind == ClipGesture::Trim) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    loopBadge = edgeDragLoops(s, cg.base, cg.anchor, cg.kind == ClipGesture::Resize, cg.moved, tempoMap);
  devGeom.loopBadge = loopBadge;
  if (loopBadge) {
    const ImVec2 bc = drawLoopBadge(ImGui::GetForegroundDrawList(), m, P);
    devGeom.loopBadgeX = bc.x, devGeom.loopBadgeY = bc.y;
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
  auto group = [&](const char* label) {  // the gesture's edits become one undo step
    if (!cg.grouped) {
      a.doc.beginGroup(label);
      cg.grouped = true;
    }
  };

  bool lClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool rClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  auto laneValueAt = [&](int tr, float py) {  // R04-08: inverse of laneValueY
    const float t0 = curveTop(tr) + 3, t1 = curveBot(tr) - 3;
    return 1.0 - std::clamp((py - t0) / std::max(1.0f, t1 - t0), 0.0f, 1.0f);
  };
  if (laneH > 0 && m.y >= rLaneY && m.y < rY && m.x >= gMin.x) lClick = rClick = false;  // K-179: the meter lane's own
  if (gEndDrag) lClick = rClick = false;  // checkpoint 47: the Project End marker took the press
  // R08-10: the open folder's own header row: a click there closes it; the lane beside it takes no press.
  if ((lClick || rClick) && foldHdrH > 0 && m.y >= yOf(foldHead) - foldHdrH && m.y < yOf(foldHead) && m.y >= gMin.y) {
    if (lClick && inHead) logicplug::setAutoFolderOpen(a, false);
    if (inHead || inGrid) lClick = rClick = false;
  }
  // K-123: take lanes. A press in a lane starts a swipe; on release the swiped range plays that take
  // (a click without a drag picks it for the comp section there). One undo step each. Right-click: the
  // lanes' menu. The lane headers: a click picks that take wherever it has audio.
  static int takeMenuTrack = -1, takeMenuTake = -1;
  auto laneLocked = [&](int tr) { return tr >= 0 && tr < nTracks && s.tracks[static_cast<size_t>(tr)].locked; };
  {
    int lt = -1, lk = -1;
    if (const int tr = trackAt(m.y); inGrid && tr >= 0 && tr < nTracks) {
      const int nl = takeLaneN[static_cast<size_t>(tr)];
      const float top = takeLaneTop(tr);
      if (nl > 0 && m.y >= top && m.y < top + static_cast<float>(nl) * laneTH)
        lt = tr, lk = std::clamp(static_cast<int>((m.y - top) / laneTH), 0, nl - 1);
    }
    if (tswipe.track >= 0) {
      if (tswipe.track < nTracks) tswipe.to = std::max<Tick>(0, snapPos(std::max<Tick>(0, tickAt(m.x))));
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const TakeSwipe w = tswipe;
        tswipe = TakeSwipe{};
        const bool drag = std::fabs(m.x - w.x0) >= 4.0f && w.to != w.from;
        if (w.track < nTracks) {
          bool changed = false;
          if (drag) {
            a.doc.editSong("comp takes", [&](Song& x) { changed = takes::compRange(x, w.track, w.take, std::min(w.from, w.to), std::max(w.from, w.to)); });
          } else {
            const Tick at = std::max<Tick>(0, tickAt(w.x0));
            a.doc.editSong("comp take", [&](Song& x) { changed = takes::compPick(x, w.track, w.take, at); });
          }
          if (!changed) a.notify("Take " + std::to_string(w.take + 1) + " has no audio there");
        }
      }
      lClick = rClick = false;
    }
    if (lt >= 0) {
      if (hovered && tswipe.track < 0) a.setHint("Take " + std::to_string(lk + 1) + ": drag across it to play this take there, click to pick it for that section \xC2\xB7 right-click: flatten, remove unused takes");
      if (lClick && !laneLocked(lt)) {
        tswipe.track = lt, tswipe.take = lk, tswipe.x0 = m.x;
        tswipe.from = tswipe.to = std::max<Tick>(0, snapPos(std::max<Tick>(0, tickAt(m.x))));
        lClick = false;
      } else if (rClick) {
        takeMenuTrack = lt, takeMenuTake = lk;
        ImGui::OpenPopup("##takelanes");
        rClick = false;
      } else if (lClick) {
        lClick = false;
      }
    }
  }
  if (takeHeadTrack >= 0) {
    a.setHint("Take " + std::to_string(takeHeadTake + 1) + ": click to play this take wherever it has audio \xC2\xB7 right-click: the lanes' menu");
    if (lClick && recordrows::laneHeaderClick(a, takeHeadTrack, takeHeadTake)) {  // K-123: a rating dot, the solo mark
      lClick = false;
    } else if (lClick && !laneLocked(takeHeadTrack)) {
      const int tr = takeHeadTrack, k = takeHeadTake;
      a.doc.editSong("comp take", [&](Song& x) { takes::compRange(x, tr, k, 0, std::numeric_limits<Tick>::max() / 4); });
      lClick = false;
    } else if (rClick) {
      takeMenuTrack = takeHeadTrack, takeMenuTake = takeHeadTake;
      ImGui::OpenPopup("##takelanes");
      rClick = false;
    }
  }
  if (ImGui::BeginPopup("##takelanes")) {
    const int tr = takeMenuTrack;
    const std::vector<ObjectId> groups = tr >= 0 && tr < nTracks ? takes::groupsOn(s, tr) : std::vector<ObjectId>{};
    if (groups.empty()) ImGui::CloseCurrentPopup();
    recordrows::laneMenuItems(a, tr, takeMenuTake);  // K-123: rating, solo, audition
    if (ImGui::MenuItem(i18n::tr("Flatten the comp"), nullptr, false, !groups.empty() && !laneLocked(tr))) {
      std::string err;
      int made = 0;
      const std::string dir = recordedDir(a);
      a.doc.editSong("flatten comp", [&](Song& x) {
        for (ObjectId g : groups) made += takes::flatten(x, g, dir, &err) != kNoId;
      });
      a.notify(made ? std::string("The comp is one clip now (undo brings the takes back)") : "Nothing to flatten: " + err);
    }
    ui::Hint("Renders the parts that play into one audio clip and removes the takes");
    if (ImGui::MenuItem(i18n::tr("Remove unused takes"), nullptr, false, !groups.empty() && !laneLocked(tr))) {
      int n = 0;
      a.doc.editSong("remove unused takes", [&](Song& x) {
        for (ObjectId g : groups) n += takes::removeUnused(x, g);
      });
      if (n == 0) a.notify("Every take plays somewhere: nothing removed");
    }
    ui::Hint("Removes the parts of the takes that are not in the comp; what plays stays");
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Hide the take lanes"), nullptr, false, tr >= 0 && tr < nTracks))
      a.doc.editPlaylistTrack(tr, "hide take lanes", [](PlaylistTrack& t) { t.takeLanes = false; });
    ImGui::EndPopup();
  }
  {  // K-112b (b4): the automation lanes' own clicks: a point added (and dragged) or removed; the header's parameter menu
    struct LaneDrag {
      ObjectId autoClip = kNoId;
      int point = -1, track = -1;
      Tick clipStart = 0, offset = 0, lo = 0, hi = 0;
      float vmin = 0, vmax = 1;
      int mode = 0;  // R05-17: 0 a point, 1 a segment bent (Automation Curve), 2 a range selected, 3 the selection moved
      float y0 = 0, baseT = 0, rise = 1;
      std::vector<std::pair<int, float>> base;  // mode 3: the selected points' values at the start
      std::vector<Tick> basePos;                // R07-07: and their positions
      Tick t0 = 0, minShift = 0, maxShift = 0;  // R07-07: the press's tick and how far they may go
    };
    static LaneDrag ld;
    // R08-02: the selected points belong to a lane on screen. Lanes hidden (A), the track folded or
    // the lane showing another parameter: the selection goes, so Delete never takes a point unseen.
    if (gAutoSelClip != kNoId) {
      const AutomationClip* sac = s.automation(gAutoSelClip);
      bool seen = lg && sac && laneOf(gAutoSelTrack) > 0;
      if (seen) {
        const ParamId lp = autoLanes[gAutoSelTrack];
        seen = lp.owner == sac->owner && lp.index == sac->index;
      }
      if (!seen) gAutoSelPts.clear(), gAutoSelClip = kNoId, gAutoSelTrack = -1, gAutoSelDragging = false;
    }
    // R05-17: Delete removes the Automation Select tool's points (one undo step); a curve keeps one point.
    if (lg && gAutoSelClip != kNoId && !gAutoSelPts.empty() && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
      const std::set<int> del = gAutoSelPts;
      a.doc.editAutomation(gAutoSelClip, "delete automation points", [&del](AutomationClip& x) {
        for (auto it = del.rbegin(); it != del.rend(); ++it)
          if (*it >= 0 && *it < static_cast<int>(x.points.size()) && x.points.size() > 1) x.points.erase(x.points.begin() + *it);
      });
      gAutoSelPts.clear();
      gAutoSelClip = kNoId;
    }
    if (ld.autoClip != kNoId && ld.mode != 0) {
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        a.doc.endGesture();
        gAutoSelDragging = false;
        ld = LaneDrag{};
      } else if (const AutomationClip* lac = s.automation(ld.autoClip); lac && ld.track >= 0 && ld.track < nTracks && laneOf(ld.track) > 0) {
        if (ld.mode == 1) {
          const float t = std::clamp(ld.baseT + (m.y - ld.y0) / 120.0f * ld.rise, -1.0f, 1.0f);
          const int k = ld.point;
          a.doc.editAutomation(ld.autoClip, "bend automation", [&](AutomationClip& c) {
            if (k >= 0 && k < static_cast<int>(c.points.size())) c.points[static_cast<size_t>(k)].tension = t;
          }, (0xB5ull << 56) | ld.autoClip);
        } else if (ld.mode == 2) {
          gAutoSelX1 = m.x;
          const Tick r0 = std::max<Tick>(0, tickAt(std::min(gAutoSelX0, m.x))) - ld.clipStart + ld.offset;
          const Tick r1 = std::max<Tick>(0, tickAt(std::max(gAutoSelX0, m.x))) - ld.clipStart + ld.offset;
          gAutoSelPts.clear();
          for (size_t k = 0; k < lac->points.size(); ++k)
            if (lac->points[k].pos >= r0 && lac->points[k].pos <= r1) gAutoSelPts.insert(static_cast<int>(k));
        } else if (ld.mode == 3) {
          const float t0 = curveTop(ld.track) + 3, t1 = curveBot(ld.track) - 3;
          const float dn = (ld.y0 - m.y) / std::max(1.0f, t1 - t0);
          const float dv = ld.vmax > ld.vmin ? dn / (ld.vmax - ld.vmin) : dn;
          const auto base = ld.base;
          const auto basePos = ld.basePos;
          // R07-07: across too, never past an unselected neighbour (the points keep their order)
          const Tick shift = std::clamp<Tick>(std::max<Tick>(0, tickAt(m.x)) - ld.t0, ld.minShift, ld.maxShift);
          a.doc.editAutomation(ld.autoClip, "move automation points", [&](AutomationClip& c) {
            for (size_t i = 0; i < base.size(); ++i) {
              const int k = base[i].first;
              if (k < 0 || k >= static_cast<int>(c.points.size())) continue;
              c.points[static_cast<size_t>(k)].value = std::clamp(base[i].second + dv, 0.0f, 1.0f);
              if (i < basePos.size()) c.points[static_cast<size_t>(k)].pos = basePos[i] + shift;
            }
          }, (0xB6ull << 56) | ld.autoClip);
        }
      }
      lClick = rClick = false;
    }
    if (ld.autoClip != kNoId && ld.mode == 0) {
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        a.doc.endGesture();
        ld = LaneDrag{};
      } else if (ld.track >= 0 && ld.track < nTracks && laneOf(ld.track) > 0) {
        const double n = laneValueAt(ld.track, m.y);
        const float v = ld.vmax > ld.vmin ? static_cast<float>(std::clamp((n - ld.vmin) / (ld.vmax - ld.vmin), 0.0, 1.0)) : 0.5f;
        const Tick pos = std::clamp<Tick>(std::max<Tick>(0, tickAt(m.x)) - ld.clipStart + ld.offset, ld.lo, ld.hi);
        const int k = ld.point;
        a.doc.editAutomation(ld.autoClip, "move automation point", [&](AutomationClip& c) {
          if (k >= 0 && k < static_cast<int>(c.points.size())) c.points[static_cast<size_t>(k)].pos = pos, c.points[static_cast<size_t>(k)].value = v;
        }, (0xB4ull << 56) | ld.autoClip);
        if (const auto lit = autoLanes.find(ld.track); lit != autoLanes.end())  // K-112B: the value in its units (dB)
          if (const auto d = describeParam(s, lit->second))
            a.setHint(paramDisplayName(s, lit->second) + ": " + laneValueText(s, lit->second, d->fromNormalized(n), *d));
      }
      lClick = rClick = false;
    }
    int laneTrack = -1;
    const Tool autoTool = io.KeySuper || io.KeyCtrl ? st.cmdTool : st.tool;
    const bool autoTools = lg && (autoTool == Tool::AutoSelect || autoTool == Tool::AutoCurve);
    if (lg && inGrid)
      for (const auto& lt : autoLanes) {
        if (inCurveArea(lt.first, m.y)) laneTrack = lt.first;
      }
    // R07-02: with an automation tool, a press anywhere in a row that shows automation (its regions'
    // name bars too) works on that automation.
    if (autoTools && inGrid && laneTrack < 0)
      if (const int rt = trackAt(m.y); rt >= 0 && rt < nTracks && autoLanes.count(rt) && laneOf(rt) > 0) laneTrack = rt;
    // R09-01: with the Pencil, the strip above the curve (where only the regions' name bars take the
    // press) belongs to the lane too when no region is under the pointer: as the reference's, a
    // Pencil click on a track that shows its automation adds a point, never a region.
    if (lg && inGrid && laneTrack < 0 && hoverClip == kNoId && autoTool == Tool::Draw && m.y >= gMin.y)
      if (const int rt = trackAt(m.y); rt >= 0 && rt < nTracks && autoLanes.count(rt) && laneOf(rt) > 0 && m.y >= yOf(rt) && m.y < yOf(rt) + hOf(rt))
        laneTrack = rt;
    bool laneMenuClick = false;
    // R08-02: a click anywhere else in the tracks area deselects the points (as a click on empty space does).
    if (laneTrack < 0 && lClick && inGrid) gAutoSelPts.clear(), gAutoSelClip = kNoId, gAutoSelTrack = -1;
    if (laneTrack >= 0 && (lClick || rClick)) {
      laneMenuClick = rClick;  // (cleared when the right-click deletes a point)
      const ParamId pid = autoLanes[laneTrack];
      const double n = laneValueAt(laneTrack, m.y);
      const Tick at = std::max<Tick>(0, tickAt(m.x));
      // The clip that drives the parameter here (the last one placed wins, as it plays).
      const Clip* hit = nullptr;
      for (const Clip& c : s.playlist)
        if (c.kind == ClipKind::Automation && at >= c.start && at < c.start + s.clipLength(c))
          if (const AutomationClip* ac = s.automation(c.source); ac && ac->owner == pid.owner && ac->index == pid.index) hit = &c;
      if (hit) {
        const Clip c = *hit;
        const AutomationClip ac = *s.automation(c.source);
        Tick pos = at - c.start + c.offset;
        if (c.loop && ac.loop > 0) pos %= ac.loop;
        int near = -1;  // a point within reach of the pointer
        for (size_t k = 0; k < ac.points.size(); ++k) {
          const AutoPoint& p = ac.points[k];
          const float px = xOf(c.start + p.pos - c.offset), py = laneValueY(laneTrack, ac.min + p.value * (ac.max - ac.min));
          if (std::fabs(px - m.x) <= 5 && std::fabs(py - m.y) <= 6) near = static_cast<int>(k);
        }
        const Tool laneTool = io.KeySuper || io.KeyCtrl ? st.cmdTool : st.tool;
        if (lClick && laneTool == Tool::AutoCurve && near < 0) {  // R05-17: bend the segment under the pointer
          size_t k = 1;
          while (k < ac.points.size() && ac.points[k].pos <= pos) ++k;
          if (k >= 1 && k < ac.points.size()) {
            ld = LaneDrag{};
            ld.autoClip = ac.id, ld.track = laneTrack, ld.mode = 1, ld.point = static_cast<int>(k), ld.y0 = m.y;
            ld.baseT = ac.points[k].tension;
            ld.rise = ac.points[k].value >= ac.points[k - 1].value ? 1.0f : -1.0f;
          }
          lClick = false;
        } else if (lClick && laneTool == Tool::AutoSelect) {  // R05-17: select points by a range, or move the selected ones
          ld = LaneDrag{};
          ld.autoClip = ac.id, ld.track = laneTrack, ld.clipStart = c.start, ld.offset = c.offset, ld.y0 = m.y;
          ld.vmin = ac.min, ld.vmax = ac.max;
          if (near >= 0 && gAutoSelClip == ac.id && gAutoSelPts.count(near)) {
            ld.mode = 3;
            ld.t0 = at;
            ld.minShift = std::numeric_limits<Tick>::min() / 4, ld.maxShift = std::numeric_limits<Tick>::max() / 4;
            const int np = static_cast<int>(ac.points.size());
            for (int k : gAutoSelPts) {
              if (k >= np) continue;
              const Tick pk = ac.points[static_cast<size_t>(k)].pos;
              ld.base.emplace_back(k, ac.points[static_cast<size_t>(k)].value);
              ld.basePos.push_back(pk);
              int pv = k - 1, nx = k + 1;  // the nearest unselected neighbours bound the move
              while (pv >= 0 && gAutoSelPts.count(pv)) --pv;
              while (nx < np && gAutoSelPts.count(nx)) ++nx;
              const Tick lo = pv >= 0 ? ac.points[static_cast<size_t>(pv)].pos : 0;
              const Tick hi = nx < np ? ac.points[static_cast<size_t>(nx)].pos : std::max<Tick>(pk, s.clipLength(c) + c.offset);
              ld.minShift = std::max(ld.minShift, lo - pk), ld.maxShift = std::min(ld.maxShift, hi - pk);
            }
            ld.maxShift = std::max(ld.maxShift, ld.minShift);
          } else {
            ld.mode = 2;
            st.selected.clear();  // Delete now acts on the points
            gAutoSelClip = ac.id, gAutoSelTrack = laneTrack, gAutoSelDragging = true;
            gAutoSelX0 = gAutoSelX1 = m.x;
            gAutoSelPts.clear();
            if (near >= 0) gAutoSelPts.insert(near);
          }
          lClick = false;
        }
        if (lClick && (laneTool == Tool::AutoCurve || laneTool == Tool::AutoSelect)) lClick = false;
        if (lClick) gAutoSelPts.clear(), gAutoSelClip = kNoId;
        if (rClick && near >= 0) {
          laneMenuClick = false;
          a.doc.editAutomation(ac.id, "delete automation point", [&](AutomationClip& x) {
            if (near < static_cast<int>(x.points.size()) && x.points.size() > 1) x.points.erase(x.points.begin() + near);
          });
        } else if (lClick) {
          const float v = ac.max > ac.min ? static_cast<float>(std::clamp((n - ac.min) / (ac.max - ac.min), 0.0, 1.0)) : 0.5f;
          int idx = near;
          if (idx < 0)
            a.doc.editAutomation(ac.id, "add automation point", [&](AutomationClip& x) { idx = insertAutoPoint(x, AutoPoint{pos, v, 0, AutoShape::Curve}); });
          if (const AutomationClip* now = a.doc.song().automation(ac.id); now && idx >= 0 && idx < static_cast<int>(now->points.size())) {
            const size_t k = static_cast<size_t>(idx);
            ld.autoClip = ac.id, ld.point = idx, ld.track = laneTrack, ld.clipStart = c.start, ld.offset = c.offset;
            ld.lo = k > 0 ? now->points[k - 1].pos : 0;
            ld.hi = k + 1 < now->points.size() ? now->points[k + 1].pos : std::max<Tick>(now->points[k].pos, s.clipLength(c) + c.offset);
            ld.vmin = now->min, ld.vmax = now->max;
          }
        }
      } else if (lClick && (io.KeySuper || io.KeyCtrl ? st.cmdTool : st.tool) != Tool::AutoCurve &&
                 (io.KeySuper || io.KeyCtrl ? st.cmdTool : st.tool) != Tool::AutoSelect) {  // (R05-17: those tools add no curve)
        // R02-03: nothing drives it here. The reference has one curve per track: the first click
        // adds a point at the project start with the present value and one at the click, and
        // later clicks add to the same curve. So the curve before the click (a clip of this lane
        // on its own track that ends before it) grows up to it; else one clip starts the curve,
        // on the lane's own track, from the end of whatever drives the parameter earlier (the
        // project start) to the start of whatever drives it later (else past the song's end).
        // Past its last point the value holds. No new tracks, no clip per click.
        const auto d = describeParam(s, pid);
        if (d) {
          const MeterMap mmap(s);
          const Clip* prev = nullptr;
          Tick nextStart = -1, songEnd = 0;
          for (const Clip& c : s.playlist) {
            songEnd = std::max(songEnd, c.start + s.clipLength(c));
            if (c.kind != ClipKind::Automation) continue;
            const AutomationClip* ac = s.automation(c.source);
            if (!ac || ac->owner != pid.owner || ac->index != pid.index) continue;
            const Tick e = c.start + s.clipLength(c);
            if (e <= at && (!prev || e > prev->start + s.clipLength(*prev))) prev = &c;
            if (c.start > at && (nextStart < 0 || c.start < nextStart)) nextStart = c.start;
          }
          const Tick end = nextStart >= 0 ? nextStart : std::max(songEnd, mmap.addBars(mmap.barStartAt(at), 1));
          const float vn = static_cast<float>(std::clamp(n, 0.0, 1.0));
          const AutomationClip* pac = prev ? s.automation(prev->source) : nullptr;
          if (prev && pac && prev->track == laneTrack && !prev->loop) {  // the curve before grows up to the click
            const ObjectId kid = prev->id, aid = pac->id;
            const Tick pos = at - prev->start + prev->offset;
            const Tick newLen = end - prev->start;
            a.doc.editSong("add automation point", [&](Song& x) {
              for (Clip& k : x.playlist)
                if (k.id == kid) k.length = std::max(k.length, newLen);
              if (AutomationClip* q = x.automation(aid)) {
                const float v = q->max > q->min ? static_cast<float>(std::clamp((vn - q->min) / (q->max - q->min), 0.0f, 1.0f)) : 0.5f;
                insertAutoPoint(*q, AutoPoint{pos, v, 0, AutoShape::Curve});
              }
            });
          } else {
            const Tick start = prev ? prev->start + s.clipLength(*prev) : 0;
            double n0 = 0.5;  // the value the curve starts from: what plays there now
            bool fromCurve = false;
            for (const AutomationLane& l : automationLanes(s))
              if (l.targets(pid.owner, pid.index)) n0 = l.valueAt(static_cast<double>(start)), fromCurve = true;
            if (!fromCurve)
              if (const auto cur = getParam(s, pid)) n0 = d->toNormalized(*cur);
            const std::string name = paramDisplayName(s, pid);
            const int tr = laneTrack;
            a.doc.editSong("create automation", [&](Song& x) {
              AutomationClip c;
              c.id = x.allocateId();
              c.name = name;
              c.owner = pid.owner;
              c.index = pid.index;
              c.points = {AutoPoint{0, static_cast<float>(std::clamp(n0, 0.0, 1.0)), 0, AutoShape::Curve}};
              if (at > start) c.points.push_back(AutoPoint{at - start, vn, 0, AutoShape::Curve});
              else c.points[0].value = vn;
              Clip k;
              k.id = x.allocateId();
              k.kind = ClipKind::Automation;
              k.source = c.id;
              k.track = tr;
              k.start = start;
              k.length = std::max<Tick>(1, end - start);
              x.automations.push_back(std::move(c));
              x.playlist.push_back(k);
              if (tr >= 0 && tr < static_cast<int>(x.tracks.size())) {  // R05-01: the track's own curve from now on
                PlaylistTrack& lt = x.tracks[static_cast<size_t>(tr)];
                lt.laneSet = true, lt.laneOwner = pid.owner, lt.laneIndex = pid.index;
              }
            });
          }
        }
      }
      lClick = false;
      if (!laneMenuClick) rClick = false;  // R03-05: a right-click off a point opens the lane menu below
    }
    if (autoTools && inGrid) lClick = false;  // R07-02: never a region press (select, move, then Delete)
    if (laneTrack >= 0 && rClick && laneMenuClick) {  // R03-05: a right-click on the lane: its parameter menu
      laneMenu = laneTrack;
      ImGui::OpenPopup("##lanepar");
      rClick = false;
    }
    if (laneRowHead >= 0 && rClick) {  // R03-05: the lane header row's right-click: the parameter menu
      laneMenu = laneRowHead;
      ImGui::OpenPopup("##lanepar");
      lClick = rClick = false;
    }
    if (laneModeHead >= 0 && (lClick || rClick)) {  // K-112B: the automation mode menu
      laneModeMenu = laneModeHead;
      ImGui::OpenPopup("##lanemode");
      lClick = rClick = false;
    }
    if (laneHead >= 0 && (lClick || rClick)) {  // R04-13: a right-click on the parameter too (it opened the track menu)
      laneMenu = laneHead;
      ImGui::OpenPopup("##lanepar");
      lClick = rClick = false;
    }
  }
  if (lClick && inGrid) st.trackSel.clear();  // GitHub #46: a click among the clips ends the header selection
  // Q-3: Esc or a right-click during a clip drag (move, Shift+drag copies,
  // resize, trim, slip) cancels it: the clips go back, the copies go away,
  // the selection is restored and nothing is recorded.
  if ((cg.kind == ClipGesture::Move || cg.kind == ClipGesture::Resize || cg.kind == ClipGesture::Trim || cg.kind == ClipGesture::Slip ||
       cg.kind == ClipGesture::Warp) &&
      ImGui::IsMouseDown(ImGuiMouseButton_Left) && (keys::pressedHeld("playlist.cancelDrag") || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    if (cg.kind == ClipGesture::Warp) plwarp::end();
    if (cg.grouped) a.doc.cancelGroup();
    st.selected = cg.selBefore;
    clipAnim.clear();
    cg = ClipGesture{};
    cg.cancelled = true;
    rClick = false;
    a.setHint(i18n::tr("Drag cancelled"));
  }
  {  // scrollers: drag the thumbs, click the rail to page
    if (barDrag != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) barDrag = 0;
    if (lClick && overEnd >= 3) {  // K-24: an end grabbed: the opposite end's edge is held while the drag zooms
      barDrag = overEnd;
      if (overEnd == 3 || overEnd == 4) {
        const bool bottom = overEnd == 4;
        barGrab = m.y - (bottom ? vThumbY + vThumbH : vThumbY);
        barFixed = bottom ? vThumbY : vThumbY + vThumbH;
        barLen0 = vThumbH;
        barZ0 = trackH;
        const float px = bottom ? 0.0f : gridH;
        barRow = smoothzoom::rowAnchorAt(trackY.data(), nTracks, trackH, static_cast<double>(px + st.scrollY), px);
      } else {
        const bool right = overEnd == 6;
        barGrab = m.x - (right ? hThumbX + hThumbW : hThumbX);
        barFixed = right ? hThumbX : hThumbX + hThumbW;
        barLen0 = hThumbW;
        barZ0 = st.pxPerTick;
        barEdgePx = right ? 0.0f : gridW;
        barTick = (static_cast<double>(barEdgePx) + st.scrollX) / std::max(1e-9, static_cast<double>(st.pxPerTick));
      }
      lClick = false;
    }
    if (barDrag == 1 && totalH > gridH) {
      const float t = (m.y - barGrab - vb0.y) / std::max(1.0f, vb1.y - vb0.y - vThumbH);
      st.scrollY = std::clamp(t, 0.0f, 1.0f) * (totalH - gridH);
    } else if (barDrag == 2 && contentW > gridW) {
      const float t = (m.x - barGrab - hb0.x) / std::max(1.0f, hb1.x - hb0.x - hThumbW);
      st.scrollX = oPx + std::clamp(t, 0.0f, 1.0f) * (contentW - gridW);
    }
    if (lClick && overV) {
      if (m.y >= vThumbY && m.y < vThumbY + vThumbH) barDrag = 1, barGrab = m.y - vThumbY;
      else st.scrollY += (m.y < vThumbY ? -gridH : gridH) * 0.9f;
      lClick = false;
    } else if (lClick && overH) {
      if (m.x >= hThumbX && m.x < hThumbX + hThumbW) barDrag = 2, barGrab = m.x - hThumbX;
      else st.scrollX = std::max(oPx, st.scrollX + (m.x < hThumbX ? -gridW : gridW) * 0.9f);
      lClick = false;
    }
    if (barDrag != 0) lClick = false;
  }
  if ((lClick || rClick) && hoverCorner >= 0) {  // header corner buttons
    switch (hoverCorner) {
      case 0: st.stretchResize = !st.stretchResize; break;
      case 1: st.showFades = !st.showFades; break;
      case 2: st.zeroCross = !st.zeroCross; break;
      case 3: st.pickerOpen = !st.pickerOpen; break;
      case 4:  // R01-16: the Tracks layout's "+" opens the New tracks dialog, as the reference's does
        if (lg) newtracks::open();
        else ImGui::OpenPopup("##addtracks");
        break;
      case 5:
        if (lg) {
          const int ft = logicplug::focusTrack(a);
          a.later([&a, ft] { logicplug::newTrackSameSettings(a, ft, false); });
          break;
        }
        st.selected.clear();
        st.selStart = st.selEnd = 0;
        break;
      case 6: st.wideHeaders = !st.wideHeaders; break;
      default: break;
    }
    lClick = rClick = false;
  }
  if (lClick && headPlus >= 0) {  // K-12: the track "+" opens its menu (a new pattern goes to the song position in view, else the view's start)
    const Tick songPos = a.engine.mode() == PlayMode::Song ? static_cast<Tick>(a.engine.position()) : Tick(-1);
    trackplusui::open(a, headPlus, trackplus::placeTick(s, songPos, tickAt(gMin.x), tickAt(gMax.x)), headPlusAnchor);
    lClick = false;
  }
  {  // Track resize: drag the bottom edge of a track header (one undo step per drag).
    static int resizing = -1;
    static float resizeY0 = 0, resizeH0 = 1;
    if (resizing >= 0 && resizing < nTracks) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float nh = std::clamp(resizeH0 + (m.y - resizeY0) / std::max(1.0f, trackH), 0.25f, 4.0f);
        if (liveLook) {  // checkpoint 55 (gap 5): the view's (the person's), never the song's
          livelook::setRowScale(a, resizing, nh, false);
        } else if (std::fabs(nh - s.tracks[static_cast<size_t>(resizing)].height) > 0.01f) {
          a.doc.editPlaylistTrack(resizing, "resize playlist track", [nh](PlaylistTrack& t) { t.height = nh; });
        }
      } else {
        if (liveLook) {
          livelook::setRowScale(a, resizing, livelook::rowScale(a, resizing, s.tracks[static_cast<size_t>(resizing)].height), true);
        } else {
          a.doc.endGroup();
        }
        resizing = -1;
      }
      lClick = false;
    } else if (headResize >= 0 && headLed < 0) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      a.setHint("Drag to resize the track");
      if (lClick) {
        resizing = headResize;
        resizeY0 = m.y;
        resizeH0 = liveLook ? livelook::rowScale(a, headResize, s.tracks[static_cast<size_t>(headResize)].height)
                            : s.tracks[static_cast<size_t>(headResize)].height;
        if (!liveLook) {
          a.doc.beginGroup("resize playlist track");
        }
        lClick = false;
      }
    }
  }
  {  // GitHub #46: track reorder (see HeadDrag above).
    // Where a drop at screen y lands: before the track under the pointer, or after it from its middle
    // on (past the members of a folded group).
    auto dropBefore = [&](float y) {
      if (nTracks == 0 || y >= yOf(nTracks)) return nTracks;
      const int k = trackAt(y);
      int before = y >= yOf(k) + hOf(k) * 0.5f ? k + 1 : k;
      while (before < nTracks && hOf(before) < 1) ++before;
      return before;
    };
    if (hd.track >= 0) {
      if (hd.track >= nTracks) hd = HeadDrag{};
    }
    if (hd.track >= 0) {
      const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
      if (down && !hd.active && !hd.cancelled && std::hypot(m.x - hd.at.x, m.y - hd.at.y) > 4.0f) {
        hd.active = true;
        hd.tracks = playlistTrackSelection(a, hd.track);
        hd.moving = trackorder::withGroups(s, hd.tracks);
      }
      if (down && hd.active && !hd.cancelled &&
          (keys::pressedHeld("playlist.cancelDrag") || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        hd.cancelled = true;
        rClick = false;
        a.setHint(i18n::tr("Move cancelled"));
      }
      if (down && hd.active && !hd.cancelled) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (m.y < gMin.y + 14) st.scrollY = std::max(0.0f, st.scrollY - 6.0f);  // near an edge: the view follows
        else if (m.y > gMax.y - 14) st.scrollY += 6.0f;
        hd.before = dropBefore(std::clamp(m.y, gMin.y, gMax.y - 1));
        devGeom.dragBefore = hd.before;
        const float ly = std::round(hd.before >= nTracks ? yOf(nTracks) : yOf(hd.before));
        ImDrawList* fg = ImGui::GetWindowDrawList();
        fg->PushClipRect(ImVec2(rowX0, gMin.y - 2), ImVec2(rowX1, gMax.y + 2), true);
        fg->AddRectFilled(ImVec2(rowX0 + 2, ly - 1.5f), ImVec2(rowX1, ly + 1.5f), P.hot, 1.0f);
        fg->AddTriangleFilled(ImVec2(headX0 + 2, ly - 5), ImVec2(headX0 + 8, ly), ImVec2(headX0 + 2, ly + 5), P.hot);
        fg->PopClipRect();
        const int count = static_cast<int>(hd.moving.size());
        a.setHint(count > 1 ? i18n::tr("Drop to move the tracks here \xC2\xB7 Esc: cancel") : i18n::tr("Drop to move the track here \xC2\xB7 Esc: cancel"));
      }
      if (!down) {
        if (hd.active && !hd.cancelled && hd.before >= 0) {
          std::vector<int> now;
          if (liveLook) a.doc.beginGroup("move tracks");
          // N53-03: in Clips a track's lanes move with it, and nothing lands between a track and its lanes.
          const std::vector<int> mv = liveLook ? trackmodes::withLanes(s, hd.tracks) : hd.tracks;
          if (trackorder::movePlaylistTracks(a.doc, mv, liveLook ? trackmodes::pastLanes(s, hd.before) : hd.before, &now)) {
            if (liveLook)  // (the lanes went along, unselected)
              now.erase(std::remove_if(now.begin(), now.end(), [&a](int t) { return trackmodes::isAutomationLane(a.doc.song(), t); }), now.end());
            st.trackSel = std::set<int>(now.begin(), now.end());
            if (!now.empty()) selAnchor = now.front();
            if (liveLook) trackmodes::syncGroupRouting(a.doc);  // checkpoint 52 (N51-06): in or out of a group, the routing follows
          }
          if (liveLook) a.doc.endGroup();
        } else if (!hd.active && !hd.cancelled && hd.handle) {
          renaming = hd.track;
          ImGui::OpenPopup("##trmenu");
        } else if (!hd.active && !hd.cancelled && !io.KeyCtrl && !io.KeySuper && !io.KeyShift) {
          st.trackSel = {hd.track};  // a plain click on one of several selected headers keeps just it
        }
        hd = HeadDrag{};
      }
      lClick = false;
    } else if (lClick && headHandle >= 0) {
      hd = HeadDrag{};
      hd.track = headHandle, hd.handle = true, hd.at = m;
      if (!st.trackSel.count(headHandle)) st.trackSel = {headHandle}, selAnchor = headHandle;
      lClick = false;
    }
  }
  {  // Logic look: the header's pan knob (drag up / down, double-click centres) and R (arm)
    auto panOf = [&](int tr) -> std::optional<ParamId> {
      if (tr < 0 || tr >= nTracks) return std::nullopt;
      if (const Channel* c = trackmodes::linkedChannel(s, tr)) return channelParam(c->id, ChannelParam::Pan);
      if (const MixerTrack* mt = trackmodes::linkedMixer(s, tr)) return mixerParam(mt->id, MixerParam::Pan);
      if (const MixerTrack* own = lg ? logicplug::trackStrip(a, tr) : nullptr) return mixerParam(own->id, MixerParam::Pan);  // checkpoint 47
      return std::nullopt;
    };
    if (inHead && headPan >= 0 && lClick) {
      if (const auto pid = panOf(headPan)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          if (const auto d = describeParam(s, *pid); d && getParam(s, *pid) != std::optional<double>(d->def)) a.doc.setParam(*pid, d->def, false);
        } else if (const auto v = getParam(s, *pid)) {
          panDrag = headPan, panFrom = *v, panY = m.y, knobMoved = false;
        }
      }
      lClick = false;
    }
    if (panDrag >= 0) {
      const auto pid = panOf(panDrag);
      if (!pid || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        panDrag = -1;
        a.doc.endGesture();
      } else if (const auto d = describeParam(s, *pid)) {
        const double span = d->max - d->min;
        // R13-08: a press that has not moved writes nothing (no undo step, the project stays unmodified).
        knobMoved = knobMoved || m.y != panY;
        const double v = d->clamp(panFrom + (panY - m.y) * (io.KeyShift ? 0.001 : 0.006) * span);
        if (const auto cur = getParam(s, *pid); knobMoved && cur && *cur != v) a.doc.setParam(*pid, v, true);
      }
    }
    auto volOf = [&](int tr) -> std::optional<ParamId> {
      if (tr < 0 || tr >= nTracks) return std::nullopt;
      if (const Channel* c = trackmodes::linkedChannel(s, tr)) return channelParam(c->id, ChannelParam::Volume);
      if (const MixerTrack* mt = trackmodes::linkedMixer(s, tr)) return mixerParam(mt->id, MixerParam::Volume);
      if (const MixerTrack* own = lg ? logicplug::trackStrip(a, tr) : nullptr) return mixerParam(own->id, MixerParam::Volume);  // checkpoint 47
      return std::nullopt;
    };
    if (inHead && headVol >= 0 && lClick) {
      if (const auto pid = volOf(headVol)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          if (const auto d = describeParam(s, *pid); d && getParam(s, *pid) != std::optional<double>(d->def)) a.doc.setParam(*pid, d->def, false);
        } else if (const auto v = getParam(s, *pid)) {
          if (const auto d = describeParam(s, *pid)) volDrag = headVol, volFrom = d->toNormalized(*v), volX = m.x, knobMoved = false;
        }
      }
      lClick = false;
    }
    if (volDrag >= 0) {
      const auto pid = volOf(volDrag);
      if (!pid || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        volDrag = -1;
        a.doc.endGesture();
      } else if (const auto d = describeParam(s, *pid)) {
        // R13-08: a click without a drag writes nothing (the normalized round trip moved 0.8 to
        // 0.79999995: an undo step and a modified project from a click).
        knobMoved = knobMoved || m.x != volX;
        const double n = std::clamp(volFrom + (m.x - volX) / std::max(volW, 20.0f) * (io.KeyShift ? 0.2 : 1.0), 0.0, 1.0);
        const double v = d->clamp(d->fromNormalized(n));
        if (const auto cur = getParam(s, *pid); knobMoved && cur && *cur != v) a.doc.setParam(*pid, v, true);
      }
    }
    if (inHead && headIcon_ >= 0 && lClick) {  // R01-23: the icon picker
      iconMenuTrack = headIcon_;
      ImGui::OpenPopup("##lgicon");
      lClick = false;
    }
    if (inHead && headPower >= 0 && lClick) {  // Logic look: the power button
      const Channel* pc = trackmodes::linkedChannel(s, headPower);
      const MixerTrack* pm = trackmodes::linkedMixer(s, headPower);
      if (!pc && !pm) pc = logicplug::trackSound(s, headPower, nullptr);  // R09-06: the sound its regions play
      if (pc || pm) {
        const freeze::Unit u = pc ? freeze::channelUnit(pc->id) : freeze::mixerUnit(pm->id);
        const bool sleep = pc ? !pc->hibernate : !pm->hibernate;
        a.later([&a, u, sleep] { freezeui::setHibernate(a, {u}, sleep); });
      }
      lClick = false;
    }
    if (inHead && (headArm >= 0 || headMon >= 0) && lClick) {
      const int tr = headArm >= 0 ? headArm : headMon;
      const MixerTrack* mt = trackmodes::linkedMixer(s, tr);
      if (mt && s.tracks[static_cast<size_t>(tr)].mode == TrackMode::Audio) {  // an audio track: its strip's arm / monitoring
        if (headArm >= 0) a.later([&a, tr, on = !mt->armed] { trackmodes::setArmed(a.doc, tr, on); });
        else
          a.later([&a, tr, on = mt->monitor != static_cast<uint8_t>(InputMonitor::On)] {
            trackmodes::setMonitor(a.doc, tr, on ? InputMonitor::On : InputMonitor::Auto);
          });
      } else if (const Channel* c = logicplug::midiRecordable(a, tr) ? logicplug::trackSound(a, tr) : nullptr) {
        // R10-03: an instrument track: a performance switch of this session (never saved). Arming
        // selects the track and its sound, so what is played next is heard and recorded there.
        std::set<ObjectId>& set = headArm >= 0 ? a.playlist.midiArm : a.playlist.midiMonitor;
        const bool on = !set.count(c->id);
        if (on) set.insert(c->id);
        else set.erase(c->id);
        if (headArm >= 0 && on) {
          st.trackSel = {tr};
          a.selectChannel(c->id);
        }
      }
      lClick = false;
    }
    if (inHead && headLed >= 0 && headLedSolo && lClick) {  // R05-03: S solos the track's channel strip
      lClick = false;
      if (const MixerTrack* strip = logicplug::trackStrip(s, headLed)) {
        const ObjectId id = strip->id;
        const bool to = !strip->solo;
        a.doc.editMixerTrack(id, to ? "solo track" : "unsolo track", [to](MixerTrack& m) { m.solo = to, m.soloAlone = false; });
      } else {
        a.setHint(i18n::tr("This track has no channel strip to solo yet"));
      }
    }
  }
  if (inHead && headModeCtl && lClick) {  // PL-08: arm / monitoring / input / instrument channel
    if (headModeCtl == 3) inputTrack = headModeTrack;
    trackui::headerClick(a, headModeTrack, headModeCtl);
    lClick = false;
  }
  if (inHead && liveHeadTrack >= 0 && (lClick || rClick) && livelook::clickHeader(a, liveHeadTrack, liveHeadPart, rClick && !lClick))
    lClick = rClick = false;  // K-112c: a Live header's activator, solo, arm, volume
  if (inHead && headLed >= 0 && (lClick || rClick)) {  // mute switch; right-click: solo
    if (lClick) {
      a.doc.editPlaylistTrack(headLed, "mute playlist track", [](PlaylistTrack& t) { t.muted = !t.muted; });
    } else {
      bool solo = !s.tracks[static_cast<size_t>(headLed)].muted;
      for (int k = 0; k < nTracks && solo; ++k) solo = k == headLed || s.tracks[static_cast<size_t>(k)].muted;
      const int keep = headLed;
      a.doc.editSong(solo ? "unsolo playlist track" : "solo playlist track", [keep, solo](Song& x) {
        for (size_t k = 0; k < x.tracks.size(); ++k) x.tracks[k].muted = solo ? false : static_cast<int>(k) != keep;
      });
    }
    lClick = rClick = false;
  }
  // Live mode: clicking a clip of the live area launches it (held = gate pads). K-287: not in the Live
  // layout, whose session grid launches; its arrangement edits those clips as any other.
  static ObjectId liveHeld = kNoId;
  const bool launchHere = liveMode && !liveLook;
  if (launchHere && lClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.launchClip(hc->id);
    liveHeld = hc->id;
    lClick = false;
  } else if (launchHere && rClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.stopLiveTrack(hc->track);
    rClick = false;
  }
  if (liveHeld != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(liveHeld);
    liveHeld = kNoId;
  }
  // K-112B (checkpoint 46): a click on the closed Automation folder's header opens it; the open
  // folder's arrow closes it (a view setting, never in the project).
  if ((lClick || rClick) && inHead && foldShut && trackAt(m.y) == foldHead) {
    if (lClick) logicplug::setAutoFolderOpen(a, true);
    lClick = rClick = false;
  }
  // Group fold arrow in the track header.
  if (lClick && inHead) {
    int tr = trackAt(m.y);
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    float ax = headX0 + 2 + 8 + 4 + kHeadNumW + 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    if (t.groupSize > 0 && m.x >= ax - 6 && m.x < ax + 8 && m.y < yOf(tr) + 20) {
      a.doc.editPlaylistTrack(tr, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
      lClick = false;
    }
  }
  // GitHub #46: a press on a header's body selects it and may start a drag (see HeadDrag); a
  // double-click on the name still renames (the double-click handling below sees the click).
  if (lClick && inHead && hd.track < 0 && headResize < 0 && headLed < 0 && headPlus < 0 && !headModeCtl && m.y >= gMin.y && m.y < yOf(nTracks)) {
    const int tr = trackAt(m.y);
    if (io.KeyCtrl || io.KeySuper) {
      if (!st.trackSel.erase(tr)) st.trackSel.insert(tr);
      selAnchor = tr;
    } else if (io.KeyShift && selAnchor >= 0 && selAnchor < nTracks) {
      for (int k = std::min(selAnchor, tr); k <= std::max(selAnchor, tr); ++k)
        if (hOf(k) >= 1) st.trackSel.insert(k);
    } else if (!st.trackSel.count(tr)) {
      st.trackSel = {tr};
      selAnchor = tr;
    }
    hd = HeadDrag{};
    hd.track = tr, hd.at = m;
    if (io.KeyCtrl || io.KeySuper || io.KeyShift) hd.cancelled = true;  // a selection click, never a drag
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) hd = HeadDrag{};  // renames / shows the instrument below
    if (lg && !io.KeyAlt) {
      // R03-02: as the reference, selecting a track selects its regions (Option-click keeps the
      // region selection), so the inspector and the editors move to that track.
      st.selected.clear();
      for (const Clip& c : s.playlist)
        if (st.trackSel.count(c.track) && c.kind != ClipKind::Automation) st.selected.insert(c.id);
    }
  }
  // J-11: locked tracks keep their clips (only selection, play, zoom and menus work there).
  std::vector<char> lockedTracks(static_cast<size_t>(nTracks), 0);
  for (int k = 0; k < nTracks; ++k) lockedTracks[static_cast<size_t>(k)] = s.tracks[static_cast<size_t>(k)].locked ? 1 : 0;
  auto trackLocked = [&](int tr) { return tr >= 0 && tr < nTracks && lockedTracks[static_cast<size_t>(tr)]; };
  const bool editTool = clipTool != Tool::Select && st.tool != Tool::Play && st.tool != Tool::Zoom && st.tool != Tool::Marquee && !ctlK;
  if (inGrid && editTool && trackLocked(trackAt(m.y))) {
    if (hovered) a.setHint("This track is locked (track menu \xE2\x86\x92 Lock)");
    if (lClick && hoverMenuGlyph == kNoId) lClick = false;
    if (rClick && !lg && !(hc && clipheader::opensMenu(hoverZone))) rClick = false;  // only the clip menu opens there
  }
  // Fade handle drags (J-8; J-38: tension, crossfades): one undo step per drag.
  // Dragging a length stores the fade on the clip (it stops following the
  // overlap); dragging a ring bends the curve, mirrored onto the crossfade
  // partner so the pair keeps its level.
  static ObjectId fadeMenuClip = kNoId;
  static int fadeMenuIn = 1;
  // J-142: the selected clips a fade drag moves with the clip under the pointer
  // (captured at the press; a single clip when it is not part of a selection).
  static clipmulti::Snapshot fadeDragSnap;
  const uint64_t fadeKey = 0x46414445ull ^ (static_cast<uint64_t>(fadeDragClip) << 8);
  if (fadeDragClip != kNoId) {
    const Clip* fc = s.clip(fadeDragClip);
    const int fi = s.clipIndex(fadeDragClip);
    if (fc && fi >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ClipFades fd = static_cast<size_t>(fi) < ccache.fades.size() ? ccache.fades[static_cast<size_t>(fi)] : ClipFades{};
      const int kind = fadeDragKind;
      const ObjectId id = fadeDragClip, mirror = fadeDragMirror;
      char b[96];
      const clipmulti::Entry* anchor = fadeDragSnap.find(id);
      const size_t many = fadeDragSnap.clips.size();
      if (kind <= 2 && many > 1 && anchor) {
        // J-142: every selected clip's fade grows by the time this one's does
        // (from the pointer, before this clip's own limits), each clamped to
        // its own length.
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const double sec0 = tempoMap.secondsAt(static_cast<double>(fc->start));
        const double dur = tempoMap.secondsAt(static_cast<double>(fc->start + s.clipLength(*fc))) - sec0;
        const double at = tempoMap.secondsAt(static_cast<double>(std::max<Tick>(0, tickAt(m.x)))) - sec0;
        const double raw = kind == 1 ? at : dur - at;
        clipmulti::offset(a.doc, kind == 1 ? "clip fade in" : "clip fade out", fadeDragSnap, raw - anchor->base, fadeKey);
        const Clip* now = s.clip(id);
        const double v = now ? (kind == 1 ? now->fadeIn : now->fadeOut) : 0.0;
        std::snprintf(b, sizeof b, "Fade %s: %.0f ms, %+.0f ms on %zu clips", kind == 1 ? "in" : "out", v * 1000.0,
                      (std::clamp(raw, anchor->lo, anchor->hi) - anchor->base) * 1000.0, many);
      } else if (kind <= 2) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const double sec0 = tempoMap.secondsAt(static_cast<double>(fc->start));
        const double dur = tempoMap.secondsAt(static_cast<double>(fc->start + s.clipLength(*fc))) - sec0;
        const double at = std::clamp(tempoMap.secondsAt(static_cast<double>(std::max<Tick>(0, tickAt(m.x)))) - sec0, 0.0, dur);
        double v = std::max(0.0, kind == 1 ? std::min(at, dur - fd.out.len) : std::min(dur - at, dur - fd.in.len));
        if (fadeToolT0 >= 0) {  // R07-04: the Fade tool: as long as the span dragged over the region's start or end
          // R08-11: only the part of the drag inside the region counts (a drag past its edge adds nothing).
          const Tick r0 = fc->start, r1 = fc->start + s.clipLength(*fc);
          const double span = std::fabs(tempoMap.secondsAt(static_cast<double>(std::clamp<Tick>(tickAt(m.x), r0, r1))) -
                                        tempoMap.secondsAt(static_cast<double>(std::clamp<Tick>(fadeToolT0, r0, r1))));
          v = std::clamp(span, 0.0, std::max(0.0, dur - (kind == 1 ? fd.out.len : fd.in.len)));
        }
        a.doc.editClip(id, kind == 1 ? "clip fade in" : "clip fade out", [kind, v](Clip& x) {
          if (kind == 1) x.fadeIn = v, x.fadeInManual = true;
          else x.fadeOut = v, x.fadeOutManual = true;
        }, fadeKey);
        std::snprintf(b, sizeof b, "Fade %s: %.0f ms (set by hand; right-click the handle: automatic, curve)", kind == 1 ? "in" : "out", v * 1000.0);
      } else if (many > 1 && anchor) {
        // J-142: every selected clip's curve bends by the same amount on the
        // handle's scale (each tied crossfade partner mirrors its clip).
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const double d = -static_cast<double>(m.y - fadeDragY0) / 70.0;
        clipmulti::offset(a.doc, "fade curve", fadeDragSnap, d, fadeKey, 0.03);
        std::snprintf(b, sizeof b, "Fade curve: %+.0f %% on %zu clips", d * 100.0, many);
      } else {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        // J-43: drag up = louder (FL's positive tension); the handle moves on FL's
        // logarithmic tension scale, with a detent at the plain curve.
        const float t = std::clamp(fadeDragT0 - (m.y - fadeDragY0) / 70.0f, -1.0f, 1.0f);
        const float uq = std::fabs(t) < 0.03f ? 0.0f : t;
        const float tq = fadeTensionFromUi(uq);
        a.doc.editClips("fade curve", [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (x.id == id) (kind == 3 ? x.fadeInTension : x.fadeOutTension) = tq;
            if (mirror != kNoId && x.id == mirror) (kind == 3 ? x.fadeOutTension : x.fadeInTension) = -tq;
          }
        }, fadeKey);
        std::snprintf(b, sizeof b, "Fade curve: %+.0f %%%s", uq * 100.0f, mirror != kNoId ? "  (crossfade: the other clip mirrors it)" : "");
      }
      a.setHint(b);
    } else {
      a.doc.endGesture();
      fadeDragClip = kNoId;
    }
    lClick = false;
  } else if (fadeHoverClip != kNoId) {
    ImGui::SetMouseCursor(fadeHoverKind <= 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    a.setHint(fadeHoverKind <= 2 ? "Drag: fade length (sets it by hand) \xC2\xB7 right-click: fade menu (automatic crossfade, curve shape)"
                                 : "Drag up / down: fade curve \xC2\xB7 double-click: plain curve \xC2\xB7 right-click: fade menu");
    if (lClick) {
      const Clip* hcf = s.clip(fadeHoverClip);
      if (fadeHoverKind >= 3 && hcf && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
          clipmulti::isMulti(s, st.selected, fadeHoverClip, clipmulti::Param::FadeInCurve)) {
        // J-142: a reset reaches every selected clip (and each tied partner).
        const clipmulti::Param fp = fadeHoverKind == 3 ? clipmulti::Param::FadeInCurve : clipmulti::Param::FadeOutCurve;
        clipmulti::setAll(a.doc, "plain fade curve", clipmulti::capture(s, clipui::targets(a, fadeHoverClip, fp), fp, fadeHoverClip), 0.0);
      } else if (fadeHoverKind >= 3 && hcf && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const ObjectId id = fadeHoverClip, mirror = fadeHoverMirror;
        const int kind = fadeHoverKind;
        a.doc.editClips("plain fade curve", [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (x.id == id) (kind == 3 ? x.fadeInTension : x.fadeOutTension) = 0;
            if (mirror != kNoId && x.id == mirror) (kind == 3 ? x.fadeOutTension : x.fadeInTension) = 0;
          }
        });
      } else {
        fadeDragClip = fadeHoverClip;
        fadeDragKind = fadeHoverKind;
        fadeToolT0 = -1;
        fadeDragMirror = fadeHoverMirror;
        fadeDragY0 = m.y;
        fadeDragT0 = hcf ? fadeTensionToUi(fadeHoverKind == 3 ? hcf->fadeInTension : hcf->fadeOutTension) : 0.0f;
        const clipmulti::Param fp = fadeHoverKind == 1 ? clipmulti::Param::FadeIn
                                    : fadeHoverKind == 2 ? clipmulti::Param::FadeOut
                                    : fadeHoverKind == 3 ? clipmulti::Param::FadeInCurve
                                                         : clipmulti::Param::FadeOutCurve;
        fadeDragSnap = clipmulti::capture(s, clipui::targets(a, fadeHoverClip, fp), fp, fadeHoverClip);
      }
      lClick = false;
    }
    if (rClick) {
      fadeMenuClip = fadeHoverClip;
      fadeMenuIn = fadeHoverKind == 1 || fadeHoverKind == 3;
      ImGui::OpenPopup("##fademenu");
      rClick = false;
    }
  }
  // J-79: volume line drags. Up = louder, a fixed step in dB per pixel with a
  // short detent at 0 dB (Shift: fine, Alt: no detent), double-click resets
  // to 0 dB, right-click opens the volume menu. One undo step per drag.
  static ObjectId gainMenuClip = kNoId;
  static float gainClipboard = 1.0f;
  static float gainTypeDb = 0.0f;
  // J-142: the selected clips a volume drag moves with the clip under the pointer.
  static clipmulti::Snapshot gainDragSnap;
  const uint64_t gainKey = 0x4741494Eull ^ (static_cast<uint64_t>(gainDragClip) << 8);
  if (gainDragClip != kNoId) {
    const Clip* gc = s.clip(gainDragClip);
    if (gc && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      const bool fine = io.KeyShift, free = io.KeyAlt;
      const double up = static_cast<double>(gainDragY0 - m.y);
      if (fine != gainDrag.fine || free != gainDrag.free)  // a modifier changed: continue from here at the new rate
        gainDrag.begin(std::max(clipgain::kMinDb - 0.1, clipgain::toDb(gc->gain)), fine, free, up);
      const float g = clipgain::fromDb(gainDrag.move(up));
      const clipmulti::Entry* anchor = gainDragSnap.find(gc->id);
      if (gainDragSnap.clips.size() > 1 && anchor) {
        // J-142: every selected clip moves by the same number of dB (from the
        // pointer, past this clip's own limits), each clamped at its own.
        const double d = gainDrag.dbUnbounded() - anchor->base;
        clipmulti::offset(a.doc, "clip volume", gainDragSnap, d, gainKey);
        char b[48];
        std::snprintf(b, sizeof b, "%+.1f dB", std::clamp(anchor->base + d, anchor->lo, anchor->hi) - anchor->base);
        a.setHint(i18n::tr("Clip volume") + std::string(": ") + clipgain::label(g) + "   " + b + "   " +
                  i18n::tr("(every selected clip by the same amount)"));
      } else {
        if (g != gc->gain) {
          const ObjectId id = gc->id;
          a.doc.editClip(id, "clip volume", [g](Clip& x) { x.gain = g; }, gainKey);
        }
        a.setHint(i18n::tr("Clip volume") + std::string(": ") + clipgain::label(g) + "   " +
                  i18n::tr("(Shift: fine, Alt: no stop at 0 dB, double-click: 0 dB)"));
      }
    } else {
      a.doc.endGesture();
      gainDragClip = kNoId;
    }
    lClick = false;
  } else if (gainHoverClip != kNoId) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (const Clip* hc0 = s.clip(gainHoverClip))
      a.setHint(i18n::tr("Clip volume") + std::string(": ") + clipgain::label(hc0->gain) + "   " +
                i18n::tr("drag up / down \xC2\xB7 double-click: 0 dB \xC2\xB7 right-click: volume menu"));
    if (lClick) {
      const Clip* hcg = s.clip(gainHoverClip);
      const std::vector<ObjectId> gainTargets = clipui::targets(a, gainHoverClip, clipmulti::Param::GainDb);
      if (hcg && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {  // J-142: every selected clip back to 0 dB
        clipmulti::setAll(a.doc, "reset clip volume", clipmulti::capture(s, gainTargets, clipmulti::Param::GainDb, gainHoverClip), 0.0);
      } else if (hcg) {
        gainDragClip = gainHoverClip;
        gainDragY0 = m.y;
        gainDragSnap = clipmulti::capture(s, gainTargets, clipmulti::Param::GainDb, gainHoverClip);
        gainDrag.unbounded = gainDragSnap.clips.size() > 1;  // J-142: the others keep moving past this clip's limits
        gainDrag.begin(std::max(clipgain::kMinDb - 0.1, clipgain::toDb(hcg->gain)), io.KeyShift, io.KeyAlt);
        gainGlideClip = hcg->id;
        gainGlideDb = gainDrag.db();
      }
      lClick = false;
    }
    if (rClick) {
      gainMenuClip = gainHoverClip;
      ImGui::OpenPopup("##gainmenu");
      rClick = false;
    }
  }
  if (ImGui::BeginPopup("##gainmenu")) {
    const Clip* gc = s.clip(gainMenuClip);
    if (!gc) {
      ImGui::CloseCurrentPopup();
    } else {
      const ObjectId id = gc->id;
      // J-142: with the clip in a multi-selection, reset and paste set every
      // selected clip; a typed value is an offset (the clicked clip gets the
      // value, the others move by the same number of dB).
      const std::vector<ObjectId> gainTargets = clipui::targets(a, id, clipmulti::Param::GainDb);
      auto gainSnap = [&] { return clipmulti::capture(s, gainTargets, clipmulti::Param::GainDb, id); };
      ImGui::TextDisabled("%s %s", i18n::tr("Clip volume"), clipgain::label(gc->gain).c_str());
      if (gainTargets.size() > 1) ImGui::TextDisabled(i18n::tr("%d selected clips"), static_cast<int>(gainTargets.size()));
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Reset to 0 dB"))) clipmulti::setAll(a.doc, "reset clip volume", gainSnap(), 0.0);
      if (ImGui::MenuItem(i18n::tr("Normalize"))) {
        a.doc.editSong("normalize clip", [id](Song& x) {
          if (normalizeClip(x, id) > 0)
            if (Clip* k = x.clip(id)) k->gain = clipgain::clampGain(k->gain);
        });
      }
      std::vector<ObjectId> selAudio;
      for (ObjectId sid : st.selected)
        if (const Clip* k = s.clip(sid); k && k->kind == ClipKind::Audio) selAudio.push_back(sid);
      if (ImGui::MenuItem(i18n::tr("Normalize the selection together"), nullptr, false, selAudio.size() > 1)) {
        // One gain for every selected audio clip: the loudest one peaks at 0 dBFS.
        a.doc.editSong("normalize clips together", [&](Song& x) {
          std::vector<std::pair<ObjectId, float>> before;
          float common = clipgain::kMaxGain;
          for (ObjectId sid : selAudio) {
            const Clip* k = x.clip(sid);
            if (!k) continue;
            before.push_back({sid, k->gain});
            if (const float g = normalizeClip(x, sid); g > 0) common = std::min(common, g);
          }
          for (const auto& [sid, g0] : before)
            if (Clip* k = x.clip(sid)) k->gain = common < clipgain::kMaxGain ? clipgain::clampGain(common) : g0;
        });
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Copy value"))) gainClipboard = gc->gain;
      if (ImGui::MenuItem(i18n::tr("Paste value")))
        clipmulti::setAll(a.doc, "paste clip volume", gainSnap(), std::max(clipgain::kMinDb - 0.1, clipgain::toDb(gainClipboard)));
      if (ImGui::BeginMenu(i18n::tr("Type in value"))) {
        if (ImGui::IsWindowAppearing()) gainTypeDb = static_cast<float>(std::max(clipgain::kMinDb, clipgain::toDb(gc->gain)));
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputFloat("dB##gaintype", &gainTypeDb, 0.5f, 3.0f, "%.1f", ImGuiInputTextFlags_EnterReturnsTrue)) {
          const double db = std::clamp(static_cast<double>(gainTypeDb), clipgain::kMinDb, clipgain::kMaxDb);
          const clipmulti::Snapshot snap = gainSnap();
          if (const clipmulti::Entry* e = snap.find(id))  // the typed value for this clip, the same difference for the others
            clipmulti::offset(a.doc, "clip volume", snap, (db <= clipgain::kMinDb ? clipgain::kMinDb - 0.1 : db) - e->base);
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##fademenu")) {
    const Clip* fc = s.clip(fadeMenuClip);
    const int fi = s.clipIndex(fadeMenuClip);
    if (!fc || fi < 0 || static_cast<size_t>(fi) >= ccache.fades.size()) {
      ImGui::CloseCurrentPopup();
    } else {
      const bool in = fadeMenuIn != 0;
      const EdgeFade e = in ? ccache.fades[static_cast<size_t>(fi)].in : ccache.fades[static_cast<size_t>(fi)].out;
      const ObjectId id = fc->id, partner = e.crossfade ? e.partner : kNoId;
      // The partner edge is tied when it fades over the same span (both follow the overlap).
      bool tied = false;
      if (partner != kNoId)
        if (const int pi = s.clipIndex(partner); pi >= 0 && static_cast<size_t>(pi) < ccache.fades.size()) {
          const EdgeFade& o = in ? ccache.fades[static_cast<size_t>(pi)].out : ccache.fades[static_cast<size_t>(pi)].in;
          tied = o.len > 0 && std::fabs(o.len - e.len) < 1e-6;
        }
      // J-142: with the clip in a multi-selection the menu sets that edge on
      // every selected audio clip (each tied partner follows its own clip).
      const std::vector<ObjectId> edgeClips = clipui::targets(a, id, in ? clipmulti::Param::FadeIn : clipmulti::Param::FadeOut);
      auto editEdge = [&](const char* label, const std::function<void(Clip&, bool /*in*/)>& fn, bool alsoPartner) {
        std::set<ObjectId> ids(edgeClips.begin(), edgeClips.end()), partners;
        ids.insert(id);
        if (alsoPartner)
          for (ObjectId k : ids) {
            const int ki = s.clipIndex(k);
            if (ki < 0 || static_cast<size_t>(ki) >= ccache.fades.size()) continue;
            const EdgeFade& ke = in ? ccache.fades[static_cast<size_t>(ki)].in : ccache.fades[static_cast<size_t>(ki)].out;
            if (!ke.crossfade || ke.partner == kNoId) continue;
            const int pi = s.clipIndex(ke.partner);
            if (pi < 0 || static_cast<size_t>(pi) >= ccache.fades.size()) continue;
            const EdgeFade& o = in ? ccache.fades[static_cast<size_t>(pi)].out : ccache.fades[static_cast<size_t>(pi)].in;
            if (o.len > 0 && std::fabs(o.len - ke.len) < 1e-6) partners.insert(ke.partner);
          }
        a.doc.editClips(label, [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (ids.count(x.id)) fn(x, in);
            if (partners.count(x.id)) fn(x, !in);
          }
        });
      };
      ImGui::TextDisabled("%s", in ? "Fade in" : "Fade out");
      if (e.crossfade && ImGui::MenuItem(i18n::tr("Automatic crossfade"), nullptr, e.automatic, s.autoCrossfades)) {
        if (e.automatic) {  // keep each clip's current length, set by hand from now on
          std::map<ObjectId, double> lens;
          for (size_t k = 0; k < s.playlist.size() && k < ccache.fades.size(); ++k)
            lens[s.playlist[k].id] = in ? ccache.fades[k].in.len : ccache.fades[k].out.len;
          editEdge("fade set by hand", [&lens](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = lens[x.id], (i ? x.fadeInManual : x.fadeOutManual) = true; }, false);
        }
        else
          editEdge("automatic crossfade", [](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = 0, (i ? x.fadeInManual : x.fadeOutManual) = false; }, false);
      }
      if (!e.crossfade || !s.autoCrossfades) ui::Hint(!e.crossfade ? "Overlap another audio clip on the track to crossfade with it" : "Automatic crossfades are off for this project (playlist menu)");
      if (ImGui::MenuItem(i18n::tr("No fade"), nullptr, e.len <= 0))
        editEdge("remove fade", [](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = 0, (i ? x.fadeInManual : x.fadeOutManual) = true; }, false);
      if (ImGui::MenuItem(i18n::tr("Reset")))
        editEdge("reset fade", [](Clip& x, bool i) {
          if (i) x.fadeIn = 0, x.fadeInManual = false, x.fadeInShape = 0, x.fadeInTension = 0;
          else x.fadeOut = 0, x.fadeOutManual = false, x.fadeOutShape = 0, x.fadeOutTension = 0;
        }, false);
      ui::Hint("Back to the defaults: automatic length (a crossfade where the clip overlaps another), smooth curve, no bend");
      ImGui::Separator();
      ImGui::TextDisabled("%s", tied ? "Crossfade curve (both clips)" : "Curve");
      const uint8_t cur = in ? fc->fadeInShape : fc->fadeOutShape;
      for (int k = 0; k < kFadeShapeCount; ++k) {
        std::string label = fadeShapeName(static_cast<FadeShape>(k));
        if (k == 0) label += "  (default)";
        if (ImGui::MenuItem(label.c_str(), nullptr, (cur == 1 ? 1 : 0) == k))
          editEdge("fade curve shape", [k](Clip& x, bool i) { (i ? x.fadeInShape : x.fadeOutShape) = static_cast<uint8_t>(k); }, tied);
      }
      if (ImGui::MenuItem(i18n::tr("Plain curve (no bend)"), nullptr, false, e.tension != 0.0f))
        editEdge("plain fade curve", [](Clip& x, bool i) { (i ? x.fadeInTension : x.fadeOutTension) = 0; }, tied);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Automatic crossfades in this project"), nullptr, s.autoCrossfades))
        a.doc.editSong(s.autoCrossfades ? "automatic crossfades off" : "automatic crossfades on", [](Song& x) { x.autoCrossfades = !x.autoCrossfades; });
    }
    ImGui::EndPopup();
  }
  static int menuMarker = -1;
  if (devMenuTake("track") && nTracks > 0) {  // dev: the header menu of the first selected clip's track
    const Clip* c = st.selected.empty() ? nullptr : s.clip(*st.selected.begin());
    renaming = c ? c->track : 0;
    ImGui::OpenPopup("##trmenu");
  }
  if (devMenuTake("region") && !s.playlist.empty()) {  // dev: the first selected clip's menu
    menuClip = st.selected.empty() ? s.playlist[0].id : *st.selected.begin();
    ImGui::OpenPopup("##clipmenu");
  }
  if (inRuler && rClick && hoverMarker >= 0) {  // right-click a marker: its menu
    menuMarker = hoverMarker;
    ImGui::OpenPopup("##markmenu");
    rClick = false;
  }
  if (inGrid && (lClick || rClick) && hoverMenuGlyph != kNoId) {  // the clip's title glyph: clip menu
    menuClip = hoverMenuGlyph;
    ImGui::OpenPopup("##clipmenu");
    lClick = rClick = false;
  }
  // K-112b: in the Tracks layout the empty space below the last track makes nothing: a click there
  // clears the selection, a double-click opens the New tracks dialog (as the mapped main window does).
  if (lClick && (inGrid || inHead) && nTracks > 0 && m.y >= yOf(nTracks) && hoverClip == kNoId && logicplug::active(a)) {
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) newtracks::open();
    else if (!io.KeyShift && !io.KeyCtrl) st.selected.clear(), st.trackSel.clear();
    lClick = false;
  }
  const pltools::Source src = currentSource(a);
  const bool inMarquee = lg && st.mqB > st.mqA && inGrid && trackAt(m.y) >= st.mqT0 && trackAt(m.y) <= st.mqT1 &&
                         tickAt(m.x) >= st.mqA && tickAt(m.x) < st.mqB;
  if (lg && lClick && inGrid && !inMarquee && st.tool != Tool::Marquee) st.mqA = st.mqB = 0;  // R01-18
  // K-112c checkpoint 56 (N55-04): Clips' loop brace in the ruler's lower half, apart from the time
  // selection: a drag on it moves it, a drag at an end resizes it (snapped), a click switches Loop.
  // Transport state, as Smooth's cycle: no undo step (livelook::setLoopBrace).
  static int braceMode = -1;  // 1 move, 2 left end, 3 right end
  static Tick braceA = 0, braceB = 0, braceT0 = 0;
  static bool braceMoved = false;
  bool braceTook = false;
  if (liveLook && s.cycleEnd > s.cycleStart && braceMode < 0) {
    const float by0 = rY + kLiveNumH, by1 = rY + kLiveNumH + kLiveBraceH;
    const float bx0 = xOf(s.cycleStart), bx1 = xOf(s.cycleEnd);
    int over = -1;
    if (hovered && m.y >= by0 && m.y < by1 && m.x >= gMin.x && m.x < gMax.x) {
      if (std::fabs(m.x - bx0) <= 5) {
        over = 2;
      } else if (std::fabs(m.x - bx1) <= 5) {
        over = 3;
      } else if (m.x > bx0 && m.x < bx1) {
        over = 1;
      }
    }
    if (over >= 0) {
      ImGui::SetMouseCursor(over == 1 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
      a.setHint(over == 1 ? i18n::tr("The loop brace: drag to move it, click to switch Loop on or off") : i18n::tr("The loop brace's end: drag to resize it"));
    }
    if (lClick && over >= 0 && !io.KeyCtrl) {
      braceMode = over, braceA = s.cycleStart, braceB = s.cycleEnd, braceT0 = tickAt(m.x), braceMoved = false;
      braceTook = true;
    }
  }
  if (braceMode >= 0) {
    braceTook = true;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      braceMoved = braceMoved || ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
      if (braceMoved) {
        const Tick step = std::max<Tick>(1, snapG), org = livelook::arrangementOrigin(a);
        const Tick d = tickAt(m.x) - braceT0;
        Tick na = braceA, nb = braceB;
        if (braceMode == 1) {
          na = std::max(org, timeline::snapTick(mm, braceA + d, snapG, Snap::Line));
          nb = na + (braceB - braceA);
        } else if (braceMode == 2) {
          na = std::clamp<Tick>(timeline::snapTick(mm, braceA + d, snapG, Snap::Line), org, braceB - step);
        } else {
          nb = std::max<Tick>(braceA + step, timeline::snapTick(mm, braceB + d, snapG, Snap::Line));
        }
        livelook::setLoopBrace(a, na, nb, s.cycleOn);
      }
    } else {
      if (!braceMoved) {
        livelook::toggleLoop(a);
      }
      braceMode = -1;
    }
  }
  if ((lClick || rClick) && !braceTook) {
    cg = ClipGesture{};
    cg.selBefore = st.selected;
    cg.start = m;
    cg.startTick = tickAt(m.x);
    cg.startTrack = trackAt(m.y);
    cg.base = s.playlist;
    cg.anchor = hoverClip;
    cg.key = 0x504C000000000000ull ^ (clipGestureCounter++ << 8);
    if (inRuler && lClick && hoverMarker >= 0) {
      // J-85: dragging a marker moves it (snapped; the song loop marker moves
      // the loop start); a click without dragging jumps to it (on release).
      cg.kind = ClipGesture::MarkerDrag;
      cg.marker = hoverMarker;
      cg.markerTo = s.markers[static_cast<size_t>(hoverMarker)].pos;
    } else if (inRuler && lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      st.selStart = st.selEnd = 0;  // double-click clears the time selection
    } else if (lg && inRuler && lClick && m.y < rulerBot && !io.KeyCtrl && io.KeyShift &&
               (st.selEnd > st.selStart || s.cycleEnd > s.cycleStart)) {
      // R04-12: Shift-click in the ruler moves the cycle's nearer edge there (Cycle turns on).
      const Tick ca = st.selEnd > st.selStart ? st.selStart : s.cycleStart, cb = st.selEnd > st.selStart ? st.selEnd : s.cycleEnd;
      const Tick at = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      const Tick step = std::max<Tick>(1, snapG);
      if (std::llabs(at - ca) <= std::llabs(at - cb)) st.selStart = std::min(at, cb - step), st.selEnd = cb;
      else st.selStart = ca, st.selEnd = std::max(at, ca + step);
    } else if (lg && inRuler && lClick && m.y < rulerBot && !io.KeyCtrl) {
      // R02-12: as the reference's ruler: a drag across the bar numbers makes a cycle area; on the
      // area, a drag moves it and a drag at an edge resizes it; a click on it turns Cycle off, a
      // click elsewhere moves the playhead. R04-12: with Cycle off the grey area works the same
      // (a click turns it on).
      cg.kind = ClipGesture::TimeSel;
      cg.cycleMode = 0;
      cg.cycleA = st.selStart, cg.cycleB = st.selEnd;
      const bool greyCycle = st.selEnd <= st.selStart && s.cycleEnd > s.cycleStart;
      if (greyCycle) cg.cycleA = s.cycleStart, cg.cycleB = s.cycleEnd;
      if (st.selEnd > st.selStart || greyCycle) {
        const float x0 = xOf(cg.cycleA), x1 = xOf(cg.cycleB);
        if (std::fabs(m.x - x0) <= 5) cg.cycleMode = 2;
        else if (std::fabs(m.x - x1) <= 5) cg.cycleMode = 3;
        else if (m.x > x0 && m.x < x1) cg.cycleMode = 1;
      }
    } else if (inRuler && (rClick || (lClick && io.KeyCtrl))) {
      cg.kind = ClipGesture::TimeSel;
      st.selTracks.clear();  // N48-05: a selection drawn in the ruler covers every track
      cg.rulerMenu = rClick;  // J-85: a right-click that does not drag opens the ruler menu
    } else if (inRuler && lClick) {
      if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);  // J-49: no rewind when already in song mode
      cg.kind = ClipGesture::Scrub;  // the position follows the pointer until release
      cg.lastScrub = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      j31::songJump(a, cg.lastScrub);  // J-31: a click while playing waits for the jump grid
    } else if (inHead) {
      int tr = trackAt(m.y);
      if (lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // A double-click on the name renames; elsewhere on an instrument track's header it shows the
        // instrument (as the "+" menu's Show the instrument does). The header's controls (the "+",
        // mute switch, handle, mode controls, fold arrow) took their clicks above and never get here.
        const Channel* inst = tr >= 0 && tr < nTracks && trackmodes::modeOf(s, tr) == TrackMode::Instrument ? trackmodes::linkedChannel(s, tr) : nullptr;
        if (headName == tr) {
          renaming = tr;
          ImGui::OpenPopup("##trname");
        } else if (inst) {
          a.selectChannel(inst->id);
          a.settingsChannel = inst->id;
        }
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
    } else if (inGrid && rClick && lg) {
      // R01-01: the Tracks layout never deletes on a right-click: on a region it selects it (keeping
      // a selection it belongs to) and opens the region menu; on empty space the background menu.
      if (hc) {
        if (!st.selected.count(hoverClip)) st.selected = ops::expandClipGroups(s, {hoverClip});
        menuClip = hoverClip;
        ImGui::OpenPopup("##clipmenu");
      } else {
        lgBgTrack = trackAt(m.y);
        lgBgTick = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), std::max<Tick>(1, s.barTicks()), Snap::Line));
        ImGui::OpenPopup("##lgbgmenu");
      }
    } else if (inGrid && rClick && hc && (liveLook || clipheader::opensMenu(hoverZone))) {
      // C44-01: in Clips a right-click (a Control-click on a Mac) anywhere on a clip opens its menu and
      // never deletes: a Control-held press on a clip used to start the right-drag delete.
      if (liveLook && !st.selected.count(hoverClip)) st.selected = {hoverClip};
      // N48-19 (checkpoint 49): the clip band follows the right-clicked clip, as a click's selection does.
      if (liveLook && hc->kind == ClipKind::Pattern && hc->pattern != a.curPattern) a.selectPattern(hc->pattern);
      menuClip = hoverClip;  // J-142: right-click on the clip's name box: clip menu (the rest of the title strip deletes)
      ImGui::OpenPopup("##clipmenu");
    } else if (inGrid && rClick && st.tool == Tool::Zoom) {
      st.zoomRequest /= 1.5f;
    } else if (inGrid && rClick && st.tool == Tool::Warp && hc && plwarp::hovered(hc->id)) {
      plwarp::removeHovered(a, *hc);  // K-85: a right-click on a warp marker removes it
    } else if (inGrid && rClick && !liveLook) {
      cg.kind = ClipGesture::Erase;  // right-drag deletes with every editing tool (like FL; not in Clips, C44-01)
      group("delete clips");
    } else if (inGrid && lClick && lg && st.tool == Tool::Select && inMarquee) {
      // R01-18: a Pointer click inside the marquee cuts its range out as regions of their own
      // (selected), as the reference's does.
      std::set<int> trs;
      for (int t = st.mqT0; t <= st.mqT1; ++t)
        if (!trackLocked(t)) trs.insert(t);
      const Tick r0 = st.mqA, r1 = st.mqB;
      std::set<ObjectId> inside;
      if (!trs.empty()) a.doc.editSong("cut out the marquee", [&](Song& x) { inside = chop::isolateRange(x, r0, r1, trs); });
      st.selected = inside;
      st.mqA = st.mqB = 0;
    } else if (inGrid && lClick && lg && st.tool == Tool::Marquee) {
      cg.kind = ClipGesture::Marquee;  // R01-18: drag out a range over the tracks crossed
      cg.startTick = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      st.mqA = st.mqB = 0;
    } else if (inGrid && lClick && lg && st.tool == Tool::Fade) {
      // R01-18: the Fade tool: a drag from inside an audio region sets the fade on the nearer end
      // (its length follows the pointer); Alt+click removes that fade.
      if (hc && hc->kind == ClipKind::Audio && !trackLocked(hc->track)) {
        const float cx0 = xOf(hc->start), cx1 = xOf(hc->start + s.clipLength(*hc));
        const bool fin = m.x < (cx0 + cx1) * 0.5f;
        if (io.KeyAlt) {
          const ObjectId id = hc->id;
          a.doc.editClip(id, fin ? "clip fade in" : "clip fade out", [fin](Clip& x) {
            if (fin) x.fadeIn = 0, x.fadeInManual = true;
            else x.fadeOut = 0, x.fadeOutManual = true;
          });
        } else {
          fadeDragClip = hc->id;
          fadeDragKind = fin ? 1 : 2;
          fadeDragMirror = kNoId;
          fadeDragY0 = m.y;
          fadeDragT0 = 0;
          fadeToolT0 = std::max<Tick>(0, tickAt(m.x));
          const clipmulti::Param fp = fin ? clipmulti::Param::FadeIn : clipmulti::Param::FadeOut;
          fadeDragSnap = clipmulti::capture(s, clipui::targets(a, hc->id, fp), fp, hc->id);
        }
      }
    } else if (inGrid && lClick && !ctlK && st.tool == Tool::Mute) {
      cg.kind = ClipGesture::Mute;
      if (hc) cg.muteTo = hc->muted ? 0 : 1;
    } else if (inGrid && lClick && lg && !ctlK && st.tool == Tool::Warp && !(hc && hc->kind == ClipKind::Audio)) {
      // R07-05: the Flex tool works on audio regions only; a click elsewhere makes nothing
    } else if (inGrid && lClick && hc && !ctlK && st.tool == Tool::Warp) {  // K-85: warp markers
      if (hc->kind == ClipKind::Audio && !(hc->track >= 0 && hc->track < nTracks && s.tracks[static_cast<size_t>(hc->track)].locked)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && plwarp::removeHovered(a, *hc)) {
        } else {
          group("warp audio");
          // A new marker snaps to a transient only within a few pixels of the pointer, at any zoom.
          const double snapTicks = 8.0 / std::max(1e-6f, st.pxPerTick);
          if (plwarp::begin(a, *hc, tempoMap, static_cast<double>(std::max<Tick>(0, tickAt(m.x))), io.KeyShift, cg.key, snapTicks)) {
            cg.kind = ClipGesture::Warp;
          } else {
            a.doc.cancelGroup();
            cg.grouped = false;
          }
        }
      }
    } else if (inGrid && lClick && hc && ((!ctlK && st.tool == Tool::Slip) || (clipTool == Tool::Draw && slipDrag && !loopEdgeGrab))) {
      cg.kind = ClipGesture::Slip;  // every clip kind (audio: its sample position)
    } else if (inGrid && lClick && !ctlK && st.tool == Tool::Slice) {
      cg.kind = ClipGesture::SliceLine;  // cut on release: the clicked clip, or every clip the vertical line crossed
      cg.startTick = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
    } else if (inGrid && lClick && lg && !ctlK && st.tool == Tool::Text) {  // R05-17: rename the region clicked
      // R07-01: the region's own name, typed in place over its name bar (its pattern, sound and the
      // other regions keep theirs; an empty name goes back to the pattern's or sound's).
      if (hc) {
        const int ci = s.clipIndex(hc->id);
        gRegionRename = hc->id;
        gRegionRenameAt = ImVec2(std::max(gMin.x, xOf(hc->start)) + 2, yOf(hc->track) + 1);
        gRegionRenameW = std::clamp(xOf(hc->start + s.clipLength(*hc)) - gRegionRenameAt.x - 4, 80.0f, 260.0f);
        gRegionRenameText = !hc->name.empty() ? hc->name : ci >= 0 && static_cast<size_t>(ci) < ccache.name.size() ? *ccache.name[static_cast<size_t>(ci)] : std::string();
        ImGui::OpenPopup("##rgname");
      }
    } else if (inGrid && lClick && lg && !ctlK && st.tool == Tool::Glue) {  // R05-17: join the selected regions
      if (hc) {
        if (!st.selected.count(hc->id)) a.setHint(i18n::tr("Join: select two or more regions, then click one of them"));
        else tracksJoin(a, hc->kind);
      }
    } else if (inGrid && lClick && lg && !ctlK && st.tool == Tool::Solo) {  // R05-17: play the region's track alone while held
      if (hc) {
        if (const MixerTrack* strip = logicplug::trackStrip(s, hc->track); strip && !strip->solo) {
          group("solo tool");
          a.doc.editMixerTrack(strip->id, "solo mixer track", [](MixerTrack& x) { x.solo = true; });
          cg.soloHeld = true;
        }
        a.engine.setMode(PlayMode::Song);
        a.engine.setPosition(std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line)));
        if (!a.engine.playing()) a.engine.play();
        cg.kind = ClipGesture::Play;
      }
    } else if (inGrid && lClick && !ctlK && st.tool == Tool::Play) {
      a.engine.setMode(PlayMode::Song);
      a.engine.setPosition(std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line)));
      if (!a.engine.playing()) a.engine.play();
      cg.kind = ClipGesture::Play;
    } else if (inGrid && lClick && !ctlK && st.tool == Tool::Zoom) {
      cg.kind = ClipGesture::Zoom;
    } else if (inGrid && lClick && hoverAuto && clipTool == Tool::Draw && !ctlK && !io.KeyShift && (hoverPoint >= 0 || hoverHandle >= 0)) {
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
    } else if (inGrid && lClick && hoverAuto && clipTool == Tool::Draw && !ctlK && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      // Double-click on the curve: add a point there and keep dragging it.
      const AutomationClip* ac = s.automation(hc->source);
      const float headH = std::min(16.0f, (hOf(hc->track) - 4) * 0.45f);
      AutoGeom g{ImVec2(xOf(hc->start) + 4, yOf(hc->track) + 2 + headH + 2), ImVec2(xOf(hc->start + s.clipLength(*hc)) - 4, yOf(hc->track) + hOf(hc->track) - 5),
                 xOf(hc->start), st.pxPerTick, hc->start, hc->offset, s.clipLength(*hc)};
      Tick pos = hc->offset + std::max<Tick>(0, snapPos(tickAt(m.x)) - hc->start);
      if (hc->loop && hc->loopLength > 0) pos = wrapClipOffset(s, *hc, pos);  // K-140: into the part it repeats
      AutoPoint np{pos, static_cast<float>(g.valueAtY(m.y)), 0.0f, AutoShape::Curve};
      ObjectId aid = ac->id;
      int idx = 0;
      cg.autoId = aid;
      editAuto(aid, [&](AutomationClip& q) { idx = insertAutoPoint(q, np); });
      cg.kind = ClipGesture::AutoPoint;
      cg.point = idx;
    } else if (inGrid && lClick) {
      if (hoverClip != kNoId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (hc->kind == ClipKind::Pattern && hoverCurve.owner != kNoId) {
          openPatternAutomation(a, hc->pattern, hoverCurve);  // J-173: the curve's clip in the automation editor
        } else if (livelook::active(a) && hc->kind != ClipKind::Automation) {
          // Clips: a double-click shows the clip in the clip band (the selection already chose its
          // notes or sample), as the reference; Studio's channel rack is not part of this layout.
          // (Before K-278's piano roll: the clip band is the Clips layout's clip editor.)
          if (hc->kind == ClipKind::Pattern) a.selectPattern(hc->pattern);
          layoutui::setRegionVisible(a, "live.clip", true);
        } else if (hc->kind == ClipKind::Pattern) {
          // R01-11: a region with notes opens the piano roll in the lower pane (the reference's MIDI
          // region); a steps-only region (drums) opens the step sequencer (its pattern region).
          // K-278: the same in every layout: a double-click on a pattern clip opens the piano roll on it.
          a.selectPattern(hc->pattern);
          const Pattern* hp = s.pattern(hc->pattern);
          const ObjectId noteCh = patternNoteChannel(a, s, *hc);
          bool steps = false;
          if (hp)
            for (const auto& [cid, row] : hp->steps)
              for (const uint8_t stp : row) steps = steps || stp != 0;
          if (noteCh != kNoId || !steps) {
            if (noteCh != kNoId) a.selectChannel(noteCh);
            a.showPiano = true;
            a.piano.fitRequest = true;  // R02-18: it opens showing the whole region
            a.focusWindow = "Piano roll";
          } else {
            a.showRack = true;
            a.focusWindow = "Channel rack";
          }
        } else if (hc->kind == ClipKind::Audio && lg) {
          // R01-12: the audio editor (Loupe) in the lower pane, as the reference's double-click opens
          // its audio track editor there; no floating channel window.
          if (const Channel* ach = s.channel(hc->source); ach && !ach->plugin.data.empty()) {
            layoutui::setRegionVisible(a, "logic.lower", true);
            a.showPiano = a.showRack = a.showMixer = false;  // the pane opens with the piano roll: the audio editor takes its place
            a.settings.set("ui.loupePinned", true);
            packs::openAudioEditor(a, ach->plugin.data, ach->id);
          }
        } else if (hc->kind == ClipKind::Audio) {
          a.settingsChannel = hc->source;  // the channel that plays the clip (its sample, stretch, ...)
        } else if (hc->kind == ClipKind::Automation) {
          openAutomationEditor(a, hc->source, hc->id);  // J-5: automation clip editor
        }
      } else if (liveLook && !hc && pastRows(m.y)) {
        // N52-03: below every track's row (the return lanes, the free area) there is no track's time to
        // select: nothing starts (a folded group's hidden last track was taken before).
      } else if (liveLook && ((hc && hoverZone == clipheader::Zone::Body && !onEdge) || (!hc && cg.startTrack >= 0 && cg.startTrack < nTracks && !pastRows(m.y))) &&
                 !io.KeyCtrl && !io.KeyAlt && !slipDrag && (st.tool != Tool::Draw || !hc)) {
        // N50-03: in Draw mode too an empty lane takes the marker and a time selection: Draw mode never
        // paints the current pattern (another track's notes) into a lane.
        // N47-08: in Clips' arrangement a click in a clip's body selects it and puts the insert marker
        // there (Cmd+E splits at it); a drag from there draws a time selection. The clip moves by its
        // title bar, as the reference's. N49-07: a click in an empty lane does the same without a clip
        // (the track is selected: Insert MIDI clip goes there).
        if (hc) {
          if (!io.KeyShift) st.selected = {hoverClip};
          else st.selected.insert(hoverClip);
        } else if (!io.KeyShift) {
          st.selected.clear();
        }
        st.trackSel = {cg.startTrack};
        const Tick at = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
        if (a.engine.mode() == PlayMode::Pattern) a.engine.setMode(PlayMode::Song);
        a.engine.setPosition(at);
        cg.kind = ClipGesture::TimeSel;
        cg.startTick = tickAt(m.x);
        cg.insertMarker = at;
        st.selTracks.clear();
      } else if ((ctlK || clipTool == Tool::Select) && !loopEdgeGrab) {  // (K-114: Ctrl on an edge loops it)
        cg.kind = ClipGesture::Select;
        if (!io.KeyShift) st.selected.clear();
      } else if (st.tool == Tool::Erase) {
        cg.kind = ClipGesture::Erase;
        group("delete clips");
      } else if (st.tool == Tool::Paint) {
        if (hc) {  // painting starts from a clip: it becomes the source (J-118: and the brush)
          setSourceFromClip(a, *hc);
        }
        const pltools::Source ps = currentSource(a);
        if (pltools::sourceValid(s, ps)) {
          cg.kind = ClipGesture::Paint;
          cg.paintKind = ps.kind;
          cg.paintId = ps.id;
          cg.paintLen = pltools::brushLength(s, ps, currentBrush(a, ps));
          cg.paintAnchor = hc ? hc->start : std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));
          group("paint clips");
        }
      } else if (hoverClip != kNoId) {
        if (!st.selected.count(hoverClip)) {
          if (!io.KeyShift || shiftDrag || loopEdgeGrab) st.selected.clear();
          st.selected.insert(hoverClip);
          st.selected = ops::expandClipGroups(s, st.selected);  // J-31: grouped clips are selected together
        }
        setSourceFromClip(a, *hc);  // clicking a clip makes it (its source, as it is: J-118) current
        // K-278: a click on a pattern clip switches the piano roll to it (the pattern above; its
        // channel with notes when the selected one has none there).
        if (hc->kind == ClipKind::Pattern)
          if (const ObjectId nc = patternNoteChannel(a, s, *hc); nc != kNoId && nc != a.selChannel) a.selectChannel(nc);
        cg.kind = hoverEdge ? ClipGesture::Resize : hoverLeftEdge ? ClipGesture::Trim : ClipGesture::Move;
        // Shift+drag: copies of the selection are dragged, the originals stay
        // (Q-3: made once the pointer actually moves, so a Shift+click adds
        // nothing).
        if (cg.kind == ClipGesture::Move && shiftDrag && !trackLocked(hc->track)) cg.copyPending = true;
      } else if (lg && st.tool == Tool::Draw && hoverClip == kNoId && trackAt(m.y) >= 0 && trackAt(m.y) < nTracks && !trackLocked(trackAt(m.y)) &&
                 s.tracks[static_cast<size_t>(trackAt(m.y))].mode != TrackMode::Audio && !fold.takesNoRegions(trackAt(m.y))) {
        // R10-02: the reference's Pencil on empty space of a MIDI track: a new, empty one-bar region
        // each time, named after the track (it placed the current pattern, so two clicks gave two
        // regions sharing one note list). It becomes the current pattern, its sound the selected one.
        const int tr = trackAt(m.y);
        const Tick at = std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));
        group("add clip");
        const std::string nm = playlistTrackName(s, static_cast<size_t>(tr));
        const ObjectId sound = [&] {
          const Channel* c = logicplug::trackSound(a, tr);
          return c ? c->id : kNoId;
        }();
        const bool plainTrack = tr >= static_cast<int>(smoothTrackColors(a).size()) || smoothTrackColors(a)[static_cast<size_t>(tr)] == 0;
        const ObjectId pid = a.doc.addPattern(nm.empty() ? ops::nextPatternName(a.doc.song()) : nm);
        if (pid != kNoId && plainTrack) {
          // (a track with no colour of its own: the region takes one of the reference's track colours by row)
          static const Rgb kRowCols[] = {0x3E8FD8, 0x4DB06A, 0xD9A23A, 0xC85A4E, 0x8C6BD6, 0x37ABA8, 0xD36AA6, 0x9AA548};
          const Rgb col = kRowCols[static_cast<size_t>(tr) % (sizeof kRowCols / sizeof kRowCols[0])];
          a.doc.editPattern(pid, "pattern colour", [col](Pattern& p) { p.color = col; });
        }
        ObjectId id = kNoId;
        if (pid != kNoId)
          a.doc.editSong("add clip", [&](Song& x) {
            Clip c = pltools::makeClip(x, pltools::Source{ClipKind::Pattern, pid}, tr, at);
            c.id = x.allocateId();
            x.playlist.push_back(c);
            id = c.id;
          });
        if (id) {
          setSource(a, pltools::Source{ClipKind::Pattern, pid});
          if (sound != kNoId) a.selectChannel(sound);
          st.selected = {id};
          st.trackSel = {tr};
          cg.base = a.doc.song().playlist;
          cg.anchor = id;
          cg.startTick = at;
          cg.kind = ClipGesture::Move;
          cg.placed = true;
        }
      } else if (pltools::sourceValid(s, src) && !clipAllowed(pltools::makeClip(s, src, trackAt(m.y), 0).kind, trackAt(m.y))) {
        a.notify(clipRefused(trackAt(m.y)), true);  // PL-08, R09-01
      } else if (pltools::sourceValid(s, src) && groupLane(trackAt(m.y)) && pltools::makeClip(s, src, trackAt(m.y), 0).kind != ClipKind::Automation) {
        a.notify(i18n::tr(kGroupLaneHint), true);
      } else if (pltools::sourceValid(s, src)) {
        const Tick at = std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));
        const int tr = trackAt(m.y);
        ObjectId id = kNoId;
        group("add clip");
        const Clip* brush = currentBrush(a, src);
        const Clip brushCopy = brush ? *brush : Clip{};
        a.doc.editSong("add clip", [&](Song& x) {
          Clip c = pltools::makeBrushClip(x, src, brush ? &brushCopy : nullptr, tr, at);
          c.id = x.allocateId();
          x.playlist.push_back(c);
          id = c.id;
        });
        if (id) {
          st.selected = {id};
          cg.base = a.doc.song().playlist;
          cg.anchor = id;
          cg.startTick = at;
          cg.kind = ClipGesture::Move;
          cg.placed = true;
        }
      }
    }
  }

  if ((lClick || rClick) && gestureAnimates(cg.kind) && cg.kind != ClipGesture::Paint) {
    // J-37: the clips this drag changes start gliding from where they are drawn.
    // J-188: tracked whatever the setting (drawn where they are while it is off),
    // so switching smooth snapping on in the middle of a drag glides at once.
    const Song& ns = a.doc.song();
    const double now = ImGui::GetTime();
    for (const auto& c : ns.playlist)
      if (cg.kind == ClipGesture::Slip && !st.selected.count(cg.anchor) ? c.id == cg.anchor : st.selected.count(c.id) > 0) clipAnim.seed(c.id, clipAnimValues(c, ns.clipLength(c)), now);
  }
  if (active && cg.kind != ClipGesture::None) {
    // Dragging past the left/right edge scrolls the view (scrub, time selection, clips).
    if (cg.kind != ClipGesture::Select && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) + ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f)) {
      const float edge = 24.0f;
      if (m.x > gMax.x - edge) st.scrollX += (m.x - (gMax.x - edge)) * 0.35f;
      if (m.x < gMin.x + edge) st.scrollX = std::max(static_cast<float>(viewO) * st.pxPerTick, st.scrollX - ((gMin.x + edge) - m.x) * 0.35f);
    }
    Tick curTick = std::max<Tick>(0, tickAt(std::clamp(m.x, gMin.x, gMax.x)));
    int curTrack = trackAt(m.y);
    auto baseClip = [&]() -> const Clip* {
      for (const auto& c : cg.base)
        if (c.id == cg.anchor) return &c;
      return nullptr;
    };
    auto applyClips = [&](const char* label, const std::vector<Clip>& after) {
      a.doc.editClips(label, [&](std::vector<Clip>& cs) {
        if (cs.size() == after.size()) cs = after;
      }, cg.key);
    };
    // Q-3: Shift+V while dragging copies makes them unique (each pattern
    // clip copy gets its own copy of the pattern), like FL's drag + Shift+V.
    if (cg.kind == ClipGesture::Move && (cg.copied || cg.copyPending) && !cg.unique && keys::pressedHeld("playlist.uniqueCopy")) {
      if (cg.copyPending) {  // not moved yet: copy in place first
        cg.copyPending = false;
        group("copy clips");
        std::vector<ObjectId> copies;
        const std::set<ObjectId> selNow = st.selected;
        a.doc.editSong("copy clips", [&](Song& x) { copies = pltools::cloneClips(x, selNow); });
        size_t k = 0;
        for (const auto& c : cg.base)
          if (selNow.count(c.id)) {
            if (c.id == cg.anchor && k < copies.size()) cg.anchor = copies[k];
            ++k;
          }
        st.selected = std::set<ObjectId>(copies.begin(), copies.end());
        cg.base = a.doc.song().playlist;
        cg.copied = true;
      }
      int made = 0;
      for (ObjectId id : std::set<ObjectId>(st.selected))
        if (const Clip* c = a.doc.song().clip(id); c && c->kind == ClipKind::Pattern) made += ops::makeClipUnique(a.doc, id) != kNoId;
      for (Clip& b : cg.base)  // the drag goes on from the gesture's start positions, with the new patterns
        if (const Clip* c = a.doc.song().clip(b.id); c && st.selected.count(b.id)) b.pattern = c->pattern;
      cg.unique = true;
      a.notify(made ? i18n::tr("The copies are unique (new patterns)") : i18n::tr("No pattern clips to make unique"));
    }
    switch (cg.kind) {
      case ClipGesture::Scrub: {
        // Smooth: follows the pointer every frame, landing on the snap grid (Alt = free).
        Tick t = std::max<Tick>(0, timeline::snapTick(mm, curTick, snapG, Snap::Line));
        if (t != cg.lastScrub) {
          j31::songJump(a, t);  // J-31: immediate when stopped, quantized while playing
          cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::MarkerDrag: {
        if (!cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;
        cg.moved = true;
        const Marker* mk = cg.marker >= 0 && cg.marker < static_cast<int>(s.markers.size()) ? &s.markers[static_cast<size_t>(cg.marker)] : nullptr;
        if (mk) cg.markerTo = std::max<Tick>(0, timeline::snapTick(mm, mk->pos + (curTick - cg.startTick), snapG, Snap::Line));
        break;
      }
      case ClipGesture::TimeSel: {
        if (cg.rulerMenu && !cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.0f)) break;  // J-85: a click so far
        if (cg.cycleMode >= 0 && !cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;  // R02-12: a click so far
        if (cg.cycleMode >= 1) {  // R02-12: the cycle area moved or resized (at least one grid step long)
          cg.moved = true;
          const Tick step = std::max<Tick>(1, snapG), len = cg.cycleB - cg.cycleA;
          const Tick at = std::max<Tick>(0, timeline::snapTick(mm, curTick, snapG, Snap::Line));
          if (cg.cycleMode == 1) {
            const Tick a0 = std::max<Tick>(0, timeline::snapTick(mm, cg.cycleA + (curTick - cg.startTick), snapG, Snap::Line));
            st.selStart = a0, st.selEnd = a0 + len;
          } else if (cg.cycleMode == 2) {
            st.selStart = std::min(at, cg.cycleB - step), st.selEnd = cg.cycleB;
          } else {
            st.selStart = cg.cycleA, st.selEnd = std::max(at, cg.cycleA + step);
          }
          break;
        }
        cg.moved = true;
        Tick t0 = std::max<Tick>(0, timeline::snapTick(mm, cg.startTick, snapG, Snap::Line));
        Tick t1 = std::max<Tick>(0, timeline::snapTick(mm, curTick, snapG, Snap::Line));
        st.selStart = std::min(t0, t1);
        st.selEnd = std::max(t0, t1);
        if (cg.insertMarker >= 0 && st.selEnd > st.selStart) {
          // N48-05: Clips: the selected time replaces the clip selection (Cmd+E and Cmd+D act on the time),
          // on the tracks from the press to the pointer.
          st.selected.clear();
          st.selTracks.clear();
          const int ta = std::clamp(std::min(cg.startTrack, curTrack), 0, std::max(0, nTracks - 1));
          const int tb = std::clamp(std::max(cg.startTrack, curTrack), 0, std::max(0, nTracks - 1));
          for (int t = ta; t <= tb; ++t) st.selTracks.insert(t);
        }
        break;
      }
      case ClipGesture::Move: {
        const Clip* an = baseClip();
        if (!an) break;
        if (!cg.moved && !cg.placed && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;  // a click moves nothing
        Tick earliest = an->start;
        for (const auto& c : cg.base)
          if (st.selected.count(c.id)) earliest = std::min(earliest, c.start);
        Tick dt = timeline::moveDistance(mm, curTick - cg.startTick, snapG, st.snap, earliest);
        if (viewO > 0 && earliest >= viewO) dt = std::max(dt, viewO - earliest);  // C44-20: arrangement clips stay out of the clip grid
        const int dtr = curTrack - cg.startTrack;
        if (dt == 0 && dtr == 0 && !cg.moved) break;
        if (cg.copyPending) {
          // Q-3: Shift+drag — the copies (every clip setting kept, grouped
          // clips in new groups, the same pattern) are what moves from here.
          cg.copyPending = false;
          group("copy clips");
          std::vector<ObjectId> copies;
          const std::set<ObjectId> selNow = st.selected;
          a.doc.editSong("copy clips", [&](Song& x) { copies = pltools::cloneClips(x, selNow); });
          size_t k = 0;
          ObjectId anchorCopy = kNoId;
          for (const auto& c : cg.base)
            if (selNow.count(c.id)) {
              if (c.id == cg.anchor && k < copies.size()) anchorCopy = copies[k];
              ++k;
            }
          st.selected = std::set<ObjectId>(copies.begin(), copies.end());
          cg.anchor = anchorCopy;
          cg.base = a.doc.song().playlist;
          cg.copied = true;
          for (const auto& c : cg.base)  // the copies glide from the originals' place (with smooth snapping on)
            if (st.selected.count(c.id)) clipAnim.seed(c.id, clipAnimValues(c, a.doc.song().clipLength(c)), ImGui::GetTime());
          an = baseClip();
          if (!an) break;
        }
        cg.moved = true;
        {
          std::vector<Clip> moved = pltools::moveClips(cg.base, st.selected, dt, dtr, nTracks, lockedTracks);
          int refused = -1;  // PL-08: instrument tracks take pattern clips only (R09-01: bus tracks none)
          for (const auto& c : moved)
            if (st.selected.count(c.id) && (!clipAllowed(c.kind, c.track) || (liveLook && !livelook::clipFitsTrack(s, c.kind, c.track))))
              refused = c.track;
          if (refused < 0) group("move clips"), applyClips("move clips", std::move(moved));
          else a.setHint(liveLook ? i18n::tr("MIDI clips go on MIDI tracks and audio clips on audio tracks")  // N50-02
                                  : clipRefused(refused));
        }
        break;
      }
      case ClipGesture::Resize: {
        const Clip* an = baseClip();
        if (!an) break;
        if (!cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;  // a click on the edge changes nothing
        const Tick oldEnd = an->start + s.clipLength(*an);
        // R02-10: the Tracks layout keeps a region at least one grid step long (it went down to one
        // tick: a sliver with no name that could hardly be selected again).
        Tick end = std::max(timeline::snapTick(mm, curTick, snapG, Snap::Line), an->start + (lg ? std::max<Tick>(1, snapG) : 1));
        if (loopMod) end = snapToCutPoint(tickAt(m.x), end, oldEnd, st.pxPerTick);  // K-140 (the pointer itself, not clamped to the view)
        const Tick d = end - oldEnd;
        if (d == 0 && !cg.moved) break;
        cg.moved = true;
        group("resize clips");
        // K-114: a plain drag extends (a clip that loops keeps looping); with the
        // loop key held the selected clips loop, in stretch mode too (they
        // loop instead of stretching). Read every frame, so pressing or
        // releasing it mid-drag switches between the two. K-140: they loop from
        // their cut point (the part they showed repeats) only once the edge is
        // past it; back at it or inside, they are as they were (plain).
        const std::vector<Clip> from = loopDragBase(a, cg.base, st.selected, loopMod && d > 0, tempoMap);
        const bool stretchNow = st.stretchResize && !loopMod;
        if (stretchNow) keepPitchWhenStretching(a, cg.base, st.selected);  // J-79
        std::vector<Clip> after = pltools::resizeEnds(a.doc.song(), from, st.selected, d, stretchNow, &tempoMap, st.zeroCross);
        // Back to its own length or shorter, a looping clip stops looping (live: dragged
        // out again in the same drag, it loops again, as it started from the drag's base).
        if (!loopMod) unloopWithinFirstPass(a.doc.song(), after, st.selected, tempoMap);
        a.setHint(resizeHint(a.doc.song(), from, after, an->id, d, loopMod));
        applyClips("resize clips", after);
        break;
      }
      case ClipGesture::Trim: {  // left edge: later start, content stays in place
        const Clip* an = baseClip();
        if (!an) break;
        if (!cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;  // a click on the edge changes nothing
        Tick to = timeline::snapTick(mm, curTick, snapG, Snap::Line);
        if (loopMod) to = snapToCutPoint(tickAt(m.x), to, an->start, st.pxPerTick);  // K-140 (the pointer itself)
        if (lg) to = std::min(to, an->start + s.clipLength(*an) - std::max<Tick>(1, snapG));  // R02-10: one grid step at least
        const Tick d = to - an->start;
        if (d == 0 && !cg.moved) break;
        cg.moved = true;
        group("trim clips");
        // K-114: with the loop key the clips loop and the edge sets where the loop starts
        // (the content wraps, so the clip may grow to the left). K-140: once the edge is
        // out past the clip's cut point; back at it or inside, a clip that did not loop
        // is plain (trimmed as without the key).
        const bool loopNow = loopMod && d < 0;
        const std::vector<Clip> from = loopDragBase(a, cg.base, st.selected, loopNow, tempoMap);
        const bool stretchNow = st.stretchResize && !loopMod;
        if (stretchNow) keepPitchWhenStretching(a, cg.base, st.selected);  // J-79
        a.setHint(loopNow ? std::string(i18n::tr("Looping: the left edge sets where the loop starts (back to the cut point: no loop)"))
                  : loopMod ? std::string(i18n::tr("At or inside the cut point: the clip does not loop (drag further out to loop it)"))
                            : std::string(i18n::tr("Trimming")) + "  (" + kLoopKey + i18n::tr(": set the loop start instead)"));
        applyClips("trim clips", pltools::trimStarts(a.doc.song(), tempoMap, from, st.selected, d, stretchNow, st.zeroCross));
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
        std::set<ObjectId> sel = io.KeyShift ? st.selected : std::set<ObjectId>{};
        for (const auto& c : s.playlist) {
          float x0 = xOf(c.start), x1 = xOf(c.start + s.clipLength(c)), y = yOf(c.track);
          if (hOf(c.track) >= 1 && x1 >= lo.x && x0 <= hi.x && y + hOf(c.track) >= lo.y && y <= hi.y) sel.insert(c.id);
        }
        st.selected = sel;
        break;
      }
      case ClipGesture::Paint: {
        const pltools::Source ps{cg.paintKind, cg.paintId};
        if (!pltools::sourceValid(s, ps) || cg.paintLen <= 0 || trackLocked(curTrack)) break;
        if (!clipAllowed(pltools::makeClip(s, ps, curTrack, 0).kind, curTrack)) break;  // PL-08, R09-01
        if (groupLane(curTrack) && pltools::makeClip(s, ps, curTrack, 0).kind != ClipKind::Automation) break;  // checkpoint 52
        std::vector<Tick> todo;
        for (Tick t : pltools::paintSlots(cg.paintAnchor, curTick, cg.paintLen))
          if (!pltools::occupied(s, curTrack, t, cg.paintLen)) todo.push_back(t);
        if (todo.empty()) break;
        const Tick len = cg.paintLen;
        const Clip* brush = currentBrush(a, ps);
        const Clip brushCopy = brush ? *brush : Clip{};
        std::vector<Clip> painted;
        a.doc.editSong("paint clips", [&](Song& x) {
          for (Tick t : todo) {
            Clip c = pltools::makeBrushClip(x, ps, brush ? &brushCopy : nullptr, curTrack, t);
            if (c.kind != ClipKind::Pattern || len != x.clipLength(c)) c.length = len;
            c.id = x.allocateId();
            x.playlist.push_back(c);
            painted.push_back(c);
          }
        });
        if (scrollui::smoothSnap() && snapG > 1)  // J-37: painted clips grow in from their start
          for (const Clip& c : painted) clipAnim.seed(c.id, clipAnimValues(c, 0), ImGui::GetTime());
        break;
      }
      case ClipGesture::Erase: {
        // J-79: the clip drawn on top goes; the clip it uncovers stays while
        // the pointer is still where the deleted clip was.
        const Clip* hcE = hoverClip != kNoId ? s.clip(hoverClip) : nullptr;
        if (!hcE || trackLocked(hcE->track)) break;
        const ObjectId del = cg.sweep.pick(s, hcE->track, std::max<Tick>(0, tickAt(m.x)), hoverClip);
        if (del == kNoId) break;
        cg.sweep.add(s, *hcE);
        st.selected.erase(del);
        logicplug::keepTrackSounds(a, {del});  // R08-07 (inside the gesture's group)
        a.doc.removeClip(del);
        break;
      }
      case ClipGesture::Mute: {  // every touched clip gets the first clip's new state
        if (hoverClip == kNoId || trackLocked(s.clip(hoverClip)->track)) break;
        const Clip* c = s.clip(hoverClip);
        if (cg.muteTo < 0) cg.muteTo = c->muted ? 0 : 1;
        if (c->muted == (cg.muteTo == 1)) break;
        const bool to = cg.muteTo == 1;
        a.doc.editClip(hoverClip, to ? "mute clips" : "unmute clips", [to](Clip& k) { k.muted = to; }, cg.key);
        break;
      }
      case ClipGesture::Warp:  // K-85: the grabbed warp marker follows the pointer (on the grid unless Alt)
        plwarp::drag(a, tempoMap, curTick, snapG);
        break;
      case ClipGesture::Slip: {  // content moves inside the clip (every kind)
        const Clip* an = baseClip();
        if (!an || trackLocked(an->track)) break;
        const Tick d = timeline::snapTick(mm, curTick, snapG, Snap::Line) - timeline::snapTick(mm, cg.startTick, snapG, Snap::Line);
        if (st.selected.count(an->id) && st.selected.size() > 1) {  // J-187: a selected clip slips every selected clip with it
          std::vector<Clip> all = cg.base;
          for (Clip& c : all)
            if (st.selected.count(c.id) && !trackLocked(c.track)) c = pltools::slipClip(a.doc.song(), tempoMap, c, d, st.zeroCross);
          if (!(all == a.doc.song().playlist)) {
            group("slip clip content");
            applyClips("slip clip content", all);
            a.setHint(std::to_string(st.selected.size()) + " clips: content offset " + timeline::rulerLabel(s, d, std::max<Tick>(1, s.ppq / 4)));
          }
          break;
        }
        const Clip after = pltools::slipClip(a.doc.song(), tempoMap, *an, d, st.zeroCross);
        if (const Clip* live = s.clip(an->id); live && !(after == *live)) {
          group("slip clip content");
          a.doc.editClip(an->id, "slip clip content", [&after](Clip& k) { k = after; }, cg.key);
          if (after.kind == ClipKind::Audio) {
            char b[64];
            std::snprintf(b, sizeof b, "Audio starts at %.3f s", after.audioStart);
            a.setHint(b);
          } else {
            a.setHint("Content offset " + timeline::rulerLabel(s, after.offset, std::max<Tick>(1, s.ppq / 4)));
          }
        }
        break;
      }
      case ClipGesture::Play: {  // dragging moves the playback start
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
          Tick t = std::max<Tick>(0, timeline::snapTick(mm, curTick, snapG, Snap::Line));
          if (t != cg.lastScrub) a.engine.setPosition(t), cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::Zoom: break;       // applied on release
      case ClipGesture::SliceLine: break;  // applied on release
      case ClipGesture::Marquee: {  // R01-18
        if (!cg.moved && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) break;
        cg.moved = true;
        const Tick e = std::max<Tick>(0, timeline::snapTick(mm, curTick, snapG, Snap::Line));
        st.mqA = std::min(cg.startTick, e), st.mqB = std::max(cg.startTick, e);
        st.mqT0 = std::clamp(std::min(cg.startTrack, curTrack), 0, std::max(0, nTracks - 1));
        st.mqT1 = std::clamp(std::max(cg.startTrack, curTrack), 0, std::max(0, nTracks - 1));
        break;
      }
      default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (cg.kind == ClipGesture::Play) a.engine.pause();  // the play tool plays while held
    if (cg.soloHeld && cg.grouped) {  // R05-17: the Solo tool's solo goes with the button, leaving no undo step
      a.doc.cancelGroup();
      cg.grouped = false;
    }
    if (cg.kind == ClipGesture::Zoom && ImGui::IsMousePosValid(&m)) {  // J-46: released outside the window: no zoom
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
    const bool regionCut = cg.kind == ClipGesture::SliceLine && std::fabs(m.x - cg.start.x) > 6.0f &&
                           std::fabs(m.x - cg.start.x) > std::fabs(m.y - cg.start.y);
    if (regionCut) {
      // PL-13: a horizontal drag cuts the clips at both ends of the dragged range
      // (Shift: on every track), selects the inside and makes it the time
      // selection, ready for Chop and stutter (Alt+C) or a time selection command.
      const Tick e = std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      const Tick r0 = std::min(cg.startTick, e), r1 = std::max(cg.startTick, e);
      const int t0 = io.KeyShift ? 0 : std::min(cg.startTrack, trackAt(m.y)), t1 = io.KeyShift ? nTracks - 1 : std::max(cg.startTrack, trackAt(m.y));
      std::set<int> trs;
      for (int t = t0; t <= t1; ++t)
        if (!trackLocked(t)) trs.insert(t);
      if (r1 > r0 && !trs.empty()) {
        std::set<ObjectId> inside;
        a.doc.editSong("slice clips", [&](Song& x) { inside = chop::isolateRange(x, r0, r1, trs); });
        st.selected = inside;
        st.selStart = r0, st.selEnd = r1;
      }
    } else if (cg.kind == ClipGesture::SliceLine) {
      // Shift: the whole column; otherwise the tracks the line was dragged over.
      const int t0 = io.KeyShift ? 0 : cg.startTrack, t1 = io.KeyShift ? nTracks - 1 : trackAt(m.y);
      std::vector<ObjectId> ids;
      for (ObjectId id : pltools::clipsAcross(s, cg.startTick, t0, t1))
        if (!trackLocked(s.clip(id)->track)) ids.push_back(id);
      if (!ids.empty()) {
        std::vector<ObjectId> right;
        const Tick at = cg.startTick;
        const bool zc = st.zeroCross;
        // R07-09: Alt in the Tracks layout cuts each region into pieces as long as the first one
        const bool equal = lg && io.KeyAlt;
        a.doc.editSong(ids.size() > 1 ? "slice clips" : "slice clip", [&](Song& x) {
          std::vector<std::pair<Tick, Tick>> spans;  // each region's start and end, before the cut
          if (equal)
            for (const ObjectId id : ids)
              if (const Clip* c = x.clip(id)) spans.emplace_back(c->start, c->start + x.clipLength(*c));
          right = pltools::sliceClips(x, ids, at, zc);
          if (!equal) return;
          std::vector<ObjectId> all = right;
          for (size_t i = 0; i < right.size() && i < spans.size(); ++i) {
            const Tick seg = at - spans[i].first;
            if (seg <= 0) continue;
            ObjectId piece = right[i];
            for (Tick cut = at + seg; cut < spans[i].second && piece != kNoId; cut += seg) {
              const std::vector<ObjectId> r = pltools::sliceClips(x, {piece}, cut, zc);
              piece = r.empty() ? kNoId : r.front();
              if (piece != kNoId) all.push_back(piece);
            }
          }
          right = all;
        });
        st.selected = std::set<ObjectId>(right.begin(), right.end());
      }
    }
    // Q-3: FL checks the V key as a Shift+drag copy ends (drag + Shift+V): held
    // then, the copies become unique too.
    if (cg.kind == ClipGesture::Move && cg.copied && !cg.unique && keys::keyDown("playlist.uniqueCopy")) {
      int made = 0;
      for (ObjectId id : std::set<ObjectId>(st.selected))
        if (const Clip* c = a.doc.song().clip(id); c && c->kind == ClipKind::Pattern) made += ops::makeClipUnique(a.doc, id) != kNoId;
      if (made) a.notify(i18n::tr("The copies are unique (new patterns)"));
    }
    if (cg.kind == ClipGesture::Move && !cg.moved && cg.anchor != kNoId && !io.KeyShift)
      st.selected = ops::expandClipGroups(a.doc.song(), {cg.anchor});  // the clicked clip and its group (J-31)
    if (cg.kind == ClipGesture::MarkerDrag && cg.marker >= 0 && cg.marker < static_cast<int>(s.markers.size())) {
      const Tick from = s.markers[static_cast<size_t>(cg.marker)].pos;
      if (cg.moved) {
        if (cg.markerTo != from) {
          const Tick to = cg.markerTo;
          ops::editMarker(a.doc, cg.marker, "move marker", [to](Marker& k) { k.pos = to; });
        }
      } else {
        if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);  // J-49: no rewind when already in song mode
        j31::songJump(a, from);  // jump to the marker (J-31: quantized when the option is on)
      }
    }
    if (cg.kind == ClipGesture::TimeSel && cg.rulerMenu && !cg.moved) {
      // J-85: right-click in the ruler without dragging: the ruler menu at that position.
      rulerMenuAt = std::max<Tick>(0, timeline::snapTick(mm, cg.startTick, snapG, Snap::Line));
      ImGui::OpenPopup("##rulermenu");
    } else if (cg.kind == ClipGesture::TimeSel && cg.cycleMode >= 0) {  // R02-12
      const bool wasOn = s.cycleOn;  // (off: cycleA / cycleB are the grey area, which stays off)
      if (!cg.moved && cg.cycleMode == 0) {  // a click beside the area: the playhead goes there
        st.selStart = wasOn ? cg.cycleA : 0, st.selEnd = wasOn ? cg.cycleB : 0;
        if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
        j31::songJump(a, std::max<Tick>(0, timeline::snapTick(mm, std::max<Tick>(0, cg.startTick), snapG, Snap::Line)));
      } else if (!cg.moved) {  // a click on the area: Cycle off (the range is kept for the next C)
        toggleCycle(a);
      } else if (st.selEnd <= st.selStart) {
        st.selStart = wasOn ? cg.cycleA : 0, st.selEnd = wasOn ? cg.cycleB : 0;  // a drag that made nothing changes nothing
      }
    } else if (cg.kind == ClipGesture::TimeSel) {
      if (st.selEnd > st.selStart) {
        if (!liveLook || a.engine.mode() == PlayMode::Pattern) a.engine.setMode(PlayMode::Song);
        a.engine.setPosition(st.selStart);  // playback starts at the selection, like FL
      } else {
        st.selStart = st.selEnd = 0;
        if (cg.insertMarker >= 0) a.engine.setPosition(cg.insertMarker);  // N47-08: a click: the insert marker
      }
    }
    if (cg.kind == ClipGesture::Warp) plwarp::end();
    if (lg && cg.kind == ClipGesture::Move && cg.moved && !st.selected.empty() && static_cast<int>(a.settings.getNumber("ui.logicDragMode")) == 1) {
      // R01-21 / R03-01: the Tracks layout drags with No Overlap, the reference's default drag mode:
      // where two regions overlap, the EARLIER one is shortened (guide: "the overlapped area of the
      // left region is shortened"); a later region keeps its start. So a region dropped over the end
      // of another shortens that one, and a dropped region that runs into a later one is itself
      // shortened. (It trimmed the later region's start, cutting a region the user never touched.)
      const Song& ns = a.doc.song();
      std::vector<Clip> v = ns.playlist;
      auto at = [&v](ObjectId id) -> const Clip* {
        for (const Clip& c : v)
          if (c.id == id) return &c;
        return nullptr;
      };
      bool any = false;
      for (const ObjectId mid : st.selected) {
        for (size_t i = 0; i < v.size(); ++i) {
          if (st.selected.count(v[i].id)) continue;
          const Clip* mc = at(mid);
          if (!mc || mc->track != v[i].track) continue;
          if ((mc->kind == ClipKind::Automation) != (v[i].kind == ClipKind::Automation)) continue;  // R02-03: a region never trims the track's curve
          const Tick ms = mc->start, me = mc->start + ns.clipLength(*mc);
          const Tick os = v[i].start, oe = v[i].start + ns.clipLength(v[i]);
          if (oe <= ms || os >= me) continue;
          if (os < ms) v = pltools::resizeEnds(ns, v, {v[i].id}, ms - oe, false);  // the other is earlier: it ends where the dropped one starts
          else if (os > ms) v = pltools::resizeEnds(ns, v, {mid}, os - me, false);  // the dropped one is earlier: it ends where the other starts
          else continue;  // same start: neither is earlier (both stay)
          any = true;
        }
      }
      if (any) a.doc.editClips("no overlap", [&v](std::vector<Clip>& cs) { cs = v; });
    }
    if (liveLook && cg.kind == ClipGesture::Move && cg.moved && cg.grouped) {  // C44-02: the moved clips overwrite what they land on
      const std::set<ObjectId> placed = st.selected;
      a.doc.editSong("move clips", [&](Song& x) { overwriteCovered(x, placed, viewO); });
    }
    if (cg.grouped) a.doc.endGroup();
    a.doc.endGesture();
    cg = ClipGesture{};
  }

  // Drop a clip source from the picker (or a pattern from elsewhere).
  // K-297 (round 6): the whole arrangement takes the drops. The target was the last item drawn,
  // which in Clips is a control of the track headers drawn over the lanes: nothing dragged from
  // the browser (an instrument, a pattern) could be dropped on the lanes there.
  if (ImGui::BeginDragDropTargetCustom(arrRect, arrId)) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (peek && peek->IsDataType("GL_PATTERN") && groupLane(trackAt(m.y))) {  // checkpoint 52
      ImGui::SetTooltip("%s", i18n::tr(kGroupLaneHint));
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    } else if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PATTERN")) {
      ObjectId pid = *static_cast<const ObjectId*>(pl->Data);
      ObjectId id = a.doc.addClip(pid, trackAt(m.y), std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell)));
      if (id) st.selected = {id};
    }
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CLIPSRC")) {
      const ClipSrcPayload ps = *static_cast<const ClipSrcPayload*>(pl->Data);
      const pltools::Source dsrc{static_cast<ClipKind>(ps.kind), ps.id};
      const int tr = trackAt(m.y);
      const Tick at = std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));
      if (pltools::sourceValid(s, dsrc) && !trackLocked(tr) && !clipAllowed(pltools::makeClip(s, dsrc, tr, at).kind, tr)) {
        a.notify(clipRefused(tr), true);  // PL-08, R09-01
      } else if (pltools::sourceValid(s, dsrc) && groupLane(tr) && pltools::makeClip(s, dsrc, tr, at).kind != ClipKind::Automation) {
        a.notify(i18n::tr(kGroupLaneHint), true);  // checkpoint 52
      } else if (pltools::sourceValid(s, dsrc) && !trackLocked(tr)) {
        ObjectId id = kNoId;
        a.doc.editSong("add clip", [&](Song& x) {
          Clip c = pltools::makeClip(x, dsrc, tr, at);
          c.id = x.allocateId();
          x.playlist.push_back(c);
          id = c.id;
        });
        st.selected = {id};
        setSource(a, dsrc);
      }
    }
    // K-112B (ref 068): audio files dragged below the track headers: "Create New Track using", as the
    // reference's: the option under the pointer when the button comes up makes a new track at the end.
    bool newTrackMenu = false;
    if (lg && m.x >= headX0 && m.x < headX1 && m.y >= std::max(gMin.y, yOf(nTracks))) {
      const char* ptype = nullptr;
      const std::vector<std::string> files = browserui::draggedAudioFiles(&ptype);
      if (!files.empty() && ptype) {
        newTrackMenu = true;
        const auto& P = pal();
        static const char* const kOpts[] = {i18n::N_("Audio track"), i18n::N_("Sampler"), i18n::N_("Sampler, a track per file")};
        const int nOpts = files.size() > 1 ? 3 : 2;
        const float rowH = 22.0f, px0 = headX0 + 10, px1 = headX1 - 10;
        const float py0 = std::max(gMin.y, yOf(nTracks)) + 8, ly0 = py0 + 24;
        int hot = -1;
        for (int i = 0; i < nOpts; ++i)
          if (m.y >= ly0 + 4 + rowH * static_cast<float>(i) && m.y < ly0 + 4 + rowH * static_cast<float>(i + 1)) hot = i;
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->PushClipRect(ImVec2(headX0, gMin.y), ImVec2(headX1, gMax.y), true);
        fdl->AddRectFilled(ImVec2(headX0, py0 - 8), ImVec2(headX1 - 1, ly0 + 8 + rowH * static_cast<float>(nOpts) + 6), theme::withAlpha(P.surface800, 0.96f));
        fdl->AddText(ImVec2(px0, py0), P.text, i18n::tr("Create new track using:"));
        fdl->AddRectFilled(ImVec2(px0, ly0), ImVec2(px1, ly0 + 8 + rowH * static_cast<float>(nOpts)), P.surface700, 8.0f);
        for (int i = 0; i < nOpts; ++i) {
          const float ry = ly0 + 4 + rowH * static_cast<float>(i);
          if (i == hot) fdl->AddRectFilled(ImVec2(px0, ry), ImVec2(px1, ry + rowH), theme::withAlpha(P.accent, 0.55f));
          fdl->AddText(ImVec2(px0 + 10, std::round(ry + (rowH - ImGui::GetFontSize()) * 0.5f)), P.text, i18n::tr(kOpts[i]));
        }
        fdl->PopClipRect();
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(ptype, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
          ImGui::SetMouseCursor(hot >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_NotAllowed);
          if (pl->Delivery && hot >= 0) {
            a.later([&a, files, hot] {
              const int n0 = static_cast<int>(a.doc.song().tracks.size());
              a.doc.beginGroup("create new track");
              if (hot == 0) {  // R08-05: a real audio track (its own strip), the files as regions from bar 1
                newAudioTrackFor(a, files, 0);
              } else {  // a sampler track (one for all the files, or one per file)
                const std::vector<std::vector<std::string>> groups = [&] {
                  std::vector<std::vector<std::string>> g;
                  if (hot == 1) g.push_back(files);
                  else
                    for (const std::string& f : files) g.push_back({f});
                  return g;
                }();
                int tr = n0;
                for (const auto& g : groups) {
                  std::set<ObjectId> before;
                  for (const Channel& c : a.doc.song().channels) before.insert(c.id);
                  browserui::dropFiles(a, {g.front()}, browse::DropTarget::Rack, browserui::DropPlace{});
                  ObjectId ch = kNoId;  // the channel the drop made (wherever it was inserted)
                  for (const Channel& c : a.doc.song().channels)
                    if (!before.count(c.id)) ch = c.id;
                  if (ch == kNoId) continue;
                  a.doc.setPlaylistTrackCount(tr + 1);
                  std::string why;
                  if (!trackmodes::makeInstrumentTrack(a.doc, tr, ch, &why)) a.notify(why, true);
                  a.playlist.trackSel = {tr};
                  ++tr;
                }
              }
              {  // checkpoint 47: each new track is named after its file, as the reference names them
                 // (a Sampler track that takes several files keeps its instrument's name)
                const Song& ns = a.doc.song();
                for (int t2 = n0; hot != 0 && t2 < static_cast<int>(ns.tracks.size()); ++t2) {
                  if (!ns.tracks[static_cast<size_t>(t2)].name.empty()) continue;
                  std::string path;
                  if (hot == 2 && static_cast<size_t>(t2 - n0) < files.size()) {
                    path = files[static_cast<size_t>(t2 - n0)];
                  }
                  const std::string stem = path.empty() ? std::string() : std::filesystem::path(path).stem().string();
                  if (!stem.empty()) a.doc.editPlaylistTrack(t2, "name track", [&stem](PlaylistTrack& k) { k.name = stem; });
                }
              }
              a.doc.endGroup();
            });
          }
        }
      }
    }
    // R08-04: audio dropped on the lanes below the last track makes a new audio track for it there
    // (the reference's), never a region on a track out of sight.
    if (lg && !newTrackMenu && m.x >= gMin.x && m.x < gMax.x && m.y >= std::max(gMin.y, yOf(nTracks)) && m.y < gMax.y) {
      const char* ptype = nullptr;
      const std::vector<std::string> files = browserui::draggedAudioFiles(&ptype);
      if (!files.empty() && ptype) {
        newTrackMenu = true;
        const Tick at = std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));
        {
          const auto& P = pal();
          const float gy = std::max(gMin.y, yOf(nTracks));
          dl->PushClipRect(gMin, gMax, true);
          dl->AddRectFilled(ImVec2(xOf(at), gy + 2), ImVec2(xOf(at) + 120, gy + 2 + std::max(24.0f, trackH)), theme::withAlpha(P.accent, 0.25f), 4.0f);
          dl->AddText(ImVec2(xOf(at) + 6, gy + 6), P.text, i18n::tr("New audio track"));
          dl->PopClipRect();
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(ptype, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery))
          if (pl->Delivery)
            a.later([&a, files, at] {
              a.doc.beginGroup("create new track");
              newAudioTrackFor(a, files, at);
              a.doc.endGroup();
            });
      }
    }
    // Browser files (J-16): samples become audio clips at the drop point.
    browserui::DropPlace place;
    place.track = trackAt(m.y);
    // C44-04: in Clips, below the last track (the "each makes a new track" zone) a file makes a new track.
    if (liveLook && m.y >= yOf(nTracks) && nTracks < kMaxPlaylistTracks) place.track = nTracks;
    place.tick = std::max<Tick>(0, timeline::snapTick(mm, tickAt(m.x), snapG, Snap::Cell));  // K-193: cells of the meter map
    // R10-01: files dragged onto a bus track (its lane or its header) are refused, as the Pencil, a
    // move, a paste and a pattern drop are there: a bus track holds its automation only. (It became
    // an audio track holding the file, and its hidden automation tracks came out as rows.)
    if (lg && !newTrackMenu && m.y >= gMin.y && place.track >= 0 && place.track < nTracks && fold.takesNoRegions(place.track)) {
      const ImGuiPayload* cur = ImGui::GetDragDropPayload();
      const char* ftype = !cur ? nullptr : cur->IsDataType("GL_FILE") ? "GL_FILE" : cur->IsDataType("GL_FILES") ? "GL_FILES" : cur->IsDataType("GL_ITEM") ? "GL_ITEM" : nullptr;
      if (ftype) {
        newTrackMenu = true;  // (nothing else takes the drop here)
        const char* why = clipRefused(place.track);
        ImGui::SetTooltip("%s", why);
        a.setHint(why);
        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(ftype, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
          if (pl->Delivery) {
            xdragui::markConsumed();  // (a drop no target takes would open the files)
            a.notify(why, true);
          }
        }
      }
    }
    const ImGuiPayload* filePl = ImGui::GetDragDropPayload();
    if (filePl && !(filePl->IsDataType("GL_FILE") || filePl->IsDataType("GL_ITEM") || filePl->IsDataType("GL_FILES"))) filePl = nullptr;
    if (filePl && groupLane(place.track) && m.y >= gMin.y) {  // checkpoint 52: files go on the group's tracks
      ImGui::SetTooltip("%s", i18n::tr(kGroupLaneHint));
      a.setHint(i18n::tr(kGroupLaneHint));
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    } else if (!newTrackMenu) {
      browserui::acceptFileDrop(a, browse::DropTarget::Playlist, place);
    }
    // J-75: ghost clips where the dropped files will land (sized to the audio
    // when its length is known), gliding from cell to cell like a clip drag.
    if (const browserui::HoverPlan* hp = browserui::hoverPlan(browse::DropTarget::Playlist)) {
      const auto& P = pal();
      dl->PushClipRect(gMin, gMax, true);
      uint64_t key = 0x4A37354700000000ull;  // "J75G"
      for (const xdrop::Step& sp : hp->plan.steps) {
        if (sp.action != browse::DropAction::AudioClip && sp.action != browse::DropAction::MidiPatternClip) continue;
        const double gt = scrollui::glide(key++, static_cast<double>(sp.tick), true, 0.5);
        const double gr = scrollui::glide(key++, static_cast<double>(sp.track), true, 1e-3);
        const int r0 = static_cast<int>(std::floor(gr));
        const float fr = static_cast<float>(gr - r0);
        const float y0 = yOf(r0) + (yOf(r0 + 1) - yOf(r0)) * fr;
        const float h = hOf(r0);
        const float x0 = gMin.x + static_cast<float>(gt) * st.pxPerTick - st.scrollX;
        const float x1 = x0 + std::max(6.0f, static_cast<float>(sp.length) * st.pxPerTick);
        const bool audio = sp.action == browse::DropAction::AudioClip;
        const ImU32 col = lg ? P.accent : audio ? theme::rgb(theme::role::kPaint) : theme::rgb(theme::role::kDraw);  // R09-09
        dl->AddRectFilled(ImVec2(x0, y0 + 1), ImVec2(x1, y0 + h - 1), theme::withAlpha(col, 0.28f), theme::kRadiusSm);
        dl->AddRect(ImVec2(x0, y0 + 1), ImVec2(x1, y0 + h - 1), theme::withAlpha(col, 0.95f), theme::kRadiusSm, 1.5f, 0);
        if (sp.item < hp->items.size() && x1 - x0 > 24)
          theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(x0 + 5, y0 + 4), x1 - x0 - 10, P.text,
                              xdrop::displayName(hp->items[sp.item]).c_str());
      }
      dl->PopClipRect();
    }
    // J-208: a plugin dropped on a track (header or lane) is the track's: an instrument makes it an
    // instrument track (or replaces its instrument), an effect goes onto its mixer track. The track lights up.
    {
      browserui::DropFeedback fb;
      fb.highlight = false;
      browserui::DropPlace pplace = place;
      if (m.y < gMin.y || nTracks == 0) pplace.track = -1;  // the ruler: a new channel, as before
      const int dropTr = pplace.track;
      if (lg && inHead && dropTr >= 0 && dropTr < nTracks) {
        // R13-10: over a header, the hint stands beside it in the lane, centred on the row, so it never
        // covers the next track's name (under the pointer it hid "Weather Level").
        fb.tipAt = true;
        fb.tipX = headX1 + 10, fb.tipY = std::round(yOf(dropTr) + hOf(dropTr) * 0.5f), fb.tipPivotY = 0.5f;
      }
      browserui::acceptPluginDrop(a, browse::DropTarget::Playlist, pplace, &fb);
      if (fb.hovering && dropTr >= 0 && dropTr < nTracks) {
        devGeom.dropTrack = dropTr;
        const float y0 = std::max(yOf(dropTr), gMin.y), y1 = std::min(yOf(dropTr) + hOf(dropTr), gMax.y);
        if (y1 > y0) {
          const ImU32 hot = lg ? P.accent : theme::rgb(theme::role::kDraw);  // R09-09
          ImDrawList* fdl = ImGui::GetWindowDrawList();
          fdl->PushClipRect(ImVec2(rowX0, gMin.y), ImVec2(rowX1, gMax.y), true);
          fdl->AddRectFilled(ImVec2(rowX0, y0), ImVec2(rowX1, y1), theme::withAlpha(hot, 0.16f));
          fdl->AddRect(ImVec2(rowX0 + 1, y0 + 1), ImVec2(rowX1 - 1, y1 - 1), theme::withAlpha(hot, 0.95f), theme::kRadiusSm, 2.0f, 0);
          fdl->PopClipRect();
        }
      }
    }
    ImGui::EndDragDropTarget();
  }

  // Add-tracks menu (header corner "+").
  if (ImGui::BeginPopup("##addtracks")) {
    const int n = nTracks;
    // K-112c (round 41): in Clips the rows are the tracks in use, so new ones are made in use right after
    // them (the song's count would shrink the empty tracks after them away and the new ones stay hidden).
    auto add = [&](int k) {
      if (!liveLook) {
        a.doc.setPlaylistTrackCount(n + k);
        return;
      }
      trackmodes::addUsedTrack(a.doc, k);
    };
    if (ImGui::MenuItem(i18n::tr("Add 1 track"), nullptr, false, n < kMaxPlaylistTracks)) add(1);
    if (ImGui::MenuItem(i18n::tr("Add 8 tracks"), nullptr, false, n + 8 <= kMaxPlaylistTracks)) add(8);
    if (ImGui::MenuItem(i18n::tr("Add 32 tracks"), nullptr, false, n + 32 <= kMaxPlaylistTracks)) add(32);
    // PL-08: a new audio track (on the next free mixer track) or instrument track, on the first free playlist track.
    ImGui::SeparatorText(i18n::tr("Track with a mode"));
    const int free = trackmodes::freeTrack(s);
    if (ImGui::MenuItem(i18n::tr("New audio track"), nullptr, false, free >= 0)) {
      std::string why;
      if (!trackmodes::makeAudioTrack(a.doc, free, kNoId, &why)) a.notify(why, true);
    }
    if (ImGui::BeginMenu(i18n::tr("New instrument track"), free >= 0)) {
      for (const auto& info : pluginCatalog()) {
        if (info.kind != PluginKind::Generator || info.hidden) continue;
        if (ImGui::MenuItem(info.name.c_str())) {
          std::string why;
          const ObjectId ch = trackmodes::newInstrumentTrack(a.doc, free, info.type, &why);
          if (ch == kNoId) a.notify(why, true);
          else a.showNewInstrument(ch);  // K-125
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  // R07-01: the Text tool's in-place region name.
  if (gRegionRename != kNoId) {
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    ImGui::SetNextWindowPos(ImVec2(gRegionRenameAt.x - pad.x, gRegionRenameAt.y - pad.y));
  }
  if (ImGui::BeginPopup("##rgname")) {
    static char rbuf[128];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(rbuf, sizeof rbuf, "%s", gRegionRenameText.c_str());
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(gRegionRenameW);
    if (ImGui::InputText("##rn", rbuf, sizeof rbuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::string n = rbuf;
      const ObjectId id = gRegionRename;
      const int ci = s.clipIndex(id);
      const std::string base = ci >= 0 && static_cast<size_t>(ci) < ccache.name.size() ? *ccache.name[static_cast<size_t>(ci)] : std::string();
      if (n == base) n.clear();  // its pattern's or sound's name: no name of its own
      a.doc.editClip(id, "rename region", [&n](Clip& c) { c.name = n; });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  } else {
    gRegionRename = kNoId;
  }
  // Track popups.
  float renameW = 180;
  if (lg && ImGui::IsPopupOpen("##trname"))  // R01-22: in place, over the name itself
    if (const auto it = lgNameAt.find(renaming); it != lgNameAt.end()) {
      const ImVec2 pad = ImGui::GetStyle().WindowPadding;
      renameW = std::max(80.0f, it->second.z - it->second.x + 6);
      ImGui::SetNextWindowPos(ImVec2(it->second.x - 3 - pad.x, std::round(it->second.y - ImGui::GetFrameHeight() * 0.5f) - pad.y));
    }
  if (ImGui::BeginPopup("##trname")) {
    devGeom.renaming = renaming;
    static char buf[128];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof buf, "%s", renaming >= 0 && renaming < nTracks ? s.tracks[static_cast<size_t>(renaming)].name.c_str() : "");
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(renameW);
    if (ImGui::InputText("##n", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::string n = buf;
      // R02-20: in the Tracks layout the icon is the track's own setting: the one it shows now stays
      // (it was guessed from the name, so a rename changed it).
      const int keepIcon = lg && renaming >= 0 && renaming < nTracks && s.tracks[static_cast<size_t>(renaming)].icon == 0 ? headIcon(renaming) : 0;
      // N52-06: the track's mixer track and instrument follow when they carried its old name (every layout).
      trackmodes::renameTrack(a.doc, renaming, n, [&](PlaylistTrack& t) {
        if (keepIcon > 0) t.icon = keepIcon, t.flIcon = trackicon::toFl(keepIcon);
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trmenu")) {
    playlistTrackMenu(a, renaming);
    if (renaming >= 0 && renaming < nTracks && takes::laneCount(s, renaming) > 0) {  // K-123
      ImGui::Separator();
      const bool shown = s.tracks[static_cast<size_t>(renaming)].takeLanes;
      if (ImGui::MenuItem(i18n::tr("Show take lanes"), nullptr, shown)) {
        const int tr = renaming;
        a.doc.editPlaylistTrack(tr, shown ? "hide take lanes" : "show take lanes", [shown](PlaylistTrack& t) { t.takeLanes = !shown; });
      }
    }
    if (lg && renaming >= 0 && renaming < nTracks) {  // K-112b (b4)
      ImGui::Separator();
      const bool shown = autoLanes.count(renaming) > 0;
      if (ImGui::MenuItem(i18n::tr("Show automation"), keys::label("view.automation"), shown)) setLane(renaming, !shown, std::nullopt);
      if (ImGui::BeginMenu(i18n::tr("Configure track header"))) {  // R04-09: as the reference's header menu
        configureTrackHeaderItems(a);
        ImGui::EndMenu();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##lanemode")) {  // K-112B: the track's automation mode
    if (laneModeMenu < 0 || laneModeMenu >= nTracks) {
      ImGui::CloseCurrentPopup();
    } else {
      const int cur = s.tracks[static_cast<size_t>(laneModeMenu)].autoMode;
      for (int k = 0; k < 5; ++k) {
        if (ImGui::MenuItem(i18n::tr(kAutoModeNames[k]), nullptr, cur == k) && cur != k) {
          const int tr = laneModeMenu;
          const uint8_t mode = static_cast<uint8_t>(k);
          // R05-01: a mode is a choice about the shown parameter: it becomes the track's own (laneSet)
          std::optional<ParamId> shownPid;
          if (const auto it = autoLanes.find(tr); it != autoLanes.end()) shownPid = it->second;
          a.doc.editSong("automation mode", [tr, mode, shownPid](Song& x) {
            if (tr < 0 || tr >= static_cast<int>(x.tracks.size())) return;
            PlaylistTrack& lt = x.tracks[static_cast<size_t>(tr)];
            lt.autoMode = mode;
            if (!lt.laneSet && shownPid) lt.laneSet = true, lt.laneOwner = shownPid->owner, lt.laneIndex = shownPid->index;
          });
        }
        if (ImGui::IsItemHovered()) ui::Hint(i18n::tr(kAutoModeHints[k]));
      }
    }
    ImGui::EndPopup();
  } else {
    laneModeMenu = -1;
  }
  if (ImGui::BeginPopup("##lanepar")) {  // K-112b (b4): what the lane shows
    const auto it = autoLanes.find(laneMenu);
    if (it == autoLanes.end()) {
      ImGui::CloseCurrentPopup();
    } else {
      const ParamId cur = it->second;
      const int laneTr = laneMenu;
      auto item = [&](const ParamId& pid, const char* label) {
        if (ImGui::MenuItem(label, nullptr, pid == cur) && pid != cur) setLane(laneTr, true, pid);
      };
      auto pluginItems = [&](const PluginInfo* info, const std::function<ParamId(int)>& pidOf) {
        if (!info) return;
        int shown = 0;
        for (int k = 0; k < static_cast<int>(info->params.size()) && shown < 64; ++k) {
          const std::string& nm = info->params[static_cast<size_t>(k)].name;
          if (nm.find("(legacy)") != std::string::npos) continue;  // R06-14: kept for old projects, not offered
          ++shown;
          ImGui::PushID(k);
          item(pidOf(k), nm.c_str());
          ImGui::PopID();
        }
      };
      std::set<uint64_t> listed;
      for (const Clip& c : s.playlist)  // what automation clips on this track drive
        if (c.track == laneMenu && c.kind == ClipKind::Automation)
          if (const AutomationClip* ac = s.automation(c.source); ac && !(ParamId{ac->owner, ac->index} == cur) && listed.insert(ParamId{ac->owner, ac->index}.packed()).second) {
            ImGui::PushID(static_cast<int>(listed.size()));
            item(ParamId{ac->owner, ac->index}, paramDisplayName(s, ParamId{ac->owner, ac->index}).c_str());
            ImGui::PopID();
          }
      if (!listed.empty()) ImGui::Separator();
      if (const Channel* lc = logicplug::trackSound(s, laneMenu, nullptr)) {  // R02-02: a free track: the sound it plays
        ImGui::PushID("ch");
        item(channelParam(lc->id, ChannelParam::Volume), i18n::tr("Instrument volume"));  // R04-03: the fader is "Volume" below
        item(channelParam(lc->id, ChannelParam::Pan), i18n::tr("Instrument pan"));
        item(channelParam(lc->id, ChannelParam::Pitch), i18n::tr("Pitch"));
        if (const PluginInfo* info = findPlugin(lc->plugin.type); info && !info->params.empty() && ImGui::BeginMenu(info->name.c_str())) {
          pluginItems(info, [&](int k) { return channelPluginParam(lc->id, k); });
          ImGui::EndMenu();
        }
        ImGui::PopID();
      }
      const MixerTrack* mt = logicplug::trackStrip(s, laneMenu);  // R05-13
      if (mt) {
        ImGui::PushID("mx");
        ImGui::Separator();
        ImGui::TextDisabled("%s", mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(mt->id)))).c_str());
        item(mixerParam(mt->id, MixerParam::Volume), i18n::tr("Volume"));  // R04-03: the channel strip's fader
        item(mixerParam(mt->id, MixerParam::Pan), i18n::tr("Pan"));
        item(mixerParam(mt->id, MixerParam::Mute), i18n::tr("Mute"));  // R05-15: the reference's Main group
        for (const Send& sd : mt->sends)  // and its send levels (the output route is not a send)
          if (!sd.sidechain && &sd != &mt->sends.front())
            if (const int di = s.mixerIndex(sd.to); di >= 0) {
              ImGui::PushID(static_cast<int>(sd.to));
              item(sendParam(mt->id, di), (std::string(i18n::tr("Send to")) + " " + busName(s, static_cast<size_t>(di))).c_str());
              ImGui::PopID();
            }
        for (int k = 0; k < kEffectSlots; ++k) {
          const PluginInfo* info = findPlugin(mt->slots[static_cast<size_t>(k)].plugin.type);
          if (!info) continue;
          ImGui::PushID(k);
          if (ImGui::BeginMenu(info->name.c_str())) {
            item(slotMixParam(mt->id, k), i18n::tr("Mix"));
            ImGui::Separator();
            pluginItems(info, [&](int q) { return slotParam(mt->id, k, q); });
            ImGui::EndMenu();
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      }
      ImGui::Separator();
      // R04-13: the track's automation commands, as the reference's Mix menu has them for the
      // selected track: the shown parameter's curve, or every curve of the track (one undo step;
      // automation data nothing else plays goes with its clips).
      auto dropAutomation = [&](bool onlyShown) {
        const int tr = laneTr;
        const ParamId only = cur;
        a.doc.editSong(onlyShown ? "delete visible automation" : "delete track automation", [&](Song& x) {
          std::set<ObjectId> gone;
          for (const Clip& c : x.playlist)
            if (c.track == tr && c.kind == ClipKind::Automation)
              if (const AutomationClip* ac = x.automation(c.source); ac && (!onlyShown || (ac->owner == only.owner && ac->index == only.index)))
                gone.insert(c.source);
          x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                          [&](const Clip& c) { return c.track == tr && c.kind == ClipKind::Automation && gone.count(c.source); }),
                           x.playlist.end());
          for (const ObjectId id : gone)
            if (ops::clipSourceUses(x, ClipKind::Automation, id) == 0)
              x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(), [id](const AutomationClip& q) { return q.id == id; }),
                                  x.automations.end());
        });
      };
      bool anyShown = false, anyAll = false;
      for (const Clip& c : s.playlist)
        if (c.track == laneTr && c.kind == ClipKind::Automation)
          if (const AutomationClip* ac = s.automation(c.source)) anyAll = true, anyShown = anyShown || (ac->owner == cur.owner && ac->index == cur.index);
      if (ImGui::MenuItem(i18n::tr("Delete visible automation on this track"), nullptr, false, anyShown)) dropAutomation(true);
      if (ImGui::MenuItem(i18n::tr("Delete all automation on this track"), nullptr, false, anyAll)) dropAutomation(false);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Hide automation"))) setLane(laneTr, false, std::nullopt);
    }
    ImGui::EndPopup();
  }
  trackplusui::menu(a);  // K-12
  trackui::inputPopup(a, inputTrack);  // PL-08
  if (ImGui::BeginPopup("##clipmenu")) {
    arrangementClipMenu(a, menuClip);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##lgicon")) {  // R01-23
    if (iconMenuTrack < 0 || iconMenuTrack >= nTracks || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // R03-04: Escape closes it
      ImGui::CloseCurrentPopup();
    } else {
      const int cur = s.tracks[static_cast<size_t>(iconMenuTrack)].icon;
      int ic = cur;
      if (trackicons::picker(cur, &ic)) {
        const int tr = iconMenuTrack;
        a.doc.editPlaylistTrack(tr, "change track icon", [ic](PlaylistTrack& x) { x.icon = ic, x.flIcon = trackicon::toFl(ic); });
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##lgbgmenu")) {  // R01-01 / R01-16: the Tracks layout's background menu (021)
    const bool onTrack = lgBgTrack >= 0 && lgBgTrack < nTracks;
    if (ImGui::MenuItem(i18n::tr("Create pattern region"), nullptr, false, onTrack && !trackLocked(lgBgTrack) && !fold.takesNoRegions(lgBgTrack))) {
      const int tr = lgBgTrack;
      const Tick at = lgBgTick;
      a.doc.beginGroup("create pattern region");
      const ObjectId pid = a.doc.addPattern(ops::nextPatternName(a.doc.song()));
      ObjectId nid = kNoId;
      if (pid != kNoId)
        a.doc.editSong("add clip", [&](Song& x) {
          Clip c = pltools::makeClip(x, pltools::Source{ClipKind::Pattern, pid}, tr, at);
          c.id = x.allocateId();
          x.playlist.push_back(c);
          nid = c.id;
        });
      a.doc.endGroup();
      if (nid != kNoId) st.selected = {nid};
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Paste"), keys::label("playlist.paste"), false, !st.clipboard.empty() && onTrack && !fold.takesNoRegions(lgBgTrack))) {
      // At the clicked bar and track, the regions keeping their spacing (R11-01: each a copy of its
      // own playing the track it lands on; No Overlap trims what lies under it).
      logicplug::pasteRegionsAt(a, lgBgTrack, std::max<Tick>(0, lgBgTick));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Select all"), keys::label("playlist.selectAll"))) {
      st.selected.clear();
      for (const Clip& c : s.playlist)
        if (livelook::arrangementClip(a, c)) st.selected.insert(c.id);  // N53-01
    }
    if (ImGui::MenuItem(i18n::tr("Select all on this track"), nullptr, false, onTrack)) {
      st.selected.clear();
      for (const Clip& c : s.playlist)
        if (c.track == lgBgTrack && livelook::arrangementClip(a, c)) st.selected.insert(c.id);
    }
    if (ImGui::MenuItem(i18n::tr("Select muted regions"))) {
      st.selected.clear();
      for (const Clip& c : s.playlist)
        if (c.muted && livelook::arrangementClip(a, c)) st.selected.insert(c.id);
    }
    if (ImGui::MenuItem(i18n::tr("Deselect all"), keys::label("playlist.deselect"), false, !st.selected.empty())) st.selected.clear();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##markmenu")) {
    markerMenu(a, menuMarker);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##rulermenu")) {
    rulerMenu(a, rulerMenuAt);  // J-85: add a loop marker / marker here
    ImGui::EndPopup();
  }

  // ---------------- shortcuts ----------------
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (keys::pressed("playlist.delete") && st.pickerFocus && st.pickerOpen && !st.pickerSel.empty()) {
      // J-82: Delete in the picker deletes the selected sources (asks when the arrangement uses them).
      requestDeleteSources(a, pickerKind(st.pickerTab), std::vector<ObjectId>(st.pickerSel.begin(), st.pickerSel.end()));
    } else if (lg && keys::pressed("playlist.delete") && st.mqB > st.mqA && !inMeterLane) {
      // R01-18: Delete with a marquee removes the material inside it (the regions are cut at its edges).
      std::set<int> trs;
      for (int t = st.mqT0; t <= st.mqT1; ++t)
        if (!trackLocked(t)) trs.insert(t);
      const Tick r0 = st.mqA, r1 = st.mqB;
      if (!trs.empty())
        a.doc.editSong("delete the marquee", [&](Song& x) {
          const std::set<ObjectId> inside = chop::isolateRange(x, r0, r1, trs);
          x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [&](const Clip& c) { return inside.count(c.id) > 0; }), x.playlist.end());
        });
      st.selected.clear();
      st.mqA = st.mqB = 0;
    } else if (keys::pressed("playlist.delete") && !st.selected.empty() && !inMeterLane) {
      a.doc.beginGroup("delete clips");
      logicplug::keepTrackSounds(a, st.selected);  // R08-07
      for (ObjectId id : std::set<ObjectId>(st.selected))  // N53-01: never the clip grid's clips from here
        if (const Clip* c = s.clip(id); c && !trackLocked(c->track) && livelook::arrangementClip(a, *c)) a.doc.removeClip(id);
      a.doc.endGroup();
      st.selected.clear();
    }
    if (keys::pressed("playlist.selectAll")) {
      st.selected.clear();
      for (const auto& c : s.playlist)
        if ((!lg || !logicplug::laneClip(s, c)) && livelook::arrangementClip(a, c)) st.selected.insert(c.id);
    }
    if (keys::pressed("playlist.deselect")) st.selected.clear();
    // Copies keep every clip setting (kind, source, offsets, audio settings).
    auto duplicate = [&](const std::vector<Clip>& src, const char* label) {
      if (src.empty()) return;
      Tick lo = src[0].start, hi = 0;
      for (const auto& c : src) lo = std::min(lo, c.start), hi = std::max(hi, c.start + s.clipLength(c));
      Tick shift = snapNearest(hi - lo, std::max<Tick>(s.barTicks(), 1));
      if (shift <= 0) shift = s.barTicks();
      if (liveLook) shift = std::max<Tick>(1, hi - lo);  // N48-05: Clips: the copy goes right after the clips' end
      else if (!mm.constant()) {  // K-193: whole bars of the meter map from where the clips start
        const Tick target = mm.nearestBar(hi - (lo - mm.barStartAt(lo)));
        shift = mm.addBars(lo, std::max<int64_t>(1, mm.barIndex(target) - mm.barIndex(lo))) - lo;
      }
      std::set<ObjectId> added;
      a.doc.editSong(label, [&](Song& x) {
        std::vector<Clip> copies = src;
        pltools::regroupCopies(x, copies);  // Q-3: grouped copies form new groups
        for (Clip c : copies) {
          if (!clipValid(x, c)) continue;
          c.id = x.allocateId();
          c.start += shift;
          x.playlist.push_back(c);
          added.insert(c.id);
        }
        if (liveLook) overwriteCovered(x, added, viewO);  // C44-02
      });
      st.selected = added;
    };
    if (keys::pressed("playlist.cut") && !st.selected.empty()) menus::editCut(a);  // R02-22
    if (keys::pressed("playlist.copy") && !st.selected.empty()) {
      st.clipboard.clear();
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
      a.notify(copiedText(st.clipboard.size()));
    }
    if (lg && keys::pressed("playlist.paste") && !st.clipboard.empty()) {
      logicplug::pasteRegions(a);  // R10-08: at the playhead, onto the selected track
    } else if (keys::pressed("playlist.paste") && !st.clipboard.empty()) {
      duplicate(st.clipboard, "paste clips");
      st.clipboard.clear();
      for (const auto& c : a.doc.song().playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
    }
    chopui::shortcuts(a);  // PL-13: chop window, slice at the grid, time selection edits
    if (keys::pressed("playlist.duplicate") && st.selected.empty() && st.selEnd > st.selStart) chopui::duplicateTime(a);
    else if (keys::pressed("playlist.duplicate") && !st.selected.empty()) {  // N50-01: one copy (the time copy selects its clips)
      std::vector<Clip> src;
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) src.push_back(c);
      duplicate(src, "duplicate clips");
    }
    if (!a.typingKeyboard) {  // tool keys
      struct K { const char* cmd; Tool tool; };
      const K toolKeys[] = {{"playlist.toolDraw", Tool::Draw}, {"playlist.toolPaint", Tool::Paint}, {"playlist.toolDelete", Tool::Erase},
                            {"playlist.toolMute", Tool::Mute}, {"playlist.toolSlip", Tool::Slip},   {"playlist.toolSlice", Tool::Slice},
                            {"playlist.toolSelect", Tool::Select}, {"playlist.toolZoom", Tool::Zoom}, {"playlist.toolPlay", Tool::Play},
                            {"playlist.toolWarp", Tool::Warp}};
      for (const auto& k : toolKeys)
        if (keys::pressed(k.cmd)) st.tool = k.tool;
    }
    if (keys::pressed("playlist.marker")) {  // marker at the song position (the Tracks layout: its nearest bar)
      if (layoutui::active() == layout::Preset::Logic) tracksAddMarker(a);
      else ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
    }
    // FL view toggles: Shift+M stretch on resize, Shift+F fade handles, Alt+P picker panel.
    if (keys::pressed("playlist.stretchResize")) {
      st.stretchResize = !st.stretchResize;
      a.notify(st.stretchResize ? "Resizing audio clips stretches them" : "Resizing audio clips trims them");
    }
    if (keys::pressed("playlist.fades")) {
      st.showFades = !st.showFades;
      a.notify(st.showFades ? "Fade and volume handles shown" : "Fade and volume handles hidden");
    }
    if (keys::pressed("playlist.zeroCross")) {  // J-79: Shift+X
      st.zeroCross = !st.zeroCross;
      a.notify(st.zeroCross ? "Audio clip edits land on zero crossings" : "Audio clip edits land exactly where dropped");
    }
    if (keys::pressed("playlist.picker")) st.pickerOpen = !st.pickerOpen;
    if (keys::pressed("playlist.meterChange")) meterui::addAndEdit(a, static_cast<Tick>(a.engine.position()));  // K-179
    if (keys::pressed("playlist.loopMarkers") && liveLook) {
      livelook::loopFromSelection(a);  // checkpoint 56 (N55-04): Clips' Cmd+L: the loop brace from the selection
    } else if (keys::pressed("playlist.loopMarkers")) {  // Shift+T: place loop around the selection (FL), else (J-85) a loop marker at the song position
      if (st.selEnd > st.selStart)
        ops::placeLoop(a.doc, st.selStart, st.selEnd, 0);
      else
        addLoopMarker(a, static_cast<Tick>(a.engine.position()));
    }
    // Quick zooms (Shift+1..3) and zoom on selection (Shift+4).
    if (keys::pressed("playlist.quickZoom1")) st.quickZoom = 1;
    if (keys::pressed("playlist.quickZoom2")) st.quickZoom = 2;
    if (keys::pressed("playlist.quickZoom3")) st.quickZoom = 3;
    if (keys::pressed("playlist.zoomSelection")) st.zoomSelRequest = true;
    // M: mute / unmute the selected clips.
    if (keys::pressed("playlist.mute") && !st.selected.empty()) {
      auto sel = st.selected;
      a.doc.editClips("mute clips", [&](std::vector<Clip>& cs) {
        for (auto& c : cs)
          if (sel.count(c.id)) c.muted = !c.muted;
      });
    }
  }
  ui::EndDockWindow();
  drawClipProperties(a);  // J-8: audio clip properties window
  chopui::draw(a);         // PL-13: chop and stutter window
}

void devArrangementClipMenu(App& a, ObjectId clip) { arrangementClipMenu(a, clip); }  // K-158: --menu arrclip

// K-112 (Live): the clip band's audio clip view. Left, the clip column: the clip's title bar in its
// colour (its name, position, length and loop), then its sample (gain, pitch, fades, reverse) and
// the clip's whole menu under More. Right, the file's details, a ruler in the clip's bars, the loop
// brace and the clip's waveform as the arrangement plays it (a double-click opens it in Loupe).
namespace {
int audioViewFrame = -100;  // dev: the frame the audio clip view last drew
}  // namespace
bool devLiveAudioClipShown() { return ImGui::GetFrameCount() - audioViewFrame <= 1; }

void drawLiveAudioClip(App& a, ObjectId id) {
  const auto& P = pal();
  audioViewFrame = ImGui::GetFrameCount();
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  const Channel* ch = c ? s.channel(c->source) : nullptr;
  if (!c || !ch) return;
  constexpr float kW = 188.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const ImU32 clipCol = theme::content(ch->color);
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
  const Tick len = std::max<Tick>(1, s.clipLength(*c));
  ImGui::PushID(static_cast<int>(id));
  // ---- the column ----
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));  // round 22: 8 px in from the panel, as the device band's cards below
  ImGui::BeginChild("##audiocol", ImVec2(kW - 2, av.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  // K-112c (round 21): labels above their fields, two to a row where they pair, and Loop and
  // Reverse as toggle buttons, as the refs' clip view (037, 038, 041).
  auto barsBeats = [&](Tick t, bool position) {
    char b[48];
    std::snprintf(b, sizeof b, "%lld.%lld", static_cast<long long>(t / bar + (position ? 1 : 0)),
                  static_cast<long long>((t % bar) / beat + (position ? 1 : 0)));
    return std::string(b);
  };
  auto valueRow = [&](const char* label, const std::string& v, const char* hint, int col) {
    livelook::valueBox(a, v.c_str(), livelook::columnRow(a, label, col, 2));  // K-112c (round 23): boxed, as the refs
    ui::Hint(hint);
  };
  if (livelook::columnSection(a, ch->name.c_str(), clipCol, "##foldaclip")) {
    const float nameW = livelook::columnRow(a, i18n::tr("Name"), 0, 1);
    static ObjectId nameFor = kNoId;
    static char name[128] = {};
    if (nameFor != ch->id || ImGui::GetActiveID() != ImGui::GetID("##aname")) {  // follows the clip unless being typed in
      std::snprintf(name, sizeof name, "%s", ch->name.c_str());
      nameFor = ch->id;
    }
    ImGui::SetNextItemWidth(nameW);
    if (livelook::renameFocusNow()) ImGui::SetKeyboardFocusHere();  // checkpoint 49: the audio clip menu's Rename
    ImGui::InputText("##aname", name, sizeof name, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && ch->name != name) a.doc.renameChannel(ch->id, name);
    ui::Hint("The clip's name (its channel's, as the playlist shows it)");
    // N48-14: Start and End are places in the sample (from its start, at the song's tempo), as the
    // reference's clip view: no arrangement or grid storage position.
    auto bbs = [&](Tick t, bool position) {
      const Tick six = std::max<Tick>(1, beat / 4);
      const long long o = position ? 1 : 0;
      char b[96];  // (room for three 64-bit numbers: GCC -Wformat-truncation)
      std::snprintf(b, sizeof b, "%lld . %lld . %lld", static_cast<long long>(t / bar) + o, static_cast<long long>(t % bar / beat) + o,
                    static_cast<long long>(t % beat / six) + o);
      return std::string(b);
    };
    const double tps = s.tempo / 60.0 * static_cast<double>(s.ppq) * std::max(1e-6, c->stretch);
    const Tick inSample = std::max<Tick>(0, static_cast<Tick>(std::llround(std::max(0.0, c->audioStart) * tps)));
    if (c->syncBpm > 0) {
      valueRow(i18n::tr("Start"), bbs(inSample, true), "Where the clip starts in its sample (bars. beats. sixteenths)", 0);
      valueRow(i18n::tr("End"), bbs(inSample + len, true), "Where the clip ends in its sample (bars. beats. sixteenths)", 1);
      valueRow(i18n::tr("Length"), bbs(len, false), "The clip's length (bars. beats. sixteenths)", 0);
    } else {
      // Checkpoint 54 (N53-02): a clip that does not follow the tempo (a one-shot) is timed in seconds, as
      // the reference's unwarped clips: a 0.1 s tick reads 0:00.100, not "0. 0. 0".
      auto mss = [](double sec) {
        const long long ms = std::llround(std::max(0.0, sec) * 1000.0);
        char b[48];
        std::snprintf(b, sizeof b, "%lld:%02lld.%03lld", ms / 60000, ms / 1000 % 60, ms % 1000);
        return std::string(b);
      };
      const double s0 = std::max(0.0, c->audioStart);
      double s1 = s0 + static_cast<double>(len) / tps;
      if (const ClipSource cs = clipSourceIfReady(*ch); cs.sample && !c->loop) s1 = std::min(s1, c->audioEnd > s0 ? c->audioEnd : cs.sample->seconds());
      valueRow(i18n::tr("Start"), mss(s0), "Where the clip starts in its sample (minutes: seconds. milliseconds)", 0);
      valueRow(i18n::tr("End"), mss(s1), "Where the clip ends in its sample (minutes: seconds. milliseconds)", 1);
      valueRow(i18n::tr("Length"), mss(s1 - s0), "The clip's length (minutes: seconds. milliseconds)", 0);
    }
    (void)barsBeats;
    // Checkpoint 57 (N56-09): Loop beside Length (the column fits the detail band's height).
    const float loopW = livelook::columnRow(a, "", 1, 2);
    if (c->start < s.live.areaEnd) {
      // Checkpoint 54 (N53-02): a grid clip's Loop is whether it repeats while launched (off: a one-shot,
      // played once, as the reference's short samples).
      const bool loop = !c->launchOnce;
      if (ui::TextButton(i18n::tr("Loop"), loop, loopW))
        a.doc.editClip(id, loop ? "clip loop off" : "clip loop on", [loop](Clip& x) { x.launchOnce = loop; });
      ui::Hint("On: the clip repeats while it plays; off: it plays once (a one-shot)");
    } else {
      const bool loop = c->loop;
      if (ui::TextButton(i18n::tr("Loop"), loop, loopW)) a.doc.editClip(id, "loop clip", [loop](Clip& x) { x.loop = !loop; });
      ui::Hint("On: the clip repeats its audio to its end");
    }
  }
  ImGui::Dummy(ImVec2(0, 4));
  if (livelook::columnSection(a, i18n::tr("Sample"), 0, "##foldasample")) {
    static uint64_t key = 0x4C410000;
    using clipmulti::Param;
    const float halfW = livelook::columnRow(a, i18n::tr("Gain"), 0, 2);
    float gainDb = static_cast<float>(std::max(clipgain::kMinDb, clipgain::toDb(c->gain)));
    ImGui::SetNextItemWidth(halfW);
    if (ImGui::DragFloat("##again", &gainDb, 0.1f, static_cast<float>(clipgain::kMinDb), static_cast<float>(clipgain::kMaxDb), "%.1f dB"))
      a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = clipgain::fromDb(gainDb <= clipgain::kMinDb ? clipgain::kMinDb - 0.1 : gainDb); }, key);
    ui::Hint("The clip's gain (drag; Ctrl+click to type)");
    livelook::columnRow(a, i18n::tr("Pitch"), 1, 2);
    // K-112c (round 29): a semitone knob with its value beside it, and the cents under it, as the refs'
    // clip view (037, 244). The clip keeps one pitch in cents (semitones x 100 plus the detune).
    const int semis = static_cast<int>(std::lround(c->pitch / 100.0f));
    const float detune = c->pitch - static_cast<float>(semis) * 100.0f;
    {
      const float r = std::floor(ImGui::GetFrameHeight() * 0.5f);
      ParamDesc d;
      d.name = i18n::tr("Pitch");
      d.unit = "st";
      d.min = -24;
      d.max = 24;
      d.def = 0;
      double sv = semis;
      bool rel = false;
      ui::SetNextKnobStep(1);
      if (ui::Knob("##apitchk", &sv, d, r, nullptr, true, &rel)) {
        const float nc = static_cast<float>(std::lround(d.clamp(sv))) * 100.0f + detune;
        a.doc.editClip(id, "clip pitch", [nc](Clip& x) { x.pitch = nc; }, key + 1);
      }
      if (rel) a.doc.endGesture();
      ui::Hint("The clip's pitch in semitones, added to its channel's (drag)");
      ImGui::SameLine(0, 6);  // checkpoint 57 (N56-09): the knob's margins even (the column gap is 6)
      int st = semis;
      ImGui::SetNextItemWidth(std::max(24.0f, halfW - 2 * r - 6));
      if (ImGui::DragInt("##apitch", &st, 0.2f, -24, 24, "%d st"))
        a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = static_cast<float>(std::clamp(st, -24, 24)) * 100.0f + detune; }, key + 1);
      ui::Hint("The clip's pitch in semitones (drag; Ctrl+click to type)");
    }
    livelook::columnRow(a, i18n::tr("Detune"), 0, 2);
    float ct = detune;
    ImGui::SetNextItemWidth(halfW);
    if (ImGui::DragFloat("##adetune", &ct, 0.5f, -50.0f, 50.0f, "%.0f ct"))
      a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = static_cast<float>(semis) * 100.0f + std::clamp(ct, -50.0f, 50.0f); }, key + 1);
    ui::Hint("The clip's fine pitch in cents (drag; Ctrl+click to type)");
    // Checkpoint 57 (N56-09): Reverse beside Detune, so the whole Sample section fits the band.
    if (ui::TextButton(i18n::tr("Reverse"), c->reverse, livelook::columnRow(a, "", 1, 2))) a.doc.editClip(id, "reverse clip", [&a](Clip& x) { toggleClipReverse(a.doc.song(), x); });
    ui::Hint("Plays the clip backwards (keeps the same audio)");
    livelook::columnRow(a, i18n::tr("Fade in"), 0, 2);
    float fi = static_cast<float>(c->fadeIn * 1000.0), fo = static_cast<float>(c->fadeOut * 1000.0);
    ImGui::SetNextItemWidth(halfW);
    if (ImGui::DragFloat("##afi", &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0, x.fadeInManual = true; }, key + 2);
    livelook::columnRow(a, i18n::tr("Fade out"), 1, 2);
    ImGui::SetNextItemWidth(halfW);
    if (ImGui::DragFloat("##afo", &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0, x.fadeOutManual = true; }, key + 3);
  }
  ImGui::Dummy(ImVec2(0, 4));
  if (ui::TextButton(i18n::tr("More..."), false, ImGui::GetContentRegionAvail().x)) ImGui::OpenPopup("##aclipmore");
  ui::Hint("The clip's whole menu: warp markers, the audio tools, export, Loupe, the channel's settings");
  if (ImGui::BeginPopup("##aclipmore")) {
    arrangementClipMenu(a, id);
    ImGui::EndPopup();
  }
  livelook::columnScrollEdges();
  ImGui::EndChild();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(p0.x + kW - 1.5f, p0.y), ImVec2(p0.x + kW - 1.5f, p0.y + av.y), P.line);
  c = s.clip(id);  // (an edit above may have moved the clip list)
  if (!c) {
    ImGui::PopID();
    return;
  }
  // ---- the editor ----
  const float ex0 = p0.x + kW + 6, ex1 = p0.x + av.x - 4, ey1 = p0.y + av.y;
  if (ex1 - ex0 < 80) {
    ImGui::PopID();
    return;
  }
  // The file: its name, rate, channels and length (the refs' line over the waveform).
  const float barH = 28.0f;
  {
    const ClipSource src = clipSourceIfReady(*ch);
    std::string info = ch->plugin.data.empty() ? std::string(i18n::tr("No sample")) : ch->plugin.data;
    if (const size_t k = info.find_last_of("/\\"); k != std::string::npos) info = info.substr(k + 1);
    if (src.sample) {
      char b[96];
      std::snprintf(b, sizeof b, "   %.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", src.sample->sampleRate / 1000.0,
                    src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"), src.sample->seconds());
      info += b;
    }
    dl->AddRectFilled(ImVec2(ex0 - 6, p0.y), ImVec2(p0.x + av.x, p0.y + barH), P.surface900);
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    theme::textEllipsis(dl, f, fs, ImVec2(ex0 + 4, theme::textTop(f, fs, p0.y, barH)), ex1 - ex0 - 8, P.textDim, info.c_str());
  }
  // Checkpoint 55 (N54-07): the whole sample, as the reference's sample display: the ruler in the
  // sample's own bars (1 at its start), the clip's region on it (Start and End markers, the loop
  // brace from Start to End) with the rest of the sample dimmed, then the waveform.
  const float rulerY0 = p0.y + barH, rulerH = 22.0f, braceH = 12.0f;
  const float wy0 = rulerY0 + rulerH + braceH, wy1 = ey1 - 4;
  const ClipSource wsrc = clipSourceIfReady(*ch);
  const double sampleSec = wsrc.sample ? wsrc.sample->seconds() : 0.0;
  const ClipRate rate = clipRate(*ch, *c);
  // Song ticks per second of the sample as the clip plays it (warped: at its own tempo).
  const double tpsSample = c->syncBpm > 0 ? c->syncBpm / 60.0 * static_cast<double>(s.ppq) * rate.timeFactor
                                          : s.tempo / 60.0 * static_cast<double>(s.ppq) / std::max(1e-6, rate.speed);
  const Tick regionAt = std::max<Tick>(0, static_cast<Tick>(std::llround(std::max(0.0, c->audioStart) * tpsSample)));
  const Tick sampleLen = sampleSec > 0 ? std::max<Tick>(1, static_cast<Tick>(std::llround(sampleSec * tpsSample))) : regionAt + len;
  const Tick regionEnd = c->loop ? std::min(sampleLen, c->audioEnd > c->audioStart ? static_cast<Tick>(std::llround(c->audioEnd * tpsSample)) : sampleLen)
                                 : std::min(sampleLen, regionAt + len);
  const Tick viewLen = std::max<Tick>(1, std::max(sampleLen, regionEnd));
  const float pxPerTick = (ex1 - ex0) / static_cast<float>(viewLen);
  auto xOf = [&](Tick t) { return ex0 + static_cast<float>(t) * pxPerTick; };
  dl->AddRectFilled(ImVec2(ex0 - 6, rulerY0), ImVec2(p0.x + av.x, rulerY0 + rulerH + braceH), P.dark ? P.field : P.surface950);
  dl->AddRectFilled(ImVec2(ex0 - 6, wy0), ImVec2(p0.x + av.x, ey1), P.dark ? P.surface950 : P.paper);
  dl->PushClipRect(ImVec2(ex0, rulerY0), ImVec2(ex1, ey1), true);
  Tick step = pxPerTick * static_cast<float>(beat) >= 10.0f ? beat : bar;
  while (pxPerTick * static_cast<float>(step) < 10.0f && step < viewLen) {
    step *= 2;
  }
  const Tick labelEvery = std::max<Tick>(1, static_cast<Tick>(std::ceil(28.0 / std::max(1e-6, static_cast<double>(pxPerTick) * static_cast<double>(bar)))));
  for (Tick t = 0; t <= viewLen; t += step) {
    const float x = std::round(xOf(t));
    const bool isBar = t % bar == 0;
    dl->AddLine(ImVec2(x, rulerY0 + rulerH - (isBar ? 10.0f : 5.0f)), ImVec2(x, rulerY0 + rulerH - 1), isBar ? P.lineStrong : P.line);
    if (isBar) {
      if ((t / bar) % 2 == 1 && x < ex1) {
        dl->AddRectFilled(ImVec2(x, wy0), ImVec2(std::round(xOf(std::min(viewLen, t + bar))), wy1), theme::withAlpha(P.text, 0.03f));
      }
      dl->AddLine(ImVec2(x, wy0), ImVec2(x, wy1), P.gridBar);
      if ((t / bar) % labelEvery == 0) {
        const std::string lbl = std::to_string(t / bar + 1);
        dl->AddText(livelook::numFont(a), theme::kFontLabel, ImVec2(x + 4, theme::textTop(livelook::numFont(a), theme::kFontLabel, rulerY0, rulerH - 6)), P.text,
                    lbl.c_str());
      }
    } else {
      dl->AddLine(ImVec2(x, wy0), ImVec2(x, wy1), P.gridBeat);
    }
  }
  const float xs = std::round(xOf(regionAt)), xe = std::round(xOf(std::max(regionAt + 1, regionEnd)));
  {  // the loop brace: the clip's region of the sample, in its colour
    const float by0 = rulerY0 + rulerH + 2, by1 = by0 + braceH - 5;
    dl->AddRectFilled(ImVec2(xs, by0), ImVec2(xe, by1), theme::withAlpha(clipCol, 0.85f));
    dl->AddTriangleFilled(ImVec2(xs, by0), ImVec2(xs + 5, (by0 + by1) * 0.5f), ImVec2(xs, by1), theme::contrastText(clipCol));
    dl->AddTriangleFilled(ImVec2(xe, by0), ImVec2(xe - 5, (by0 + by1) * 0.5f), ImVec2(xe, by1), theme::contrastText(clipCol));
  }
  dl->AddLine(ImVec2(ex0, (wy0 + wy1) * 0.5f), ImVec2(ex1, (wy0 + wy1) * 0.5f), theme::withAlpha(P.text, 0.12f));
  static TempoMap tm;
  tm.build(s);
  const ImU32 waveCol = P.dark ? theme::mix(clipCol, P.text, 0.35f) : theme::mix(clipCol, P.ink, 0.35f);
  {  // the whole sample, as one pass from its start (the clip's own gain and rate)
    Clip whole = *c;
    whole.start = 0;
    whole.audioStart = 0;
    whole.audioEnd = 0;
    whole.loop = false;
    whole.warp.clear();
    whole.length = sampleLen;
    whole.fadeIn = whole.fadeOut = 0;
    static TempoMap flat;  // (one tempo: the sample's length as the clip plays it)
    static Song one;
    one.tempo = s.tempo;
    one.ppq = s.ppq;
    flat.build(one);
    drawClipWave(dl, one, *ch, whole, flat, ImVec2(ex0, wy0 + 4), ImVec2(xOf(sampleLen), wy1 - 4), ex0, pxPerTick, ex0, ex1, waveCol);
  }
  // Outside the region: dimmed; the Start and End markers at its edges, as the reference's flags.
  const ImU32 dim = theme::withAlpha(P.dark ? P.surface950 : P.paper, 0.62f);
  if (xs > ex0) {
    dl->AddRectFilled(ImVec2(ex0, wy0), ImVec2(xs, wy1), dim);
  }
  if (xe < ex1) {
    dl->AddRectFilled(ImVec2(xe, wy0), ImVec2(ex1, wy1), dim);
  }
  auto marker = [&](float x, bool start, const char* label) {
    const ImU32 mc = theme::mix(clipCol, P.text, 0.2f);
    dl->AddLine(ImVec2(x + (start ? 0.5f : -0.5f), wy0), ImVec2(x + (start ? 0.5f : -0.5f), wy1), mc, 1.0f);
    const float fh = 12.0f, fw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, label).x + 8;
    const ImVec2 f0(start ? x : x - fw, wy0), f1(start ? x + fw : x, wy0 + fh);
    dl->AddRectFilled(f0, f1, mc);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(f0.x + 4, f0.y + std::round((fh - theme::kFontLabel) * 0.5f) - 1), theme::contrastText(mc), label);
  };
  marker(xs, true, i18n::tr("S"));
  marker(xe, false, i18n::tr("E"));
  if (a.engine.mode() == PlayMode::Song) {  // the song's position inside the clip, on the sample
    const double pos = a.engine.position();
    if (pos >= static_cast<double>(c->start) && pos < static_cast<double>(c->start + len)) {
      double in = pos - static_cast<double>(c->start);
      const double reg = static_cast<double>(std::max<Tick>(1, regionEnd - regionAt));
      if (c->loop && in >= reg) {
        in = std::fmod(in, reg);
      }
      const float x = std::round(xOf(regionAt) + static_cast<float>(in) * pxPerTick);
      dl->AddLine(ImVec2(x, rulerY0), ImVec2(x, wy1), P.playhead);
    }
  }
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(ImVec2(ex0, wy0));
  ImGui::InvisibleButton("##awave", ImVec2(ex1 - ex0, std::max(1.0f, wy1 - wy0)));
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ch->plugin.data.empty())
    packs::openAudioEditor(a, ch->plugin.data, ch->id);
  ui::Hint("The clip's sample: the clip plays from S to E (the brace). Double-click: edit it in Loupe");
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(av);
  ImGui::PopID();
}

}  // namespace gl
