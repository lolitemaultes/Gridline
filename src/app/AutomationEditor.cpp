// Gridline — automation editing UI (see AutomationEditor.h).
//
// Layout follows FL Studio's automation workflow: a piano-roll-style envelope
// view with a time ruler, a value axis, points with per-segment tension
// handles and a right-click point menu (delete · curve mode · value
// clipboard · type a value), an options menu of selection and level tools, a
// separate LFO dialog (start / end value, range and speed, shape, gate), and
// the clip's target. Drawn entirely with Gridline's palette.
#include "../core/I18n.h"
#include "DialogLayout.h"
#include "Keys.h"
#include "AutomationEditor.h"
#include "SmoothScrollUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include "../core/Automation.h"
#include "../core/AutomationEdit.h"
#include "../core/PatternAutomation.h"
#include "../core/Presets.h"
#include "../core/Timeline.h"
#include "App.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

using theme::pal;
namespace ae = autoedit;

namespace {

// --------------------------------------------------------------- helpers ----

std::string fmtParam(const Song& s, ParamId pid, double normalized) {
  auto d = describeParam(s, pid);
  if (!d) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", normalized * 100.0);
    return b;
  }
  return d->format(d->clamp(d->fromNormalized(std::clamp(normalized, 0.0, 1.0))));
}

// Clip value (0..1 inside min..max) ↔ the parameter's normalized value.
double clipToParam(const AutomationClip& c, double v) { return c.min + v * (c.max - c.min); }
double paramToClip(const AutomationClip& c, double n) {
  double r = c.max - c.min;
  return std::fabs(r) < 1e-9 ? 0.0 : std::clamp((n - c.min) / r, 0.0, 1.0);
}

// Text as an ImGui format string (a literal '%' doubled).
std::string fmtEscape(const std::string& t) {
  std::string o;
  for (char c : t) o += c == '%' ? std::string("%%") : std::string(1, c);
  return o;
}

std::string shortTarget(const Song& s, ParamId pid) {
  if (pid.owner == kNoId) return "No target";
  return paramDisplayName(s, pid);
}

// --------------------------------------------------------------- editor ----

enum class Tool { Draw, Select, Erase };

struct Editor {
  bool open = false;
  ObjectId autoId = kNoId, placement = kNoId;
  // J-173: a pattern automation clip (pattern != kNoId): the clip of `target` in that pattern.
  ObjectId pattern = kNoId;
  ParamId target;
  ae::Selection sel;
  float pxPerTick = 0.3f, scrollX = 0;
  smoothscroll::Axis axX;  // J-67: smooth scrolling / elastic left edge
  bool fit = true;
  Snap snap = Snap::Line;
  Tool tool = Tool::Draw;
  ae::Points clipboard;
  // Gesture.
  enum Gesture { None, Move, Tension, Box, Erase } g = None;
  ImVec2 start;
  ae::Points base;
  Tick startTick = 0;
  int anchor = -1;
  float baseTension = 0;
  uint64_t key = 0;
  Tick moveDelta = 0;  // J-67: snapped time shift of the points being moved (their drawing glides to it)
  // Menus and tools.
  int menuPoint = -1;
  double valueClip = 0;
  bool hasValueClip = false;
  bool lfoOpen = false;
  ae::LfoSettings lfo;
  bool lfoSync = true;
  double lfoDivStart = 4, lfoDivEnd = 4;  // index into kDivs
  double lfoHzStart = 1, lfoHzEnd = 1;
  bool scaleOpen = false;
  double scCentre = 0.5, scMul = 1.0, scOff = 0.0, scTension = 0.0;
  bool thinOpen = false;
  double thinTol = 0.02;
  std::optional<ae::Points> preview;
  char shapeName[96] = "";
  char targetFilter[64] = "";
  char shapeFilter[64] = "";
  bool typeValue = false;
  std::vector<PresetEntry> shapes;
  double shapesLoaded = -1;
  ImVec2 winMin, winMax;      // editor window rect (tool dialogs open over its lower right)
  bool toolWasOpen = false;   // a tool dialog was open last frame (focus it when it opens)
};
Editor E;
uint64_t gestureCounter = 1;

// Tempo-synced LFO speeds: cycle length in bars.
struct Div {
  const char* name;
  double bars;
};
const Div kDivs[] = {{"8 bars", 8},    {"4 bars", 4},     {"2 bars", 2},    {"1 bar", 1},    {"1/2", 0.5},   {"1/4", 0.25},
                     {"1/8", 0.125},   {"1/16", 0.0625},  {"1/32", 0.03125}, {"1/4 T", 1.0 / 6}, {"1/8 T", 1.0 / 12}, {"1/16 T", 1.0 / 24}};
constexpr int kDivCount = static_cast<int>(sizeof kDivs / sizeof kDivs[0]);

const Clip* placementOf(const Song& s) {
  if (E.pattern != kNoId) return nullptr;  // J-173: a pattern clip plays in pattern time
  const Clip* c = E.placement != kNoId ? s.clip(E.placement) : nullptr;
  return c && c->kind == ClipKind::Automation && c->source == E.autoId ? c : nullptr;
}

// J-173: the clip being edited (a playlist automation clip or a pattern's) and its edits.
const AutomationClip* curClip(const Song& s) {
  if (E.pattern != kNoId) {
    const Pattern* p = s.pattern(E.pattern);
    return p ? findPatternLane(*p, E.target.owner, E.target.index) : nullptr;
  }
  return s.automation(E.autoId);
}
bool editCur(App& a, const std::string& label, const std::function<void(AutomationClip&)>& fn, uint64_t key = 0) {
  if (E.pattern != kNoId) {
    const ParamId t = E.target;
    return a.doc.editPattern(E.pattern, label, [&](Pattern& p) {
      if (AutomationClip* c = findPatternLane(p, t.owner, t.index)) fn(*c);
    }, key);
  }
  return a.doc.editAutomation(E.autoId, label, fn, key);
}
// Content length the editor shows when there is no placement: the pattern's
// length (pattern clips), else four bars; at least up to the last point.
Tick naturalLength(const Song& s, const AutomationClip& ac) {
  Tick len = 4 * s.barTicks();
  if (E.pattern != kNoId)
    if (const Pattern* p = s.pattern(E.pattern)) len = s.patternContentLength(*p);
  return std::max(len, patternClipEnd(ac));
}
// The playhead in content ticks (-1: not playing this clip's time).
double contentPlayhead(App& a) {
  const Song& s = a.doc.song();
  if (E.pattern != kNoId)
    return a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == E.pattern ? a.engine.position() : -1.0;
  const Clip* pl = placementOf(s);
  if (pl && a.engine.mode() == PlayMode::Song) return a.engine.position() - static_cast<double>(pl->start - pl->offset);
  return -1.0;
}

// Content range the tools act on: the selection's span (2+ points), else the
// visible part of the placement, else the content up to the last point.
std::pair<Tick, Tick> toolRange(const Song& s, const AutomationClip& ac) {
  if (E.sel.size() >= 2) {
    int i0 = *E.sel.begin(), i1 = *E.sel.rbegin();
    if (i1 < static_cast<int>(ac.points.size())) return {ac.points[static_cast<size_t>(i0)].pos, ac.points[static_cast<size_t>(i1)].pos};
  }
  if (const Clip* pl = placementOf(s)) return {pl->offset, pl->offset + s.clipLength(*pl)};
  return {0, naturalLength(s, ac)};
}

void editPoints(App& a, const std::string& label, const std::function<void(ae::Points&)>& fn, uint64_t key = 0) {
  editCur(a, label, [&](AutomationClip& c) { fn(c.points); }, key);
}

void refreshShapes(App& a) {
  if (E.shapesLoaded >= 0 && a.now - E.shapesLoaded < 3.0) return;
  E.shapes = listAutomationShapes(presetRoot(a.configDir()));
  E.shapesLoaded = a.now;
}

void applyShapeFile(App& a, const PresetEntry& e) {
  const Song& s = a.doc.song();
  const AutomationClip* ac = curClip(s);
  AutomationShape sh;
  if (!ac || !loadAutomationShape(e.file, s.ppq, sh)) {
    a.notify("Could not read that shape", true);
    return;
  }
  const auto range = toolRange(s, *ac);
  const Tick from = range.first;
  Tick to = range.second;
  if (E.sel.size() < 2 && !placementOf(s)) to = from + std::max<Tick>(1, sh.points.back().pos);  // natural length
  editPoints(a, "apply automation shape", [&](ae::Points& p) { ae::applyShape(p, sh.points, from, to); });
  E.sel.clear();
}

// ---------------------------------------------------------------- menus ----

void curveModeItems(App& a, const AutomationClip& ac, const ae::Selection& target) {
  AutoShape cur = AutoShape::Count;
  if (target.size() == 1 && *target.begin() < static_cast<int>(ac.points.size())) cur = ac.points[static_cast<size_t>(*target.begin())].shape;
  for (AutoShape sh : ae::menuOrder())
    if (ImGui::MenuItem(autoShapeName(sh), nullptr, sh == cur)) {
      ae::Selection t = target;
      editPoints(a, "curve mode", [&](ae::Points& p) { ae::setShape(p, t, sh); });
    }
}

void copySelection(App& a, const AutomationClip& ac) {
  E.clipboard = ae::copyPoints(ac.points, E.sel);
  if (!E.clipboard.empty()) a.notify(std::to_string(E.clipboard.size()) + " points copied");
}

void pasteAt(App& a, Tick at, bool replace) {
  if (E.clipboard.empty()) return;
  ae::Selection added;
  editPoints(a, replace ? "paste points (replace)" : "paste points", [&](ae::Points& p) { added = ae::pastePoints(p, E.clipboard, at, replace); });
  E.sel = added;
}

Tick pasteTick(App& a, const AutomationClip& ac) {
  const Song& s = a.doc.song();
  if (!E.sel.empty() && *E.sel.rbegin() < static_cast<int>(ac.points.size())) return ac.points[static_cast<size_t>(*E.sel.rbegin())].pos;
  if (const double ph = contentPlayhead(a); ph >= 0) return std::max<Tick>(0, static_cast<Tick>(ph));
  (void)s;
  return ac.points.empty() ? 0 : ac.points.back().pos;
}

void deleteSelection(App& a) {
  if (E.sel.empty()) return;
  ae::Selection sel = E.sel;
  editPoints(a, "delete points", [&](ae::Points& p) { ae::deletePoints(p, sel); });
  E.sel.clear();
}

// Options menu: selection, clipboard, level tools, LFO, shapes.
void editorMenu(App& a) {
  const Song& s = a.doc.song();
  const AutomationClip* ac = curClip(s);
  if (!ac) return;
  const int n = static_cast<int>(ac->points.size());
  ImGui::TextDisabled("%s", i18n::tr("Points"));
  if (ImGui::MenuItem(i18n::tr("Select all"), keys::label("auto.selectAll")))
    for (int i = 0; i < n; ++i) E.sel.insert(i);
  if (ImGui::MenuItem(i18n::tr("Deselect all"), keys::label("auto.deselect"))) E.sel.clear();
  if (ImGui::MenuItem(i18n::tr("Invert selection"), keys::label("auto.invertSelection"))) {
    ae::Selection inv;
    for (int i = 0; i < n; ++i)
      if (!E.sel.count(i)) inv.insert(i);
    E.sel = inv;
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Cut"), keys::label("auto.cut"), false, !E.sel.empty())) {
    copySelection(a, *ac);
    deleteSelection(a);
  }
  if (ImGui::MenuItem(i18n::tr("Copy"), keys::label("auto.copy"), false, !E.sel.empty())) copySelection(a, *ac);
  if (ImGui::MenuItem(i18n::tr("Paste (insert)"), keys::label("auto.paste"), false, !E.clipboard.empty())) pasteAt(a, pasteTick(a, *ac), false);
  if (ImGui::MenuItem(i18n::tr("Paste (replace)"), keys::label("auto.pasteReplace"), false, !E.clipboard.empty())) pasteAt(a, pasteTick(a, *ac), true);
  if (ImGui::MenuItem(i18n::tr("Duplicate"), "Ctrl+B", false, E.sel.size() >= 1)) {
    ae::Points c = ae::copyPoints(ac->points, E.sel);
    Tick at = ac->points[static_cast<size_t>(*E.sel.rbegin())].pos + std::max<Tick>(ae::clipSpan(c), s.beatTicks() / 4);
    std::swap(c, E.clipboard);
    pasteAt(a, at, false);
    std::swap(c, E.clipboard);
  }
  if (ImGui::MenuItem(i18n::tr("Delete"), keys::label("auto.delete"), false, !E.sel.empty())) deleteSelection(a);
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Tools"));
  if (ImGui::MenuItem(i18n::tr("Flip vertically"))) {
    ae::Selection t = E.sel;
    editPoints(a, "flip vertically", [&](ae::Points& p) { ae::flipVertical(p, t); });
  }
  if (ImGui::MenuItem(i18n::tr("Flip horizontally"))) {
    ae::Selection t = E.sel;
    editPoints(a, "flip horizontally", [&](ae::Points& p) { ae::flipHorizontal(p, t); });
  }
  if (ImGui::MenuItem(i18n::tr("Scale levels..."))) E.scaleOpen = true, E.lfoOpen = false, E.thinOpen = false;
  if (ImGui::MenuItem(i18n::tr("Quantize to the grid"), "Q")) {
    Tick grid = timeline::snapGrid(s, E.snap == Snap::None ? Snap::Line : E.snap, E.pxPerTick, false);
    ae::Selection t = E.sel, out;
    editPoints(a, "quantize points", [&](ae::Points& p) { out = ae::quantize(p, t, grid); });
    E.sel = out;
  }
  if (ImGui::MenuItem(i18n::tr("Thin out points..."))) E.thinOpen = true, E.lfoOpen = false, E.scaleOpen = false;
  if (ImGui::BeginMenu(E.sel.empty() ? i18n::tr("Curve of every point") : i18n::tr("Curve of the selection"))) {
    curveModeItems(a, *ac, E.sel);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Reset tensions"))) {
    ae::Selection t = E.sel;
    editPoints(a, "reset tensions", [&](ae::Points& p) { ae::setTension(p, t, 0.0f); });
  }
  if (ImGui::MenuItem(i18n::tr("Insert point at the playhead"))) {
    Tick at = pasteTick(a, *ac);
    if (const double ph = contentPlayhead(a); ph >= 0) at = std::max<Tick>(0, static_cast<Tick>(ph));
    AutoPoint np;
    np.pos = at;
    np.value = static_cast<float>(ae::valueAt(ac->points, static_cast<double>(at), ac->flCurves));
    int idx = 0;
    editPoints(a, "insert point", [&](ae::Points& p) { idx = ae::addPoint(p, np); });
    E.sel = {idx};
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("LFO..."), "L")) E.lfoOpen = true, E.scaleOpen = false, E.thinOpen = false;
}

void pointMenu(App& a, const AutomationClip& ac) {
  const Song& s = a.doc.song();
  int k = E.menuPoint;
  if (k < 0 || k >= static_cast<int>(ac.points.size())) return;
  const AutoPoint& p = ac.points[static_cast<size_t>(k)];
  ParamId pid{ac.owner, ac.index};
  ImGui::TextDisabled("%s", fmtParam(s, pid, clipToParam(ac, p.value)).c_str());
  ae::Selection target = E.sel.count(k) ? E.sel : ae::Selection{k};
  if (ImGui::MenuItem(i18n::tr("Delete"), nullptr, false, ac.points.size() > 1)) {
    editPoints(a, "delete point", [&](ae::Points& q) { ae::deletePoints(q, target); });
    E.sel.clear();
  }
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Curve"));
  curveModeItems(a, ac, target);
  if (ImGui::MenuItem(i18n::tr("Reset tension"), nullptr, false, p.tension != 0.0f))
    editPoints(a, "reset tension", [&](ae::Points& q) { ae::setTension(q, target, 0.0f); });
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Copy value"))) E.valueClip = p.value, E.hasValueClip = true;
  if (ImGui::MenuItem(i18n::tr("Paste value"), nullptr, false, E.hasValueClip)) {
    float v = static_cast<float>(E.valueClip);
    editPoints(a, "paste value", [&](ae::Points& q) {
      for (int i : target)
        if (i < static_cast<int>(q.size())) q[static_cast<size_t>(i)].value = v;
    });
  }
  if (ImGui::MenuItem(i18n::tr("Type a value..."))) E.typeValue = true;
}

void typeValuePopup(App& a, const AutomationClip& ac) {
  static char buf[64];
  const Song& s = a.doc.song();
  ParamId pid{ac.owner, ac.index};
  int k = E.menuPoint;
  if (ImGui::BeginPopup("##ptvalue")) {
    if (k < 0 || k >= static_cast<int>(ac.points.size())) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof buf, "%s", fmtParam(s, pid, clipToParam(ac, ac.points[static_cast<size_t>(k)].value)).c_str());
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::TextDisabled("%s", i18n::tr("Point value"));
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
      auto d = describeParam(s, pid);
      double v = 0;
      bool ok = false;
      if (d && pui::parseValue(*d, buf, v)) {
        v = paramToClip(ac, d->toNormalized(d->clamp(v)));
        ok = true;
      } else if (std::sscanf(buf, "%lf", &v) == 1) {
        v = std::clamp(v / 100.0, 0.0, 1.0);
        ok = true;
      }
      if (ok) editPoints(a, "type point value", [&](ae::Points& q) { q[static_cast<size_t>(k)].value = static_cast<float>(v); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("%s", i18n::tr("Enter to apply \xC2\xB7 Esc to cancel"));
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------ side panel ----

void targetPicker(App& a, const AutomationClip& ac) {
  const Song& s = a.doc.song();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##tf", i18n::tr("Filter parameters"), E.targetFilter, sizeof E.targetFilter);
  std::string f = E.targetFilter;
  for (auto& ch : f) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  ImGui::BeginChild("##tlist", ImVec2(320, 260));
  int shown = 0;
  for (ParamId pid : listParams(s)) {
    std::string name = paramDisplayName(s, pid), low = name;
    for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!f.empty() && low.find(f) == std::string::npos) continue;
    if (++shown > 400) break;
    ImGui::PushID(static_cast<int>(pid.packed() & 0x7FFFFFFF) ^ static_cast<int>(pid.packed() >> 31));
    if (ImGui::Selectable(name.c_str(), pid.owner == ac.owner && pid.index == ac.index)) {
      const std::string oldName = shortTarget(s, {ac.owner, ac.index});
      const Pattern* inPat = E.pattern != kNoId ? s.pattern(E.pattern) : nullptr;
      if (inPat && !(pid == ParamId{ac.owner, ac.index}) && findPatternLane(*inPat, pid.owner, pid.index)) {
        a.notify("This pattern has an automation clip for that control already", true);  // J-173: one per control
      } else {
        editCur(a, "change automation target", [&](AutomationClip& c) {
          if (c.name == "Automation" || c.name == oldName) c.name = name;
          c.owner = pid.owner;
          c.index = pid.index;
        });
        if (inPat) E.target = pid;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void sidePanel(App& a, const AutomationClip& ac, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ParamId pid{ac.owner, ac.index};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = width - 2 * theme::kS3;
  auto section = [&](const char* t) {
    ImGui::Dummy(ImVec2(0, 4));
    ui::Label(t);
    ImGui::Dummy(ImVec2(0, 1));
  };
  section("TARGET");
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 40;
    ImGui::InvisibleButton("##target", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##targetpop");
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? P.surface700 : P.surface800, theme::kRadius);
    dl->AddRectFilled(ImVec2(p.x + 6, p.y + 8), ImVec2(p.x + 10, p.y + h - 8), theme::rgb(ac.color), 2);
    theme::textEllipsis(dl, a.fontBold, theme::kFontCaption, ImVec2(p.x + 16, p.y + 5), w - 34, P.text, shortTarget(s, pid).c_str());
    std::string now = pid.owner == kNoId ? "Click to choose a parameter" : "Now " + fmtParam(s, pid, describeParam(s, pid) ? describeParam(s, pid)->toNormalized(*getParam(s, pid)) : 0);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(p.x + 16, p.y + 22), w - 34, P.textDim, now.c_str());
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(p.x + w - 12, p.y + h * 0.5f), 10, P.textDim);
    ui::Hint("The parameter this clip drives: click to change (retarget) it");
    if (ImGui::BeginPopup("##targetpop")) {
      targetPicker(a, ac);
      ImGui::EndPopup();
    }
  }
  const bool canInit = pid.owner != kNoId && getParam(s, pid).has_value();
  if (ImGui::Button(i18n::tr("Start from control"), ImVec2((w - 4) * 0.5f, 0)) && canInit && !ac.points.empty()) {
    double n = describeParam(s, pid)->toNormalized(*getParam(s, pid));
    float v = static_cast<float>(paramToClip(ac, n));
    editPoints(a, "start value from control", [&](ae::Points& p) { p.front().value = v; });
  }
  ui::Hint("Sets the clip's first point to the control's current value (the value the song starts with)");
  ImGui::SameLine(0, 4);
  if (ImGui::Button(i18n::tr("Control to start"), ImVec2((w - 4) * 0.5f, 0)) && canInit && !ac.points.empty()) {
    auto d = describeParam(s, pid);
    a.doc.setParam(pid, d->fromNormalized(clipToParam(ac, ac.points.front().value)), false);
  }
  ui::Hint("Sets the control to the clip's start value");

  section("RANGE");
  {
    float lo = ac.min * 100.0f, hi = ac.max * 100.0f;
    ImGui::SetNextItemWidth(w);
    std::string loFmt = fmtEscape("Low  " + fmtParam(s, pid, ac.min));
    if (ImGui::SliderFloat("##lo", &lo, 0.0f, 100.0f, loFmt.c_str()))
      editCur(a, "automation range", [&](AutomationClip& c) { c.min = std::clamp(lo / 100.0f, 0.0f, 1.0f); }, 0x41520001);
    ui::Hint("Parameter value at the bottom of the clip (Ctrl+click to type)");
    ImGui::SetNextItemWidth(w);
    std::string hiFmt = fmtEscape("High  " + fmtParam(s, pid, ac.max));
    if (ImGui::SliderFloat("##hi", &hi, 0.0f, 100.0f, hiFmt.c_str()))
      editCur(a, "automation range", [&](AutomationClip& c) { c.max = std::clamp(hi / 100.0f, 0.0f, 1.0f); }, 0x41520001);
    ui::Hint("Parameter value at the top of the clip (Ctrl+click to type)");
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }

  section("CLIP");
  {
    const Clip* pl = placementOf(s);
    const Tick beat = std::max<Tick>(1, s.beatTicks());
    if (pl) {
      int beats = static_cast<int>(std::max<Tick>(1, s.clipLength(*pl) / beat));
      ImGui::SetNextItemWidth(w);
      if (ImGui::DragInt("##len", &beats, 0.1f, 1, 4096, "Length  %d beats")) {
        ObjectId id = pl->id;
        a.doc.editClip(id, "automation clip length", [&](Clip& c) { c.length = static_cast<Tick>(beats) * beat; }, 0x41520002);
      }
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      ui::Hint("Length of this placement in the playlist (drag or Ctrl+click to type)");
    }
    bool loop = ac.loop > 0;
    if (ImGui::Checkbox(i18n::tr("Loop the content"), &loop)) {
      Tick len = ac.points.empty() ? s.barTicks() : std::max<Tick>(beat, (ac.points.back().pos + beat - 1) / beat * beat);
      editCur(a, "loop automation", [&](AutomationClip& c) { c.loop = loop ? len : 0; });
    }
    ui::Hint("Repeat the points every loop length when the clip is longer");
    if (ac.loop > 0) {
      int lb = static_cast<int>(std::max<Tick>(1, ac.loop / beat));
      ImGui::SetNextItemWidth(w);
      if (ImGui::DragInt("##loopl", &lb, 0.1f, 1, 1024, "Loop every %d beats"))
        editCur(a, "loop length", [&](AutomationClip& c) { c.loop = static_cast<Tick>(lb) * beat; }, 0x41520003);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    }
    if (E.pattern != kNoId) {
      // J-173: a clip inside a pattern: its name, and where it lives.
      static char nameBuf[96] = "";
      ImGui::SetNextItemWidth(w);
      ImGui::InputTextWithHint("##pname", patternAutoName(s, ac).c_str(), nameBuf, sizeof nameBuf);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string nm = nameBuf;
        editCur(a, "rename automation clip", [&](AutomationClip& c) { c.name = nm; });
      } else if (!ImGui::IsItemActive()) {
        std::snprintf(nameBuf, sizeof nameBuf, "%s", ac.name.c_str());  // follows the clip (undo, other edits)
      }
      ui::Hint("Name of the clip (empty: the control's name)");
      const Pattern* inPat = s.pattern(E.pattern);
      if (ImGui::Button(i18n::tr("Show the pattern"), ImVec2(w, 0)) && inPat) {
        a.selectPattern(E.pattern);
        a.showRack = true;
        a.focusWindow = "Channel rack";
      }
      ui::Hint("Selects the pattern this clip belongs to (its row is in the channel rack)");
    } else if (ImGui::Button(i18n::tr("Select in playlist"), ImVec2(w, 0))) {
      a.playlist.selected.clear();
      for (const auto& c : s.playlist)
        if (c.kind == ClipKind::Automation && c.source == ac.id) a.playlist.selected.insert(c.id);
      a.showPlaylist = true;
      a.focusWindow = "Playlist";
    }
  }

  section("SHAPES");
  refreshShapes(a);
  ImGui::SetNextItemWidth(w);
  ImGui::InputTextWithHint("##sf", i18n::tr("Filter shapes"), E.shapeFilter, sizeof E.shapeFilter);
  {
    float listH = std::max(60.0f, ImGui::GetContentRegionAvail().y - 66);
    ImGui::BeginChild("##shapes", ImVec2(w, listH), ImGuiChildFlags_Borders);
    std::string f = E.shapeFilter;
    for (auto& ch : f) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string lastFolder = "\x01";
    if (E.shapes.empty()) ImGui::TextWrapped("%s", i18n::tr("No shapes yet: save one below, or import FL Studio's (Tools > Import FL Studio presets)."));
    for (size_t i = 0; i < E.shapes.size(); ++i) {
      const auto& e = E.shapes[i];
      std::string low = e.folder + "/" + e.name;
      for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (!f.empty() && low.find(f) == std::string::npos) continue;
      if (e.folder != lastFolder) {
        lastFolder = e.folder;
        ImGui::TextColored(theme::vec(P.textFaint), "%s", e.folder.empty() ? "My shapes" : e.folder.c_str());
      }
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(("   " + e.name).c_str())) applyShapeFile(a, e);
      ui::Hint("Click: replace the selection (or the whole clip) with this shape");
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
  ImGui::SetNextItemWidth(w - 64);
  ImGui::InputTextWithHint("##sn", i18n::tr("Shape name"), E.shapeName, sizeof E.shapeName);
  ImGui::SameLine(0, 4);
  if (ImGui::Button(i18n::tr("Save"), ImVec2(60, 0)) && E.shapeName[0]) {
    AutomationShape sh;
    sh.points = E.sel.size() >= 2 ? ae::copyPoints(ac.points, E.sel) : ac.points;
    if (saveAutomationShape(presetRoot(a.configDir()), sh, s.ppq, safePresetName(E.shapeName))) {
      a.notify(std::string("Saved shape ") + E.shapeName);
      E.shapeName[0] = 0;
      E.shapesLoaded = -1;
    } else {
      a.notify("Could not save the shape", true);
    }
  }
  ui::Hint("Save the selected points (or the whole clip) as a shape");
}

// ------------------------------------------------------------- LFO tool ----

ParamDesc knobDesc(const char* name, double lo, double hi, double def, ParamScale sc = ParamScale::Linear) {
  ParamDesc d;
  d.name = name;
  d.min = lo;
  d.max = hi;
  d.def = def;
  d.scale = sc;
  return d;
}

// Fills E.lfo's periods from the speed controls.
void lfoPeriods(App& a) {
  const Song& s = a.doc.song();
  const double bar = static_cast<double>(s.barTicks());
  auto ticksFor = [&](double div, double hz) {
    if (E.lfoSync) return bar * kDivs[std::clamp(static_cast<int>(std::lround(div)), 0, kDivCount - 1)].bars;
    return s.tempo / 60.0 * s.ppq / std::max(0.01, hz);  // ticks per cycle at the song tempo
  };
  E.lfo.periodStart = ticksFor(E.lfoDivStart, E.lfoHzStart);
  E.lfo.periodEnd = ticksFor(E.lfoDivEnd, E.lfoHzEnd);
}

bool lfoKnob(const char* id, double* v, const ParamDesc& d, const char* label, const std::string& text, float cell) {
  const auto& P = pal();
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float r = 17;
  ImGui::SetCursorScreenPos(ImVec2(p.x + (cell - 2 * r) * 0.5f, p.y));
  bool ch = ui::Knob(id, v, d, r, nullptr, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y = p.y + 2 * r + 4;
  auto centred = [&](ImFont* f, float size, ImU32 col, const std::string& t) {
    float tw = f->CalcTextSizeA(size, 1e9f, 0, t.c_str()).x;
    if (tw > cell - 2) {
      theme::textEllipsis(dl, f, size, ImVec2(p.x + 1, y), cell - 2, col, t.c_str());
    } else {
      dl->AddText(f, size, ImVec2(std::round(p.x + (cell - tw) * 0.5f), y), col, t.c_str());
    }
    y += size + 3;
  };
  centred(app().fontRegular, theme::kFontLabel, P.textDim, label);
  centred(app().fontMono, theme::kFontLabel, P.text, text);
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(cell, y - p.y));
  ImGui::EndGroup();
  return ch;
}

void drawLfoTool(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const AutomationClip* ac = curClip(s);
  if (!E.lfoOpen || !ac) {
    E.lfoOpen = false;
    return;
  }
  const ParamId pid{ac->owner, ac->index};
  ImGui::SetNextWindowPos(ImVec2(E.winMax.x - 16, E.winMax.y - 16), ImGuiCond_Appearing, ImVec2(1, 1));
  if (!E.toolWasOpen) ImGui::SetNextWindowFocus();
  bool open = true;
  if (!ImGui::Begin(i18n::tr("LFO tool###autolfo"), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) E.lfoOpen = false, E.preview.reset();
    return;
  }
  const float cell = 74;
  // Shape.
  ui::Label("SHAPE");
  static const char* const shapes[] = {"Sine", "Triangle", "Square", "Saw up", "Saw down"};
  int sh = static_cast<int>(E.lfo.shape);
  if (ui::Segmented("##lshape", shapes, 5, &sh, 72)) E.lfo.shape = static_cast<ae::LfoShape>(sh);
  ImGui::Dummy(ImVec2(0, 4));
  // Speed mode.
  ui::Label("SPEED");
  static const char* const sync[] = {"Tempo", "Free (Hz)"};
  int sy = E.lfoSync ? 0 : 1;
  if (ui::Segmented("##lsync", sync, 2, &sy, 100)) E.lfoSync = sy == 0;
  ImGui::Dummy(ImVec2(0, 4));
  ParamDesc dVal = knobDesc("Value", 0, 1, 0.5), dRange = knobDesc("Range", 0, 0.5, 0.5);
  ParamDesc dDiv = knobDesc("Speed", 0, kDivCount - 1, 4);
  dDiv.kind = ParamKind::Choice;
  for (const auto& d : kDivs) dDiv.choices.push_back(d.name);
  ParamDesc dHz = knobDesc("Speed", 0.05, 20, 1, ParamScale::Log);
  auto pct = [](double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", v * 100);
    return std::string(b);
  };
  auto hz = [](double v) {
    char b[16];
    std::snprintf(b, sizeof b, v < 1 ? "%.2f Hz" : "%.1f Hz", v);
    return std::string(b);
  };
  for (int side = 0; side < 2; ++side) {
    ImGui::PushID(side);
    if (side) ImGui::SameLine(0, theme::kS4);
    ImGui::BeginGroup();
    ui::Label(side ? "END" : "START");
    double& centre = side ? E.lfo.centreEnd : E.lfo.centreStart;
    double& amount = side ? E.lfo.amountEnd : E.lfo.amountStart;
    lfoKnob("##val", &centre, dVal, "Value", fmtParam(s, pid, clipToParam(*ac, centre)), cell);
    ImGui::SameLine(0, 0);
    lfoKnob("##rng", &amount, dRange, "Range", pct(amount * 2), cell);
    ImGui::SameLine(0, 0);
    if (E.lfoSync) {
      double& div = side ? E.lfoDivEnd : E.lfoDivStart;
      lfoKnob("##spd", &div, dDiv, "Speed", kDivs[std::clamp(static_cast<int>(std::lround(div)), 0, kDivCount - 1)].name, cell);
    } else {
      double& f = side ? E.lfoHzEnd : E.lfoHzStart;
      lfoKnob("##spd", &f, dHz, "Speed", hz(f), cell);
    }
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::Dummy(ImVec2(0, 4));
  ui::Label("SHAPE CONTROLS");
  ParamDesc dPh = knobDesc("Phase", 0, 1, 0), dTen = knobDesc("Tension", -1, 1, 0), dPw = knobDesc("Gate", 0.05, 0.95, 0.5);
  char b[32];
  std::snprintf(b, sizeof b, "%.0f\xC2\xB0", E.lfo.phase * 360);
  lfoKnob("##ph", &E.lfo.phase, dPh, "Phase", b, cell);
  ImGui::SameLine(0, 0);
  std::snprintf(b, sizeof b, "%+.0f%%", E.lfo.tension * 100);
  lfoKnob("##ten", &E.lfo.tension, dTen, "Tension", b, cell);
  ImGui::SameLine(0, 0);
  lfoKnob("##pw", &E.lfo.pulseWidth, dPw, "Gate", pct(E.lfo.pulseWidth), cell);
  lfoPeriods(a);
  const auto range = toolRange(s, *ac);
  const Tick from = range.first;
  Tick to = range.second;
  {
    std::string where = (E.sel.size() >= 2 ? "Selection " : "Clip ") + timeline::rulerLabel(s, from, s.ppq / 4) + " \xE2\x80\x93 " +
                        timeline::rulerLabel(s, to, s.ppq / 4);
    ImGui::TextColored(theme::vec(P.textDim), "%s", where.c_str());
  }
  // Live preview in the editor.
  ae::Points pv = ac->points;
  ae::applyLfo(pv, E.lfo, from, to);
  E.preview = pv;
  ImGui::Dummy(ImVec2(0, 2));
  if (ImGui::Button(i18n::tr("Accept"), ImVec2(90, 0))) {
    ae::LfoSettings l = E.lfo;
    editPoints(a, "LFO tool", [&](ae::Points& p) { ae::applyLfo(p, l, from, to); });
    E.lfoOpen = false;
    E.preview.reset();
    E.sel.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Reset"), ImVec2(90, 0))) {
    E.lfo = ae::LfoSettings{};
    E.lfoDivStart = E.lfoDivEnd = 4;
    E.lfoHzStart = E.lfoHzEnd = 1;
  }
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Cancel"), ImVec2(90, 0))) E.lfoOpen = false, E.preview.reset();
  ImGui::End();
  if (!open) E.lfoOpen = false, E.preview.reset();
}

// Scale levels / thin out: small tool windows with a live preview.
void drawLevelTools(App& a) {
  const Song& s = a.doc.song();
  const AutomationClip* ac = curClip(s);
  if (!ac || (!E.scaleOpen && !E.thinOpen)) return;
  ImGui::SetNextWindowPos(ImVec2(E.winMax.x - 16, E.winMax.y - 16), ImGuiCond_Appearing, ImVec2(1, 1));
  if (!E.toolWasOpen) ImGui::SetNextWindowFocus();
  bool open = true;
  const char* title = E.scaleOpen ? "Scale levels###autoscale" : "Thin out points###autothin";
  if (!ImGui::Begin(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) E.scaleOpen = E.thinOpen = false, E.preview.reset();
    return;
  }
  ae::Points pv = ac->points;
  ae::Selection sel = E.sel;
  std::string label;
  if (E.scaleOpen) {
    float c = static_cast<float>(E.scCentre * 100), m = static_cast<float>(E.scMul * 100), o = static_cast<float>(E.scOff * 100),
          t = static_cast<float>(E.scTension * 100);
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("##c", &c, 0, 100, "Centre  %.0f%%")) E.scCentre = c / 100.0;
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("##m", &m, -200, 200, "Multiplier  %.0f%%")) E.scMul = m / 100.0;
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("##o", &o, -100, 100, "Offset  %+.0f%%")) E.scOff = o / 100.0;
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("##t", &t, -100, 100, "Tension  %+.0f%%")) E.scTension = t / 100.0;
    ae::scaleLevels(pv, sel, E.scCentre, E.scMul, E.scOff);
    if (E.scTension != 0.0)
      for (size_t i = 0; i < pv.size(); ++i)
        if (sel.empty() || sel.count(static_cast<int>(i))) pv[i].tension = std::clamp(pv[i].tension + static_cast<float>(E.scTension), -1.0f, 1.0f);
    label = "scale levels";
  } else {
    float t = static_cast<float>(E.thinTol * 100);
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("##tol", &t, 0.1f, 20.0f, "Threshold  %.1f%%")) E.thinTol = t / 100.0;
    pv = ae::thin(pv, E.thinTol);
    ImGui::TextDisabled(i18n::tr("%zu \xE2\x86\x92 %zu points"), ac->points.size(), pv.size());
    sel.clear();
    label = "thin out points";
  }
  E.preview = pv;
  if (ImGui::Button(i18n::tr("Accept"), ImVec2(76, 0))) {
    editPoints(a, label, [&](ae::Points& p) { p = pv; });
    E.scaleOpen = E.thinOpen = false;
    E.preview.reset();
    if (label != "scale levels") E.sel.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Reset"), ImVec2(76, 0))) E.scCentre = 0.5, E.scMul = 1, E.scOff = 0, E.scTension = 0, E.thinTol = 0.02;
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Cancel"), ImVec2(76, 0))) E.scaleOpen = E.thinOpen = false, E.preview.reset();
  ImGui::End();
  if (!open) E.scaleOpen = E.thinOpen = false, E.preview.reset();
}

// --------------------------------------------------------------- canvas ----

void drawCanvas(App& a, const AutomationClip& ac) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGuiIO& io = ImGui::GetIO();
  const ParamId pid{ac.owner, ac.index};
  const Clip* pl = placementOf(s);
  const Tick base = pl ? pl->start - pl->offset : 0;  // song tick of content tick 0
  const Tick clipLen = pl ? s.clipLength(*pl) : naturalLength(s, ac);
  const Tick vis0 = pl ? pl->start : 0, vis1 = pl ? pl->start + clipLen : clipLen;  // song ticks of the clip region

  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 200.0f);
  avail.y = std::max(avail.y, 140.0f);
  ImGui::InvisibleButton("##aecanvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float axisW = 64, rulerH = 26, pad = 8;
  const ImVec2 gMin(origin.x + axisW, origin.y + rulerH), gMax(origin.x + avail.x, origin.y + avail.y);
  const float gridW = gMax.x - gMin.x;
  const float y0 = gMin.y + pad, y1 = gMax.y - pad;

  if (E.fit) {
    Tick span = std::max<Tick>(vis1 - vis0, s.beatTicks());
    if (E.pattern != kNoId) {  // J-173: a pattern's clip: up to its last point (whole bars), not a long pattern's whole length
      const Tick bar = std::max<Tick>(1, s.barTicks());
      span = std::max<Tick>(s.beatTicks(), std::min(span, (patternClipEnd(ac) + bar - 1) / bar * bar));
    }
    E.pxPerTick = static_cast<float>(timeline::fitZoom(s, span + span / 4, gridW));  // some context after the clip
    E.scrollX = std::max(0.0f, static_cast<float>(vis0) * E.pxPerTick - gridW / 40);
    E.fit = false;
  }
  E.pxPerTick = static_cast<float>(timeline::clampZoom(s, E.pxPerTick));
  scrollui::clampOuter(E.axX, E.scrollX, 0.0f, 1e9f);  // J-67: the view's own glide / overscroll is left alone
  auto xOfSong = [&](double t) { return gMin.x + static_cast<float>(t) * E.pxPerTick - E.scrollX; };
  auto xOf = [&](Tick content) { return xOfSong(static_cast<double>(base + content)); };
  auto songAt = [&](float x) { return static_cast<double>((x - gMin.x + E.scrollX) / E.pxPerTick); };
  auto contentAt = [&](float x) { return static_cast<Tick>(std::floor(songAt(x))) - base; };
  auto yOf = [&](double v) { return y1 - static_cast<float>(v) * (y1 - y0); };
  auto valueAtY = [&](float y) { return std::clamp(static_cast<double>((y1 - y) / (y1 - y0)), 0.0, 1.0); };

  // Wheel: scroll / zoom.
  // J-67: through the smooth scroller (notches glide 80 px, trackpads 1:1, elastic left edge).
  float wheelX = 0;
  bool notchX = false;
  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) timeline::zoomAt(s, E.pxPerTick, E.scrollX, std::pow(1.2, static_cast<double>(io.MouseWheel)), io.MousePos.x - gMin.x);
    else wheelX -= io.MouseWheel * 80, notchX = smoothscroll::isNotch(io.MouseWheel);
  }
  if (hovered && io.MouseWheelH != 0) wheelX -= io.MouseWheelH * 80, notchX = smoothscroll::isNotch(io.MouseWheelH);
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) wheelX -= io.MouseDelta.x, notchX = false;
  scrollui::step(E.axX, E.scrollX, wheelX, notchX, 0.0f, 1e9f);
  const Tick snapG = timeline::snapGrid(s, E.snap, E.pxPerTick, io.KeyAlt);
  auto snapContent = [&](Tick c) { return timeline::snapTick(base + c, snapG, E.snap) - base; };

  // ---- background, clip region, grid ----
  dl->PushClipRect(gMin, gMax, true);
  // Studio editor well (as the piano roll / playlist): darker outside the
  // clip, alternate bars shaded, bar / beat / step lines.
  dl->AddRectFilled(gMin, gMax, theme::mix(P.well, P.ink, P.dark ? 0.22f : 0.08f));
  const float clipX0 = std::max(gMin.x, xOfSong(static_cast<double>(vis0))), clipX1 = std::min(gMax.x, xOfSong(static_cast<double>(vis1)));
  dl->AddRectFilled(ImVec2(clipX0, gMin.y), ImVec2(clipX1, gMax.y), P.well);
  const Tick visGrid = timeline::visibleGrid(s, E.pxPerTick);
  const Tick barT = std::max<Tick>(1, s.barTicks()), beatT = std::max<Tick>(1, s.beatTicks());
  for (Tick t = snapDown(std::max<Tick>(0, static_cast<Tick>(songAt(gMin.x))), barT); xOfSong(static_cast<double>(t)) < gMax.x; t += barT)
    if ((t / barT) % 2 == 1)
      dl->AddRectFilled(ImVec2(std::max(clipX0, xOfSong(static_cast<double>(t))), gMin.y), ImVec2(std::min(clipX1, xOfSong(static_cast<double>(t + barT))), gMax.y),
                        theme::withAlpha(P.paper, P.dark ? 0.03f : 0.02f));
  const ImU32 fine = theme::withAlpha(P.gridBeat, 0.45f);
  for (Tick t = snapDown(std::max<Tick>(0, static_cast<Tick>(songAt(gMin.x))), visGrid); xOfSong(static_cast<double>(t)) < gMax.x; t += visGrid) {
    float x = std::round(xOfSong(static_cast<double>(t)));
    dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), t % barT == 0 ? P.gridBar : t % beatT == 0 ? P.gridBeat : fine);
  }
  for (int k = 0; k <= 8; ++k) {
    float y = std::round(yOf(k / 8.0));
    dl->AddLine(ImVec2(gMin.x, y), ImVec2(gMax.x, y), k % 4 == 0 ? P.gridBeat : theme::withAlpha(P.gridBeat, 0.5f));
  }
  // ---- ghosts: the target's other clips (song view) ----
  if (pl && pid.owner != kNoId) {
    for (const auto& c : s.playlist) {
      if (c.kind != ClipKind::Automation || c.id == pl->id) continue;
      const AutomationClip* o = s.automation(c.source);
      if (!o || o->owner != pid.owner || o->index != pid.index || o->points.empty()) continue;
      const Tick len = s.clipLength(c);
      float gx0 = std::max(gMin.x, xOfSong(static_cast<double>(c.start))), gx1 = std::min(gMax.x, xOfSong(static_cast<double>(c.start + len)));
      if (gx1 <= gx0) continue;
      dl->AddRectFilled(ImVec2(gx0, gMin.y), ImVec2(gx1, gMax.y), theme::withAlpha(theme::rgb(o->color), 0.05f));
      float prevY = 0;
      for (float x = std::floor(gx0); x <= gx1; x += 1.0f) {
        double content = songAt(x) - static_cast<double>(c.start) + static_cast<double>(c.offset);
        float y = yOf(paramToClip(ac, clipToParam(*o, automationValue(*o, content))));
        if (x > std::floor(gx0)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), theme::withAlpha(P.textDim, 0.45f), 1.2f);
        prevY = y;
      }
    }
  }
  // J-67: points being moved glide between grid lines (smooth motion, like
  // clip moves): drawn from a copy shifted by the eased minus the snapped
  // shift; the document keeps the snapped positions.
  if (E.g != Editor::Move) E.moveDelta = 0;
  const AutomationClip* drawAc = &ac;
  AutomationClip glided;
  {
    const double d = static_cast<double>(E.moveDelta);
    const double off = scrollui::glide(0x41454D4F5645ull, d, E.g == Editor::Move && snapG > 1, 0.5) - d;
    if (off != 0 && !E.sel.empty()) {
      glided = ac;
      for (int i : E.sel)
        if (i >= 0 && static_cast<size_t>(i) < glided.points.size())
          glided.points[static_cast<size_t>(i)].pos = std::max<Tick>(0, glided.points[static_cast<size_t>(i)].pos + static_cast<Tick>(std::llround(off)));
      for (size_t i = 1; i < glided.points.size(); ++i)  // keep the drawn copy in order while it glides
        glided.points[i].pos = std::max(glided.points[i].pos, glided.points[i - 1].pos);
      drawAc = &glided;
    }
  }
  // ---- the curve ----
  const ImU32 col = theme::rgb(ac.color);
  const ImU32 ink = theme::mix(col, P.paper, 0.35f);
  {
    float prevY = 0;
    const float x0 = gMin.x, x1 = gMax.x;
    for (float x = std::floor(x0); x <= x1; x += 1.0f) {
      double song = songAt(x);
      bool inClip = song >= static_cast<double>(vis0) && song <= static_cast<double>(vis1);
      float y = yOf(automationValue(*drawAc, song - static_cast<double>(base)));
      dl->AddLine(ImVec2(x, y), ImVec2(x, gMax.y), theme::withAlpha(col, inClip ? (E.preview ? 0.08f : 0.16f) : 0.05f));
      if (x > std::floor(x0)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), inClip ? (E.preview ? theme::withAlpha(ink, 0.4f) : ink) : theme::withAlpha(ink, 0.35f), 1.6f);
      prevY = y;
    }
  }
  if (E.preview) {  // tool preview (LFO, scale, thin)
    float prevY = 0;
    for (float x = std::floor(gMin.x); x <= gMax.x; x += 1.0f) {
      float y = yOf(ae::valueAt(*E.preview, songAt(x) - static_cast<double>(base), ac.flCurves));
      if (x > std::floor(gMin.x)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), P.hot, 1.8f);
      prevY = y;
    }
  }
  // ---- points + tension handles ----
  const auto& pts = ac.points;
  int hoverPoint = -1, hoverHandle = -1;
  const ImVec2 m = io.MousePos;
  const bool inGrid = m.x >= gMin.x && m.x < gMax.x && m.y >= gMin.y && m.y < gMax.y;
  if (hovered && inGrid && !E.preview) {
    float best = 7.5f * 7.5f;
    for (size_t i = 0; i < pts.size(); ++i) {
      float dx = m.x - xOf(pts[i].pos), dy = m.y - yOf(pts[i].value);
      if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, hoverPoint = static_cast<int>(i);
    }
    if (hoverPoint < 0)
      for (size_t i = 1; i < pts.size(); ++i) {
        if (pts[i].shape == AutoShape::Hold || xOf(pts[i].pos) - xOf(pts[i - 1].pos) < 16) continue;
        double mid = 0.5 * static_cast<double>(pts[i - 1].pos + pts[i].pos);
        float dx = m.x - xOfSong(mid + static_cast<double>(base)), dy = m.y - yOf(automationValue(ac, mid));
        if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, hoverHandle = static_cast<int>(i);
      }
  }
  if (E.g == Editor::Tension) hoverHandle = E.anchor;
  if (!E.preview) {
    const auto& dpts = drawAc->points;  // J-67: drawn (possibly gliding) positions
    for (size_t i = 1; i < dpts.size(); ++i) {
      if (dpts[i].shape == AutoShape::Hold || xOf(dpts[i].pos) - xOf(dpts[i - 1].pos) < 16) continue;
      double mid = 0.5 * static_cast<double>(dpts[i - 1].pos + dpts[i].pos);
      ImVec2 c(xOfSong(mid + static_cast<double>(base)), yOf(automationValue(*drawAc, mid)));
      if (c.x < gMin.x - 8 || c.x > gMax.x + 8) continue;
      bool hot = static_cast<int>(i) == hoverHandle;
      dl->AddCircleFilled(c, hot ? 4.5f : 3.5f, P.field, 12);
      dl->AddCircle(c, hot ? 4.5f : 3.5f, hot ? P.paper : theme::withAlpha(ink, 0.8f), 12, 1.3f);
    }
    for (size_t i = 0; i < dpts.size(); ++i) {
      ImVec2 c(xOf(dpts[i].pos), yOf(dpts[i].value));
      if (c.x < gMin.x - 8 || c.x > gMax.x + 8) continue;
      bool sel = E.sel.count(static_cast<int>(i)) > 0, hot = static_cast<int>(i) == hoverPoint;
      dl->AddCircleFilled(c, hot ? 6.0f : 5.0f, sel ? P.hot : P.field, 16);
      dl->AddCircle(c, hot ? 6.0f : 5.0f, sel ? theme::mix(P.hot, P.paper, 0.5f) : hot ? P.paper : ink, 16, 1.6f);
    }
  }
  // Playhead.
  if (const double ph = contentPlayhead(a); ph >= 0) {  // J-173: pattern clips follow the pattern's playhead
    float x = std::round(xOfSong(static_cast<double>(base) + ph));
    dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), P.playhead, 1.5f);
  }
  if (E.g == Editor::Box && active) {
    dl->AddRectFilled(E.start, m, theme::withAlpha(theme::litRole(theme::role::kSelect), 0.12f));
    dl->AddRect(E.start, m, theme::litRole(theme::role::kSelect), 0, 0, 1.5f);
  }
  if (pts.empty() || pid.owner == kNoId)
    ui::EmptyState(dl, gMin, gMax, pid.owner == kNoId ? "No target" : "No points", pid.owner == kNoId ? "Choose the parameter this clip drives (left)" : "Click to add a point");
  dl->PopClipRect();

  // ---- ruler ----
  dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(gMax.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(gMax.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  dl->PushClipRect(ImVec2(gMin.x, origin.y), ImVec2(gMax.x, gMin.y), true);
  {
    const Tick labelGrid = timeline::visibleGrid(s, E.pxPerTick, 64.0);
    for (Tick t = snapDown(std::max<Tick>(0, static_cast<Tick>(songAt(gMin.x))), visGrid); xOfSong(static_cast<double>(t)) < gMax.x; t += visGrid) {
      float x = std::round(xOfSong(static_cast<double>(t)));
      bool major = t % labelGrid == 0;
      dl->AddLine(ImVec2(x, gMin.y - (major ? 10.0f : 4.0f)), ImVec2(x, gMin.y - 1), major ? P.lineStrong : P.line);
      if (major)
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 4, origin.y + 4), t % barT == 0 ? P.textDim : P.textFaint,
                    timeline::rulerLabel(s, t, labelGrid).c_str());
    }
    // Clip region in the ruler.
    float rx0 = xOfSong(static_cast<double>(vis0)), rx1 = xOfSong(static_cast<double>(vis1));
    dl->AddRectFilled(ImVec2(rx0, gMin.y - 4), ImVec2(rx1, gMin.y - 1), theme::withAlpha(col, 0.8f));
    if (ac.loop > 0)
      for (Tick l = ac.loop; base + l < vis1; l += ac.loop) {
        float x = std::round(xOf(l));
        dl->AddTriangleFilled(ImVec2(x - 4, gMin.y - 10), ImVec2(x + 4, gMin.y - 10), ImVec2(x, gMin.y - 3), P.textDim);
      }
    if (const double ph = contentPlayhead(a); ph >= 0) {
      float x = std::round(xOfSong(static_cast<double>(base) + ph));
      dl->AddTriangleFilled(ImVec2(x - 5, gMin.y - 9), ImVec2(x + 5, gMin.y - 9), ImVec2(x, gMin.y - 1), P.playhead);
    }
  }
  dl->PopClipRect();
  // ---- value axis ----
  dl->AddRectFilled(ImVec2(origin.x, gMin.y), ImVec2(gMin.x, gMax.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(gMin.x - 1, gMin.y), ImVec2(gMin.x - 1, gMax.y), P.dark ? P.ink : P.lineStrong);
  for (int k = 0; k <= 4; ++k) {
    float y = yOf(k / 4.0);
    std::string t = fmtParam(s, pid, clipToParam(ac, k / 4.0));
    theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(origin.x + 4, std::clamp(y - 7, gMin.y + 1, std::max(gMin.y + 1, gMax.y - 14))), axisW - 8,
                        k == 2 ? P.textDim : P.textFaint, t.c_str());
  }
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(origin.x + 6, origin.y + 7), P.textFaint, "VALUE");

  // ---- interaction ----
  const Tick curContent = contentAt(std::clamp(m.x, gMin.x, gMax.x));
  // Read-out under the pointer.
  if (hovered && inGrid) {
    double v = hoverPoint >= 0 ? pts[static_cast<size_t>(hoverPoint)].value : valueAtY(m.y);
    Tick t = hoverPoint >= 0 ? pts[static_cast<size_t>(hoverPoint)].pos : snapContent(curContent);
    std::string where = timeline::rulerLabel(s, std::max<Tick>(0, base + t), std::max<Tick>(1, s.ppq / 4 / 4));
    std::string text = where + "  \xC2\xB7  " + fmtParam(s, pid, clipToParam(ac, v));
    if (hoverHandle >= 0) {
      char b[48];
      std::snprintf(b, sizeof b, "Tension %+.0f%%", pts[static_cast<size_t>(hoverHandle)].tension * 100);
      text = b;
    }
    ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, text.c_str());
    ImVec2 tp(std::min(m.x + 14, gMax.x - ts.x - 10), std::max(gMin.y + 4, m.y - 26));
    dl->AddRectFilled(ImVec2(tp.x - 5, tp.y - 3), ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3), theme::withAlpha(theme::popupBg(P), 0.95f), theme::kRadiusSm);
    dl->AddRect(ImVec2(tp.x - 5, tp.y - 3), ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3), P.dark ? theme::withAlpha(P.ink, 0.6f) : P.line, theme::kRadiusSm);
    dl->AddText(a.fontMono, theme::kFontLabel, tp, P.text, text.c_str());
    if (hoverPoint >= 0 || hoverHandle >= 0) ImGui::SetMouseCursor(hoverHandle >= 0 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Hand);
    a.setHint("Click: add a point \xC2\xB7 drag points (Shift+click: add to selection) \xC2\xB7 drag a ring: tension \xC2\xB7 Ctrl+drag: select \xC2\xB7 "
              "right-click a point: curve menu \xC2\xB7 Alt: no snap \xC2\xB7 Ctrl+wheel: zoom");
  }
  if (E.preview) return;  // a tool window is previewing: no edits on the canvas
  const bool lClick = hovered && inGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool rClick = hovered && inGrid && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  if (lClick || rClick) {
    E.g = Editor::None;
    E.start = m;
    E.startTick = curContent;
    E.base = pts;
    E.key = 0x4145000000000000ull ^ (gestureCounter++ << 8);
    if (rClick) {
      if (hoverPoint >= 0) {
        E.menuPoint = hoverPoint;
        if (!E.sel.count(hoverPoint)) E.sel = {hoverPoint};
        ImGui::OpenPopup("##aepoint");
      } else if (E.tool == Tool::Erase) {
        E.g = Editor::Erase;
      } else {
        ImGui::OpenPopup("##aemenu");
      }
    } else if (E.tool == Tool::Erase) {
      E.g = Editor::Erase;
    } else if (hoverPoint >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      E.menuPoint = hoverPoint;
      E.typeValue = true;
    } else if (hoverPoint >= 0) {
      if (io.KeyShift) {
        if (!E.sel.erase(hoverPoint)) E.sel.insert(hoverPoint);
      } else {
        if (!E.sel.count(hoverPoint)) E.sel = {hoverPoint};
        E.g = Editor::Move;
        E.anchor = hoverPoint;
      }
    } else if (hoverHandle >= 0 && E.tool == Tool::Draw) {
      E.g = Editor::Tension;
      E.anchor = hoverHandle;
      E.baseTension = pts[static_cast<size_t>(hoverHandle)].tension;
    } else if (io.KeyCtrl || E.tool == Tool::Select) {
      E.g = Editor::Box;
      if (!io.KeyShift) E.sel.clear();
    } else if (pid.owner != kNoId || !pts.empty()) {  // draw: add a point and drag it
      AutoPoint np;
      np.pos = std::max<Tick>(0, snapContent(curContent));
      np.value = static_cast<float>(valueAtY(m.y));
      np.shape = pts.empty() ? AutoShape::Curve : pts.back().shape == AutoShape::Hold ? AutoShape::Hold : AutoShape::Curve;
      int idx = 0;
      editPoints(a, "add automation point", [&](ae::Points& p) { idx = ae::addPoint(p, np); }, E.key);
      E.sel = {idx};
      E.base = curClip(a.doc.song())->points;
      E.g = Editor::Move;
      E.anchor = idx;
      E.startTick = np.pos;
      E.start = ImVec2(m.x, yOf(np.value));
    }
  }
  if (active && E.g != Editor::None) {
    const float edge = 24.0f;
    if (E.g != Editor::Box && E.g != Editor::Tension) {
      if (m.x > gMax.x - edge) E.scrollX += (m.x - (gMax.x - edge)) * 0.35f;
      if (m.x < gMin.x + edge) E.scrollX = std::max(0.0f, E.scrollX - ((gMin.x + edge) - m.x) * 0.35f);
    }
    switch (E.g) {
      case Editor::Move: {
        if (E.anchor < 0 || E.anchor >= static_cast<int>(E.base.size())) break;
        const AutoPoint& an = E.base[static_cast<size_t>(E.anchor)];
        Tick target = io.KeyShift ? an.pos : snapContent(an.pos + (curContent - E.startTick));
        double dv = static_cast<double>((E.start.y - m.y) / (y1 - y0));
        if (io.KeyShift && std::fabs(m.x - E.start.x) > std::fabs(m.y - E.start.y)) {  // Shift: one axis
          target = snapContent(an.pos + (curContent - E.startTick));
          dv = 0;
        }
        ae::Points out;
        E.moveDelta = target - an.pos;
        ae::movePoints(E.base, E.sel, target - an.pos, dv, out);
        if (out != curClip(a.doc.song())->points) editPoints(a, "move automation points", [&](ae::Points& p) { p = out; }, E.key);
        break;
      }
      case Editor::Tension: {
        int k = E.anchor;
        if (k < 1 || k >= static_cast<int>(pts.size())) break;
        float rise = pts[static_cast<size_t>(k)].value >= pts[static_cast<size_t>(k) - 1].value ? 1.0f : -1.0f;
        float t = std::clamp(E.baseTension + (m.y - E.start.y) / 120.0f * rise, -1.0f, 1.0f);
        if (t != pts[static_cast<size_t>(k)].tension)
          editPoints(a, "automation tension", [&](ae::Points& p) { p[static_cast<size_t>(k)].tension = t; }, E.key);
        break;
      }
      case Editor::Box: {
        Tick t0 = contentAt(std::min(E.start.x, m.x)), t1 = contentAt(std::max(E.start.x, m.x));
        ae::Selection box = ae::selectBox(pts, t0, t1, valueAtY(std::max(E.start.y, m.y)), valueAtY(std::min(E.start.y, m.y)));
        if (io.KeyShift)
          for (int i : box) E.sel.insert(i);
        else
          E.sel = box;
        break;
      }
      case Editor::Erase:
        if (hoverPoint >= 0 && pts.size() > 1) {
          int k = hoverPoint;
          editPoints(a, "delete automation points", [&](ae::Points& p) { ae::deletePoints(p, {k}); }, E.key);
          E.sel.clear();
        }
        break;
      default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    a.doc.endGesture();
    E.g = Editor::None;
  }
}

void drawToolbar(App& a, const AutomationClip& ac) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ui::BeginToolbar("##aetb");
  const float fh = ImGui::GetFrameHeight();
  menuButton("aemenu", "Automation editor menu: selection, clipboard, flip, scale, quantize, thin, curves, LFO", [&a]() { editorMenu(a); });
  ImGui::SameLine(0, 4);
  // Q-12: the shared tool family; tooltips "Name (key): what it does" from the keymap.
  struct T {
    ui::Icon icon;
    Tool tool;
    const char* name;
    const char* desc;
    const char* key;
    uint32_t role;
  };
  const T tools[] = {{ui::Icon::ToolDraw, Tool::Draw, i18n::N_("Draw"), i18n::N_("click to add points, drag points and tension rings"), "auto.toolDraw", theme::role::kDraw},
                     {ui::Icon::ToolSelect, Tool::Select, i18n::N_("Select"), i18n::N_("drag a box (Ctrl+drag works with any tool)"), "auto.toolSelect", theme::role::kSelect},
                     {ui::Icon::ToolDelete, Tool::Erase, i18n::N_("Delete"), i18n::N_("click or drag over points to delete them"), "auto.toolDelete", theme::role::kDelete}};
  for (const auto& t : tools) {
    if (ui::ToolButton(t.key, t.icon, E.tool == t.tool, fh, t.name, t.desc, t.key, theme::rgb(t.role))) E.tool = t.tool;
    if (E.tool == t.tool) ui::noteActiveTool("autoedit", static_cast<int>(t.tool), t.name, t.key);
    ImGui::SameLine(0, 2);
  }
  ui::ToolbarDivider();
  ui::ToolButton("aesnap", ui::Icon::ToolSnap, E.snap != Snap::None, fh, i18n::N_("Snap"),
                 i18n::N_("snap to the grid chosen beside it (hold Alt while dragging to bypass)"), nullptr, theme::rgb(theme::role::kSnap));
  ImGui::SameLine(0, 2);
  std::string snapLabel = snapName(E.snap);
  if (ui::BeginCombo("##aesnapc", snapLabel.c_str(), 110)) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::Selectable(snapName(static_cast<Snap>(i)), E.snap == static_cast<Snap>(i))) E.snap = static_cast<Snap>(i);
    ImGui::EndCombo();
  }
  ui::ToolbarDivider();
  if (ui::IconButton("aezo", ui::Icon::Minus, false, fh, "Zoom out (Ctrl+wheel)")) E.pxPerTick /= 1.5f;
  ImGui::SameLine(0, 2);
  if (ui::IconButton("aezi", ui::Icon::Plus, false, fh, "Zoom in (Ctrl+wheel)")) E.pxPerTick *= 1.5f;
  ImGui::SameLine(0, 2);
  if (ui::IconButton("aezf", ui::Icon::Playlist, false, fh, "Zoom to fit the clip")) E.fit = true;
  ui::ToolbarDivider();
  if (ui::TextButton("LFO", E.lfoOpen, 0, "LFO tool: generate a sine, triangle, square or saw over the selection (or the clip)"))
    E.lfoOpen = !E.lfoOpen, E.scaleOpen = E.thinOpen = false;
  ImGui::SameLine(0, 2);
  if (ui::TextButton("SCALE", E.scaleOpen, 0, "Scale levels: centre, multiplier, offset")) E.scaleOpen = !E.scaleOpen, E.lfoOpen = E.thinOpen = false;
  ImGui::SameLine(0, 2);
  bool& rec = automationRecording();
  if (ui::IconButton("aerec", ui::Icon::Record, rec, fh, "Record automation: moving a control while playing writes points", P.danger)) rec = !rec;
  // Right: point count / selection.
  ImGui::SameLine(0, theme::kS4);
  float room = ImGui::GetContentRegionAvail().x;
  if (room > 120) {
    char b[64];
    std::snprintf(b, sizeof b, "%zu points%s", ac.points.size(), E.sel.empty() ? "" : (" \xC2\xB7 " + std::to_string(E.sel.size()) + " selected").c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, b).x;
    ImGui::GetWindowDrawList()->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + room - tw - 8, p.y + (fh - theme::kFontLabel) * 0.5f), P.textDim, b);
    ImGui::Dummy(ImVec2(room - 1, fh));
  }
  (void)s;
  ui::EndToolbar();
}

void shortcuts(App& a, const AutomationClip& ac) {
  ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) return;
  const int n = static_cast<int>(ac.points.size());
  if (keys::pressed("auto.delete") && !E.sel.empty()) deleteSelection(a);
  if (keys::pressed("auto.selectAll"))
    for (int i = 0; i < n; ++i) E.sel.insert(i);
  if (keys::pressed("auto.deselect")) E.sel.clear();
  if (keys::pressed("auto.invertSelection")) {
    ae::Selection inv;
    for (int i = 0; i < n; ++i)
      if (!E.sel.count(i)) inv.insert(i);
    E.sel = inv;
  }
  if (keys::pressed("auto.copy") && !E.sel.empty()) copySelection(a, ac);
  if (keys::pressed("auto.cut") && !E.sel.empty()) {
    copySelection(a, ac);
    deleteSelection(a);
  }
  if (keys::pressed("auto.paste")) pasteAt(a, pasteTick(a, ac), false);
  if (keys::pressed("auto.pasteReplace")) pasteAt(a, pasteTick(a, ac), true);
  if (keys::pressed("auto.toolDraw")) E.tool = Tool::Draw;
  if (keys::pressed("auto.toolSelect")) E.tool = Tool::Select;
  if (keys::pressed("auto.toolDelete")) E.tool = Tool::Erase;
  if (keys::pressed("auto.lfo")) E.lfoOpen = true, E.scaleOpen = E.thinOpen = false;
  if (keys::pressed("auto.quantize")) {
    const Song& s = a.doc.song();
    Tick grid = timeline::snapGrid(s, E.snap == Snap::None ? Snap::Line : E.snap, E.pxPerTick, false);
    ae::Selection t = E.sel, out;
    editPoints(a, "quantize points", [&](ae::Points& p) { out = ae::quantize(p, t, grid); });
    E.sel = out;
  }
  if (keys::pressed("auto.cancel")) {
    if (E.preview) E.lfoOpen = E.scaleOpen = E.thinOpen = false, E.preview.reset();
    else E.sel.clear();
  }
}

// ------------------------------------------------------------- recording ----

struct Recording {
  ParamId pid;
  bool pattern = false;
  ObjectId patternId = kNoId;
  ObjectId autoId = kNoId, placement = kNoId;  // song mode: clip being written (kNoId: create one)
  ae::Points pts;                               // absolute ticks (song or pattern), parameter-normalized values
  double last = 0;
};
std::vector<Recording> recs;
bool recordOn = false;
bool recordHold = false;
bool observerInstalled = false;

void commitRecording(App& a, Recording& r) {
  if (r.pts.size() < 1) return;
  const Song& s = a.doc.song();
  Tick from = r.pts.front().pos, to = r.pts.back().pos;
  for (const auto& p : r.pts) from = std::min(from, p.pos), to = std::max(to, p.pos);
  const double tol = 0.004;
  if (r.pattern) {
    const Pattern* p = s.pattern(r.patternId);
    if (!p) return;
    ae::Points lane, rec = r.pts;
    if (const PatternAutomation* l = findPatternLane(*p, r.pid.owner, r.pid.index)) {
      lane = l->points;
      for (auto& q : rec) q.value = static_cast<float>(paramToAutomation(*l, q.value));  // J-173: into the clip's range
    }
    ae::mergeRecorded(lane, rec, from, to, tol);
    ParamId pid = r.pid;
    a.doc.editPattern(r.patternId, "record automation", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, lane); });
    return;
  }
  const AutomationClip* ac = r.autoId != kNoId ? s.automation(r.autoId) : nullptr;
  const Clip* pl = r.placement != kNoId ? s.clip(r.placement) : nullptr;
  if (ac && pl) {
    ae::Points rec;
    for (auto p : r.pts) {
      p.pos = p.pos - pl->start + pl->offset;
      p.value = static_cast<float>(paramToClip(*ac, p.value));
      rec.push_back(p);
    }
    Tick cf = from - pl->start + pl->offset, ct = to - pl->start + pl->offset;
    a.doc.editAutomation(ac->id, "record automation", [&](AutomationClip& c) { ae::mergeRecorded(c.points, rec, cf, ct, tol); });
    return;
  }
  // New clip from the start of the gesture's beat.
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  const Tick start = from / beat * beat;
  const Tick len = std::max(beat, (to - start + beat - 1) / beat * beat);
  ae::Points rec;
  for (auto p : r.pts) {
    p.pos -= start;
    rec.push_back(p);
  }
  ae::Points pts;
  ae::mergeRecorded(pts, rec, from - start, to - start, tol);
  ParamId pid = r.pid;
  std::string name = paramDisplayName(s, pid);
  // A free playlist track for [start, start + len).
  int track = 0;
  for (;; ++track) {
    bool busy = false;
    for (const auto& c : s.playlist) busy = busy || (c.track == track && c.start < start + len && c.start + s.clipLength(c) > start);
    if (!busy) break;
  }
  a.doc.editSong("record automation clip", [&](Song& x) {
    AutomationClip c;
    c.id = x.allocateId();
    c.name = name;
    c.owner = pid.owner;
    c.index = pid.index;
    c.points = pts;
    Clip k;
    k.id = x.allocateId();
    k.kind = ClipKind::Automation;
    k.source = c.id;
    k.track = track;
    k.start = start;
    k.length = len;
    while (static_cast<int>(x.tracks.size()) <= track) x.tracks.push_back(PlaylistTrack{});
    x.automations.push_back(std::move(c));
    x.playlist.push_back(k);
  });
}

void onParamSet(App& a, ParamId pid, double value) {
  if (!recordOn || recordHold || !a.engine.playing()) return;
  const Song& s = a.doc.song();
  auto d = describeParam(s, pid);
  if (!d) return;
  const double n = d->toNormalized(value);
  const bool pattern = a.engine.mode() == PlayMode::Pattern;
  const Tick t = static_cast<Tick>(a.engine.position());
  Recording* r = nullptr;
  for (auto& x : recs)
    if (x.pid == pid) r = &x;
  if (r && !r->pts.empty() && t < r->pts.back().pos) {  // pattern loop wrapped / position jumped: close the take
    commitRecording(a, *r);
    recs.erase(recs.begin() + (r - recs.data()));
    r = nullptr;
  }
  if (!r) {
    Recording nr;
    nr.pid = pid;
    nr.pattern = pattern;
    nr.patternId = a.curPattern;
    if (!pattern)  // the clip of this target under the playhead
      for (const auto& c : s.playlist) {
        const AutomationClip* ac = c.kind == ClipKind::Automation ? s.automation(c.source) : nullptr;
        if (!ac || ac->owner != pid.owner || ac->index != pid.index || c.muted) continue;
        if (t >= c.start && t < c.start + s.clipLength(c)) nr.autoId = ac->id, nr.placement = c.id;
      }
    recs.push_back(nr);
    r = &recs.back();
  }
  AutoPoint p;
  p.pos = t;
  p.value = static_cast<float>(n);
  p.shape = AutoShape::Hold;
  r->pts.push_back(p);
  r->last = a.now;
}

void recorderTick(App& a) {
  if (!observerInstalled) {
    observerInstalled = true;
    a.doc.paramObserver = [&a](ParamId pid, double v) { onParamSet(a, pid, v); };
  }
  for (size_t i = 0; i < recs.size();) {
    if (!a.engine.playing() || a.now - recs[i].last > 0.35 || !recordOn) {
      Recording r = recs[i];
      recs.erase(recs.begin() + static_cast<long>(i));
      commitRecording(a, r);
      a.doc.endGesture();
    } else {
      ++i;
    }
  }
}

// --------------------------------------------------------- pattern lane ----

ParamId laneTarget{kNoId, 0};

struct LaneGesture {
  bool drawing = false, line = false;  // left drag draws, right drag is the line tool (Q-4, FL)
  double lastTick = -1;
  float lastY = 0;
  double tickA = 0;  // line tool: start point
  float yA = 0;
  ae::Points base;   // line tool: the lane at the press
  uint64_t key = 0;
} lg;

}  // namespace

// =================================================================== API ====

bool& automationRecording() { return recordOn; }
bool& automationRecordHold() { return recordHold; }
ParamId& pianoAutomationLane() { return laneTarget; }

void openAutomationEditor(App& a, ObjectId automation, ObjectId placement) {
  (void)a;
  if (E.autoId != automation || E.pattern != kNoId) E.sel.clear();
  E.open = true;
  E.pattern = kNoId;
  E.target = ParamId{};
  E.autoId = automation;
  E.placement = placement;
  E.fit = true;
  E.preview.reset();
  E.lfoOpen = E.scaleOpen = E.thinOpen = false;
  if (placement == kNoId)  // the first placement, when there is one
    for (const auto& c : a.doc.song().playlist)
      if (c.kind == ClipKind::Automation && c.source == automation) {
        E.placement = c.id;
        break;
      }
}

void openPatternAutomation(App& a, ObjectId pattern, ParamId target) {
  const Pattern* p = a.doc.song().pattern(pattern);
  if (!p || !findPatternLane(*p, target.owner, target.index)) return;
  if (E.pattern != pattern || !(E.target == target)) E.sel.clear();
  E.open = true;
  E.autoId = kNoId;
  E.placement = kNoId;
  E.pattern = pattern;
  E.target = target;
  E.fit = true;
  E.preview.reset();
  E.lfoOpen = E.scaleOpen = E.thinOpen = false;
}

bool automationEditorTarget(ObjectId* automation, ObjectId* pattern, ParamId* target) {
  if (!E.open) return false;
  if (automation) *automation = E.autoId;
  if (pattern) *pattern = E.pattern;
  if (target) *target = E.target;
  return true;
}

void drawAutomationEditor(App& a) {
  recorderTick(a);
  if (!E.open) {
    E.lfoOpen = E.scaleOpen = E.thinOpen = E.toolWasOpen = false;
    return;
  }
  const Song& s = a.doc.song();
  const AutomationClip* ac = curClip(s);
  if (!ac) {
    E.open = false;
    E.preview.reset();
    return;
  }
  ui::PlaceNextDialog(ImVec2(1120, 640), ImGuiCond_Appearing, ImVec2(640, 360));  // J-158: within the work area (it ran under the status bar)
  bool open = true;
  std::string title = "Automation clip \xC2\xB7 " + (E.pattern != kNoId ? patternAutoName(s, *ac) : ac->name);
  if (E.pattern != kNoId)  // J-173: a pattern's clip
    if (const Pattern* p = s.pattern(E.pattern)) title += " \xC2\xB7 in " + p->name;
  title += "###autoedit";
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  ui::KeepWindowInViewport();
  E.winMin = ImGui::GetWindowPos();
  E.winMax = ImVec2(E.winMin.x + ImGui::GetWindowWidth(), E.winMin.y + ImGui::GetWindowHeight());
  if (vis) {
    drawToolbar(a, *ac);
    const float sideW = 248;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(pal().surface900));
    ImGui::BeginChild("##aeside", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    if (const AutomationClip* cur = curClip(a.doc.song())) sidePanel(a, *cur, sideW);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SameLine(0, 0);
    if (const AutomationClip* cur = curClip(a.doc.song())) {
      drawCanvas(a, *cur);
      if (ImGui::BeginPopup("##aepoint")) {
        pointMenu(a, *curClip(a.doc.song()));
        ImGui::EndPopup();
      }
      if (ImGui::BeginPopup("##aemenu")) {
        editorMenu(a);
        ImGui::EndPopup();
      }
      if (E.typeValue) {
        ImGui::OpenPopup("##ptvalue");
        E.typeValue = false;
      }
      if (const AutomationClip* c2 = curClip(a.doc.song())) {
        typeValuePopup(a, *c2);
        shortcuts(a, *c2);
      }
    }
  }
  ImGui::End();
  if (!open) {
    E.open = false;
    E.lfoOpen = E.scaleOpen = E.thinOpen = false;
    E.preview.reset();
  }
  // Tool dialogs float over the editor.
  drawLfoTool(a);
  drawLevelTools(a);
  E.toolWasOpen = E.lfoOpen || E.scaleOpen || E.thinOpen;
}

ObjectId createAutomationClip(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return kNoId;
  const float n = static_cast<float>(d->toNormalized(*v));
  const Tick bar = std::max<Tick>(1, s.barTicks());
  Tick start = 0, len = 4 * bar;
  if (a.playlist.selEnd > a.playlist.selStart) {
    start = a.playlist.selStart;
    len = a.playlist.selEnd - a.playlist.selStart;
  } else if (a.engine.mode() == PlayMode::Song) {
    start = static_cast<Tick>(a.engine.position()) / bar * bar;  // at the playback position, like FL
  }
  // First track (from the bottom of the used ones) that is free over the clip.
  int track = 0;
  for (const auto& c : s.playlist) track = std::max(track, c.track + 1);
  for (int t = 0; t < track; ++t) {
    bool busy = false;
    for (const auto& c : s.playlist) busy = busy || (c.track == t && c.start < start + len && c.start + s.clipLength(c) > start);
    bool hasAuto = false;
    for (const auto& c : s.playlist) hasAuto = hasAuto || (c.track == t && c.kind == ClipKind::Automation);
    if (!busy && hasAuto) {
      track = t;
      break;
    }
  }
  std::string name = paramDisplayName(s, pid);
  ObjectId autoId = kNoId, clipId = kNoId;
  a.doc.editSong("create automation clip", [&](Song& x) {
    AutomationClip c;
    c.id = x.allocateId();
    c.name = name;
    c.color = 0x7A8696;
    c.owner = pid.owner;
    c.index = pid.index;
    c.points = {AutoPoint{0, n, 0, AutoShape::Curve}, AutoPoint{len, n, 0, AutoShape::Curve}};
    Clip k;
    k.id = x.allocateId();
    k.kind = ClipKind::Automation;
    k.source = c.id;
    k.track = track;
    k.start = start;
    k.length = len;
    while (static_cast<int>(x.tracks.size()) <= track) x.tracks.push_back(PlaylistTrack{});
    autoId = c.id;
    clipId = k.id;
    x.automations.push_back(std::move(c));
    x.playlist.push_back(k);
  });
  openAutomationEditor(a, autoId, clipId);
  a.notify("Automation clip for " + name);
  return autoId;
}

bool createPatternAutomationClip(App& a, ParamId pid, ObjectId pattern) {
  const Song& s = a.doc.song();
  if (pattern == kNoId) pattern = a.curPattern;
  const Pattern* p = s.pattern(pattern);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!p || !d || !v) return false;
  if (!findPatternLane(*p, pid.owner, pid.index)) {
    PatternAutomation c = newPatternClip(s, pid, static_cast<float>(d->toNormalized(*v)), s.patternContentLength(*p));
    a.doc.editPattern(pattern, "create automation clip in pattern", [&](Pattern& x) { addPatternClip(x, c); });
    a.notify("Automation clip for " + paramDisplayName(s, pid) + " in " + p->name);
  }
  openPatternAutomation(a, pattern, pid);
  return true;
}

void editPatternEvents(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!p || !d || !v) {
    a.notify("Select a pattern first", true);
    return;
  }
  if (!findPatternLane(*p, pid.owner, pid.index)) {
    AutoPoint pt{0, static_cast<float>(d->toNormalized(*v)), 0, AutoShape::Hold};
    a.doc.editPattern(p->id, "edit events", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, {pt}); });
  }
  laneTarget = pid;
  if (s.channel(pid.owner)) a.openPianoRoll(pid.owner);
  a.showPiano = true;
  a.focusWindow = "Piano roll";
}

void automationMenuItems(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  if (!describeParam(s, pid)) return;
  // J-173: a clip in the playlist or inside the current pattern (the pattern first while
  // pattern mode plays, the arrangement first in song mode).
  const Pattern* curPat = s.pattern(a.curPattern);
  const bool hasInPat = curPat && findPatternLane(*curPat, pid.owner, pid.index);
  const std::string inPat = curPat ? (hasInPat ? std::string(i18n::tr("Edit automation clip in pattern")) + " \"" + curPat->name + "\""
                                               : std::string(i18n::tr("Create automation clip in pattern")) + " \"" + curPat->name + "\"")
                                   : std::string();
  auto patternItem = [&]() {
    if (!curPat) return;
    if (ImGui::MenuItem(inPat.c_str())) createPatternAutomationClip(a, pid, curPat->id);
    ui::Hint("The clip lives in the pattern and plays wherever the pattern plays; it shows on the pattern's playlist clips");
  };
  const bool patternFirst = a.engine.mode() == PlayMode::Pattern;
  if (patternFirst) patternItem();
  if (ImGui::MenuItem(i18n::tr("Create automation clip"))) createAutomationClip(a, pid);
  ui::Hint("A clip on the playlist, at the song position");
  if (!patternFirst) patternItem();
  std::vector<const AutomationClip*> mine;
  for (const auto& c : s.automations)
    if (c.owner == pid.owner && c.index == pid.index) mine.push_back(&c);
  std::vector<const Pattern*> pats;  // other patterns with a clip for this control
  for (const auto& p : s.patterns)
    if (findPatternLane(p, pid.owner, pid.index) && p.id != a.curPattern) pats.push_back(&p);
  if (mine.size() == 1 && pats.empty()) {
    if (ImGui::MenuItem(i18n::tr("Edit automation clip..."))) openAutomationEditor(a, mine[0]->id);
  } else if (ImGui::BeginMenu(i18n::tr("Edit automation clip"), !mine.empty() || !pats.empty())) {
    for (const auto* c : mine) {
      ImGui::PushID(static_cast<int>(c->id));
      if (ImGui::MenuItem(c->name.c_str())) openAutomationEditor(a, c->id);
      ImGui::PopID();
    }
    if (!mine.empty() && !pats.empty()) ImGui::Separator();
    for (const auto* p : pats) {
      ImGui::PushID(static_cast<int>(p->id) + 0x40000000);
      const std::string l = std::string(i18n::tr("In pattern")) + " " + p->name;
      if (ImGui::MenuItem(l.c_str())) openPatternAutomation(a, p->id, pid);
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Edit events"), nullptr, false, s.pattern(a.curPattern) != nullptr)) editPatternEvents(a, pid);
  if (ImGui::MenuItem(i18n::tr("Start the song at this value"), nullptr, false, !mine.empty())) {
    // The earliest placement's clip starts at the control's current value.
    const Clip* first = nullptr;
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Automation)
        for (const auto* m : mine)
          if (c.source == m->id && (!first || c.start < first->start)) first = &c;
    const AutomationClip* ac = first ? s.automation(first->source) : mine[0];
    double n = describeParam(s, pid)->toNormalized(*getParam(s, pid));
    float v = static_cast<float>(paramToClip(*ac, n));
    Tick at = first ? first->offset : 0;
    a.doc.editAutomation(ac->id, "start song at value", [&](AutomationClip& c) {
      if (c.points.empty() || c.points.front().pos > at) ae::addPoint(c.points, AutoPoint{at, v, 0, AutoShape::Curve});
      for (auto& p : c.points)
        if (p.pos <= at) p.value = v;
    });
  }
  ImGui::MenuItem(i18n::tr("Record automation"), nullptr, &recordOn);
}

void devAutomationEditor(App& a, int mode) {
  const Song& s = a.doc.song();
  if (s.channels.empty()) return;
  ParamId pid = channelParam(s.channels[0].id, ChannelParam::Volume);
  if (mode == 3) {
    ParamId pan = channelParam(s.channels[0].id, ChannelParam::Pan);
    editPatternEvents(a, pan);
    const Pattern* p = a.doc.song().pattern(a.curPattern);
    if (!p) return;
    const Tick step = std::max<Tick>(1, s.ppq / 4);
    ae::Points pts;
    for (int i = 0; i < 16; ++i)
      pts.push_back(AutoPoint{i * step, static_cast<float>(0.5 + 0.4 * std::sin(i * 0.6)), 0, AutoShape::Hold});
    a.doc.editPattern(p->id, "demo events", [&](Pattern& x) { setPatternLane(x, pan.owner, pan.index, pts); });
    return;
  }
  ObjectId id = createAutomationClip(a, pid);
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  a.doc.editAutomation(id, "demo points", [&](AutomationClip& c) {
    c.color = 0x5A8FD8;
    c.points = {AutoPoint{0, 0.15f, 0, AutoShape::Curve},          AutoPoint{2 * beat, 0.85f, 0.45f, AutoShape::Curve},
                AutoPoint{4 * beat, 0.55f, -0.5f, AutoShape::SCurve}, AutoPoint{6 * beat, 0.7f, 0, AutoShape::Hold},
                AutoPoint{8 * beat, 0.25f, 0.6f, AutoShape::Stairs}, AutoPoint{10 * beat, 0.9f, -0.35f, AutoShape::HalfSine},
                AutoPoint{12 * beat, 0.4f, 0, AutoShape::Wave},       AutoPoint{16 * beat, 0.6f, 0.3f, AutoShape::Curve2}};
  });
  // A second clip of the same target later in the song: shows as a ghost.
  a.doc.editSong("demo ghost", [&](Song& x) {
    const AutomationClip* src = x.automation(id);
    const Clip* first = nullptr;
    for (const auto& c : x.playlist)
      if (c.source == id) first = &c;
    if (!src || !first) return;
    AutomationClip g = *src;
    g.id = x.allocateId();
    g.points = {AutoPoint{0, 0.6f, 0, AutoShape::Curve}, AutoPoint{8 * beat, 0.1f, 0.5f, AutoShape::Curve}, AutoPoint{16 * beat, 0.8f, 0, AutoShape::SCurve}};
    Clip k = *first;
    k.id = x.allocateId();
    k.source = g.id;
    k.start = first->start + first->length;
    x.automations.push_back(g);
    x.playlist.push_back(k);
  });
  E.sel = {1, 2};
  if (mode == 2) {
    E.sel = {2, 6};
    E.lfoOpen = true;
    E.lfo.shape = ae::LfoShape::Sine;
    E.lfoDivStart = 5;
    E.lfoDivEnd = 6;
    E.lfo.amountStart = 0.35;
    E.lfo.amountEnd = 0.2;
  }
}

// --------------------------------------------------------- pattern lane ----

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
  if (laneTarget.owner != kNoId && findPatternLane(*p, laneTarget.owner, laneTarget.index)) {
    if (ImGui::MenuItem(i18n::tr("Open in the automation editor"))) openPatternAutomation(a, pattern, laneTarget);  // J-173
    if (ImGui::MenuItem(i18n::tr("Delete this lane"))) {
      ParamId pid = laneTarget;
      a.doc.editPattern(pattern, "delete event lane", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, {}); });
      laneTarget = ParamId{};
    }
  }
}

void drawPatternAutomationLane(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, ObjectId pattern, const std::function<float(double)>& xOf,
                               const std::function<double(float)>& tickAt, Tick grid, bool canvasHovered, bool canvasActive) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return;
  const ParamId pid = laneTarget;
  const PatternAutomation* lane = findPatternLane(*p, pid.owner, pid.index);
  ae::Points pts = lane ? lane->points : ae::Points{};
  // Q-10: drawn values glide to edits (display only; the lane already holds them).
  // J-173: drawn and edited as the parameter moves (the clip's range applied; a new lane: 0..1).
  const AutomationClip rng = lane ? *lane : patternLane(pid.owner, pid.index, {});
  ae::Points shown = pts;
  for (auto& q : shown) {
    q.value = scrollui::glideBar((static_cast<uint64_t>(pattern) << 44) ^ (static_cast<uint64_t>(pid.owner) << 28) ^ (static_cast<uint64_t>(pid.index) << 20) ^
                                     (static_cast<uint64_t>(q.pos) * 0x9E3779B1ull),
                                 static_cast<float>(automationToParam(rng, q.value)));
    q.fine = 0.0;
  }
  auto yOf = [&](double v) { return mx.y - static_cast<float>(v) * (mx.y - mn.y); };
  auto valAt = [&](float y) { return std::clamp(static_cast<double>((mx.y - y) / (mx.y - mn.y)), 0.0, 1.0); };
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool inLane = m.x >= mn.x && m.x < mx.x && m.y >= mn.y - 6 && m.y < mx.y + 4;
  grid = std::max<Tick>(1, grid);

  // ---- draw: event bars per grid step (like an event editor) + the curve ----
  dl->PushClipRect(ImVec2(mn.x, mn.y - 4), mx, true);
  // Event-editor look: graph-role (cyan) fill under the curve, lit stems with
  // round heads at the points (same role as the channel rack's graph editor).
  const ImU32 col = theme::litRole(theme::role::kGraph);
  const ImU32 lineCol = P.dark ? theme::mix(col, P.paper, 0.45f) : col;
  if (!pts.empty()) {
    float prevY = 0;
    for (float x = std::floor(mn.x); x <= mx.x; x += 1.0f) {
      double t = tickAt(x);
      float y = yOf(ae::valueAt(shown, t, rng.flCurves));
      dl->AddLine(ImVec2(x, y), ImVec2(x, mx.y), theme::withAlpha(col, 0.16f));
      if (x > std::floor(mn.x)) dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), theme::withAlpha(lineCol, 0.9f), 1.3f);
      prevY = y;
    }
    for (const auto& q : shown) {
      float x = xOf(static_cast<double>(q.pos));
      if (x < mn.x - 4 || x > mx.x + 4) continue;
      dl->AddRectFilled(ImVec2(x, yOf(q.value)), ImVec2(x + 2, mx.y), theme::withAlpha(col, 0.7f));
      dl->AddCircleFilled(ImVec2(x + 1, yOf(q.value)), 3.0f, lineCol, 10);
    }
  } else {
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(mn.x + 8, mn.y + 4), P.textFaint, i18n::tr("Drag to draw values \xC2\xB7 right-drag for a straight line"));
  }
  dl->PopClipRect();
  if (canvasHovered && inLane) {
    double t = tickAt(m.x);
    a.setHint(paramDisplayName(s, pid) + ": " + fmtParam(s, pid, valAt(m.y)) + " at " + timeline::rulerLabel(s, std::max<Tick>(0, static_cast<Tick>(t)), grid) +
              "   " + i18n::tr("(drag: draw values on the grid \xC2\xB7 right-drag: straight line \xC2\xB7 Alt: free)"));
  }

  // ---- edit ----
  if (canvasHovered && inLane && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    lg = LaneGesture{};
    lg.drawing = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    lg.line = !lg.drawing;
    lg.tickA = tickAt(m.x);
    lg.yA = m.y;
    lg.base = pts;
    lg.key = 0x504C410000000000ull ^ (gestureCounter++ << 8);
  }
  if ((lg.drawing || lg.line) && canvasActive) {
    // Draw: the segment since the last frame (nothing skipped on fast drags).
    // Line: from the press point to the pointer, on the lane as it was at the press.
    double t0 = lg.line ? lg.tickA : lg.lastTick < 0 ? tickAt(m.x) : lg.lastTick, t1 = tickAt(m.x);
    float yA = lg.line ? lg.yA : lg.lastTick < 0 ? m.y : lg.lastY, yB = m.y;
    if (t1 < t0) std::swap(t0, t1), std::swap(yA, yB);
    ae::Points next = lg.line ? lg.base : pts;
    Tick c0 = std::max<Tick>(0, snapDown(static_cast<Tick>(std::floor(t0)), grid)), c1 = std::max<Tick>(0, snapDown(static_cast<Tick>(std::floor(t1)), grid));
    for (Tick c = c0; c <= c1; c += grid) {
      next.erase(std::remove_if(next.begin(), next.end(), [&](const AutoPoint& q) { return q.pos >= c && q.pos < c + grid; }), next.end());
      double f = t1 > t0 ? std::clamp((static_cast<double>(c) - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
      AutoPoint np;
      np.pos = c;
      np.value = static_cast<float>(paramToAutomation(rng, valAt(yA + (yB - yA) * static_cast<float>(f))));
      np.shape = AutoShape::Hold;
      ae::addPoint(next, np);
    }
    if (next != pts)
      a.doc.editPattern(pattern, lg.line ? "draw line" : "draw events", [&](Pattern& x) { setPatternLane(x, pid.owner, pid.index, next); }, lg.key);
    lg.lastTick = tickAt(m.x);
    lg.lastY = m.y;
  }
  if (lg.line && canvasActive) {  // line tool preview
    const ImVec2 pa(xOf(lg.tickA), std::clamp(lg.yA, mn.y, mx.y)), pb(m.x, std::clamp(m.y, mn.y, mx.y));
    dl->PushClipRect(ImVec2(mn.x, mn.y - 4), ImVec2(mx.x, mx.y + 4), true);
    dl->AddLine(pa, pb, theme::withAlpha(P.ink, 0.6f), 3.0f);
    dl->AddLine(pa, pb, col, 1.5f);
    dl->AddCircleFilled(pa, 3.0f, col, 10);
    dl->AddCircleFilled(pb, 3.0f, col, 10);
    dl->PopClipRect();
  }
  if (!canvasActive && (lg.drawing || lg.line)) {
    lg = LaneGesture{};
    a.doc.endGesture();
  }
}

}  // namespace gl
