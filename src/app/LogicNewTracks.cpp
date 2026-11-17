// Gridline - K-112b: the New tracks dialog. See LogicNewTracks.h.
#include "LogicNewTracks.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MenuOps.h"
#include "../core/PluginInfo.h"
#include "../core/TrackModes.h"
#include "App.h"
#include "LiveLook.h"
#include "ContextMenus.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::newtracks {
namespace {

using theme::pal;

bool gOpen = false, gAppearing = false;
int gCentreFrames = 0;  // K-297 (round 7): frames the dialog is centred again while its auto size settles
int gClosedAt = -10;  // R06-07: the frame the dialog closed (its Return is not a shortcut too)
int gKind = 0;  // 0 instrument, 1 empty playlist track, 2 mixer track, 3 audio track (R05-09)
int gCount = 1;
std::string gType = "gl.synth";
bool gOpenEditor = true;

struct Kind {
  const char* name;
  const char* line;  // what it makes, one short line
  ui::Icon icon;
  uint32_t badge;  // the card's icon disc
};
const Kind kKinds[] = {
    {i18n::N_("Instrument"), i18n::N_("A channel with an instrument, on a track"), ui::Icon::CatSynth, 0x3B8F55},
    {i18n::N_("Track"), i18n::N_("An empty track for any regions, MIDI or audio"), ui::Icon::Playlist, 0x5B57C9},
    {i18n::N_("Mixer track"), i18n::N_("A mixer track for effects and sends"), ui::Icon::Mixer, 0x2F7EDB},
    {i18n::N_("Audio"), i18n::N_("A track for recording and audio regions, on a mixer track of its own"), ui::Icon::Wave, 0xB0573A},
};
constexpr int kKindCount = 4;

// One kind as a card: an icon disc, the name, a line under it; the chosen one has an accent ring.
bool card(App& a, int k, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(k);
  const bool hit = ImGui::InvisibleButton("##card", size);
  ImGui::PopID();
  const bool hov = ImGui::IsItemHovered(), on = gKind == k;
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::mix(bg, P.text, on ? 0.10f : hov ? 0.08f : 0.05f), 16.0f);  // checkpoint 26: rounder, as 067
  // Checkpoint 36: the chosen card's ring in its own colour (067 rings the chosen kind in its disc's colour).
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), on ? P.accent : theme::withAlpha(P.text, 0.12f), 16.0f, 0, on ? 2.0f : 1.0f);  // R09-09: the ring in Smooth's blue
  const float cx = std::round(p.x + size.x * 0.5f), discR = 30.0f, discY = p.y + 20 + discR;  // K-112b (checkpoint 26): a larger disc, as 067
  dl->AddCircleFilled(ImVec2(cx, discY), discR, theme::rgb(kKinds[k].badge), 40);
  ui::drawIconBox(dl, kKinds[k].icon, ImVec2(cx, discY), 28.0f, pal().paper);
  const char* name = i18n::tr(kKinds[k].name);
  const float nf = theme::kFontTitle;
  const float nw = a.fontBold->CalcTextSizeA(nf, FLT_MAX, 0, name).x;
  dl->AddText(a.fontBold, nf, ImVec2(std::round(cx - nw * 0.5f), std::round(discY + discR + 10)), P.text, name);
  const char* line = i18n::tr(kKinds[k].line);
  const float lf = theme::kFontCaption;
  const char* const lineEnd = line + std::strlen(line);
  auto lines = [&](float w) {
    int n = 0;
    for (const char* s = line; s < lineEnd; ++n) {
      const char* e = a.fontRegular->CalcWordWrapPosition(lf, s, lineEnd, w);
      s = e == s ? s + 1 : e;
      while (s < lineEnd && *s == ' ') ++s;
    }
    return n;
  };
  // K-112b (second pass): balanced lines. The narrowest width that keeps the same number of lines,
  // so a description never leaves one word alone on its last line.
  float room = size.x - 24;
  if (const int n = lines(room); n > 1) {
    float lo = room * 0.3f, hi = room;
    for (int i = 0; i < 12; ++i) {
      const float mid = (lo + hi) * 0.5f;
      (lines(mid) <= n ? hi : lo) = mid;
    }
    room = std::ceil(hi);
  }
  float ly = std::round(discY + discR + 10 + nf + 6);
  for (const char *s = line, *end = lineEnd; s < end;) {  // wrapped, each line centred
    const char* e = a.fontRegular->CalcWordWrapPosition(lf, s, end, room);
    if (e == s) e = s + 1;
    const float lw = a.fontRegular->CalcTextSizeA(lf, FLT_MAX, 0, s, e).x;
    dl->AddText(a.fontRegular, lf, ImVec2(std::round(cx - lw * 0.5f), ly), P.textDim, s, e);
    ly += lf + 3;
    s = e;
    while (s < end && *s == ' ') ++s;
  }
  return hit;
}

void create(App& a) {
  const int n = std::clamp(gCount, 1, 16);
  if (gKind == 0) {
    ObjectId last = kNoId;
    for (int i = 0; i < n; ++i) {
      last = a.addInstrument(gType);
      // R05-08: each instrument gets a track named after it, as the reference's software instrument
      // track: the first empty free track, else a new one at the end; it is selected.
      if (last == kNoId) continue;
      const Song& s = a.doc.song();
      int t = -1;
      for (int k = 0; k < static_cast<int>(s.tracks.size()) && t < 0; ++k) {
        bool used = trackmodes::modeOf(s, k) != TrackMode::Unassigned;
        for (const Clip& c : s.playlist) used = used || c.track == k;
        if (!used) t = k;
      }
      if (t < 0 && ops::insertPlaylistTrack(a.doc, static_cast<int>(s.tracks.size()))) t = static_cast<int>(a.doc.song().tracks.size()) - 1;
      if (t < 0) continue;
      trackmodes::makeInstrumentTrack(a.doc, t, last);
      if (const Channel* ch = a.doc.song().channel(last)) {
        const std::string nm = ch->name;
        a.doc.editPlaylistTrack(t, "name track", [nm](PlaylistTrack& x) { x.name = nm; });
      }
      a.playlist.trackSel = {t};
    }
    if (gOpenEditor && last != kNoId) a.showNewInstrument(last);
  } else if (gKind == 1 && livelook::active(a)) {
    // K-112c (round 39): in Clips a new track comes right after the tracks in use, so it shows at
    // once as an arrangement row and a clip grid column (appending after the empty tracks hid it).
    trackmodes::addUsedTrack(a.doc, n);  // one undo step
  } else if (gKind == 1) {
    for (int i = 0; i < n && static_cast<int>(a.doc.song().tracks.size()) < kMaxPlaylistTracks; ++i)
      ops::insertPlaylistTrack(a.doc, static_cast<int>(a.doc.song().tracks.size()));
  } else if (gKind == 3) {  // R05-09
    for (int i = 0; i < n; ++i)
      if (const int t = trackmodes::addAudioTrack(a.doc); t >= 0) {
        a.playlist.trackSel = {t};
        // R06-13: named as the reference names a new audio track: "Audio 1", "Audio 2", ...
        const Song& s = a.doc.song();
        int k = 0;
        for (int j = 0; j < static_cast<int>(s.tracks.size()); ++j) k += j != t && trackmodes::modeOf(s, j) == TrackMode::Audio ? 1 : 0;
        const std::string nm = std::string(i18n::tr("Audio")) + " " + std::to_string(k + 1);
        a.doc.editPlaylistTrack(t, "name track", [nm](PlaylistTrack& x) { x.name = nm; });
        // R07-11: its strip carries the same name (it read "2 Audio")
        if (const MixerTrack* mt = trackmodes::linkedMixer(a.doc.song(), t))
          a.doc.editMixerTrack(mt->id, "name track", [nm](MixerTrack& x) { x.name = nm; });
      }
  } else {
    ObjectId last = kNoId;
    for (int i = 0; i < n; ++i)
      if (const ObjectId id = a.doc.addMixerTrack(); id != kNoId) last = id;
    if (last != kNoId) a.selMixer = last;
  }
}

}  // namespace

void open() {
  gOpen = gAppearing = true;
  gCentreFrames = 4;
}
bool isOpen() { return gOpen; }
bool holdsKeys() { return gOpen || ImGui::GetFrameCount() <= gClosedAt + 1; }

void draw(App& a) {
  if (!gOpen) return;
  const auto& P = pal();
  const float cardW = 196, cardH = 172, gap = theme::kS3, pad = theme::kS6;
  const float w = cardW * kKindCount + gap * (kKindCount - 1) + pad * 2;
  // (The first time it opens its height is not known yet: it is centred again until its auto size
  // settles. It opened at the top left corner, centred only from the second time on.)
  ui::PlaceNextDialog(ImVec2(w, 0), gCentreFrames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing);
  if (gCentreFrames > 0) --gCentreFrames;
  if (gAppearing) ImGui::SetNextWindowFocus();
  gAppearing = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, theme::kS4));
  const bool vis = ImGui::Begin(ui::windowTitle("New tracks").c_str(), &gOpen,
                                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  for (int k = 0; k < kKindCount; ++k) {
    if (k) ImGui::SameLine(0, gap);
    if (card(a, k, ImVec2(cardW, cardH))) gKind = k;
  }
  ImGui::Dummy(ImVec2(0, theme::kS2));
  // The chosen kind's details, on one form column.
  constexpr float col = 132;
  {  // Checkpoint 36: under a disclosure heading, on a lighter band the dialog's full width, as 067
    ImDrawList* hdl = ImGui::GetWindowDrawList();
    const ImVec2 hp = ImGui::GetCursorScreenPos();
    const float hh = ImGui::GetFrameHeight();
    ui::drawIcon(hdl, ui::Icon::ChevronDown, ImVec2(hp.x + 5, std::round(hp.y + hh * 0.5f)), 9, P.textDim);
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 16, hp.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Details"));
  }
  ImDrawList* bandDl = ImGui::GetWindowDrawList();
  bandDl->ChannelsSplit(2);
  bandDl->ChannelsSetCurrent(1);
  const float bandY0 = ImGui::GetCursorScreenPos().y;
  ImGui::Dummy(ImVec2(0, theme::kS2));
  ui::SetFormColumn(col);  // labels end at the column
  if (gKind == 0) {
    const PluginInfo* info = findPlugin(gType);
    ui::FormLabelFit(i18n::tr("Instrument"), col, P.textDim);
    if (ui::BeginCombo("##ntinst", info ? info->name.c_str() : i18n::tr("Choose one"), 260)) {
      menus::instrumentItems([](const std::string& t) { gType = t; }, gType);
      ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SameLine(col);
    ImGui::Checkbox(i18n::tr("Open its editor"), &gOpenEditor);
  } else {
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::SameLine(col);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", gKind == 1   ? i18n::tr("Added below the last track")
                                                       : gKind == 3 ? i18n::tr("Records from the audio input; added after the tracks in use")
                                                                    : i18n::tr("Added after the last mixer track"));
  }
  ui::SetFormColumn(0);
  // Footer: how many in the middle, Cancel and Create on the right.
  ImGui::Dummy(ImVec2(0, theme::kS4));
  const ImVec2 f0 = ImGui::GetCursorScreenPos();
  const float fw = ImGui::GetContentRegionAvail().x;
  bandDl->ChannelsSetCurrent(0);
  bandDl->AddRectFilled(ImVec2(f0.x - pad, bandY0), ImVec2(f0.x + fw + pad, f0.y), theme::mix(ImGui::GetColorU32(ImGuiCol_WindowBg), P.text, 0.07f));
  bandDl->ChannelsMerge();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(f0.x - pad, f0.y), ImVec2(f0.x + fw + pad, f0.y), P.dark ? theme::withAlpha(P.ink, 0.6f) : P.lineStrong);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  const char* howMany = i18n::tr("Number of tracks");
  const float hw = ImGui::CalcTextSize(howMany).x, fieldW = 56;
  const float y = ImGui::GetCursorPosY();
  ImGui::SetCursorPosX(std::round(pad + (fw - hw - theme::kS2 - fieldW) * 0.5f));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", howMany);
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::InputInt("##ntcount", &gCount, 0, 0)) gCount = std::clamp(gCount, 1, 16);
  ImGui::SetCursorPos(ImVec2(pad + fw - ui::ButtonRowWidth(2, 96), y));
  const bool cancel = ui::TextButton(i18n::tr("Cancel"), false, 96) || keys::escapePressed();
  ImGui::SameLine(0, theme::kS2);
  const bool can = gKind != 0 || findPlugin(gType) != nullptr;
  // R06-07: Create is the default button: Return (or Enter) presses it while the dialog has the keys.
  const bool enter = can && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
                     (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
  const bool ok = ui::PrimaryButton(i18n::tr("Create"), 96, nullptr, can) || enter;
  if (ok) {
    a.later([&a] { create(a); });
    gOpen = false;
  } else if (cancel) {
    gOpen = false;
  }
  if (!gOpen) gClosedAt = ImGui::GetFrameCount();
  ImGui::End();
}

// K-112D (D3): Create with the dialog's defaults (one Tercet instrument track, no editor opened), for tests.
void createDefault(App& a) {
  const int k = gKind, n = gCount;
  const std::string t = gType;
  const bool ed = gOpenEditor;
  gKind = 0, gCount = 1, gType = "gl.synth", gOpenEditor = false;
  create(a);
  gKind = k, gCount = n, gType = t, gOpenEditor = ed;
}

}  // namespace gl::newtracks
