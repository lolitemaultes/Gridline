// Gridline — K-151: swing presets in the interface (see GrooveUi.h).
#include "GrooveUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "../core/Presets.h"
#include "../engine/WarpOps.h"
#include "../host/net/Http.h"
#include "App.h"
#include "ContextMenus.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::grooveui {

namespace fs = std::filesystem;
namespace gv = gl::groove;

namespace {

struct Cache {
  std::vector<gv::Preset> list;
  double stamp = -100;
  std::string root;
};
Cache& cache() {
  static Cache c;
  return c;
}

std::string libraryRoot(App& a) { return presetRoot(a.configDir()); }

const char* revealText() {
#if defined(__APPLE__)
  return i18n::N_("Show in Finder");
#elif defined(_WIN32)
  return i18n::N_("Show in Explorer");
#else
  return i18n::N_("Show in Files");
#endif
}

std::string fullName(const gv::Preset& p) { return p.folder.empty() ? p.name : p.folder + "/" + p.name; }

// Where things were drawn, for scripted checks ("groove" dev command): name -> rect and frame.
struct DevRect {
  ImVec2 a, b;
  int frame = -1;
};
std::map<std::string, DevRect>& devRects() {
  static std::map<std::string, DevRect> m;
  return m;
}
bool gInEditor = false;  // the chip being drawn is the Groove editor's
void devRec(const std::string& name) { devRects()[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetFrameCount()}; }

// The preset's row: a tick when the song uses it, its feel as the hint.
void presetRow(App& a, const gv::Preset& p, const std::string& label) {
  const Song& s = a.doc.song();
  const bool on = s.groove.preset == fullName(p) && gv::matches(s, p);
  if (ImGui::MenuItem(label.c_str(), nullptr, on)) applyLater(a, p);
  devRec("item " + fullName(p));
  ui::Hint(gv::feelText(p.shape, p.amount));
}

// User presets grouped by folder: top level first, folders as sub-menus.
void userRows(App& a, const std::vector<const gv::Preset*>& user) {
  std::map<std::string, std::vector<const gv::Preset*>> byFolder;
  for (const gv::Preset* p : user) {
    if (p->folder.empty()) presetRow(a, *p, p->name + "##u" + p->file);
    else byFolder[p->folder].push_back(p);
  }
  for (const auto& [folder, list] : byFolder) {
    if (!ImGui::BeginMenu(folder.c_str())) continue;
    for (const gv::Preset* p : list) presetRow(a, *p, p->name + "##u" + p->file);
    ImGui::EndMenu();
  }
}

// ---- editor state (UI only) ----
struct EditorState {
  int dragLane = -1;  // 0 timing, 1 velocity
  bool fresh = true;
};
EditorState E;

}  // namespace

const std::vector<gv::Preset>& presets(App& a) {
  Cache& c = cache();
  const std::string root = libraryRoot(a);
  if (a.now - c.stamp > 3.0 || c.root != root) {
    c.list = gv::allPresets(root);
    c.stamp = a.now;
    c.root = root;
  }
  return c.list;
}

void refresh() { cache().stamp = -100; }

void apply(App& a, const gv::Preset& p) {
  a.doc.editSong(std::string("swing preset: ") + p.name, [&p](Song& s) { gv::apply(s, p); });
  a.setHint(std::string(i18n::tr("Swing preset")) + ": " + p.name + " (" + gv::feelText(p.shape, p.amount) + ")");
}

void applyLater(App& a, const gv::Preset& p) {
  a.later([&a, p] { apply(a, p); });
}

void toggleSwingNotesLater(App& a) {
  a.later([&a] {
    const bool notes = a.doc.song().swingNotes;
    a.doc.editSong(notes ? "swing steps only" : "swing notes too", [](Song& x) { x.swingNotes = !x.swingNotes; });
  });
}

void presetMenuItems(App& a) {
  const auto& list = presets(a);
  std::vector<const gv::Preset*> user;
  ImGui::TextDisabled("%s", i18n::tr("Factory"));
  for (const gv::Preset& p : list) {
    if (p.factory) presetRow(a, p, p.name + "##f");
    else user.push_back(&p);
  }
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("My presets"));
  if (user.empty()) ImGui::TextDisabled("  %s", i18n::tr("None saved yet"));
  else userRows(a, user);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Save current as preset..."))) savePrompt(a);
  if (ImGui::MenuItem(i18n::tr("Groove editor..."), nullptr, a.showGroove)) a.showGroove = true;
  devRec("menu editor");
  if (ImGui::MenuItem(i18n::tr("Import preset..."))) importDialog(a);
  ui::Hint("A swing preset (.json), or the groove of a MIDI file or a score file");
  if (ImGui::MenuItem(i18n::tr("Export current..."))) exportDialog(a);
  if (ImGui::MenuItem(i18n::tr(revealText()))) {
    std::error_code ec;
    fs::create_directories(fs::u8path(gv::grooveFolder(libraryRoot(a))), ec);
    http::reveal(gv::grooveFolder(libraryRoot(a)));
  }
  ImGui::Separator();
  const bool notes = a.doc.song().swingNotes;
  if (ImGui::MenuItem(i18n::tr("Swing piano roll notes"), nullptr, notes)) toggleSwingNotesLater(a);
  devRec("menu notes");
}

void chip(App& a, float width) {
  const Song& s = a.doc.song();
  const std::string label = gv::chipLabel(s, presets(a));
  const float fh = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();  // (once open, the current window is the popup)
  // The whole menu without scrolling: every preset and the actions under them.
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(width, 230.0f), 0),
                                      ImVec2(FLT_MAX, std::max(200.0f, ImGui::GetMainViewport()->WorkSize.y * 0.85f)));
  const bool open = ui::BeginCombo("##swingpreset", ("      " + label).c_str(), width);
  devRec(gInEditor ? "editorchip" : "chip");
  // The groove glyph inside the chip, left of the name.
  ui::drawIcon(dl, ui::Icon::Groove, ImVec2(p.x + theme::kS2 + 6, p.y + fh * 0.5f), 12,
               theme::pal().textDim);
  if (!open) {
    ui::Hint("Swing preset: " + gv::feelText(s.groove.shape, s.swing) + " (click for presets, save and the groove editor)");
    if (ImGui::BeginDragDropTarget()) {  // a swing preset dragged from the browser
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SWING")) {
        const std::string name(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (const gv::Preset* p = gv::findIn(presets(a), name)) applyLater(a, *p);
      }
      ImGui::EndDragDropTarget();
    }
  }
  if (open) {
    presetMenuItems(a);
    ImGui::EndCombo();
  }
}

void savePrompt(App& a) {
  const Song& s = a.doc.song();
  std::string name = s.groove.preset;
  if (const size_t k = name.rfind('/'); k != std::string::npos) name = name.substr(k + 1);
  if (name.empty() || gv::findFactory(name)) name = name.empty() ? "My groove" : name + " (mine)";
  menus::openPrompt(i18n::tr("Save the swing as a preset (folders with /)"), name, [&a](const std::string& typed) {
    if (typed.empty()) return;
    const gv::Preset p = gv::fromSong(a.doc.song(), typed);
    std::string path, err;
    if (gv::saveUserPreset(libraryRoot(a), p, typed, &path, &err)) {
      refresh();
      std::string stored = typed;
      for (char& c : stored)
        if (c == '\\') c = '/';
      a.doc.editSong("name the swing preset", [&stored](Song& x) { x.groove.preset = stored; });
      a.notify("Saved swing preset " + fs::u8path(path).stem().u8string());
    } else {
      a.notify("Could not save the swing preset", true, err);
    }
  });
}

void importDialog(App& a) {
  menus::fileDialog(a, i18n::tr("Import a swing preset"), false, {".json", ".mid", ".midi", ".fsc"}, "", [&a](const std::string& path) {
    gv::Preset p;
    std::string err;
    if (!gv::importFile(path, p, &err)) {
      a.notify("Could not import the swing preset", true, err.empty() ? path : err);
      return;
    }
    if (p.name.empty()) p.name = fs::u8path(path).stem().u8string();
    std::string saved;
    if (gv::saveUserPreset(libraryRoot(a), p, p.name, &saved, &err)) {
      refresh();
      p.name = fs::u8path(saved).stem().u8string();
      apply(a, p);
      a.notify("Imported swing preset " + p.name);
    } else {
      a.notify("Could not save the swing preset", true, err);
    }
  });
}

void exportDialog(App& a) {
  const Song& s = a.doc.song();
  std::string name = s.groove.preset.empty() ? std::string("Groove") : s.groove.preset;
  if (const size_t k = name.rfind('/'); k != std::string::npos) name = name.substr(k + 1);
  menus::fileDialog(a, i18n::tr("Export the swing preset"), true, {".json"}, safePresetName(name) + ".json", [&a, name](const std::string& path) {
    std::string err;
    const gv::Preset p = gv::fromSong(a.doc.song(), name);
    if (gv::savePresetFile(path, p, &err)) a.notify("Exported swing preset " + fs::u8path(path).filename().u8string());
    else a.notify("Could not export the swing preset", true, err);
  });
}

bool fromPattern(App& a, ObjectId pattern, ObjectId channel) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!p) return false;
  std::vector<gv::Onset> on;
  const Tick st = s.ticksPerStep(*p);
  auto take = [&](ObjectId ch) {
    if (auto it = p->notes.find(ch); it != p->notes.end())
      for (const gv::Onset& o : gv::onsetsOfNotes(it->second)) on.push_back(o);
    if (auto it = p->steps.find(ch); it != p->steps.end() && st > 0)
      for (size_t i = 0; i < it->second.size(); ++i)
        if (it->second[i]) on.push_back({static_cast<double>(static_cast<Tick>(i) * st), static_cast<float>(it->second[i])});
  };
  if (channel != kNoId) take(channel);
  else
    for (const Channel& c : s.channels) take(c.id);
  if (on.size() < 2) {
    a.notify(i18n::tr("Not enough notes to take a groove from"), true);
    return false;
  }
  const int grid = s.groove.shape.grid == gv::kGridStep ? gv::kGrid16th : s.groove.shape.grid;
  const double cell = gv::cellTicks(s, p, grid);
  gv::Preset g = gv::extract(on, cell, static_cast<double>(s.barTicks()), grid, 16, "");
  const Channel* c = s.channel(channel);
  g.name = c ? "Groove of " + c->name : "Groove of " + p->name;
  a.doc.editSong("groove from the pattern", [&g](Song& x) {
    gv::apply(x, g);
    x.groove.preset.clear();
  });
  a.setHint(std::string(i18n::tr("Groove taken")) + ": " + gv::feelText(g.shape, g.amount));
  return true;
}

bool fromClip(App& a, ObjectId clip, const TempoMap& tm) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return false;
  const auto on = clipGrooveOnsets(s, *c, tm);
  if (on.size() < 2) {
    a.notify(i18n::tr("No transients found"), true);
    return false;
  }
  const int grid = s.groove.shape.grid == gv::kGridStep ? gv::kGrid16th : s.groove.shape.grid;
  gv::Preset g = gv::extract(on, gv::cellTicks(s, nullptr, grid), static_cast<double>(s.barTicks()), grid, 16, "");
  a.doc.editSong("groove from the audio clip", [&g](Song& x) {
    gv::apply(x, g);
    x.groove.preset.clear();
  });
  a.setHint(std::string(i18n::tr("Groove taken")) + ": " + gv::feelText(g.shape, g.amount));
  return true;
}

bool swingClip(App& a, ObjectId clip, const TempoMap& tm, double strength) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c) return false;
  Clip after = *c;
  const int n = grooveAudioClip(a.doc.song(), after, tm, strength);
  if (!(after.warp == c->warp)) a.doc.editClip(clip, "swing the audio clip", [&after](Clip& k) { k.warp = after.warp; });
  a.setHint(std::to_string(n) + " " + i18n::tr("transients moved to the swing"));
  return n > 0;
}

// ------------------------------------------------------------------ editor ----

namespace {

ParamDesc pdesc(const char* name, double lo, double hi, double def) {
  ParamDesc d;
  d.name = name;
  d.unit = "%";
  d.min = lo, d.max = hi, d.def = def;
  return d;
}

// One undo step per pointer gesture (the app joins the frames of a drag).
void editGroove(App& a, const char* label, const std::function<void(SongGroove&)>& fn) {
  a.doc.editSong(label, [&fn](Song& x) {
    fn(x.groove);
    gv::normalize(x.groove);
  });
}

// A lane of bars, one per cell: drag to draw. Timing is bipolar (-0.5..0.5 of a cell), velocity 0..2 (1 in the middle).
void lane(App& a, int which, ImVec2 size) {
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const GrooveShape& sh = s.groove.shape;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  ImGui::InvisibleButton(which == 0 ? "##timing" : "##velocity", size);
  devRec(which == 0 ? "lane timing" : "lane velocity");
  const bool hov = ImGui::IsItemHovered();
  dl->AddRectFilled(p0, p1, P.field, 3);
  const int n = sh.cycle;
  const float cw = size.x / static_cast<float>(n);
  const float mid = std::round(p0.y + size.y * 0.5f);
  dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), P.line, 1);
  for (int k = 1; k < n; ++k) {
    const float x = std::round(p0.x + cw * static_cast<float>(k));
    dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), k % 2 == 0 ? P.gridBeat : P.gridStep, 1);
  }
  const ImU32 col = which == 0 ? theme::rgb(theme::role::kGraph) : P.accent;
  const bool velOff = which == 1 && sh.velocity.empty();
  for (int k = 0; k < n; ++k) {
    float v = 0;
    if (which == 0) v = sh.timing[static_cast<size_t>(k)] * 2.0f;  // -1..1
    else v = velOff ? 0.0f : sh.velocity[static_cast<size_t>(k)] - 1.0f;  // -1..1
    const float x0 = std::round(p0.x + cw * static_cast<float>(k) + std::max(2.0f, cw * 0.18f));
    const float x1 = std::round(p0.x + cw * static_cast<float>(k + 1) - std::max(2.0f, cw * 0.18f));
    const float y = std::round(mid - v * (size.y * 0.5f - 4));
    if (std::fabs(v) < 1e-4f) dl->AddRectFilled(ImVec2(x0, mid - 1), ImVec2(x1, mid + 1), velOff ? P.textFaint : col, 1);
    else dl->AddRectFilled(ImVec2(x0, std::min(y, mid)), ImVec2(x1, std::max(y, mid)), col, 2);
  }
  if (velOff && !hov)
    ui::drawLabel(dl, ImVec2(p0.x + 8, p0.y + 4), P.textFaint, i18n::tr("No accents: draw to add them"));
  if (hov) ui::Hint(which == 0 ? "Timing per cell: up = later, down = earlier (half a cell at full swing). Drag to draw, double-click resets"
                               : "Velocity per cell: up = louder, down = quieter. Drag to draw, double-click clears the accents");
  if (ImGui::IsItemActivated()) E.dragLane = which;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    editGroove(a, which == 0 ? "reset the groove timing" : "clear the groove accents", [which](SongGroove& g) {
      if (which == 0) g.shape.timing.assign(static_cast<size_t>(g.shape.cycle), 0.0f);
      else g.shape.velocity.clear();
      g.preset.clear();
    });
    E.dragLane = -1;
    return;
  }
  if (E.dragLane == which && ImGui::IsItemActive()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int k = std::clamp(static_cast<int>((m.x - p0.x) / cw), 0, n - 1);
    float v = std::clamp((mid - m.y) / (size.y * 0.5f - 4), -1.0f, 1.0f);
    if (ImGui::GetIO().KeyAlt) v = std::round(v * 20.0f) / 20.0f;  // Alt: steps of 2.5 %
    if (std::fabs(v) < 0.03f) v = 0;  // the centre line catches
    editGroove(a, which == 0 ? "draw the groove timing" : "draw the groove accents", [which, k, v](SongGroove& g) {
      if (which == 0) g.shape.timing[static_cast<size_t>(k)] = v * 0.5f;
      else {
        if (g.shape.velocity.empty()) g.shape.velocity.assign(static_cast<size_t>(g.shape.cycle), 1.0f);
        g.shape.velocity[static_cast<size_t>(k)] = 1.0f + v;
      }
      g.preset.clear();
    });
  }
  if (!ImGui::IsItemActive() && E.dragLane == which) E.dragLane = -1;
}

// One bar: the straight grid (faint) and where the groove puts each cell (lit).
void preview(App& a, ImVec2 size) {
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, P.well, 3);
  Pattern ref;
  ref.stepsPerBar = 16;
  ref.bars = 1;
  const gv::Warp w = gv::makeWarp(s, &ref, s.groove.shape, s.swing, s.groove.accent);
  const double bar = static_cast<double>(s.barTicks());
  if (!(w.cell > 0) || !(bar > 0)) return;
  const int cells = static_cast<int>(std::floor(bar / w.cell + 1e-9));
  const float k = size.x / static_cast<float>(bar);
  for (int i = 0; i < cells; ++i) {
    const double t = w.cell * i;
    const float xs = std::round(p0.x + static_cast<float>(t) * k);
    dl->AddLine(ImVec2(xs, p0.y + 3), ImVec2(xs, p0.y + size.y * 0.42f), P.textFaint, 1);
    const double g = w.active ? std::max(0.0, w.map(t)) : t;
    const float xg = std::round(p0.x + static_cast<float>(g) * k);
    const float vel = std::clamp(w.velocityAt(t), 0.0f, 1.0f);
    const float top = p0.y + size.y * 0.5f + (1.0f - vel) * size.y * 0.3f;
    dl->AddRectFilled(ImVec2(xg - 1, top), ImVec2(xg + 2, p1.y - 3), P.accent, 1);
  }
  if (ImGui::IsItemHovered()) ui::Hint("One bar: the straight grid above, where the swing puts each cell below (height = accent)");
}

}  // namespace

void drawEditor(App& a) {
  if (!a.showGroove) return;
  ui::PlaceNextDialog(ImVec2(620, 420), ImGuiCond_FirstUseEver, ImVec2(520, 380));
  if (!ImGui::Begin(ui::windowTitle("Groove editor").c_str(), &a.showGroove, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const float fh = ImGui::GetFrameHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ---- top row: icon, preset chip, grid, cycle ----
  {
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ui::drawIcon(dl, ui::Icon::Groove, ImVec2(c.x + 10, c.y + fh * 0.5f), 16, theme::rgb(theme::role::kGraph));
    ImGui::Dummy(ImVec2(22, fh));
    ImGui::SameLine(0, 6);
    gInEditor = true;
    chip(a, 200);
    gInEditor = false;
    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Grid"));
    ImGui::SameLine(0, 6);
    static const char* const grids[] = {i18n::N_("Step"), i18n::N_("8ths"), i18n::N_("16ths"), i18n::N_("32nds")};
    static const char* const gridHints[] = {i18n::N_("The pattern's step (16ths in a 16-step pattern)"), i18n::N_("Eighth notes"),
                                            i18n::N_("Sixteenth notes, whatever the pattern's steps"), i18n::N_("Thirty-second notes")};
    int grid = s.groove.shape.grid;
    if (ui::Segmented("##grid", grids, gv::kGridCount, &grid, 46, 0, gridHints))
      editGroove(a, "swing grid", [grid](SongGroove& g) {
        g.shape.grid = grid;
        g.preset.clear();
      });
    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Cycle"));
    ImGui::SameLine(0, 6);
    char cyc[16];
    std::snprintf(cyc, sizeof cyc, "%d", s.groove.shape.cycle);
    if (ui::BeginCombo("##cycle", cyc, 52)) {
      for (int c2 : gv::kCycles) {
        char l[16];
        std::snprintf(l, sizeof l, "%d", c2);
        if (ImGui::Selectable(l, c2 == s.groove.shape.cycle))
          editGroove(a, "swing cycle", [c2](SongGroove& g) {
            g.shape.cycle = c2;
            g.preset.clear();
          });
      }
      ImGui::EndCombo();
    }
    ui::Hint("Cells per repeat: 2 = a classic swing pair, 4, 8 or 16 = a groove over a beat, two beats or a bar");
  }
  ImGui::Dummy(ImVec2(0, 4));

  // ---- amount and accent knobs, the feel ----
  {
    ImGui::BeginGroup();
    ui::ParamKnob("##amount", songParam(SongParam::Swing), 16.0f);
    ImGui::SameLine(0, 6);
    ImGui::BeginGroup();
    ImGui::TextColored(theme::vec(P.text), "%s", i18n::tr("Amount"));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f%%", s.swing * 100.0);
    ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ImGui::TextColored(theme::vec(P.textDim), "%s", buf);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::EndGroup();
    ui::Hint("How much of the groove plays (the channel rack's Swing knob)");
    ImGui::SameLine(0, 24);
    ImGui::BeginGroup();
    double acc = s.groove.accent;
    bool rel = false;
    if (ui::Knob("##accent", &acc, pdesc("Accent", 0, 1, 1), 16.0f, nullptr, false, &rel))
      editGroove(a, "groove accent", [acc](SongGroove& g) { g.accent = static_cast<float>(std::clamp(acc, 0.0, 1.0)); });
    if (rel) a.doc.endGesture();
    ImGui::SameLine(0, 6);
    ImGui::BeginGroup();
    ImGui::TextColored(theme::vec(P.text), "%s", i18n::tr("Accent"));
    std::snprintf(buf, sizeof buf, "%.0f%%", s.groove.accent * 100.0);
    ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ImGui::TextColored(theme::vec(P.textDim), "%s", buf);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::EndGroup();
    ui::Hint("How much of the velocity lane plays");
    ImGui::SameLine(0, 24);
    ImGui::BeginGroup();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Feel"));
    ImGui::TextColored(theme::vec(P.text), "%s", gv::feelText(s.groove.shape, s.swing).c_str());
    ImGui::EndGroup();
  }
  ImGui::Dummy(ImVec2(0, 4));

  // ---- lanes ----
  const float w = ImGui::GetContentRegionAvail().x;
  const float below = fh * 2 + 52;  // the preview strip and the button row
  const float room = std::max(120.0f, ImGui::GetContentRegionAvail().y - below);
  const float tH = std::floor(room * 0.56f), vH = std::floor(room * 0.44f) - 22;
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("TIMING"));
  lane(a, 0, ImVec2(w, std::max(48.0f, tH - 18)));
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("VELOCITY"));
  lane(a, 1, ImVec2(w, std::max(36.0f, vH)));
  ImGui::Dummy(ImVec2(0, 2));
  preview(a, ImVec2(w, fh + 4));
  ImGui::Dummy(ImVec2(0, 4));

  // ---- buttons ----
  {
    const float x0 = ImGui::GetCursorPosX();
    const ObjectId ch = a.selChannel;
    const bool canNotes = s.pattern(a.curPattern) != nullptr;
    if (ui::TextButton(i18n::tr("From pattern notes"), false, 0,
                       "Take the groove from the selected channel's notes and steps in the current pattern (all channels when none is selected)") &&
        canNotes)
      fromPattern(a, a.curPattern, ch);
    devRec("button frompattern");
    ImGui::SameLine(0, 6);
    ObjectId audio = kNoId;
    for (ObjectId id : a.playlist.selected)
      if (const Clip* c = s.clip(id); c && c->kind == ClipKind::Audio) audio = id;
    if (ui::TextButton(i18n::tr("From audio clip"), false, 0, "Take the groove from the selected audio clip's transients")) {
      if (audio == kNoId) a.notify(i18n::tr("Select an audio clip in the playlist first"), true);
      else {
        TempoMap tm;
        tm.build(s);
        fromClip(a, audio, tm);
      }
    }
    ImGui::SameLine(0, 6);
    if (ui::TextButton(i18n::tr("Save preset..."), false, 0, "Save this swing as one of your presets")) savePrompt(a);
    const float rw = ui::TextButtonWidth(i18n::tr("Reset to straight"));
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - rw));
    if (ui::TextButton(i18n::tr("Reset to straight"), false, 0, "No swing: the classic step swing at 0 %"))
      a.doc.editSong("reset the swing", [](Song& x) {
        x.groove = SongGroove{};
        x.swing = 0;
      });
    devRec("button reset");
  }
  ImGui::End();
}

void devCommand(App& a, int frame) {
  const Song& s = a.doc.song();
  const GrooveShape& g = s.groove.shape;
  std::printf("groove frame %d: swing %.4f preset \"%s\" label \"%s\" grid %d cycle %d accent %.3f notes %d undo %zu editor %d\n  timing",
              frame, s.swing, s.groove.preset.c_str(), gv::chipLabel(s, presets(a)).c_str(), g.grid, g.cycle,
              static_cast<double>(s.groove.accent), s.swingNotes ? 1 : 0, a.doc.history().position(), a.showGroove ? 1 : 0);
  for (float t : g.timing) std::printf(" %.4f", static_cast<double>(t));
  std::printf("\n  velocity");
  for (float v : g.velocity) std::printf(" %.4f", static_cast<double>(v));
  std::printf("\n");
  const int now = ImGui::GetFrameCount();
  for (const auto& [name, r] : devRects())
    if (r.frame >= now - 2)
      std::printf("  rect %s %.1f %.1f %.1f %.1f\n", name.c_str(), static_cast<double>(r.a.x), static_cast<double>(r.a.y),
                  static_cast<double>(r.b.x), static_cast<double>(r.b.y));
  std::fflush(stdout);
}

}  // namespace gl::grooveui
