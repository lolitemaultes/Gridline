// Gridline - K-199: game controllers and the command pad (see GamepadUi.h).
#include "GamepadUi.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Gamepad.h"
#include "../core/I18n.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "Shell.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::gamepadui {

namespace {
using gamepad::Button;

struct State {
  bool inited = false;
  bool gameModeSession = false;
  bool used = false;  // a controller button was pressed (Automatic listens from then on)
  bool open = false;
  int lit = 0;
  bool l1 = false;
  int view = -1;  // the window the D-pad last stepped to (gamepad::viewCycle)
  int stickX = 0, stickY = 0;  // the left stick's last step (-1, 0, 1), so a held stick moves once
  bool notes = false;           // the note-entry layer
  int octave = gamepad::kDefaultOctave;
  int lastNote = -1;            // (dev) the last note played
  struct Held {
    ObjectId channel = kNoId;
    int pitch = -1;
  };
  std::map<Button, Held> held;  // notes sounding, by the button that plays them
  std::deque<std::string> queue;  // keymap commands to run, one per frame
  std::string lastRun;            // (dev) the last command sent
  size_t pads = 0;                      // controllers connected (GamepadSdl.cpp)
  bool (*openUrl)(const char*) = nullptr;  // the program's URL opener (plugin builds have none)
};
State S;

bool listeningNow(App& a) {
  return gamepad::listening(gamepad::modeFromSetting(a.settings.getString("input.gamepad")), S.gameModeSession, S.used);
}

void sendEscape() {
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_Escape, true);
  io.AddKeyEvent(ImGuiKey_Escape, false);
}

bool gameModeSession() {
  if (!S.inited) S.gameModeSession = gamepad::gameMode([](const char* k) { return static_cast<const char*>(std::getenv(k)); });
  return S.gameModeSession;
}

// Something in front takes the D-pad as arrow keys and A as Enter: a menu, a pop-up, a dialog
// or the command palette.
bool somethingInFront() {
  return cmdpalette::isOpen() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

void sendKey(ImGuiKey k) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(k, true);
  io.AddKeyEvent(k, false);
}

// A main window: shown and brought to the front when it is hidden or behind; never closed (the
// shortcut toggles it, which on a controller would shut the window it meant to show).
struct ViewWin {
  const char* command;
  bool App::*flag;
  const char* window;
};
constexpr ViewWin kViews[] = {{"view.playlist", &App::showPlaylist, "Playlist"},
                              {"view.channelRack", &App::showRack, "Channel rack"},
                              {"view.pianoRoll", &App::showPiano, "Piano roll"},
                              {"view.mixer", &App::showMixer, "Mixer"},
                              {"view.browser", &App::showBrowser, "Browser"}};

bool showView(App& a, const std::string& cmd) {
  if (layoutui::active() == layout::Preset::Tracker) return false;  // its pages come to the front already
  for (const ViewWin& v : kViews) {
    if (cmd != v.command) continue;
    if (!(a.*v.flag)) return false;  // closed: the command opens it as usual
    a.focusWindow = v.window;
    return true;
  }
  return false;
}

void stepChannel(App& a, int dir) {
  const auto& chs = a.doc.song().channels;
  if (chs.empty()) return;
  int at = -1;
  for (size_t i = 0; i < chs.size(); ++i)
    if (chs[i].id == a.selChannel) at = static_cast<int>(i);
  const int n = static_cast<int>(chs.size());
  at = at < 0 ? (dir > 0 ? 0 : n - 1) : std::clamp(at + dir, 0, n - 1);
  a.selectChannel(chs[static_cast<size_t>(at)].id);
  a.notify(std::string(i18n::tr("Channel")) + ": " + chs[static_cast<size_t>(at)].name);
}

void noteOff(App& a, Button b) {
  auto it = S.held.find(b);
  if (it == S.held.end()) return;
  a.engine.noteOff(it->second.channel, it->second.pitch);
  a.playedNoteOff(it->second.channel, it->second.pitch);
  S.held.erase(it);
}

void allNotesOff(App& a) {
  while (!S.held.empty()) noteOff(a, S.held.begin()->first);
}

void noteOn(App& a, Button b) {
  if (a.selChannel == kNoId) {
    a.notify(i18n::tr("Pick a channel first: D-pad up and down"));
    return;
  }
  noteOff(a, b);
  const int p = gamepad::noteFor(b, S.l1, S.octave);
  if (p < 0) return;
  const int vel = std::clamp(a.typing.velocity, 1, 127);
  a.engine.noteOn(a.selChannel, p, static_cast<float>(vel) / 127.0f);
  a.playedNoteOn(a.selChannel, p, vel);
  S.held[b] = {a.selChannel, p};
  S.lastNote = p;
}

void setNotes(App& a, bool on) {
  if (S.notes == on) return;
  S.notes = on;
  if (!on) allNotesOff(a);
  a.notify(on ? i18n::tr("Note entry on: the D-pad and A B X Y play notes, R1 turns it off")
              : i18n::tr("Note entry off"));
}

void run(App& a, const std::string& cmd) {
  if (cmd.empty()) return;
  S.lastRun = cmd;
  if (cmd == "pad.toggle") {
    S.open = !S.open;
  } else if (cmd == "pad.close") {
    if (S.open) S.open = false;
    else sendEscape();  // B outside the pad: as Escape (closes a dialog or menu)
  } else if (cmd == "pad.nextView" || cmd == "pad.prevView") {
    S.view = gamepad::nextView(S.view, cmd == "pad.nextView" ? 1 : -1);
    const std::string v = gamepad::viewCycle()[static_cast<size_t>(S.view)];
    if (!showView(a, v)) S.queue.push_back(v);
  } else if (cmd == "pad.prevChannel" || cmd == "pad.nextChannel") {
    stepChannel(a, cmd == "pad.nextChannel" ? 1 : -1);
  } else if (cmd == "pad.notes") {
    setNotes(a, !S.notes);
  } else if (cmd == "pad.osk") {
    if (S.openUrl) S.openUrl("steam://open/keyboard");  // Steam's on-screen keyboard (nothing happens outside Steam)
  } else if (!showView(a, cmd)) {
    S.queue.push_back(cmd);
  }
}

void runTile(App& a, int i) {
  const gamepad::Tile& t = gamepad::padTiles()[static_cast<size_t>(std::clamp(i, 0, gamepad::kPadTiles - 1))];
  const std::string cmd = t.command;
  // Undo and redo leave the pad open (pressed again and again); every other tile closes it.
  if (cmd != "edit.undo" && cmd != "edit.redo") S.open = false;
  run(a, cmd);
}

void press(App& a, Button b, bool down) {
  if (b == Button::L1) {
    S.l1 = down;
    return;
  }
  if (!down) {
    noteOff(a, b);  // a note button let go (nothing when it played none)
    return;
  }
  if (gamepad::modeFromSetting(a.settings.getString("input.gamepad")) == gamepad::Mode::Off) return;
  S.used = true;
  if (S.open) {
    switch (b) {
      case Button::A: runTile(a, S.lit); return;
      case Button::B:
      case Button::Start: S.open = false; return;
      case Button::Y:
        if (!S.l1) {
          S.open = false;
          return;
        }
        break;
      case Button::Up: S.lit = gamepad::padMove(S.lit, 0, -1); return;
      case Button::Down: S.lit = gamepad::padMove(S.lit, 0, 1); return;
      case Button::Left: S.lit = gamepad::padMove(S.lit, -1, 0); return;
      case Button::Right: S.lit = gamepad::padMove(S.lit, 1, 0); return;
      default: break;
    }
  }
  // A menu, dialog or the command palette in front: the D-pad moves in it, A picks, B closes.
  if (somethingInFront()) {
    switch (b) {
      case Button::Up: sendKey(ImGuiKey_UpArrow); return;
      case Button::Down: sendKey(ImGuiKey_DownArrow); return;
      case Button::Left: sendKey(ImGuiKey_LeftArrow); return;
      case Button::Right: sendKey(ImGuiKey_RightArrow); return;
      case Button::A: sendKey(ImGuiKey_Enter); return;
      case Button::B: sendKey(ImGuiKey_Escape); return;
      default: break;
    }
  }
  if (S.notes && gamepad::playsNote(b)) {
    noteOn(a, b);
    return;
  }
  run(a, gamepad::commandFor(b, S.l1));
}

void stick(App& a, int& last, int value, int dx, int dy) {
  const int step = value > 16000 ? 1 : value < -16000 ? -1 : 0;
  if (step != 0 && step != last) {
    if (S.open) {
      S.lit = gamepad::padMove(S.lit, dx * step, dy * step);
    } else if (S.notes && dy != 0) {  // note entry: the stick moves the octave (up = higher)
      S.octave = std::clamp(S.octave - step, gamepad::kMinOctave, gamepad::kMaxOctave);
      char msg[64];
      std::snprintf(msg, sizeof msg, i18n::tr("Octave %d"), S.octave);
      a.notify(msg);
    }
  }
  last = step;
}

// The note layer's badge: which octave the buttons play, at the bottom of the window.
void drawNoteBadge(App& a) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 12), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##gamepadnotes", nullptr, fl)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    const Channel* c = a.doc.song().channel(a.selChannel);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::pal().hot), "%s", i18n::tr("Note entry"));
    ImGui::SameLine();
    ImGui::Text(i18n::tr("octave %d   %s   stick: octave   L1: sharp   R1: off"), S.octave, c ? c->name.c_str() : i18n::tr("no channel"));
  }
  ImGui::End();
}

void drawPad(App& a) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float tileW = 150, tileH = 64, gap = 8, pad = 14;
  const ImVec2 size(gamepad::kPadCols * tileW + (gamepad::kPadCols - 1) * gap + 2 * pad,
                    gamepad::kPadRows * tileH + (gamepad::kPadRows - 1) * gap + 2 * pad + 22);
  // The lower half, so the transport readout above stays in view.
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.72f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size);
  const theme::Palette& P = theme::pal();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##gamepadpad", nullptr, fl)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textDim), "%s", i18n::tr("Command pad   A runs   B closes   L1 + button: second command"));
    for (int i = 0; i < gamepad::kPadTiles; ++i) {
      const gamepad::Tile& t = gamepad::padTiles()[static_cast<size_t>(i)];
      if (i % gamepad::kPadCols) ImGui::SameLine();
      const bool lit = i == S.lit;
      ImGui::PushID(i);
      if (lit) ImGui::PushStyleColor(ImGuiCol_Button, P.accent);
      if (ImGui::Button(i18n::tr(t.label), ImVec2(tileW, tileH))) {  // a finger on the touch screen
        S.lit = i;
        runTile(a, i);
      }
      if (lit) ImGui::PopStyleColor();
      ImGui::PopID();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}
}  // namespace

void initRules(App& a) {
  (void)a;
  gameModeSession();
  S.inited = true;
}

void pressButton(App& a, gamepad::Button b, bool down) { press(a, b, down); }

void stickAxis(App& a, int axis, int value) {
  if (!listeningNow(a)) return;
  if (axis == 0) stick(a, S.stickX, value, 1, 0);
  if (axis == 1) stick(a, S.stickY, value, 0, 1);
}

void setControllers(size_t n) { S.pads = n; }

void setOpenUrl(bool (*f)(const char*)) { S.openUrl = f; }

void draw(App& a) {
  if (!S.inited) initRules(a);
  if (!S.queue.empty()) {
    keys::inject(S.queue.front().c_str(), ImGui::GetFrameCount() + 1);
    S.queue.pop_front();
  }
  if (!listeningNow(a)) {
    S.open = false;
    if (S.notes) setNotes(a, false);
    return;
  }
  if (S.gameModeSession && !a.settings.getBool("input.gamepadHintShown")) {
    a.notify(i18n::tr("Controller: press the Menu button for the command pad"));
    a.settings.set("input.gamepadHintShown", true);
  }
  if (S.notes) drawNoteBadge(a);
  if (S.open) drawPad(a);
}

bool padOpen() { return S.open; }
bool gameMode() { return gameModeSession(); }
bool listening(App& a) {
  gameModeSession();
  return listeningNow(a);
}

void bindingsTable(App& a, float height) {
  (void)a;
  const auto rows = gamepad::bindingRows([](const std::string& id) -> const char* {
    const keys::Command* c = keys::findCommand(id);
    return c ? c->label : nullptr;
  });
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##padkeys", 4, tf, ImVec2(0, height))) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn(i18n::tr("Button"), ImGuiTableColumnFlags_WidthFixed, 90);
  ImGui::TableSetupColumn(i18n::tr("Alone"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(i18n::tr("With L1 held"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(i18n::tr("Note entry"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(theme::pal().textFaint);
  auto cell = [&](const std::string& t) {
    ImGui::TableNextColumn();
    if (t.empty()) ImGui::TextColored(dim, "-");
    else ImGui::TextUnformatted(i18n::tr(t.c_str()));
  };
  for (const auto& r : rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(i18n::tr(gamepad::buttonLabel(r.button)));
    cell(r.alone);
    cell(r.withL1);
    cell(r.notes);
  }
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(i18n::tr("Left stick"));
  cell("Moves the lit tile on the command pad");
  cell("");
  cell("Octave up and down");
  ImGui::EndTable();
}

bool dockPlugins(App& a) {
  const std::string m = a.settings.getString("input.dockPlugins");
  if (m == "on") return true;
  if (m == "off") return false;
  return gameModeSession();
}

void nextPluginWindowDocked(App& a) {
  (void)a;
  // With the main view: the node the playlist is docked in (else the layout's main node).
  ImGuiID node = 0;
  const ImGuiContext& g = *ImGui::GetCurrentContext();
  for (const ImGuiWindow* w : g.Windows)  // titles are translated ("<title>###Playlist"): match the stable part
    if (w && w->DockId && std::strcmp(ui::stableWindowName(w->Name), "Playlist") == 0) node = w->DockId;
  if (!node) node = shell::builtNodes().main;
  if (!node) return;
  ImGui::SetNextWindowDockID(node, ImGuiCond_Appearing);
  ImGui::GetCurrentContext()->NextWindowData.PosUndock = false;  // the window's centring must not undock it
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.size() >= 2 && (args[0] == "button" || args[0] == "down" || args[0] == "up")) {
    const Button b = gamepad::buttonFromName(args[1]);
    const bool l1 = args.size() >= 3 && args[2] == "l1";
    if (l1) press(a, Button::L1, true);
    if (args[0] != "up") press(a, b, true);
    if (args[0] != "down") press(a, b, false);
    if (l1) press(a, Button::L1, false);
  } else if (!args.empty() && args[0] == "chan") {  // the selected channel's plugin window
    a.settingsChannel = a.selChannel;
  } else if (args.size() >= 2 && args[0] == "settings") {  // gamepad settings general|keys: that Settings page
    a.showSettings = true;
    a.settingsTab = args[1] == "keys" ? 8 : 5;
  } else if (args.size() >= 2 && args[0] == "stick") {  // gamepad stick up|down: one step of the left stick
    const int v = args[1] == "up" ? -30000 : 30000;
    stick(a, S.stickY, v, 0, 1);
    stick(a, S.stickY, 0, 0, 1);
  }
  const Channel* ch = a.doc.song().channel(a.selChannel);
  std::string views;
  for (const ViewWin& v : kViews) views += (a.*v.flag) ? '1' : '0';
  const ImGuiWindow* pw = ImGui::FindWindowByName("###channelsettings");
  const int plugWin = !pw || !pw->WasActive ? 0 : pw->DockIsActive ? 2 : 1;  // 0 none, 1 floating, 2 docked
  std::printf("gamepad frame %d: listening %d game-mode %d pad %d lit %d last %s controllers %zu notes %d octave %d note %d held %zu "
              "channel %s focus %s dock %d views %s plugwin %d\n",
              frame, listeningNow(a) ? 1 : 0, S.gameModeSession ? 1 : 0, S.open ? 1 : 0, S.lit, S.lastRun.empty() ? "-" : S.lastRun.c_str(),
              S.pads, S.notes ? 1 : 0, S.octave, S.lastNote, S.held.size(), ch ? ch->name.c_str() : "-",
              a.focusWindow.empty() ? "-" : a.focusWindow.c_str(), dockPlugins(a) ? 1 : 0, views.c_str(), plugWin);
  std::fflush(stdout);
}

}  // namespace gl::gamepadui
