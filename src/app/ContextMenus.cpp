// Gridline — context menus (channel, pattern, playlist track/clip, piano roll,
// mixer track/slot, knob, plugin window) and the editing helpers they share
// with the main menu. Window files open these popups and call the bodies here.
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
#include "../packs/PacksUi.h"

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
  ImGui::TextDisabled("Enter to apply");
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
  if (ImGui::BeginPopupModal("##menuprompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::PushFont(app().fontBold, 0.0f);
    ImGui::TextUnformatted(prompt.title.c_str());
    ImGui::PopFont();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280);
    bool enter = ImGui::InputText("##p", prompt.buf, sizeof prompt.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ui::TextButton("OK", true, 80) || enter) {
      if (prompt.ok) prompt.ok(prompt.buf);
      prompt.ok = nullptr;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::TextButton("Cancel", false, 80) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
  const char* h = std::getenv("HOME");
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
  if (ImGui::MenuItem("Quick quantize", "Ctrl+Q", false, ok))
    pianoEdit(a, "quantize notes", [&](auto& n, auto& s) { ops::quantize(n, s, grid, false); });
  if (ImGui::MenuItem("Quantize starts and ends", nullptr, false, ok))
    pianoEdit(a, "quantize notes", [&](auto& n, auto& s) { ops::quantize(n, s, grid, true); });
  if (ImGui::MenuItem("Quantize...", "Alt+Q", false, ok)) openNoteTool(a, NoteTool::Quantize);
  if (ImGui::MenuItem("Quick legato", "Ctrl+L", false, ok)) pianoEdit(a, "legato", [](auto& n, auto& s) { ops::legato(n, s); });
  if (ImGui::MenuItem("Articulate...", "Alt+L", false, ok)) openNoteTool(a, NoteTool::Articulate);
  if (ImGui::MenuItem("Chop...", "Alt+U", false, ok)) openNoteTool(a, NoteTool::Chop);
  if (ImGui::BeginMenu("Quick chop", ok)) {
    if (ImGui::MenuItem("Into steps", "Ctrl+U")) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, step); });
    if (ImGui::MenuItem("Into half steps")) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, std::max<Tick>(1, step / 2)); });
    if (ImGui::MenuItem("Into beats")) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, a.doc.song().beatTicks()); });
    if (ImGui::MenuItem("Into snap grid")) pianoEdit(a, "chop notes", [&](auto& n, auto& s) { ops::chop(n, s, grid); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Glue", "Ctrl+G", false, ok)) pianoEdit(a, "glue notes", [](auto& n, auto& s) { ops::glue(n, s); });
  if (ImGui::MenuItem("Discard lengths", nullptr, false, ok))
    pianoEdit(a, "discard note lengths", [&](auto& n, auto& s) { ops::discardLengths(n, s, step); });
  ImGui::Separator();
  if (ImGui::MenuItem("Strum...", "Alt+S", false, ok)) openNoteTool(a, NoteTool::Strum);
  if (ImGui::MenuItem("Arpeggiate...", "Alt+A", false, ok)) openNoteTool(a, NoteTool::Arpeggiate);
  if (ImGui::MenuItem("Grace notes...", "Alt+G", false, ok)) openNoteTool(a, NoteTool::Flam);
  if (ImGui::BeginMenu("Quick strum", ok)) {
    for (int div : {1, 2, 4}) {
      std::string l = div == 1 ? "Wide (1/4 step)" : div == 2 ? "Medium (1/8 step)" : "Tight (1/16 step)";
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "strum", [&](auto& n, auto& s) { ops::strum(n, s, std::max<Tick>(1, step / (4 * div))); });
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Quick arpeggio", ok)) {
    if (ImGui::MenuItem("Steps")) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, step); });
    if (ImGui::MenuItem("Half steps")) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, std::max<Tick>(1, step / 2)); });
    if (ImGui::MenuItem("Beats")) pianoEdit(a, "arpeggiate", [&](auto& n, auto& s) { ops::arpeggiate(n, s, a.doc.song().beatTicks()); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Flip", ok)) {
    if (ImGui::MenuItem("Upside down (pitch)")) pianoEdit(a, "flip notes", [](auto& n, auto& s) { ops::flipVertical(n, s); });
    if (ImGui::MenuItem("Backwards (time)")) pianoEdit(a, "flip notes", [](auto& n, auto& s) { ops::flipHorizontal(n, s); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Randomize...", "Alt+R", false, ok)) openNoteTool(a, NoteTool::Randomize);
  if (ImGui::MenuItem("Riff generator...", nullptr, false, ok)) openNoteTool(a, NoteTool::Riff);
  if (ImGui::MenuItem("Humanize", "Alt+Shift+R", false, ok)) {
    uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 7;
    pianoEdit(a, "humanize", [&](auto& n, auto& s) { ops::humanize(n, s, 12, std::max<Tick>(1, step / 8), seed); });
  }
  if (ImGui::MenuItem("Note LFO...", "Alt+O", false, ok)) openNoteTool(a, NoteTool::Lfo);
  if (ImGui::MenuItem("Scale levels...", "Alt+X", false, ok)) openNoteTool(a, NoteTool::ScaleLevels);
  if (ImGui::MenuItem("Limit key range...", "Alt+K", false, ok)) openNoteTool(a, NoteTool::Limit);
  if (ImGui::MenuItem("Snap to the scale", nullptr, false, ok && a.piano.scale > 0)) {
    const int root = a.piano.scaleRoot, sc = a.piano.scale;
    pianoEdit(a, "snap notes to scale", [&](auto& n, auto& s) { tools::snapToScale(n, s, root, sc); });
  }
  if (ImGui::BeginMenu("Scale velocity", ok)) {
    for (int pct : {50, 75, 90, 110, 125, 150}) {
      std::string l = std::to_string(pct) + "%";
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "scale velocity", [&](auto& n, auto& s) { ops::scaleVelocity(n, s, pct / 100.0); });
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Note properties...", "Alt+N", false, ok)) openNoteTool(a, NoteTool::Properties);
  if (ImGui::BeginMenu("Mute", ok)) {
    if (ImGui::MenuItem("Mute")) pianoEdit(a, "mute notes", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 1); });
    if (ImGui::MenuItem("Unmute")) pianoEdit(a, "unmute notes", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 0); });
    if (ImGui::MenuItem("Toggle")) pianoEdit(a, "toggle note mute", [](auto& n, auto& s) { tools::setNotesMuted(n, s, 2); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Slide and portamento", ok)) {
    if (ImGui::MenuItem("Make slide notes")) pianoEdit(a, "slide notes", [](auto& n, auto& s) { tools::setNotesSlide(n, s, 1); });
    if (ImGui::MenuItem("Make normal notes")) pianoEdit(a, "slide notes off", [](auto& n, auto& s) { tools::setNotesSlide(n, s, 0); });
    ImGui::Separator();
    if (ImGui::MenuItem("Portamento on")) pianoEdit(a, "portamento on", [](auto& n, auto& s) { tools::setNotesPorta(n, s, 1); });
    if (ImGui::MenuItem("Portamento off")) pianoEdit(a, "portamento off", [](auto& n, auto& s) { tools::setNotesPorta(n, s, 0); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Colour group", ok)) {
    for (int k = 0; k < kNoteColors; ++k) {
      std::string l = "Group " + std::to_string(k + 1) + (k == 0 ? " (channel colour)" : "");
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "colour notes", [k](auto& n, auto& s) { tools::setNotesColor(n, s, k); });
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Transpose up", "Up", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, 1); });
  if (ImGui::MenuItem("Transpose down", "Down", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, -1); });
  if (ImGui::MenuItem("Octave up", "Shift+Up", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, 12); });
  if (ImGui::MenuItem("Octave down", "Shift+Down", false, ok)) pianoEdit(a, "transpose notes", [](auto& n, auto& s) { ops::transpose(n, s, -12); });
  if (ImGui::MenuItem("Nudge left", "Left", false, ok)) pianoEdit(a, "move notes", [&](auto& n, auto& s) { ops::nudge(n, s, -grid); });
  if (ImGui::MenuItem("Nudge right", "Right", false, ok)) pianoEdit(a, "move notes", [&](auto& n, auto& s) { ops::nudge(n, s, grid); });
}

void pianoSelectItems(App& a) {
  bool ok = pianoHasTarget(a);
  if (ImGui::MenuItem("Select all", "Ctrl+A", false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectAll(n, s); });
  if (ImGui::MenuItem("Deselect", "Ctrl+D", false, ok)) pianoEdit(a, "", [](auto&, auto& s) { ops::selectNone(s); });
  if (ImGui::MenuItem("Invert selection", "Shift+I", false, ok)) pianoEdit(a, "", [](auto&, auto& s) { ops::invertSelection(s); });
  if (ImGui::MenuItem("Every other note", nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectOdd(n, s); });
  if (ImGui::MenuItem("Same pitches as selection", nullptr, false, ok && pianoSelectionCount(a)))
    pianoEdit(a, "", [](auto& n, auto& s) { ops::selectSamePitch(n, s); });
  if (ImGui::MenuItem("Overlapping notes", nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { ops::selectOverlapping(n, s); });
  if (ImGui::MenuItem("Random notes", "Shift+R", false, ok)) {
    uint32_t seed = static_cast<uint32_t>(a.now * 1000.0) + 3;
    pianoEdit(a, "", [&](auto& n, auto& s) { ops::selectRandom(n, s, seed); });
  }
  if (ImGui::MenuItem("Muted notes", nullptr, false, ok)) pianoEdit(a, "", [](auto& n, auto& s) { tools::selectMuted(n, s); });
  if (ImGui::BeginMenu("Colour group", ok)) {
    for (int k = 0; k < kNoteColors; ++k) {
      std::string l = "Group " + std::to_string(k + 1);
      if (ImGui::MenuItem(l.c_str())) pianoEdit(a, "", [k](auto& n, auto& s) { tools::selectColor(n, s, k); });
    }
    ImGui::EndMenu();
  }
  const bool sc = a.piano.scale > 0;
  if (ImGui::MenuItem("Notes in the scale", nullptr, false, ok && sc))
    pianoEdit(a, "", [&a](auto& n, auto& s) { tools::selectInScale(n, s, a.piano.scaleRoot, a.piano.scale, true); });
  if (ImGui::MenuItem("Notes outside the scale", nullptr, false, ok && sc))
    pianoEdit(a, "", [&a](auto& n, auto& s) { tools::selectInScale(n, s, a.piano.scaleRoot, a.piano.scale, false); });
}

void progressionItems(App& a, bool riff) {
  bool ok = pianoHasTarget(a);
  static int root = 60;
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (ImGui::BeginMenu("Key")) {
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
  if (ImGui::MenuItem("Audio file as sampler...")) {
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
  if (ImGui::MenuItem("Piano roll", "F7")) a.openPianoRoll(id);
  if (ImGui::MenuItem("Channel settings")) a.settingsChannel = id;
  {  // J-7: the channel's sample in the audio editor
    const PluginInfo* pi = findPlugin(c->plugin.type);
    const bool hasSample = pi && pi->usesSample && !c->plugin.data.empty() && c->plugin.data[0] != '{' &&
                           c->plugin.data.find('\n') == std::string::npos;
    if (ImGui::MenuItem("Edit sample in audio editor", nullptr, false, hasSample)) packs::openAudioEditor(a, c->plugin.data, id);
    if (ImGui::MenuItem("Export sample as WAV...", nullptr, false, hasSample)) exportui::exportChannelSample(a, id);  // J-20
  }
  if (ImGui::MenuItem("Select in playlist")) {
    a.playlist.selected.clear();
    for (const auto& clip : s.playlist) {
      const Pattern* cp = s.pattern(clip.pattern);
      if (cp && (cp->steps.count(id) || cp->notes.count(id))) a.playlist.selected.insert(clip.id);
    }
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("Rename...", true)) {
    renameField(c->name, [&a, id](const std::string& n) { a.doc.renameChannel(id, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Colour")) {
    uint32_t col;
    if (colorGrid(&col)) ops::setChannelsColor(a.doc, {id}, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Random colour")) ops::setChannelsColor(a.doc, {id}, randomColor());
  if (ImGui::MenuItem(c->muted ? "Unmute" : "Mute")) ops::setChannelsMuted(a.doc, {id}, !c->muted);
  {
    const bool self = c->cutGroup > 0 && c->cutGroup == c->cutBy;
    if (ImGui::MenuItem("Cut itself", nullptr, self, !c->plugin.type.empty())) {
      if (self) a.doc.editChannel(id, "stop cutting itself", [](Channel& ch) { ch.cutGroup = ch.cutBy = 0; });
      else ops::cutItself(a.doc, {id});
    }
    ui::Hint("A new note stops this channel's previous note (monophonic choke)");
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("Replace instrument")) {
    instrumentItems([&a, id](const std::string& t) { a.doc.setChannelPlugin(id, t); }, c->plugin.type);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Insert instrument below")) {
    instrumentItems([&a, id](const std::string& t) {
      a.selectChannel(id);
      a.addInstrument(t);
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Route to mixer track")) {
    routeMenuItems(a, [c](ObjectId t) { return c->mixerTrack == t; }, [&a, id](ObjectId t) { a.doc.routeChannel(id, t); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Route to a free mixer track", "Ctrl+L")) {
    auto t = ops::routeToFreeMixerTracks(a.doc, {id});
    if (!t.empty()) a.selMixer = t[0];
  }
  ImGui::Separator();
  if (p) {
    if (ImGui::MenuItem("Fill every step")) ops::fillSteps(a.doc, p->id, id, 1);
    if (ImGui::MenuItem("Fill every 2 steps")) ops::fillSteps(a.doc, p->id, id, 2);
    if (ImGui::MenuItem("Fill every 4 steps")) ops::fillSteps(a.doc, p->id, id, 4);
    if (ImGui::MenuItem("Fill every 8 steps")) ops::fillSteps(a.doc, p->id, id, 8);
    if (ImGui::BeginMenu("Fill offbeats")) {
      if (ImGui::MenuItem("Every 4 from step 3")) ops::fillSteps(a.doc, p->id, id, 4, 2);
      if (ImGui::MenuItem("Every 8 from step 5")) ops::fillSteps(a.doc, p->id, id, 8, 4);
      if (ImGui::MenuItem("Every 2 from step 2")) ops::fillSteps(a.doc, p->id, id, 2, 1);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Advanced fill...")) openStepTool(a, StepTool::Fill, id);
    if (ImGui::MenuItem("Rotate left", "Ctrl+Shift+Left")) ops::rotateChannel(a.doc, p->id, id, -1);
    if (ImGui::MenuItem("Rotate right", "Ctrl+Shift+Right")) ops::rotateChannel(a.doc, p->id, id, 1);
    if (ImGui::BeginMenu("Steps")) {  // J-11 step tools
      const ObjectId pid = p->id;
      if (ImGui::MenuItem("Shift left")) ops::editStepRow(a.doc, pid, id, "shift steps left", [](auto& r, auto& pr) { tools::shiftSteps(r, pr, -1); });
      if (ImGui::MenuItem("Shift right")) ops::editStepRow(a.doc, pid, id, "shift steps right", [](auto& r, auto& pr) { tools::shiftSteps(r, pr, 1); });
      if (ImGui::MenuItem("Reverse")) ops::editStepRow(a.doc, pid, id, "reverse steps", [](auto& r, auto& pr) { tools::reverseSteps(r, pr); });
      if (ImGui::MenuItem("Invert")) ops::editStepRow(a.doc, pid, id, "invert steps", [](auto& r, auto&) { tools::invertSteps(r); });
      if (ImGui::MenuItem("Randomize...")) openStepTool(a, StepTool::Randomize, id);
      ImGui::Separator();
      if (ImGui::MenuItem("Insert a step at the start")) ops::editStepRow(a.doc, pid, id, "insert step", [](auto& r, auto& pr) { tools::insertStep(r, pr, 0); });
      if (ImGui::MenuItem("Delete the first step")) ops::editStepRow(a.doc, pid, id, "delete step", [](auto& r, auto& pr) { tools::deleteStep(r, pr, 0); });
      ImGui::TextDisabled("Insert / Delete over a step: insert or remove it");
      ImGui::Separator();
      if (ImGui::MenuItem("Reset step parameters", nullptr, false, p->stepProps.count(id) > 0))
        ops::editStepRow(a.doc, pid, id, "reset step parameters", [](auto&, auto& pr) { std::fill(pr.begin(), pr.end(), StepProps{}); });
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Graph editor", "Ctrl+K", a.rack.showGraph && a.rack.graphLane != static_cast<int>(tools::StepLane::Pitch)))
      a.rack.showGraph = true, a.rack.graphLane = 0, a.selectChannel(id);
    if (ImGui::MenuItem("Keyboard editor", nullptr, a.rack.showGraph && a.rack.graphLane == static_cast<int>(tools::StepLane::Pitch)))
      a.rack.showGraph = true, a.rack.graphLane = static_cast<int>(tools::StepLane::Pitch), a.selectChannel(id);
    const bool hasSteps = p->steps.count(id) > 0, hasNotes = p->notes.count(id) && !p->notes.at(id).empty();
    if (ImGui::MenuItem("Steps to piano roll", nullptr, false, hasSteps)) ops::convertStepsToNotes(a.doc, p->id, id);
    if (ImGui::MenuItem("Piano roll to steps", nullptr, false, hasNotes)) {
      ops::convertNotesToSteps(a.doc, p->id, id);
      if (const Pattern* q = a.doc.song().pattern(p->id); q && q->notes.count(id)) a.notify("Notes off the step grid stay in the piano roll");
    }
    if (ImGui::MenuItem("Clear steps and notes")) ops::clearChannel(a.doc, p->id, id);
    ImGui::Separator();
    a.editFocus = App::EditFocus::Rack;  // edit commands below act on the selected channel
    if (ImGui::MenuItem("Cut", "Ctrl+X")) editCut(a);
    if (ImGui::MenuItem("Copy", "Ctrl+C")) editCopy(a);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste(a))) editPaste(a);
  }
  ImGui::Separator();
  int idx = s.channelIndex(id);
  if (ImGui::MenuItem("Move up", "Alt+Up", false, idx > 0)) a.doc.moveChannel(id, idx - 1);
  if (ImGui::MenuItem("Move down", "Alt+Down", false, idx + 1 < static_cast<int>(s.channels.size()))) a.doc.moveChannel(id, idx + 1);
  if (ImGui::MenuItem("Clone", "Alt+C")) a.selectChannel(a.doc.cloneChannel(id));
  menuparity::channelItems(a, id);  // J-27
  j31::channelItems(a, id);         // J-31: step loop, key range, truncated swing, layer split
  if (ImGui::MenuItem(a.rack.zipped.count(id) ? "Unzip" : "Zip", "Alt+Z")) {
    if (a.rack.zipped.count(id))
      a.rack.zipped.erase(id);
    else
      a.rack.zipped.insert(id);
  }
  if (ImGui::MenuItem("Delete", "Alt+Del")) a.doc.removeChannel(id);
}

void rackOptionsMenu(App& a) {
  const Song& s = a.doc.song();
  auto sel = selChannelsInOrder(a);
  bool any = !sel.empty();
  if (ImGui::BeginMenu("Add channel")) {
    addChannelMenu(a);
    ImGui::EndMenu();
  }
  ImGui::SeparatorText("Selected channels");
  if (ImGui::MenuItem("Clone selected", "Alt+C", false, any)) {
    auto ids = ops::cloneChannels(a.doc, sel);
    if (!ids.empty()) a.selChannel = ids.back(), a.selChannels = std::set<ObjectId>(ids.begin(), ids.end());
  }
  if (ImGui::MenuItem("Delete selected", "Alt+Del", false, any)) ops::removeChannels(a.doc, sel);
  if (ImGui::MenuItem("Move selected up", "Alt+Up", false, any)) ops::moveChannels(a.doc, sel, -1);
  if (ImGui::MenuItem("Move selected down", "Alt+Down", false, any)) ops::moveChannels(a.doc, sel, 1);
  if (ImGui::BeginMenu("Colour selected", any)) {
    uint32_t col;
    if (colorGrid(&col)) ops::setChannelsColor(a.doc, sel, col);
    ImGui::Separator();
    if (ImGui::MenuItem("Gradient (first to last)", nullptr, false, sel.size() >= 3)) ops::gradientColor(a.doc, sel);
    if (ImGui::MenuItem("Random", "Ctrl+Alt+C")) {
      a.doc.beginGroup("random colours");
      for (ObjectId c : sel) ops::setChannelsColor(a.doc, {c}, randomColor());
      a.doc.endGroup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Mute selected", nullptr, false, any)) ops::setChannelsMuted(a.doc, sel, true);
  if (ImGui::MenuItem("Unmute selected", nullptr, false, any)) ops::setChannelsMuted(a.doc, sel, false);
  if (ImGui::BeginMenu("Transpose selected", any)) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposeChannels(a.doc, a.curPattern, sel, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Swing mix for selected", any)) {
    for (int pct : {0, 25, 50, 75, 100}) {
      std::string l = std::to_string(pct) + "% of the song swing";
      if (ImGui::MenuItem(l.c_str())) ops::setSwingMix(a.doc, sel, pct / 100.0f);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Cut itself (selected)", nullptr, false, any)) ops::cutItself(a.doc, sel);
  if (ImGui::MenuItem("Create layer with selected channels", nullptr, false, any)) {
    ObjectId l = ops::createLayer(a.doc, sel);
    if (l) a.selectChannel(l);
    else a.notify("Layers can only hold instrument channels", true);
  }
  if (ImGui::BeginMenu("Fill selected", any)) {
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
  if (ImGui::MenuItem("Create layer from selected", nullptr, false, any)) {
    ObjectId l = ops::createLayer(a.doc, sel);
    if (l) a.selectChannel(l), a.notify("Layer created: its notes play on " + std::to_string(sel.size()) + " channel(s)");
  }
  if (ImGui::MenuItem("Move selected to a new pattern", nullptr, false, any)) {
    ObjectId np = ops::channelsToNewPattern(a.doc, a.curPattern, sel, true);
    if (np) a.selectPattern(np);
  }
  if (ImGui::MenuItem("Copy selected to a new pattern", nullptr, false, any)) {
    ObjectId np = ops::channelsToNewPattern(a.doc, a.curPattern, sel, false);
    if (np) a.selectPattern(np);
  }
  if (ImGui::BeginMenu("Swing mix for selected", any)) {
    for (int pctv : {0, 25, 50, 75, 100})
      if (ImGui::MenuItem((std::to_string(pctv) + "%").c_str())) ops::setSwingMix(a.doc, sel, pctv / 100.0f);
    ImGui::EndMenu();
  }
  menuparity::rackItems(a, sel);  // J-27
  j31::rackItems(a, sel);         // J-31: step loops, truncated swing
  if (ImGui::MenuItem("Zip selected", "Alt+Z", false, any))
    for (ObjectId c : sel) a.rack.zipped.insert(c);
  if (ImGui::MenuItem("Unzip all", "Alt+U", false, !a.rack.zipped.empty())) a.rack.zipped.clear();
  if (ImGui::MenuItem("Route selected to free mixer tracks", "Ctrl+L", false, any)) {
    auto t = ops::routeToFreeMixerTracks(a.doc, sel);
    if (!t.empty()) a.selMixer = t.back();
  }
  if (ImGui::MenuItem("Select their mixer tracks", nullptr, false, any))
    if (const Channel* c = s.channel(sel.front())) a.selMixer = c->mixerTrack, a.showMixer = true;
  ImGui::SeparatorText("All channels");
  if (ImGui::BeginMenu("Sort by")) {
    if (ImGui::MenuItem("Name")) ops::sortChannels(a.doc, ops::ChannelSort::Name);
    if (ImGui::MenuItem("Colour")) ops::sortChannels(a.doc, ops::ChannelSort::Color);
    if (ImGui::MenuItem("Mixer track")) ops::sortChannels(a.doc, ops::ChannelSort::MixerTrack);
    if (ImGui::MenuItem("Instrument")) ops::sortChannels(a.doc, ops::ChannelSort::Instrument);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Select all", "Ctrl+A")) {
    a.selChannels.clear();
    for (const auto& c : s.channels) a.selChannels.insert(c.id);
  }
  if (ImGui::MenuItem("Select unused channels")) {
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
  if (ImGui::BeginMenu("Rename...")) {
    renameField(p->name, [&a, id](const std::string& n) { a.doc.renamePattern(id, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Colour")) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, id, col);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Random colour")) ops::setPatternColor(a.doc, id, randomColor());
  if (ImGui::MenuItem("Select in playlist")) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.pattern == id) a.playlist.selected.insert(c.id);
  }
  ImGui::Separator();
  if (ImGui::BeginMenu("Length")) {
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
  if (ImGui::BeginMenu("Transpose")) {
    for (int st : {12, 1, -1, -12}) {
      std::string l = (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 12 ? " (octave)" : " semitone");
      if (ImGui::MenuItem(l.c_str())) ops::transposePattern(a.doc, id, st);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Split by channel")) {
    auto ids = ops::splitPatternByChannel(a.doc, id);
    if (ids.empty())
      a.notify("Nothing to split: the pattern is empty");
    else
      a.notify("Split into " + std::to_string(ids.size()) + " patterns");
  }
  ImGui::Separator();
  int idx = s.patternIndex(id);
  if (ImGui::MenuItem("Insert one before", "Ctrl+Shift+Ins")) a.selectPattern(ops::insertPattern(a.doc, id, ops::nextPatternName(s)));
  if (ImGui::MenuItem("Clone", "Ctrl+Shift+C")) a.selectPattern(a.doc.clonePattern(id));
  if (ImGui::MenuItem("Move up", "Ctrl+Shift+Up", false, idx > 0)) ops::movePattern(a.doc, id, idx - 1);
  if (ImGui::MenuItem("Move down", "Ctrl+Shift+Down", false, idx + 1 < static_cast<int>(s.patterns.size()))) ops::movePattern(a.doc, id, idx + 1);
  if (ImGui::MenuItem("Delete", "Ctrl+Shift+Del", false, s.patterns.size() > 1)) a.doc.removePattern(id);
}

// ---------------------------------------------------------------- playlist ----

void playlistTrackMenu(App& a, int track) {
  const Song& s = a.doc.song();
  int n = static_cast<int>(s.tracks.size());
  if (track < 0 || track >= n) return;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  menuTitle(playlistTrackName(s, static_cast<size_t>(track)).c_str());
  if (ImGui::BeginMenu("Rename...")) {
    renameField(t.name, [&a, track](const std::string& nm) {
      a.doc.editPlaylistTrack(track, "rename playlist track", [&](PlaylistTrack& x) { x.name = nm; });
    });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Colour")) {
    uint32_t col;
    if (colorGrid(&col)) a.doc.editPlaylistTrack(track, "change track colour", [&](PlaylistTrack& x) { x.color = col; });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Random colour")) {
    uint32_t col = randomColor();
    a.doc.editPlaylistTrack(track, "change track colour", [&](PlaylistTrack& x) { x.color = col; });
  }
  if (ImGui::BeginMenu("Live settings")) {
    liveTrackMenu(a, track);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Group")) {
    for (int k = 1; k <= 16 && track + k < n; ++k) {
      std::string label = "Hold the next " + std::to_string(k) + (k == 1 ? " track" : " tracks");
      if (ImGui::MenuItem(label.c_str(), nullptr, t.groupSize == k))
        a.doc.editPlaylistTrack(track, "group playlist tracks", [k](PlaylistTrack& x) { x.groupSize = k; });
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Fold", nullptr, t.folded, t.groupSize > 0))
      a.doc.editPlaylistTrack(track, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
    if (ImGui::MenuItem("Ungroup", nullptr, false, t.groupSize > 0))
      a.doc.editPlaylistTrack(track, "ungroup playlist tracks", [](PlaylistTrack& x) { x.groupSize = 0, x.folded = false; });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Name after its first clip")) {
    const Clip* first = nullptr;
    for (const auto& c : s.playlist)
      if (c.track == track && (!first || c.start < first->start)) first = &c;
    if (first)
      if (const Pattern* p = s.pattern(first->pattern)) {
        std::string nm = p->name;
        a.doc.editPlaylistTrack(track, "auto name track", [&](PlaylistTrack& x) { x.name = nm; });
      }
  }
  if (ImGui::MenuItem("Reset name and colour")) a.doc.editPlaylistTrack(track, "reset track", [](PlaylistTrack& x) { x = PlaylistTrack{}; });
  ImGui::Separator();
  if (ImGui::MenuItem("Mute", nullptr, t.muted))
    a.doc.editPlaylistTrack(track, "mute playlist track", [](PlaylistTrack& x) { x.muted = !x.muted; });
  if (ImGui::MenuItem("Lock", nullptr, t.locked)) ops::setTrackLocked(a.doc, track, !t.locked);
  if (ImGui::BeginMenu("Size")) {
    for (int pct : {33, 50, 100, 150, 200}) {
      const float h = pct / 100.0f;
      if (ImGui::MenuItem((std::to_string(pct) + "%").c_str(), nullptr, std::fabs(t.height - h) < 0.02f)) ops::setTrackHeights(a.doc, {track}, h);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Give every track this size")) {
      std::vector<int> all;
      for (int k = 0; k < n; ++k) all.push_back(k);
      ops::setTrackHeights(a.doc, all, t.height);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Group with the track above", nullptr, false, track > 0)) {
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
    if (ImGui::MenuItem("Auto colour group", nullptr, false, grouped)) ops::autoColorTrackGroup(a.doc, track);
    if (ImGui::MenuItem("Collapse grouped tracks", nullptr, false, grouped)) {
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
  if (ImGui::MenuItem("Mute all clips")) ops::muteTrackClips(a.doc, track, true);
  if (ImGui::MenuItem("Unmute all clips")) ops::muteTrackClips(a.doc, track, false);
  if (ImGui::MenuItem("Merge pattern clips")) {
    ObjectId nc = ops::mergeTrackPatternClips(a.doc, track);
    if (nc) a.playlist.selected = {nc};
    else a.notify("Merging needs at least two pattern clips on the track");
  }
  if (ImGui::MenuItem("Select all clips on track")) {
    a.playlist.selected.clear();
    for (const auto& c : s.playlist)
      if (c.track == track) a.playlist.selected.insert(c.id);
  }
  if (ImGui::MenuItem("Delete all clips on track")) ops::clearPlaylistTrack(a.doc, track);
  ImGui::Separator();
  if (ImGui::MenuItem("Insert one above", nullptr, false, n < kMaxPlaylistTracks)) ops::insertPlaylistTrack(a.doc, track);
  if (ImGui::MenuItem("Clone", nullptr, false, n < kMaxPlaylistTracks)) ops::clonePlaylistTrack(a.doc, track);
  if (ImGui::MenuItem("Move up", nullptr, false, track > 0)) ops::movePlaylistTrack(a.doc, track, -1);
  if (ImGui::MenuItem("Move down", nullptr, false, track + 1 < n)) ops::movePlaylistTrack(a.doc, track, 1);
  if (ImGui::MenuItem("Delete track", nullptr, false, n > 1)) ops::deletePlaylistTrack(a.doc, track);
  if (ImGui::MenuItem("Add 8 tracks", nullptr, false, n + 8 <= kMaxPlaylistTracks)) a.doc.setPlaylistTrackCount(n + 8);
}

void clipContextMenu(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  const Pattern* p = s.pattern(c->pattern);
  if (!p) return;
  menuTitle(p->name.c_str());
  if (ImGui::MenuItem("Edit pattern")) {
    a.selectPattern(p->id);
    a.showPiano = true;
    a.focusWindow = "Piano roll";
  }
  if (ImGui::MenuItem("Select all clips of this pattern")) a.playlist.selected = ops::clipsOfSameSource(s, clip);
  if (ImGui::BeginMenu("Rename pattern...")) {
    ObjectId pid = p->id;
    renameField(p->name, [&a, pid](const std::string& n) { a.doc.renamePattern(pid, n); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Pattern colour")) {
    uint32_t col;
    if (colorGrid(&col)) ops::setPatternColor(a.doc, p->id, col);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Make unique")) {
    ObjectId np = ops::makeClipUnique(a.doc, clip);
    if (np) a.selectPattern(np);
  }
  Tick patLen = s.patternContentLength(*p), len = s.clipLength(*c);
  if (ImGui::MenuItem("Split at pattern repeats", nullptr, false, patLen > 0 && len > patLen)) ops::chopClip(a.doc, clip, patLen);
  {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem("Slice at the song position", nullptr, false, pos > c->start && pos < c->start + len)) ops::sliceClip(a.doc, clip, pos);
  }
  if (ImGui::BeginMenu("Chop into pieces")) {
    const Tick bar = s.barTicks(), beat = s.beatTicks();
    struct Ch { const char* name; Tick t; };
    const Ch list[] = {{"Every 4 bars", bar * 4}, {"Every 2 bars", bar * 2}, {"Every bar", bar}, {"Every 2 beats", beat * 2},
                       {"Every beat", beat}, {"Every step", std::max<Tick>(1, s.ppq / 4)}};
    for (const auto& k : list)
      if (ImGui::MenuItem(k.name, nullptr, false, k.t < len)) {
        auto ids = ops::chopClips(a.doc, {clip}, k.t);
        a.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end());
      }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Split by channel")) {
    auto ids = ops::splitClipByChannel(a.doc, clip);
    a.notify(ids.empty() ? std::string("Nothing to split") : "Split into " + std::to_string(ids.size()) + " clips");
  }
  if (ImGui::MenuItem("Fit length to pattern", nullptr, false, c->length != 0)) a.doc.setClipLength(clip, 0);
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(clip)) sel = {clip};
  if (ImGui::MenuItem("Merge selected clips", "Ctrl+G", false, sel.size() >= 2)) {
    ObjectId nc = ops::mergeClips(a.doc, sel);
    if (nc) a.playlist.selected = {nc};
  }
  if (ImGui::MenuItem("Duplicate", "Ctrl+B")) {
    auto ids = ops::duplicateClips(a.doc, sel);
    a.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end());
  }
  if (ImGui::MenuItem("Delete", "Del")) {
    ops::removeClips(a.doc, sel);
    a.playlist.selected.clear();
  }
}

void playlistOptionsMenu(App& a) {
  const Song& s = a.doc.song();
  a.editFocus = App::EditFocus::Playlist;
  bool any = !a.playlist.selected.empty();
  j31::playlistItems(a);  // J-31: clip groups
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, any)) editCut(a);
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, any)) editCopy(a);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste(a))) editPaste(a);
    if (ImGui::MenuItem("Duplicate", "Ctrl+B", false, any)) editDuplicate(a);
    if (ImGui::MenuItem("Delete", "Del", false, any)) editDelete(a);
    ImGui::Separator();
    if (ImGui::MenuItem("Merge selected clips", "Ctrl+G", false, a.playlist.selected.size() >= 2)) {
      ObjectId nc = ops::mergeClips(a.doc, a.playlist.selected);
      if (nc) a.playlist.selected = {nc};
    }
    if (ImGui::MenuItem("Mute / unmute selected clips", "M", false, any)) ops::muteClips(a.doc, a.playlist.selected, 2);
    if (ImGui::BeginMenu("Chop selected clips", any)) {
      const Tick bar = s.barTicks(), beat = s.beatTicks();
      if (ImGui::MenuItem("Every bar")) ops::chopClips(a.doc, a.playlist.selected, bar);
      if (ImGui::MenuItem("Every 2 beats")) ops::chopClips(a.doc, a.playlist.selected, beat * 2);
      if (ImGui::MenuItem("Every beat")) ops::chopClips(a.doc, a.playlist.selected, beat);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Slice at the song position", nullptr, false, true)) {  // J-28: every clip kind, audio included
      const Tick at = static_cast<Tick>(a.engine.position());
      const auto ids = pltools::clipsAcross(s, at, 0, static_cast<int>(s.tracks.size()) - 1);
      if (!ids.empty()) a.doc.editSong("slice clips", [&](Song& x) { pltools::sliceClips(x, ids, at); });
    }
    {  // J-28: audio and automation clip items (operations from J-27)
      bool audioSel = false, autoSel = false;
      for (const auto& c : s.playlist)
        if (a.playlist.selected.count(c.id)) audioSel = audioSel || c.kind == ClipKind::Audio, autoSel = autoSel || c.kind == ClipKind::Automation;
      ImGui::Separator();
      if (ImGui::MenuItem("Pitch selected audio clips a semitone up", nullptr, false, audioSel)) ops::pitchAudioClips(a.doc, a.playlist.selected, 100.0f);
      if (ImGui::MenuItem("Pitch selected audio clips a semitone down", nullptr, false, audioSel)) ops::pitchAudioClips(a.doc, a.playlist.selected, -100.0f);
      if (ImGui::MenuItem("Reverse selected audio clips", nullptr, false, audioSel)) ops::reverseAudioClips(a.doc, a.playlist.selected);
      if (ImGui::MenuItem("Merge selected automation clips", nullptr, false, autoSel)) {
        ObjectId nc = ops::mergeAutomationClips(a.doc, a.playlist.selected);
        if (nc) a.playlist.selected = {nc};
        else a.notify("Merging needs two automation clips of the same parameter");
      }
    }
    if (ImGui::MenuItem("Make selected clips unique", nullptr, false, any)) {
      a.doc.beginGroup("make clips unique");
      for (ObjectId id : a.playlist.selected) ops::makeClipUnique(a.doc, id);
      a.doc.endGroup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Arrangements")) {
    arrangementMenu(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Live area")) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem("End it at the time selection end", nullptr, false, a.playlist.selEnd > a.playlist.selStart))
      setLiveArea(a, a.playlist.selEnd);
    if (ImGui::MenuItem("End it at the song position", nullptr, false, pos > 0)) setLiveArea(a, pos);
    if (ImGui::MenuItem("Remove the live area", nullptr, false, s.live.areaEnd > 0)) setLiveArea(a, 0);
    ImGui::Separator();
    ImGui::MenuItem("Live pads window", nullptr, &a.showLivePads);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Time markers")) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem("Add marker at the song position")) ops::addMarker(a.doc, pos, ops::nextMarkerName(s));
    if (ImGui::BeginMenu("Add time signature marker")) {
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
    if (ImGui::MenuItem("Jump to next marker", nullptr, false, nx >= 0)) j31::songJump(a, s.markers[static_cast<size_t>(nx)].pos);
    if (ImGui::MenuItem("Jump to previous marker", nullptr, false, pv >= 0)) j31::songJump(a, s.markers[static_cast<size_t>(pv)].pos);
    const bool hasSel = a.playlist.selEnd > a.playlist.selStart;
    {  // J-28: markers around the time selection (operations from J-27)
      const Tick s0 = a.playlist.selStart, s1 = a.playlist.selEnd;
      if (ImGui::MenuItem("Add two (selection start and end)", nullptr, false, s1 > s0)) ops::addMarkerPair(a.doc, s0, s1);
      if (ImGui::MenuItem("Place loop", "Shift+T", false, s1 > s0)) ops::placeLoopMarkers(a.doc, s0, s1);
      if (ImGui::MenuItem("Add jump to next bar")) ops::addJumpToNextBar(a.doc, pos);
    }
    if (ImGui::MenuItem("Delete markers in the time selection", nullptr, false, hasSel))
      ops::removeMarkersIn(a.doc, a.playlist.selStart, a.playlist.selEnd);
    if (!s.markers.empty()) ImGui::SeparatorText("Markers");
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
  if (ImGui::BeginMenu("Time selection")) {
    const bool hasSel = a.playlist.selEnd > a.playlist.selStart;
    if (ImGui::MenuItem("Insert space", nullptr, false, hasSel))
      ops::insertSpace(a.doc, a.playlist.selStart, a.playlist.selEnd - a.playlist.selStart);
    ui::Hint("Moves everything from the selection start right by the selection length");
    if (ImGui::MenuItem("Delete space", nullptr, false, hasSel)) ops::deleteSpace(a.doc, a.playlist.selStart, a.playlist.selEnd);
    ui::Hint("Removes the selected time and closes the gap");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Sort patterns")) {
    if (ImGui::MenuItem("By name")) ops::sortPatterns(a.doc, ops::PatternSort::Name);
    if (ImGui::MenuItem("By colour")) ops::sortPatterns(a.doc, ops::PatternSort::Color);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Select")) {
    if (ImGui::MenuItem("Select all", "Ctrl+A")) editSelectAll(a);
    if (ImGui::MenuItem("Deselect", "Ctrl+D")) editDeselect(a);
    if (ImGui::MenuItem("Clips with the same source as the selection", nullptr, false, any)) {
      std::set<ObjectId> out;
      for (ObjectId id : a.playlist.selected)
        for (ObjectId k : ops::clipsOfSameSource(s, id)) out.insert(k);
      a.playlist.selected = out;
    }
    {  // J-28: time selection from the clips / stepped by its own length (helpers from J-27)
      ImGui::Separator();
      if (ImGui::MenuItem("Time around the selected clips", nullptr, false, any)) {
        const auto r = ops::clipsTimeRange(s, a.playlist.selected);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      const bool hasSel = a.playlist.selEnd > a.playlist.selStart;
      if (ImGui::MenuItem("Previous time", nullptr, false, hasSel && a.playlist.selStart > 0)) {
        const auto r = ops::shiftTimeSelection(a.playlist.selStart, a.playlist.selEnd, -1);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      if (ImGui::MenuItem("Next time", nullptr, false, hasSel)) {
        const auto r = ops::shiftTimeSelection(a.playlist.selStart, a.playlist.selEnd, 1);
        a.playlist.selStart = r.first, a.playlist.selEnd = r.second;
      }
      ImGui::Separator();
    }
    if (ImGui::MenuItem("Muted clips")) {
      a.playlist.selected.clear();
      for (const auto& c : s.playlist)
        if (c.muted) a.playlist.selected.insert(c.id);
    }
    if (ImGui::MenuItem("Clips of the current pattern")) {
      a.playlist.selected.clear();
      for (const auto& c : s.playlist)
        if (c.pattern == a.curPattern) a.playlist.selected.insert(c.id);
    }
    if (ImGui::MenuItem("Invert selection")) {
      std::set<ObjectId> inv;
      for (const auto& c : s.playlist)
        if (!a.playlist.selected.count(c.id)) inv.insert(c.id);
      a.playlist.selected = inv;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Snap")) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::MenuItem(snapName(static_cast<Snap>(i)), nullptr, a.playlist.snap == static_cast<Snap>(i))) a.playlist.snap = static_cast<Snap>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Tool")) {
    struct T { const char* name; const char* key; Tool tool; };
    const T list[] = {{"Draw", "P", Tool::Draw}, {"Paint", "B", Tool::Paint},   {"Delete", "D", Tool::Erase},
                      {"Mute", "T", Tool::Mute}, {"Slip", "S", Tool::Slip},     {"Slice", "C", Tool::Slice},
                      {"Select", "E", Tool::Select}, {"Zoom", "Z", Tool::Zoom}, {"Play", "Y", Tool::Play}};
    for (const auto& t : list)
      if (ImGui::MenuItem(t.name, t.key, a.playlist.tool == t.tool)) a.playlist.tool = t.tool;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Zoom")) {  // J-28: FL's zoom menu (quick zooms, selection, fit)
    if (ImGui::MenuItem("Zoom in", "PgUp")) a.playlist.zoomRequest *= 1.5f;
    if (ImGui::MenuItem("Zoom out", "PgDn")) a.playlist.zoomRequest /= 1.5f;
    ImGui::Separator();
    if (ImGui::MenuItem("Quick zoom 1 (4 bars)", "Shift+1")) a.playlist.quickZoom = 1;
    if (ImGui::MenuItem("Quick zoom 2 (16 bars)", "Shift+2")) a.playlist.quickZoom = 2;
    if (ImGui::MenuItem("Quick zoom 3 (64 bars)", "Shift+3")) a.playlist.quickZoom = 3;
    if (ImGui::MenuItem("Zoom on selection", "Shift+4", false, any || a.playlist.selEnd > a.playlist.selStart)) a.playlist.zoomSelRequest = true;
    if (ImGui::MenuItem("Whole song", "Home")) a.playlist.fitRequest = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::Separator();
    for (int pct : {50, 75, 100, 150, 200}) {
      std::string l = "Track height " + std::to_string(pct) + "%";
      float h = 46.0f * pct / 100.0f;
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(a.playlist.trackHeight - h) < 0.5f)) a.playlist.trackHeight = std::clamp(h, 24.0f, 120.0f);
    }
    // J-28: picker panel and audio clip editing aids (also in the header corner).
    ImGui::Separator();
    ImGui::MenuItem("Clip source picker", "Alt+P", &a.playlist.pickerOpen);
    ImGui::MenuItem("Wide track headers", nullptr, &a.playlist.wideHeaders);
    ImGui::MenuItem("Stretch audio clips when resizing", "Shift+M", &a.playlist.stretchResize);
    ImGui::MenuItem("Fade handles on audio clips", "Shift+F", &a.playlist.showFades);
    ImGui::MenuItem("Audio clip edges on zero crossings", nullptr, &a.playlist.zeroCross);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Add 8 tracks", nullptr, false, static_cast<int>(s.tracks.size()) + 8 <= kMaxPlaylistTracks))
    a.doc.setPlaylistTrackCount(static_cast<int>(s.tracks.size()) + 8);
  if (ImGui::MenuItem("Collapse all grouped tracks")) ops::setAllGroupsFolded(a.doc, true);   // J-28 (operation: J-27)
  if (ImGui::MenuItem("Expand all grouped tracks")) ops::setAllGroupsFolded(a.doc, false);
  if (ImGui::MenuItem("Delete unused patterns")) {
    int n = ops::deleteUnusedPatterns(a.doc);
    a.notify(n ? "Deleted " + std::to_string(n) + " unused pattern(s)" : std::string("No unused patterns"));
  }
  if (ImGui::MenuItem("Reset empty track names and colours")) {
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
  if (ImGui::MenuItem("Add marker at the time selection", nullptr, false, a.playlist.selEnd > a.playlist.selStart))
    ops::addMarker(a.doc, a.playlist.selStart, ops::nextMarkerName(s));
  if (ImGui::BeginMenu("Add a marker every")) {
    const Tick bar = s.barTicks(), end = std::max(s.lengthTicks(), bar * 16);
    for (int b : {1, 2, 4, 8, 16})
      if (ImGui::MenuItem((std::to_string(b) + (b == 1 ? " bar" : " bars")).c_str())) ops::addMarkersEvery(a.doc, 0, end, bar * b);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Delete all markers", nullptr, false, !s.markers.empty()))
    a.doc.editSong("delete markers", [](Song& x) { x.markers.clear(); });
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
  if (ImGui::MenuItem("Jump here")) {
    a.engine.setMode(PlayMode::Song);
    j31::songJump(a, m.pos);  // J-31: quantized when the option is on
  }
  if (ImGui::BeginMenu("Rename...")) {
    renameField(m.name, [&a, index](const std::string& n) { ops::editMarker(a.doc, index, "rename marker", [&](Marker& k) { k.name = n; }); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Time signature")) {
    if (ImGui::MenuItem("None", nullptr, m.timeSigNum == 0))
      ops::editMarker(a.doc, index, "marker time signature", [](Marker& k) { k.timeSigNum = k.timeSigDen = 0; });
    for (const auto& ts : kTimeSigs) {
      std::string l = std::to_string(ts[0]) + "/" + std::to_string(ts[1]);
      if (ImGui::MenuItem(l.c_str(), nullptr, m.timeSigNum == ts[0] && m.timeSigDen == ts[1]))
        ops::editMarker(a.doc, index, "marker time signature", [&](Marker& k) { k.timeSigNum = ts[0], k.timeSigDen = ts[1]; });
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Action during playback")) {
    for (int k = 0; k < static_cast<int>(MarkerAction::Count); ++k) {
      const auto act = static_cast<MarkerAction>(k);
      if (ImGui::MenuItem(ops::markerActionName(act), nullptr, m.action == act))
        ops::editMarker(a.doc, index, "marker action", [act](Marker& x) { x.action = act; });
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Loop to the next marker")) {
    auto r = ops::markerLoopRange(s, index);
    a.playlist.selStart = r.first;
    a.playlist.selEnd = r.second;
  }
  if (ImGui::MenuItem("Move to the song position")) {
    const Tick pos = static_cast<Tick>(a.engine.position());
    ops::editMarker(a.doc, index, "move marker", [pos](Marker& k) { k.pos = pos; });
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Delete")) ops::removeMarker(a.doc, index);
}

// J-11 multi-channel editing: the selected notes (or all) go to another channel.
void sendNotesItems(App& a, bool move) {
  bool ok = pianoHasTarget(a);
  if (!ImGui::BeginMenu(move ? "Move notes to channel" : "Copy notes to channel", ok)) return;
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
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Import MIDI file...", nullptr, false, ok))  // J-20: into this channel at the view start
      fileDialog(a, "Import MIDI file into the piano roll", false, {".mid", ".midi", ".rmi"}, "",
                 [&a](const std::string& p) { exportui::importMidiAtCursor(a, p); });
    if (ImGui::MenuItem("Import MIDI file as new channels...", nullptr, false, true))
      fileDialog(a, "Import MIDI file", false, {".mid", ".midi", ".rmi"}, "", [&a](const std::string& p) { exportui::importMidiFile(a, p); });
    if (ImGui::MenuItem("Export pattern as MIDI...", nullptr, false, ok)) exportui::exportMidiDialog(a, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, ok)) editCut(a);
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, ok)) editCopy(a);
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, ok && canPaste(a))) editPaste(a);
    if (ImGui::MenuItem("Duplicate", "Ctrl+B", false, ok)) editDuplicate(a);
    if (ImGui::MenuItem("Delete", "Del", false, ok && any)) editDelete(a);
    ImGui::Separator();
    sendNotesItems(a, true);
    sendNotesItems(a, false);
    ImGui::Separator();
    menuparity::pianoEditItems(a);  // J-27
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Tools")) {
    pianoToolItems(a);
    ImGui::Separator();
    menuparity::pianoToolItems(a);  // J-27
    ImGui::Separator();
    j31::pianoToolItems(a);  // J-31: chord progression editor, note groups
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Select")) {
    pianoSelectItems(a);
    menuparity::pianoSelectItems(a);  // J-27
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Chord progression")) {
    progressionItems(a, false);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Riff from progression")) {
    progressionItems(a, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Scripts")) {  // J-24: Lua piano roll scripts
    scriptsMenuItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Snap")) {
    for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i)
      if (ImGui::MenuItem(snapName(static_cast<Snap>(i)), nullptr, a.piano.snap == static_cast<Snap>(i))) a.piano.snap = static_cast<Snap>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Ghost notes", nullptr, &a.piano.ghostNotes);
    if (ImGui::BeginMenu("Ghost channels", a.piano.ghostNotes)) {
      if (ImGui::MenuItem("All other channels", nullptr, a.piano.ghostChannels.empty())) a.piano.ghostChannels.clear();
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
    ImGui::MenuItem("Note names on every key", nullptr, &a.piano.keyNames);
    if (ImGui::BeginMenu("Scale highlighting")) {
      static const char* const keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      if (ImGui::BeginMenu("Root note")) {
        for (int k = 0; k < 12; ++k)
          if (ImGui::MenuItem(keys[k], nullptr, a.piano.scaleRoot == k)) a.piano.scaleRoot = k;
        ImGui::EndMenu();
      }
      const auto& sc = tools::scales();
      if (ImGui::MenuItem("Off", nullptr, a.piano.scale == 0)) a.piano.scale = 0;
      for (size_t i = 1; i < sc.size(); ++i)
        if (ImGui::MenuItem(sc[i].name, nullptr, a.piano.scale == static_cast<int>(i))) a.piano.scale = static_cast<int>(i);
      ImGui::Separator();
      ImGui::MenuItem("Snap notes to the scale", nullptr, &a.piano.snapToScale);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Zoom in")) a.piano.pxPerTick = std::min(4.0f, a.piano.pxPerTick * 1.25f);
    if (ImGui::MenuItem("Zoom out")) a.piano.pxPerTick = std::max(0.02f, a.piano.pxPerTick / 1.25f);
    if (ImGui::MenuItem("Taller rows")) a.piano.rowHeight = std::min(28.0f, a.piano.rowHeight + 2);
    if (ImGui::MenuItem("Shorter rows")) a.piano.rowHeight = std::max(7.0f, a.piano.rowHeight - 2);
    ImGui::Separator();
    menuparity::pianoViewItems(a);  // J-27
    j31::pianoViewItems(a);         // J-31: MIDI snap, editable ghosts, key markers, background waveform
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Tool")) {
    struct T { const char* name; const char* key; Tool tool; };
    const T list[] = {{"Draw", "P", Tool::Draw},     {"Paint", "B", Tool::Paint}, {"Delete", "D", Tool::Erase},
                      {"Select", "E", Tool::Select}, {"Slice", "C", Tool::Slice}, {"Mute", "T", Tool::Mute},
                      {"Play", "Y", Tool::Play},     {"Zoom", "Z", Tool::Zoom},   {"Detune", "U", Tool::Detune}};
    for (const auto& t : list)
      if (ImGui::MenuItem(t.name, t.key, a.piano.tool == t.tool)) a.piano.tool = t.tool;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Stamp")) {
    if (ImGui::MenuItem("Single notes", nullptr, a.piano.stampChord < 0 && a.piano.stampAuto == 0)) a.piano.stampChord = -1, a.piano.stampAuto = 0;
    // J-27: chords from the highlighted scale (the clicked note is the bottom or the top one).
    if (ImGui::MenuItem("Scale chord, clicked note at the bottom", nullptr, a.piano.stampAuto == 1, a.piano.scale > 0))
      a.piano.stampAuto = 1, a.piano.stampChord = -1;
    if (ImGui::MenuItem("Scale chord, clicked note on top", nullptr, a.piano.stampAuto == 2, a.piano.scale > 0))
      a.piano.stampAuto = 2, a.piano.stampChord = -1;
    ImGui::MenuItem("Just the next chord", nullptr, &a.piano.stampOnce);
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
  if (ImGui::BeginMenu("Rename, colour and icon")) {
    if (ImGui::BeginMenu("Rename...", !master)) {
      renameField(t.name.empty() ? mixerTrackName(s, index) : t.name,
                  [&a, id](const std::string& n) { a.doc.editMixerTrack(id, "rename mixer track", [&](MixerTrack& m) { m.name = n; }); });
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Colour")) {
      uint32_t col;
      if (colorGrid(&col)) mixops::setColor(a.doc, selIds, col);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Random colour")) {
      a.doc.beginGroup("random colour");
      for (ObjectId x : selIds) mixops::setColor(a.doc, {x}, randomColor());
      a.doc.endGroup();
    }
    if (ImGui::MenuItem("Gradient colour", nullptr, false, selIds.size() >= 3)) mixops::gradientColor(a.doc, selIds);
    if (ImGui::BeginMenu("Icon")) {
      int ic = t.icon;
      if (trackicons::picker(t.icon, &ic)) mixops::setIcon(a.doc, selIds, ic);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open mixer track state...")) {
      fileDialog(a, "Open mixer track state", false, {mixops::kTrackStateExt}, "", [&a, id](const std::string& p) {
        std::string err;
        if (!mixops::loadTrackState(a.doc, id, p, &err)) a.notify(err, true);
      });
    }
    if (ImGui::MenuItem("Save mixer track state as...", "Ctrl+Shift+S")) {
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
  if (ImGui::MenuItem(many ? "Reset selected tracks to default" : "Reset track to default")) mixops::resetTracks(a.doc, selIds);
  if (ImGui::MenuItem(t.muted ? "Unmute" : "Mute")) a.doc.editMixerTrack(id, "mute mixer track", [](MixerTrack& m) { m.muted = !m.muted; });
  if (ImGui::MenuItem("Disable track", nullptr, t.disabled)) mixops::setDisabled(a.doc, selIds, !t.disabled);
  if (ImGui::MenuItem(mixops::isMono(t) ? "Make stereo" : "Make mono")) mixops::setMono(a.doc, selIds, !mixops::isMono(t));
  if (ImGui::MenuItem("Swap left and right", nullptr, t.swapLR)) mixops::setSwap(a.doc, selIds, !t.swapLR);
  if (ImGui::MenuItem("Reverse polarity", nullptr, t.invertPolarity)) mixops::setPolarity(a.doc, selIds, !t.invertPolarity);
  if (ImGui::BeginMenu("Stereo separation")) {
    static const struct { const char* name; float v; } seps[] = {{"Merged (mono)", -1.0f}, {"Narrow", -0.5f}, {"Normal", 0.0f}, {"Wide", 0.5f}, {"Widest", 1.0f}};
    for (const auto& sp : seps)
      if (ImGui::MenuItem(sp.name, nullptr, t.stereoSep == sp.v)) a.doc.setParam(mixerParam(id, MixerParam::StereoSep), sp.v, false);
    ImGui::EndMenu();
  }

  // ---- routing ----
  ImGui::SeparatorText("Routing");
  auto chans = selChannelsInOrder(a);
  if (ImGui::BeginMenu("Channel routing")) {
    if (ImGui::MenuItem("Route selected channels to this track", "Ctrl+L", false, !chans.empty())) ops::routeChannels(a.doc, chans, id, false);
    if (ImGui::MenuItem("Route selected channels starting from this track", "Ctrl+Shift+L", false, !chans.empty() && !master))
      ops::routeChannels(a.doc, chans, id, true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Track routing")) {
    std::string err;
    ImGui::SeparatorText("Selected tracks to this track");
    if (ImGui::MenuItem("Route selected to this track", nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Send, &err), err);
    if (ImGui::MenuItem("Route selected to this track only", nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Only, &err), err);
    if (ImGui::MenuItem("Sidechain selected to this track", nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::Sidechain, &err), err);
    if (ImGui::MenuItem("Sidechain selected to this track only", nullptr, false, !others.empty()))
      report(mixops::route(a.doc, others, {id}, mixops::RouteMode::SidechainOnly, &err), err);
    if (ImGui::MenuItem("Create submix...", nullptr, false, !selIds.empty() && !(selIds.size() == 1 && master))) {
      std::vector<ObjectId> src = selIds;
      menus::openPrompt("Submix name", "Submix", [&a, src](const std::string& n) {
        ObjectId sub = mixops::createSubmix(a.doc, src, n.empty() ? std::string("Submix") : n);
        if (sub) a.selMixer = sub, a.mixerView.sel = {sub};
        else a.notify("Could not create the submix", true);
      });
    }
    ImGui::SeparatorText("This track to selected tracks");
    if (ImGui::MenuItem("Route this track to selected", nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Send, &err), err);
    if (ImGui::MenuItem("Route this track to selected only", nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Only, &err), err);
    if (ImGui::MenuItem("Sidechain this track to selected", nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::Sidechain, &err), err);
    if (ImGui::MenuItem("Sidechain this track to selected only", nullptr, false, !others.empty() && !master))
      report(mixops::route(a.doc, {id}, others, mixops::RouteMode::SidechainOnly, &err), err);
    ImGui::EndMenu();
  }
  if (!master) {
    if (ImGui::BeginMenu("Send to")) {
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
    if (ImGui::BeginMenu("Route only to")) {
      routeMenuItems(a, [&t](ObjectId to) { return t.sends.size() == 1 && t.sends[0].to == to; },
                     [&a, id](ObjectId to) {
                       std::string err;
                       if (!mixops::route(a.doc, {id}, {to}, mixops::RouteMode::Only, &err) && !err.empty()) a.notify(err, true);
                     });
      ImGui::EndMenu();
    }
  }
  if (ImGui::BeginMenu("Select")) {
    if (ImGui::MenuItem("Channels routed to this track", "Alt+L")) {
      a.selChannels.clear();
      for (const auto& c : s.channels)
        if (c.mixerTrack == id) a.selChannels.insert(c.id);
      if (!a.selChannels.empty()) a.selChannel = *a.selChannels.begin();
      a.showRack = true;
    }
    if (ImGui::MenuItem("Tracks this one is routed to")) {
      auto v = mixops::routedFrom(s, id);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem("Tracks routed to this one", "Ctrl+Alt+L")) {
      auto v = mixops::routedTo(s, id);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem("Group", nullptr, false, !t.group.empty())) {
      auto v = mixops::groupMembers(s, t.group);
      a.mixerView.sel = std::set<ObjectId>(v.begin(), v.end());
    }
    if (ImGui::MenuItem("All")) {
      a.mixerView.sel.clear();
      for (const auto& m : s.mixer) a.mixerView.sel.insert(m.id);
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(many ? "Reset selected tracks' routing" : "Reset routing to master", nullptr, false, !(master && !many)))
    mixops::resetRouting(a.doc, selIds);
  if (ImGui::MenuItem("Unroute tracks routed to this track", nullptr, false, !mixops::routedTo(s, id).empty()))
    mixops::unrouteIncoming(a.doc, id);
  if (ImGui::MenuItem("Solo", "S", t.solo)) ops::soloMixerTrack(a.doc, id);
  j31::mixerItems(a, id);  // J-31: alt solo, recording point
  {
    // Disk recording arm (J-8's MixerTrack::armed; toggleArm also picks an input).
    if (ImGui::MenuItem(many ? "Arm selected tracks" : "Arm for disk recording", nullptr, t.armed && !many, !master))
      for (ObjectId x : selIds) {
        const MixerTrack* m = a.doc.song().mixerTrack(x);
        if (m && (many ? !m->armed : true)) toggleArm(a, x);
      }
    bool anyArmed = false;
    for (ObjectId x : selIds) anyArmed = anyArmed || (a.doc.song().mixerTrack(x) && a.doc.song().mixerTrack(x)->armed);
    if (ImGui::MenuItem("Disarm selected tracks", nullptr, false, anyArmed))
      for (ObjectId x : selIds)
        if (const MixerTrack* m = a.doc.song().mixerTrack(x); m && m->armed) toggleArm(a, x);
  }
  if (ImGui::MenuItem("Render selected tracks to wave files...", "Alt+Shift+R")) renderMixerStems(a, selIds);

  // ---- group ----
  ImGui::SeparatorText("Group");
  if (ImGui::MenuItem("Create group...")) {
    std::vector<ObjectId> ids = selIds;
    menus::openPrompt("Group name", mixops::newGroupName(s), [&a, ids](const std::string& n) {
      if (!n.empty()) mixops::setGroup(a.doc, ids, n);
    });
  }
  const auto groups = mixops::groupNames(s);
  if (ImGui::BeginMenu("Add to group", !groups.empty())) {
    for (const auto& g : groups)
      if (ImGui::MenuItem(g.c_str(), nullptr, t.group == g)) mixops::setGroup(a.doc, selIds, g);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Remove from group", nullptr, false, !t.group.empty())) mixops::setGroup(a.doc, selIds, "");
  if (ImGui::MenuItem("Auto colour group", nullptr, false, mixops::groupMembers(s, t.group).size() >= 3))
    mixops::gradientColor(a.doc, mixops::groupMembers(s, t.group));

  // ---- effects ----
  ImGui::SeparatorText("Effects");
  if (ImGui::BeginMenu("Insert effect")) {
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
  if (ImGui::BeginMenu("Effect chain")) {
    if (ImGui::MenuItem("Copy effect chain", "Ctrl+C")) editCopy(a);
    if (ImGui::MenuItem("Paste effect chain", "Ctrl+V", false, canPaste(a))) editPaste(a);
    if (ImGui::MenuItem("Clear all effects")) a.doc.editMixerTrack(id, "clear effects", [](MixerTrack& m) { m.slots.assign(kEffectSlots, EffectSlot{}); });
    // Effect-chain presets from the preset library (yours and imported ones).
    if (ImGui::BeginMenu("Load effect chain")) {
      static std::vector<PresetEntry> chains;
      if (ImGui::IsWindowAppearing()) chains = listChains(presetRoot(a.configDir()));
      if (chains.empty()) ImGui::TextDisabled("No saved chains");
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
    if (ImGui::BeginMenu("Save effect chain")) {
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
  ImGui::SeparatorText("Layout");
  if (ImGui::BeginMenu("Dock to", !master)) {
    static const char* docks[] = {"Left", "Middle", "Right"};
    for (int k = 0; k < 3; ++k)
      if (ImGui::MenuItem(docks[k], nullptr, t.dock == k - 1)) mixops::setDock(a.doc, selIds, static_cast<MixerDock>(k - 1));
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Move left", "Alt+Left", false, index > 1)) ops::moveMixerTrack(a.doc, id, -1);
  if (ImGui::MenuItem("Move right", "Alt+Right", false, !master && index + 1 < s.mixer.size())) ops::moveMixerTrack(a.doc, id, 1);
  if (ImGui::MenuItem(many ? "Delete selected tracks" : "Delete", "Del", false, !master)) {
    a.doc.beginGroup("delete mixer tracks");
    for (ObjectId x : selIds)
      if (x != a.doc.song().masterId()) a.doc.removeMixerTrack(x);
    a.doc.endGroup();
    a.mixerView.sel.clear();
  }
  if (ImGui::MenuItem("Insert one")) {
    auto ids = mixops::insertTracks(a.doc, master ? 1 : static_cast<int>(index), 1);
    a.selMixer = ids.empty() ? kNoId : ids.front();
  }
  if (ImGui::MenuItem("Insert multiple...")) {
    const int at = master ? 1 : static_cast<int>(index);
    menus::openPrompt("Insert mixer tracks (how many?)", "4", [&a, at](const std::string& n) {
      auto ids = mixops::insertTracks(a.doc, at, std::atoi(n.c_str()));
      if (!ids.empty()) a.selMixer = ids.front();
    });
  }
  if (ImGui::MenuItem("Insert from state...")) {
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
  if (ImGui::MenuItem("Trim unused tracks at the end")) {
    int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? "Removed " + std::to_string(n) + " unused track(s)" : std::string("No unused tracks at the end"));
  }

  // ---- CPU ----
  ImGui::SeparatorText("CPU");
  if (ImGui::MenuItem("Allow threaded processing", nullptr, t.threaded)) mixops::setThreaded(a.doc, selIds, !t.threaded);
  if (ImGui::BeginMenu("Track latency")) {
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
  if (ImGui::BeginMenu(info ? "Replace" : "Insert")) {
    effectItems([&a, track, slot](const std::string& type) {
      a.doc.setSlotPlugin(track, slot, type);
      a.fxEditor = {track, slot};
    }, es.plugin.type);
    ImGui::EndMenu();
  }
  if (!info) return;
  if (ImGui::MenuItem("Open editor")) a.fxEditor = {track, slot};
  if (isHostedType(es.plugin.type) && ImGui::MenuItem("Show plugin editor")) openHostedEditor(a, PluginTarget{track, slot});
  if (ImGui::MenuItem("Bypass", nullptr, !es.enabled)) ops::setSlotEnabled(a.doc, track, slot, !es.enabled);
  if (ImGui::BeginMenu("Enable switch")) {  // automation / remote control of the slot's on/off switch
    knobContextMenu(a, slotEnableParam(track, slot));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Mix")) {
    knobContextMenu(a, slotMixParam(track, slot));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Presets and state")) {
    pluginWindowMenu(a, track, slot);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Move up", nullptr, false, slot > 0)) a.doc.moveSlot(track, slot, slot - 1);
  if (ImGui::MenuItem("Move down", nullptr, false, slot + 1 < kEffectSlots)) a.doc.moveSlot(track, slot, slot + 1);
  if (ImGui::BeginMenu("Move to track")) {
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
  if (ImGui::MenuItem("Reset to default")) {
    PluginState d = defaultPluginState(es.plugin.type);
    setPluginState(a, PluginTarget{track, slot}, d, "reset effect");
  }
  if (ImGui::MenuItem("Remove")) a.doc.setSlotPlugin(track, slot, "");
}

// -------------------------------------------------------------------- knob ----

void knobContextMenu(App& a, ParamId pid) {
  static double valueClip = 0;
  static bool hasClip = false;
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) return;
  if (ImGui::MenuItem("Copy value")) valueClip = d->toNormalized(*v), hasClip = true;
  if (ImGui::MenuItem("Paste value", nullptr, false, hasClip)) a.doc.setParam(pid, d->fromNormalized(valueClip), false);
  if (d->kind == ParamKind::Continuous && ImGui::BeginMenu("Set to")) {
    if (ImGui::MenuItem("Minimum")) a.doc.setParam(pid, d->min, false);
    if (ImGui::MenuItem("Centre")) a.doc.setParam(pid, d->fromNormalized(0.5), false);
    if (ImGui::MenuItem("Maximum")) a.doc.setParam(pid, d->max, false);
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
  if (ImGui::BeginMenu("Presets", !info->presets.empty())) {
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
  if (ImGui::MenuItem("Save preset to file...")) {
    PluginState copy = *st;
    fileDialog(a, "Save preset", true, {".json"}, info->name + " preset.json", [&a, copy](const std::string& p) {
      std::string path = fs::path(p).extension().empty() ? p + ".json" : p;
      a.notify(writePresetFile(path, copy) ? "Saved preset " + fs::path(path).filename().string() : std::string("Could not save preset"),
               false);
    });
  }
  if (ImGui::MenuItem("Load preset from file...")) {
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
  if (ImGui::MenuItem("Reset to default")) {
    PluginState ns = defaultPluginState(info->type);
    ns.data = st->data;
    setPluginState(a, t, ns, "reset plugin");
  }
  if (ImGui::MenuItem("Randomize")) {
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
  if (ImGui::MenuItem("Copy state")) stateClip = *st;
  if (ImGui::MenuItem("Paste state", nullptr, false, stateClip.type == st->type)) {
    PluginState ns = stateClip;
    ns.data = st->data;
    setPluginState(a, t, ns, "paste plugin state");
  }
  ImGui::Separator();
  if (t.isChannel()) {
    const Channel* c = s.channel(owner);
    if (ImGui::MenuItem("Piano roll", "F7")) a.openPianoRoll(owner);
    if (ImGui::BeginMenu("Rename...")) {
      renameField(c->name, [&a, owner](const std::string& n) { a.doc.renameChannel(owner, n); });
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Colour")) {
      uint32_t col;
      if (colorGrid(&col)) ops::setChannelsColor(a.doc, {owner}, col);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Replace instrument")) {
      instrumentItems([&a, owner](const std::string& type) { a.doc.setChannelPlugin(owner, type); }, info->type);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Route to a free mixer track", "Ctrl+L")) {
      auto tr = ops::routeToFreeMixerTracks(a.doc, {owner});
      if (!tr.empty()) a.selMixer = tr[0];
    }
    if (ImGui::MenuItem("Clone channel")) a.selectChannel(a.doc.cloneChannel(owner));
    if (ImGui::MenuItem("Delete channel")) a.doc.removeChannel(owner);
  } else {
    const MixerTrack* m = s.mixerTrack(owner);
    bool enabled = m && m->slots[static_cast<size_t>(slot)].enabled;
    if (ImGui::BeginMenu("Replace effect")) {
      effectItems([&a, owner, slot](const std::string& type) { a.doc.setSlotPlugin(owner, slot, type); }, info->type);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(enabled ? "Disable" : "Enable")) ops::setSlotEnabled(a.doc, owner, slot, !enabled);
    if (ImGui::MenuItem("Move up", nullptr, false, slot > 0)) {
      a.doc.moveSlot(owner, slot, slot - 1);
      a.fxEditor = {owner, slot - 1};
    }
    if (ImGui::MenuItem("Move down", nullptr, false, slot + 1 < kEffectSlots)) {
      a.doc.moveSlot(owner, slot, slot + 1);
      a.fxEditor = {owner, slot + 1};
    }
    if (ImGui::MenuItem("Remove effect")) a.doc.setSlotPlugin(owner, slot, "");
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Close window")) {
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
  else if (name == "track") playlistTrackMenu(a, 0);
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
  else ImGui::TextDisabled("Unknown menu: %s", name.c_str());
  ImGui::EndPopup();
}

}  // namespace gl
