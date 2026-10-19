// Gridline — channel rack: channels of the selected pattern with step
// sequencer rows (or a note preview when the channel has piano-roll notes).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "App.h"
#include "ContextMenus.h"
#include "EditTools.h"
#include "../core/NoteTools.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"

namespace gl {

using theme::pal;

namespace {

constexpr uint64_t kStepGestureKey = 0x5354455000000000ull;  // "STEP"

std::vector<uint8_t> rowOf(const Pattern& p, ObjectId ch) {
  std::vector<uint8_t> row;
  if (auto it = p.steps.find(ch); it != p.steps.end()) row = it->second;
  row.resize(static_cast<size_t>(p.bars * p.stepsPerBar), 0);
  return row;
}

// Accepts instruments and audio files dropped from the browser.
void acceptDrops(App& a) {
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PLUGIN")) {
      std::string type(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (const PluginInfo* info = findPlugin(type); info && info->kind == PluginKind::Generator) a.addInstrument(type);
    }
    // Browser files: samples, MIDI, channel presets, projects (J-16).
    browserui::acceptFileDrop(a, browse::DropTarget::Rack, {});
    ImGui::EndDragDropTarget();
  }
}

// Display group filter of the rack (-1 = all channels). Groups and membership
// are saved with the project (Song::channelGroups, Channel::group).
int groupFilter = -1;
char newGroupName[64] = "";

void groupMenu(App& a) {
  const Song& s = a.doc.song();
  auto countIn = [&](int g) {
    int n = 0;
    for (const auto& c : s.channels) n += c.group == g;
    return n;
  };
  if (ImGui::Selectable("All channels", groupFilter < 0)) groupFilter = -1;
  for (size_t g = 0; g < s.channelGroups.size(); ++g) {
    std::string l = s.channelGroups[g] + "  (" + std::to_string(countIn(static_cast<int>(g))) + ")##g" + std::to_string(g);
    if (ImGui::Selectable(l.c_str(), groupFilter == static_cast<int>(g))) groupFilter = static_cast<int>(g);
  }
  ImGui::Separator();
  const bool anySel = !a.selChannels.empty();
  auto assign = [&](int g, const char* label) {
    std::set<ObjectId> sel = a.selChannels;
    a.doc.editSong(label, [&](Song& song) {
      for (auto& c : song.channels)
        if (sel.count(c.id)) c.group = g;
    });
  };
  if (ImGui::BeginMenu("Add selected channels to", anySel && !s.channelGroups.empty())) {
    for (size_t g = 0; g < s.channelGroups.size(); ++g)
      if (ImGui::MenuItem((s.channelGroups[g] + "##m" + std::to_string(g)).c_str())) assign(static_cast<int>(g), "group channels");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("New group from selected", anySel)) {
    ImGui::SetNextItemWidth(160);
    bool enter = ImGui::InputTextWithHint("##ng", "Group name", newGroupName, sizeof newGroupName, ImGuiInputTextFlags_EnterReturnsTrue);
    if ((enter || ImGui::Button("Create")) && newGroupName[0]) {
      std::string name = newGroupName;
      std::set<ObjectId> sel = a.selChannels;
      int g = static_cast<int>(s.channelGroups.size());
      a.doc.editSong("new channel group", [&](Song& song) {
        song.channelGroups.push_back(name);
        for (auto& c : song.channels)
          if (sel.count(c.id)) c.group = g;
      });
      groupFilter = g;
      newGroupName[0] = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Remove selected from their group", nullptr, false, anySel)) assign(-1, "ungroup channels");
  if (groupFilter >= 0 && ImGui::BeginMenu("Rename this group")) {
    const int g = groupFilter;
    menus::renameField(s.channelGroups[static_cast<size_t>(g)], [&a, g](const std::string& n) { ops::renameChannelGroup(a.doc, g, n); });
    ImGui::EndMenu();
  }
  if (groupFilter >= 0 && ImGui::MenuItem("Delete this group", nullptr, false)) {
    int g = groupFilter;
    a.doc.editSong("delete channel group", [&](Song& song) {
      if (g < 0 || static_cast<size_t>(g) >= song.channelGroups.size()) return;
      song.channelGroups.erase(song.channelGroups.begin() + g);
      for (auto& c : song.channels) c.group = c.group == g ? -1 : (c.group > g ? c.group - 1 : c.group);
    });
    groupFilter = -1;
  }
}

// Graph editor lanes (J-11): the value each step bar shows and edits.
const char* const kLaneShort[] = {"VELOCITY", "PAN", "RELEASE", "FINE PITCH", "MOD X", "MOD Y", "SHIFT", "KEYBOARD"};
// Keyboard lane: pitches shown over five octaves from C2 (36).
constexpr int kKeyLo = 36, kKeySpan = 60;
double laneNorm(uint8_t vel, const StepProps& p, tools::StepLane l) {
  if (l == tools::StepLane::Pitch) return std::clamp(((p.pitch < 0 ? 60 : p.pitch) - kKeyLo) / static_cast<double>(kKeySpan), 0.0, 1.0);
  return tools::stepLaneNorm(vel, p, l);
}
void setLaneNorm(uint8_t& vel, StepProps& p, tools::StepLane l, double v) {
  if (l == tools::StepLane::Pitch) {
    int pitch = std::clamp(kKeyLo + static_cast<int>(std::lround(v * kKeySpan)), 0, 127);
    p.pitch = pitch == 60 ? -1 : pitch;
    return;
  }
  tools::setStepLaneNorm(vel, p, l, v);
}
std::string laneText(uint8_t vel, const StepProps& p, tools::StepLane l) {
  char b[48];
  switch (l) {
    case tools::StepLane::Pan:
      if (p.pan == 0) return "centre";
      std::snprintf(b, sizeof b, "%d%% %s", std::abs(p.pan) * 100 / kNotePanMax, p.pan < 0 ? "left" : "right");
      return b;
    case tools::StepLane::Release: return std::to_string(p.release);
    case tools::StepLane::Fine: std::snprintf(b, sizeof b, "%+d cents", p.fine); return b;
    case tools::StepLane::ModX: return std::to_string(p.modX * 100 / kNoteModMax) + "%";
    case tools::StepLane::ModY: return std::to_string(p.modY * 100 / kNoteModMax) + "%";
    case tools::StepLane::Shift: return std::to_string(p.shift) + "% of a step";
    case tools::StepLane::Pitch: return tools::pitchName(p.pitch < 0 ? 60 : p.pitch);
    default: return std::to_string(vel);
  }
}

}  // namespace

void drawChannelRack(App& a) {
  if (!a.showRack) return;
  if (!ui::BeginDockWindow("Channel rack", &a.showRack, ImVec2(760, 360))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  // Ctrl+K (rack focused): toggle the graph editor (works when the header is
  // too narrow to show its button).
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
      ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K))
    a.rack.showGraph = !a.rack.showGraph;

  // ---------------- header: menu · display group · pattern · (right) swing · length · steps · graph · add ----------------
  ui::BeginToolbar("##racktb");
  {
    const float W = ImGui::GetContentRegionAvail().x;
    const float fh = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Width budget (most important first): add, menu, pattern name, length, group, graph, steps/bar, swing.
    const float wMenu = fh + 4, wGroup = 92 + 8, wLen = 20 + 50 + 20 + 9, wSpb = 88 + 9, wSwing = 22 + 4 + 40 + 9, wGraph = fh + 9,
                wAdd = fh + 4, minName = 64;
    float need = wMenu + minName + wAdd;
    auto fits = [&](float w) {
      if (need + w > W) return false;
      need += w;
      return true;
    };
    const bool showLen = pat && fits(wLen);
    const bool showGroup = fits(wGroup);
    const bool showSwing = pat && fits(wSwing);
    const bool showGraphBtn = fits(wGraph);
    const bool showSpb = pat && fits(wSpb);
    const float nameW = std::clamp(W - need + minName, minName, 200.0f);

    menuButton("rackmenu", "Channel rack menu: selected channels, sorting, colours", [&a]() { rackOptionsMenu(a); });
    ImGui::SameLine(0, 4);
    if (showGroup) {
      if (groupFilter >= static_cast<int>(s.channelGroups.size())) groupFilter = -1;
      if (ui::BeginCombo("##group", groupFilter < 0 ? "All" : s.channelGroups[static_cast<size_t>(groupFilter)].c_str(), 92)) {
        groupMenu(a);
        ImGui::EndCombo();
      }
      ui::Hint("Display group: show all channels or one group (groups are saved with the project)");
      ImGui::SameLine(0, 8);
    }
    // Pattern chip: colour + name.
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(nameW, fh));
      if (pat) dl->AddRectFilled(ImVec2(p.x, p.y + 5), ImVec2(p.x + 4, p.y + fh - 5), theme::rgb(pat->color), 1);
      theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(p.x + 10, p.y + (fh - theme::kFontBody) * 0.5f - 1), nameW - 14,
                          P.text, pat ? pat->name.c_str() : "No pattern");
      ui::Hint("Pattern being edited (pick another in the top bar or the playlist)");
    }
    // Right cluster, laid out from the right edge.
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    float rx = right - (wAdd - 4);
    const float y0 = ImGui::GetCursorScreenPos().y;
    ImGui::SameLine(0, 0);
    const float rowY = ImGui::GetCursorScreenPos().y;
    (void)y0;
    ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
    if (ui::IconButton("add", ui::Icon::Plus, false, fh, "Add an instrument channel")) ImGui::OpenPopup("##addch");
    if (ImGui::BeginPopup("##addch")) {
      addChannelMenu(a);
      ImGui::EndPopup();
    }
    if (showGraphBtn) {
      rx -= wGraph;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
      if (ui::IconButton("graph", ui::Icon::Wave, a.rack.showGraph, fh,
                         "Graph editor (Ctrl+K): step velocity, pan, release, fine pitch, mod X / Y, shift and keyboard (note) of the selected channel",
                         theme::rgb(theme::role::kGraph)))
        a.rack.showGraph = !a.rack.showGraph;
    }
    if (pat && showSpb) {
      rx -= wSpb;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
      const int spbChoices[] = {12, 16, 24, 32};
      int sel = 1;
      for (int i = 0; i < 4; ++i)
        if (spbChoices[i] == pat->stepsPerBar) sel = i;
      const char* spbNames[] = {"12 / bar", "16 / bar", "24 / bar", "32 / bar"};
      if (ui::BeginCombo("##spb", spbNames[sel], 88)) {
        for (int i = 0; i < 4; ++i)
          if (ImGui::Selectable(spbNames[i], i == sel)) a.doc.setPatternLength(pat->id, pat->bars, spbChoices[i]);
        ImGui::EndCombo();
      }
      ui::Hint("Steps per bar");
    }
    if (pat && showLen) {
      rx -= wLen;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
      int bars = pat->bars;
      if (ui::IconButton("barsdn", ui::Icon::Minus, false, 20, "Shorter pattern") && bars > 1)
        a.doc.setPatternLength(pat->id, bars - 1, pat->stepsPerBar);
      ImGui::SameLine(0, 0);
      char buf[24];
      std::snprintf(buf, sizeof buf, "%d bar%s", bars, bars == 1 ? "" : "s");
      ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(50, fh));
      ui::Lcd(dl, ImVec2(q.x + 1, q.y + 2), ImVec2(48, fh - 4), buf, ImGui::IsItemHovered(), false);
      ui::Hint("Pattern length in bars");
      ImGui::SameLine(0, 0);
      if (ui::IconButton("barsup", ui::Icon::Plus, false, 20, "Longer pattern") && bars < 64)
        a.doc.setPatternLength(pat->id, bars + 1, pat->stepsPerBar);
    }
    if (pat && showSwing) {
      rx -= wSwing;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY + (fh - 22) * 0.5f));
      ui::ParamKnob("swing", songParam(SongParam::Swing), 11.0f);
      ImGui::SameLine(0, 4);
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowY));
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "Swing");
    }
  }
  ui::EndToolbar();

  // ---------------- rows ----------------
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.dark ? theme::mix(P.surface900, P.surface800, 0.35f) : P.surface900));
  ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int total = pat ? pat->bars * pat->stepsPerBar : 16;
  // Channel button proportions after the reference rack: mute lamp · pan · volume
  // · mixer track box · name button · steps · select lamp.
  const float rowH = 26.0f, rowGap = 3.0f, cellH = 20.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float stepGap = 2.0f, groupGap = 4.0f, selLampW = 14.0f;
  const int groups = (total + 3) / 4;
  // Narrow racks drop the pan/volume knobs (still in channel settings) so the
  // steps keep a usable size; names shrink before steps do, then the rack scrolls.
  const bool compact = avail < 96 + 72 + 8 + total * (11 + stepGap) + groups * groupGap + selLampW;
  const float xLed = 2, xPan = 18, xVol = 40, xMix = compact ? 18 : 64, nameX = compact ? 48 : 94;
  float nameW = std::clamp(avail - nameX - 8 - selLampW - total * (16 + stepGap) - groups * groupGap, compact ? 64.0f : 72.0f, 150.0f);
  float stepsX = nameX + nameW + 8;
  float stepW = std::floor((avail - stepsX - selLampW - groups * groupGap) / std::max(1, total)) - stepGap;
  stepW = std::clamp(stepW, 9.0f, 22.0f);
  auto stepPos = [&](int k) { return k * (stepW + stepGap) + (k / 4) * groupGap; };
  const float stepsW = stepPos(total - 1) + stepW;
  int playStep = -1;
  if (pat && a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == pat->id) {
    Tick st = s.ticksPerStep(*pat);
    if (st > 0) playStep = static_cast<int>(a.engine.position() / static_cast<double>(st));
  }
  // Playing-step lamps above the steps.
  if (pat && !s.channels.empty()) {
    ImVec2 lp = ImGui::GetCursorScreenPos();
    for (int k = 0; k < total; ++k) {
      float x0 = lp.x + stepsX + stepPos(k);
      bool lit = k == playStep;
      dl->AddRectFilled(ImVec2(x0 + 1, lp.y + 1), ImVec2(x0 + stepW - 1, lp.y + 5),
                        lit ? P.led : theme::mix(P.field, (k / 4) % 2 ? P.stepOffAlt : P.stepOff, 0.35f), 1.5f);
    }
    ImGui::Dummy(ImVec2(stepsX + stepsW, 8));
    ui::Hint("Playing step");
  }
  static int paintValue = -1;  // -1 idle, else velocity to paint (0 = erase)
  static ObjectId paintRow = kNoId;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && paintValue >= 0) {
    paintValue = -1;
    paintRow = kNoId;
    a.doc.endGesture();
  }

  a.rack.hoverStep = -1;  // set again by the step under the pointer
  for (size_t ci = 0; ci < s.channels.size(); ++ci) {
    const Channel& c = s.channels[ci];
    if (groupFilter >= 0 && c.group != groupFilter) continue;  // display group filter
    ImGui::PushID(static_cast<int>(c.id));
    ImVec2 rp = ImGui::GetCursorScreenPos();
    if (a.rack.zipped.count(c.id)) {  // J-11 zipped row: a thin strip; click unzips
      const float zh = 7.0f;
      ImGui::InvisibleButton("zip", ImVec2(stepsX + stepsW, zh));
      const bool zhov = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) a.rack.zipped.erase(c.id);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        ImGui::OpenPopup("##chmenu");
      }
      if (ui::HintHovered()) ui::Hint(c.name + " (zipped: click to unzip)");
      dl->AddRectFilled(ImVec2(rp.x + nameX, rp.y + 1), ImVec2(rp.x + stepsX + stepsW, rp.y + zh - 1),
                        theme::withAlpha(theme::rgb(c.color), zhov ? 0.75f : 0.45f), 2);
      if (ImGui::BeginPopup("##chmenu")) {
        channelContextMenu(a, c.id);
        ImGui::EndPopup();
      }
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + zh + 2));
      ImGui::Dummy(ImVec2(stepsX + stepsW + selLampW, 0));
      ImGui::PopID();
      continue;
    }
    // Rows outside the visible area only take their space (large racks: J-25),
    // unless one of the row's menus is open or a step paint runs on it.
    if (!ImGui::IsRectVisible(rp, ImVec2(rp.x + stepsX + stepsW + selLampW, rp.y + rowH)) && paintRow != c.id &&
        !ImGui::IsPopupOpen("##chmenu") && !ImGui::IsPopupOpen("##route")) {
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
      ImGui::Dummy(ImVec2(stepsX + stepsW + selLampW, 0));
      ImGui::PopID();
      continue;
    }
    const float cy = rp.y + rowH * 0.5f;
    bool selected = a.selChannels.count(c.id) > 0;
    if (selected) dl->AddRectFilled(rp, ImVec2(rp.x + stepsX + stepsW + selLampW, rp.y + rowH), theme::withAlpha(P.paper, 0.035f), theme::kRadius);

    // Mute lamp (Ctrl+click: solo).
    {
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xLed, cy - 8));
      if (ImGui::InvisibleButton("mute", ImVec2(16, 16))) {
        if (ImGui::GetIO().KeyCtrl) {
          a.doc.beginGroup("solo channel");
          for (const auto& o : s.channels) a.doc.setParam(channelParam(o.id, ChannelParam::Mute), o.id == c.id ? 0 : 1, false);
          a.doc.endGroup();
        } else {
          a.doc.setParam(channelParam(c.id, ChannelParam::Mute), c.muted ? 0 : 1, false);
        }
      }
      ui::Hint(c.muted ? "Unmute channel (Ctrl+click: solo)" : "Mute channel (Ctrl+click: solo)");
      float lvl = 0;
      if (auto rt = a.builder.channelRuntime(c.id)) lvl = rt->level.exchange(0);
      ui::Led(dl, ImVec2(rp.x + xLed + 8, cy), 4.5f, !c.muted, std::min(1.0f, lvl * 3));
    }
    // Pan & volume knobs.
    if (!compact) {
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xPan, cy - 9));
      ui::ParamKnob("pan", channelParam(c.id, ChannelParam::Pan), 9.0f, nullptr, true);
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xVol, cy - 9));
      ui::ParamKnob("vol", channelParam(c.id, ChannelParam::Volume), 9.0f);
    }
    // Mixer track box: drag to change, right-click to pick.
    {
      const float boxW = 24.0f;
      ImVec2 bp(rp.x + xMix, cy - cellH * 0.5f);
      ImGui::SetCursorScreenPos(bp);
      ImGui::InvisibleButton("mt", ImVec2(boxW, cellH));
      int mi = std::max(0, s.mixerIndex(c.mixerTrack));
      bool hov = ImGui::IsItemHovered();
      if (ImGui::IsItemActive() && std::fabs(ImGui::GetMouseDragDelta().y) > 8) {
        int delta = -static_cast<int>(ImGui::GetMouseDragDelta().y / 8);
        ImGui::ResetMouseDragDelta();
        int ni = std::clamp(mi + delta, 0, static_cast<int>(s.mixer.size()) - 1);
        if (ni != mi)
          a.doc.editChannel(c.id, "route channel to mixer track", [&](Channel& ch) { ch.mixerTrack = s.mixer[static_cast<size_t>(ni)].id; },
                            0x4D54000000000000ull ^ c.id);
      }
      if (ImGui::IsItemActive() || hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##route");
      if (ImGui::BeginPopup("##route")) {
        for (size_t i = 0; i < s.mixer.size(); ++i) {
          std::string label = (i == 0 ? std::string("Master") : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##" + std::to_string(i);
          if (ImGui::MenuItem(label.c_str(), nullptr, c.mixerTrack == s.mixer[i].id)) a.doc.routeChannel(c.id, s.mixer[i].id);
        }
        ImGui::EndPopup();
      }
      dl->AddRectFilled(bp, ImVec2(bp.x + boxW, bp.y + cellH), hov ? theme::mix(P.field, P.surface700, 0.5f) : P.field, theme::kRadiusSm);
      dl->AddRect(bp, ImVec2(bp.x + boxW, bp.y + cellH), theme::withAlpha(P.ink, 0.35f), theme::kRadiusSm);
      char buf[8];
      std::snprintf(buf, sizeof buf, "%s", mi == 0 ? "M" : std::to_string(mi).c_str());
      ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontNumber, 1e9f, 0, buf);
      dl->AddText(a.fontMono, theme::kFontNumber, ImVec2(std::round(bp.x + boxW * 0.5f - ts.x * 0.5f), std::round(bp.y + (cellH - ts.y) * 0.5f)),
                  hov ? P.hot : P.textDim, buf);
      ui::Hint("Mixer track: drag up/down to change, right-click to pick (M = master)");
    }
    // Name button: raised face, channel colour on the left edge.
    {
      ImVec2 np(rp.x + nameX, cy - cellH * 0.5f);
      ImGui::SetCursorScreenPos(np);
      ImGui::InvisibleButton("name", ImVec2(nameW, cellH));
      bool hov = ImGui::IsItemHovered();
      // Click (without dragging): select the channel and toggle its settings / info
      // window, like the reference channel button. No sound; Ctrl+click only adds to the selection.
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) a.selectChannel(c.id, ImGui::GetIO().KeyCtrl);
      if (ImGui::IsItemDeactivated() && !ImGui::GetIO().KeyCtrl &&
          ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x +
                  ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y <
              16.0f)
        a.settingsChannel = a.settingsChannel == c.id ? kNoId : c.id;
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        ImGui::OpenPopup("##chmenu");
      }
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {  // reorder
        ImGui::SetDragDropPayload("GL_CHANNEL", &c.id, sizeof c.id);
        ui::DragPreview(ui::Icon::Rack, ("Move " + c.name).c_str(), theme::content(c.color));
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CHANNEL")) {
          ObjectId moving = *static_cast<const ObjectId*>(pl->Data);
          a.doc.moveChannel(moving, static_cast<int>(ci));
        }
        ImGui::EndDragDropTarget();
      }
      const bool open = a.settingsChannel == c.id;
      ImU32 bg = selected ? P.surface600 : hov ? P.surface700 : P.surface800;
      if (open) bg = theme::mix(bg, P.hot, 0.10f);
      dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH), theme::mix(bg, P.ink, 0.3f), theme::kRadiusSm);
      dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH - 1), bg, theme::kRadiusSm);
      dl->AddLine(ImVec2(np.x + 5, np.y + 0.5f), ImVec2(np.x + nameW - 2, np.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.12f : 0.5f));
      dl->AddRectFilled(np, ImVec2(np.x + 4, np.y + cellH), theme::content(c.color), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(np.x + 10, std::round(np.y + (cellH - 13) * 0.5f) - 1),
                          nameW - 14, c.muted ? P.textFaint : selected && P.dark ? P.paper : P.text, c.name.c_str());
      if (ui::HintHovered()) {
        const PluginInfo* info = findPlugin(c.plugin.type);
        ui::Hint(c.name + (info ? "  \xC2\xB7  " + info->name : "") +
               "   (click: open / close its settings \xC2\xB7 Ctrl+click: add to selection \xC2\xB7 right-click: menu \xC2\xB7 drag: reorder)");
      }
      if (ImGui::BeginPopup("##chmenu")) {
        channelContextMenu(a, c.id);
        ImGui::EndPopup();
      }
    }

    // Steps or note preview.
    ImVec2 sp(rp.x + stepsX, cy - cellH * 0.5f);
    bool hasNotes = pat && pat->notes.count(c.id) && !pat->notes.at(c.id).empty();
    if (pat && hasNotes) {
      ImGui::SetCursorScreenPos(sp);
      ImGui::InvisibleButton("notes", ImVec2(stepsW, cellH));
      bool hov = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) a.openPianoRoll(c.id);
      ui::Hint("Piano-roll notes: click to edit them in the piano roll");
      // Mini piano-roll well (the reference rack shows note rows as an overview strip).
      dl->AddRectFilled(sp, ImVec2(sp.x + stepsW, sp.y + cellH), hov ? theme::mix(P.field, P.surface700, 0.4f) : P.field, theme::kRadiusSm);
      const auto& ns = pat->notes.at(c.id);
      int lo = 127, hi = 0;
      for (const auto& n : ns) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
      float span = static_cast<float>(std::max(hi - lo, 6));
      Tick len = std::max<Tick>(1, s.patternContentLength(*pat));
      dl->PushClipRect(sp, ImVec2(sp.x + stepsW, sp.y + cellH), true);
      for (Tick b = s.barTicks(); b < len; b += std::max<Tick>(1, s.barTicks())) {
        float x = std::round(sp.x + stepsW * static_cast<float>(b) / static_cast<float>(len));
        dl->AddLine(ImVec2(x, sp.y + 2), ImVec2(x, sp.y + cellH - 2), theme::withAlpha(P.lineStrong, 0.6f));
      }
      const ImU32 nc = theme::mix(theme::rgb(c.color), P.paper, 0.25f);
      for (const auto& n : ns) {
        float x0 = sp.x + stepsW * static_cast<float>(n.start) / static_cast<float>(len);
        float x1 = sp.x + stepsW * static_cast<float>(n.start + n.length) / static_cast<float>(len);
        float y = sp.y + 3 + (cellH - 9) * (1.0f - (n.pitch - lo) / span);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x0 + 2, x1 - 1), y + 3), nc, 1);
      }
      if (playStep >= 0) {
        float x = sp.x + stepsW * static_cast<float>(a.engine.position()) / static_cast<float>(len);
        dl->AddLine(ImVec2(x, sp.y), ImVec2(x, sp.y + cellH), P.playhead);
      }
      dl->PopClipRect();
    } else if (pat) {
      auto row = rowOf(*pat, c.id);
      const float visX0 = dl->GetClipRectMin().x, visX1 = dl->GetClipRectMax().x;
      for (int k = 0; k < total; ++k) {
        ImVec2 q(sp.x + stepPos(k), sp.y);
        if (q.x + stepW < visX0 || q.x > visX1) continue;  // scrolled out of view (long patterns)
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(k);
        ImGui::InvisibleButton("s", ImVec2(stepW, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hov) a.rack.hoverChannel = c.id, a.rack.hoverStep = k;
        uint8_t v = row[static_cast<size_t>(k)];
        if (ImGui::IsItemActivated()) {
          paintRow = c.id;
          paintValue = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || v > 0 ? 0 : 100;
        }
        if (paintValue >= 0 && hov && paintRow == c.id && v != paintValue) {
          auto nr = row;
          nr[static_cast<size_t>(k)] = static_cast<uint8_t>(paintValue);
          bool empty = std::all_of(nr.begin(), nr.end(), [](uint8_t x) { return x == 0; });
          a.doc.editPattern(pat->id, paintValue ? "add steps" : "remove steps",
                            [&](Pattern& p) {
                              if (empty)
                                p.steps.erase(c.id);
                              else
                                p.steps[c.id] = nr;
                            },
                            kStepGestureKey ^ pat->id);
          row = nr;
          v = static_cast<uint8_t>(paintValue);
        }
        // Mouse wheel over an active step adjusts its velocity.
        if (hov && v > 0 && ImGui::GetIO().MouseWheel != 0) {
          auto nr = row;
          nr[static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(v + static_cast<int>(ImGui::GetIO().MouseWheel * 8), 1, 127));
          a.doc.editPattern(pat->id, "change step velocity", [&](Pattern& p) { p.steps[c.id] = nr; }, kStepGestureKey ^ (pat->id + 7));
        }
        const bool alt = (k / 4) % 2 == 1;
        ImVec2 q1(q.x + stepW, q.y + cellH);
        ui::StepPad(dl, q, q1, v > 0, alt, hov, theme::rgb(c.color), v / 127.0f);
        if (v > 0 && pat->stepProps.count(c.id)) {  // J-11: steps with their own note / parameters get a corner mark
          const auto& pr = pat->stepProps.at(c.id);
          if (static_cast<size_t>(k) < pr.size() && !pr[static_cast<size_t>(k)].isDefault())
            dl->AddTriangleFilled(ImVec2(q1.x - 6, q.y + 1), ImVec2(q1.x - 1, q.y + 1), ImVec2(q1.x - 1, q.y + 6),
                                  theme::withAlpha(theme::rgb(theme::role::kGraph), 0.9f));
        }
        if (k == playStep) dl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q1.x + 1, q1.y + 1), P.playhead, theme::kRadius, 0, 1.5f);
        if (hov)
          app().setHint("Step " + std::to_string(k + 1) + (v ? " \xC2\xB7 velocity " + std::to_string(v) + " (scroll to change)" : "") +
                        "   (click/drag: paint \xC2\xB7 right-drag: erase \xC2\xB7 Insert / Delete: insert or remove a step here)");
        ImGui::PopID();
      }
    }
    // Select lamp (right of the steps): lit for selected channels; click toggles the selection.
    {
      ImVec2 l0(rp.x + stepsX + stepsW + 5, cy - cellH * 0.5f + 2), l1(l0.x + 6, cy + cellH * 0.5f - 2);
      ImGui::SetCursorScreenPos(ImVec2(l0.x - 3, l0.y - 2));
      if (ImGui::InvisibleButton("sel", ImVec2(12, cellH))) a.selectChannel(c.id, true);
      ui::Hint(selected ? "Selected (click to deselect)" : "Click to add this channel to the selection");
      dl->AddRectFilled(l0, l1, selected ? P.led : theme::mix(P.field, P.led, 0.10f), 2);
      if (selected) dl->AddRectFilled(ImVec2(l0.x - 2, l0.y - 2), ImVec2(l1.x + 2, l1.y + 2), theme::withAlpha(P.led, 0.18f), 3);
    }
    ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
    ImGui::Dummy(ImVec2(stepsX + stepsW + selLampW, 0));
    ImGui::PopID();
  }

  // Add-channel button under the channel buttons.
  if (!s.channels.empty()) {
    ImVec2 bp(ImGui::GetCursorScreenPos().x + nameX, ImGui::GetCursorScreenPos().y + 2);
    ImGui::SetCursorScreenPos(bp);
    if (ImGui::InvisibleButton("##addrow", ImVec2(nameW, cellH))) ImGui::OpenPopup("##addch2");
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(bp, ImVec2(bp.x + nameW, bp.y + cellH), hov ? P.surface700 : theme::withAlpha(P.surface800, 0.6f), theme::kRadiusSm);
    ui::drawIcon(dl, ui::Icon::Plus, ImVec2(bp.x + nameW * 0.5f, bp.y + cellH * 0.5f), 10, hov ? P.hot : P.textDim);
    ui::Hint("Add an instrument channel (or drop an instrument / audio file onto the rack)");
    if (ImGui::BeginPopup("##addch2")) {
      addChannelMenu(a);
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(bp.x - nameX, bp.y + cellH + 6));
    ImGui::Dummy(ImVec2(1, 0));
  } else {
    ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ui::EmptyState(dl, mn, ImVec2(mn.x + sz.x, mn.y + sz.y), "No channels yet",
                   "Add an instrument with + or drag one in from the browser");
  }

  // Graph editor (J-11): one step parameter of the selected channel, aligned
  // with the steps: velocity, pan, release, fine pitch, mod X / Y, shift, or
  // the keyboard lane (each step's note).
  const Channel* gc = s.channel(a.selChannel);
  if (a.rack.showGraph && pat && gc && !(pat->notes.count(gc->id) && !pat->notes.at(gc->id).empty())) {
    const auto lane = static_cast<tools::StepLane>(std::clamp(a.rack.graphLane, 0, static_cast<int>(tools::StepLane::Count) - 1));
    const bool keys = lane == tools::StepLane::Pitch;
    const bool bip = lane == tools::StepLane::Pan || lane == tools::StepLane::Fine;
    const float gh = keys ? 110.0f : 76.0f;
    ImVec2 g0 = ImGui::GetCursorScreenPos();
    ImVec2 w0(g0.x + stepsX, g0.y), w1(g0.x + stepsX + stepsW, g0.y + gh);
    dl->AddRectFilled(ImVec2(g0.x, g0.y), ImVec2(w1.x, w1.y), P.field, theme::kRadius);
    // Lane picker on the left.
    ImGui::SetCursorScreenPos(ImVec2(g0.x + 6, g0.y + 4));
    ImGui::PushID("graphlane");
    if (ui::BeginCombo("##lane", kLaneShort[static_cast<int>(lane)], std::min(120.0f, stepsX - 14))) {
      for (int k = 0; k < static_cast<int>(tools::StepLane::Count); ++k)
        if (ImGui::Selectable(tools::stepLaneName(static_cast<tools::StepLane>(k)), k == static_cast<int>(lane))) a.rack.graphLane = k;
      ImGui::EndCombo();
    }
    ui::Hint("Graph editor lane: the step parameter shown and edited (Keyboard: each step's note)");
    ImGui::PopID();
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(g0.x + 8, g0.y + 32), stepsX - 16, P.text, gc->name.c_str());
    tools::StepRow row;
    tools::StepPropRow props;
    ops::readStepRow(*pat, gc->id, row, props);
    const float top = w0.y + 6, bot = w1.y - 6, span = bot - top;
    if (keys) {  // keyboard lane: octave lines and C labels
      for (int pch = kKeyLo; pch <= kKeyLo + kKeySpan; pch += 12) {
        float y = std::round(bot - span * static_cast<float>(pch - kKeyLo) / kKeySpan);
        dl->AddLine(ImVec2(w0.x, y), ImVec2(w1.x, y), theme::withAlpha(P.lineStrong, 0.45f));
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(w0.x - 30, y - 6), P.textFaint, tools::pitchName(pch).c_str());
      }
    } else {
      for (float f : {0.25f, 0.5f, 0.75f}) {
        float y = std::round(bot - span * f);
        dl->AddLine(ImVec2(w0.x, y), ImVec2(w1.x, y), theme::withAlpha(P.lineStrong, bip && f == 0.5f ? 0.7f : 0.35f));
      }
    }
    ImGui::SetCursorScreenPos(w0);
    ImGui::InvisibleButton("##graph", ImVec2(stepsW, gh));
    const ImVec2 m = ImGui::GetIO().MousePos;
    int hk = -1;
    for (int i = 0; i < total; ++i)
      if (m.x >= w0.x + stepPos(i) - stepGap && m.x < w0.x + stepPos(i) + stepW + groupGap) hk = i;
    const ObjectId cid = gc->id;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hk >= 0 && row[static_cast<size_t>(hk)] > 0) {
      double v = std::clamp((bot - m.y) / span, 0.0f, 1.0f);
      if (bip && std::fabs(v - 0.5) < 0.03) v = 0.5;
      uint8_t vel = row[static_cast<size_t>(hk)];
      StepProps sp = props[static_cast<size_t>(hk)];
      setLaneNorm(vel, sp, lane, v);
      if (vel != row[static_cast<size_t>(hk)] || !(sp == props[static_cast<size_t>(hk)])) {
        const int k = hk;
        ops::editStepRow(a.doc, pat->id, cid, keys ? "set step note" : "change step parameter",
                         [&](tools::StepRow& r, tools::StepPropRow& pr) { r[static_cast<size_t>(k)] = vel, pr[static_cast<size_t>(k)] = sp; },
                         kStepGestureKey ^ (pat->id + 11));
        row[static_cast<size_t>(hk)] = vel;
        props[static_cast<size_t>(hk)] = sp;
        if (keys) a.engine.noteOn(cid, sp.pitch < 0 ? 60 : sp.pitch, 0.7f), a.engine.noteOff(cid, sp.pitch < 0 ? 60 : sp.pitch);
      }
    }
    // Right-click resets the step's value to the default.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hk >= 0 && row[static_cast<size_t>(hk)] > 0) {
      const int k = hk;
      uint8_t vel = row[static_cast<size_t>(k)];
      StepProps sp = props[static_cast<size_t>(k)];
      const StepProps d;
      uint8_t dv = 100;
      setLaneNorm(dv, sp, lane, laneNorm(100, d, lane));
      if (lane == tools::StepLane::Velocity) vel = 100;
      ops::editStepRow(a.doc, pat->id, cid, "reset step parameter",
                       [&](tools::StepRow& r, tools::StepPropRow& pr) { r[static_cast<size_t>(k)] = vel, pr[static_cast<size_t>(k)] = sp; });
    }
    // Keyboard lane: the wheel moves the hovered step by a semitone.
    if (keys && ImGui::IsItemHovered() && hk >= 0 && row[static_cast<size_t>(hk)] > 0 && ImGui::GetIO().MouseWheel != 0) {
      const int k = hk;
      StepProps sp = props[static_cast<size_t>(k)];
      int pitch = std::clamp((sp.pitch < 0 ? 60 : sp.pitch) + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 0, 127);
      sp.pitch = pitch == 60 ? -1 : pitch;
      ops::editStepRow(a.doc, pat->id, cid, "set step note", [&](tools::StepRow&, tools::StepPropRow& pr) { pr[static_cast<size_t>(k)] = sp; },
                       kStepGestureKey ^ (pat->id + 13));
      props[static_cast<size_t>(k)] = sp;
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (hk >= 0 && ImGui::IsItemHovered())
      ui::Hint(std::string(tools::stepLaneName(lane)) + " of step " + std::to_string(hk + 1) + ": " +
               (row[static_cast<size_t>(hk)] ? laneText(row[static_cast<size_t>(hk)], props[static_cast<size_t>(hk)], lane) : std::string("(step off)")) +
               "   (drag: set \xC2\xB7 right-click: default" + (keys ? " \xC2\xB7 wheel: semitone)" : ")"));
    const ImU32 bar = theme::litRole(theme::role::kGraph);
    for (int k = 0; k < total; ++k) {
      uint8_t v = row[static_cast<size_t>(k)];
      if (!v) continue;
      const float x0 = w0.x + stepPos(k) + 1, x1 = x0 + stepW - 2;
      const float f = static_cast<float>(laneNorm(v, props[static_cast<size_t>(k)], lane));
      const float y = bot - span * f;
      if (keys) {  // keyboard lane: a note block at the step's pitch
        dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + 3), bar, 2);
        if (stepW >= 14 && k % 1 == 0) {
          std::string nn = tools::pitchName(props[static_cast<size_t>(k)].pitch < 0 ? 60 : props[static_cast<size_t>(k)].pitch);
          ImGui::PushFont(a.fontMono, 9.0f);
          float tw = ImGui::CalcTextSize(nn.c_str()).x;
          if (tw < stepW + 4) dl->AddText(ImVec2(std::round((x0 + x1 - tw) * 0.5f), y - 15), P.textDim, nn.c_str());
          ImGui::PopFont();
        }
      } else {
        const float base = bip ? bot - span * 0.5f : bot;
        dl->AddRectFilled(ImVec2(x0, std::min(y, base)), ImVec2(x1, std::max(y, base)), theme::withAlpha(bar, 0.35f), 1);
        dl->AddRectFilled(ImVec2(x0, y - 1), ImVec2(x1, y + 1), bar, 1);
      }
      if (k == hk) dl->AddRect(ImVec2(x0 - 1, top - 2), ImVec2(x1 + 1, bot + 2), theme::withAlpha(bar, 0.5f), 2);
    }
    ImGui::SetCursorScreenPos(ImVec2(g0.x, w1.y + 6));
    ImGui::Dummy(ImVec2(stepsX + stepsW, 0));
  }
  // Insert / Delete over a step: insert or remove a step there (later steps move).
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && pat && a.rack.hoverStep >= 0 && s.channel(a.rack.hoverChannel) &&
      !ImGui::GetIO().WantTextInput) {
    const int k = a.rack.hoverStep;
    const ObjectId cid = a.rack.hoverChannel;
    if (ImGui::IsKeyPressed(ImGuiKey_Insert, false) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
      ops::editStepRow(a.doc, pat->id, cid, "insert step", [k](tools::StepRow& r, tools::StepPropRow& p) { tools::insertStep(r, p, k); });
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
      ops::editStepRow(a.doc, pat->id, cid, "delete step", [k](tools::StepRow& r, tools::StepPropRow& p) { tools::deleteStep(r, p, k); });
  }
  ImGui::EndChild();
  acceptDrops(a);
  ui::EndDockWindow();
}

}  // namespace gl
