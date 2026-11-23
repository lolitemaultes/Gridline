// Gridline — channel rack: channels of the selected pattern with step
// sequencer rows (or a note preview when the channel has piano-roll notes).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include "GrooveUi.h"
#include "MeterUi.h"
#include "../core/MeterEdit.h"
#include "PixelGrid.h"
#include "CollabUi.h"
#include "CursorAnchor.h"
#include "../core/I18n.h"
#include "../core/ExternalDrop.h"
#include "../core/RackLayout.h"
#include "../core/RackSelect.h"
#include "../core/WheelRoute.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "ChannelRoute.h"
#include "FreezeUi.h"
#include "MidiArm.h"
#include "App.h"
#include "HostedUi.h"
#include "ContextMenus.h"
#include "EditTools.h"
#include "../core/NoteTools.h"
#include "Theme.h"
#include "TrackerPages.h"
#include "TrackIcons.h"
#include "Widgets.h"
#include "SmoothScrollUi.h"
#include "Windows.h"
#include "BrowserUi.h"
#include "PatternAutoUi.h"
#include "PlayLights.h"
#include "DevSignal.h"
#include "LogicPlugins.h"
#include "LaneUi.h"
#include "../core/Lanes.h"
#include "../core/ClipSources.h"
#include "../engine/Sample.h"

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
  float lastRowBottom = -1;                                 // K-136: the last channel row's bottom (screen)
  int lastRowIndex = -1;                                    // K-173: the last channel row's index in the song
  // K-173: the separator between the channel rows and the automation rows (screen; sepY < 0: none)
  float sepY = -1, sepTop = -1, sepBottom = -1, sepX0 = 0, sepX1 = 0;
  ImVec4 sepCaption{0, 0, 0, 0};  // the caption's text box (x0 y0 x1 y1)
  float autoY0 = -1;              // the first automation row's top (-1: no rows)
  int autoRows = 0;
  // K-146: the rows pane as ImGui sees it (its own scrollbars, its own sideways scroll, content vs. room)
  bool barX = false, barY = false;
  float paneScrollX = 0, paneContentW = 0, paneInnerW = 0, itemsRight = 0, rowRight = 0;
  // K-349: the rows drawn whole inside the rows pane
  std::set<ObjectId> visible;
};
RackShown gShown;
// K-136: the rack's height follows its content (racklayout::fitHeight), measured each frame.
struct RackFit {
  float rowsContent = 0;  // the rows' content height last frame (0: not measured yet)
  float fitH = 0;         // the floating window's largest height (its content)
  float winH = 0, winBottom = 0;
  bool docked = false;
  bool detached = false;  // in an OS window of its own (popped out): its menus need the room, no limit
  ImVec2 addMin{0, 0}, addMax{0, 0};  // the add button under the rows (screen)
} gFit;
// K-127 dev check: each row's controls as drawn last frame (screen rectangles: x0 y0 x1 y1).
struct RowGeom {
  ImVec4 phones, pan, vol, mix, name, lamp, steps;
  ImVec4 chip, handle;  // K-255: the lane chip and length handle (0 wide: not shown)
  float step0 = 0, step1 = 0;  // K-255: screen x of the row's first step and of its lane step 1 (step pitch)
  const char* phonesState = "on";  // K-186: the headphones button as drawn (on, muted, solo)
  int lights = 0;  // K-127: lights drawn on the row (one)
  int frame = -1;
};
std::unordered_map<ObjectId, RowGeom> gRowGeom;
ImVec4 rectOf(ImVec2 p0, ImVec2 p1) { return ImVec4(p0.x, p0.y, p1.x, p1.y); }
// K-126 / K-186: the rack's channel solo (the soloed channel and the mute states from before it;
// a second right-click on it brings them back).
ops::ChannelSolo gSolo;
struct RackDev {
  int steps = 0;
  float w = 0, h = 0, scroll = -1;
  bool dock = false;
  int playStep = -1;  // J-137 "play<k>": the song-position outline on step k (stills of the play highlight)
  int group = -9;     // K-173 "group<n>": the display group filter (-1: all channels; -2: audio clips; -9: left as it is)
  bool graph = false; // K-173 "graph": the graph editor open under the rows
  ImVec2 addMin{0, 0}, addMax{0, 0};  // K-72: the toolbar's "+" (add an instrument) button, last drawn
  struct Rect {
    ImVec2 a, b;
    int frame = -1;
  };
  std::map<std::string, Rect> rects;  // the group combo and its menu rows, last drawn ("rack" dev dump)
} gRackDev;
void devRackRect(const std::string& name) {
  gRackDev.rects[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetFrameCount()};
}

// J-186: the play light of each channel row (UI state, per channel; rows not drawn keep theirs).
struct RackLight {
  playlight::ChannelLight light;
  int frame = -1;
  double time = 0;
  bool reported = false;  // dev signal "rack-lit:<index>" sent for this lighting
};
std::unordered_map<ObjectId, RackLight> gRackLights;

// Updates channel `c`'s light once per frame from the engine's signals; returns its brightness.
float rackPlayLight(App& a, const Channel& c, size_t index) {
  RackLight& e = gRackLights[c.id];
  const int frame = ImGui::GetFrameCount();
  if (e.frame != frame) {
    const double now = ImGui::GetTime();
    if (e.frame != frame - 1) e.light.reprime();  // not drawn last frame (scrolled away, zipped, closed)
    const float dt = e.frame < 0 ? 0.0f : static_cast<float>(std::clamp(now - e.time, 0.0, 0.25));
    if (auto rt = a.builder.channelRuntime(c.id))
      e.light.update(rt->keys.starts(), rt->keys.lastVelocity(), rt->keys.anyHeld(), rt->peak.load(std::memory_order_relaxed), c.muted, dt,
                     scrollui::animations());
    else
      e.light.update(0, 0, false, 0, true, dt);
    e.frame = frame;
    e.time = now;
    if (e.light.lit() && !e.reported) devsig::reach("rack-lit:" + std::to_string(index));  // dev checks (waitfor)
    e.reported = e.light.lit();
  }
  return e.light.alpha();
}

// The channel's select button, between the name button and the steps (K-232; K-127 / K-186 drew a
// sound meter here): lit green while the channel is selected, and a soft glow with its sound
// (J-186). Mute and solo live on the headphones button.
void drawRowLamp(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float level, bool selected, bool hot) {
  const auto& P = pal();
  ImU32 idle = P.dark ? theme::mix(P.field, P.ink, 0.35f) : theme::mix(P.surface700, P.ink, 0.18f);
  if (hot && !selected) idle = theme::mix(idle, P.led, 0.25f);
  if (level > 0.003f)  // the sound: a soft glow around the button (the fill is the selection)
    dl->AddRectFilled(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), theme::withAlpha(P.led, 0.22f * std::min(1.0f, level)), 3.0f);
  dl->AddRectFilled(p0, p1, selected ? theme::mix(P.led, P.paper, hot ? 0.2f : 0.0f) : idle, 2.0f);
  dl->AddRect(p0, p1, theme::withAlpha(P.ink, P.dark ? 0.5f : 0.25f), 2.0f);
}

std::vector<uint8_t> rowOf(const Pattern& p, ObjectId ch) {
  std::vector<uint8_t> row;
  if (auto it = p.steps.find(ch); it != p.steps.end()) row = it->second;
  row.resize(static_cast<size_t>(std::max(0, lanes::steps(p, ch))), 0);  // K-255: the row's own steps
  return row;
}

// K-255: where a row's steps sit. A row on the pattern's grid uses the layout's steps as before;
// a row on its own grid stretches its steps so a bar is as wide on every row (step k sits where
// pattern step k x spb / grid would).
struct LaneGeo {
  const racklayout::Layout* L = nullptr;
  int spb = 16, grid = 16, steps = 16, total = 16;
  bool own = false;
  float pos(int k) const {
    if (!own) return L->stepPos(k);
    const double u = static_cast<double>(k) * spb / grid;
    const int i = static_cast<int>(std::floor(u));
    const double f = u - i;
    if (i >= total) return L->stepPos(total - 1) + L->stepW + racklayout::kStepGap + static_cast<float>(u - total) * (L->stepW + racklayout::kStepGap);
    const float a0 = L->stepPos(i), a1 = i + 1 >= total ? a0 + L->stepW + racklayout::kStepGap : L->stepPos(i + 1);
    return a0 + static_cast<float>(f) * (a1 - a0);
  }
  float width(int k) const { return own ? std::max(2.0f, pos(k + 1) - pos(k) - racklayout::kStepGap) : L->stepW; }
  // The beat band (K-236) of step k: on its own grid, the pattern beat it starts in.
  bool alt(int k) const {
    if (!own) return (k / L->group) % 2 == 1;
    const int u = static_cast<int>(std::floor(static_cast<double>(k) * spb / grid + 1e-9));
    return (u / L->group) % 2 == 1;
  }
};
LaneGeo laneGeo(const racklayout::Layout& L, const Pattern& p, ObjectId ch) {
  LaneGeo g;
  g.L = &L;
  g.spb = std::max(1, p.stepsPerBar);
  g.grid = lanes::grid(p, ch);
  g.own = lanes::ownGrid(p, ch);
  g.steps = std::max(1, lanes::steps(p, ch));
  g.total = std::max(1, p.bars * p.stepsPerBar);
  return g;
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
  // Copies: a drop below adds channels or imports notes (whole-song edits) and `c` may not survive it.
  const ObjectId cid = c.id;
  const std::string cname = c.name;
  const xdrop::RowZone zone = xdrop::rowZone(ImGui::GetIO().MousePos.y, r.Min.y, r.GetHeight());
  browserui::DropPlace place;
  if (zone == xdrop::RowZone::Replace) place.channel = cid;
  else place.insertAt = zone == xdrop::RowZone::Before ? index : index + 1;
  browserui::DropFeedback fb;
  fb.highlight = false;
  browserui::acceptFileDrop(a, browse::DropTarget::Rack, place, &fb);
  browserui::acceptPluginDrop(a, browse::DropTarget::Rack, place, &fb);
  ImGui::EndDragDropTarget();
  if (!fb.hovering) return;
  // What the row shows: replace (the default), the notes going in, or new channels below (Shift / extra files).
  bool replace = zone == xdrop::RowZone::Replace, below = zone == xdrop::RowZone::After;
  auto named = [&cname](const char* f) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, cname.c_str());
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

// Display group filter of the rack (-1 = all channels, kAudioClipsFilter = the audio clip channels).
// Groups and membership are saved with the project (Song::channelGroups, Channel::group).
constexpr int kAudioClipsFilter = -2;
int groupFilter = -1;
char newGroupName[64] = "";

// K-349: the channel passes the rack's display filter.
bool filterShows(const Channel& c, const std::set<ObjectId>& audioClips) {
  if (groupFilter == kAudioClipsFilter) return audioClips.count(c.id) > 0;
  return groupFilter < 0 || c.group == groupFilter;
}

// K-349: a channel made since the last frame (an audio file dropped on the playlist, a render, a
// recording, stems, an instrument added, in any layout) is brought into view the next time the rack
// draws: a display filter that would hide it gives way to all channels, and the rows scroll to it.
// A project opened or replaced starts afresh.
struct NewRowReveal {
  uint64_t epoch = ~uint64_t{0};
  std::set<ObjectId> known;
  ObjectId pending = kNoId;  // the row to scroll to (drawn rows clear it)
  int frames = 0;            // frames the rack drew while it was pending
} gReveal;

// Every frame, rack shown or not: notes the channels that are new since the last frame.
void noteNewChannels(const App& a) {
  const Song& s = a.doc.song();
  std::set<ObjectId> now;
  for (const auto& c : s.channels) now.insert(c.id);
  if (a.doc.projectEpoch() != gReveal.epoch) {
    gReveal.epoch = a.doc.projectEpoch();
    gReveal.known = std::move(now);
    gReveal.pending = kNoId;
    return;
  }
  for (const auto& c : s.channels)
    if (!gReveal.known.count(c.id)) {  // the first new one in rack order
      gReveal.pending = c.id, gReveal.frames = 0;
      break;
    }
  gReveal.known = std::move(now);
}

// While the rack draws: the new channel to bring into view (kNoId: none).
ObjectId revealNewChannel(const Song& s, const std::set<ObjectId>& audioClips) {
  if (gReveal.pending == kNoId) return kNoId;
  const Channel* c = s.channel(gReveal.pending);
  if (!c || c->rackHidden || ++gReveal.frames > 30) {  // hidden on purpose, gone, or never reached
    gReveal.pending = kNoId;
    return kNoId;
  }
  if (!filterShows(*c, audioClips)) groupFilter = -1;
  return gReveal.pending;
}

void groupMenu(App& a) {
  const Song& s = a.doc.song();
  auto countIn = [&](int g) {
    int n = 0;
    for (const auto& c : s.channels) n += c.group == g;
    return n;
  };
  if (ImGui::Selectable(i18n::tr("All channels"), groupFilter == -1)) groupFilter = -1;
  {  // K-349: the channels playlist audio clips play from
    const std::string l = std::string(i18n::tr("Audio clips")) + "  (" + std::to_string(ops::audioClipChannels(s).size()) + ")##gaudio";
    if (ImGui::Selectable(l.c_str(), groupFilter == kAudioClipsFilter)) groupFilter = kAudioClipsFilter;
    devRackRect("group audio");
  }
  for (size_t g = 0; g < s.channelGroups.size(); ++g) {
    std::string l = s.channelGroups[g] + "  (" + std::to_string(countIn(static_cast<int>(g))) + ")##g" + std::to_string(g);
    if (ImGui::Selectable(l.c_str(), groupFilter == static_cast<int>(g))) groupFilter = static_cast<int>(g);
    devRackRect("group " + std::to_string(g));
  }
  ImGui::Separator();
  const bool anySel = !a.selChannels.empty();
  // (Every edit runs at the start of the next frame: the rack's rows below hold pointers into the
  // song while they draw, and a whole-song edit replaces its containers. App::later.)
  auto assign = [&](int g, const char* label) {
    std::set<ObjectId> sel = a.selChannels;
    std::string what = label;
    a.later([&a, g, sel, what] {
      a.doc.editSong(what, [&](Song& song) {
        for (auto& c : song.channels)
          if (sel.count(c.id)) c.group = g;
      });
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
      a.later([&a, g, sel, name] {
        a.doc.editSong("new channel group", [&](Song& song) {
          if (static_cast<int>(song.channelGroups.size()) != g) return;  // (changed since the click)
          song.channelGroups.push_back(name);
          for (auto& c : song.channels)
            if (sel.count(c.id)) c.group = g;
        });
      });
      groupFilter = g;
      newGroupName[0] = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Remove selected from their group"), nullptr, false, anySel)) assign(-1, "ungroup channels");
  devRackRect("menu ungroup");
  if (groupFilter >= 0 && ImGui::BeginMenu(i18n::tr("Rename this group"))) {
    const int g = groupFilter;
    menus::renameField(s.channelGroups[static_cast<size_t>(g)],
                       [&a, g](const std::string& n) { a.later([&a, g, n] { ops::renameChannelGroup(a.doc, g, n); }); });
    ImGui::EndMenu();
  }
  if (groupFilter >= 0 && ImGui::MenuItem(i18n::tr("Delete this group"), nullptr, false)) {
    int g = groupFilter;
    a.later([&a, g] {
      a.doc.editSong("delete channel group", [&](Song& song) {
        if (g < 0 || static_cast<size_t>(g) >= song.channelGroups.size()) return;
        song.channelGroups.erase(song.channelGroups.begin() + g);
        for (auto& c : song.channels) c.group = c.group == g ? -1 : (c.group > g ? c.group - 1 : c.group);
      });
    });
    groupFilter = -1;
  }
  devRackRect("menu deletegroup");
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
  noteNewChannels(a);  // K-349: every frame, the rack shown or not
  if (!a.showRack) return;
  // K-136: a floating rack is never taller than its content (docked, the dock node sets the size).
  // A rack popped out into an OS window of its own (or being placed outside the main window this
  // frame) keeps its size: its menus open inside it.
  const float prevFit = gFit.fitH;
  bool goingOut = false;
  if (const ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack"); rw && !rw->DockIsActive) {
    // (the viewports module marks a window it pops out with NoAutoMerge, app/Viewports.cpp)
    goingOut = (rw->Viewport && rw->Viewport != ImGui::GetMainViewport()) ||
               (rw->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge) != 0;
  }
  if (prevFit > 0 && !gFit.detached && !goingOut) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, prevFit));
  if (!ui::BeginDockWindow("Channel rack", &a.showRack, ImVec2(760, 360))) {
    ui::EndDockWindow();
    return;
  }
  if (trackerpages::active(a)) {  // K-106: the Tracker's Steps page (the same steps, its own look)
    trackerpages::steps(a);
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
    // Width budget (most important first): add, menu, pattern name, length, group, graph, steps/bar, swing,
    // swing preset (K-151).
    const float wMenu = fh + 4, wGroup = 92 + 8, wLen = 20 + 50 + 20 + 9, wSpb = 88 + 9, wSwing = 22 + 4 + 40 + 9, wGraph = fh + 9,
                wAdd = fh + 4, minName = 64, wChip = 150 + 9;
    float need = wMenu + minName + wAdd;
    auto fits = [&](float w) {
      if (need + w > W) return false;
      need += w;
      return true;
    };
    const bool showLen = pat && fits(wLen);
    // K-236: a pattern in another meter than 4/4 shows it after its name; a click on it picks another.
    const int mNum = pat ? s.patternMeterNum(*pat) : 4, mDen = pat ? s.patternMeterDen(*pat) : 4;
    const std::string ml = pat && (mNum != 4 || mDen != 4 || pat->ownMeter()) ? meter::text(mNum, mDen) : std::string();
    const float mw = ml.empty() ? 0.0f : std::ceil(a.fontMono->CalcTextSizeA(theme::kFontBody, FLT_MAX, 0.0f, ml.c_str()).x) + 10;
    const bool meterFits = mw > 0 && fits(mw + 4);
    const bool showGroup = fits(wGroup);
    bool showSwing = pat && fits(wSwing);
    const bool showGraphBtn = fits(wGraph);
    bool showSpb = pat && fits(wSpb);
    bool showChip = pat && showSwing && fits(wChip);
    const float nameW = std::clamp(W - need + minName, minName, 200.0f);

    {
      // The menu's items may replace the song's patterns (move lanes to the playlist, a new pattern
      // from the selection, deleting channels): look the pattern up again rather than read the old one.
      const uint64_t rev = a.doc.revision();
      const ObjectId patId = pat ? pat->id : kNoId;
      menuButton("rackmenu", "Channel rack menu: selected channels, sorting, colours", [&a]() { rackOptionsMenu(a); });
      if (a.doc.revision() != rev && (pat = s.pattern(patId)) == nullptr) showSwing = showSpb = showChip = false;
    }
    ImGui::SameLine(0, 4);
    if (showGroup) {
      if (groupFilter >= static_cast<int>(s.channelGroups.size())) groupFilter = -1;
      const bool groupOpen =
          ui::BeginCombo("##group", groupFilter == kAudioClipsFilter ? i18n::tr("Audio clips")
                                    : groupFilter < 0                ? "All"
                                                                     : s.channelGroups[static_cast<size_t>(groupFilter)].c_str(),
                         92);
      if (!groupOpen) devRackRect("groupcombo");
      if (groupOpen) {
        groupMenu(a);
        ImGui::EndCombo();
      }
      ui::Hint("Display group: show all channels or one group (groups are saved with the project)");
      ImGui::SameLine(0, 8);
    }
    // GitHub #58: channels hidden from the rack: how many, and a click lists them to show again.
    if (const size_t nHidden = ops::rackHiddenChannels(s).size(); nHidden > 0) {
      const std::string hl = std::to_string(nHidden) + " " + i18n::tr("hidden") + "##rkhiddenbtn";
      if (ui::TextButton(hl.c_str(), ImGui::IsPopupOpen("##rkhiddenpop"), 0.0f, "Channels hidden from the rack: click to show them again"))
        ImGui::OpenPopup("##rkhiddenpop");
      devRackRect("hiddenbtn");
      if (ImGui::BeginPopup("##rkhiddenpop")) {
        hiddenChannelItems(a);
        ImGui::EndPopup();
      }
      ImGui::SameLine(0, 8);
    }
    // Pattern chip: colour + name.
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      const char* pname = pat ? pat->name.c_str() : "No pattern";
      const float pnameW = a.fontBold->CalcTextSizeA(theme::kFontBody, FLT_MAX, 0.0f, pname).x;
      const float nameRoom = nameW;
      ImGui::Dummy(ImVec2(nameRoom, fh));
      if (pat) dl->AddRectFilled(ImVec2(p.x, p.y + 5), ImVec2(p.x + 4, p.y + fh - 5), theme::rgb(pat->color), 1);
      theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(p.x + 10, std::round(p.y + (fh - theme::kFontBody) * 0.5f)), nameRoom - 14,
                          P.text, pname);
      ui::Hint("Pattern being edited (pick another in the top bar or the playlist)");
      if (meterFits) {
        // Right after the name (or at the end of a long, shortened one).
        const float nw = std::min(nameRoom - 14, pnameW);
        const ImVec2 m0(p.x + 10 + nw + 4, p.y);
        ImGui::SetCursorScreenPos(m0);
        const bool click = ImGui::InvisibleButton("##rackmeter", ImVec2(mw, fh));
        const bool hov = ImGui::IsItemHovered();
        devRackRect("meter");
        if (hov) dl->AddRectFilled(ImVec2(m0.x, m0.y + 3), ImVec2(m0.x + mw, m0.y + fh - 3), theme::withAlpha(P.text, 0.10f), theme::kRadiusSm);
        dl->AddText(a.fontMono, theme::kFontBody, ImVec2(m0.x + 5, std::round(p.y + (fh - theme::kFontBody) * 0.5f)),
                    pat->ownMeter() ? P.hot : P.textDim, ml.c_str());
        ui::Hint(pat->ownMeter() ? "Meter of this pattern, its own (click to change)" : "Meter of this pattern: the song's (click to give it its own)");
        if (click) ImGui::OpenPopup("##rackmeterpop");
        if (ImGui::BeginPopup("##rackmeterpop")) {
          const ObjectId pid = pat->id;
          meterui::patternMeterMenu(a, pid, false);
          ImGui::EndPopup();
          pat = s.pattern(pid);  // the meter edit may have replaced the pattern
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + nameW + mw + 4, p.y));
        ImGui::Dummy(ImVec2(0, fh));
      }
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
    gRackDev.addMin = ImGui::GetItemRectMin(), gRackDev.addMax = ImGui::GetItemRectMax();
    if (ImGui::BeginPopup("##addch")) {
      addChannelMenu(a);
      ImGui::EndPopup();
    }
    if (showGraphBtn) {
      rx -= wGraph;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
      if (ui::IconButton("graph", ui::Icon::Wave, a.rack.showGraph, fh,
                         keys::withKeys(i18n::N_("Graph editor (%s): step velocity, pan, release, fine pitch, mod X / Y, shift and keyboard (note) of the selected channel"), {keys::cmdKey("rack.graphEditor", "Ctrl+K")}),
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
      bool listed = false;
      for (int c : spbChoices) listed = listed || c == pat->stepsPerBar;
      char spbNow[24];  // K-236: a meter's own step count (14 in 7/8) shows as it is
      std::snprintf(spbNow, sizeof spbNow, "%d / bar", pat->stepsPerBar);
      if (ui::BeginCombo("##spb", listed ? spbNames[sel] : spbNow, 88)) {
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
    if (pat && showChip) {  // K-151: the swing preset, right of the Swing knob
      rx -= wChip;
      ImGui::SetCursorScreenPos(ImVec2(rx, rowY));
      grooveui::chip(a, wChip - 9);
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
          grooveui::toggleSwingNotesLater(a);  // next frame: the rows below keep pointers into the song
        ImGui::EndPopup();
      }
    }
  }
  ui::EndToolbar();
  pat = s.pattern(a.curPattern);  // a header menu may have replaced the song (App::later covers most)

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
  // K-136: the rows take their content's height when it is shorter than the room, so the add
  // button sits under the last row; a taller list fills the room and scrolls.
  const float rowsTop = ImGui::GetCursorPosY();
  const float rowsRoom = ImGui::GetContentRegionAvail().y - (showBar ? kBarH : 0.0f);
  const float bottomChrome =
      ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - (ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y);
  const float rowsH = racklayout::rowsRegion(gFit.rowsContent, rowsRoom);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.dark ? theme::mix(P.surface900, P.surface800, 0.35f) : P.surface900));
  // The horizontal scrollbar only shows for a rack narrower than its left section plus a few steps.
  ImGui::BeginChild("##rows", ImVec2(0, rowsH > 0 ? rowsH : showBar ? -kBarH : 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Channel button proportions after the reference rack: headphones (mute / solo) · pan · volume
  // · mixer track box · name button · activity / select lamp · steps (K-127).
  // Checkpoint 28 (Tracks layout): taller rows and pads, as the step sequencer of 188, 052 and 187 draws
  // them (about 31 px a row, the pads nearly the row's height).
  const bool lgSteps = logicplug::active(a);
  const float rowH = lgSteps ? 30.0f : 26.0f, rowGap = 3.0f, cellH = lgSteps ? 26.0f : 20.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  a.rack.lastAvail = avail;
  // Checkpoint 28 (Tracks layout): the steps fill the pane's width, as 188 and 052 do (up to 64 px a step).
  // K-295: the name button keeps what the widest channel name needs (the text plus the button's
  // 14 px of padding, as the row draws it below), so a long pattern in a narrow rack scrolls its
  // last steps instead of cutting every name to three letters.
  float nameNeed = 0;
  for (const auto& ch : s.channels)
    nameNeed = std::max(nameNeed, a.fontRegular->CalcTextSizeA(theme::kFontCaption + 1, 1e9f, 0, ch.name.c_str()).x + 14);
  // K-236: the steps group by beats of the pattern's meter (3/4: three groups a bar; 7/8: pairs).
  const int stepGrp = pat ? racklayout::stepGroup(pat->stepsPerBar, s.patternMeterNum(*pat), s.patternMeterDen(*pat)) : 4;
  // K-255: the lane column (each row's grid chip) shows while the pattern has a lane of its own or
  // the rack menu asks for it; without it the rack is laid out as before.
  const bool laneCol = laneui::columnShown(a, pat);
  racklayout::Input lin{avail, total, lgSteps ? 64.0f : racklayout::kStepMax, nameNeed, stepGrp};
  lin.laneW = laneCol ? racklayout::kLaneW : 0.0f;
  const racklayout::Layout L = racklayout::compute(lin);
  if (laneCol) laneui::beginRows();
  const float xLeft = racklayout::kXLeft, xPan = racklayout::kXPan, xVol = racklayout::kXVol, xMix = racklayout::kXMix,
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
  if (playStep >= 0 && playStep != a.rack.lastPlayStep && L.scrolls() && !a.rack.thumbDrag && a.settings.getBool("ui.autoScrollPattern")) {  // K-4
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
                        lit ? P.led : theme::mix(P.field, (k / L.group) % 2 ? P.stepOffAlt : P.stepOff, 0.35f), 1.5f);
    }
    ImGui::PopClipRect();
    ImGui::Dummy(ImVec2(rowW, 8));
    ui::Hint("Playing step");
  }
  if (pat && laneCol && lanes::anyRunOn(*pat)) {  // K-255: where the lanes that run on meet again
    const std::string up = laneui::lineUpText(s, *pat);
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    dl->AddText(a.fontRegular, theme::kFontLabel + 1, ImVec2(std::round(lp.x + stepsX), std::round(lp.y)), theme::litRole(theme::role::kLoop), up.c_str());
    ImGui::Dummy(ImVec2(rowW, 15));
    ui::Hint("Lanes of other lengths that keep running meet the pattern's start together again here");
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
  // The channel menu is opened by a row but drawn after the rows (below): its items may replace
  // the song or add, remove and move channels, and every row holds references into the song.
  const ImGuiID chMenuId = ImGui::GetID("##chmenu");
  static ObjectId chMenuChannel = kNoId;
  const std::set<ObjectId> audioClips = ops::audioClipChannels(s);  // K-349
  const ObjectId reveal = revealNewChannel(s, audioClips);
  const float paneY0 = ImGui::GetWindowPos().y, paneY1 = paneY0 + ImGui::GetWindowHeight();
  for (size_t ci = 0; ci < s.channels.size(); ++ci) {
    const Channel& c = s.channels[ci];
    if (!filterShows(c, audioClips)) continue;  // display group filter
    if (c.rackHidden) continue;  // GitHub #58: hidden from the rack (the rack menu shows it again)
    ImGui::PushID(static_cast<int>(c.id));
    ImVec2 rp = ImGui::GetCursorScreenPos();
    if (c.id == reveal) {  // K-349: a new channel: the rows scroll to it when it is out of sight
      if (rp.y < paneY0 || rp.y + rowH > paneY1) ImGui::SetScrollFromPosY(rp.y + rowH * 0.5f - paneY0, 0.5f);
      gReveal.pending = kNoId;
    }
    if (rp.y >= paneY0 - 0.5f && rp.y + rowH <= paneY1 + 0.5f) shown.visible.insert(c.id);
    if (a.rack.zipped.count(c.id)) {  // J-11 zipped row: a thin strip; click unzips
      const float zh = 7.0f;
      ImGui::InvisibleButton("zip", ImVec2(L.rowW, zh));
      const bool zhov = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked()) a.rack.zipped.erase(c.id);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        chMenuChannel = c.id;
        ImGui::OpenPopup(chMenuId);
      }
      if (ui::HintHovered()) ui::Hint(c.name + " (zipped: click to unzip)");
      dl->AddRectFilled(ImVec2(rp.x + nameX, rp.y + 1), ImVec2(rp.x + L.rowW, rp.y + zh - 1),
                        theme::withAlpha(theme::rgb(c.color), zhov ? 0.75f : 0.45f), 2);
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + zh + 2));
      ImGui::Dummy(ImVec2(rowW, 0));
      shown.lastRowBottom = rp.y + zh;
      shown.lastRowIndex = static_cast<int>(ci);
      ImGui::PopID();
      continue;
    }
    // Rows outside the visible area only take their space (large racks: J-25),
    // unless the row's routing menu is open or a step paint runs on it.
    if (!ImGui::IsRectVisible(rp, ImVec2(rp.x + rowW, rp.y + rowH)) && paintRow != c.id &&
        !ImGui::IsPopupOpen("##route")) {
      ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
      ImGui::Dummy(ImVec2(rowW, 0));
      shown.lastRowBottom = rp.y + rowH;
      shown.lastRowIndex = static_cast<int>(ci);
      ImGui::PopID();
      continue;
    }
    ++shown.rows;
    const float cy = rp.y + rowH * 0.5f;
    if (shown.row0Y < 0) shown.row0Y = cy;
    shown.rowPitch = rowH + rowGap + ImGui::GetStyle().ItemSpacing.y;
    bool selected = a.selChannels.count(c.id) > 0;
    if (selected) dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), theme::withAlpha(P.paper, 0.035f), theme::kRadius);

    RowGeom& geom = gRowGeom[c.id];
    geom.lights = 0;
    geom.frame = ImGui::GetFrameCount();
    const bool soloed = ops::channelSoloed(s, c.id, gSolo);
    // K-186: the headphones button at the far left. Click: mute / unmute; right-click or
    // Ctrl/Cmd+click: solo this channel, again: back to how it was; Shift+click: select.
    {
      const ImVec2 b0(rp.x + racklayout::kXLeft, std::round(cy - racklayout::kPhonesW * 0.5f));
      const ImVec2 b1(b0.x + racklayout::kPhonesW, b0.y + racklayout::kPhonesW);
      ImGui::SetCursorScreenPos(b0);
      const bool muted = c.muted;
      const ObjectId cid = c.id;
      if (ImGui::InvisibleButton("phones", ImVec2(racklayout::kPhonesW, racklayout::kPhonesW))) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) a.selectChannel(cid, true);
        else if (io.KeyCtrl || io.KeySuper) ops::toggleChannelSolo(a.doc, cid, gSolo);
        else a.doc.setParam(channelParam(cid, ChannelParam::Mute), muted ? 0 : 1, false);
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ops::toggleChannelSolo(a.doc, cid, gSolo);
      const bool hov = ImGui::IsItemHovered();
      const bool pressed = hov && (ImGui::IsItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Right));
      geom.phones = rectOf(b0, b1);
      geom.phonesState = soloed ? "solo" : muted ? "muted" : "on";
      ui::Hint(soloed  ? i18n::N_("Soloed: every other channel is muted \xC2\xB7 right-click: back to normal \xC2\xB7 click: mute")
               : muted ? i18n::N_("Muted \xC2\xB7 click: unmute \xC2\xB7 right-click: solo this channel, again: back to normal \xC2\xB7 Shift+click: select")
                       : i18n::N_("Click: mute this channel \xC2\xB7 right-click: solo it, again: back to normal \xC2\xB7 Shift+click: select"));
      ui::PhonesFace(dl, b0, b1, rackPlayLight(a, c, ci), muted, soloed, hov, pressed);
    }
    // Pan & volume knobs: always shown (J-84).
    {
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xPan, cy - 9));
      ui::ParamKnob("pan", channelParam(c.id, ChannelParam::Pan), 9.0f, nullptr, true);
      geom.pan = rectOf(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImGui::SetCursorScreenPos(ImVec2(rp.x + xVol, cy - 9));
      ui::ParamKnob("vol", channelParam(c.id, ChannelParam::Volume), 9.0f);
      geom.vol = rectOf(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ++shown.knobRows;
    }
    // Mixer track box: drag to change, right-click to pick.
    {
      const float boxW = racklayout::kMixW;
      ImVec2 bp(rp.x + xMix, cy - cellH * 0.5f);
      ImGui::SetCursorScreenPos(bp);
      ImGui::InvisibleButton("mt", ImVec2(boxW, cellH));
      geom.mix = rectOf(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
        if (chanroute::menuItem(keys::label("rack.routeFree")))  // J-195; next frame: a whole-song edit, and the row holds `c`
          a.later([&a, cid = c.id] { chanroute::routeToFree(a, cid); });
        ImGui::Separator();
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
    // K-123 (4.5): the record dot at the name button's right end, armed (red) or on the row's hover. Its
    // button goes first, so the name button under it does not take its clicks.
    float armDotX = 0;  // 0: not shown
    bool armHot = false;
    if (midiarm::applies(a)) {
      const bool on = midiarm::armed(a, c.id);
      const bool rowHov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(rp, ImVec2(rp.x + rowW, rp.y + rowH));
      if (on || rowHov) {
        const float mk = freezeui::hasMarker(a, freeze::channelUnit(c.id)) ? std::min(14.0f, cellH - 6) + 3 : 0.0f;
        armDotX = std::round(rp.x + nameX + nameW - 10 - mk);
        ImGui::SetCursorScreenPos(ImVec2(armDotX - 7, cy - 7));
        if (ImGui::InvisibleButton("arm", ImVec2(14, 14))) midiarm::toggle(a, c.id, ImGui::GetIO().KeyAlt);
        armHot = ImGui::IsItemHovered();
        if (armHot) ui::Hint(midiarm::dotHint(on));
      }
    }
    // Name button: raised face, channel colour on the left edge.
    {
      ImVec2 np(rp.x + nameX, cy - cellH * 0.5f);
      ImGui::SetCursorScreenPos(np);
      ImGui::InvisibleButton("name", ImVec2(nameW, cellH));
      geom.name = rectOf(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      bool hov = ImGui::IsItemHovered();
      // Click (without dragging): select the channel and toggle its plugin window, like the
      // reference channel button. No sound; Ctrl+click only adds to the selection.
      // K-233: a double-click opens the channel's piano roll instead. The window of a single click
      // opens once the double-click time has passed without a second click (it would otherwise
      // open over the name and catch the second click).
      static ObjectId dblHeld = kNoId;  // the press of a double-click: its release toggles nothing
      static struct {
        ObjectId id = kNoId;
        double at = 0;
      } pendingToggle;
      const double now = ImGui::GetTime();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) a.selectChannel(c.id, ImGui::GetIO().KeyCtrl);
      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl) {
        dblHeld = c.id;
        pendingToggle = {};
        a.openPianoRoll(c.id);
      }
      if (ImGui::IsItemDeactivated()) {
        const ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        if (dblHeld == c.id) dblHeld = kNoId;
        else if (!ImGui::GetIO().KeyCtrl && dd.x * dd.x + dd.y * dd.y < 16.0f) pendingToggle = {c.id, now + ImGui::GetIO().MouseDoubleClickTime};
      }
      if (pendingToggle.id == c.id && now >= pendingToggle.at) {
        if (now - pendingToggle.at < 1.0) togglePluginWindow(a, PluginTarget{c.id, -1});  // a stale one (the rack hidden meanwhile) is dropped
        pendingToggle = {};
      }

      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        a.selectChannel(c.id);
        chMenuChannel = c.id;
        ImGui::OpenPopup(chMenuId);
      }
      // J-84: a name cut short by a narrow rack shows in full in a tooltip.
      const float nameFont = theme::kFontCaption + 1;
      // J-211: the channel's icon before its name, when the button has room for both.
      const float iconBox = c.icon > 0 && nameW >= 96 ? trackicons::boxFor(cellH - 4, 14.0f) : 0.0f;
      const float iconW = iconBox > 0 ? iconBox + 4 : 0.0f;
      const float nameTextW = nameW - 14 - iconW - (armDotX > 0 ? 14.0f : 0.0f);  // K-123: the record dot
      const bool truncated = a.fontRegular->CalcTextSizeA(nameFont, 1e9f, 0, c.name.c_str()).x > nameTextW;
      if (truncated) {
        ++shown.truncated;
        if (!ImGui::IsPopupOpen(chMenuId, 0)) ui::Tooltip(c.name.c_str());
      }
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {  // reorder
        ImGui::SetDragDropPayload("GL_CHANNEL", &c.id, sizeof c.id);
        ui::DragPreview(ui::Icon::Rack, ("Move " + c.name).c_str(), theme::content(c.color));
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CHANNEL")) {
          const ObjectId moving = *static_cast<const ObjectId*>(pl->Data);
          const int to = static_cast<int>(ci);
          a.later([&a, moving, to] { a.doc.moveChannel(moving, to); });  // next frame: the row still reads `c`
        }
        ImGui::EndDragDropTarget();  // browser files / plugins: the whole row (rowDropTarget, J-97)
      }
      const bool open = pluginWindowShown(a, PluginTarget{c.id, -1});
      ImU32 bg = selected ? P.surface600 : hov ? P.surface700 : P.surface800;
      if (open) bg = theme::mix(bg, P.hot, 0.10f);
      if (lgSteps) {  // checkpoint 28 (Tracks layout): the name on the row itself, as 188 and 052 set it (no raised button)
        if (selected || hov || open) dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH), (selected ? bg : theme::withAlpha(bg, 0.7f)), theme::kRadiusSm);
      } else {
        dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH), theme::mix(bg, P.ink, 0.3f), theme::kRadiusSm);
        dl->AddRectFilled(np, ImVec2(np.x + nameW, np.y + cellH - 1), bg, theme::kRadiusSm);
        dl->AddLine(ImVec2(np.x + 5, np.y + 0.5f), ImVec2(np.x + nameW - 2, np.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.12f : 0.5f));
      }
      dl->AddRectFilled(np, ImVec2(np.x + 4, np.y + cellH), theme::content(c.color), theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      if (selected && !lgSteps)  // K-127: the selection shows on the name button (checkpoint 29: the Tracks layout fills it, no outline) (the light shows the channel's state)
        dl->AddRect(ImVec2(np.x + 0.5f, np.y + 0.5f), ImVec2(np.x + nameW - 0.5f, np.y + cellH - 0.5f), theme::withAlpha(P.paper, P.dark ? 0.55f : 0.7f),
                    theme::kRadiusSm, 0, 1.0f);
      if (iconBox > 0)
        trackicons::draw(dl, c.icon, ImVec2(std::round(np.x + 9 + iconBox * 0.5f), std::round(np.y + cellH * 0.5f)), iconBox,
                         c.muted ? P.textFaint : P.textDim);
      const freeze::Unit fu = freeze::channelUnit(c.id);  // K-87: frozen / hibernating marker at the right end
      const float mkBox = freezeui::hasMarker(a, fu) ? std::min(14.0f, cellH - 6) : 0.0f;
      theme::textEllipsis(dl, a.fontRegular, nameFont, ImVec2(np.x + 10 + iconW, std::round(np.y + (cellH - 13) * 0.5f) - 1),
                          nameTextW - (mkBox > 0 ? mkBox + 3 : 0.0f), c.muted ? P.textFaint : selected && P.dark ? P.paper : P.text, c.name.c_str());
      if (mkBox > 0)
        freezeui::drawMarker(a, dl, fu, ImVec2(std::round(np.x + nameW - 4 - mkBox * 0.5f), std::round(np.y + cellH * 0.5f)), mkBox);
      if (armDotX > 0) midiarm::drawDot(dl, ImVec2(armDotX, std::round(cy)), 4.0f, midiarm::armed(a, c.id), armHot);  // K-123
      freezeui::drawShade(a, dl, fu, np, ImVec2(np.x + nameW, np.y + cellH));
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
        const std::string fz = freezeui::statusText(a, freeze::channelUnit(c.id));  // K-87
        ui::Hint(c.name + (info ? "  \xC2\xB7  " + info->name : "") + (fz.empty() ? std::string() : "  \xC2\xB7  " + fz) +
               "   (click: open / close its settings \xC2\xB7 Ctrl+click: add to selection \xC2\xB7 right-click: menu \xC2\xB7 drag: reorder)");
      }
    }

    // Steps or note preview, inside the step viewport (scrolled by scrollX).
    ImVec2 sp(rp.x + stepsX - scrollX, cy - cellH * 0.5f);
    bool hasNotes = pat && pat->notes.count(c.id) && !pat->notes.at(c.id).empty();
    laneui::Handle handleShown;  // K-255: the length handle, drawn after the steps' clip
    LaneMode handleMode = LaneMode::Restart;
    bool drawHandle = false;
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
      for (Tick b = s.patternBarTicks(*pat); b < len; b += std::max<Tick>(1, s.patternBarTicks(*pat))) {  // K-179: its own bars
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
      const float dpp = pixelgrid::frameScale();  // K-276
      // K-255: the row's own steps (its grid), its length, and what plays at its playing step.
      const LaneGeo lg = laneGeo(L, *pat, c.id);
      const int laneLen = lanes::length(*pat, c.id);
      const bool laneShade = laneCol && laneLen > 0;  // steps past the length show what plays there (the loop again)
      int lanePlay = playStep;
      if (lg.own && playStep >= 0) {
        const double st = lanes::stepTicks(s, *pat, c.id);
        lanePlay = st > 0 ? static_cast<int>(std::floor(a.engine.position() / st + 1e-9)) : -1;
        if (gRackDev.playStep >= 0) lanePlay = static_cast<int>(static_cast<int64_t>(gRackDev.playStep) * lg.grid / lg.spb);
      }
      laneui::Handle lh;
      if (laneCol) {  // the length handle takes the pointer before the steps under it
        lh.at = laneLen > 0 ? laneLen : lg.steps;
        lh.looping = laneLen > 0;
        lh.x = std::min(sp.x + lg.pos(lh.at) - racklayout::kStepGap * 0.5f, viewX1 - 2.0f);
        lh.y0 = sp.y - 1, lh.y1 = sp.y + cellH + 1;
        if (lh.x > viewX0) {
          const float sx = sp.x;
          bool hclick = false;
          laneui::handleInput(a, *pat, c, lh, lg.steps, [&lg, sx](int k) { return sx + lg.pos(k) - racklayout::kStepGap * 0.5f; }, &hclick);
          if (hclick) laneui::openLength(c.id);
        }
        geom.handle = rectOf(ImVec2(lh.x - 4, lh.y0), ImVec2(lh.x + 4, lh.y1));
      } else {
        geom.handle = ImVec4(0, 0, 0, 0);
      }
      geom.step0 = sp.x + lg.pos(0), geom.step1 = sp.x + lg.pos(1);
      const int rowSteps = lg.steps;
      for (int k = 0; k < rowSteps; ++k) {
        const float kw = lg.width(k);
        ImVec2 q(sp.x + lg.pos(k), sp.y);
        if (q.x + kw <= viewX0 || q.x >= viewX1) continue;  // scrolled out of the step viewport (long patterns)
        const bool looped = laneShade && k >= laneLen;  // K-255: a step the loop plays again (edits go to the step it repeats)
        const int ek = looped ? k % laneLen : k;
        // K-146: a step cut by an edge of the viewport is clickable only where it shows. Its button
        // must not reach past the row's right edge: that widened the rows pane, which then showed a
        // second sideways scrollbar of its own (and took sideways swipes, sliding the whole rack).
        const float bx1 = std::min(q.x + kw, viewX1), bx0 = std::min(std::max(q.x, viewX0), bx1 - 1.0f);  // (at least a pixel)
        ImGui::SetCursorScreenPos(ImVec2(bx0, q.y));
        ImGui::PushID(k);
        ImGui::InvisibleButton("s", ImVec2(bx1 - bx0, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hov) a.rack.hoverChannel = c.id, a.rack.hoverStep = ek;
        uint8_t v = row[static_cast<size_t>(ek)];
        // Q-15: in a scrolling rack the plain wheel scrolls; the step takes it after a click or with Ctrl / Cmd.
        const ui::WheelGate wg = ui::wheelGate(v > 0);
        if (ImGui::IsItemActivated() && wg.dragOk) {
          paintRow = c.id;
          paintValue = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || v > 0 ? 0 : 100;
        }
        if (paintValue >= 0 && hov && paintRow == c.id && v != paintValue) {
          auto nr = row;
          nr[static_cast<size_t>(ek)] = static_cast<uint8_t>(paintValue);
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
          nr[static_cast<size_t>(ek)] = static_cast<uint8_t>(std::clamp(v + (dv != 0 ? dv : (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)), 1, 127));
          a.doc.editPattern(pat->id, "change step velocity", [&](Pattern& p) { p.steps[c.id] = nr; }, kStepGestureKey ^ (pat->id + 7));
          wheelUsed = true;
        }
        const bool alt = lg.alt(k);  // K-236: one colour band a beat
        // K-276: the pad on whole device pixels, its gap to the next pad whole too (PixelGrid.h), so
        // the gaps stay even at every interface scale (a pad may be a device pixel wider instead).
        const float nextX = sp.x + lg.pos(k + 1);
        const pixelgrid::Span padX = pixelgrid::block(q.x, nextX, dpp, 0.0f, nextX - q.x - kw), padY = pixelgrid::block(q.y, q.y + cellH, dpp, 0.0f, 0.0f);
        q = ImVec2(padX.x0, padY.x0);
        ImVec2 q1(padX.x1, padY.x1);
        if (lgSteps) {
          // Checkpoint 28 (Tracks layout): flat pads in the row's own colour, as 188 and 052 draw them: a
          // dim tint when off, the colour itself when on (brighter with the velocity); no bevel.
          const ImU32 rc = theme::content(c.color);
          const ImU32 off = theme::mix(P.surface900, rc, P.dark ? 0.20f : 0.16f);
          ImU32 face = v > 0 ? theme::mix(off, rc, 0.55f + 0.45f * std::clamp(v / 127.0f, 0.0f, 1.0f)) : off;
          if (hov) face = theme::mix(face, P.text, 0.12f);
          dl->AddRectFilled(q, q1, face, std::min(3.0f, (q1.x - q.x) * 0.2f));
        } else {
          ui::StepPad(dl, q, q1, v > 0, alt, hov, theme::rgb(c.color), v / 127.0f);
        }
        // J-31 step loop: steps past the channel's loop length repeat the loop (shaded; the first one marked).
        // K-255: with the lane column they show the steps the loop plays there (the handle marks the length).
        if (looped) {
          dl->AddRectFilled(q, q1, theme::withAlpha(P.surface950, 0.5f), 2.0f);
        } else if (auto lp = pat->stepLoop.find(c.id); !laneShade && lp != pat->stepLoop.end() && lp->second > 0 && k >= lp->second) {
          dl->AddRectFilled(q, q1, theme::withAlpha(P.surface950, 0.55f), 2.0f);
          if (k == lp->second) dl->AddLine(ImVec2(q.x - 1, q.y - 2), ImVec2(q.x - 1, q1.y + 2), theme::lit(theme::rgb(theme::role::kLoop)), 2.0f);
        }
        if (v > 0 && pat->stepProps.count(c.id)) {  // J-11: steps with their own note / parameters get a corner mark
          const auto& pr = pat->stepProps.at(c.id);
          if (static_cast<size_t>(ek) < pr.size() && !pr[static_cast<size_t>(ek)].isDefault())
            dl->AddTriangleFilled(ImVec2(q1.x - 6, q.y + 1), ImVec2(q1.x - 1, q.y + 1), ImVec2(q1.x - 1, q.y + 6),
                                  theme::withAlpha(theme::rgb(theme::role::kGraph), 0.9f));
        }
        if (k == lanePlay) ui::StepPlayOutline(dl, q, q1);  // J-137: inside the pad (the step viewport's clip cut the first column's left edge)
        if (hov)
          app().setHint("Step " + std::to_string(ek + 1) + (looped ? std::string(" ") + i18n::tr("(played again by the loop)") : std::string()) +
                        (v ? " \xC2\xB7 velocity " + std::to_string(v) + " (" + (wg.hint ? i18n::tr("click or Ctrl+wheel to change") : i18n::tr("scroll to change")) + ")"
                           : std::string()) +
                        "   (click/drag: paint \xC2\xB7 right-drag: erase \xC2\xB7 Insert / Delete: insert or remove a step here)");
        ImGui::PopID();
      }
      if (laneCol && lh.x > viewX0) handleShown = lh, handleMode = lanes::mode(*pat, c.id), drawHandle = true;
    }
    ImGui::PopClipRect();
    if (drawHandle) laneui::handleDraw(dl, handleShown, handleMode);  // K-255: over the row's top edge (its length tab)
    // Scrolled steps end under a soft edge (more steps to either side).
    if (L.scrolls()) {
      const ImU32 edge = theme::withAlpha(P.surface950, 0.0f), edgeIn = theme::withAlpha(P.surface950, 0.55f);
      if (scrollX > 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(viewX0, sp.y), ImVec2(viewX0 + 6, sp.y + cellH), edgeIn, edge, edge, edgeIn);
      if (scrollX < L.scrollMax - 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(viewX1 - 6, sp.y), ImVec2(viewX1, sp.y + cellH), edge, edgeIn, edgeIn, edge);
    }
    geom.steps = rectOf(ImVec2(viewX0, sp.y), ImVec2(viewX1, sp.y + cellH));
    // K-232: the channel's select button (between the name button and the steps, as in the
    // reference rack): click adds the channel to the selection or takes it out, right-click
    // selects it alone, Shift+click selects a run. It never opens a window: the typing keyboard
    // plays every selected instrument. Lit green while selected; glows with the channel's sound.
    {
      const float lx = std::round(rp.x + L.lampX + (racklayout::kSelLampW - 6) * 0.5f);
      ImVec2 l0(lx, cy - cellH * 0.5f + 2), l1(l0.x + 6, cy + cellH * 0.5f - 2);
      ImGui::SetCursorScreenPos(ImVec2(rp.x + L.lampX, cy - cellH * 0.5f));
      ImGui::InvisibleButton("sel", ImVec2(racklayout::kSelLampW, cellH));
      const bool lhov = ImGui::IsItemHovered();
      const bool lclick = ImGui::IsItemClicked(ImGuiMouseButton_Left), rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      if (lclick || rclick) {
        std::vector<ObjectId> order;
        order.reserve(s.channels.size());
        for (const auto& oc : s.channels)
          if (!oc.rackHidden) order.push_back(oc.id);  // a run skips hidden channels
        const rackselect::Click how = rclick ? rackselect::Click::Only : ImGui::GetIO().KeyShift ? rackselect::Click::Range : rackselect::Click::Toggle;
        rackselect::click(order, a.selChannels, a.selChannel, c.id, how);
        selected = a.selChannels.count(c.id) > 0;
      }
      geom.lamp = rectOf(l0, l1);
      ++geom.lights;
      ui::Hint(i18n::N_("Select: click adds or removes this channel \xC2\xB7 right-click: this one only \xC2\xB7 Shift+click: a run \xC2\xB7 the keyboard plays every selected channel"));
      drawRowLamp(dl, l0, l1, rackPlayLight(a, c, ci), selected, lhov);
    }
    if (laneCol && pat) {  // K-255: the row's grid chip (click: grid, right-click: length)
      const ImVec2 c0(std::round(rp.x + L.laneX), std::round(cy - cellH * 0.5f + 2));
      laneui::chip(a, *pat, c, c0, ImVec2(L.laneW, cellH - 4));
      geom.chip = rectOf(c0, ImVec2(c0.x + L.laneW, c0.y + cellH - 4));
    } else {
      geom.chip = ImVec4(0, 0, 0, 0);
    }

    const ObjectId rowId = c.id;
    const uint64_t dropRev = a.doc.revision();
    rowDropTarget(a, c, static_cast<int>(ci), rp, rowW, rowH, rowGap, stepsX + 6);  // J-97: drop to replace / insert
    // A drop that edited the song (new channels, imported notes) may have replaced the channels and
    // patterns this loop holds: the rows after this one are drawn next frame.
    const bool dropped = a.doc.revision() != dropRev;
    // J-111d: a collaborator's pointer on this row (logical px from its start: the row's layout is fixed from the left)
    canchor::publish(canchor::RackRow, rowId, 0, rp, ImVec2(1.0f, rowH), rp, ImVec2(rp.x + rowW, rp.y + rowH));
    ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + rowGap));
    ImGui::Dummy(ImVec2(rowW, 0));
    shown.lastRowBottom = rp.y + rowH;
    shown.lastRowIndex = static_cast<int>(ci);
    ImGui::PopID();
    if (dropped) break;
  }
  if (ImGui::BeginPopupEx(chMenuId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    channelContextMenu(a, chMenuChannel);
    ImGui::EndPopup();
  }
  if (laneCol) laneui::popovers(a);  // K-255
  pat = s.pattern(a.curPattern);  // the menu may have replaced the song (its patterns too)

  // J-173: the pattern's automation clips as rows below the channels, and a
  // drop row while an automation clip is dragged from the clip sources.
  if (pat) {
    const ObjectId patId = pat->id;
    // K-173: one quiet hairline with a small caption between the channel rows and the automation
    // rows (or the automation drop row), only while both are shown. It adds a small fixed gap only.
    if (shown.lastRowBottom >= 0 && (!pat->automation.empty() || patautoui::rackDropRowShown(a, patId))) {
      constexpr float kSepH = 12.0f;  // the gap the separator adds to the rows' rhythm
      const ImVec2 s0 = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(s0.x, s0.y + kSepH - ImGui::GetStyle().ItemSpacing.y));
      ImGui::Dummy(ImVec2(rowW, 0));  // the next row starts exactly kSepH lower
      const float top = shown.lastRowBottom, bottom = ImGui::GetCursorScreenPos().y;
      const float y = std::floor((top + bottom) * 0.5f) + 0.5f;  // a crisp one-pixel line
      const char* cap = i18n::tr("AUTOMATION");
      const float fs = theme::kFontLabel - 1;
      const ImVec2 ts = a.fontMono->CalcTextSizeA(fs, FLT_MAX, 0, cap);
      const float cx = std::round(s0.x + xLeft + 2), cyTop = std::round(y - ts.y * 0.5f);
      const float lx0 = std::round(cx + ts.x + 8), lx1 = s0.x + rowW;
      dl->AddText(a.fontMono, fs, ImVec2(cx, cyTop), P.textFaint, cap);
      if (lx1 > lx0) dl->AddLine(ImVec2(lx0, y), ImVec2(lx1, y), P.line, 1.0f);
      shown.sepY = y, shown.sepTop = top, shown.sepBottom = bottom, shown.sepX0 = lx0, shown.sepX1 = lx1;
      shown.sepCaption = ImVec4(cx, cyTop, cx + ts.x, cyTop + ts.y);
      // A channel dragged onto the separator goes last (the end of the channel rows above it).
      if (shown.lastRowIndex >= 0) {
        const ImRect band(ImVec2(s0.x, top), ImVec2(s0.x + rowW, bottom));
        if (ImGui::BeginDragDropTargetCustom(band, ImGui::GetID("##autosep"))) {
          if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CHANNEL"))
            a.doc.moveChannel(*static_cast<const ObjectId*>(pl->Data), shown.lastRowIndex);
          ImGui::EndDragDropTarget();
        }
      }
    }
    shown.autoRows = static_cast<int>(pat->automation.size());
    if (shown.autoRows > 0) shown.autoY0 = ImGui::GetCursorScreenPos().y;
    patautoui::RackRows rr{rowW, rowH, rowGap, cellH, xLeft, nameX, nameW, stepsX, stepsW, scrollX, viewX0, viewX1};
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
    const LaneGeo gg = laneGeo(L, *pat, gc->id);  // K-255: the row's own steps
    if (m.x >= viewX0 && m.x < viewX1)
      for (int i = 0; i < static_cast<int>(row.size()); ++i)
        if (m.x >= w0.x + gg.pos(i) - stepGap && m.x < w0.x + gg.pos(i) + gg.width(i) + (gg.own ? stepGap : groupGap)) hk = i;
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
    for (int k = 0; k < static_cast<int>(row.size()); ++k) {
      uint8_t v = row[static_cast<size_t>(k)];
      if (!v) continue;
      const float x0 = w0.x + gg.pos(k) + 1, x1 = x0 + gg.width(k) - 2;
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
      wheelDx = -h * static_cast<float>(L.group) * (stepW + stepGap);  // a notch: one beat (K-236: of the pattern's meter)
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
  {  // K-146 dev check: the pane's own scroll state and how far its items reach (vs. the row's right edge)
    const ImGuiWindow* rw = ImGui::GetCurrentWindow();
    shown.barX = rw->ScrollbarX, shown.barY = rw->ScrollbarY;
    shown.paneScrollX = rw->Scroll.x, shown.paneContentW = rw->ContentSize.x, shown.paneInnerW = rw->InnerRect.GetWidth();
    shown.itemsRight = rw->DC.CursorMaxPos.x, shown.rowRight = originX + rowW;
  }
  // K-146: the rows pane is exactly one row wide. Nothing drawn in it may widen it past the row's
  // right edge (the step viewport's end): its own sideways scrollbar is only for a rack too narrow
  // for the row itself; long patterns scroll with the step slider in the bar below.
  if (ImGuiWindow* pw = ImGui::GetCurrentWindow(); pw->DC.CursorMaxPos.x > originX + rowW) pw->DC.CursorMaxPos.x = originX + rowW;
  gShown = shown;
  {  // K-136: the rows' content height (step lamps, rows, automation rows, graph editor, both paddings)
    const ImGuiWindow* rw = ImGui::GetCurrentWindow();
    gFit.rowsContent = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + rw->WindowPadding.y +
                       (rw->ScrollbarX ? ImGui::GetStyle().ScrollbarSize : 0.0f);
  }
  ImGui::EndChild();
  acceptDrops(a);
  pat = s.pattern(a.curPattern);  // a drop on the free area may have imported notes (a whole-song edit)
  // Bottom bar: add-channel button under the names, time scroller under the
  // steps (bar ticks, the playing position and a thumb for the visible part).
  float restTop = 0;  // K-136: the top of the space under the add button bar
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
      gFit.addMin = ImGui::GetItemRectMin(), gFit.addMax = ImGui::GetItemRectMax();
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
    restTop = b0.y + kBarH;
    ImGui::SetCursorScreenPos(b0);  // the bar is the window's last item (its content ends under the add button)
    ImGui::Dummy(ImVec2(bw, kBarH));
  }
  // K-136: below the add button (a docked rack taller than its content) the rest is plain
  // background that still takes drops from the browser (no item there: the window's size is the dock node's).
  if (const ImGuiWindow* rw = ImGui::GetCurrentWindow(); restTop > 0 && rw->InnerRect.Max.y - restTop > 1) {
    const ImRect r(ImVec2(rw->InnerRect.Min.x, restTop), rw->InnerRect.Max);
    if (ImGui::BeginDragDropTargetCustom(r, ImGui::GetID("##rackrest"))) {
      browserui::acceptPluginDrop(a, browse::DropTarget::Rack, {});
      browserui::acceptFileDrop(a, browse::DropTarget::Rack, {});
      ImGui::EndDragDropTarget();
    }
  }
  {  // K-136: the floating rack's largest height for the next frame; a rack at its full height grows with new rows
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    gFit.docked = ImGui::IsWindowDocked();
    gFit.detached = !gFit.docked && w->Viewport && w->Viewport != ImGui::GetMainViewport();
    gFit.winH = w->Size.y;
    gFit.winBottom = w->Pos.y + w->Size.y;
    const float fit = racklayout::fitHeight(rowsTop, gFit.rowsContent, showBar ? kBarH : 0.0f, bottomChrome, s.channels.empty(), 140.0f);
    if (!gFit.docked && !gFit.detached && !w->Collapsed && prevFit > 0 && fit > prevFit + 0.5f && w->SizeFull.y >= prevFit - 1.0f) {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      const float room = vp->WorkPos.y + vp->WorkSize.y - w->Pos.y;
      if (w->SizeFull.y < room - 0.5f) ImGui::SetWindowSize(ImVec2(w->SizeFull.x, std::max(w->SizeFull.y, std::min(fit, room))));
    }
    gFit.fitH = fit;
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
    else if (k.rfind("group", 0) == 0) gRackDev.group = std::atoi(k.c_str() + 5);
    else if (k == "graph") gRackDev.graph = true;
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
  if (gRackDev.group >= kAudioClipsFilter && frame == 2) groupFilter = gRackDev.group;
  if (gRackDev.graph && frame == 2) a.rack.showGraph = true;
}

void rackDevAfter(App& a, int frame) {
  (void)a;
  if (frame == 3 && !gRackDev.dock && (gRackDev.w > 0 || gRackDev.h > 0))
    if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack"))
      ImGui::SetWindowSize(rw, ImVec2(gRackDev.w > 0 ? gRackDev.w : rw->Size.x, gRackDev.h > 0 ? gRackDev.h : rw->Size.y), ImGuiCond_Always);
}

void devRackItem(const std::string& name) { devRackRect(name); }

void rackDevDump(App& a, int frame) {
  const Song& s = a.doc.song();
  const RackShown& r = gShown;
  const racklayout::Layout& L = r.layout;
  std::printf("rack frame %d: steps %d avail %.1f nameW %.1f stepW %.1f viewW %.1f scroll %.1f scrollMax %.1f rowsScroll %.1f rowsScrollMax %.1f "
              "wheelScrolls %d rows %d knobRows %d truncated %d undo %zu\n"
              "  geom originX %.1f row0Y %.1f viewX0 %.1f graphY %.1f rowPitch %.1f addbutton %.1f %.1f %.1f %.1f"
              " docked %d winH %.1f fitH %.1f winBottom %.1f lastRowBottom %.1f addrow %.1f %.1f %.1f %.1f rowsContent %.1f",
              frame, L.steps, static_cast<double>(a.rack.lastAvail), static_cast<double>(L.nameW), static_cast<double>(L.stepW),
              static_cast<double>(L.viewW), static_cast<double>(r.scroll), static_cast<double>(L.scrollMax), static_cast<double>(r.rowsScrollY),
              static_cast<double>(r.rowsScrollMaxY), r.wheelScrolls ? 1 : 0, r.rows, r.knobRows, r.truncated, a.doc.history().position(),
              static_cast<double>(r.originX), static_cast<double>(r.row0Y), static_cast<double>(r.viewX0), static_cast<double>(r.graphY),
              static_cast<double>(r.rowPitch), static_cast<double>(gRackDev.addMin.x), static_cast<double>(gRackDev.addMin.y),
              static_cast<double>(gRackDev.addMax.x), static_cast<double>(gRackDev.addMax.y), gFit.docked ? 1 : 0,
              static_cast<double>(gFit.winH), static_cast<double>(gFit.fitH), static_cast<double>(gFit.winBottom),
              static_cast<double>(r.lastRowBottom), static_cast<double>(gFit.addMin.x), static_cast<double>(gFit.addMin.y),
              static_cast<double>(gFit.addMax.x), static_cast<double>(gFit.addMax.y), static_cast<double>(gFit.rowsContent));
  // K-146: the rows pane's own scroll state, on the same line
  std::printf(" paneBarX %d paneBarY %d paneScrollX %.1f paneContentW %.1f paneInnerW %.1f itemsRight %.1f rowRight %.1f\n", r.barX ? 1 : 0,
              r.barY ? 1 : 0, static_cast<double>(r.paneScrollX), static_cast<double>(r.paneContentW), static_cast<double>(r.paneInnerW),
              static_cast<double>(r.itemsRight), static_cast<double>(r.rowRight));
  const Pattern* pat = s.pattern(a.curPattern);
  for (size_t i = 0; i < s.channels.size(); ++i) {
    const Channel& c = s.channels[i];
    int velSum = 0, pitchSum = 0;
    if (pat) {
      for (uint8_t v : rowOf(*pat, c.id)) velSum += v;
      if (auto it = pat->stepProps.find(c.id); it != pat->stepProps.end())
        for (const auto& p : it->second) pitchSum += p.pitch < 0 ? 60 : p.pitch;
    }
    const auto lt = gRackLights.find(c.id);  // J-186: the row's play light as drawn last
    const float light = lt == gRackLights.end() ? 0.0f : lt->second.light.alpha();
    std::printf("  channel %zu volume %.4f pan %.4f velsum %d pitchsum %d light %.3f muted %d hidden %d\n", i, static_cast<double>(c.gain),
                static_cast<double>(c.pan), velSum, pitchSum, static_cast<double>(light), c.muted ? 1 : 0, c.rackHidden ? 1 : 0);
    if (const auto g = gRowGeom.find(c.id); g != gRowGeom.end() && !c.rackHidden) {  // K-127: the row's controls, left to right
      const RowGeom& r = g->second;
      const std::pair<const char*, ImVec4> parts[] = {{"pan", r.pan}, {"vol", r.vol}, {"mix", r.mix},       {"name", r.name},
                                                      {"lamp", r.lamp}, {"steps", r.steps}, {"phones", r.phones}};
      std::printf("  rowgeom %zu lights %d selected %d", i, r.lights, a.selChannels.count(c.id) ? 1 : 0);
      for (const auto& [n, v] : parts)
        std::printf(" %s %.1f %.1f %.1f %.1f", n, static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z), static_cast<double>(v.w));
      std::printf(" phonestate %s\n", r.phonesState);  // K-186: the headphones button (on, muted, solo)
      if (pat)  // K-255: the row's lane and its controls
        std::printf("  lane %zu grid %d steps %d length %d mode %s chip %.1f %.1f %.1f %.1f handle %.1f %.1f %.1f %.1f step0 %.1f step1 %.1f\n", i,
                    lanes::grid(*pat, c.id), lanes::steps(*pat, c.id), lanes::length(*pat, c.id), lanes::runsOn(*pat, c.id) ? "runOn" : "restart",
                    static_cast<double>(r.chip.x), static_cast<double>(r.chip.y), static_cast<double>(r.chip.z), static_cast<double>(r.chip.w),
                    static_cast<double>(r.handle.x), static_cast<double>(r.handle.y), static_cast<double>(r.handle.z), static_cast<double>(r.handle.w),
                    static_cast<double>(r.step0), static_cast<double>(r.step1));
    }
  }
  // K-173: the separator above the automation rows and the first automation row
  std::printf("  autosep y %.1f top %.1f bottom %.1f x %.1f %.1f caption %.1f %.1f %.1f %.1f autoRows %d autoY0 %.1f lastRowIndex %d\n",
              static_cast<double>(r.sepY), static_cast<double>(r.sepTop), static_cast<double>(r.sepBottom), static_cast<double>(r.sepX0),
              static_cast<double>(r.sepX1), static_cast<double>(r.sepCaption.x), static_cast<double>(r.sepCaption.y),
              static_cast<double>(r.sepCaption.z), static_cast<double>(r.sepCaption.w), r.autoRows, static_cast<double>(r.autoY0), r.lastRowIndex);
  // The display groups and where the group combo and its menu rows were drawn (last).
  std::printf("  groups %zu filter %d\n", s.channelGroups.size(), groupFilter);
  {  // K-349: each channel as the rack listed it last frame: an audio clip channel, in view, its steps
    // (an audio clip channel is a row of steps like any other channel's)
    const std::set<ObjectId> clips = ops::audioClipChannels(s);
    const Pattern* pat = s.pattern(a.curPattern);
    for (size_t i = 0; i < s.channels.size(); ++i) {
      const Channel& c = s.channels[i];
      int on = 0;
      if (pat)
        if (auto st = pat->steps.find(c.id); st != pat->steps.end())
          for (uint8_t v : st->second) on += v ? 1 : 0;
      std::printf("  rackrow %zu id %u audioclip %d steps %d visible %d shown %d name %s\n", i, static_cast<unsigned>(c.id), clips.count(c.id) ? 1 : 0, on,
                  r.visible.count(c.id) ? 1 : 0, filterShows(c, clips) && !c.rackHidden ? 1 : 0, c.name.c_str());
    }
  }
  for (const auto& [name, rc] : gRackDev.rects)
    if (rc.frame >= ImGui::GetFrameCount() - 2)
      std::printf("  rect %s %.1f %.1f %.1f %.1f\n", name.c_str(), static_cast<double>(rc.a.x), static_cast<double>(rc.a.y),
                  static_cast<double>(rc.b.x), static_cast<double>(rc.b.y));
  std::fflush(stdout);
}

}  // namespace gl
