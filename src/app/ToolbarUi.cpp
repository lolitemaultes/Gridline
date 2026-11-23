// Gridline - the control bars' model in the app. See ToolbarUi.h.
#include "ToolbarUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/CommandPalette.h"
#include "../core/I18n.h"
#include "../core/Settings.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "SettingsPages.h"
#include "Theme.h"
#include "WorkspacesUi.h"

namespace gl::toolbarui {

using layout::Preset;
namespace tb = toolbar;

namespace {

// ---------------------------------------------------------------- model ----

int gFrame = -1;
Preset gFor = Preset::Count;
tb::Model gModel;
bool gDirty = true;

// A --layout run keeps its bars in memory only (like its workspaces and region state).
std::map<int, std::string> gRunText;
bool runOnly() { return layoutui::hasDevOverride(); }

std::string layoutText(App& a, Preset p) {
  if (runOnly()) return gRunText[static_cast<int>(p)];
  return a.settings.getString(tb::settingsKey(p));
}

void storeLayoutText(App& a, Preset p, const std::string& text) {
  if (runOnly()) gRunText[static_cast<int>(p)] = text;
  else if (a.settings.getString(tb::settingsKey(p)) != text) a.settings.set(tb::settingsKey(p), text);
}

// Smooth's older show / hide switches become its bar, once.
void migrate(App& a) {
  if (runOnly() || a.settings.getBool("layout.logic.toolbarMigrated")) return;
  a.settings.set("layout.logic.toolbarMigrated", true);
  if (!a.settings.getString(tb::settingsKey(Preset::Logic)).empty()) return;
  const tb::Bar b = tb::migrateSmooth([&](const std::string& key, bool& v) {
    const Json* j = a.settings.value(key);
    if (!j || !j->is(Json::Type::Bool)) return false;
    v = j->boolean;
    return true;
  });
  tb::Model m;
  m.bar = b;
  storeLayoutText(a, Preset::Logic, tb::toJson(Preset::Logic, m));
}

void dropGonePins(std::vector<tb::Pin>& pins) { tb::cleanPins(pins); }

}  // namespace

tb::Model layoutModel(App& a, Preset p) {
  if (p == Preset::Logic) migrate(a);
  tb::Model m = tb::fromJson(p, layoutText(a, p));
  dropGonePins(m.pins);
  return m;
}

tb::Model modelFor(App& a, Preset p) {
  tb::Model m = layoutModel(a, p);
  const std::string ownBar = workspacesui::activeOwnBar(a, p), ownPins = workspacesui::activeOwnPins(a, p);
  if (!ownBar.empty()) m.bar = tb::barFromJson(p, ownBar);
  if (!ownPins.empty()) {
    m.pins = tb::pinsFromJson(ownPins);
    dropGonePins(m.pins);
  }
  return m;
}

const tb::Model& model(App& a) {
  const Preset p = layoutui::active();
  const int f = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : 0;
  if (gDirty || f != gFrame || p != gFor) {
    gModel = modelFor(a, p);
    gFrame = f;
    gFor = p;
    gDirty = false;
  }
  return gModel;
}

void changed() { gDirty = true; }

bool shown(App& a, const char* part) { return tb::shown(model(a).bar, part); }

bool always(App& a, const char* part) {
  const tb::Part* x = tb::find(model(a).bar, part);
  return x && x->always;
}

bool barIsOwn(App& a, Preset p) { return !workspacesui::activeOwnBar(a, p).empty(); }
bool pinsAreOwn(App& a, Preset p) { return !workspacesui::activeOwnPins(a, p).empty(); }

void setBar(App& a, Preset p, const tb::Bar& b) {
  tb::Bar n = b;
  tb::normalise(p, n);
  if (barIsOwn(a, p)) {
    const std::string text = tb::barJson(n);
    workspacesui::setActiveOwn(a, p, &text, nullptr);
  } else {
    tb::Model m = layoutModel(a, p);
    m.bar = n;
    storeLayoutText(a, p, tb::toJson(p, m));
  }
  changed();
}

void setPins(App& a, Preset p, const std::vector<tb::Pin>& pins) {
  if (pinsAreOwn(a, p)) {
    const std::string text = tb::pinsJson(pins);
    workspacesui::setActiveOwn(a, p, nullptr, &text);
  } else {
    tb::Model m = layoutModel(a, p);
    m.pins = pins;
    storeLayoutText(a, p, tb::toJson(p, m));
  }
  changed();
}

void resetBar(App& a, Preset p) { setBar(a, p, tb::defaults(p)); }

// ---------------------------------------------------------------- pins ----

bool togglePin(App& a, const std::string& command, const std::string& label) {
  const Preset p = layoutui::active();
  std::vector<tb::Pin> pins = modelFor(a, p).pins;
  if (tb::pinned(pins, command)) {
    tb::unpin(pins, command);
  } else if (!tb::pin(pins, tb::Pin{command, label})) {
    return false;
  }
  setPins(a, p, pins);
  return true;
}

bool isPinned(App& a, const std::string& command) { return tb::pinned(model(a).pins, command); }

std::vector<std::string> paletteFirst(App& a) { return tb::paletteFirst(model(a).pins); }

std::vector<tb::Pin> barPins(App& a) {
  std::vector<tb::Pin> out;
  if (!tb::shown(model(a).bar, tb::kPins)) return out;
  for (const tb::Pin& p : model(a).pins)
    if (p.onBar()) out.push_back(p);
  return out;
}

std::string pinLabel(const tb::Pin& p) {
  if (!p.label.empty()) return i18n::tr(p.label.c_str());
  // No name kept: the end of its id ("menu:Tools/Quantize" -> "Quantize", "cmd:edit.undo" -> "edit.undo").
  std::string id = p.command.substr(p.command.find(':') + 1);
  if (const size_t s = id.rfind('/'); s != std::string::npos) id = id.substr(s + 1);
  return id;
}

ui::Icon pinIcon(const tb::Pin& p) {
  std::string id = p.command + " " + p.label;
  for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  struct Word {
    const char* word;
    ui::Icon icon;
  };
  static const Word kWords[] = {
      {"undo", ui::Icon::Undo},        {"redo", ui::Icon::Redo},         {"save", ui::Icon::Save},           {"open", ui::Icon::Open},
      {"mixer", ui::Icon::Mixer},      {"piano", ui::Icon::Piano},       {"playlist", ui::Icon::Playlist},   {"rack", ui::Icon::Rack},
      {"browser", ui::Icon::Browser},  {"metronome", ui::Icon::Metronome}, {"loop", ui::Icon::LoopArrows},  {"cycle", ui::Icon::LoopArrows},
      {"quantiz", ui::Icon::Magnet},   {"quantis", ui::Icon::Magnet},    {"record", ui::Icon::Record},       {"stop", ui::Icon::Stop},
      {"play", ui::Icon::Play},        {"zoom", ui::Icon::Zoom},         {"render", ui::Icon::Download},     {"bounce", ui::Icon::Download},
      {"freeze", ui::Icon::Snowflake}, {"export", ui::Icon::Download},   {"split", ui::Icon::ToolSlice},     {"slice", ui::Icon::ToolSlice},
      {"join", ui::Icon::CirclesMerged}, {"merge", ui::Icon::CirclesMerged}, {"consolidat", ui::Icon::CirclesMerged}, {"mute", ui::Icon::ToolMute},
      {"delete", ui::Icon::Trash},     {"clone", ui::Icon::Clone},       {"duplicat", ui::Icon::Clone},      {"marker", ui::Icon::Flag},
      {"tempo", ui::Icon::TapTempo},   {"setting", ui::Icon::Settings},  {"help", ui::Icon::Help},           {"typing", ui::Icon::Keyboard},
      {"keyboard", ui::Icon::Keyboard}, {"automation", ui::Icon::ClipAutomation}, {"stretch", ui::Icon::Stretch}, {"warp", ui::Icon::Stretch},
      {"new", ui::Icon::NewFile},      {"pattern", ui::Icon::ClipPattern}, {"stage", ui::Icon::Stage},       {"loupe", ui::Icon::Wave},
  };
  for (const Word& w : kWords)
    if (id.find(w.word) != std::string::npos) return w.icon;
  return ui::Icon::Star;
}

bool runPin(App& a, const tb::Pin& p) {
  if (cmdpalette::runFresh(a, p.command)) return true;
  a.notify(i18n::trMessage("That pinned command cannot run right now"), true);
  return false;
}

void pinMenu(App& a, const tb::Pin& pin) {
  const Preset lp = layoutui::active();
  std::vector<tb::Pin> pins = modelFor(a, lp).pins;
  int at = -1;
  for (size_t i = 0; i < pins.size(); ++i)
    if (pins[i].command == pin.command) at = static_cast<int>(i);
  if (at < 0) return;
  tb::Pin& p = pins[static_cast<size_t>(at)];
  bool edit = false;
  if (ImGui::MenuItem(i18n::tr("Show its name"), nullptr, !p.icon)) p.icon = false, edit = true;
  if (ImGui::MenuItem(i18n::tr("Show its icon only"), nullptr, p.icon)) p.icon = true, edit = true;
  ImGui::Separator();
  for (tb::Where w : {tb::Where::Both, tb::Where::Bar, tb::Where::Palette})
    if (ImGui::MenuItem(i18n::tr(tb::whereName(w)), nullptr, p.where == w)) p.where = w, edit = true;
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Move left"), nullptr, false, at > 0)) edit = tb::movePin(pins, at, at - 1);
  if (ImGui::MenuItem(i18n::tr("Move right"), nullptr, false, at + 1 < static_cast<int>(pins.size()))) edit = tb::movePin(pins, at, at + 1);
  if (ImGui::MenuItem(i18n::tr("Customise the bar..."))) openBarList(a);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Unpin"))) edit = tb::unpin(pins, pin.command);
  if (edit) setPins(a, lp, pins);
}

// ------------------------------------------------------------ dragging parts ----

void PartDrag::heading(const std::string& section, const char* label, ImVec2 r0, ImVec2 r1) {
  rows_.push_back(Row{section, "", label ? label : "", false, r0, r1, true});
}

void PartDrag::row(const std::string& section, const std::string& id, const char* label, bool movable, ImVec2 r0, ImVec2 r1) {
  rows_.push_back(Row{section, id, label ? label : "", movable, r0, r1, false});
  if (id_.empty() && movable && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
    id_ = id;
    section_ = section;
  }
}

bool PartDrag::finish(Preset p, tb::Bar& bar, ImDrawList* dl, float x0, float x1) {
  last_.swap(rows_);
  rows_.clear();
  devLine_.clear();
  const int frame = ImGui::GetFrameCount(), was = frame_;
  frame_ = frame;
  if (!id_.empty() && was != frame - 1) id_.clear();
  target_.clear();
  if (id_.empty()) return false;
  // The groups it may land in, top to bottom: each with its heading (when listed) and movable rows.
  struct Group {
    std::string id, label;
    const Row* head = nullptr;
    std::vector<const Row*> rows;
  };
  std::vector<Group> groups;
  const Row* self = nullptr;
  for (const Row& r : last_) {
    if (!r.heading && r.id == id_) self = &r;
    if (!tb::canMoveInto(p, id_, r.section)) continue;
    if (groups.empty() || groups.back().id != r.section) groups.push_back(Group{r.section, "", nullptr, {}});
    Group& g = groups.back();
    if (r.heading) g.head = &r, g.label = r.label;
    else if (r.movable) g.rows.push_back(&r);
  }
  if (!self || ImGui::IsKeyPressed(ImGuiKey_Escape)) {  // not listed any more, or given up
    id_.clear();
    return false;
  }
  const ImVec2 mp = ImGui::GetIO().MousePos;
  // The group under the pointer: the last one whose heading's middle (or first row's top) is above it.
  const Group* tg = nullptr;
  for (const Group& g : groups) {
    if (!g.head && g.rows.empty()) continue;
    const float top = g.head ? (g.head->r0.y + g.head->r1.y) * 0.5f : g.rows.front()->r0.y;
    if (!tg || mp.y >= top) tg = &g;
  }
  if (!tg) {
    id_.clear();
    return false;
  }
  target_ = tg->id;
  // The gap nearest the pointer in it: before the first row whose middle is below it (or after the last);
  // in a group with no parts, under its heading.
  const std::vector<const Row*>& group = tg->rows;
  size_t k = 0;
  while (k < group.size() && (group[k]->r0.y + group[k]->r1.y) * 0.5f < mp.y) ++k;
  const float ly = std::round(group.empty() ? tg->head->r1.y - 4 : k < group.size() ? group[k]->r0.y : group.back()->r1.y);
  const std::string anchor = group.empty() ? std::string() : k == 0 ? group[0]->id : group[k - 1]->id;
  const bool after = k > 0;
  const bool across = tg->id != section_;
  // The part lifted: its place dimmed, a ghost of it under the pointer, and the line where it lands.
  const auto& P = theme::pal();
  const float rh = self->r1.y - self->r0.y;
  dl->AddRectFilled(ImVec2(x0, self->r0.y), ImVec2(x1, self->r1.y), theme::withAlpha(P.surface900, 0.55f), 4.0f);
  dl->AddRect(ImVec2(x0, self->r0.y + 0.5f), ImVec2(x1, self->r1.y - 0.5f), theme::withAlpha(P.textFaint, 0.45f), 4.0f);
  if (across) {  // another side: its whole group lit faintly, so it is plain which side the part goes to
    const float g0 = tg->head ? tg->head->r0.y + 4 : group.front()->r0.y;  // (clear of the row above the heading)
    const float g1 = std::max(group.empty() ? tg->head->r1.y : group.back()->r1.y, ly + 3);
    dl->AddRectFilled(ImVec2(x0 - 2, g0 - 1), ImVec2(x1 + 2, g1 + 1), theme::withAlpha(P.hot, 0.07f), 5.0f);
    dl->AddRect(ImVec2(x0 - 2, g0 - 1), ImVec2(x1 + 2, g1 + 1), theme::withAlpha(P.hot, 0.35f), 5.0f);
  }
  dl->AddLine(ImVec2(x0 + 4, ly), ImVec2(x1, ly), P.hot, 2.0f);
  dl->AddCircleFilled(ImVec2(x0 + 4, ly), 3.5f, P.hot, 12);
  {  // a chip with its name follows the pointer, beside it, so the line stays in sight (and, going to
     // another side, that side's name)
    const float fs = ImGui::GetFontSize();
    const float tw = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, self->label.c_str()).x;
    const std::string to = across && !tg->label.empty() ? std::string("\xE2\x86\x92 ") + tg->label : std::string();
    const float sw = to.empty() ? 0.0f : ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, to.c_str()).x + 10;
    const float ch = std::round(rh - 4), cw = std::min(tw + 34 + sw, x1 - x0);
    const float cx = std::round(std::clamp(mp.x + 16, x0, x1 - cw));
    const float cy = std::round(mp.y - ch * 0.5f);
    ImDrawList* fg = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    const ImVec2 g0(cx, cy), g1(cx + cw, cy + ch);
    fg->AddRectFilled(ImVec2(g0.x + 1, g0.y + 3), ImVec2(g1.x + 1, g1.y + 3), theme::withAlpha(0xFF000000, 0.22f), 6.0f);  // its shadow
    fg->AddRectFilled(g0, g1, P.surface700, 6.0f);
    fg->AddRect(g0, g1, theme::withAlpha(P.hot, 0.7f), 6.0f, 0, 1.0f);
    ui::drawIcon(fg, ui::Icon::GripDots, ImVec2(g0.x + 11, cy + ch * 0.5f), 12, P.textDim);
    theme::textEllipsis(fg, ImGui::GetFont(), fs, ImVec2(g0.x + 22, std::round(cy + (ch - fs) * 0.5f)), cw - 28 - sw, P.text, self->label.c_str());
    if (!to.empty())
      fg->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(g1.x - sw + 2, std::round(cy + (ch - theme::kFontCaption) * 0.5f)), P.hot, to.c_str());
  }
  ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  // A long list scrolls while the pointer is near its top or bottom edge.
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  if (ImGui::GetScrollMaxY() > 0) {
    if (mp.y > wp.y + ws.y - 24) ImGui::SetScrollY(ImGui::GetScrollY() + 8);
    else if (mp.y < wp.y + 24) ImGui::SetScrollY(ImGui::GetScrollY() - 8);
  }
  char line[160];
  std::snprintf(line, sizeof line, "BARLIST drag %s %s %s %d %s\n", id_.c_str(), after ? "after" : "before", anchor.empty() ? "-" : anchor.c_str(),
                static_cast<int>(ly), target_.c_str());
  devLine_ = line;
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return false;
  const std::string id = id_, to = target_;
  id_.clear();
  return tb::moveInto(p, bar, id, to, anchor, after);
}

std::string PartDrag::devRows() const {
  std::string out;
  char line[200];
  for (const Row& r : last_) {
    std::snprintf(line, sizeof line, r.heading ? "BARLIST head %s%s %d %d %d %d\n" : "BARLIST row %s %s %d %d %d %d\n", r.section.c_str(), r.id.c_str(), static_cast<int>(r.r0.x), static_cast<int>(r.r0.y),
                  static_cast<int>(r.r1.x), static_cast<int>(r.r1.y));
    out += line;
  }
  return out + devLine_;
}

// ------------------------------------------------------------ bar list ----

namespace {

bool gOpenReq = false;
bool gOpen = false;
ImVec2 gAt(0, 0);
char gQuery[96] = "";
std::string gDevQuery;
std::string gQueryFor = "\x01";
struct Found {
  std::string id, label, labelEn, path;
};
std::vector<Found> gFound;
PartDrag gDrag;  // a part dragged in the list

// A small on / off switch (the bar list's rows), lit in the hot colour while on.
bool switchAt(const char* id, ImVec2 p, bool on) {
  const auto& P = theme::pal();
  const ImVec2 size(26, 14);
  ImGui::SetCursorScreenPos(p);
  const bool click = ImGui::InvisibleButton(id, size);
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 track = on ? (hov ? theme::mix(P.hot, P.text, 0.12f) : P.hot) : (hov ? P.surface600 : P.surface700);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), track, size.y * 0.5f);
  const float r = size.y * 0.5f - 2;
  const float cx = on ? p.x + size.x - r - 2 : p.x + r + 2;
  dl->AddCircleFilled(ImVec2(cx, p.y + size.y * 0.5f), r, on ? theme::contrastText(P.hot) : P.textDim, 16);
  return click;
}

bool matches(const std::string& text, const std::string& q) {
  if (q.empty()) return true;
  return cmdpal::wordScore(q, text) >= 0;
}

void refreshFound(App& a) {
  if (gQueryFor == gQuery) return;
  gQueryFor = gQuery;
  gFound.clear();
  if (!gQuery[0]) return;
  std::vector<cmdpal::Entry> rows;
  for (const cmdpalette::CommandInfo& c : cmdpalette::commandList(a)) {
    cmdpal::Entry e;
    e.id = c.id;
    e.kind = c.kind;
    e.label = c.label;
    e.path = c.path;
    e.shortcut = c.shortcut;
    e.order = static_cast<int>(rows.size());
    if (cmdpal::pinnableRow(e)) rows.push_back(e);
  }
  for (const cmdpal::Ranked& r : cmdpal::rank(rows, gQuery, {})) {
    const cmdpal::Entry& e = rows[static_cast<size_t>(r.index)];
    gFound.push_back({e.id, e.label, e.label, e.path});
    if (gFound.size() >= 6) break;
  }
}

}  // namespace

void openBarList(App& a) {
  (void)a;
  gOpenReq = true;
  gAt = ImGui::GetIO().MousePos;
}

bool barListOpen() { return gOpen; }

void drawBarList(App& a) {
  if (gOpenReq) {
    gOpenReq = false;
    gQuery[0] = 0;
    gQueryFor = "\x01";
    ImGui::OpenPopup("##barlist");
  }
  const auto& P = theme::pal();
  const Preset lp = layoutui::active();
  const float W = 320;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(std::clamp(gAt.x - W * 0.5f, vp->WorkPos.x + 8, vp->WorkPos.x + vp->WorkSize.x - W - 8), gAt.y + 6), ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(W, 0));
  ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0), ImVec2(W, vp->WorkSize.y * 0.8f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS3));
  gOpen = ImGui::BeginPopup("##barlist");
  ImGui::PopStyleVar();
  if (!gOpen) {
    gDrag.cancel();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float x0 = ImGui::GetCursorScreenPos().x, x1 = x0 + W - 2 * theme::kS3;
  tb::Model m = modelFor(a, lp);
  bool barEdit = false, pinEdit = false;
  // ---- title: Control bar, the layout, Reset ----
  {
    const ImVec2 t = ImGui::GetCursorScreenPos();
    dl->AddText(a.fontBold, fs, t, P.text, i18n::tr("Control bar"));
    const float tw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, i18n::tr("Control bar")).x;
    std::string scope = i18n::tr(layout::displayName(lp));
    if (barIsOwn(a, lp)) scope += std::string(" \xC2\xB7 ") + i18n::tr("this workspace");
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(t.x + tw + 8, t.y + 1), P.textFaint, scope.c_str());
    const char* reset = i18n::tr("Reset");
    const float rw = ImGui::CalcTextSize(reset).x + 16;
    ImGui::SetCursorScreenPos(ImVec2(x1 - rw, t.y - 3));
    if (ui::TextButton(reset, false, rw, "Put this bar back as it was designed (the pinned commands stay)")) m.bar = tb::defaults(lp), barEdit = true;
    ImGui::SetCursorScreenPos(ImVec2(t.x, t.y + fs + theme::kS3));
  }
  // ---- find a part or a command ----
  {
    ImGui::SetNextItemWidth(x1 - x0);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (!gDevQuery.empty()) {  // dev: typed into the field (the field let go first, so it reads the text again)
      ImGui::ClearActiveID();
      std::snprintf(gQuery, sizeof gQuery, "%s", gDevQuery.c_str());
      gDevQuery.clear();
    }
    ImGui::InputTextWithHint("##barfind", i18n::tr("Find a part or a command"), gQuery, sizeof gQuery);
    ImGui::Dummy(ImVec2(1, 2));
    bool anyMovable = false;
    for (const tb::PartInfo& i : tb::parts(lp)) anyMovable = anyMovable || (i.movable && std::string(i.id) != tb::kPins);
    if (anyMovable) {  // how to move one (the grips alone did not say it)
      const ImVec2 h = ImGui::GetCursorScreenPos();
      bool sides = false;  // a bar with free sides: parts move from one to another too
      for (const tb::SectionInfo& s : tb::sections(lp)) sides = sides || s.open;
      dl->AddText(a.fontRegular, theme::kFontCaption, h, P.textFaint,
                  sides ? i18n::tr("Drag a part up or down to move it, also to another side") : i18n::tr("Drag a part up or down to move it in its group"));
      ImGui::Dummy(ImVec2(x1 - x0, theme::kFontCaption + 2));
    }
  }
  const std::string q = gQuery;
  auto heading = [&](const char* text, bool lit = false) {
    ImGui::Dummy(ImVec2(1, 2));
    const ImVec2 h = ImGui::GetCursorScreenPos();
    std::string t = i18n::tr(text);
    for (char& ch : t)
      if (static_cast<unsigned char>(ch) < 128) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    dl->AddText(a.fontBold, theme::kFontLabel, h, lit ? P.hot : P.textFaint, t.c_str());
    ImGui::Dummy(ImVec2(x1 - x0, theme::kFontLabel + 6));
  };
  const float rowH = 26;
  // ---- the parts, section by section ----
  for (const tb::SectionInfo& sec : tb::sections(lp)) {
    std::vector<std::string> ids;
    for (const tb::Part* x : tb::sectionParts(lp, m.bar, sec.id))
      if (x->id != tb::kPins) ids.push_back(x->id);
    std::vector<std::string> show;
    for (const std::string& id : ids)
      if (matches(i18n::tr(tb::part(lp, id)->label), q)) show.push_back(id);
    // A free side with nothing left on it still shows (a part can be dragged back onto it).
    if (show.empty() && !(q.empty() && sec.open)) continue;
    const ImVec2 h0 = ImGui::GetCursorScreenPos();
    heading(sec.label, !gDrag.dragging().empty() && gDrag.target() == sec.id && tb::sectionOf(lp, m.bar, gDrag.dragging()) != sec.id);
    if (show.empty()) {
      const ImVec2 e0 = ImGui::GetCursorScreenPos();
      dl->AddRect(ImVec2(e0.x + 0.5f, e0.y + 0.5f), ImVec2(x1 - 0.5f, e0.y + rowH - 0.5f), theme::withAlpha(P.textFaint, 0.35f), 4.0f);
      theme::centreText(dl, a.fontRegular, theme::kFontCaption, e0, ImVec2(x1, e0.y + rowH), P.textFaint, i18n::tr("Nothing here: drag a part onto this side"));
      ImGui::Dummy(ImVec2(x1 - x0, rowH));
    }
    gDrag.heading(sec.id, i18n::tr(sec.label), ImVec2(x0, h0.y), ImVec2(x1, ImGui::GetCursorScreenPos().y));
    for (const std::string& id : show) {
      const tb::PartInfo* info = tb::part(lp, id);
      const tb::Part* x = tb::find(m.bar, id);
      ImGui::PushID(id.c_str());
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      const ImVec2 r1(x1, r0.y + rowH);
      const bool hov = ImGui::IsMouseHoveringRect(r0, r1);
      if (hov) dl->AddRectFilled(r0, r1, theme::withAlpha(P.surface600, 0.45f), 4.0f);
      // The grip and the name: a drag on either moves the part inside its group (the line shows where).
      ImGui::SetCursorScreenPos(r0);
      ImGui::InvisibleButton("##row", ImVec2(x1 - r0.x - 34, rowH));
      if (info->movable && ImGui::IsItemHovered() && gDrag.dragging().empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      gDrag.row(sec.id, id, i18n::tr(info->label), info->movable, r0, r1);
      if (info->movable) ui::drawIcon(dl, ui::Icon::GripDots, ImVec2(r0.x + 9, r0.y + rowH * 0.5f), 12, hov && gDrag.dragging().empty() ? P.text : P.textFaint);
      if (ImGui::BeginPopupContextItem("##partmenu")) {
        bool alw = x && x->always;
        if (ImGui::MenuItem(i18n::tr("Always shown"), nullptr, &alw)) barEdit = tb::setAlways(m.bar, id, alw) || barEdit;
        if (info->fitLevel <= 0) ImGui::TextDisabled("%s", i18n::tr("This part never leaves a narrow window"));
        ImGui::EndPopup();
      }
      const float ty = std::round(r0.y + (rowH - fs) * 0.5f);
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(r0.x + 22, ty), x1 - r0.x - 22 - 80, x && x->shown ? P.text : P.textDim, i18n::tr(info->label));
      if (x && x->always) dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(x1 - 30 - 46, ty + 1), P.textFaint, i18n::tr("always"));
      if (switchAt("##on", ImVec2(x1 - 28, r0.y + (rowH - 14) * 0.5f), x && x->shown)) barEdit = tb::setShown(m.bar, id, !(x && x->shown)) || barEdit;
      ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y));
      ImGui::PopID();
    }
  }
  barEdit = gDrag.finish(lp, m.bar, dl, x0, x1) || barEdit;
  // ---- commands found (pin them from here, as from the palette) ----
  if (!q.empty()) {
    refreshFound(a);
    if (!gFound.empty()) {
      heading("Commands");
      for (const Found& f : gFound) {
        ImGui::PushID(f.id.c_str());
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const ImVec2 r1(x1, r0.y + rowH);
        const bool on = tb::pinned(m.pins, f.id);
        ImGui::InvisibleButton("##cmd", ImVec2(x1 - r0.x, rowH));
        const bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(r0, r1, theme::withAlpha(P.surface600, 0.45f), 4.0f);
        if (ImGui::IsItemClicked()) {
          if (on) pinEdit = tb::unpin(m.pins, f.id) || pinEdit;
          else pinEdit = tb::pin(m.pins, tb::Pin{f.id, f.labelEn}) || pinEdit;
        }
        const float ty = std::round(r0.y + (rowH - fs) * 0.5f);
        const float lw = std::min(ImGui::CalcTextSize(f.label.c_str()).x, (x1 - r0.x) * 0.55f);
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(r0.x + 22, ty), lw + 1, P.text, f.label.c_str());
        theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 30 + lw, ty + 1), x1 - r0.x - 60 - lw, P.textFaint, f.path.c_str());
        ui::drawIcon(dl, on ? ui::Icon::StarFilled : ui::Icon::Star, ImVec2(r0.x + 9, r0.y + rowH * 0.5f), 12, on ? P.hot : P.textDim);
        if (hov) ImGui::SetTooltip("%s", on ? i18n::tr("Unpin it") : i18n::tr("Pin it to the bar"));
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y));
        ImGui::PopID();
      }
    }
  }
  // ---- pinned commands ----
  if (tb::part(lp, tb::kPins)) {
    heading("Pinned commands", true);
    const tb::Part* pp = tb::find(m.bar, tb::kPins);
    {
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      const float ty = std::round(r0.y + (rowH - fs) * 0.5f);
      dl->AddText(a.fontRegular, fs, ImVec2(r0.x + 22, ty), P.textDim, i18n::tr("Show them on the bar"));
      if (switchAt("##pinson", ImVec2(x1 - 28, r0.y + (rowH - 14) * 0.5f), pp && pp->shown)) barEdit = tb::setShown(m.bar, tb::kPins, !(pp && pp->shown)) || barEdit;
      ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH));
    }
    for (int i = 0; i < static_cast<int>(m.pins.size()); ++i) {
      tb::Pin& p = m.pins[static_cast<size_t>(i)];
      ImGui::PushID(i);
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      const ImVec2 r1(x1, r0.y + rowH);
      const bool hov = ImGui::IsMouseHoveringRect(r0, r1);
      if (hov) dl->AddRectFilled(r0, r1, theme::withAlpha(P.surface600, 0.45f), 4.0f);
      ImGui::SetCursorScreenPos(r0);
      ImGui::InvisibleButton("##pgrip", ImVec2(x1 - r0.x - 96, rowH));
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        ImGui::SetDragDropPayload("GL_BARPIN", &i, sizeof i);
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_BARPIN")) pinEdit = tb::movePin(m.pins, *static_cast<const int*>(pl->Data), i) || pinEdit;
        ImGui::EndDragDropTarget();
      }
      ui::drawIcon(dl, ui::Icon::GripDots, ImVec2(r0.x + 9, r0.y + rowH * 0.5f), 12, hov ? P.textDim : P.textFaint);
      ui::drawIcon(dl, pinIcon(p), ImVec2(r0.x + 30, r0.y + rowH * 0.5f), 13, P.hot);
      const float ty = std::round(r0.y + (rowH - fs) * 0.5f);
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(r0.x + 42, ty), x1 - r0.x - 42 - 100, P.text, pinLabel(p).c_str());
      // Label / Icon, then where it shows (a click cycles), then remove.
      const char* look = p.icon ? i18n::tr("Icon") : i18n::tr("Label");
      const float lookW = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, look).x;
      ImGui::SetCursorScreenPos(ImVec2(x1 - 22 - 8 - lookW - 8, r0.y));
      if (ImGui::InvisibleButton("##look", ImVec2(lookW + 8, rowH))) p.icon = !p.icon, pinEdit = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr("Show it on the bar by its name or by its icon"));
      dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(x1 - 22 - 8 - lookW - 4, ty + 1), ImGui::IsItemHovered() ? P.text : P.textFaint, look);
      if (p.where != tb::Where::Both) {
        const char* w = p.where == tb::Where::Bar ? i18n::tr("bar") : i18n::tr("palette");
        const float ww = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, w).x;
        dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(x1 - 22 - 8 - lookW - 14 - ww, ty + 1), P.textFaint, w);
      }
      ImGui::SetCursorScreenPos(ImVec2(x1 - 20, r0.y + (rowH - 20) * 0.5f));
      if (ui::IconButton("##unpin", ui::Icon::Close, false, 20, "Unpin")) {
        pinEdit = tb::unpin(m.pins, p.command) || pinEdit;
        ImGui::PopID();
        break;
      }
      ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y));
      ImGui::PopID();
    }
    // Pin more: the palette's keys.
    {
      ImGui::Dummy(ImVec2(1, 4));
      const ImVec2 f = ImGui::GetCursorScreenPos();
      const char* pal = keys::label("view.commandPalette");
      char line[160];
      std::snprintf(line, sizeof line, i18n::tr("Pin more: %s, a command, Ctrl+Enter"), pal ? pal : keys::cmdKey("view.commandPalette", "Ctrl+K").c_str());
      dl->AddText(ImGui::GetFont(), theme::kFontCaption, f, P.textFaint, line);
      ImGui::Dummy(ImVec2(x1 - x0, theme::kFontCaption + 4));
    }
  }
  // ---- More: the layout's settings page ----
  {
    ImGui::Dummy(ImVec2(1, 2));
    ImGui::Separator();
    if (ImGui::Selectable(i18n::tr("More layout settings..."))) {
      a.showSettings = true;
      a.settingsTab = kLayoutsSettingsTab;
      a.focusWindow = "Settings";
      ImGui::CloseCurrentPopup();
    }
  }
  if (barEdit) setBar(a, lp, m.bar);
  if (pinEdit) setPins(a, lp, m.pins);
  ImGui::EndPopup();
}

// ------------------------------------------------------------------ dev ----

namespace {
struct DrawnPart {
  std::string id, side;
  float x0, x1;
};
std::vector<DrawnPart> gDrawn;
int gDrawnFrame = -1;
float gEdgeL = 0, gEdgeR = 0;
void drawnFrame() {
  const int f = ImGui::GetFrameCount();
  if (f != gDrawnFrame) gDrawn.clear(), gDrawnFrame = f;
}
}  // namespace

void notePart(const std::string& id, const std::string& side, float x0, float x1) {
  drawnFrame();
  gDrawn.push_back(DrawnPart{id, side, x0, x1});
}

void noteBarEdges(float left, float right) {
  drawnFrame();
  gEdgeL = left, gEdgeR = right;
}

void devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return;
  const std::string& c = args[0];
  const Preset p = layoutui::active();
  std::string arg = args.size() > 1 ? args[1] : std::string();
  for (size_t i = 2; i < args.size(); ++i) arg += " " + args[i];
  if (c == "dump") {
    const tb::Model& m = model(a);
    std::printf("TOOLBAR %s own %d/%d parts", layout::id(p), barIsOwn(a, p) ? 1 : 0, pinsAreOwn(a, p) ? 1 : 0);
    for (const tb::Part& x : m.bar.parts)
      std::printf(" %s%s%s%s%s", x.id.c_str(), x.section.empty() ? "" : "@", x.section.c_str(), x.shown ? "" : "-", x.always ? "!" : "");
    std::printf(" pins");
    for (const tb::Pin& x : m.pins) std::printf(" [%s|%s|%d|%d]", x.command.c_str(), x.label.c_str(), x.icon ? 1 : 0, static_cast<int>(x.where));
    std::printf("\n");
    std::fflush(stdout);
  } else if ((c == "hide" || c == "show") && args.size() > 1) {
    tb::Bar b = modelFor(a, p).bar;
    if (tb::setShown(b, args[1], c == "show")) setBar(a, p, b);
  } else if (c == "always" && args.size() > 1) {
    tb::Bar b = modelFor(a, p).bar;
    const tb::Part* x = tb::find(b, args[1]);
    if (x && tb::setAlways(b, args[1], !x->always)) setBar(a, p, b);
  } else if (c == "move" && args.size() > 2) {
    tb::Bar b = modelFor(a, p).bar;
    if (tb::step(p, b, args[1], std::atoi(args[2].c_str()))) setBar(a, p, b);
  } else if (c == "pin" && args.size() > 1) {
    std::string label;
    for (size_t i = 2; i < args.size(); ++i) label += (label.empty() ? "" : " ") + args[i];
    std::vector<tb::Pin> pins = modelFor(a, p).pins;
    const bool ok = tb::pin(pins, tb::Pin{args[1], label});
    if (ok) setPins(a, p, pins);
    std::printf("TOOLBAR pin %s %s\n", args[1].c_str(), ok ? "ok" : "refused");
    std::fflush(stdout);
  } else if (c == "unpin" && args.size() > 1) {
    std::vector<tb::Pin> pins = modelFor(a, p).pins;
    if (tb::unpin(pins, args[1])) setPins(a, p, pins);
  } else if (c == "icon" && args.size() > 1) {
    std::vector<tb::Pin> pins = modelFor(a, p).pins;
    for (tb::Pin& x : pins)
      if (x.command == args[1]) x.icon = !x.icon;
    setPins(a, p, pins);
  } else if (c == "run" && args.size() > 1) {
    const std::vector<tb::Pin> pins = barPins(a);
    const int i = std::atoi(args[1].c_str());
    const bool ok = i >= 0 && i < static_cast<int>(pins.size()) && runPin(a, pins[static_cast<size_t>(i)]);
    std::printf("TOOLBAR run %d %s\n", i, ok ? "ok" : "refused");
    std::fflush(stdout);
  } else if (c == "reset") {
    resetBar(a, p);
  } else if (c == "parts") {  // where the bar drew its parts last frame (and the edges its sides are laid from)
    std::printf("BARPARTS %s frame %d edges %.1f %.1f n %d\n", layout::id(p), gDrawnFrame, gEdgeL, gEdgeR, static_cast<int>(gDrawn.size()));
    for (const DrawnPart& d : gDrawn) std::printf("BARPART %s %s %.1f %.1f\n", d.side.c_str(), d.id.c_str(), d.x0, d.x1);
    std::fflush(stdout);
  } else if (c == "into" && args.size() > 2) {  // into <part> <section> [<anchor> [after]]: a drop, as a drag makes it
    tb::Bar b = modelFor(a, p).bar;
    if (tb::moveInto(p, b, args[1], args[2], args.size() > 3 ? args[3] : std::string(), args.size() > 4 && args[4] == "after")) setBar(a, p, b);
  } else if (c == "list") {
    openBarList(a);
  } else if (c == "rows") {  // the bar list's rows as drawn last frame (and the drag going on)
    std::printf("BARLIST open %d\n%s", gOpen ? 1 : 0, gDrag.devRows().c_str());
    std::fflush(stdout);
  } else if (c == "find") {
    gDevQuery = arg;  // (put in the field next frame, while the list is open)
  }
}

}  // namespace gl::toolbarui
