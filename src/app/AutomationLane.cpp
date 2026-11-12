// Gridline - the piano roll's pattern automation lane (see AutomationLane.h).
#include "AutomationLane.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include "../core/Automation.h"
#include "../core/AutomationEdit.h"
#include "../core/I18n.h"
#include "../core/PatternAutomation.h"
#include "../core/Timeline.h"
#include "App.h"
#include "AutomationEditor.h"
#include "Keys.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

namespace ae = autoedit;
using theme::pal;

namespace {

ParamId laneTarget{kNoId, 0};
std::vector<AutoPoint> clipboardPts;

// Lane gestures and state (one piano roll).
struct Lane {
  ObjectId pattern = kNoId;
  ParamId target;
  ae::Selection sel;
  bool focus = false;  // the editing keys act on the lane's points
  enum G { None, Paint, Line, RightPending, Move, Box, Erase } g = None;
  ImVec2 start;
  double tickA = 0;  // press position (pattern ticks, fractional)
  float yA = 0;
  double lastTick = -1;  // paint: the pointer last frame
  float lastY = 0, lastX = 0;
  Tick paintLo = 0, paintHi = -1;  // painted cells (linear / smooth strokes are simplified on release)
  ae::Points base;  // the lane at the press
  int anchor = -1, menuPoint = -1;
  uint64_t key = 0;
  double pointerTick = 0;  // where the pointer last was over the lane (paste)
  bool openPointMenu = false, openSimplify = false;
  float tol = 1.5f;      // simplify tolerance (% of the range)
  int simplifyShape = 0;  // 0 linear, 1 smooth
  std::optional<ae::Points> preview;
  ImVec2 mn, mx, chip0, chip1;  // last drawn rects (dev check)
} L;
uint64_t counter = 1;

std::string fmtValue(const Song& s, ParamId pid, double normalized) {
  auto d = describeParam(s, pid);
  if (!d) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", normalized * 100.0);
    return b;
  }
  return d->format(d->clamp(d->fromNormalized(std::clamp(normalized, 0.0, 1.0))));
}

const char* drawShapeName(AutoShape s) {
  return s == AutoShape::Hold ? i18n::N_("Stepped") : s == AutoShape::Smooth ? i18n::N_("Smooth") : i18n::N_("Linear");
}
const char* drawShapeShort(AutoShape s) { return s == AutoShape::Hold ? "STEP" : s == AutoShape::Smooth ? "SMOOTH" : "LINE"; }

// The lane's points of `pid` in `pattern` (empty when it has none).
ae::Points lanePoints(const Song& s, ObjectId pattern, ParamId pid) {
  const Pattern* p = s.pattern(pattern);
  const PatternAutomation* l = p ? findPatternLane(*p, pid.owner, pid.index) : nullptr;
  return l ? l->points : ae::Points{};
}

// One edit of the lane's points (an empty result keeps the lane as it was:
// removing a lane is the menu's "Delete this lane").
void editLane(App& a, ObjectId pattern, ParamId pid, const char* label, const std::function<void(ae::Points&)>& fn, uint64_t key = 0) {
  a.doc.editPattern(pattern, label, [&](Pattern& x) {
    const PatternAutomation* l = findPatternLane(x, pid.owner, pid.index);
    ae::Points p = l ? l->points : ae::Points{};
    fn(p);
    if (!p.empty()) setPatternLane(x, pid.owner, pid.index, std::move(p));
  }, key);
}

// The selection belongs to one lane; a different lane or pattern starts empty,
// and indices an outside edit (undo, a collaborator) removed are dropped.
void syncSelection(ObjectId pattern, size_t n) {
  if (L.pattern != pattern || !(L.target == laneTarget)) {
    L.sel.clear();
    L.pattern = pattern;
    L.target = laneTarget;
    L.g = Lane::None;
    L.preview.reset();
  }
  for (auto it = L.sel.begin(); it != L.sel.end();) it = *it >= 0 && static_cast<size_t>(*it) < n ? std::next(it) : L.sel.erase(it);
}

void copySel(App& a, const ae::Points& pts) {
  if (L.sel.empty()) return;
  clipboardPts = ae::copyPoints(pts, L.sel);
  a.notify(std::to_string(clipboardPts.size()) + " points copied");
}
void deleteSel(App& a, ObjectId pattern, const char* label) {
  if (L.sel.empty()) return;
  const ae::Selection sel = L.sel;
  editLane(a, pattern, laneTarget, label, [&](ae::Points& p) { ae::deletePoints(p, sel, 1); });
  L.sel.clear();
}
// Paste over the clipboard's span (the lane's points there are replaced): at
// the pattern's playhead while it plays, else after the selection, else at the
// pointer.
void pasteHere(App& a, ObjectId pattern, const ae::Points& pts, Tick grid) {
  if (clipboardPts.empty()) return;
  Tick at;
  if (a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == pattern)
    at = static_cast<Tick>(a.engine.position());
  else if (!L.sel.empty() && static_cast<size_t>(*L.sel.rbegin()) < pts.size())
    at = pts[static_cast<size_t>(*L.sel.rbegin())].pos + std::max<Tick>(1, grid);
  else
    at = std::max<Tick>(0, grid > 1 ? snapDown(static_cast<Tick>(L.pointerTick), grid) : static_cast<Tick>(L.pointerTick));
  ae::Selection added;
  const ae::Points clip = clipboardPts;
  editLane(a, pattern, laneTarget, "paste points", [&](ae::Points& p) { added = ae::pastePoints(p, clip, at, true); });
  L.sel = added;
}
void quantizeSel(App& a, ObjectId pattern, Tick grid) {
  const ae::Selection t = L.sel;
  ae::Selection out;
  editLane(a, pattern, laneTarget, "quantize points", [&](ae::Points& p) { out = ae::quantize(p, t, std::max<Tick>(1, grid)); });
  if (!L.sel.empty()) L.sel = out;
}
void setSelShape(App& a, ObjectId pattern, AutoShape sh, const ae::Selection& t) {
  editLane(a, pattern, laneTarget, "curve mode", [&](ae::Points& p) {
    ae::setShape(p, t, sh);
    if (sh == AutoShape::Hold || sh == AutoShape::Curve || sh == AutoShape::Smooth) ae::setTension(p, t, 0.0f);
  });
}

// The curve items of a menu for the points `t`: the three common ones, then every curve.
void curveItems(App& a, ObjectId pattern, AutoShape cur, const ae::Selection& t) {
  for (AutoShape sh : {AutoShape::Hold, AutoShape::Curve, AutoShape::Smooth})
    if (ImGui::MenuItem(i18n::tr(drawShapeName(sh)), nullptr, sh == cur)) setSelShape(a, pattern, sh, t);
  if (ImGui::BeginMenu(i18n::tr("More curves"))) {
    for (AutoShape sh : ae::menuOrder())
      if (ImGui::MenuItem(autoShapeName(sh), nullptr, sh == cur)) setSelShape(a, pattern, sh, t);
    ImGui::EndMenu();
  }
}

// The common curve of the points `t` (Count: mixed or none).
AutoShape selShape(const ae::Points& pts, const ae::Selection& t) {
  AutoShape cur = AutoShape::Count;
  bool first = true;
  for (int i : t) {
    if (static_cast<size_t>(i) >= pts.size()) continue;
    if (first) cur = pts[static_cast<size_t>(i)].shape, first = false;
    else if (pts[static_cast<size_t>(i)].shape != cur) return AutoShape::Count;
  }
  return cur;
}

void simplifyPopup(App& a, ObjectId pattern, const ae::Points& pts) {
  if (L.openSimplify) {
    ImGui::OpenPopup("##lanesimplify");
    ImGui::SetNextWindowPos(ImVec2(L.mx.x - 8, L.mn.y - 10), ImGuiCond_Always, ImVec2(1, 1));
    L.openSimplify = false;
  }
  if (!ImGui::BeginPopup("##lanesimplify")) {
    L.preview.reset();
    return;
  }
  const auto& P = pal();
  ImGui::PushFont(a.fontBold ? a.fontBold : ImGui::GetFont(), 0.0f);
  ImGui::TextUnformatted(i18n::tr("Simplify points"));
  ImGui::PopFont();
  ImGui::Spacing();
  const float w = 260;
  ImGui::SetNextItemWidth(w);
  ImGui::SliderFloat("##tol", &L.tol, 0.1f, 20.0f, i18n::tr("Tolerance  %.1f%%"), ImGuiSliderFlags_Logarithmic);
  const char* shapes[2] = {i18n::N_("Linear"), i18n::N_("Smooth")};
  const char* hints[2] = {i18n::N_("Straight lines between the kept points"), i18n::N_("Eased curves between the kept points")};
  ui::Segmented("##shape", shapes, 2, &L.simplifyShape, w / 2, 0, hints);
  ae::Points pv = pts;
  const ae::Selection sel = L.sel;
  const AutoShape result = L.simplifyShape == 1 ? AutoShape::Smooth : AutoShape::Curve;
  ae::thinRange(pv, sel, L.tol / 100.0, result);
  L.preview = pv;
  ImGui::Spacing();
  ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%zu \xE2\x86\x92 %zu points (%s)"), pts.size(), pv.size(),
                     sel.size() >= 2 ? i18n::tr("the selection") : i18n::tr("the whole lane"));
  ImGui::Spacing();
  const float bw = 88;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
  if (ui::TextButton("Cancel", false, bw) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    L.preview.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::PrimaryButton("Apply", bw, "Keep the simplified points (one undo step)") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    simplifyAutomationLane(a, pattern, L.tol / 100.0, result);
    L.preview.reset();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void pointMenu(App& a, ObjectId pattern, const ae::Points& pts, const AutomationClip& rng) {
  if (L.openPointMenu) {
    ImGui::OpenPopup("##lanepoint");
    L.openPointMenu = false;
  }
  if (!ImGui::BeginPopup("##lanepoint")) return;
  const Song& s = a.doc.song();
  const int k = L.menuPoint;
  if (k >= 0 && static_cast<size_t>(k) < pts.size()) {
    ImGui::TextDisabled("%s", fmtValue(s, laneTarget, automationToParam(rng, pts[static_cast<size_t>(k)].value)).c_str());
    if (L.sel.size() > 1) ImGui::TextDisabled(i18n::tr("%zu points selected"), L.sel.size());
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Delete"), nullptr, false, pts.size() > 1)) deleteSel(a, pattern, "delete points");
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("Curve"));
    curveItems(a, pattern, selShape(pts, L.sel), L.sel);
    if (ImGui::MenuItem(i18n::tr("Reset tension"))) {
      const ae::Selection t = L.sel;
      editLane(a, pattern, laneTarget, "reset tension", [&](ae::Points& p) { ae::setTension(p, t, 0.0f); });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Copy"), keys::label("piano.copy"))) copySel(a, pts);
    if (ImGui::MenuItem(i18n::tr("Simplify points..."), nullptr, false, pts.size() > 2)) L.openSimplify = true;
    if (ImGui::MenuItem(i18n::tr("Open in the automation editor"))) openPatternAutomation(a, pattern, laneTarget);
  }
  ImGui::EndPopup();
}

}  // namespace

// =================================================================== API ====

ParamId& pianoAutomationLane() { return laneTarget; }
std::vector<AutoPoint>& automationClipboard() { return clipboardPts; }
void automationLaneBlur() { L.focus = false; }

AutoShape laneDrawShape(App& a) {
  const int v = static_cast<int>(std::lround(a.settings.getNumber("piano.autoDrawCurve")));
  return v == 1 ? AutoShape::Curve : v == 2 ? AutoShape::Smooth : AutoShape::Hold;
}
void setLaneDrawShape(App& a, AutoShape s) { a.settings.set("piano.autoDrawCurve", s == AutoShape::Curve ? 1 : s == AutoShape::Smooth ? 2 : 0); }

std::string patternLaneLabel(App& a) {
  if (laneTarget.owner == kNoId) return "";
  const Song& s = a.doc.song();
  auto d = describeParam(s, laneTarget);
  return d ? d->name : "?";
}

void patternLaneMenu(App& a, ObjectId pattern, ObjectId channel) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Pattern automation (events)"));
  for (const auto& l : p->automation) {
    ParamId pid{l.owner, l.index};
    ImGui::PushID(static_cast<int>(pid.packed() & 0x7FFFFFFF));
    if (ImGui::Selectable(patternAutoName(s, l).c_str(), laneTarget == pid)) laneTarget = pid;
    ImGui::PopID();
  }
  if (ImGui::BeginMenu(i18n::tr("Add lane for"))) {
    auto add = [&](ParamId pid) {
      if (ImGui::MenuItem(paramDisplayName(s, pid).c_str(), nullptr, findPatternLane(*p, pid.owner, pid.index) != nullptr)) editPatternEvents(a, pid);
    };
    int shown = 0;
    for (ParamId pid : listParams(s))
      if (pid.owner == channel && ++shown < 120) add(pid);
    ImGui::Separator();
    for (ParamId pid : listParams(s))
      if (pid.owner == kSongId) add(pid);
    if (const Channel* ch = s.channel(channel))
      if (ch->mixerTrack != kNoId) {
        ImGui::Separator();
        int n = 0;
        for (ParamId pid : listParams(s))
          if (pid.owner == ch->mixerTrack && ++n < 60) add(pid);
      }
    ImGui::EndMenu();
  }
  const PatternAutomation* lane = laneTarget.owner != kNoId ? findPatternLane(*p, laneTarget.owner, laneTarget.index) : nullptr;
  if (lane) {
    // K-171: the lane's points, as in the automation clip editor.
    const ae::Points pts = lane->points;
    syncSelection(pattern, pts.size());
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("Points"));
    if (ImGui::MenuItem(i18n::tr("Select all points"), keys::label("piano.selectAll"))) {
      for (int i = 0; i < static_cast<int>(pts.size()); ++i) L.sel.insert(i);
      L.focus = true;
    }
    if (ImGui::MenuItem(i18n::tr("Copy points"), keys::label("piano.copy"), false, !L.sel.empty())) copySel(a, pts);
    if (ImGui::MenuItem(i18n::tr("Paste points"), keys::label("piano.paste"), false, !clipboardPts.empty()))
      pasteHere(a, pattern, pts, std::max<Tick>(1, a.piano.effGrid));
    if (ImGui::MenuItem(i18n::tr("Delete selected points"), keys::label("piano.delete"), false, !L.sel.empty() && pts.size() > 1))
      deleteSel(a, pattern, "delete points");
    if (ImGui::MenuItem(i18n::tr("Quantize points"), keys::label("piano.quickQuantize"))) quantizeSel(a, pattern, std::max<Tick>(1, a.piano.effGrid));
    if (ImGui::BeginMenu(L.sel.empty() ? i18n::tr("Curve of every point") : i18n::tr("Curve of the selection"))) {
      ae::Selection t = L.sel;
      if (t.empty())
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) t.insert(i);
      curveItems(a, pattern, selShape(pts, t), t);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Simplify points..."), nullptr, false, pts.size() > 2)) L.openSimplify = true;
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Open in the automation editor"))) openPatternAutomation(a, pattern, laneTarget);  // J-173
    if (ImGui::MenuItem(i18n::tr("Delete this lane"))) {
      ParamId pid = laneTarget;
      a.doc.editPattern(pattern, "delete event lane", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, {}); });
      laneTarget = ParamId{};
    }
  }
}

bool drawLaneCurveChip(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, ObjectId pattern, bool canvasHovered, bool clicked) {
  const auto& P = pal();
  L.chip0 = mn, L.chip1 = mx;
  const AutoShape ds = laneDrawShape(a);
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool hov = canvasHovered && m.x >= mn.x && m.x < mx.x && m.y >= mn.y && m.y < mx.y;
  const bool open = ImGui::IsPopupOpen("##lanecurve");
  dl->AddRectFilled(mn, mx, hov || open ? P.surface800 : theme::withAlpha(P.surface800, 0.55f), theme::kRadius);
  dl->AddRect(mn, mx, hov || open ? P.lineStrong : P.line, theme::kRadius);
  // A small vector glyph of the curve, then its name and a chevron.
  const float gh = std::round((mx.y - mn.y) * 0.5f), gx = mn.x + 5, gy = std::round((mn.y + mx.y) * 0.5f);
  const ImU32 gc = theme::litRole(theme::role::kGraph);
  const ImVec2 g0(gx, gy + gh * 0.5f), g1(gx + 9, gy - gh * 0.5f);
  if (ds == AutoShape::Hold) {
    const ImVec2 q[4] = {g0, ImVec2(gx + 4.5f, g0.y), ImVec2(gx + 4.5f, g1.y), g1};
    dl->AddPolyline(q, 4, gc, 0, 1.4f);
  } else if (ds == AutoShape::Smooth) {
    ImVec2 q[9];
    for (int i = 0; i < 9; ++i) {
      const float f = static_cast<float>(i) / 8.0f;
      q[i] = ImVec2(gx + 9 * f, g0.y - (g0.y - g1.y) * (0.5f - 0.5f * std::cos(3.14159265f * f)));
    }
    dl->AddPolyline(q, 9, gc, 0, 1.4f);
  } else {
    dl->AddLine(g0, g1, gc, 1.4f);
  }
  theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(gx + 13, mn.y + (mx.y - mn.y - theme::kFontLabel) * 0.5f), mx.x - gx - 13 - 12,
                      hov || open ? P.text : P.textDim, drawShapeShort(ds));
  ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(mx.x - 7, gy), 8, hov || open ? P.text : P.textFaint);
  if (hov) a.setHint(std::string(i18n::tr("Curve the drawing tools write")) + ": " + i18n::tr(drawShapeName(ds)) + "   " +
                     i18n::tr("(click: choose; also sets the curve of selected points)"));
  bool took = false;
  if (hov && clicked) {
    ImGui::OpenPopup("##lanecurve");
    took = true;
  }
  if (ImGui::BeginPopup("##lanecurve")) {
    const Song& s = a.doc.song();
    const ae::Points pts = lanePoints(s, pattern, laneTarget);
    syncSelection(pattern, pts.size());
    ImGui::TextDisabled("%s", i18n::tr("Draw with"));
    for (AutoShape sh : {AutoShape::Hold, AutoShape::Curve, AutoShape::Smooth})
      if (ImGui::MenuItem(i18n::tr(drawShapeName(sh)), nullptr, sh == ds)) setLaneDrawShape(a, sh);
    if (!L.sel.empty()) {
      ImGui::Separator();
      ImGui::TextDisabled(i18n::tr("Set the %zu selected points to"), L.sel.size());
      ImGui::PushID("sel");
      curveItems(a, pattern, selShape(pts, L.sel), L.sel);
      ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Simplify points..."), nullptr, false, pts.size() > 2)) L.openSimplify = true;
    ImGui::EndPopup();
  }
  return took;
}

bool automationLaneKeys(App& a, ObjectId pattern, Tick grid) {
  if (!L.focus || laneTarget.owner == kNoId) return false;
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  const PatternAutomation* lane = p ? findPatternLane(*p, laneTarget.owner, laneTarget.index) : nullptr;
  if (!lane) {
    L.focus = false;
    return false;
  }
  const ae::Points pts = lane->points;
  syncSelection(pattern, pts.size());
  if (keys::pressed("piano.delete") && !L.sel.empty() && pts.size() > 1) deleteSel(a, pattern, "delete points");
  if (keys::pressed("piano.selectAll"))
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) L.sel.insert(i);
  if (keys::pressed("piano.deselect")) L.sel.clear();
  if (keys::pressed("piano.copy")) copySel(a, pts);
  if (keys::pressed("piano.cut") && !L.sel.empty()) {
    copySel(a, pts);
    deleteSel(a, pattern, "cut points");
  }
  if (keys::pressed("piano.paste")) pasteHere(a, pattern, pts, grid);
  if (keys::pressed("piano.quickQuantize")) quantizeSel(a, pattern, grid);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    keys::claimEscape();
    if (!L.sel.empty()) L.sel.clear();
    else L.focus = false;
  }
  return true;
}

void drawPatternAutomationLane(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, ObjectId pattern, const std::function<float(double)>& xOf,
                               const std::function<double(float)>& tickAt, Tick grid, bool canvasHovered, bool canvasActive, Tool tool) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  const ParamId pid = laneTarget;
  const PatternAutomation* lane = findPatternLane(*p, pid.owner, pid.index);
  const ae::Points pts = lane ? lane->points : ae::Points{};
  syncSelection(pattern, pts.size());
  L.mn = mn, L.mx = mx;
  // J-173: drawn and edited as the parameter moves (the clip's range applied; a new lane: 0..1).
  const AutomationClip rng = lane ? *lane : patternLane(pid.owner, pid.index, {});
  const double span = static_cast<double>(rng.max - rng.min);
  // Q-10: drawn values glide to edits (display only; the lane already holds them).
  ae::Points shown = pts;
  for (auto& q : shown) {
    q.value = static_cast<float>(scrollui::glideBar((static_cast<uint64_t>(pattern) << 44) ^ (static_cast<uint64_t>(pid.owner) << 28) ^
                                                        (static_cast<uint64_t>(pid.index) << 20) ^ (static_cast<uint64_t>(q.pos) * 0x9E3779B1ull),
                                                    automationToParam(rng, q.value)));
    q.fine = 0.0;
  }
  const float h = std::max(1.0f, mx.y - mn.y);
  auto yOf = [&](double v) { return mx.y - static_cast<float>(v) * h; };
  auto dispAt = [&](float y) { return std::clamp(static_cast<double>((mx.y - y) / h), 0.0, 1.0); };
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool inLane = m.x >= mn.x && m.x < mx.x && m.y >= mn.y - 4 && m.y < mx.y + 4;  // (above: the splitter)
  grid = std::max<Tick>(1, grid);
  auto snapT = [&](double t) { return std::max<Tick>(0, grid > 1 ? snapNearest(static_cast<Tick>(std::llround(t)), grid) : static_cast<Tick>(std::llround(t))); };
  const AutoShape drawShape = laneDrawShape(a);
  const bool eraseTool = tool == Tool::Erase;
  const bool selectTool = tool == Tool::Select;
  const bool passive = tool == Tool::Zoom || tool == Tool::Play;

  // ---- hit test: the point under the pointer (6 px) ----
  int hoverPoint = -1;
  if (canvasHovered && inLane && !L.preview) {
    float best = 6.5f * 6.5f;
    for (size_t i = 0; i < pts.size(); ++i) {
      const float dx = m.x - xOf(static_cast<double>(pts[i].pos)), dy = m.y - yOf(automationToParam(rng, pts[i].value));
      if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, hoverPoint = static_cast<int>(i);
    }
  }
  if (canvasHovered && inLane) L.pointerTick = std::max(0.0, tickAt(m.x));

  // ---- draw: the curve, stems and points ----
  dl->PushClipRect(ImVec2(mn.x, mn.y - 4), mx, true);
  // Event-editor look: graph-role (cyan) fill under the curve, lit stems with
  // round heads at the points (same role as the channel rack's graph editor).
  const ImU32 col = theme::litRole(theme::role::kGraph);
  const ImU32 lineCol = P.dark ? theme::mix(col, P.paper, 0.45f) : col;
  if (!pts.empty()) {
    float prevY = 0;
    for (float x = std::floor(mn.x); x <= mx.x; x += 1.0f) {
      const double t = tickAt(x);
      const float y = yOf(ae::valueAt(shown, t, rng.flCurves));
      dl->AddLine(ImVec2(x, y), ImVec2(x, mx.y), theme::withAlpha(col, L.preview ? 0.08f : 0.16f));
      if (x > std::floor(mn.x)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), theme::withAlpha(lineCol, L.preview ? 0.4f : 0.9f), 1.3f);
      prevY = y;
    }
    for (size_t i = 0; i < shown.size(); ++i) {
      const auto& q = shown[i];
      const float x = xOf(static_cast<double>(q.pos));
      if (x < mn.x - 6 || x > mx.x + 6) continue;
      const bool sel = L.sel.count(static_cast<int>(i)) > 0, hot = static_cast<int>(i) == hoverPoint;
      const ImU32 c = sel ? P.hot : lineCol;
      dl->AddRectFilled(ImVec2(x, yOf(q.value)), ImVec2(x + 2, mx.y), theme::withAlpha(sel ? P.hot : col, 0.7f));
      dl->AddCircleFilled(ImVec2(x + 1, yOf(q.value)), hot ? 4.5f : sel ? 4.0f : 3.0f, c, 12);
      if (sel || hot) dl->AddCircle(ImVec2(x + 1, yOf(q.value)), hot ? 4.5f : 4.0f, theme::withAlpha(P.paper, 0.8f), 12, 1.2f);
    }
  } else {
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(mn.x + 8, mn.y + 4), P.textFaint, i18n::tr("Drag to draw values \xC2\xB7 right-drag for a straight line"));
  }
  if (L.preview) {  // simplify preview
    float prevY = 0;
    for (float x = std::floor(mn.x); x <= mx.x; x += 1.0f) {
      const float y = yOf(automationToParam(rng, ae::valueAt(*L.preview, tickAt(x), rng.flCurves)));
      if (x > std::floor(mn.x)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), P.hot, 1.6f);
      prevY = y;
    }
    for (const auto& q : *L.preview) {
      const float x = xOf(static_cast<double>(q.pos));
      if (x >= mn.x - 4 && x <= mx.x + 4) dl->AddCircleFilled(ImVec2(x, yOf(automationToParam(rng, q.value))), 3.0f, P.hot, 10);
    }
  }
  if (L.g == Lane::Box && canvasActive) {
    const ImU32 sc = theme::litRole(theme::role::kSelect);
    dl->AddRectFilled(ImVec2(std::min(L.start.x, m.x), std::min(L.start.y, m.y)), ImVec2(std::max(L.start.x, m.x), std::max(L.start.y, m.y)), theme::withAlpha(sc, 0.12f));
    dl->AddRect(ImVec2(std::min(L.start.x, m.x), std::min(L.start.y, m.y)), ImVec2(std::max(L.start.x, m.x), std::max(L.start.y, m.y)), sc, 0, 0, 1.5f);
  }
  dl->PopClipRect();

  // ---- hint ----
  if (canvasHovered && inLane) {
    const double t = tickAt(m.x);
    std::string how;
    if (passive) how = i18n::tr("(this tool does not edit the lane)");
    else if (eraseTool) how = i18n::tr("(drag over points to delete them)");
    else if (selectTool) how = i18n::tr("(click or drag a box to select \xC2\xB7 drag selected points to move \xC2\xB7 right-click a point: its menu)");
    else
      how = std::string("(") + i18n::tr(drawShapeName(drawShape)) + ": " +
            i18n::tr("drag draws \xC2\xB7 right-drag: straight line \xC2\xB7 Ctrl+drag: select \xC2\xB7 drag a selected point to move \xC2\xB7 right-click a point: its menu)");
    const MeterMap pm = MeterMap::forPattern(s, p);  // K-193: pattern time, the pattern's own meter
    a.setHint(paramDisplayName(s, pid) + ": " + fmtValue(s, pid, dispAt(m.y)) + " at " + timeline::rulerLabel(pm, std::max<Tick>(0, static_cast<Tick>(t)), grid) +
              "   " + how);
    if (hoverPoint >= 0 && !eraseTool && !passive) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  // ---- press ----
  const bool lClick = canvasHovered && inLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool rClick = canvasHovered && inLane && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  if ((lClick || rClick) && !L.preview) {
    L.focus = true;
    L.g = Lane::None;
    L.start = m;
    L.tickA = tickAt(m.x);
    L.yA = m.y;
    L.lastTick = -1;
    L.lastX = m.x;
    L.paintLo = 0, L.paintHi = -1;
    L.base = pts;
    L.anchor = -1;
    L.key = 0x504C410000000000ull ^ (counter++ << 8);
    const bool selDrag = keys::modifierHeld("piano.laneSelectDrag");
    if (passive) {
      // nothing: the zoom and playback tools leave the lane alone
    } else if (eraseTool) {
      L.g = Lane::Erase;
    } else if (rClick) {
      L.g = Lane::RightPending;
      L.menuPoint = hoverPoint;
    } else if (hoverPoint >= 0 && io.KeyShift) {
      if (!L.sel.erase(hoverPoint)) L.sel.insert(hoverPoint);
    } else if (hoverPoint >= 0 && (selectTool || selDrag || L.sel.count(hoverPoint))) {
      if (!L.sel.count(hoverPoint)) L.sel = {hoverPoint};
      L.g = Lane::Move;
      L.anchor = hoverPoint;
    } else if (selectTool || selDrag) {
      L.g = Lane::Box;
      if (!io.KeyShift) L.sel.clear();
    } else {
      L.g = Lane::Paint;
      L.sel.clear();
    }
  }

  // ---- drag ----
  if (canvasActive && L.g != Lane::None) {
    switch (L.g) {
      case Lane::RightPending:
        if (std::fabs(m.x - L.start.x) + std::fabs(m.y - L.start.y) > 3.0f) L.g = Lane::Line, L.sel.clear();
        break;
      case Lane::Paint:
      case Lane::Line: {
        // Paint: the segment since the last frame (nothing skipped on fast drags).
        // Line: from the press point to the pointer, on the lane as it was at the press.
        const bool line = L.g == Lane::Line;
        double t0 = line ? L.tickA : L.lastTick < 0 ? tickAt(m.x) : L.lastTick, t1 = tickAt(m.x);
        float yA = line ? L.yA : L.lastTick < 0 ? m.y : L.lastY, yB = m.y;
        if (t1 < t0) std::swap(t0, t1), std::swap(yA, yB);
        ae::Points next = line ? L.base : pts;
        const Tick c0 = std::max<Tick>(0, snapDown(static_cast<Tick>(std::floor(t0)), grid)),
                   c1 = std::max<Tick>(0, snapDown(static_cast<Tick>(std::floor(t1)), grid));
        const auto valueAtF = [&](double f) { return static_cast<float>(paramToAutomation(rng, dispAt(yA + (yB - yA) * static_cast<float>(f)))); };
        if (line && drawShape != AutoShape::Hold) {
          // A straight (or eased) line: its two ends, the points between replaced.
          next.erase(std::remove_if(next.begin(), next.end(), [&](const AutoPoint& q) { return q.pos >= c0 && q.pos < c1 + grid; }), next.end());
          AutoPoint e0, e1;
          e0.pos = c0, e0.value = valueAtF(0.0), e0.shape = drawShape;
          e1.pos = c1, e1.value = valueAtF(1.0), e1.shape = drawShape;
          ae::addPoint(next, e0);
          if (c1 > c0) ae::addPoint(next, e1);
        } else {
          for (Tick c = c0; c <= c1; c += grid) {
            next.erase(std::remove_if(next.begin(), next.end(), [&](const AutoPoint& q) { return q.pos >= c && q.pos < c + grid; }), next.end());
            const double f = t1 > t0 ? std::clamp((static_cast<double>(c) - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
            AutoPoint np;
            np.pos = c;
            np.value = valueAtF(f);
            np.shape = drawShape;
            ae::addPoint(next, np);
          }
          if (!line) {
            L.paintLo = L.paintHi < L.paintLo ? c0 : std::min(L.paintLo, c0);
            L.paintHi = std::max(L.paintHi, c1);
          }
        }
        if (next != pts)
          a.doc.editPattern(pattern, line ? "draw line" : "draw events", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, next); }, L.key);
        L.lastTick = tickAt(m.x);
        L.lastY = m.y;
        break;
      }
      case Lane::Move: {
        if (L.anchor < 0 || static_cast<size_t>(L.anchor) >= L.base.size()) break;
        const AutoPoint& an = L.base[static_cast<size_t>(L.anchor)];
        Tick target = snapT(static_cast<double>(an.pos) + (tickAt(m.x) - L.tickA));
        double dDisp = static_cast<double>((L.start.y - m.y) / h);
        if (io.KeyShift) {  // one axis
          if (std::fabs(m.x - L.start.x) > std::fabs(m.y - L.start.y)) dDisp = 0;
          else target = an.pos;
        }
        const double dv = std::fabs(span) > 1e-9 ? dDisp / span : 0.0;
        ae::Points out;
        ae::movePoints(L.base, L.sel, target - an.pos, dv, out);
        if (out != pts) a.doc.editPattern(pattern, "move automation points", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, out); }, L.key);
        break;
      }
      case Lane::Box: {
        const Tick t0 = static_cast<Tick>(std::floor(tickAt(std::min(L.start.x, m.x)))), t1 = static_cast<Tick>(std::ceil(tickAt(std::max(L.start.x, m.x))));
        const double v0 = paramToAutomation(rng, dispAt(std::max(L.start.y, m.y))), v1 = paramToAutomation(rng, dispAt(std::min(L.start.y, m.y)));
        ae::Selection box = ae::selectBox(pts, t0, t1, std::min(v0, v1) - 1e-6, std::max(v0, v1) + 1e-6);
        if (io.KeyShift)
          for (int i : box) L.sel.insert(i);
        else
          L.sel = box;
        break;
      }
      case Lane::Erase: {
        const float xa = std::min(L.lastX, m.x) - 4, xb = std::max(L.lastX, m.x) + 4;
        ae::Selection hit;
        for (size_t i = 0; i < pts.size(); ++i) {
          const float x = xOf(static_cast<double>(pts[i].pos));
          if (x >= xa && x <= xb) hit.insert(static_cast<int>(i));
        }
        if (!hit.empty() && pts.size() > 1)
          editLane(a, pattern, pid, "delete automation points", [&](ae::Points& q) { ae::deletePoints(q, hit, 1); }, L.key);
        L.sel.clear();
        L.lastX = m.x;
        break;
      }
      default: break;
    }
  }
  if (L.g == Lane::Line && canvasActive) {  // line tool preview
    const ImVec2 pa(xOf(L.tickA), std::clamp(L.yA, mn.y, mx.y)), pb(m.x, std::clamp(m.y, mn.y, mx.y));
    dl->PushClipRect(ImVec2(mn.x, mn.y - 4), ImVec2(mx.x, mx.y + 4), true);
    dl->AddLine(pa, pb, theme::withAlpha(P.ink, 0.6f), 3.0f);
    dl->AddLine(pa, pb, col, 1.5f);
    dl->AddCircleFilled(pa, 3.0f, col, 10);
    dl->AddCircleFilled(pb, 3.0f, col, 10);
    dl->PopClipRect();
  }

  // ---- release ----
  if (!canvasActive && L.g != Lane::None) {
    if (L.g == Lane::RightPending && L.menuPoint >= 0) {  // a right click on a point: its menu
      if (!L.sel.count(L.menuPoint)) L.sel = {L.menuPoint};
      L.openPointMenu = true;
    } else if (L.g == Lane::Paint && drawShape != AutoShape::Hold && L.paintHi >= L.paintLo) {
      // A linear or smooth stroke keeps a few points of its curve (same undo step).
      const Tick lo = L.paintLo, hi = L.paintHi;
      const AutoShape sh = drawShape;
      editLane(a, pattern, pid, "draw events", [&](ae::Points& q) {
        ae::Selection span;
        for (size_t i = 0; i < q.size(); ++i)
          if (q[i].pos >= lo && q[i].pos <= hi) span.insert(static_cast<int>(i));
        if (span.size() > 2) ae::thinRange(q, span, 0.005, sh);
      }, L.key);
    }
    L.g = Lane::None;
    a.doc.endGesture();
  }
  pointMenu(a, pattern, pts, rng);
  simplifyPopup(a, pattern, pts);
}

void simplifyAutomationLane(App& a, ObjectId pattern, double tolerance, AutoShape result) {
  const ae::Selection sel = L.pattern == pattern && L.target == laneTarget ? L.sel : ae::Selection{};
  ae::Selection kept;
  editLane(a, pattern, laneTarget, "simplify points", [&](ae::Points& p) { kept = ae::thinRange(p, sel, tolerance, result); });
  L.sel = sel.size() >= 2 ? kept : ae::Selection{};
}

std::string automationLaneDump(App& a, ObjectId pattern) {
  const ae::Points pts = lanePoints(a.doc.song(), pattern, laneTarget);
  char b[256];
  std::snprintf(b, sizeof b, "target %u %u points %zu sel %zu focus %d shape %d lane %.1f %.1f %.1f %.1f chip %.1f %.1f %.1f %.1f undo %zu",
                static_cast<unsigned>(laneTarget.owner), static_cast<unsigned>(laneTarget.index), pts.size(), L.sel.size(), L.focus ? 1 : 0,
                static_cast<int>(laneDrawShape(a)), static_cast<double>(L.mn.x), static_cast<double>(L.mn.y), static_cast<double>(L.mx.x),
                static_cast<double>(L.mx.y), static_cast<double>(L.chip0.x), static_cast<double>(L.chip0.y), static_cast<double>(L.chip1.x),
                static_cast<double>(L.chip1.y), a.doc.history().position());
  std::string out = b;
  out += " pts";
  for (size_t i = 0; i < pts.size() && i < 80; ++i) {
    std::snprintf(b, sizeof b, " %lld:%.4f:%d%s", static_cast<long long>(pts[i].pos), static_cast<double>(pts[i].value), static_cast<int>(pts[i].shape),
                  L.sel.count(static_cast<int>(i)) ? "*" : "");
    out += b;
  }
  return out;
}

}  // namespace gl
