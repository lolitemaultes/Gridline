// Gridline: the piano roll's chord stamp button and its chord picker (see ChordStampUi.h).
#include "ChordStampUi.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/ChordTools.h"
#include "../core/I18n.h"
#include "../core/NoteTools.h"
#include "App.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::stampui {
namespace {

constexpr int kRecentMax = 6;
constexpr const char* kRecentKey = "piano.recentChords";

// The chord types in the picker's groups (tools::chords() indices; ChordTools' order).
struct Group {
  const char* title;
  std::vector<int> chords;
};
const std::vector<Group>& groups() {
  static const std::vector<Group> g = {
      {i18n::N_("Triads"), {0, 1, 2, 3, 4, 5, 6}},
      {i18n::N_("Sixths and sevenths"), {7, 8, 9, 10, 11, 12, 13, 14, 21, 24, 27}},
      {i18n::N_("Extended"), {15, 22, 16, 17, 18, 19, 23, 25, 26}},
      {i18n::N_("Other"), {20}},
  };
  return g;
}

// A chip's label: the chord's symbol, "maj" for the major triad (whose symbol is empty).
std::string symbolOf(int chord) {
  const auto& q = chord::qualities();
  if (chord < 0 || chord >= static_cast<int>(q.size())) return {};
  const std::string sym = q[static_cast<size_t>(chord)].symbol;
  if (sym.empty()) return "maj";
  if (sym == "(8va)") return "8va";
  return sym;
}

int devChip() {  // tests: the chip whose rectangle is noted as "prstampchip" (a chord index)
  static const int c = [] {
    const char* e = std::getenv("GL_DEV_STAMP_CHIP");
    return e ? std::atoi(e) : 11;
  }();
  return c;
}

void saveRecent(App& a, const std::vector<int>& r) {
  std::string s;
  for (int c : r) s += (s.empty() ? "" : ",") + std::to_string(c);
  a.settings.set(kRecentKey, s);
}

// One chord chip: its symbol, lit when it is the chord being stamped; the chord's full name as its hint.
bool chip(int chord, bool lit, float w, const char* idPrefix) {
  const auto& ch = tools::chords();
  const std::string label = symbolOf(chord) + "##" + idPrefix + std::to_string(chord);
  const bool r = ui::TextButton(label.c_str(), lit, w, ch[static_cast<size_t>(chord)].name);
  if (chord == devChip() && idPrefix[0] == 'g') devNoteRect("prstampchip");
  return r;
}

void picker(App& a) {
  PianoRollState& st = a.piano;
  const auto& ch = tools::chords();
  const float gap = 4.0f;
  // Chip width: room for the longest symbol.
  float chipW = 52.0f;
  for (int i = 0; i < static_cast<int>(ch.size()); ++i)
    chipW = std::max(chipW, ImGui::CalcTextSize(symbolOf(i).c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + 4);
  chipW = std::floor(chipW);
  constexpr int kPerRow = 6;
  auto row = [&](const std::vector<int>& list, const char* idPrefix) -> int {
    int picked = -1;
    for (size_t i = 0; i < list.size(); ++i) {
      if (i % kPerRow) ImGui::SameLine(0, gap);
      if (chip(list[i], st.stampAuto == 0 && st.stampChord == list[i], chipW, idPrefix)) picked = list[i];
    }
    return picked;
  };
  ImGui::PushFont(a.fontBold, theme::kFontBody);
  ImGui::TextUnformatted(i18n::tr("Chord stamp"));
  ImGui::PopFont();
  ImGui::SameLine(0, theme::kS2);
  ImGui::TextDisabled("%s", i18n::tr("Draw and Paint place it, its root on the clicked key"));
  int picked = -1;
  const std::vector<int> rec = recent(a);
  if (!rec.empty()) {
    ImGui::Spacing();
    ui::Label("Recent");
    if (int p = row(rec, "r"); p >= 0) picked = p;
  }
  for (const Group& g : groups()) {
    ImGui::Spacing();
    ui::Label(g.title);
    if (int p = row(g.chords, "g"); p >= 0) picked = p;
  }
  ImGui::Spacing();
  ui::Label("From the scale");
  const bool haveScale = st.scale > 0;
  ImGui::BeginDisabled(!haveScale);
  const float halfW = std::floor((chipW * kPerRow + gap * (kPerRow - 1) - gap) * 0.5f);
  if (ui::TextButton("Clicked note at the bottom##sauto1", st.stampAuto == 1, halfW, "A chord of the scale's notes, the clicked note at the bottom")) {
    st.stampAuto = 1, st.stampChord = -1;
    if (st.tool != Tool::Draw && st.tool != Tool::Paint) st.tool = Tool::Draw;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine(0, gap);
  if (ui::TextButton("Clicked note on top##sauto2", st.stampAuto == 2, halfW, "A chord of the scale's notes, the clicked note on top")) {
    st.stampAuto = 2, st.stampChord = -1;
    if (st.tool != Tool::Draw && st.tool != Tool::Paint) st.tool = Tool::Draw;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  if (!haveScale) ImGui::TextDisabled("%s", i18n::tr("Choose a scale in the toolbar to stamp its chords"));
  ImGui::Spacing();
  ui::Label("Inversion");
  static const char* const inv[] = {i18n::N_("Root"), i18n::N_("1st"), i18n::N_("2nd"), i18n::N_("3rd")};
  static const char* const invHint[] = {i18n::N_("Root position"), i18n::N_("1st inversion: the lowest note goes up an octave"),
                                        i18n::N_("2nd inversion: the two lowest notes go up an octave"),
                                        i18n::N_("3rd inversion: the three lowest notes go up an octave")};
  for (int k = 0; k < 4; ++k) {
    if (k) ImGui::SameLine(0, gap);
    ImGui::PushID(k);
    if (ui::TextButton(inv[k], st.stampInversion == k, chipW, invHint[k])) st.stampInversion = k;
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::Checkbox(i18n::tr("Just the next chord"), &st.stampOnce);
  ui::Hint("The stamp turns itself off after placing one chord");
  if (on(st)) {
    ImGui::SameLine(0, theme::kS4);
    if (ui::TextButton("Turn the stamp off", false, 0, "Draw and Paint place single notes again (Esc)")) {
      turnOff(st);
      ImGui::CloseCurrentPopup();
    }
  }
  if (picked >= 0) {
    pick(a, picked);
    ImGui::CloseCurrentPopup();
  }
}

}  // namespace

bool on(const PianoRollState& st) { return st.stampChord >= 0 || st.stampAuto > 0; }

std::string shortLabel(const PianoRollState& st) {
  if (st.stampAuto > 0) return i18n::tr("Scale");
  if (st.stampChord < 0) return {};
  std::string s = symbolOf(st.stampChord);
  static const char* const inv[] = {"", " /1", " /2", " /3"};
  if (st.stampInversion > 0 && st.stampInversion < 4) s += inv[st.stampInversion];
  return s;
}

std::vector<int> recent(const App& a) {
  std::vector<int> out;
  const std::string s = a.settings.getString(kRecentKey);
  const int n = static_cast<int>(tools::chords().size());
  for (size_t pos = 0; pos < s.size() && static_cast<int>(out.size()) < kRecentMax;) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    const int c = std::atoi(s.substr(pos, end - pos).c_str());
    if (end > pos && c >= 0 && c < n && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    pos = end + 1;
  }
  return out;
}

void pick(App& a, int chord) {
  PianoRollState& st = a.piano;
  if (chord < 0 || chord >= static_cast<int>(tools::chords().size())) return;
  st.stampChord = chord;
  st.stampAuto = 0;
  if (st.tool != Tool::Draw && st.tool != Tool::Paint) st.tool = Tool::Draw;  // the stamp draws with them
  std::vector<int> r = recent(a);
  r.erase(std::remove(r.begin(), r.end(), chord), r.end());
  r.insert(r.begin(), chord);
  if (static_cast<int>(r.size()) > kRecentMax) r.resize(kRecentMax);
  saveRecent(a, r);
}

void turnOff(PianoRollState& st) {
  st.stampChord = -1;
  st.stampAuto = 0;
}

void button(App& a, bool roomForLabel) {
  PianoRollState& st = a.piano;
  const float fh = ImGui::GetFrameHeight();
  const bool stamping = on(st);
  ImGui::PushID("prstamp");
  bool openPicker = false;
  char hint[200];
  if (stamping)
    std::snprintf(hint, sizeof hint, i18n::tr("Chord stamp on (%s): click to turn it off (Esc), right-click to pick another chord"),
                  shortLabel(st).c_str());
  else
    std::snprintf(hint, sizeof hint, "%s", i18n::tr("Chord stamp: pick a chord for Draw and Paint to place (the clicked key is its root)"));
  if (ui::IconButton("stamp", ui::Icon::ChordStamp, stamping || ImGui::IsPopupOpen("##stamppick"), fh, hint, theme::rgb(theme::role::kStamp),
                     theme::rgb(theme::role::kStamp, 0.85f))) {
    if (stamping) turnOff(st);
    else openPicker = true;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) openPicker = true;
  devNoteRect("prstamp");
  const ImVec2 under(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2);
  if (stamping && roomForLabel) {  // the chord being stamped: a click picks another one
    ImGui::SameLine(0, 2);
    const std::string lbl = shortLabel(st) + "##stamplbl";
    // (lit while its picker is open, like a drop-down; the stamp icon beside it shows the stamp is on)
    if (ui::TextButton(lbl.c_str(), ImGui::IsPopupOpen("##stamppick"), 0, "The chord the stamp places: click to pick another"))
      openPicker = true;
    devNoteRect("prstamplbl");
  }
  // The keymap's "Chord stamp" command (Alt+C by default) opens the picker too.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && keys::pressed("piano.chordStamp")) openPicker = true;
  if (openPicker) ImGui::OpenPopup("##stamppick");
  ImGui::SetNextWindowPos(under, ImGuiCond_Appearing);
  if (ImGui::BeginPopup("##stamppick")) {
    picker(a);
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

void escapeTurnsOff(App& a) {
  if (!on(a.piano) || keys::escapeBusy()) return;
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
  if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return;
  turnOff(a.piano);
  keys::claimEscape();
}

}  // namespace gl::stampui
