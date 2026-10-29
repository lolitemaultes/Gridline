// Gridline — channel rack: channels of the selected pattern with step
// sequencer rows (or a note preview when the channel has piano-roll notes).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include "CollabUi.h"
#include "CursorAnchor.h"
#include "../core/I18n.h"
#include "../core/ExternalDrop.h"
#include "../core/RackLayout.h"
#include "../core/WheelRoute.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "HostedUi.h"
#include "ContextMenus.h"
#include "EditTools.h"
#include "../core/NoteTools.h"
#include "Theme.h"
#include "Widgets.h"
#include "SmoothScrollUi.h"
#include "Windows.h"
#include "BrowserUi.h"
#include "PatternAutoUi.h"

namespace gl {

using theme::pal;

namespace {

constexpr uint64_t kStepGestureKey = 0x5354455000000000ull;  // "STEP"

// J-84 dev check (--dev-input "rack"): what the rack drew last frame.
struct RackShown {
  racklayout::Layout layout;
  int rows = 0, knobRows = 0, truncated = 0;
  float scroll = 0, rowsScrollY = 0, rowsScrollMaxY = 0;
  bool wheelScrolls = false;
  float originX = 0, row0Y = -1, viewX0 = 0, graphY = -1;  // screen geometry for scripted input
  float rowPitch = 0;                                       // J-97: from one row to the next
};
RackShown gShown;
struct RackDev {
  int steps = 0;
  float w = 0, h = 0, scroll = -1;
  bool dock = false;
  int playStep = -1;  // J-137 "play<k>": the song-position outline on step k (stills of the play highlight)
} gRackDev;

std::vector<uint8_t> rowOf(const Pattern& p, ObjectId ch) {
  std::vector<uint8_t> row;
  if (auto it = p.steps.find(ch); it != p.steps.end()) row = it->second;
  row.resize(static_cast<size_t>(p.bars * p.stepsPerBar), 0);
  return row;
}

// Accepts instruments and audio files dropped from the browser on the rack's
// free area (new channels; rows replace / insert, see rowDropTarget).
void acceptDrops(App& a) {
  if (ImGui::BeginDragDropTarget()) {
    browserui::acceptPluginDrop(a, browse::DropTarget::Rack, {});  // instruments: a new channel
    // Browser files: samples, MIDI, channel presets, projects (J-16).
    browserui::acceptFileDrop(a, browse::DropTarget::Rack, {});
    ImGui::EndDragDropTarget();
  }
}

// J-97: FL-style drop onto a channel row. The middle of the row replaces the
// channel's content (sample, instrument, preset; routing / volume / pan /
// steps / notes stay), its top and bottom edges (and the gap between rows)
// insert new channels there. Draws the row highlight with a "Replace <name>"
// badge, or an insertion line.
void rowDropTarget(App& a, const Channel& c, int index, ImVec2 rp, float rowW, float rowH, float rowGap, float badgeX) {
  const ImGuiPayload* cur = ImGui::GetDragDropPayload();
  if (!cur || !(cur->IsDataType("GL_FILE") || cur->IsDataType("GL_ITEM") || cur->IsDataType("GL_FILES") || cur->IsDataType("GL_PLUGIN") ||
                cur->IsDataType("GL_CLIPSRC")))  // J-110: audio clip sources (recordings) from the playlist picker
    return;
  rowGap += ImGui::GetStyle().ItemSpacing.y;  // the rows' pitch: no dead band between two rows
  const ImRect r(ImVec2(rp.x, rp.y - rowGap * 0.5f), ImVec2(rp.x + rowW, rp.y + rowH + rowGap * 0.5f));
  if (!ImGui::BeginDragDropTargetCustom(r, ImGui::GetID("##rowdrop"))) return;
  const xdrop::RowZone zone = xdrop::rowZone(ImGui::GetIO().MousePos.y, r.Min.y, r.GetHeight());
  browserui::DropPlace place;
  if (zone == xdrop::RowZone::Replace) place.channel = c.id;
  else place.insertAt = zone == xdrop::RowZone::Before ? index : index + 1;
  browserui::DropFeedback fb;
  fb.highlight = false;
  browserui::acceptFileDrop(a, browse::DropTarget::Rack, place, &fb);
  browserui::acceptPluginDrop(a, browse::DropTarget::Rack, place, &fb);
  ImGui::EndDragDropTarget();
  if (!fb.hovering) return;
  // What the row shows: replace (the default), the notes going in, or new channels below (Shift / extra files).
  bool replace = zone == xdrop::RowZone::Replace, below = zone == xdrop::RowZone::After;
  auto named = [&c](const char* f) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, c.name.c_str());
    return std::string(buf);
  };
  std::string badge;
  if (replace) {
    badge = named(i18n::tr("Replace %s"));
    if (const browserui::HoverPlan* hp = browserui::hoverPlan(browse::DropTarget::Rack); hp && !hp->plan.steps.empty()) {
      const browse::DropAction act = hp->plan.steps.front().action;
      if (act == browse::DropAction::ImportMidiNotes) {
        badge = named(i18n::tr("Notes into %s"));
      } else if (act == browse::DropAction::NewSamplerChannel || act == browse::DropAction::NewChannelFromPreset) {
        replace = false;
        below = true;
      }
    }
  }
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 hot = P.hot;
  ImFont* font = ImGui::GetFont();
  const float fs = theme::kFontCaption + 1;
  if (replace) {
    const ImRect b(ImVec2(rp.x + 1, rp.y), ImVec2(rp.x + rowW - 1, rp.y + rowH));
    dl->AddRectFilled(b.Min, b.Max, theme::withAlpha(hot, 0.14f), theme::kRadiusSm);
    dl->AddRect(b.Min, b.Max, theme::withAlpha(hot, 0.95f), theme::kRadiusSm, 0, 2.0f);
  } else {
    const float y = std::round(below ? rp.y + rowH + rowGap * 0.5f : rp.y - rowGap * 0.5f);
    dl->AddLine(ImVec2(rp.x + 2, y), ImVec2(rp.x + rowW - 2, y), hot, 3.0f);
    dl->AddCircleFilled(ImVec2(rp.x + 4, y), 4.0f, hot);
    badge = i18n::tr("Insert new channel here");
  }
  const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0, badge.c_str());
  const float bh = ts.y + 4, by = replace ? std::round(rp.y + (rowH - bh) * 0.5f) : std::round((below ? rp.y + rowH : rp.y) - bh * 0.5f);
  const ImVec2 b0(std::round(rp.x + badgeX), by), b1(b0.x + ts.x + 12, by + bh);
  dl->AddRectFilled(b0, b1, hot, bh * 0.5f);
  dl->AddText(font, fs, ImVec2(b0.x + 6, b0.y + 2), theme::contrastText(hot), badge.c_str());
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
  if (ImGui::Selectable(i18n::tr("All channels"), groupFilter < 0)) groupFilter = -1;
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
  if (ImGui::BeginMenu(i18n::tr("Add selected channels to"), anySel && !s.channelGroups.empty())) {
    for (size_t g = 0; g < s.channelGroups.size(); ++g)
      if (ImGui::MenuItem((s.channelGroups[g] + "##m" + std::to_string(g)).c_str())) assign(static_cast<int>(g), "group channels");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("New group from selected"), anySel)) {
    ImGui::SetNextItemWidth(160);
    bool enter = ImGui::InputTextWithHint("##ng", i18n::tr("Group name"), newGroupName, sizeof newGroupName, ImGuiInputTextFlags_EnterReturnsTrue);
    if ((enter || ImGui::Button(i18n::tr("Create"))) && newGroupName[0]) {
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
  if (ImGui::MenuItem(i18n::tr("Remove selected from their group"), nullptr, false, anySel)) assign(-1, "ungroup channels");
  if (groupFilter >= 0 && ImGui::BeginMenu(i18n::tr("Rename this group"))) {
    const int g = groupFilter;
    menus::renameField(s.channelGroups[static_cast<size_t>(g)], [&a, g](const std::string& n) { ops::renameChannelGroup(a.doc, g, n); });
    ImGui::EndMenu();
  }
  if (groupFilter >= 0 && ImGui::MenuItem(i18n::tr("Delete this group"), nullptr, false)) {
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
      keys::pressed("rack.graphEditor"))
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
      theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(p.x + 10, std::round(p.y + (fh - theme::kFontBody) * 0.5f)), nameW - 14,
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
      // J-128: both buttons on the read-out's centre line
      const float by = rowY + std::floor((fh - 20) * 0.5f);
      ImGui::SetCursorScreenPos(ImVec2(rx, by));
      int bars = pat->bars;
      if (ui::IconButton("barsdn", ui::Icon::Minus, false, 20, "Shorter pattern") && bars > 1)
        a.doc.setPatternLength(pat->id, bars - 1, pat->stepsPerBar);
      char buf[24];
      std::snprintf(buf, sizeof buf, "%d bar%s", bars, bars == 1 ? "" : "s");
      const ImVec2 q(rx + 20, rowY);
      ImGui::SetCursorScreenPos(q);
      ImGui::Dummy(ImVec2(50, fh));
      ui::Lcd(dl, ImVec2(q.x + 1, q.y + 2), ImVec2(48, fh - 4), buf, ImGui::IsItemHovered(), false);
      ui::Hint("Pattern length in bars");
      ImGui::SetCursorScreenPos(ImVec2(rx + 70, by));
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
      ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Swing"));
      // J-138: song swing moves steps and piano roll notes; projects from .flp files swing steps only.
      ui::Hint(a.doc.song().swingNotes ? "Song swing moves steps and piano roll notes (right-click: steps only)"
                                       : "Song swing moves steps only (right-click: piano roll notes too)");
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##swingnotes");
      if (ImGui::BeginPopup("##swingnotes")) {
        const bool notes = a.doc.song().swingNotes;
        if (ImGui::MenuItem(i18n::tr("Swing piano roll notes"), nullptr, notes))
          a.doc.editSong(notes ? "swing steps only" : "swing notes too", [](Song& x) { x.swingNotes = !x.swingNotes; });
        ImGui::EndPopup();
      }
    }
  }
  ui::EndToolbar();

  // ---------------- rows ----------------
  // J-84: the left section (mute LED, pan, volume, mixer track, name) keeps
  // its place at every width and pattern length; the step area takes the rest
  // and scrolls sideways under the time scroller in the bottom bar (the
  // reference rack's behaviour), instead of the old compact mode that dropped
  // the pan / volume knobs (core/RackLayout.h).
  const int total = pat ? pat->bars * pat->stepsPerBar : 16;
  // Bottom bar (the reference rack's bottom panel): the add-channel button
  // under the channel names and, for long patterns, the time scroller under
  // the steps. It takes the place of the old add row, so the rack's default
  // height (J-28) is unchanged.
  constexpr float kBarH = 28.0f;
  const bool showBar = !s.channels.empty();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.dark ? theme::mix(P.surface900, P.surface800, 0.35f) : P.surface900));
  // The horizontal scrollbar only shows for a rack narrower than its left section plus a few steps.
  ImGui::BeginChild("##rows", ImVec2(0, showBar ? -kBarH : 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Channel button proportions after the reference rack: mute lamp · pan · volume
  // · mixer track box · name button · steps · select lamp.
  const float rowH = 26.0f, rowGap = 3.0f, cellH = 20.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  a.rack.lastAvail = avail;
  const racklayout::Layout L = racklayout::compute({avail, total});
  const float xLed = racklayout::kXLed, xPan = racklayout::kXPan, xVol = racklayout::kXVol, xMix = racklayout::kXMix,
              nameX = racklayout::kNameX, nameW = L.nameW, stepsX = L.stepsX, stepW = L.stepW, stepGap = racklayout::kStepGap,
              groupGap = racklayout::kGroupGap, viewW = L.viewW, rowW = L.rowW;
  auto stepPos = [&L](int k) { return L.stepPos(k); };
  const float stepsW = L.contentW;
  int playStep = -1;
  if (pat && a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == pat->id) {
    Tick st = s.ticksPerStep(*pat);
    if (st > 0) playStep = static_cast<int>(a.engine.position() / static_cast<double>(st));
  }
  if (gRackDev.playStep >= 0) playStep = gRackDev.playStep;  // dev stills (--rack play<k>)
  // Step scroll: kept in range (shorter pattern, wider rack); while the
  // pattern plays the view pages along with the playing step.
  scrollui::clampOuter(a.rack.axStep, a.rack.stepScroll, 0, L.scrollMax);
  if (playStep >= 0 && playStep != a.rack.lastPlayStep && L.scrolls() && !a.rack.thumbDrag) {
    const float x0 = L.stepPos(playStep);
    if (x0 < a.rack.stepScroll || x0 + stepW > a.rack.stepScroll + viewW) a.rack.stepScroll = racklayout::clampScroll(L, x0);
  }
  a.rack.lastPlayStep = playStep;
  const float scrollX = a.rack.stepScroll;  // drawn step scroll (may be elastically past an edge)
  const float originX = ImGui::GetCursorScreenPos().x;
  const float viewX0 = originX + stepsX, viewX1 = viewX0 + viewW;  // step viewport (screen)
  a.rack.viewX0 = viewX0, a.rack.viewX1 = viewX1;
  a.rack.originX = originX;
  auto stepClipBegin = [&](float y0, float y1) { ImGui::PushClipRect(ImVec2(viewX0, y0), ImVec2(viewX1, y1), true); };

  // J-83 / J-84 wheel routing: while the rack scrolls (its list vertically or
  // its steps sideways) the plain wheel scrolls it; a step's velocity, a
  // keyboard-lane note and the pan / volume knobs take the wheel after a click
  // on them or with Ctrl / Cmd (Settings > Interface, "Mouse wheel over controls").
  const bool rowsScroll = ImGui::GetScrollMaxY() > 0;
  const bool rackScrolls = wheelroute::rackScrolls(wheelroute::modeFromSetting(a.settings.getNumber("ui.mixerWheel")), rowsScroll, L.scrolls());
  ui::WheelPolicyScope wheelScope(rackScrolls);
  bool wheelUsed = false;  // a step / lane took this frame's wheel (adjusting rack)

  // Playing-step lamps above the steps.
  if (pat && !s.channels.empty()) {
    ImVec2 lp = ImGui::GetCursorScreenPos();
    stepClipBegin(lp.y, lp.y + 8);
    for (int k = 0; k < total; ++k) {
      float x0 = lp.x + stepsX + stepPos(k) - scrollX;
      if (x0 + stepW < viewX0 || x0 > viewX1) continue;
      bool lit = k == playStep;
      dl->AddRectFilled(ImVec2(x0 + 1, lp.y + 1), ImVec2(x0 + stepW - 1, lp.y + 5),
                        lit ? P.led : theme::mix(P.field, (k / 4) % 2 ? P.stepOffAlt : P.stepOff, 0.35f), 1.5f);
    }
    ImGui::PopClipRect();
    ImGui::Dummy(ImVec2(rowW, 8));
    ui::Hint("Playing step");
  }
  static int paintValue = -1;  // -1 idle, else velocity to paint (0 = erase)
  static ObjectId paintRow = kNoId;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && paintValue >= 0) {
    paintValue = -1;
    paintRow = kNoId;
    a.doc.endGesture();
  }

  RackShown shown;
  a.rack.hoverStep = -1;  // set again by the step under the pointer
  for (size_t ci = 0; ci < s.channels.size(); ++ci) {
    const Channel& c = s.channels[ci];
    if (groupFilter >= 0 && c.group != groupFilter) continue;  // display group filter
    ImGui::PushID(static_cast<int>(c.id));
    ImVec2 rp = ImGui::GetCursorScreenPos();
    if (a.rack.zipped.count(c.id)) {  // J-11 zipped row: a thin strip; click unzips
      const float zh = 7.0f;
      ImGui::InvisibleButton("zip", ImVec2(L.lampX, zh));
      const bool zhov = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) a.rack.zipped.erase(c.id);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        ImGui::OpenPopup("##chmenu");
      }
      if (ui::HintHovered()) ui::Hint(c.name + " (zipped: click to unzip)");
      dl->AddRectFilled(ImVec2(rp.x + nameX, rp.y + 1), ImVec2(rp.x + L.lampX, rp.y + zh - 1),
                        theme::withAlpha(theme::rgb(c.color), zhov ? 0.75f : 0.45f), 2);
      if (ImGui::BeginPopup("##chmenu")) {
        channelContextMenu(a, c.id);
        ImGui::EndPopup();
      }
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + zh + 2));
      ImGui::Dummy(ImVec2(rowW, 0));
      ImGui::PopID();
      continue;
    }
    // Rows outside the visible area only take their space (large racks: J-25),
    // unless one of the row's menus is open or a step paint runs on it.
    if (!ImGui::IsRectVisible(rp, ImVec2(rp.x + rowW, rp.y + rowH)) && paintRow != c.id &&
        !ImGui::IsPopupOpen("##chmenu") && !ImGui::IsPopupOpen("##route")) {
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
      ImGui::Dummy(ImVec2(rowW, 0));
      ImGui::PopID();
      continue;
    }
    ++shown.rows;
    const float cy = rp.y + rowH * 0.5f;
    if (shown.row0Y < 0) shown.row0Y = cy;
    shown.rowPitch = rowH + rowGap + ImGui::GetStyle().ItemSpacing.y;
    bool selected = a.selChannels.count(c.id) > 0;
    if (selected) dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), theme::withAlpha(P.paper, 0.035f), theme::kRadius);

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
    // Pan & volume knobs: always shown (J-84).
    {
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xPan, cy - 9));
      ui::ParamKnob("pan", channelParam(c.id, ChannelParam::Pan), 9.0f, nullptr, true);
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xVol, cy - 9));
      ui::ParamKnob("vol", channelParam(c.id, ChannelParam::Volume), 9.0f);
      ++shown.knobRows;
    }
    // Mixer track box: drag to change, right-click to pick.
    {
      const float boxW = racklayout::kMixW;
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
        togglePluginWindow(a, PluginTarget{c.id, -1});
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        ImGui::OpenPopup("##chmenu");
      }
      // J-84: a name cut short by a narrow rack shows in full in a tooltip.
      const float nameFont = theme::kFontCaption + 1;
      const bool truncated = a.fontRegular->CalcTextSizeA(nameFont, 1e9f, 0, c.name.c_str()).x > nameW - 14;
      if (truncated) {
        ++shown.truncated;
        if (!ImGui::IsPopupOpen("##chmenu")) ui::Tooltip(c.name.c_str());
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
        ImGui::EndDragDropTarget();  // browser files / plugins: the whole row (rowDropTarget, J-97)
      }
      const bool open = pluginWindowShown(a, PluginTarget{c.id, -1});
      ImU32 bg = selected ? P.surface600 : hov ? P.surface700 : P.surface800;
      if (open) bg = theme::mix(bg, P.hot, 0.10f);
      dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH), theme::mix(bg, P.ink, 0.3f), theme::kRadiusSm);
      dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH - 1), bg, theme::kRadiusSm);
      dl->AddLine(ImVec2(np.x + 5, np.y + 0.5f), ImVec2(np.x + nameW - 2, np.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.12f : 0.5f));
      dl->AddRectFilled(np, ImVec2(np.x + 4, np.y + cellH), theme::content(c.color), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      theme::textEllipsis(dl, a.fontRegular, nameFont, ImVec2(np.x + 10, std::round(np.y + (cellH - 13) * 0.5f) - 1),
                          nameW - 14, c.muted ? P.textFaint : selected && P.dark ? P.paper : P.text, c.name.c_str());
      {  // J-111: a collaborator's change flashes; a sample still arriving shows its progress
        ImU32 fc = 0;
        if (const float fl = collabui::flash(a, "channels/#" + std::to_string(c.id), &fc); fl > 0)
          dl->AddRect(ImVec2(np.x + 1, np.y + 1), ImVec2(np.x + nameW - 1, np.y + cellH - 1), theme::withAlpha(fc, fl), theme::kRadiusSm, 0, 2.0f);  // J-137: inside
        float pr = 0;
        if (collabui::fileLoading(a, c.plugin.data, &pr)) {
          collabui::drawLoading(dl, ImVec2(np.x + 5, np.y + cellH - 4), ImVec2(np.x + nameW - 2, np.y + cellH - 1), pr, true);
          if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("The sample is still arriving from a collaborator: silent until it is here"));
        }
      }
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

    // Steps or note preview, inside the step viewport (scrolled by scrollX).
    ImVec2 sp(rp.x + stepsX - scrollX, cy - cellH * 0.5f);
    bool hasNotes = pat && pat->notes.count(c.id) && !pat->notes.at(c.id).empty();
    stepClipBegin(rp.y, rp.y + rowH);
    if (pat && hasNotes) {
      ImGui::SetCursorScreenPos(ImVec2(viewX0, sp.y));
      ImGui::InvisibleButton("notes", ImVec2(viewW, cellH));
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
    } else if (pat) {
      auto row = rowOf(*pat, c.id);
      for (int k = 0; k < total; ++k) {
        ImVec2 q(sp.x + stepPos(k), sp.y);
        if (q.x + stepW <= viewX0 || q.x >= viewX1) continue;  // scrolled out of the step viewport (long patterns)
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(k);
        ImGui::InvisibleButton("s", ImVec2(stepW, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hov) a.rack.hoverChannel = c.id, a.rack.hoverStep = k;
        uint8_t v = row[static_cast<size_t>(k)];
        // Q-15: in a scrolling rack the plain wheel scrolls; the step takes it after a click or with Ctrl / Cmd.
        const ui::WheelGate wg = ui::wheelGate(v > 0);
        if (ImGui::IsItemActivated() && wg.dragOk) {
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
        // Mouse wheel over an active step adjusts its velocity (scrolling rack: after a click / with Ctrl).
        if (wg.scale > 0 && v > 0 && ImGui::GetIO().MouseWheel != 0) {
          auto nr = row;
          const int dv = static_cast<int>(std::lround(ImGui::GetIO().MouseWheel * 8 * wg.scale));
          nr[static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(v + (dv != 0 ? dv : (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)), 1, 127));
          a.doc.editPattern(pat->id, "change step velocity", [&](Pattern& p) { p.steps[c.id] = nr; }, kStepGestureKey ^ (pat->id + 7));
          wheelUsed = true;
        }
        const bool alt = (k / 4) % 2 == 1;
        ImVec2 q1(q.x + stepW, q.y + cellH);
        ui::StepPad(dl, q, q1, v > 0, alt, hov, theme::rgb(c.color), v / 127.0f);
        // J-31 step loop: steps past the channel's loop length repeat the loop (shaded; the first one marked).
        if (auto lp = pat->stepLoop.find(c.id); lp != pat->stepLoop.end() && lp->second > 0 && k >= lp->second) {
          dl->AddRectFilled(q, q1, theme::withAlpha(P.surface950, 0.55f), 2.0f);
          if (k == lp->second) dl->AddLine(ImVec2(q.x - 1, q.y - 2), ImVec2(q.x - 1, q1.y + 2), theme::lit(theme::rgb(theme::role::kLoop)), 2.0f);
        }
        if (v > 0 && pat->stepProps.count(c.id)) {  // J-11: steps with their own note / parameters get a corner mark
          const auto& pr = pat->stepProps.at(c.id);
          if (static_cast<size_t>(k) < pr.size() && !pr[static_cast<size_t>(k)].isDefault())
            dl->AddTriangleFilled(ImVec2(q1.x - 6, q.y + 1), ImVec2(q1.x - 1, q.y + 1), ImVec2(q1.x - 1, q.y + 6),
                                  theme::withAlpha(theme::rgb(theme::role::kGraph), 0.9f));
        }
        if (k == playStep) ui::StepPlayOutline(dl, q, q1);  // J-137: inside the pad (the step viewport's clip cut the first column's left edge)
        if (hov)
          app().setHint("Step " + std::to_string(k + 1) +
                        (v ? " \xC2\xB7 velocity " + std::to_string(v) + " (" + (wg.hint ? i18n::tr("click or Ctrl+wheel to change") : i18n::tr("scroll to change")) + ")"
                           : std::string()) +
                        "   (click/drag: paint \xC2\xB7 right-drag: erase \xC2\xB7 Insert / Delete: insert or remove a step here)");
        ImGui::PopID();
      }
    }
    ImGui::PopClipRect();
    // Scrolled steps end under a soft edge (more steps to either side).
    if (L.scrolls()) {
      const ImU32 edge = theme::withAlpha(P.surface950, 0.0f), edgeIn = theme::withAlpha(P.surface950, 0.55f);
      if (scrollX > 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(viewX0, sp.y), ImVec2(viewX0 + 6, sp.y + cellH), edgeIn, edge, edge, edgeIn);
      if (scrollX < L.scrollMax - 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(viewX1 - 6, sp.y), ImVec2(viewX1, sp.y + cellH), edge, edgeIn, edgeIn, edge);
    }
    // Select lamp (right of the step viewport): lit for selected channels; click toggles the selection.
    {
      ImVec2 l0(rp.x + L.lampX + 5, cy - cellH * 0.5f + 2), l1(l0.x + 6, cy + cellH * 0.5f - 2);
      ImGui::SetCursorScreenPos(ImVec2(l0.x - 3, l0.y - 2));
      if (ImGui::InvisibleButton("sel", ImVec2(12, cellH))) a.selectChannel(c.id, true);
      ui::Hint(selected ? "Selected (click to deselect)" : "Click to add this channel to the selection");
      dl->AddRectFilled(l0, l1, selected ? P.led : theme::mix(P.field, P.led, 0.10f), 2);
      if (selected) dl->AddRectFilled(ImVec2(l0.x - 2, l0.y - 2), ImVec2(l1.x + 2, l1.y + 2), theme::withAlpha(P.led, 0.18f), 3);
    }
    rowDropTarget(a, c, static_cast<int>(ci), rp, rowW, rowH, rowGap, stepsX + 6);  // J-97: drop to replace / insert
    // J-111d: a collaborator's pointer on this row (logical px from its start: the row's layout is fixed from the left)
    canchor::publish(canchor::RackRow, c.id, 0, rp, ImVec2(1.0f, rowH), rp, ImVec2(rp.x + rowW, rp.y + rowH));
    ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
    ImGui::Dummy(ImVec2(rowW, 0));
    ImGui::PopID();
  }
  // J-173: the pattern's automation clips as rows below the channels, and a
  // drop row while an automation clip is dragged from the clip sources.
  if (pat) {
    const ObjectId patId = pat->id;
    patautoui::RackRows rr{rowW, rowH, rowGap, cellH, xLed, nameX, nameW, stepsX, stepsW, scrollX, viewX0, viewX1};
    if (a.engine.playing() && a.engine.mode() == PlayMode::Pattern && a.engine.pattern() == patId) rr.playTick = a.engine.position();
    patautoui::drawRackRows(a, patId, rr);
    patautoui::rackDropRow(a, patId, rowW, rowH);
    pat = s.pattern(patId);  // a row's menu may have replaced the pattern
  }

  // (The add-channel button lives in the bottom bar, next to the time scroller.)
  if (s.channels.empty()) {
    ImVec2 mn = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ui::EmptyState(dl, mn, ImVec2(mn.x + sz.x, mn.y + sz.y), "No channels yet",
                   "Add an instrument with + or drag one in from the browser");
  }

  // Graph editor (J-11): one step parameter of the selected channel, aligned
  // with the steps: velocity, pan, release, fine pitch, mod X / Y, shift, or
  // the keyboard lane (each step's note). It scrolls with the steps (J-84).
  const Channel* gc = s.channel(a.selChannel);
  if (a.rack.showGraph && pat && gc && !(pat->notes.count(gc->id) && !pat->notes.at(gc->id).empty())) {
    const auto lane = static_cast<tools::StepLane>(std::clamp(a.rack.graphLane, 0, static_cast<int>(tools::StepLane::Count) - 1));
    const bool keys = lane == tools::StepLane::Pitch;
    const bool bip = lane == tools::StepLane::Pan || lane == tools::StepLane::Fine;
    const float gh = keys ? 110.0f : 76.0f;
    ImVec2 g0 = ImGui::GetCursorScreenPos();
    shown.graphY = g0.y;
    ImVec2 w0(g0.x + stepsX - scrollX, g0.y), w1(g0.x + stepsX + viewW, g0.y + gh);  // w0.x: step 0 (scrolled); w1.x: view end
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
    if (keys) {  // keyboard lane: octave lines and C labels (labels stay left of the viewport)
      for (int pch = kKeyLo; pch <= kKeyLo + kKeySpan; pch += 12) {
        float y = std::round(bot - span * static_cast<float>(pch - kKeyLo) / kKeySpan);
        dl->AddLine(ImVec2(viewX0, y), ImVec2(viewX1, y), theme::withAlpha(P.lineStrong, 0.45f));
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(viewX0 - 30, y - 6), P.textFaint, tools::pitchName(pch).c_str());
      }
    } else {
      for (float f : {0.25f, 0.5f, 0.75f}) {
        float y = std::round(bot - span * f);
        dl->AddLine(ImVec2(viewX0, y), ImVec2(viewX1, y), theme::withAlpha(P.lineStrong, bip && f == 0.5f ? 0.7f : 0.35f));
      }
    }
    ImGui::SetCursorScreenPos(ImVec2(viewX0, w0.y));
    ImGui::InvisibleButton("##graph", ImVec2(viewW, gh));
    const ImVec2 m = ImGui::GetIO().MousePos;
    int hk = -1;
    if (m.x >= viewX0 && m.x < viewX1)
      for (int i = 0; i < total; ++i)
        if (m.x >= w0.x + stepPos(i) - stepGap && m.x < w0.x + stepPos(i) + stepW + groupGap) hk = i;
    const ObjectId cid = gc->id;
    // Q-15: in a scrolling rack the keyboard lane takes the wheel after a click on it or with Ctrl / Cmd.
    const ui::WheelGate wg = ui::wheelGate(keys && hk >= 0 && row[static_cast<size_t>(hk)] > 0);
    if (ImGui::IsItemActive() && wg.dragOk && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hk >= 0 && row[static_cast<size_t>(hk)] > 0) {
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
    if (wg.scale > 0 && ImGui::GetIO().MouseWheel != 0) {
      const int k = hk;
      StepProps sp = props[static_cast<size_t>(k)];
      int pitch = std::clamp((sp.pitch < 0 ? 60 : sp.pitch) + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 0, 127);
      sp.pitch = pitch == 60 ? -1 : pitch;
      ops::editStepRow(a.doc, pat->id, cid, "set step note", [&](tools::StepRow&, tools::StepPropRow& pr) { pr[static_cast<size_t>(k)] = sp; },
                       kStepGestureKey ^ (pat->id + 13));
      props[static_cast<size_t>(k)] = sp;
      wheelUsed = true;
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (hk >= 0 && ImGui::IsItemHovered())
      ui::Hint(std::string(tools::stepLaneName(lane)) + " of step " + std::to_string(hk + 1) + ": " +
               (row[static_cast<size_t>(hk)] ? laneText(row[static_cast<size_t>(hk)], props[static_cast<size_t>(hk)], lane) : std::string("(step off)")) +
               "   (drag: set \xC2\xB7 right-click: default" +
               (keys ? std::string(" \xC2\xB7 ") + (wg.hint ? i18n::tr("click or Ctrl+wheel: semitone") : i18n::tr("wheel: semitone")) + ")" : std::string(")")));
    const ImU32 bar = theme::litRole(theme::role::kGraph);
    stepClipBegin(g0.y, w1.y);
    for (int k = 0; k < total; ++k) {
      uint8_t v = row[static_cast<size_t>(k)];
      if (!v) continue;
      const float x0 = w0.x + stepPos(k) + 1, x1 = x0 + stepW - 2;
      if (x1 < viewX0 || x0 > viewX1) continue;
      const float f = static_cast<float>(laneNorm(v, props[static_cast<size_t>(k)], lane));
      const float y = bot - span * f;
      if (keys) {  // keyboard lane: a note block at the step's pitch
        dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + 3), bar, 2);
        if (stepW >= 14) {
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
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(g0.x, w1.y + 6));
    ImGui::Dummy(ImVec2(rowW, 0));
  }
  // Insert / Delete over a step: insert or remove a step there (later steps move).
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && pat && a.rack.hoverStep >= 0 && s.channel(a.rack.hoverChannel) &&
      !ImGui::GetIO().WantTextInput) {
    const int k = a.rack.hoverStep;
    const ObjectId cid = a.rack.hoverChannel;
    if (keys::pressed("rack.insertStep"))
      ops::editStepRow(a.doc, pat->id, cid, "insert step", [k](tools::StepRow& r, tools::StepPropRow& p) { tools::insertStep(r, p, k); });
    if (keys::pressed("rack.deleteStep"))
      ops::editStepRow(a.doc, pat->id, cid, "delete step", [k](tools::StepRow& r, tools::StepPropRow& p) { tools::deleteStep(r, p, k); });
  }
  // Sideways step scroll (J-84, smooth with elastic edges like every view, J-67):
  // a horizontal wheel / trackpad swipe or Shift+wheel anywhere over the rows;
  // over the step area of a rack whose list does not scroll vertically the
  // plain wheel scrolls the steps too (unless a step took it).
  float wheelDx = 0;
  bool wheelNotch = false;
  if (L.scrolls() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive()) {
    const ImGuiIO& io = ImGui::GetIO();
    float h = io.MouseWheelH;
    if (io.MouseWheelRequestAxisSwap) h = io.MouseWheel;
    const ImVec2 m = io.MousePos;
    if (h == 0 && !rowsScroll && !wheelUsed && m.x >= viewX0 && m.x < viewX1 && ImGui::TestKeyOwner(ImGuiKey_MouseWheelY, ImGuiKeyOwner_NoOwner))
      h = io.MouseWheel;
    if (h != 0) {
      wheelDx = -h * 4 * (stepW + stepGap);  // a notch: one beat
      wheelNotch = smoothscroll::isNotch(h);
    }
  }
  scrollui::step(a.rack.axStep, a.rack.stepScroll, wheelDx, wheelNotch, 0, L.scrollMax);
  shown.layout = L;
  shown.rowsScrollY = ImGui::GetScrollY();
  shown.rowsScrollMaxY = ImGui::GetScrollMaxY();
  shown.wheelScrolls = rackScrolls;
  shown.scroll = a.rack.stepScroll;
  shown.originX = originX;
  shown.viewX0 = viewX0;
  gShown = shown;
  ImGui::EndChild();
  acceptDrops(a);
  // Bottom bar: add-channel button under the names, time scroller under the
  // steps (bar ticks, the playing position and a thumb for the visible part).
  if (showBar) {
    ImDrawList* bdl = ImGui::GetWindowDrawList();
    const ImVec2 b0(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);  // flush under the rows (no item spacing)
    ImGui::SetCursorScreenPos(b0);
    const float bw = ImGui::GetContentRegionAvail().x;
    bdl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + kBarH), P.dark ? theme::mix(P.surface900, P.surface800, 0.35f) : P.surface900);
    const float cellH = 20.0f, by = std::round(b0.y + (kBarH - cellH) * 0.5f) - 2;
    {  // add channel
      const ImVec2 bp(a.rack.originX + racklayout::kNameX, by);
      ImGui::SetCursorScreenPos(bp);
      if (ImGui::InvisibleButton("##addrow", ImVec2(L.nameW, cellH))) ImGui::OpenPopup("##addch2");
      const bool hov = ImGui::IsItemHovered();
      bdl->AddRectFilled(bp, ImVec2(bp.x + L.nameW, bp.y + cellH), hov ? P.surface700 : theme::withAlpha(P.surface800, 0.6f), theme::kRadiusSm);
      ui::drawIcon(bdl, ui::Icon::Plus, ImVec2(bp.x + L.nameW * 0.5f, bp.y + cellH * 0.5f), 10, hov ? P.hot : P.textDim);
      ui::Hint("Add an instrument channel (or drop an instrument / audio file onto the rack)");
      if (ImGui::BeginPopup("##addch2")) {
        addChannelMenu(a);
        ImGui::EndPopup();
      }
    }
    float th0 = 0, thw = 0;
    const float tx0 = a.rack.viewX0, tw = std::max(8.0f, a.rack.viewX1 - a.rack.viewX0);
    if (racklayout::thumb(L, std::clamp(a.rack.stepScroll, 0.0f, L.scrollMax), tw, &th0, &thw)) {
      const ImVec2 t0(tx0, by + 5), t1(tx0 + tw, by + cellH - 5);
      ImGui::SetCursorScreenPos(ImVec2(t0.x, by));
      ImGui::InvisibleButton("##timescroll", ImVec2(tw, cellH));
      const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
      const float mx = ImGui::GetIO().MousePos.x - t0.x;
      if (ImGui::IsItemActivated()) {
        a.rack.thumbDrag = true;
        a.rack.thumbGrab = (mx >= th0 && mx <= th0 + thw) ? mx - th0 : thw * 0.5f;  // a click beside the thumb centres it there
      }
      if (act) a.rack.stepScroll = racklayout::scrollForTrack(L, mx - a.rack.thumbGrab + thw * 0.5f, tw);
      if (!act) a.rack.thumbDrag = false;
      if (hov && !act && (ImGui::GetIO().MouseWheel != 0 || ImGui::GetIO().MouseWheelH != 0)) {  // the wheel over the scroller scrolls the steps
        const float w = ImGui::GetIO().MouseWheelH != 0 ? ImGui::GetIO().MouseWheelH : ImGui::GetIO().MouseWheel;
        scrollui::step(a.rack.axStep, a.rack.stepScroll, -w * 4 * (L.stepW + racklayout::kStepGap), smoothscroll::isNotch(w), 0, L.scrollMax);
      }
      bdl->AddRectFilled(t0, t1, P.field, 3);
      if (pat && L.contentW > 0) {
        const int spb = std::max(1, pat->stepsPerBar);
        for (int b = 1; b * spb < total; ++b) {  // a tick per bar
          const float x = std::round(t0.x + tw * L.stepPos(b * spb) / L.contentW);
          bdl->AddLine(ImVec2(x, t0.y + 2), ImVec2(x, t1.y - 2), theme::withAlpha(P.lineStrong, 0.8f));
        }
        if (playStep >= 0) {
          const float x = std::round(t0.x + tw * (L.stepPos(playStep) + L.stepW * 0.5f) / L.contentW);
          bdl->AddLine(ImVec2(x, t0.y - 1), ImVec2(x, t1.y + 1), P.playhead, 1.5f);
        }
      }
      bdl->AddRectFilled(ImVec2(t0.x + th0, t0.y), ImVec2(t0.x + th0 + thw, t1.y),
                         act ? theme::withAlpha(P.hot, 0.6f) : theme::withAlpha(hov ? P.textDim : P.textFaint, 0.55f), 3);
      bdl->AddRect(ImVec2(t0.x + th0, t0.y), ImVec2(t0.x + th0 + thw, t1.y), theme::withAlpha(P.paper, 0.18f), 3);
      ui::Hint("Steps: drag to scroll the pattern (Shift+wheel or a sideways swipe over the steps also scrolls)");
    }
  }
  ui::EndDockWindow();
}

// ---------------- J-84 dev checks ----------------

void rackDevSpec(const std::string& spec) {
  size_t i = 0;
  while (i <= spec.size()) {
    size_t j = spec.find(',', i);
    if (j == std::string::npos) j = spec.size();
    const std::string k = spec.substr(i, j - i);
    if (k.rfind("steps", 0) == 0) gRackDev.steps = std::atoi(k.c_str() + 5);
    else if (k.rfind("scroll", 0) == 0) gRackDev.scroll = static_cast<float>(std::atof(k.c_str() + 6));
    else if (k == "dock") gRackDev.dock = true;
    else if (k.rfind("play", 0) == 0) gRackDev.playStep = std::atoi(k.c_str() + 4);
    else if (k.rfind("w", 0) == 0) gRackDev.w = static_cast<float>(std::atof(k.c_str() + 1));
    else if (k.rfind("h", 0) == 0) gRackDev.h = static_cast<float>(std::atof(k.c_str() + 1));
    i = j + 1;
  }
}

void rackDevBefore(App& a, int frame) {
  if (frame == 2 && gRackDev.steps > 0) {
    if (const Pattern* p = a.doc.song().pattern(a.curPattern)) {
      const int spb = std::max(1, p->stepsPerBar);
      a.doc.setPatternLength(p->id, std::max(1, gRackDev.steps / spb), spb);
    }
  }
  if (gRackDev.dock && frame >= 3 && frame <= 4)  // docked beside the browser (a narrow dock node)
    if (ImGuiWindow* bw = ImGui::FindWindowByName("Browser"); bw && bw->DockId) ImGui::SetNextWindowDockID(bw->DockId, ImGuiCond_Always);
  if (gRackDev.dock && frame == 5) a.focusWindow = "Channel rack";  // its tab in front of the browser's
  if (gRackDev.scroll >= 0 && frame == 6) a.rack.stepScroll = gRackDev.scroll;
}

void rackDevAfter(App& a, int frame) {
  (void)a;
  if (frame == 3 && !gRackDev.dock && (gRackDev.w > 0 || gRackDev.h > 0))
    if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack"))
      ImGui::SetWindowSize(rw, ImVec2(gRackDev.w > 0 ? gRackDev.w : rw->Size.x, gRackDev.h > 0 ? gRackDev.h : rw->Size.y), ImGuiCond_Always);
}

void rackDevDump(App& a, int frame) {
  const Song& s = a.doc.song();
  const RackShown& r = gShown;
  const racklayout::Layout& L = r.layout;
  std::printf("rack frame %d: steps %d avail %.1f nameW %.1f stepW %.1f viewW %.1f scroll %.1f scrollMax %.1f rowsScroll %.1f rowsScrollMax %.1f "
              "wheelScrolls %d rows %d knobRows %d truncated %d undo %zu\n"
              "  geom originX %.1f row0Y %.1f viewX0 %.1f graphY %.1f rowPitch %.1f\n",
              frame, L.steps, static_cast<double>(a.rack.lastAvail), static_cast<double>(L.nameW), static_cast<double>(L.stepW),
              static_cast<double>(L.viewW), static_cast<double>(r.scroll), static_cast<double>(L.scrollMax), static_cast<double>(r.rowsScrollY),
              static_cast<double>(r.rowsScrollMaxY), r.wheelScrolls ? 1 : 0, r.rows, r.knobRows, r.truncated, a.doc.history().position(),
              static_cast<double>(r.originX), static_cast<double>(r.row0Y), static_cast<double>(r.viewX0), static_cast<double>(r.graphY),
              static_cast<double>(r.rowPitch));
  const Pattern* pat = s.pattern(a.curPattern);
  for (size_t i = 0; i < s.channels.size(); ++i) {
    const Channel& c = s.channels[i];
    int velSum = 0, pitchSum = 0;
    if (pat) {
      for (uint8_t v : rowOf(*pat, c.id)) velSum += v;
      if (auto it = pat->stepProps.find(c.id); it != pat->stepProps.end())
        for (const auto& p : it->second) pitchSum += p.pitch < 0 ? 60 : p.pitch;
    }
    std::printf("  channel %zu volume %.4f pan %.4f velsum %d pitchsum %d\n", i, static_cast<double>(c.gain), static_cast<double>(c.pan), velSum, pitchSum);
  }
  std::fflush(stdout);
}

}  // namespace gl
