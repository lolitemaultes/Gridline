// Gridline — context menus (channel, pattern, playlist track/clip, piano roll,
// mixer track/slot, knob, plugin window) and the editing helpers they share
// with the main menu. Window files open these popups and call the bodies here.
#include "../core/ClipLoop.h"
#include "../core/ClipSources.h"
#include "../core/I18n.h"
#include "J59Ui.h"
#include "Keys.h"
#include "J31Ui.h"
#include "HostedUi.h"
#include "ContextMenus.h"
#include "ExportUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include "../../third_party/imgui/imgui.h"
#include "../core/Json.h"
#include "../core/MenuOpsExtra.h"
#include "../core/Project.h"
#include "../core/TrackModes.h"
#include "../core/Timeline.h"
#include "../core/Presets.h"
#include "../core/MixerOps.h"
#include "../engine/PlaylistTools.h"
#include "../core/MenuParity.h"
#include "TrackIcons.h"
#include "App.h"
#include "EditTools.h"
#include "ScriptUi.h"
#include "../core/NoteTools.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "MidiUi.h"
#include "MenuParityUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "TrackModeUi.h"
#include "ChopUi.h"
#include "../packs/PacksUi.h"
#include "../core/Platform.h"

namespace gl {

namespace fs = std::filesystem;
using theme::pal;

namespace menus {

// ------------------------------------------------------------------ helpers ----

bool colorGrid(uint32_t* out) {
  int n = 0;
  const uint32_t* cols = ui::contentPalette(&n);
  bool picked = false;
  for (int i = 0; i < n; ++i) {
    if (i % 8) ImGui::SameLine(0, 4);
    ImGui::PushID(i);
    ImVec2 q = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("c", ImVec2(20, 20))) {
      *out = cols[i];
      picked = true;
    }
    bool hov = ImGui::IsItemHovered();
    ImGui::GetWindowDrawList()->AddRectFilled(q, ImVec2(q.x + 20, q.y + 20), theme::rgb(cols[i]), 4);
    if (hov) ImGui::GetWindowDrawList()->AddRect(q, ImVec2(q.x + 20, q.y + 20), pal().paper, 4, 0, 1.5f);
    ImGui::PopID();
  }
  if (picked) ImGui::CloseCurrentPopup();
  return picked;
}

uint32_t randomColor() {
  static std::mt19937 rng(std::random_device{}());
  int n = 0;
  const uint32_t* cols = ui::contentPalette(&n);
  // Saturated part of the palette (first 11 entries) reads best as a content colour.
  return cols[std::uniform_int_distribution<int>(0, std::min(n, 11) - 1)(rng)];
}

void renameField(const std::string& current, const std::function<void(const std::string&)>& ok) {
  static char buf[128];
  if (ImGui::IsWindowAppearing()) {
    std::snprintf(buf, sizeof buf, "%s", current.c_str());
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(200);
  if (ImGui::InputText("##rename", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
    ok(buf);
    ImGui::CloseCurrentPopup();
  }
  ImGui::TextDisabled("%s", i18n::tr("Enter to apply"));
}

namespace {
struct PromptState {
  bool requested = false;
  std::string title;
  char buf[160] = {};
  std::function<void(const std::string&)> ok;
};
PromptState prompt;
}  // namespace

void openPrompt(const std::string& title, const std::string& initial, std::function<void(const std::string&)> ok) {
  prompt.requested = true;
  prompt.title = title;
  std::snprintf(prompt.buf, sizeof prompt.buf, "%s", initial.c_str());
  prompt.ok = std::move(ok);
}

void drawPrompt() {
  if (prompt.requested) {
    ImGui::OpenPopup("##menuprompt");
    prompt.requested = false;
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.35f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  // J-54: dialog padding and a right-aligned footer like the other dialogs (Cancel, then the lit OK).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
  const bool open = ImGui::BeginPopupModal("##menuprompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (open) {
    constexpr float kFieldW = 320.0f, kButtonW = 88.0f;
    ImGui::PushFont(app().fontBold, 0.0f);
    ImGui::TextUnformatted(theme::fitText(i18n::tr(prompt.title.c_str()), kFieldW));
    ImGui::PopFont();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(kFieldW);
    bool enter = ImGui::InputText("##p", prompt.buf, sizeof prompt.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ui::FocusRing();
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kFieldW - ui::ButtonRowWidth(2, kButtonW));
    const bool cancel = ui::TextButton("Cancel", false, kButtonW) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("OK", true, kButtonW) || enter) {
      if (prompt.ok) prompt.ok(prompt.buf);
      prompt.ok = nullptr;
      ImGui::CloseCurrentPopup();
    } else if (cancel) {
      prompt.ok = nullptr;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void fileDialog(App& a, const char* title, bool save, std::vector<std::string> exts, const std::string& name,
                std::function<void(const std::string&)> ok) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.save = save;
  a.fileDialog.title = title;
  const char* h = gl::platform::homeEnv();
  a.fileDialog.dir = a.projectPath.empty() ? (h ? h : ".") : fs::path(a.projectPath).parent_path().string();
  a.fileDialog.exts = std::move(exts);
  a.fileDialog.name = name;
  a.fileDialog.onOk = std::move(ok);
}

namespace {
void pluginItems(PluginKind kind, const std::function<void(const std::string&)>& pick, const std::string& current) {
  std::map<std::string, std::vector<const PluginInfo*>> cats;
  std::vector<std::string> order;
  for (const auto& p : pluginCatalog()) {
    if (p.kind != kind) continue;
    if (!cats.count(p.category)) order.push_back(p.category);
    cats[p.category].push_back(&p);
  }
  for (const auto& c : order) {
    if (ImGui::BeginMenu(c.c_str())) {
      for (const PluginInfo* p : cats[c])
        if (ImGui::MenuItem(p->name.c_str(), nullptr, p->type == current)) pick(p->type);
      ImGui::EndMenu();
    }
  }
}
}  // namespace

void instrumentItems(const std::function<void(const std::string&)>& pick, const std::string& current) {
  pluginItems(PluginKind::Generator, pick, current);
}
void effectItems(const std::function<void(const std::string&)>& pick, const std::string& current) {
  pluginItems(PluginKind::Effect, pick, current);
}

// --------------------------------------------------------------- piano roll ----

bool pianoHasTarget(const App& a) {
  return a.doc.song().pattern(a.curPattern) && a.doc.song().channel(a.selChannel);
}

namespace {
std::vector<Note> pianoNotes(const App& a) {
  const Pattern* p = a.doc.song().pattern(a.curPattern);
  if (!p) return {};
  auto it = p->notes.find(a.selChannel);
  return it == p->notes.end() ? std::vector<Note>{} : it->second;
}
ops::NoteSel pianoSel(const App& a, size_t n) {
  ops::NoteSel sel(n, false);
  if (a.piano.selPattern == a.curPattern && a.piano.selChannel == a.selChannel)
    for (size_t i : a.piano.selected)
      if (i < n) sel[i] = true;
  return sel;
}
Tick pianoGrid(const App& a) { return std::max<Tick>(1, snapTicks(a.doc.song(), a.piano.snap)); }
Tick stepTicks(const App& a) { return std::max<Tick>(1, a.doc.song().ppq / 4); }
}  // namespace

size_t pianoSelectionCount(const App& a) {
  return a.piano.selPattern == a.curPattern && a.piano.selChannel == a.selChannel ? a.piano.selected.size() : 0;
}

bool pianoEdit(App& a, const char* label, const std::function<void(std::vector<Note>&, ops::NoteSel&)>& fn) {
  if (!pianoHasTarget(a)) return false;
  auto notes = pianoNotes(a);
  auto sel = pianoSel(a, notes.size());
  auto before = notes;
  fn(notes, sel);
  ops::sortNotes(notes, sel);
  if (notes != before) a.doc.setNotes(a.curPattern, a.selChannel, notes, label);
  a.piano.selPattern = a.curPattern;
  a.piano.selChannel = a.selChannel;
  a.piano.selected.clear();
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) a.piano.selected.insert(i);
  return true;
}

void pianoToolItems(App& a) {
  bool ok = pianoHasTarget(a);
  Tick grid = pianoGrid(a), step = stepTicks(a);
  if (ImGui::MenuItem(i18n::tr("Quick quantize"), keys::label("piano.quickQuantize"), false, ok))
    pianoEdit(a, "quantize notes", [&](auto& n, auto& s) { ops::quantize(n, s, grid, false); });
  if (ImGui::MenuItem(i18n::tr("Quantize starts and ends"), nullptr, false, ok))
    pianoEdit(a, "quantize notes", [&](auto& n, auto& s) { ops::quantize(n, s, grid, true); });
  if (ImGui::MenuItem(i18n::tr("Quantize..."), keys::label("piano.quantize"), false, ok)) openNoteTool(a, NoteTool::Quantize);
  if (ImGui::MenuItem(i18n::tr("Quick legato"), keys::label("piano.legato"), false, ok)) pianoEdit(a, "legato", [](auto& n, auto& s) { ops::legato(n, s); });
  if (ImGui::MenuItem(i18n::tr("Articulate..."), keys::label("piano.articulate"), false, ok)) openNoteTool(a, NoteTool::Articulate);
  if (ImGui::MenuItem(i18n::tr("Chop..."), keys::label("piano.chop"), false, ok)) openNoteTool(a, NoteTool::Chop);
  if (ImGui::BeginMenu(i18n::tr("Quick chop"), ok)) {
    if (ImGui::MenuItem(i18n::tr("Into steps"), keys::label("piano.chopSteps"))) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, step); });
    if (ImGui::MenuItem(i18n::tr("Into half steps"))) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, std::max<Tick>(1, step / 2)); });
    if (ImGui::MenuItem(i18n::tr("Into beats"))) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, a.doc.song().beatTicks()); });
    if (ImGui::MenuItem(i18n::tr("Into snap grid"))) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, grid); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Glue"), keys::label("piano.glue"), false, ok)) pianoEdit(a, "glue notes", [](auto& n, auto& s) { ops::glue(n, s); });
  if (ImGui::MenuItem(i18n::tr("Discard lengths"), nullptr, false, ok))
    pianoEdit(a, "discard note lengths", [&](auto& n, auto& s) { ops::discardLengths(n, s, step); });
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Strum..."), keys::label("piano.strum"), false, ok)) openNoteTool(a, NoteTool::Strum);
  if (ImGui::MenuItem(i18n::tr("Arpeggiate..."), keys::label("piano.arpeggiate"), false, ok)) openNoteTool(a, NoteTool::Arpeggiate);
  if (ImGui::MenuItem(i18n::tr("Grace notes..."), keys::label("piano.flam"), false, ok)) openNoteTool(a, NoteTool::Flam);
  if (ImGui::BeginMenu(i18n::tr("Quick strum"), ok)) {
    for (int div : {1, 2, 4}) {
      std::string l = div == 1 ? "Wide (1/4 step)" : div == 2 ? "Medium (1/8 step)" : "Tight (1/16 step)";
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "strum", [&](auto& n, auto& s) { ops::strum(n, s, std::max<Tick>(1, step / (4 * div))); });
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Quick arpeggio"), ok)) {
    if (ImGui::MenuItem(i18n::tr("Steps"))) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, step); });
    if (ImGui::MenuItem(i18n::tr("Half steps"))) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, std::max<Tick>(1, step / 2)); });
    if (ImGui::MenuItem(i18n::tr("Beats"))) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, a.doc.song().beatTicks()); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Flip"), ok)) {
    if (ImGui::MenuItem(i18n::tr("Upside down (pitch)"))) pianoEdit(a, "flip notes", [](auto& n, auto& s) { ops::flipVertical(n, s); });
    if (ImGui::MenuItem(i18n::tr("Backwards (time)"))) pianoEdit(a, "flip notes", [](auto& n, auto& s) { ops::flipHorizontal(n, s); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Randomize..."), keys::label("piano.randomize"), false, ok)) openNoteTool(a, NoteTool::Randomize);
  if (ImGui::MenuItem(i18n::tr("Riff generator..."), nullptr, false, ok)) openNoteTool(a, NoteTool::Riff);
  if (ImGui::MenuItem(i18n::tr("Humanize"), keys::label("piano.humanize"), false, ok)) {
    uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 7;
    pianoEdit(a, "humanize", [&](auto& n, auto& s) { ops::humanize(n, s, 12, std::max<Tick>(1, step / 8), seed); });
  }
  if (ImGui::MenuItem(i18n::tr("Note LFO..."), keys::label("piano.lfo"), false, ok)) openNoteTool(a, NoteTool::Lfo);
  if (ImGui::MenuItem(i18n::tr("Scale levels..."), keys::label("piano.scaleLevels"), false, ok)) openNoteTool(a, NoteTool::ScaleLevels);
  if (ImGui::MenuItem(i18n::tr("Limit key range..."), keys::label("piano.limit"), false, ok)) openNoteTool(a, NoteTool::Limit);
  if (ImGui::MenuItem(i18n::tr("Snap to the scale"), nullptr, false, ok && a.piano.scale > 0)) {
    const int root = a.piano.scaleRoot, sc = a.piano.scale;
    pianoEdit(a, "snap notes to scale", [&](auto& n, auto& s) { tools::snapToScale(n, s, root, sc); });
  }
  if (ImGui::BeginMenu(i18n::tr("Scale velocity"), ok)) {
    for (int pct : {50, 75, 90, 110, 125, 150}) {
      std::string l = std::to_string(pct) + "%";
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "scale velocity", [&](auto& n, auto& s) { ops::scaleVelocity(n, s, pct / 100.0); });
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Note properties..."), keys::label("piano.properties"), false, ok)) openNoteTool(a, NoteTool::Properties);
  if (ImGui::BeginMenu(i18n::tr("Mute"), ok)) {
    if (ImGui::MenuItem(i18n::tr("Mute"))) pianoEdit(a, "mute notes", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 1); });
    if (ImGui::MenuItem(i18n::tr("Unmute"))) pianoEdit(a, "unmute notes", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 0); });
    if (ImGui::MenuItem(i18n::tr("Toggle"))) pianoEdit(a, "toggle note mute", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 2); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Slide and portamento"), ok)) {
    if (ImGui::MenuItem(i18n::tr("Make slide notes"))) pianoEdit(a, "slide notes", [](auto& n, auto& s) { tools::setNotesSlide(n, s, 1); });
    if (ImGui::MenuItem(i18n::tr("Make normal notes"))) pianoEdit(a, "slide notes off", [](auto& n, auto& s) { tools::setNotesSlide(n, s, 0); });
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Portamento on"))) pianoEdit(a, "portamento on", [](auto& n, auto& s) { tools::setNotesPorta(n, s, 1); });
    if (ImGui::MenuItem(i18n::tr("Portamento off"))) pianoEdit(a, "portamento off", [](auto& n, auto& s) { tools::setNotesPorta(n, s, 0); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour group"), ok)) {
    for (int k = 0; k < kNoteColors; ++k) {
      std::string l = "Group " + std::to_string(k + 1) + (k == 0 ? " (channel colour)" : "");
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "colour notes", [k](auto& n, auto& s) { tools::setNotesColor(n, s, k); });
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Transpose up"), "Up", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, 1); });
  if (ImGui::MenuItem(i18n::tr("Transpose down"), "Down", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, -1); });
  if (ImGui::MenuItem(i18n::tr("Octave up"), keys::label("piano.octaveUp"), false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, 12); });
  if (ImGui::MenuItem(i18n::tr("Octave down"), keys::label("piano.octaveDown"), false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, -12); });
  if (ImGui::MenuItem(i18n::tr("Nudge left"), "Left", false, ok)) pianoEdit(a, "move notes", [&](auto& n, auto& s) { ops::nudge(n, s, -grid); });
  if (ImGui::MenuItem(i18n::tr("Nudge right"), "Right", false, ok)) pianoEdit(a, "move notes", [&](auto& n, auto& s) { ops::nudge(n, s, grid); });
}

void pianoSelectItems(App& a) {
  bool ok = pianoHasTarget(a);
  if (ImGui::MenuItem(i18n::tr("Select all"), "Ctrl+A", false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectAll(n, s); });
  if (ImGui::MenuItem(i18n::tr("Deselect"), "Ctrl+D", false, ok)) pianoEdit(a, "", [](auto&, auto& s) { ops::selectNone(s); });
  if (ImGui::MenuItem(i18n::tr("Invert selection"), keys::label("piano.invertSelection"), false, ok)) pianoEdit(a, "", [](auto&, auto& s) { ops::invertSelection(s); });
  if (ImGui::MenuItem(i18n::tr("Every other note"), nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectOdd(n, s); });
  if (ImGui::MenuItem(i18n::tr("Same pitches as selection"), nullptr, false, ok && pianoSelectionCount(a)))
    pianoEdit(a, "", [](auto& n, auto& s) { ops::selectSamePitch(n, s); });
  if (ImGui::MenuItem(i18n::tr("Overlapping notes"), nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectOverlapping(n, s); });
  if (ImGui::MenuItem(i18n::tr("Random notes"), keys::label("piano.selectRandom"), false, ok)) {
    uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 3;
    pianoEdit(a, "", [&](auto& n, auto& s) { ops::selectRandom(n, s, seed); });
  }
  if (ImGui::MenuItem(i18n::tr("Muted notes"), nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { tools::selectMuted(n, s); });
  if (ImGui::BeginMenu(i18n::tr("Colour group"), ok)) {
    for (int k = 0; k < kNoteColors; ++k) {
      std::string l = "Group " + std::to_string(k + 1);
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "", [k](auto& n, auto& s) { tools::selectColor(n, s, k); });
    }
    ImGui::EndMenu();
  }
  const bool sc = a.piano.scale > 0;
  if (ImGui::MenuItem(i18n::tr("Notes in the scale"), nullptr, false, ok && sc))
    pianoEdit(a, "", [&a](auto& n, auto& s) { tools::selectInScale(n, s, a.piano.scaleRoot, a.piano.scale, true); });
  if (ImGui::MenuItem(i18n::tr("Notes outside the scale"), nullptr, false, ok && sc))
    pianoEdit(a, "", [&a](auto& n, auto& s) { tools::selectInScale(n, s, a.piano.scaleRoot, a.piano.scale, false); });
}

void progressionItems(App& a, bool riff) {
  bool ok = pianoHasTarget(a);
  static int root = 60;
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (ImGui::BeginMenu(i18n::tr("Key"))) {
    for (int k = 0; k < 12; ++k)
      if (ImGui::MenuItem(names[k], nullptr, root % 12 == k)) root = 60 + k - (k > 6 ? 12 : 0);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  for (const auto& p : ops::progressions()) {
    if (!ImGui::MenuItem(p.name, nullptr, false, ok)) continue;
    const Song& s = a.doc.song();
    const Pattern* pat = s.pattern(a.curPattern);
    int bars = std::max(4, pat->bars);
    auto add = riff ? ops::progressionRiff(p, root, s.barTicks(), stepTicks(a) * 2, bars)
                    : ops::progressionNotes(p, root, s.barTicks(), bars);
    a.doc.beginGroup(riff ? "insert riff" : "insert chord progression");
    if (pat->bars < bars) a.doc.setPatternLength(pat->id, bars, pat->stepsPerBar);
    pianoEdit(a, riff ? "insert riff" : "insert chord progression", [&](auto& n, auto& sel) {
      sel.assign(n.size(), false);
      for (const auto& x : add) n.push_back(x), sel.push_back(true);
    });
    a.doc.endGroup();
    a.showPiano = true;
    a.notify(std::string(riff ? "Riff" : "Chords") + " added to " + s.channel(a.selChannel)->name);
  }
}

// ------------------------------------------------------------- edit routing ----

namespace {
struct RackClip {
  std::vector<uint8_t> steps;
  std::vector<Note> notes;
  bool valid = false;
};
RackClip rackClip;
std::vector<EffectSlot> fxChainClip;

std::vector<ObjectId> selectedChannels(const App& a) {
  std::vector<ObjectId> out;
  for (const auto& c : a.doc.song().channels)
    if (a.selChannels.count(c.id) || c.id == a.selChannel) out.push_back(c.id);
  return out;
}
}  // namespace

const char* editTargetName(const App& a) {
  switch (a.editFocus) {
    case App::EditFocus::Piano: return "notes";
    case App::EditFocus::Playlist: return "clips";
    case App::EditFocus::Rack: return "channel steps";
    case App::EditFocus::Mixer: return "effect chain";
    default: return "notes";
  }
}

void editCopy(App& a) {
  const Song& s = a.doc.song();
  switch (a.editFocus) {
    case App::EditFocus::Playlist:
      a.playlist.clipboard.clear();
      for (const auto& c : s.playlist)
        if (a.playlist.selected.count(c.id)) a.playlist.clipboard.push_back(c);
      a.notify(std::to_string(a.playlist.clipboard.size()) + " clips copied");
      break;
    case App::EditFocus::Rack: {
      const Pattern* p = s.pattern(a.curPattern);
      if (!p || !s.channel(a.selChannel)) return;
      rackClip = {};
      if (auto it = p->steps.find(a.selChannel); it != p->steps.end()) rackClip.steps = it->second;
      if (auto it = p->notes.find(a.selChannel); it != p->notes.end()) rackClip.notes = it->second;
      rackClip.valid = true;
      a.notify("Copied " + s.channel(a.selChannel)->name + " in " + p->name);
      break;
    }
    case App::EditFocus::Mixer:
      if (const MixerTrack* t = s.mixerTrack(a.selMixer)) {
        fxChainClip = t->slots;
        a.notify("Copied the effect chain of " + mixerTrackName(s, static_cast<size_t>(s.mixerIndex(t->id))));
      }
      break;
    default: {
      auto notes = pianoNotes(a);
      auto sel = pianoSel(a, notes.size());
      bool any = std::find(sel.begin(), sel.end(), true) != sel.end();
      a.piano.clipboard.clear();
      for (size_t i = 0; i < notes.size(); ++i)
        if (!any || sel[i]) a.piano.clipboard.push_back(notes[i]);
      a.notify(std::to_string(a.piano.clipboard.size()) + " notes copied");
    }
  }
}

void editDelete(App& a) {
  const Song& s = a.doc.song();
  switch (a.editFocus) {
    case App::EditFocus::Playlist:
      ops::removeClips(a.doc, a.playlist.selected);
      a.playlist.selected.clear();
      break;
    case App::EditFocus::Rack:
      a.doc.beginGroup("clear channels in pattern");
      for (ObjectId c : selectedChannels(a)) ops::clearChannel(a.doc, a.curPattern, c);
      a.doc.endGroup();
      break;
    case App::EditFocus::Mixer:
      if (const MixerTrack* t = s.mixerTrack(a.selMixer))
        a.doc.editMixerTrack(t->id, "clear effects", [](MixerTrack& m) { m.slots.assign(kEffectSlots, EffectSlot{}); });
      break;
    default:
      if (pianoSelectionCount(a)) pianoEdit(a, "delete notes", [](auto& n, auto& sel) { ops::deleteNotes(n, sel); });
  }
}

void editCut(App& a) {
  editCopy(a);
  if (a.editFocus == App::EditFocus::Piano || a.editFocus == App::EditFocus::None) {
    // Cut takes everything when nothing is selected, like copy.
    pianoEdit(a, "cut notes", [](auto& n, auto& sel) { ops::deleteNotes(n, sel); });
    return;
  }
  editDelete(a);
}

bool canPaste(const App& a) {
  switch (a.editFocus) {
    case App::EditFocus::Playlist: return !a.playlist.clipboard.empty();
    case App::EditFocus::Rack: return rackClip.valid;
    case App::EditFocus::Mixer: return !fxChainClip.empty();
    default: return !a.piano.clipboard.empty();
  }
}

void editPaste(App& a) {
  const Song& s = a.doc.song();
  switch (a.editFocus) {
    case App::EditFocus::Playlist: {
      auto& src = a.playlist.clipboard;
      if (src.empty()) return;
      Tick lo = src[0].start, hi = 0;
      for (const auto& c : src) lo = std::min(lo, c.start), hi = std::max(hi, c.start + s.clipLength(c));
      Tick bar = std::max<Tick>(1, s.barTicks());
      Tick shift = std::max(bar, (hi - lo + bar / 2) / bar * bar);
      std::set<ObjectId> added;
      a.doc.beginGroup("paste clips");
      for (auto& c : src) {
        if (ObjectId id = a.doc.addClip(c.pattern, c.track, c.start + shift, c.length)) added.insert(id);
        c.start += shift;
      }
      a.doc.endGroup();
      a.playlist.selected = added;
      break;
    }
    case App::EditFocus::Rack:
      if (!rackClip.valid || !s.pattern(a.curPattern)) return;
      a.doc.beginGroup("paste channel steps");
      for (ObjectId c : selectedChannels(a)) {
        a.doc.setSteps(a.curPattern, c, rackClip.steps);
        a.doc.setNotes(a.curPattern, c, rackClip.notes);
      }
      a.doc.endGroup();
      break;
    case App::EditFocus::Mixer:
      if (!fxChainClip.empty() && s.mixerTrack(a.selMixer))
        a.doc.editMixerTrack(a.selMixer, "paste effect chain", [](MixerTrack& m) { m.slots = fxChainClip; });
      break;
    default: {
      auto& clip = a.piano.clipboard;
      if (clip.empty()) return;
      Tick lo = clip[0].start, hi = 0;
      for (const auto& n : clip) lo = std::min(lo, n.start), hi = std::max(hi, n.start + n.length);
      Tick grid = pianoGrid(a);
      Tick shift = std::max(grid, (hi - lo + grid - 1) / grid * grid);
      pianoEdit(a, "paste notes", [&](auto& n, auto& sel) {
        sel.assign(n.size(), false);
        for (auto x : clip) {
          x.start += shift;
          n.push_back(x);
          sel.push_back(true);
        }
      });
      for (auto& x : clip) x.start += shift;
    }
  }
}

void editSelectAll(App& a) {
  const Song& s = a.doc.song();
  switch (a.editFocus) {
    case App::EditFocus::Playlist:
      a.playlist.selected.clear();
      for (const auto& c : s.playlist) a.playlist.selected.insert(c.id);
      break;
    case App::EditFocus::Rack:
      a.selChannels.clear();
      for (const auto& c : s.channels) a.selChannels.insert(c.id);
      break;
    default: pianoEdit(a, "", [](auto& n, auto& sel) { ops::selectAll(n, sel); });
  }
}

void editDeselect(App& a) {
  switch (a.editFocus) {
    case App::EditFocus::Playlist: a.playlist.selected.clear(); break;
    case App::EditFocus::Rack: a.selChannels = {a.selChannel}; break;
    default: a.piano.selected.clear();
  }
}

void editDuplicate(App& a) {
  switch (a.editFocus) {
    case App::EditFocus::Playlist: {
      auto ids = ops::duplicateClips(a.doc, a.playlist.selected);
      a.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end());
      break;
    }
    case App::EditFocus::Rack: {
      auto ids = ops::cloneChannels(a.doc, selectedChannels(a));
      if (!ids.empty()) {
        a.selChannel = ids.back();
        a.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
      }
      break;
    }
    case App::EditFocus::Mixer: break;
    default: pianoEdit(a, "duplicate notes", [&](auto& n, auto& sel) { ops::duplicate(n, sel, pianoGrid(a)); });
  }
}

void renameFocused(App& a) {
  const Song& s = a.doc.song();
  if (a.editFocus == App::EditFocus::Rack && s.channel(a.selChannel)) {
    ObjectId id = a.selChannel;
    openPrompt("Rename channel", s.channel(id)->name, [&a, id](const std::string& n) { a.doc.renameChannel(id, n); });
  } else if (a.editFocus == App::EditFocus::Mixer && s.mixerTrack(a.selMixer) && a.selMixer != s.masterId()) {
    ObjectId id = a.selMixer;
    openPrompt("Rename mixer track", mixerTrackName(s, static_cast<size_t>(s.mixerIndex(id))),
               [&a, id](const std::string& n) { a.doc.editMixerTrack(id, "rename mixer track", [&](MixerTrack& m) { m.name = n; }); });
  } else if (const Pattern* p = s.pattern(a.curPattern)) {
    ObjectId id = p->id;
    openPrompt("Rename pattern", p->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
  }
}

}  // namespace menus

using namespace menus;

// ---------------------------------------------------------- shared helpers ----

// Context menu caption (the object the menu acts on): a dim heading in the
// label column with a hairline, like the studio menus' title rows.
static void menuTitle(const char* name) { ImGui::SeparatorText(name && *name ? name : " "); }

void menuButton(const char* id, const char* hint, const std::function<void()>& body) {
  ImGui::PushID(id);
  const bool open = ImGui::IsPopupOpen("##menubtnpop");
  if (ui::IconButton("##menubtn", ui::Icon::Menu, open, ImGui::GetFrameHeight(), hint, theme::rgb(theme::role::kMenu),
                     theme::rgb(theme::role::kMenu, 0.85f)))
    ImGui::OpenPopup("##menubtnpop");
  if (ImGui::BeginPopup("##menubtnpop")) {
    body();
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

namespace {

void routeMenuItems(App& a, const std::function<bool(ObjectId)>& checked, const std::function<void(ObjectId)>& pick) {
  const Song& s = a.doc.song();
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    std::string label = (i == 0 ? std::string("Master") : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##r" + std::to_string(i);
    if (ImGui::MenuItem(label.c_str(), nullptr, checked(s.mixer[i].id))) pick(s.mixer[i].id);
  }
}

std::vector<ObjectId> selChannelsInOrder(const App& a) {
  std::vector<ObjectId> out;
  for (const auto& c : a.doc.song().channels)
    if (a.selChannels.count(c.id) || c.id == a.selChannel) out.push_back(c.id);
  return out;
}

}  // namespace

// ------------------------------------------------------------ channel rack ----

void addChannelMenu(App& a) {
  instrumentItems([&a](const std::string& t) { a.addInstrument(t); });
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Audio file as sampler..."))) {
    fileDialog(a, "Load audio file into a new sampler", false, {".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff"}, "",
               [&a](const std::string& p) { a.addInstrument("gl.sampler", p); });
  }
}

void channelContextMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(id);
  if (!c) return;
  const Pattern* p = s.pattern(a.curPattern);
  menuTitle(c->name.c_str());
  if (ImGui::MenuItem(i18n::tr("Piano roll"), keys::label("view.pianoRoll"))) a.openPianoRoll(id);
  if (ImGui::MenuItem(i18n::tr("Channel settings"))) a.settingsChannel = id;
  {  // J-7: the channel's sample in the audio editor
    const PluginInfo* pi = findPlugin(c->plugin.type);
    const bool hasSample = pi && pi->usesSample && !c->plugin.data.empty() && c->plugin.data[0] != '{' &&
                           c->plugin.data.find('\n') == std::string::npos;
    if (ImGui::MenuItem(i18n::tr("Edit sample in Loupe"), nullptr, false, hasSample)) packs::openAudioEditor(a, c->plugin.data, id);
    if (ImGui::MenuItem(i18n::tr("Export sample as WAV..."), nullptr, false, hasSample)) exportui::exportChannelSample(a, id);  // J-20
  }
  if (ImGui::MenuItem(i18n::tr("Select in playlist"))) {
    a.playlist.selected.clear();
    for (const auto& clip : s.playlist) {
      const Pattern* cp = s.pattern(clip.pattern);
      if (cp && (cp->steps.count(id) || cp->notes.count(id))) a.playlist.selected.insert(clip.id);
    }
  }
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Rename..."), true)) {
    renameField(c->name, [&a, id](const std::string& n) { a.doc.renameChannel(id, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col;
    if (colorGrid(&col)) ops::setChannelsColor(a.doc, {id}, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Random colour"))) ops::setChannelsColor(a.doc, {id}, randomColor());
  if (ImGui::MenuItem(c->muted ? i18n::tr("Unmute") : i18n::tr("Mute"))) ops::setChannelsMuted(a.doc, {id}, !c->muted);
  {
    const bool self = c->cutGroup > 0 && c->cutGroup == c->cutBy;
    if (ImGui::MenuItem(i18n::tr("Cut itself"), nullptr, self, !c->plugin.type.empty())) {
      if (self) a.doc.editChannel(id, "stop cutting itself", [](Channel& ch) { ch.cutGroup = ch.cutBy = 0; });
      else ops::cutItself(a.doc, {id});
    }
    ui::Hint("A new note stops this channel's previous note (monophonic choke)");
  }
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Replace instrument"))) {
    instrumentItems([&a, id](const std::string& t) { a.doc.setChannelPlugin(id, t); }, c->plugin.type);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Insert instrument below"))) {
    instrumentItems([&a, id](const std::string& t) {
      a.selectChannel(id);
      a.addInstrument(t);
    });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Render to sampled instrument..."))) j59::openRenderSampler(a, id);  // J-59 (RN-08)
  ui::Hint("Renders the instrument note by note into a multisampled Folio instrument");
  if (ImGui::BeginMenu(i18n::tr("Route to mixer track"))) {
    routeMenuItems(a, [c](ObjectId t) { return c->mixerTrack == t; }, [&a, id](ObjectId t) { a.doc.routeChannel(id, t); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Route to a free mixer track"), keys::label("rack.routeFree"))) {
    auto t = ops::routeToFreeMixerTracks(a.doc, {id});
    if (!t.empty()) a.selMixer = t[0];
  }
  trackui::channelMenuItems(a, id);  // PL-08: instrument track link
  ImGui::Separator();
  if (p) {
    if (ImGui::MenuItem(i18n::tr("Fill every step"))) ops::fillSteps(a.doc, p->id, id, 1);
    if (ImGui::MenuItem(i18n::tr("Fill every 2 steps"))) ops::fillSteps(a.doc, p->id, id, 2);
    if (ImGui::MenuItem(i18n::tr("Fill every 4 steps"))) ops::fillSteps(a.doc, p->id, id, 4);
    if (ImGui::MenuItem(i18n::tr("Fill every 8 steps"))) ops::fillSteps(a.doc, p->id, id, 8);
    if (ImGui::BeginMenu(i18n::tr("Fill offbeats"))) {
      if (ImGui::MenuItem(i18n::tr("Every 4 from step 3"))) ops::fillSteps(a.doc, p->id, id, 4, 2);
      if (ImGui::MenuItem(i18n::tr("Every 8 from step 5"))) ops::fillSteps(a.doc, p->id, id, 8, 4);
      if (ImGui::MenuItem(i18n::tr("Every 2 from step 2"))) ops::fillSteps(a.doc, p->id, id, 2, 1);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Advanced fill..."))) openStepTool(a, StepTool::Fill, id);
    if (ImGui::MenuItem(i18n::tr("Rotate left"), keys::label("rack.rotateLeft"))) ops::rotateChannel(a.doc, p->id, id, -1);
    if (ImGui::MenuItem(i18n::tr("Rotate right"), keys::label("rack.rotateRight"))) ops::rotateChannel(a.doc, p->id, id, 1);
    if (ImGui::BeginMenu(i18n::tr("Steps"))) {  // J-11 step tools
      const ObjectId pid = p->id;
      if (ImGui::MenuItem(i18n::tr("Shift left"))) ops::editStepRow(a.doc, pid, id, "shift steps left", [](auto& r, auto& pr) { tools::shiftSteps(r, pr, -1); });
      if (ImGui::MenuItem(i18n::tr("Shift right"))) ops::editStepRow(a.doc, pid, id, "shift steps right", [](auto& r, auto& pr) { tools::shiftSteps(r, pr, 1); });
      if (ImGui::MenuItem(i18n::tr("Reverse"))) ops::editStepRow(a.doc, pid, id, "reverse steps", [](auto& r, auto& pr) { tools::reverseSteps(r, pr); });
      if (ImGui::MenuItem(i18n::tr("Invert"))) ops::editStepRow(a.doc, pid, id, "invert steps", [](auto& r, auto&) { tools::invertSteps(r); });
      if (ImGui::MenuItem(i18n::tr("Randomize..."))) openStepTool(a, StepTool::Randomize, id);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Insert a step at the start"))) ops::editStepRow(a.doc, pid, id, "insert step", [](auto& r, auto& pr) { tools::insertStep(r, pr, 0); });
      if (ImGui::MenuItem(i18n::tr("Delete the first step"))) ops::editStepRow(a.doc, pid, id, "delete step", [](auto& r, auto& pr) { tools::deleteStep(r, pr, 0); });
      ImGui::TextDisabled("%s", i18n::tr("Insert / Delete over a step: insert or remove it"));
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Reset step parameters"), nullptr, false, p->stepProps.count(id) > 0))
        ops::editStepRow(a.doc, pid, id, "reset step parameters", [](auto&, auto& pr) { std::fill(pr.begin(), pr.end(), StepProps{}); });
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Graph editor"), keys::label("rack.graphEditor"), a.rack.showGraph && a.rack.graphLane != static_cast<int>(tools::StepLane::Pitch)))
      a.rack.showGraph = true, a.rack.graphLane = 0, a.selectChannel(id);
    if (ImGui::MenuItem(i18n::tr("Keyboard editor"), nullptr, a.rack.showGraph && a.rack.graphLane == static_cast<int>(tools::StepLane::Pitch)))
      a.rack.showGraph = true, a.rack.graphLane = static_cast<int>(tools::StepLane::Pitch), a.selectChannel(id);
    const bool hasSteps = p->steps.count(id) > 0, hasNotes = p->notes.count(id) && !p->notes.at(id).empty();
    if (ImGui::MenuItem(i18n::tr("Steps to piano roll"), nullptr, false, hasSteps)) ops::convertStepsToNotes(a.doc, p->id, id);
    if (ImGui::MenuItem(i18n::tr("Piano roll to steps"), nullptr, false, hasNotes)) {
      ops::convertNotesToSteps(a.doc, p->id, id);
      if (const Pattern* q = a.doc.song().pattern(p->id); q && q->notes.count(id)) a.notify("Notes off the step grid stay in the piano roll");
    }
    if (ImGui::MenuItem(i18n::tr("Clear steps and notes"))) ops::clearChannel(a.doc, p->id, id);
    ImGui::Separator();
    a.editFocus = App::EditFocus::Rack;  // edit commands below act on the selected channel
    if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X")) editCut(a);
    if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C")) editCopy(a);
    if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, canPaste(a))) editPaste(a);
  }
  ImGui::Separator();
  int idx = s.channelIndex(id);
  if (ImGui::MenuItem(i18n::tr("Move up"), keys::label("rack.moveUp"), false, idx > 0)) a.doc.moveChannel(id, idx - 1);
  if (ImGui::MenuItem(i18n::tr("Move down"), keys::label("rack.moveDown"), false, idx + 1 < static_cast<int>(s.channels.size()))) a.doc.moveChannel(id, idx + 1);
  if (ImGui::MenuItem(i18n::tr("Clone"), keys::label("rack.clone"))) a.selectChannel(a.doc.cloneChannel(id));
  menuparity::channelItems(a, id);  // J-27
  j31::channelItems(a, id);         // J-31: step loop, key range, truncated swing, layer split
  if (ImGui::MenuItem(a.rack.zipped.count(id) ? i18n::tr("Unzip") : i18n::tr("Zip"), keys::label("rack.zip"))) {
    if (a.rack.zipped.count(id))
      a.rack.zipped.erase(id);
    else
      a.rack.zipped.insert(id);
  }
  if (ImGui::MenuItem(i18n::tr("Delete"), keys::label("rack.delete"))) a.doc.removeChannel(id);
}

void rackOptionsMenu(App& a) {
  const Song& s = a.doc.song();
  auto sel = selChannelsInOrder(a);
  bool any = !sel.empty();
  if (ImGui::BeginMenu(i18n::tr("Add channel"))) {
    addChannelMenu(a);
    ImGui::EndMenu();
  }
  ImGui::SeparatorText(i18n::tr("Selected channels"));
  if (ImGui::MenuItem(i18n::tr("Clone selected"), keys::label("rack.clone"), false, any)) {
    auto ids = ops::cloneChannels(a.doc, sel);
    if (!ids.empty()) a.selChannel = ids.back(), a.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
  }
  if (ImGui::MenuItem(i18n::tr("Delete selected"), keys::label("rack.delete"), false, any)) ops::removeChannels(a.doc, sel);
  if (ImGui::MenuItem(i18n::tr("Move selected up"), keys::label("rack.moveUp"), false, any)) ops::moveChannels(a.doc, sel, -1);
  if (ImGui::MenuItem(i18n::tr("Move selected down"), keys::label("rack.moveDown"), false, any)) ops::moveChannels(a.doc, sel, 1);
  if (ImGui::BeginMenu(i18n::tr("Colour selected"), any)) {
    uint32_t col;
    if (colorGrid(&col)) ops::setChannelsColor(a.doc, sel, col);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Gradient (first to last)"), nullptr, false, sel.size() >= 3)) ops::gradientColor(a.doc, sel);
    if (ImGui::MenuItem(i18n::tr("Random"), keys::label("rack.randomColours"))) {
      a.doc.beginGroup("random colours");
      for (ObjectId c : sel) ops::setChannelsColor(a.doc, {c}, randomColor());
      a.doc.endGroup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Mute selected"), nullptr, false, any)) ops::setChannelsMuted(a.doc, sel, true);
  if (ImGui::MenuItem(i18n::tr("Unmute selected"), nullptr, false, any)) ops::setChannelsMuted(a.doc, sel, false);
  if (ImGui::BeginMenu(i18n::tr("Transpose selected"), any)) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposeChannels(a.doc, a.curPattern, sel, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Swing mix for selected"), any)) {
    for (int pct : {0, 25, 50, 75, 100}) {
      std::string l = std::to_string(pct) + "% of the song swing";
      if (ImGui::MenuItem(l.c_str())) ops::setSwingMix(a.doc, sel, pct / 100.0f);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Cut itself (selected)"), nullptr, false, any)) ops::cutItself(a.doc, sel);
  if (ImGui::MenuItem(i18n::tr("Create layer with selected channels"), nullptr, false, any)) {
    ObjectId l = ops::createLayer(a.doc, sel);
    if (l) a.selectChannel(l);
    else a.notify("Layers can only hold instrument channels", true);
  }
  if (ImGui::BeginMenu(i18n::tr("Fill selected"), any)) {
    for (int every : {1, 2, 4, 8}) {
      std::string l = every == 1 ? "Every step" : "Every " + std::to_string(every) + " steps";
      if (ImGui::MenuItem(l.c_str())) {
        a.doc.beginGroup("fill steps");
        for (ObjectId c : sel) ops::fillSteps(a.doc, a.curPattern, c, every);
        a.doc.endGroup();
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Create layer from selected"), nullptr, false, any)) {
    ObjectId l = ops::createLayer(a.doc, sel);
    if (l) a.selectChannel(l), a.notify("Layer created: its notes play on " + std::to_string(sel.size()) + " channel(s)");
  }
  if (ImGui::MenuItem(i18n::tr("Move selected to a new pattern"), nullptr, false, any)) {
    ObjectId np = ops::channelsToNewPattern(a.doc, a.curPattern, sel, true);
    if (np) a.selectPattern(np);
  }
  if (ImGui::MenuItem(i18n::tr("Copy selected to a new pattern"), nullptr, false, any)) {
    ObjectId np = ops::channelsToNewPattern(a.doc, a.curPattern, sel, false);
    if (np) a.selectPattern(np);
  }
  if (ImGui::BeginMenu(i18n::tr("Swing mix for selected"), any)) {
    for (int pctv : {0, 25, 50, 75, 100})
      if (ImGui::MenuItem((std::to_string(pctv) + "%").c_str())) ops::setSwingMix(a.doc, sel, pctv / 100.0f);
    ImGui::EndMenu();
  }
  menuparity::rackItems(a, sel);  // J-27
  j31::rackItems(a, sel);         // J-31: step loops, truncated swing
  if (ImGui::MenuItem(i18n::tr("Zip selected"), keys::label("rack.zip"), false, any))
    for (ObjectId c : sel) a.rack.zipped.insert(c);
  if (ImGui::MenuItem(i18n::tr("Unzip all"), keys::label("rack.unzipAll"), false, !a.rack.zipped.empty())) a.rack.zipped.clear();
  if (ImGui::MenuItem(i18n::tr("Route selected to free mixer tracks"), keys::label("rack.routeFree"), false, any)) {
    auto t = ops::routeToFreeMixerTracks(a.doc, sel);
    if (!t.empty()) a.selMixer = t.back();
  }
  if (ImGui::MenuItem(i18n::tr("Select their mixer tracks"), nullptr, false, any))
    if (const Channel* c = s.channel(sel.front())) a.selMixer = c->mixerTrack, a.showMixer = true;
  ImGui::SeparatorText(i18n::tr("All channels"));
  if (ImGui::BeginMenu(i18n::tr("Sort by"))) {
    if (ImGui::MenuItem(i18n::tr("Name"))) ops::sortChannels(a.doc, ops::ChannelSort::Name);
    if (ImGui::MenuItem(i18n::tr("Colour"))) ops::sortChannels(a.doc, ops::ChannelSort::Color);
    if (ImGui::MenuItem(i18n::tr("Mixer track"))) ops::sortChannels(a.doc, ops::ChannelSort::MixerTrack);
    if (ImGui::MenuItem(i18n::tr("Instrument"))) ops::sortChannels(a.doc, ops::ChannelSort::Instrument);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Select all"), "Ctrl+A")) {
    a.selChannels.clear();
    for (const auto& c : s.channels) a.selChannels.insert(c.id);
  }
  if (ImGui::MenuItem(i18n::tr("Select unused channels"))) {
    auto u = ops::unusedChannels(s);
    if (u.empty()) {
      a.notify("Every channel is used in a pattern");
    } else {
      a.selChannels = std::set<ObjectId>(u.begin(), u.end());
      a.selChannel = u.front();
    }
  }
}

// ---------------------------------------------------------------- patterns ----

void patternContextMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(id);
  if (!p) return;
  menuTitle(p->name.c_str());
  if (ImGui::BeginMenu(i18n::tr("Rename..."))) {
    renameField(p->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, id, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Random colour"))) ops::setPatternColor(a.doc, id, randomColor());
  if (ImGui::MenuItem(i18n::tr("Select in playlist"))) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.pattern == id) a.playlist.selected.insert(c.id);
  }
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Length"))) {
    for (int b : {1, 2, 4, 8, 16}) {
      std::string l = std::to_string(b) + (b == 1 ? " bar" : " bars");
      if (ImGui::MenuItem(l.c_str(), nullptr, p->bars == b)) a.doc.setPatternLength(id, b, p->stepsPerBar);
    }
    ImGui::Separator();
    for (int st : {8, 12, 16, 24, 32}) {
      std::string l = std::to_string(st) + " steps per bar";
      if (ImGui::MenuItem(l.c_str(), nullptr, p->stepsPerBar == st)) a.doc.setPatternLength(id, p->bars, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Transpose"))) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposePattern(a.doc, id, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Split by channel"))) {
    auto ids = ops::splitPatternByChannel(a.doc, id);
    if (ids.empty())
      a.notify("Nothing to split: the pattern is empty");
    else
      a.notify("Split into " + std::to_string(ids.size()) + " patterns");
  }
  ImGui::Separator();
  int idx = s.patternIndex(id);
  if (ImGui::MenuItem(i18n::tr("Insert one before"), keys::label("pattern.insert"))) a.selectPattern(ops::insertPattern(a.doc, id, ops::nextPatternName(s)));
  if (ImGui::MenuItem(i18n::tr("Clone"), keys::label("pattern.clone"))) a.selectPattern(a.doc.clonePattern(id));
  if (ImGui::MenuItem(i18n::tr("Move up"), keys::label("pattern.moveUp"), false, idx > 0)) ops::movePattern(a.doc, id, idx - 1);
  if (ImGui::MenuItem(i18n::tr("Move down"), keys::label("pattern.moveDown"), false, idx + 1 < static_cast<int>(s.patterns.size()))) ops::movePattern(a.doc, id, idx + 1);
  // J-82: the picker's selection when it holds this pattern; a pattern the
  // arrangement uses asks first (as audio and automation sources do).
  const auto& psel = a.playlist.pickerSel;
  std::vector<ObjectId> targets = psel.size() > 1 && psel.count(id) ? std::vector<ObjectId>(psel.begin(), psel.end()) : std::vector<ObjectId>{id};
  char label[96];
  if (targets.size() > 1)
    std::snprintf(label, sizeof label, i18n::tr("Delete %d selected..."), static_cast<int>(targets.size()));
  else
    std::snprintf(label, sizeof label, "%s", i18n::tr("Delete"));
  if (ImGui::MenuItem(label, keys::label("pattern.delete"), false, s.patterns.size() > targets.size()))
    requestDeleteSources(a, ClipKind::Pattern, targets);
}

// ---------------------------------------------------------------- playlist ----

void playlistTrackMenu(App& a, int track) {
  const Song& s = a.doc.song();
  int n = static_cast<int>(s.tracks.size());
  if (track < 0 || track >= n) return;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  menuTitle(playlistTrackName(s, static_cast<size_t>(track)).c_str());
  if (ImGui::BeginMenu(i18n::tr("Rename..."))) {
    renameField(t.name, [&a, track](const std::string& nm) {
      a.doc.editPlaylistTrack(track, "rename playlist track", [&](PlaylistTrack& x) { x.name = nm; });
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col;
    if (colorGrid(&col)) a.doc.editPlaylistTrack(track, "change track colour", [&](PlaylistTrack& x) { x.color = col; });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Random colour"))) {
    uint32_t col = randomColor();
    a.doc.editPlaylistTrack(track, "change track colour", [&](PlaylistTrack& x) { x.color = col; });
  }
  if (ImGui::BeginMenu(i18n::tr("Live settings"))) {
    liveTrackMenu(a, track);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Group"))) {
    for (int k = 1; k <= 16 && track + k < n; ++k) {
      std::string label = "Hold the next " + std::to_string(k) + (k == 1 ? " track" : " tracks");
      if (ImGui::MenuItem(label.c_str(), nullptr, t.groupSize == k))
        a.doc.editPlaylistTrack(track, "group playlist tracks", [k](PlaylistTrack& x) { x.groupSize = k; });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Fold"), nullptr, t.folded, t.groupSize > 0))
      a.doc.editPlaylistTrack(track, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
    if (ImGui::MenuItem(i18n::tr("Ungroup"), nullptr, false, t.groupSize > 0))
      a.doc.editPlaylistTrack(track, "ungroup playlist tracks", [](PlaylistTrack& x) { x.groupSize = 0, x.folded = false; });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Name after its first clip"))) {
    const Clip* first = nullptr;
    for (const auto& c : s.playlist)
      if (c.track == track && (!first || c.start < first->start)) first = &c;
    if (first)
      if (const Pattern* p = s.pattern(first->pattern)) {
        std::string nm = p->name;
        a.doc.editPlaylistTrack(track, "auto name track", [&](PlaylistTrack& x) { x.name = nm; });
      }
  }
  if (ImGui::MenuItem(i18n::tr("Reset name and colour")))
    a.doc.editPlaylistTrack(track, "reset track", [](PlaylistTrack& x) {
      PlaylistTrack k;  // the track mode stays (it is changed in its own menu)
      k.mode = x.mode, k.mixer = x.mixer, k.channel = x.channel;
      x = k;
    });
  trackui::trackMenuItems(a, track);  // PL-08: track mode, input, arm, monitoring
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Mute"), nullptr, t.muted))
    a.doc.editPlaylistTrack(track, "mute playlist track", [](PlaylistTrack& x) { x.muted = !x.muted; });
  if (ImGui::MenuItem(i18n::tr("Lock"), nullptr, t.locked)) ops::setTrackLocked(a.doc, track, !t.locked);
  if (ImGui::BeginMenu(i18n::tr("Size"))) {
    for (int pct : {33, 50, 100, 150, 200}) {
      const float h = pct / 100.0f;
      if (ImGui::MenuItem((std::to_string(pct) + "%").c_str(), nullptr, std::fabs(t.height - h) < 0.02f)) ops::setTrackHeights(a.doc, {track}, h);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Give every track this size"))) {
      std::vector<int> all;
      for (int k = 0; k < n; ++k) all.push_back(k);
      ops::setTrackHeights(a.doc, all, t.height);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Group with the track above"), nullptr, false, track > 0)) {
    int above = track - 1;  // the nearest group head above that can reach this track, else the track right above
    for (int k = track - 1; k >= 0; --k)
      if (s.tracks[static_cast<size_t>(k)].groupSize > 0) {
        above = k;
        break;
      }
    const int need = track - above;
    a.doc.editPlaylistTrack(above, "group playlist tracks", [need](PlaylistTrack& x) { x.groupSize = std::max(x.groupSize, need); });
  }
  {  // J-28 (operations: J-27)
    const bool grouped = ops::trackGroupOf(s, track).first >= 0;
    if (ImGui::MenuItem(i18n::tr("Auto colour group"), nullptr, false, grouped)) ops::autoColorTrackGroup(a.doc, track);
    if (ImGui::MenuItem(i18n::tr("Collapse grouped tracks"), nullptr, false, grouped)) {
      const int head = ops::trackGroupOf(s, track).first;
      a.doc.editPlaylistTrack(head, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
    }
    const Pattern* cur = s.pattern(a.curPattern);
    const std::string send = cur ? "Send \"" + cur->name + "\" clips here" : std::string("Send pattern clips here");
    if (ImGui::MenuItem(send.c_str(), nullptr, false, cur != nullptr)) {
      const int n2 = ops::sendPatternClipsToTrack(a.doc, a.curPattern, track);
      if (n2 == 0) a.notify("No clips of this pattern to move");
    }
  }
  if (ImGui::MenuItem(i18n::tr("Mute all clips"))) ops::muteTrackClips(a.doc, track, true);
  if (ImGui::MenuItem(i18n::tr("Unmute all clips"))) ops::muteTrackClips(a.doc, track, false);
  if (ImGui::MenuItem(i18n::tr("Merge pattern clips"))) {
    ObjectId nc = ops::mergeTrackPatternClips(a.doc, track);
    if (nc) a.playlist.selected = {nc};
    else a.notify("Merging needs at least two pattern clips on the track");
  }
  if (ImGui::MenuItem(i18n::tr("Select all clips on track"))) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.track == track) a.playlist.selected.insert(c.id);
  }
  if (ImGui::MenuItem(i18n::tr("Delete all clips on track"))) ops::clearPlaylistTrack(a.doc, track);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Insert one above"), nullptr, false, n < kMaxPlaylistTracks)) ops::insertPlaylistTrack(a.doc, track);
  if (ImGui::MenuItem(i18n::tr("Clone"), nullptr, false, n < kMaxPlaylistTracks)) ops::clonePlaylistTrack(a.doc, track);
  if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, track > 0)) ops::movePlaylistTrack(a.doc, track, -1);
  if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, track + 1 < n)) ops::movePlaylistTrack(a.doc, track, 1);
  if (ImGui::MenuItem(i18n::tr("Delete track"), nullptr, false, n > 1)) ops::deletePlaylistTrack(a.doc, track);
  if (ImGui::MenuItem(i18n::tr("Add 8 tracks"), nullptr, false, n + 8 <= kMaxPlaylistTracks)) a.doc.setPlaylistTrackCount(n + 8);
}

void clipContextMenu(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  const Pattern* p = s.pattern(c->pattern);
  if (!p) return;
  menuTitle(p->name.c_str());
  if (ImGui::MenuItem(i18n::tr("Edit pattern"))) {
    a.selectPattern(p->id);
    a.showPiano = true;
    a.focusWindow = "Piano roll";
  }
  if (ImGui::MenuItem(i18n::tr("Select all clips of this pattern"))) a.playlist.selected = ops::clipsOfSameSource(s, clip);
  if (ImGui::BeginMenu(i18n::tr("Rename pattern..."))) {
    ObjectId pid = p->id;
    renameField(p->name, [&a, pid](const std::string& n) { a.doc.renamePattern(pid, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Pattern colour"))) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, p->id, col);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Make unique"))) {
    ObjectId np = ops::makeClipUnique(a.doc, clip);
    if (np) a.selectPattern(np);
  }
  Tick patLen = s.patternContentLength(*p), len = s.clipLength(*c);
  if (ImGui::MenuItem(i18n::tr("Split at pattern repeats"), nullptr, false, patLen > 0 && (len > patLen || clipRepeats(s, *c)))) ops::chopClip(a.doc, clip, patLen);  // Q-1: at the loop points
  {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem(i18n::tr("Slice at the song position"), nullptr, false, pos > c->start && pos < c->start + len)) ops::sliceClip(a.doc, clip, pos);
  }
  chopui::clipMenu(a, clip);  // PL-13: slice at the grid / into parts / at note starts, stutter presets
  if (ImGui::MenuItem(i18n::tr("Split by channel"))) {
    auto ids = ops::splitClipByChannel(a.doc, clip);
    a.notify(ids.empty() ? std::string("Nothing to split") : "Split into " + std::to_string(ids.size()) + " clips");
  }
  if (ImGui::MenuItem(i18n::tr("Fit length to pattern"), nullptr, false, c->length != 0)) a.doc.setClipLength(clip, 0);
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(clip)) sel = {clip};
  if (ImGui::MenuItem(i18n::tr("Merge selected clips"), keys::label("playlist.merge"), false, sel.size() >= 2)) {
    ObjectId nc = ops::mergeClips(a.doc, sel);
    if (nc) a.playlist.selected = {nc};
  }
  if (ImGui::MenuItem(i18n::tr("Duplicate"), "Ctrl+B")) {
    auto ids = ops::duplicateClips(a.doc, sel);
    a.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end());
  }
  if (ImGui::MenuItem(i18n::tr("Delete"), "Del")) {
    ops::removeClips(a.doc, sel);
    a.playlist.selected.clear();
  }
}

void playlistOptionsMenu(App& a) {
  const Song& s = a.doc.song();
  a.editFocus = App::EditFocus::Playlist;
  bool any = !a.playlist.selected.empty();
  j31::playlistItems(a);  // J-31: clip groups
  if (ImGui::BeginMenu(i18n::tr("Edit"))) {
    if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, any)) editCut(a);
    if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, any)) editCopy(a);
    if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, canPaste(a))) editPaste(a);
    if (ImGui::MenuItem(i18n::tr("Duplicate"), "Ctrl+B", false, any)) editDuplicate(a);
    if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, any)) editDelete(a);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Merge selected clips"), keys::label("playlist.merge"), false, a.playlist.selected.size() >= 2)) {
      ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
      if (nc) a.playlist.selected = {nc};
    }
    if (ImGui::MenuItem(i18n::tr("Mute / unmute selected clips"), "M", false, any)) ops::muteClips(a.doc, a.playlist.selected, 2);
    if (ImGui::BeginMenu(i18n::tr("Chop selected clips"), any)) {
      const Tick bar = s.barTicks(), beat = s.beatTicks();
      if (ImGui::MenuItem(i18n::tr("Every bar"))) chopui::sliceSelectedAtGrid(a, bar);
      if (ImGui::MenuItem(i18n::tr("Every 2 beats"))) chopui::sliceSelectedAtGrid(a, beat * 2);
      if (ImGui::MenuItem(i18n::tr("Every beat"))) chopui::sliceSelectedAtGrid(a, beat);
      if (ImGui::MenuItem(i18n::tr("At the snap grid"), keys::label("playlist.sliceGrid"))) chopui::sliceSelectedAtGrid(a, snapTicks(s, a.playlist.snap == Snap::None ? Snap::Line : a.playlist.snap));
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Chop and stutter..."), keys::label("playlist.chopWindow"), false, any || a.playlist.selEnd > a.playlist.selStart))
      chopui::open(a);
    if (ImGui::MenuItem(i18n::tr("Slice at the song position"), nullptr, false, true)) {  // J-28: every clip kind, audio included
      const Tick at = static_cast<Tick>(a.engine.position());
      const auto ids = pltools::clipsAcross(s, at, 0, static_cast<int>(s.tracks.size()) - 1);
      if (!ids.empty()) a.doc.editSong("slice clips", [&](Song& x) { pltools::sliceClips(x, ids, at, a.playlist.zeroCross); });
    }
    {  // J-28: audio and automation clip items (operations from J-27)
      bool audioSel = false, autoSel = false;
      for (const auto& c : s.playlist)
        if (a.playlist.selected.count(c.id)) audioSel = audioSel || c.kind == ClipKind::Audio, autoSel = autoSel || c.kind == ClipKind::Automation;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Pitch selected audio clips a semitone up"), nullptr, false, audioSel)) ops::pitchAudioClips(a.doc, a.playlist.selected, 100.0f);
      if (ImGui::MenuItem(i18n::tr("Pitch selected audio clips a semitone down"), nullptr, false, audioSel)) ops::pitchAudioClips(a.doc, a.playlist.selected, -100.0f);
      if (ImGui::MenuItem(i18n::tr("Reverse selected audio clips"), nullptr, false, audioSel)) ops::reverseAudioClips(a.doc, a.playlist.selected);
      // J-38: fades and automatic crossfades.
      if (ImGui::MenuItem(i18n::tr("Automatic crossfades"), nullptr, s.autoCrossfades))
        a.doc.editSong(s.autoCrossfades ? "automatic crossfades off" : "automatic crossfades on", [](Song& x) { x.autoCrossfades = !x.autoCrossfades; });
      ui::Hint("Overlapping audio clips on a track crossfade over the overlap (this project)");
      if (ImGui::MenuItem(i18n::tr("Reset fades of selected audio clips"), nullptr, false, audioSel)) {
        const auto sel = a.playlist.selected;
        a.doc.editClips("reset fades", [&](std::vector<Clip>& v) {
          for (Clip& x : v)
            if (x.kind == ClipKind::Audio && sel.count(x.id)) {
              x.fadeIn = x.fadeOut = 0, x.fadeInManual = x.fadeOutManual = false;
              x.fadeInShape = x.fadeOutShape = 0, x.fadeInTension = x.fadeOutTension = 0;
            }
        });
      }
      if (ImGui::MenuItem(i18n::tr("Merge selected automation clips"), nullptr, false, autoSel)) {
        ObjectId nc = ops::mergeAutomationClips(a.doc, a.playlist.selected);
        if (nc) a.playlist.selected = {nc};
        else a.notify("Merging needs two automation clips of the same parameter");
      }
    }
    if (ImGui::MenuItem(i18n::tr("Make selected clips unique"), nullptr, false, any)) {
      a.doc.beginGroup("make clips unique");
      for (ObjectId id : a.playlist.selected) ops::makeClipUnique(a.doc, id);
      a.doc.endGroup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Arrangements"))) {
    arrangementMenu(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Live area"))) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem(i18n::tr("End it at the time selection end"), nullptr, false, a.playlist.selEnd > a.playlist.selStart))
      setLiveArea(a, a.playlist.selEnd);
    if (ImGui::MenuItem(i18n::tr("End it at the song position"), nullptr, false, pos > 0)) setLiveArea(a, pos);
    if (ImGui::MenuItem(i18n::tr("Remove the live area"), nullptr, false, s.live.areaEnd > 0)) setLiveArea(a, 0);
    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("Live pads window"), nullptr, &a.showLivePads);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Time markers"))) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem(i18n::tr("Add marker at the song position"))) ops::addMarker(a.doc, pos, ops::nextMarkerName(s));
    {  // J-85: one step to a song loop (Shift+T without a time selection)
      const bool sel = a.playlist.selEnd > a.playlist.selStart;
      if (ImGui::MenuItem(i18n::tr("Add loop marker at the song position"), sel ? nullptr : keys::label("playlist.loopMarkers"))) addLoopMarker(a, pos);
    }
    if (ImGui::BeginMenu(i18n::tr("Add time signature marker"))) {
      const int sigs[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {12, 8}};
      for (const auto& g : sigs) {
        std::string l = std::to_string(g[0]) + "/" + std::to_string(g[1]);
        if (ImGui::MenuItem(l.c_str())) ops::addMarker(a.doc, pos, l, g[0], g[1]);
      }
      ImGui::EndMenu();
    }
    markerAddItems(a);  // J-11: at the time selection, every N bars, delete all
    j31::markerItems(a, pos);  // J-31: key markers
    const int nx = ops::nextMarker(s, pos), pv = ops::previousMarker(s, pos);
    if (ImGui::MenuItem(i18n::tr("Jump to next marker"), nullptr, false, nx >= 0)) j31::songJump(a, s.markers[static_cast<size_t>(nx)].pos);
    if (ImGui::MenuItem(i18n::tr("Jump to previous marker"), nullptr, false, pv >= 0)) j31::songJump(a, s.markers[static_cast<size_t>(pv)].pos);
    const bool hasSel = a.playlist.selEnd > a.playlist.selStart;
    {  // J-28: markers around the time selection (operations from J-27)
      const Tick s0 = a.playlist.selStart, s1 = a.playlist.selEnd;
      if (ImGui::MenuItem(i18n::tr("Add two (selection start and end)"), nullptr, false, s1 > s0)) ops::addMarkerPair(a.doc, s0, s1);
      if (ImGui::MenuItem(i18n::tr("Place loop"), keys::label("playlist.loopMarkers"), false, s1 > s0)) ops::placeLoopMarkers(a.doc, s0, s1);
      if (ImGui::MenuItem(i18n::tr("Add jump to next bar"))) ops::addJumpToNextBar(a.doc, pos);
    }
    if (ImGui::MenuItem(i18n::tr("Delete markers in the time selection"), nullptr, false, hasSel))
      ops::removeMarkersIn(a.doc, a.playlist.selStart, a.playlist.selEnd);
    if (!s.markers.empty()) ImGui::SeparatorText(i18n::tr("Markers"));
    for (size_t i = 0; i < s.markers.size(); ++i) {
      const Marker& mk = s.markers[i];
      std::string l = (mk.name.empty() ? std::string("(unnamed)") : mk.name) + "##mk" + std::to_string(i);
      if (ImGui::BeginMenu(l.c_str())) {
        markerMenu(a, static_cast<int>(i));  // J-11: rename, time signature, action, loop, move, delete
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Time selection"))) {
    chopui::timeSelectionItems(a);  // PL-13: duplicate, insert / delete space, consolidate, chop it, stutter
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Sort patterns"))) {
    if (ImGui::MenuItem(i18n::tr("By name"))) ops::sortPatterns(a.doc, ops::PatternSort::Name);
    if (ImGui::MenuItem(i18n::tr("By colour"))) ops::sortPatterns(a.doc, ops::PatternSort::Color);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Select"))) {
    if (ImGui::MenuItem(i18n::tr("Select all"), "Ctrl+A")) editSelectAll(a);
    if (ImGui::MenuItem(i18n::tr("Deselect"), "Ctrl+D")) editDeselect(a);
    if (ImGui::MenuItem(i18n::tr("Clips with the same source as the selection"), nullptr, false, any)) {
      std::set<ObjectId> out;
      for (ObjectId id : a.playlist.selected)
        for (ObjectId k : ops::clipsOfSameSource(s, id)) out.insert(k);
      a.playlist.selected = out;
    }
    {  // J-28: time selection from the clips / stepped by its own length (helpers from J-27)
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Time around the selected clips"), nullptr, false, any)) {
        const auto r = ops::clipsTimeRange(s, a.playlist.selected);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      const bool hasSel = a.playlist.selEnd > a.playlist.selStart;
      if (ImGui::MenuItem(i18n::tr("Previous time"), nullptr, false, hasSel && a.playlist.selStart > 0)) {
        const auto r = ops::shiftTimeSelection(a.playlist.selStart, a.playlist.selEnd, -1);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      if (ImGui::MenuItem(i18n::tr("Next time"), nullptr, false, hasSel)) {
        const auto r = ops::shiftTimeSelection(a.playlist.selStart, a.playlist.selEnd, 1);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      ImGui::Separator();
    }
    if (ImGui::MenuItem(i18n::tr("Muted clips"))) {
      a.playlist.selected.clear();
      for (const auto& c : s.playlist)
        if (c.muted) a.playlist.selected.insert(c.id);
    }
    if (ImGui::MenuItem(i18n::tr("Clips of the current pattern"))) {
      a.playlist.selected.clear();
      for (const auto& c : s.playlist)
        if (c.pattern == a.curPattern) a.playlist.selected.insert(c.id);
    }
    if (ImGui::MenuItem(i18n::tr("Invert selection"))) {
      std::set<ObjectId> inv;
      for (const auto& c : s.playlist)
        if (!a.playlist.selected.count(c.id)) inv.insert(c.id);
      a.playlist.selected = inv;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Snap"))) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::MenuItem(snapName(static_cast<Snap>(i)), nullptr, a.playlist.snap == static_cast<Snap>(i))) a.playlist.snap = static_cast<Snap>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Tool"))) {
    struct T { const char* name; const char* cmd; Tool tool; };
    const T list[] = {{i18n::N_("Draw"), "playlist.toolDraw", Tool::Draw},       {i18n::N_("Paint"), "playlist.toolPaint", Tool::Paint},
                      {i18n::N_("Delete"), "playlist.toolDelete", Tool::Erase},  {i18n::N_("Mute"), "playlist.toolMute", Tool::Mute},
                      {i18n::N_("Slip"), "playlist.toolSlip", Tool::Slip},       {i18n::N_("Slice"), "playlist.toolSlice", Tool::Slice},
                      {i18n::N_("Select"), "playlist.toolSelect", Tool::Select}, {i18n::N_("Zoom"), "playlist.toolZoom", Tool::Zoom},
                      {i18n::N_("Play"), "playlist.toolPlay", Tool::Play}};
    for (const auto& t : list)
      if (ImGui::MenuItem(i18n::tr(t.name), keys::label(t.cmd), a.playlist.tool == t.tool)) a.playlist.tool = t.tool;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Zoom"))) {  // J-28: FL's zoom menu (quick zooms, selection, fit)
    if (ImGui::MenuItem(i18n::tr("Zoom in"), "PgUp")) a.playlist.zoomRequest *= 1.5f;
    if (ImGui::MenuItem(i18n::tr("Zoom out"), "PgDn")) a.playlist.zoomRequest /= 1.5f;
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Quick zoom 1 (4 bars)"), keys::label("playlist.quickZoom1"))) a.playlist.quickZoom = 1;
    if (ImGui::MenuItem(i18n::tr("Quick zoom 2 (16 bars)"), keys::label("playlist.quickZoom2"))) a.playlist.quickZoom = 2;
    if (ImGui::MenuItem(i18n::tr("Quick zoom 3 (64 bars)"), keys::label("playlist.quickZoom3"))) a.playlist.quickZoom = 3;
    if (ImGui::MenuItem(i18n::tr("Zoom on selection"), keys::label("playlist.zoomSelection"), false, any || a.playlist.selEnd > a.playlist.selStart)) a.playlist.zoomSelRequest = true;
    if (ImGui::MenuItem(i18n::tr("Whole song"), "Home")) a.playlist.fitRequest = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("View"))) {
    ImGui::Separator();
    for (int pct : {50, 75, 100, 150, 200}) {
      std::string l = "Track height " + std::to_string(pct) + "%";
      float h = 46.0f * pct / 100.0f;
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(a.playlist.trackHeight - h) < 0.5f)) a.playlist.trackHeight = std::clamp(h, 24.0f, 120.0f);
    }
    // J-28: picker panel and audio clip editing aids (also in the header corner).
    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("Clip source picker"), keys::label("playlist.picker"), &a.playlist.pickerOpen);
    ImGui::MenuItem(i18n::tr("Wide track headers"), nullptr, &a.playlist.wideHeaders);
    ImGui::MenuItem(i18n::tr("Stretch audio clips when resizing"), keys::label("playlist.stretchResize"), &a.playlist.stretchResize);
    ImGui::MenuItem(i18n::tr("Fade and volume handles on audio clips"), keys::label("playlist.fades"), &a.playlist.showFades);
    ImGui::MenuItem(i18n::tr("Audio clip edits on zero crossings"), keys::label("playlist.zeroCross"), &a.playlist.zeroCross);
    {  // J-37
      const bool smoothSnap = a.settings.getBool("ui.smoothSnap");
      if (ImGui::MenuItem(i18n::tr("Smooth snapping"), nullptr, smoothSnap)) a.settings.set("ui.smoothSnap", !smoothSnap);
      ui::Hint("Snapped drags glide from grid cell to grid cell instead of jumping (positions still land exactly on the grid)");
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Add 8 tracks"), nullptr, false, static_cast<int>(s.tracks.size()) + 8 <= kMaxPlaylistTracks))
    a.doc.setPlaylistTrackCount(static_cast<int>(s.tracks.size()) + 8);
  if (ImGui::MenuItem(i18n::tr("Collapse all grouped tracks"))) ops::setAllGroupsFolded(a.doc, true);   // J-28 (operation: J-27)
  if (ImGui::MenuItem(i18n::tr("Expand all grouped tracks"))) ops::setAllGroupsFolded(a.doc, false);
  if (ImGui::MenuItem(i18n::tr("Delete unused patterns"))) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem(i18n::tr("Reset empty track names and colours"))) {
    a.doc.editSong("reset empty playlist tracks", [](Song& x) {
      for (size_t i = 0; i < x.tracks.size(); ++i) {
        bool used = false;
        for (const auto& c : x.playlist) used = used || c.track == static_cast<int>(i);
        if (!used) x.tracks[i] = PlaylistTrack{};
      }
    });
  }
}

// -------------------------------------------------------------- piano roll ----

// ------------------------------------------------------------------ markers ----

namespace {
const int kTimeSigs[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {3, 8}, {5, 8}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
}

// Playlist menu > Time markers.
void markerAddItems(App& a) {
  const Song& s = a.doc.song();
  if (ImGui::MenuItem(i18n::tr("Add marker at the time selection"), nullptr, false, a.playlist.selEnd > a.playlist.selStart))
    ops::addMarker(a.doc, a.playlist.selStart, ops::nextMarkerName(s));
  if (ImGui::BeginMenu(i18n::tr("Add a marker every"))) {
    const Tick bar = s.barTicks(), end = std::max(s.lengthTicks(), bar * 16);
    for (int b : {1, 2, 4, 8, 16})
      if (ImGui::MenuItem((std::to_string(b) + " " + (b == 1 ? i18n::tr("bar") : i18n::tr("bars"))).c_str())) ops::addMarkersEvery(a.doc, 0, end, bar * b);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Delete all markers"), nullptr, false, !s.markers.empty()))
    a.doc.editSong("delete markers", [](Song& x) { x.markers.clear(); });
}

void addLoopMarker(App& a, Tick pos) {
  const Tick grid = a.playlist.effGrid > 0 ? a.playlist.effGrid : snapTicks(a.doc.song(), a.playlist.snap);
  const Tick at = std::max<Tick>(0, grid > 0 ? timeline::snapTick(pos, grid, Snap::Line) : pos);
  const bool had = ops::songLoopMarker(a.doc.song()) >= 0;
  if (ops::addSongLoopMarker(a.doc, at) >= 0)
    a.notify(i18n::tr("Loop marker placed: at the song end playback goes on from it"));
  else if (had)
    a.notify(i18n::tr("Loop marker removed"));
}

void rulerMenu(App& a, Tick pos) {
  const Song& s = a.doc.song();
  if (ImGui::MenuItem(i18n::tr("Add loop marker here"))) addLoopMarker(a, pos);
  if (ImGui::MenuItem(i18n::tr("Add marker here"))) ops::addMarker(a.doc, pos, ops::nextMarkerName(s));
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Move the song position here"))) {
    if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
    j31::songJump(a, pos);
  }
}

// Right-click on a marker in the playlist ruler.
void markerMenu(App& a, int index) {
  const Song& s = a.doc.song();
  if (index < 0 || index >= static_cast<int>(s.markers.size())) {
    ImGui::CloseCurrentPopup();
    return;
  }
  const Marker& m = s.markers[static_cast<size_t>(index)];
  ImGui::TextDisabled("%s", m.name.empty() ? "Marker" : m.name.c_str());
  if (ImGui::MenuItem(i18n::tr("Jump here"))) {
    a.engine.setMode(PlayMode::Song);
    j31::songJump(a, m.pos);  // J-31: quantized when the option is on
  }
  if (ImGui::BeginMenu(i18n::tr("Rename..."))) {
    renameField(m.name, [&a, index](const std::string& n) { ops::editMarker(a.doc, index, "rename marker", [&](Marker& k) { k.name = n; }); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Time signature"))) {
    if (ImGui::MenuItem(i18n::tr("None"), nullptr, m.timeSigNum == 0))
      ops::editMarker(a.doc, index, "marker time signature", [](Marker& k) { k.timeSigNum = k.timeSigDen = 0; });
    for (const auto& ts : kTimeSigs) {
      std::string l = std::to_string(ts[0]) + "/" + std::to_string(ts[1]);
      if (ImGui::MenuItem(l.c_str(), nullptr, m.timeSigNum == ts[0] && m.timeSigDen == ts[1]))
        ops::editMarker(a.doc, index, "marker time signature", [&](Marker& k) { k.timeSigNum = ts[0], k.timeSigDen = ts[1]; });
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Action during playback"))) {
    // J-85: song loop (FL "loop") first after None, as in FL's action menu.
    static const MarkerAction order[] = {MarkerAction::None, MarkerAction::SongLoop, MarkerAction::Loop, MarkerAction::Skip, MarkerAction::Pause,
                                         MarkerAction::Stop};
    static const char* const names[] = {i18n::N_("None"), i18n::N_("Loop (from here at the song end)"), i18n::N_("Loop to the next marker"),
                                        i18n::N_("Skip to the next marker"), i18n::N_("Pause"), i18n::N_("Stop")};
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); ++k) {
      const auto act = order[k];
      if (ImGui::MenuItem(i18n::tr(names[k]), nullptr, m.action == act)) {
        if (act == MarkerAction::SongLoop) {  // one song loop marker: the earlier one becomes plain
          a.doc.editSong("marker action", [index](Song& x) {
            for (auto& o : x.markers)
              if (o.action == MarkerAction::SongLoop) o.action = MarkerAction::None;
            x.markers[static_cast<size_t>(index)].action = MarkerAction::SongLoop;
          });
        } else {
          ops::editMarker(a.doc, index, "marker action", [act](Marker& x) { x.action = act; });
        }
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Loop to the next marker"))) {
    auto r = ops::markerLoopRange(s, index);
    a.playlist.selStart = r.first;
    a.playlist.selEnd = r.second;
  }
  if (ImGui::MenuItem(i18n::tr("Move to the song position"))) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    ops::editMarker(a.doc, index, "move marker", [pos](Marker& k) { k.pos = pos; });
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Delete"))) ops::removeMarker(a.doc, index);
}

// J-11 multi-channel editing: the selected notes (or all) go to another channel.
void sendNotesItems(App& a, bool move) {
  bool ok = pianoHasTarget(a);
  if (!ImGui::BeginMenu(move ? i18n::tr("Move notes to channel") : i18n::tr("Copy notes to channel"), ok)) return;
  for (const auto& c : a.doc.song().channels) {
    if (c.id == a.selChannel) continue;
    if (!ImGui::MenuItem((c.name + "##to" + std::to_string(c.id)).c_str())) continue;
    auto notes = pianoNotes(a);
    auto sel = pianoSel(a, notes.size());
    const bool anySel = std::find(sel.begin(), sel.end(), true) != sel.end();
    std::vector<Note> send, keep;
    for (size_t i = 0; i < notes.size(); ++i) (!anySel || sel[i] ? send : keep).push_back(notes[i]);
    if (ops::sendNotesToChannel(a.doc, a.curPattern, a.selChannel, c.id, send, move, keep)) {
      a.notify(std::to_string(send.size()) + (move ? " notes moved to " : " notes copied to ") + c.name);
      if (move) a.piano.selected.clear();
    }
  }
  ImGui::EndMenu();
}

void pianoRollMenu(App& a) {
  a.editFocus = App::EditFocus::Piano;
  bool ok = pianoHasTarget(a);
  bool any = pianoSelectionCount(a) > 0;
  if (ImGui::BeginMenu(i18n::tr("File"))) {
    if (ImGui::MenuItem(i18n::tr("Import MIDI file..."), nullptr, false, ok))  // J-20: into this channel at the view start
      fileDialog(a, "Import MIDI file into the piano roll", false, {".mid", ".midi", ".rmi"}, "",
                 [&a](const std::string& p) { exportui::importMidiAtCursor(a, p); });
    if (ImGui::MenuItem(i18n::tr("Import MIDI file as new channels..."), nullptr, false, true))
      fileDialog(a, "Import MIDI file", false, {".mid", ".midi", ".rmi"}, "", [&a](const std::string& p) { exportui::importMidiFile(a, p); });
    if (ImGui::MenuItem(i18n::tr("Export pattern as MIDI..."), nullptr, false, ok)) exportui::exportMidiDialog(a, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Edit"))) {
    if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, ok)) editCut(a);
    if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, ok)) editCopy(a);
    if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, ok && canPaste(a))) editPaste(a);
    if (ImGui::MenuItem(i18n::tr("Duplicate"), "Ctrl+B", false, ok)) editDuplicate(a);
    if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, ok && any)) editDelete(a);
    ImGui::Separator();
    sendNotesItems(a, true);
    sendNotesItems(a, false);
    ImGui::Separator();
    menuparity::pianoEditItems(a);  // J-27
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Tools"))) {
    pianoToolItems(a);
    ImGui::Separator();
    menuparity::pianoToolItems(a);  // J-27
    ImGui::Separator();
    j31::pianoToolItems(a);  // J-31: chord progression editor, note groups
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Select"))) {
    pianoSelectItems(a);
    menuparity::pianoSelectItems(a);  // J-27
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Chord progression"))) {
    progressionItems(a, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Riff from progression"))) {
    progressionItems(a, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Scripts"))) {  // J-24: Lua piano roll scripts
    scriptsMenuItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Snap"))) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::MenuItem(snapName(static_cast<Snap>(i)), nullptr, a.piano.snap == static_cast<Snap>(i))) a.piano.snap = static_cast<Snap>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("View"))) {
    ImGui::MenuItem(i18n::tr("Ghost notes"), nullptr, &a.piano.ghostNotes);
    if (ImGui::BeginMenu(i18n::tr("Ghost channels"), a.piano.ghostNotes)) {
      if (ImGui::MenuItem(i18n::tr("All other channels"), nullptr, a.piano.ghostChannels.empty())) a.piano.ghostChannels.clear();
      ImGui::Separator();
      for (const auto& c : a.doc.song().channels) {
        if (c.id == a.selChannel) continue;
        bool on = a.piano.ghostChannels.count(c.id) > 0;
        if (ImGui::MenuItem((c.name + "##g" + std::to_string(c.id)).c_str(), nullptr, on)) {
          if (on)
            a.piano.ghostChannels.erase(c.id);
          else
            a.piano.ghostChannels.insert(c.id);
        }
      }
      ImGui::EndMenu();
    }
    ImGui::MenuItem(i18n::tr("Note names on every key"), nullptr, &a.piano.keyNames);
    if (ImGui::BeginMenu(i18n::tr("Scale highlighting"))) {
      static const char* const keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      if (ImGui::BeginMenu(i18n::tr("Root note"))) {
        for (int k = 0; k < 12; ++k)
          if (ImGui::MenuItem(keys[k], nullptr, a.piano.scaleRoot == k)) a.piano.scaleRoot = k;
        ImGui::EndMenu();
      }
      const auto& sc = tools::scales();
      if (ImGui::MenuItem(i18n::tr("Off"), nullptr, a.piano.scale == 0)) a.piano.scale = 0;
      for (size_t i = 1; i < sc.size(); ++i)
        if (ImGui::MenuItem(sc[i].name, nullptr, a.piano.scale == static_cast<int>(i))) a.piano.scale = static_cast<int>(i);
      ImGui::Separator();
      ImGui::MenuItem(i18n::tr("Snap notes to the scale"), nullptr, &a.piano.snapToScale);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Zoom in"))) a.piano.pxPerTick = std::min(4.0f, a.piano.pxPerTick * 1.25f);
    if (ImGui::MenuItem(i18n::tr("Zoom out"))) a.piano.pxPerTick = std::max(0.02f, a.piano.pxPerTick / 1.25f);
    if (ImGui::MenuItem(i18n::tr("Taller rows"))) a.piano.rowHeight = std::min(28.0f, a.piano.rowHeight + 2);
    if (ImGui::MenuItem(i18n::tr("Shorter rows"))) a.piano.rowHeight = std::max(7.0f, a.piano.rowHeight - 2);
    ImGui::Separator();
    menuparity::pianoViewItems(a);  // J-27
    j31::pianoViewItems(a);         // J-31: MIDI snap, editable ghosts, key markers, background waveform
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Tool"))) {
    struct T { const char* name; const char* cmd; Tool tool; };
    const T list[] = {{i18n::N_("Draw"), "piano.toolDraw", Tool::Draw},       {i18n::N_("Paint"), "piano.toolPaint", Tool::Paint},
                      {i18n::N_("Delete"), "piano.toolDelete", Tool::Erase},  {i18n::N_("Select"), "piano.toolSelect", Tool::Select},
                      {i18n::N_("Slice"), "piano.toolSlice", Tool::Slice},    {i18n::N_("Mute"), "piano.toolMute", Tool::Mute},
                      {i18n::N_("Play"), "piano.toolPlay", Tool::Play},       {i18n::N_("Zoom"), "piano.toolZoom", Tool::Zoom},
                      {i18n::N_("Detune"), "piano.toolDetune", Tool::Detune}};
    for (const auto& t : list)
      if (ImGui::MenuItem(i18n::tr(t.name), keys::label(t.cmd), a.piano.tool == t.tool)) a.piano.tool = t.tool;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Stamp"))) {
    if (ImGui::MenuItem(i18n::tr("Single notes"), nullptr, a.piano.stampChord < 0 && a.piano.stampAuto == 0)) a.piano.stampChord = -1, a.piano.stampAuto = 0;
    // J-27: chords from the highlighted scale (the clicked note is the bottom or the top one).
    if (ImGui::MenuItem(i18n::tr("Scale chord, clicked note at the bottom"), nullptr, a.piano.stampAuto == 1, a.piano.scale > 0))
      a.piano.stampAuto = 1, a.piano.stampChord = -1;
    if (ImGui::MenuItem(i18n::tr("Scale chord, clicked note on top"), nullptr, a.piano.stampAuto == 2, a.piano.scale > 0))
      a.piano.stampAuto = 2, a.piano.stampChord = -1;
    ImGui::MenuItem(i18n::tr("Just the next chord"), nullptr, &a.piano.stampOnce);
    ui::Hint("The stamp turns itself off after placing one chord");
    ImGui::Separator();
    const auto& ch = tools::chords();
    for (size_t i = 0; i < ch.size(); ++i)
      if (ImGui::MenuItem(ch[i].name, nullptr, a.piano.stampChord == static_cast<int>(i))) a.piano.stampChord = static_cast<int>(i), a.piano.stampAuto = 0;
    ImGui::Separator();
    static const char* const inv[] = {"Root position", "1st inversion", "2nd inversion", "3rd inversion"};
    for (int k = 0; k < 4; ++k)
      if (ImGui::MenuItem(inv[k], nullptr, a.piano.stampInversion == k)) a.piano.stampInversion = k;
    ImGui::EndMenu();
  }
}

// ------------------------------------------------------------------- mixer ----

// Mixer track menu (FL-style sections: track, routing, group, effects,
// layout, CPU). Items act on the selected tracks (mixerSelection) where that
// makes sense, on the clicked track otherwise.
void mixerTrackMenu(App& a, size_t index) {
  const Song& s = a.doc.song();
  if (index >= s.mixer.size()) return;
  const MixerTrack& t = s.mixer[index];
  const ObjectId id = t.id;
  const bool master = index == 0;
  a.selMixer = id;
  a.editFocus = App::EditFocus::Mixer;
  if (!a.mixerView.sel.count(id)) a.mixerView.sel = {id};
  const std::vector<ObjectId> selIds = mixerSelection(a);
  const bool many = selIds.size() > 1;
  std::vector<ObjectId> others;  // selected tracks besides this one
  for (ObjectId x : selIds)
    if (x != id) others.push_back(x);
  menuTitle((mixerTrackName(s, index) + (many ? "  (+" + std::to_string(selIds.size() - 1) + " selected)" : std::string())).c_str());
  auto report = [&a](int made, const std::string& err) {
    if (!err.empty()) a.notify(err, true);
    else if (made == 0) a.notify("Nothing to route: select other tracks first (Ctrl+click their names)");
  };

  // ---- mixer track ----
  if (ImGui::BeginMenu(i18n::tr("Rename, colour and icon"))) {
    if (ImGui::BeginMenu(i18n::tr("Rename..."), !master)) {
      renameField(t.name.empty() ? mixerTrackName(s, index) : t.name,
                  [&a, id](const std::string& n) { a.doc.editMixerTrack(id, "rename mixer track", [&](MixerTrack& m) { m.name = n; }); });
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Colour"))) {
      uint32_t col;
      if (colorGrid(&col)) mixops::setColor(a.doc, selIds, col);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Random colour"))) {
      a.doc.beginGroup("random colour");
      for (ObjectId x : selIds) mixops::setColor(a.doc, {x}, randomColor());
      a.doc.endGroup();
    }
    if (ImGui::MenuItem(i18n::tr("Gradient colour"), nullptr, false, selIds.size() >= 3)) mixops::gradientColor(a.doc, selIds);
    if (ImGui::BeginMenu(i18n::tr("Icon"))) {
      int ic = t.icon;
      if (trackicons::picker(t.icon, &ic)) mixops::setIcon(a.doc, selIds, ic);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (!master) trackui::mixerMenuItems(a, id);  // PL-08: audio track link
  else if (ImGui::MenuItem(i18n::tr("Master chain..."))) a.showMaster = true;  // RN-09
  if (ImGui::BeginMenu(i18n::tr("File"))) {
    if (ImGui::MenuItem(i18n::tr("Open mixer track state..."))) {
      fileDialog(a, "Open mixer track state", false, {mixops::kTrackStateExt}, "", [&a, id](const std::string& p) {
        std::string err;
        if (!mixops::loadTrackState(a.doc, id, p, &err)) a.notify(err, true);
      });
    }
    if (ImGui::MenuItem(i18n::tr("Save mixer track state as..."), "Ctrl+Shift+S")) {
      fileDialog(a, "Save mixer track state", true, {mixops::kTrackStateExt}, mixerTrackName(s, index), [&a, id](const std::string& path) {
        std::string p = path;
        if (fs::path(p).extension().empty()) p += mixops::kTrackStateExt;
        const MixerTrack* m = a.doc.song().mixerTrack(id);
        std::string err;
        if (m && mixops::saveTrackState(p, *m, &err)) a.notify("Saved " + fs::path(p).filename().string());
        else a.notify("Could not save the track state" + (err.empty() ? std::string() : ": " + err), true);
      });
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(many ? i18n::tr("Reset selected tracks to default") : i18n::tr("Reset track to default"))) mixops::resetTracks(a.doc, selIds);
  if (ImGui::MenuItem(t.muted ? i18n::tr("Unmute") : i18n::tr("Mute"))) a.doc.editMixerTrack(id, "mute mixer track", [](MixerTrack& m) { m.muted = !m.muted; });
  if (ImGui::MenuItem(i18n::tr("Disable track"), nullptr, t.disabled)) mixops::setDisabled(a.doc, selIds, !t.disabled);
  if (ImGui::MenuItem(mixops::isMono(t) ? i18n::tr("Make stereo") : i18n::tr("Make mono"))) mixops::setMono(a.doc, selIds, !mixops::isMono(t));
  if (ImGui::MenuItem(i18n::tr("Swap left and right"), nullptr, t.swapLR)) mixops::setSwap(a.doc, selIds, !t.swapLR);
  if (ImGui::MenuItem(i18n::tr("Reverse polarity"), nullptr, t.invertPolarity)) mixops::setPolarity(a.doc, selIds, !t.invertPolarity);
  if (ImGui::BeginMenu(i18n::tr("Stereo separation"))) {
    static const struct { const char* name; float v; } seps[] = {{i18n::N_("Merged (mono)"), -1.0f}, {i18n::N_("Narrow"), -0.5f}, {i18n::N_("Normal"), 0.0f}, {i18n::N_("Wide"), 0.5f}, {i18n::N_("Widest"), 1.0f}};
    for (const auto& sp : seps)
      if (ImGui::MenuItem(i18n::tr(sp.name), nullptr, t.stereoSep == sp.v)) a.doc.setParam(mixerParam(id, MixerParam::StereoSep), sp.v, false);
    ImGui::EndMenu();
  }

  // ---- routing ----
  ImGui::SeparatorText(i18n::tr("Routing"));
  auto chans = selChannelsInOrder(a);
  if (ImGui::BeginMenu(i18n::tr("Channel routing"))) {
    if (ImGui::MenuItem(i18n::tr("Route selected channels to this track"), keys::label("mixer.routeHere"), false, !chans.empty())) ops::routeChannels(a.doc, chans, id, false);
    if (ImGui::MenuItem(i18n::tr("Route selected channels starting from this track"), keys::label("mixer.routeFromHere"), false, !chans.empty() && !master))
      ops::routeChannels(a.doc, chans, id, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Track routing"))) {
    std::string err;
    ImGui::SeparatorText(i18n::tr("Selected tracks to this track"));
    if (ImGui::MenuItem(i18n::tr("Route selected to this track"), nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Send, &err), err);
    if (ImGui::MenuItem(i18n::tr("Route selected to this track only"), nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Only, &err), err);
    if (ImGui::MenuItem(i18n::tr("Sidechain selected to this track"), nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Sidechain, &err), err);
    if (ImGui::MenuItem(i18n::tr("Sidechain selected to this track only"), nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::SidechainOnly, &err), err);
    if (ImGui::MenuItem(i18n::tr("Create submix..."), nullptr, false, !selIds.empty() && !(selIds.size() == 1 && master))) {
      std::vector<ObjectId> src = selIds;
      menus::openPrompt("Submix name", "Submix", [&a, src](const std::string& n) {
        ObjectId sub = mixops::createSubmix(a.doc, src, n.empty() ? std::string("Submix") : n);
        if (sub) a.selMixer = sub, a.mixerView.sel = {sub};
        else a.notify("Could not create the submix", true);
      });
    }
    ImGui::SeparatorText(i18n::tr("This track to selected tracks"));
    if (ImGui::MenuItem(i18n::tr("Route this track to selected"), nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Send, &err), err);
    if (ImGui::MenuItem(i18n::tr("Route this track to selected only"), nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Only, &err), err);
    if (ImGui::MenuItem(i18n::tr("Sidechain this track to selected"), nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Sidechain, &err), err);
    if (ImGui::MenuItem(i18n::tr("Sidechain this track to selected only"), nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::SidechainOnly, &err), err);
    ImGui::EndMenu();
  }
  if (!master) {
    if (ImGui::BeginMenu(i18n::tr("Send to"))) {
      for (size_t i = 0; i < s.mixer.size(); ++i) {
        if (i == index) continue;
        ObjectId to = s.mixer[i].id;
        bool on = false;
        for (const auto& snd : t.sends) on = on || snd.to == to;
        const std::string why = on ? std::string() : mixops::routeProblem(s, id, to);
        std::string label = (i == 0 ? std::string("Master") : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##s" + std::to_string(i);
        if (ImGui::MenuItem(label.c_str(), nullptr, on, why.empty())) a.doc.setSend(id, to, on ? 0.0f : 1.0f);
        if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Route only to"))) {
      routeMenuItems(a, [&t](ObjectId to) { return t.sends.size() == 1 && t.sends[0].to == to; },
                     [&a, id](ObjectId to) {
                       std::string err;
                       if (!mixops::route(a.doc, {id}, {to}, mixops::RouteMode::Only, &err) && !err.empty()) a.notify(err, true);
                     });
      ImGui::EndMenu();
    }
  }
  if (ImGui::BeginMenu(i18n::tr("Select"))) {
    if (ImGui::MenuItem(i18n::tr("Channels routed to this track"), "Alt+L")) {
      a.selChannels.clear();
      for (const auto& c : s.channels)
        if (c.mixerTrack == id) a.selChannels.insert(c.id);
      if (!a.selChannels.empty()) a.selChannel = *a.selChannels.begin();
      a.showRack = true;
    }
    if (ImGui::MenuItem(i18n::tr("Tracks this one is routed to"))) {
      auto v = mixops::routedFrom(s, id);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem(i18n::tr("Tracks routed to this one"), "Ctrl+Alt+L")) {
      auto v = mixops::routedTo(s, id);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem(i18n::tr("Group"), nullptr, false, !t.group.empty())) {
      auto v = mixops::groupMembers(s, t.group);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem(i18n::tr("All"))) {
      a.mixerView.sel.clear();
      for (const auto& m : s.mixer) a.mixerView.sel.insert(m.id);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(many ? i18n::tr("Reset selected tracks' routing") : i18n::tr("Reset routing to master"), nullptr, false, !(master && !many)))
    mixops::resetRouting(a.doc, selIds);
  if (ImGui::MenuItem(i18n::tr("Unroute tracks routed to this track"), nullptr, false, !mixops::routedTo(s, id).empty()))
    mixops::unrouteIncoming(a.doc, id);
  if (ImGui::MenuItem(i18n::tr("Solo"), "S", t.solo)) ops::soloMixerTrack(a.doc, id);
  j31::mixerItems(a, id);  // J-31: alt solo, recording point
  {
    // Disk recording arm (J-8's MixerTrack::armed; toggleArm also picks an input).
    if (ImGui::MenuItem(many ? i18n::tr("Arm selected tracks") : i18n::tr("Arm for disk recording"), nullptr, t.armed && !many, !master))
      for (ObjectId x : selIds) {
        const MixerTrack* m = a.doc.song().mixerTrack(x);
        if (m && (many ? !m->armed : true)) toggleArm(a, x);
      }
    bool anyArmed = false;
    for (ObjectId x : selIds) anyArmed = anyArmed || (a.doc.song().mixerTrack(x) && a.doc.song().mixerTrack(x)->armed);
    if (ImGui::MenuItem(i18n::tr("Disarm selected tracks"), nullptr, false, anyArmed))
      for (ObjectId x : selIds)
        if (const MixerTrack* m = a.doc.song().mixerTrack(x); m && m->armed) toggleArm(a, x);
  }
  if (ImGui::MenuItem(i18n::tr("Render selected tracks to wave files..."), keys::label("mixer.renderSelected"))) renderMixerStems(a, selIds);

  // ---- group ----
  ImGui::SeparatorText(i18n::tr("Group"));
  if (ImGui::MenuItem(i18n::tr("Create group..."))) {
    std::vector<ObjectId> ids = selIds;
    menus::openPrompt("Group name", mixops::newGroupName(s), [&a, ids](const std::string& n) {
      if (!n.empty()) mixops::setGroup(a.doc, ids, n);
    });
  }
  const auto groups = mixops::groupNames(s);
  if (ImGui::BeginMenu(i18n::tr("Add to group"), !groups.empty())) {
    for (const auto& g : groups)
      if (ImGui::MenuItem(g.c_str(), nullptr, t.group == g)) mixops::setGroup(a.doc, selIds, g);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Remove from group"), nullptr, false, !t.group.empty())) mixops::setGroup(a.doc, selIds, "");
  if (ImGui::MenuItem(i18n::tr("Auto colour group"), nullptr, false, mixops::groupMembers(s, t.group).size() >= 3))
    mixops::gradientColor(a.doc, mixops::groupMembers(s, t.group));

  // ---- effects ----
  ImGui::SeparatorText(i18n::tr("Effects"));
  if (ImGui::BeginMenu(i18n::tr("Insert effect"))) {
    effectItems([&a, id](const std::string& type) {
      const MixerTrack* m = a.doc.song().mixerTrack(id);
      for (int k = 0; m && k < kEffectSlots; ++k)
        if (m->slots[static_cast<size_t>(k)].plugin.empty()) {
          a.doc.setSlotPlugin(id, k, type);
          a.fxEditor = {id, k};
          return;
        }
      a.notify("All effect slots are in use", true);
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Effect chain"))) {
    if (ImGui::MenuItem(i18n::tr("Copy effect chain"), "Ctrl+C")) editCopy(a);
    if (ImGui::MenuItem(i18n::tr("Paste effect chain"), "Ctrl+V", false, canPaste(a))) editPaste(a);
    if (ImGui::MenuItem(i18n::tr("Clear all effects"))) a.doc.editMixerTrack(id, "clear effects", [](MixerTrack& m) { m.slots.assign(kEffectSlots, EffectSlot{}); });
    // Effect-chain presets from the preset library (yours and imported ones).
    if (ImGui::BeginMenu(i18n::tr("Load effect chain"))) {
      static std::vector<PresetEntry> chains;
      if (ImGui::IsWindowAppearing()) chains = listChains(presetRoot(a.configDir()));
      if (chains.empty()) ImGui::TextDisabled("%s", i18n::tr("No saved chains"));
      std::string folder;
      bool open = true;
      for (const auto& e : chains) {
        if (e.folder != folder) {
          if (!folder.empty() && open) ImGui::EndMenu();
          folder = e.folder;
          open = folder.empty() || ImGui::BeginMenu(folder.c_str());
        }
        if (!open) continue;
        if (ImGui::MenuItem(e.name.c_str())) {
          ChainPreset c;
          if (loadChain(e.file, c))
            a.doc.editMixerTrack(id, "load effect chain", [&c](MixerTrack& m) {
              m.slots.assign(kEffectSlots, EffectSlot{});
              for (size_t k = 0; k < c.slots.size() && k < m.slots.size(); ++k) {
                m.slots[k].plugin = c.slots[k];
                if (k < c.mix.size()) m.slots[k].mix = c.mix[k];
              }
            });
        }
      }
      if (!folder.empty() && open) ImGui::EndMenu();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Save effect chain"))) {
      static char buf[96] = "My chain";
      ImGui::SetNextItemWidth(200);
      bool go = ImGui::InputText("##chain", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      go = ui::TextButton("Save", true) || go;
      if (go && buf[0]) {
        if (const MixerTrack* m = a.doc.song().mixerTrack(id)) {
          ChainPreset c;
          for (const auto& sl : m->slots) {
            c.slots.push_back(sl.plugin);
            c.mix.push_back(sl.mix);
          }
          a.notify(saveChain(presetRoot(a.configDir()), c, safePresetName(buf)) ? "Saved effect chain" : "Could not save the chain", false);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  // ---- layout ----
  ImGui::SeparatorText(i18n::tr("Layout"));
  if (ImGui::BeginMenu(i18n::tr("Dock to"), !master)) {
    static const char* docks[] = {i18n::N_("Left"), i18n::N_("Middle"), i18n::N_("Right")};
    for (int k = 0; k < 3; ++k)
      if (ImGui::MenuItem(i18n::tr(docks[k]), nullptr, t.dock == k - 1)) mixops::setDock(a.doc, selIds, static_cast<MixerDock>(k - 1));
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Move left"), keys::label("mixer.moveLeft"), false, index > 1)) ops::moveMixerTrack(a.doc, id, -1);
  if (ImGui::MenuItem(i18n::tr("Move right"), keys::label("mixer.moveRight"), false, !master && index + 1 < s.mixer.size())) ops::moveMixerTrack(a.doc, id, 1);
  if (ImGui::MenuItem(many ? i18n::tr("Delete selected tracks") : i18n::tr("Delete"), "Del", false, !master)) {
    a.doc.beginGroup("delete mixer tracks");
    for (ObjectId x : selIds)
      if (x != a.doc.song().masterId()) a.doc.removeMixerTrack(x);
    a.doc.endGroup();
    a.mixerView.sel.clear();
  }
  if (ImGui::MenuItem(i18n::tr("Insert one"))) {
    auto ids = mixops::insertTracks(a.doc, master ? 1 : static_cast<int>(index), 1);
    a.selMixer = ids.empty() ? kNoId : ids.front();
  }
  if (ImGui::MenuItem(i18n::tr("Insert multiple..."))) {
    const int at = master ? 1 : static_cast<int>(index);
    menus::openPrompt("Insert mixer tracks (how many?)", "4", [&a, at](const std::string& n) {
      auto ids = mixops::insertTracks(a.doc, at, std::atoi(n.c_str()));
      if (!ids.empty()) a.selMixer = ids.front();
    });
  }
  if (ImGui::MenuItem(i18n::tr("Insert from state..."))) {
    const int at = master ? 1 : static_cast<int>(index);
    fileDialog(a, "Insert mixer track from state", false, {mixops::kTrackStateExt}, "", [&a, at](const std::string& p) {
      a.doc.beginGroup("insert mixer track from state");
      auto ids = mixops::insertTracks(a.doc, at, 1);
      std::string err;
      const bool ok = !ids.empty() && mixops::loadTrackState(a.doc, ids.front(), p, &err);
      a.doc.endGroup();
      if (!ok) {
        a.doc.undo();
        a.notify(err.empty() ? std::string("Could not insert the track") : err, true);
      } else {
        a.selMixer = ids.front();
      }
    });
  }
  if (ImGui::MenuItem(i18n::tr("Trim unused tracks at the end"))) {
    int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? "Removed " + std::to_string(n) + " unused track(s)" : std::string("No unused tracks at the end"));
  }

  // ---- CPU ----
  ImGui::SeparatorText(i18n::tr("CPU"));
  if (ImGui::MenuItem(i18n::tr("Allow threaded processing"), nullptr, t.threaded)) mixops::setThreaded(a.doc, selIds, !t.threaded);
  if (ImGui::BeginMenu(i18n::tr("Track latency"))) {
    mixerLatencyMenu(a, id);
    ImGui::EndMenu();
  }
}

void mixerSlotMenu(App& a, ObjectId track, int slot) {
  const Song& s = a.doc.song();
  const MixerTrack* t = s.mixerTrack(track);
  if (!t || slot < 0 || slot >= kEffectSlots) return;
  const EffectSlot& es = t->slots[static_cast<size_t>(slot)];
  const PluginInfo* info = findPlugin(es.plugin.type);
  menuTitle(("Slot " + std::to_string(slot + 1) + "  \xC2\xB7  " + (info ? info->name : std::string("empty"))).c_str());
  if (ImGui::BeginMenu(info ? i18n::tr("Replace") : i18n::tr("Insert"))) {
    effectItems([&a, track, slot](const std::string& type) {
      a.doc.setSlotPlugin(track, slot, type);
      a.fxEditor = {track, slot};
    }, es.plugin.type);
    ImGui::EndMenu();
  }
  if (!info) return;
  if (ImGui::MenuItem(i18n::tr("Open editor"))) a.fxEditor = {track, slot};
  if (isHostedType(es.plugin.type) && ImGui::MenuItem(i18n::tr("Gridline panel (parameters, settings)"))) showHostedPanel(a, PluginTarget{track, slot});
  if (ImGui::MenuItem(i18n::tr("Bypass"), nullptr, !es.enabled)) ops::setSlotEnabled(a.doc, track, slot, !es.enabled);
  if (ImGui::BeginMenu(i18n::tr("Enable switch"))) {  // automation / remote control of the slot's on/off switch
    knobContextMenu(a, slotEnableParam(track, slot));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Mix"))) {
    knobContextMenu(a, slotMixParam(track, slot));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Presets and state"))) {
    pluginWindowMenu(a, track, slot);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, slot > 0)) a.doc.moveSlot(track, slot, slot - 1);
  if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, slot + 1 < kEffectSlots)) a.doc.moveSlot(track, slot, slot + 1);
  if (ImGui::BeginMenu(i18n::tr("Move to track"))) {
    for (size_t i = 0; i < s.mixer.size(); ++i) {
      if (s.mixer[i].id == track) continue;
      std::string label = (i == 0 ? std::string("Master") : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##mv" + std::to_string(i);
      int k = -1;
      for (int q = 0; q < kEffectSlots && k < 0; ++q)
        if (s.mixer[i].slots[static_cast<size_t>(q)].plugin.empty()) k = q;
      if (ImGui::MenuItem(label.c_str(), nullptr, false, k >= 0)) mixops::moveSlotTo(a.doc, track, slot, s.mixer[i].id, k, ImGui::GetIO().KeyCtrl);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Reset to default"))) {
    PluginState d = defaultPluginState(es.plugin.type);
    setPluginState(a, PluginTarget{track, slot}, d, "reset effect");
  }
  if (ImGui::MenuItem(i18n::tr("Remove"))) a.doc.setSlotPlugin(track, slot, "");
}

// -------------------------------------------------------------------- knob ----

void knobContextMenu(App& a, ParamId pid) {
  static double valueClip = 0;
  static bool hasClip = false;
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) return;
  if (ImGui::MenuItem(i18n::tr("Copy value"))) valueClip = d->toNormalized(*v), hasClip = true;
  if (ImGui::MenuItem(i18n::tr("Paste value"), nullptr, false, hasClip)) a.doc.setParam(pid, d->fromNormalized(valueClip), false);
  if (d->kind == ParamKind::Continuous && ImGui::BeginMenu(i18n::tr("Set to"))) {
    if (ImGui::MenuItem(i18n::tr("Minimum"))) a.doc.setParam(pid, d->min, false);
    if (ImGui::MenuItem(i18n::tr("Centre"))) a.doc.setParam(pid, d->fromNormalized(0.5), false);
    if (ImGui::MenuItem(i18n::tr("Maximum"))) a.doc.setParam(pid, d->max, false);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  automationMenuItems(a, pid);  // J-5: create / edit automation, edit events, record
  ImGui::Separator();
  remoteLinkMenuItems(a, pid);  // MIDI remote control (MidiUi.cpp)
  menuparity::knobItems(a, pid);  // J-27: tempo presets
}

// ------------------------------------------------------------ plugin window ----

namespace {
PluginState stateClip;

bool writePresetFile(const std::string& path, const PluginState& st) {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString("gridline-preset");
  j.object["type"] = Json::makeString(st.type);
  Json p = Json::makeArray();
  for (float v : st.params) p.array.push_back(Json::makeNumber(v));
  j.object["params"] = std::move(p);
  return writeFileAtomic(path, jsonStringify(j) + "\n");
}

bool readPresetFile(const std::string& path, const std::string& type, PluginState& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || j.text("type") != type) return false;
  out = defaultPluginState(type);
  if (const Json* p = j.find("params"); p && p->is(Json::Type::Array))
    for (size_t i = 0; i < p->array.size() && i < out.params.size(); ++i)
      if (p->array[i].is(Json::Type::Number)) out.params[i] = static_cast<float>(p->array[i].number);
  normalizePluginState(out);
  return true;
}
}  // namespace

void pluginWindowMenu(App& a, ObjectId owner, int slot) {
  const Song& s = a.doc.song();
  PluginTarget t{owner, slot};
  const PluginState* st = t.state(s);
  const PluginInfo* info = t.info(s);
  if (!st || !info) return;
  menuTitle(info->name.c_str());
  if (ImGui::BeginMenu(i18n::tr("Presets"), !info->presets.empty())) {
    for (size_t i = 0; i < info->presets.size(); ++i)
      if (ImGui::MenuItem(info->presets[i].name.c_str())) {
        if (isHostedType(info->type)) {  // the plugin's own programs
          selectHostedProgram(a, t, static_cast<int>(i));
          continue;
        }
        PluginState ns = presetState(*info, i);
        ns.data = st->data;
        setPluginState(a, t, ns, "load preset");
      }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Save preset to file..."))) {
    PluginState copy = *st;
    fileDialog(a, "Save preset", true, {".json"}, info->name + " preset.json", [&a, copy](const std::string& p) {
      std::string path = fs::path(p).extension().empty() ? p + ".json" : p;
      a.notify(writePresetFile(path, copy) ? "Saved preset " + fs::path(path).filename().string() : std::string("Could not save preset"),
               false);
    });
  }
  if (ImGui::MenuItem(i18n::tr("Load preset from file..."))) {
    std::string type = info->type;
    fileDialog(a, "Load preset", false, {".json"}, "", [&a, t, type](const std::string& p) {
      PluginState ns;
      const PluginState* cur = t.state(a.doc.song());
      if (!cur || !readPresetFile(p, type, ns)) {
        a.notify("That file is not a preset for this plugin", true);
        return;
      }
      ns.data = cur->data;
      setPluginState(a, t, ns, "load preset");
    });
  }
  if (ImGui::MenuItem(i18n::tr("Reset to default"))) {
    PluginState ns = defaultPluginState(info->type);
    ns.data = st->data;
    setPluginState(a, t, ns, "reset plugin");
  }
  if (ImGui::MenuItem(i18n::tr("Randomize"))) {
    static std::mt19937 rng(12345);
    PluginState ns = *st;
    for (size_t i = 0; i < ns.params.size() && i < info->params.size(); ++i) {
      const ParamDesc& d = info->params[i];
      double n = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
      // Keep levels moderate: output/volume-like parameters stay at their defaults.
      std::string nm = d.name;
      for (char& c : nm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (nm.find("volume") != std::string::npos || nm.find("output") != std::string::npos || nm.find("gain") != std::string::npos) continue;
      ns.params[i] = static_cast<float>(d.clamp(d.fromNormalized(n)));
    }
    setPluginState(a, t, ns, "randomize plugin");
  }
  if (ImGui::MenuItem(i18n::tr("Copy state"))) stateClip = *st;
  if (ImGui::MenuItem(i18n::tr("Paste state"), nullptr, false, stateClip.type == st->type)) {
    PluginState ns = stateClip;
    ns.data = st->data;
    setPluginState(a, t, ns, "paste plugin state");
  }
  ImGui::Separator();
  if (t.isChannel()) {
    const Channel* c = s.channel(owner);
    if (ImGui::MenuItem(i18n::tr("Piano roll"), keys::label("view.pianoRoll"))) a.openPianoRoll(owner);
    if (ImGui::BeginMenu(i18n::tr("Rename..."))) {
      renameField(c->name, [&a, owner](const std::string& n) { a.doc.renameChannel(owner, n); });
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Colour"))) {
      uint32_t col;
      if (colorGrid(&col)) ops::setChannelsColor(a.doc, {owner}, col);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Replace instrument"))) {
      instrumentItems([&a, owner](const std::string& type) { a.doc.setChannelPlugin(owner, type); }, info->type);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Route to a free mixer track"), keys::label("rack.routeFree"))) {
      auto tr = ops::routeToFreeMixerTracks(a.doc, {owner});
      if (!tr.empty()) a.selMixer = tr[0];
    }
    if (ImGui::MenuItem(i18n::tr("Clone channel"))) a.selectChannel(a.doc.cloneChannel(owner));
    if (ImGui::MenuItem(i18n::tr("Delete channel"))) a.doc.removeChannel(owner);
  } else {
    const MixerTrack* m = s.mixerTrack(owner);
    bool enabled = m && m->slots[static_cast<size_t>(slot)].enabled;
    if (ImGui::BeginMenu(i18n::tr("Replace effect"))) {
      effectItems([&a, owner, slot](const std::string& type) { a.doc.setSlotPlugin(owner, slot, type); }, info->type);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(enabled ? i18n::tr("Disable") : i18n::tr("Enable"))) ops::setSlotEnabled(a.doc, owner, slot, !enabled);
    if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, slot > 0)) {
      a.doc.moveSlot(owner, slot, slot - 1);
      a.fxEditor = {owner, slot - 1};
    }
    if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, slot + 1 < kEffectSlots)) {
      a.doc.moveSlot(owner, slot, slot + 1);
      a.fxEditor = {owner, slot + 1};
    }
    if (ImGui::MenuItem(i18n::tr("Remove effect"))) a.doc.setSlotPlugin(owner, slot, "");
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Close window"))) {
    if (t.isChannel())
      a.settingsChannel = kNoId;
    else
      a.fxEditor = {kNoId, -1};
  }
}

// ------------------------------------------------------------- dev screenshots ----

void drawDevMenu(App& a, const std::string& name) {
  static int frames = 0;
  if (frames++ == 2) ImGui::OpenPopup("##devmenu");
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 60, vp->WorkPos.y + 70));
  // Pinned position: keep a tall menu on screen (it scrolls, as a real popup clamped to the display does).
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(vp->WorkSize.x - 70, vp->WorkSize.y - 78));
  if (!ImGui::BeginPopup("##devmenu")) return;
  const Song& s = a.doc.song();
  ObjectId ch = a.selChannel, pat = a.curPattern;
  if (name == "file") menuFile(a);
  else if (name == "edit") menuEdit(a);
  else if (name == "add") menuAdd(a);
  else if (name == "patterns") menuPatterns(a);
  else if (name == "view") menuView(a);
  else if (name == "options") menuOptions(a);
  else if (name == "tools") menuTools(a);
  else if (name == "help") menuHelp(a);
  else if (name == "channel") channelContextMenu(a, ch);
  else if (name == "rack") rackOptionsMenu(a);
  else if (name == "pattern") patternContextMenu(a, pat);
  else if (name == "track") {  // the first audio track when there is one (PL-08 screenshots)
    int t = 0;
    for (int k = 0; k < static_cast<int>(a.doc.song().tracks.size()); ++k)
      if (trackmodes::modeOf(a.doc.song(), k) == TrackMode::Audio) {
        t = k;
        break;
      }
    playlistTrackMenu(a, t);
  }
  else if (name == "clip") { if (!s.playlist.empty()) clipContextMenu(a, s.playlist[0].id); }
  else if (name == "playlist") playlistOptionsMenu(a);
  else if (name == "piano") pianoRollMenu(a);
  else if (name == "pianotools") pianoToolItems(a);
  else if (name == "pianoselect") pianoSelectItems(a);
  else if (name == "marker") markerMenu(a, 0);
  else if (name == "markers") markerAddItems(a);
  else if (name == "mixer") mixerTrackMenu(a, s.mixer.size() > 1 ? 1 : 0);
  else if (name == "slot") {  // the first insert slot holding an effect (else insert 1, slot 1)
    ObjectId tr = s.mixer.size() > 1 ? s.mixer[1].id : kNoId;
    int sl = 0;
    bool found = false;
    for (size_t i = 1; i < s.mixer.size() && !found; ++i)
      for (int k = 0; k < kEffectSlots && !found; ++k)
        if (!s.mixer[i].slots[static_cast<size_t>(k)].plugin.empty()) tr = s.mixer[i].id, sl = k, found = true;
    if (tr != kNoId) mixerSlotMenu(a, tr, sl);
  }
  else if (name == "knob") knobContextMenu(a, channelParam(ch, ChannelParam::Volume));
  else if (name == "plugin") pluginWindowMenu(a, ch, -1);
  else if (name == "record") recordOptionsMenu(a);  // J-8 / J-17
  else if (name == "audioclip") {                   // J-8: first audio clip
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Audio) {
        audioClipMenu(a, c.id);
        break;
      }
  }
  else if (name == "chop") {  // PL-13: the Chop submenu of the last audio (else first) clip
    ObjectId id = s.playlist.empty() ? kNoId : s.playlist[0].id;
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Audio) id = c.id;
    if (id != kNoId) chopui::clipMenuItems(a, id);
  } else if (name == "timesel") chopui::timeSelectionItems(a);  // PL-13
  else if (name == "audiosource") {  // J-82: picker menu of the first audio clip source
    a.playlist.pickerTab = 1;
    for (const auto& c : s.channels)
      if (ops::isAudioClipSource(c)) {
        audioSourceMenu(a, c.id);
        break;
      }
  } else if (name == "autosource") {  // J-82: picker menu of the first automation clip
    a.playlist.pickerTab = 2;
    if (!s.automations.empty()) automationSourceMenu(a, s.automations[0].id);
  } else if (name == "patternsource") patternSourceMenu(a, pat);  // J-82
  else ImGui::TextDisabled(i18n::tr("Unknown menu: %s"), name.c_str());
  ImGui::EndPopup();
}

}  // namespace gl
