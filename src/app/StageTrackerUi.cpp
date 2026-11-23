// Gridline - the Stage in the Tracker layout (its Stage page, beside Mix): the same stage, gestures,
// keys and edits as app/StageView.cpp draws them everywhere, in the Tracker's own look: the band's
// bevelled boxes and buttons, sunken wells, the monospaced face for numbers (two-digit hex for track
// numbers), hard pixel lines, its spacing scale (TrackerLook.h) and the theme's roles. Every edit goes
// through the mixer parameters and the Document as the Stage's own panel makes it (one undo step each,
// synced to collaborators). Only the Tracker layout draws this; the other layouts keep their Stage.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/MixerOps.h"
#include "../core/Params.h"
#include "../core/StageMap.h"
#include "../core/StageRoomOps.h"
#include "../core/TrackerGrid.h"
#include "App.h"
#include "MixerMeters.h"
#include "PixelGrid.h"
#include "StageView.h"
#include "StageViewInternal.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::stageui::detail {

using theme::pal;
using trackerview::bevelBox;
using trackerview::bevelButton;
using trackerview::boxTitle;
using trackerview::monoFont;
namespace sp = trackerview::space;

namespace {

float fsz() { return ImGui::GetFontSize(); }
float rowH() { return std::round(fsz() + 6); }
const Song& song(App& a) { return a.doc.song(); }

// A button that keeps the keys on the page (the band's buttons hand them back to the pattern).
bool button(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on, const char* hint) {
  const bool c = bevelButton(a, id, label, p0, p1, on, hint);
  if (c) trackerview::keepKeysHere();
  return c;
}

// Text in the monospaced face, right-aligned in [x0, x1] (cut at the left edge).
void monoRight(App& a, ImDrawList* dl, float x0, float x1, float y, ImU32 col, const std::string& t) {
  ImFont* mono = monoFont(a);
  const float w = mono->CalcTextSizeA(fsz(), FLT_MAX, 0, t.c_str()).x;
  if (w <= x1 - x0) dl->AddText(mono, fsz(), ImVec2(std::round(x1 - w), y), col, t.c_str());
  else theme::textEllipsis(dl, mono, fsz(), ImVec2(x0, y), x1 - x0, col, t.c_str());
}

// A sunken readout field (a value in the values' ink).
void readout(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& t, bool dim = false) {
  bevelBox(dl, p0, p1, true);
  monoRight(a, dl, p0.x + 4, p1.x - 5, std::round((p0.y + p1.y - fsz()) * 0.5f), dim ? pal().textFaint : trackerview::valueInk(), t);
}

std::string numbered(App& a, size_t index) { return trk::hex2(static_cast<int>(index)) + " " + mixerTrackName(song(a), index); }

std::string panShort(double pan) {
  const int pc = static_cast<int>(std::lround(std::fabs(pan) * 100.0));
  if (pc == 0) return "C";
  char b[16];
  std::snprintf(b, sizeof b, "%s%d", pan < 0 ? "L" : "R", pc);
  return b;
}

std::string dbShort(double gain) {
  if (gain <= 1e-6) return "-inf";
  char b[24];
  std::snprintf(b, sizeof b, "%+.1f dB", 20.0 * std::log10(gain));
  return b;
}

// One parameter row: the label in the label column, a value bar, the value in a sunken field at the right.
// Dragging the bar sets the parameter (one undo step per drag); a double-click its default.
void paramRow(App& a, const char* id, const char* label, ParamId pid, bool centred, const std::string& value, ImVec2 p0, float w, float labelW,
              float valueW, const char* hint, bool enabled = true) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = rowH() - 1, fs = fsz();
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x, std::round(p0.y + (h - fs) * 0.5f)), enabled ? P.textDim : P.textFaint, i18n::tr(label));
  const float bx0 = p0.x + labelW, bx1 = p0.x + w - valueW - sp::kGap;
  const auto d = describeParam(song(a), pid);
  const auto v = getParam(song(a), pid);
  if (!enabled || !d || !v) ImGui::BeginDisabled();
  double nv = 0;
  if (trackerview::valueBar(id, ImVec2(bx0, p0.y), ImVec2(bx1, p0.y + h), v.value_or(0.0), d ? d->min : 0.0, d ? d->max : 1.0, centred, &nv, hint) && d &&
      enabled) {
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) nv = d->def;
    a.doc.setParam(pid, nv, true);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();  // the drag is one undo step
  if (!enabled || !d || !v) ImGui::EndDisabled();
  readout(a, dl, ImVec2(p0.x + w - valueW, p0.y), ImVec2(p0.x + w, p0.y + h), value, !enabled);
}

// A row for several tracks (or the room): the label, the value in the field, the arrows step them all.
int stepRow(App& a, const char* id, const char* label, const std::string& value, ImVec2 p0, float w, float labelW, const char* hint) {
  return trackerview::stepper(a, id, label, value, p0, w, rowH() - 1, hint, labelW, true);
}

// A segmented level meter in a sunken box (the Mix page's).
void meterRow(App& a, ImVec2 p0, ImVec2 p1, ObjectId id) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bevelBox(dl, p0, p1, true);
  const float lvl = std::clamp(mixmeters::get(a, id).peak(), 0.0f, 1.0f);
  const int segs = std::max(4, static_cast<int>((p1.x - p0.x - 4) / 5));
  const int lit = static_cast<int>(std::round(lvl * static_cast<float>(segs)));
  for (int k = 0; k < segs; ++k) {
    const float sx = std::round(p0.x + 2 + static_cast<float>(k) * 5);
    const ImU32 col = k >= segs * 9 / 10 ? P.danger : k >= segs * 7 / 10 ? P.hot : P.led;
    dl->AddRectFilled(ImVec2(sx, p0.y + 3), ImVec2(sx + 3, p1.y - 3), k < lit ? col : theme::withAlpha(col, 0.12f));
  }
}

// A sunken name field: number, colour mark, name (the Mix page's).
void nameField(App& a, ImVec2 p0, ImVec2 p1, const std::string& num, ImU32 colour, const std::string& name, bool selected, bool dim) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = fsz();
  ImFont* mono = monoFont(a);
  bevelBox(dl, p0, p1, true);
  if (selected) dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), theme::withAlpha(P.surface600, 0.9f));
  if (colour) dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 3), ImVec2(p0.x + 5, p1.y - 3), colour);
  const float ty = std::round(p0.y + (p1.y - p0.y - fs) * 0.5f);
  dl->AddText(mono, fs, ImVec2(p0.x + 9, ty), selected ? P.hot : P.textDim, num.c_str());
  const float nx = p0.x + 9 + mono->CalcTextSizeA(fs, FLT_MAX, 0, (num + " ").c_str()).x;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), p1.x - nx - 4, dim ? P.textFaint : selected ? P.text : P.textDim, name.c_str());
}

// The tracks the panel is about: the selection on the stage, or the focused track alone.
std::vector<const Tok*> chosen(App& a) {
  std::vector<const Tok*> sel;
  for (const Tok& t : view().toks)
    if (t.selected) sel.push_back(&t);
  if (sel.size() > 1) return sel;
  if (const Tok* one = tokOf(a.selMixer); one && (sel.empty() || sel.front() == one)) return {one};
  return {};
}

std::vector<ObjectId> idsOf(const std::vector<const Tok*>& toks) {
  std::vector<ObjectId> ids;
  for (const Tok* t : toks) ids.push_back(t->id);
  return ids;
}

bool same(const std::vector<const Tok*>& toks, double Tok::*f) {
  for (const Tok* t : toks)
    if (std::fabs(t->*f - toks.front()->*f) > 1e-4) return false;
  return true;
}

// ---- the boxes (one track, several, the room) in the console column

// "Track": who it is about and its buttons. Rows: the name, the detail line, the buttons.
void boxWho(App& a, ImVec2 p0, ImVec2 p1, const std::vector<const Tok*>& sel) {
  const auto& P = pal();
  const Song& s = song(a);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, x1 = p1.x - sp::kPad;
  float y = p0.y + sp::kPad;
  boxTitle(a, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), sel.empty() ? "Room" : sel.size() == 1 ? "Track" : "Tracks");
  y += rh + sp::kGap;
  const float bw = std::floor((x1 - x0 - 2 * sp::kGap) / 3.0f);
  auto three = [&](float yy) {
    return std::vector<ImVec2>{ImVec2(x0, yy), ImVec2(x0 + bw + sp::kGap, yy), ImVec2(x0 + 2 * (bw + sp::kGap), yy)};
  };
  if (sel.empty()) {
    const int r = shownRoom(a);
    nameField(a, ImVec2(x0, y), ImVec2(x1, y + rh - 1), trk::hex2(r + 1), 0, roomops::roomName(s, r), true, false);
    y += rh + sp::kGap;
    char b[96];
    std::snprintf(b, sizeof b, i18n::tr("%s tracks \xC2\xB7 nothing selected"), trk::hex2(static_cast<int>(view().toks.size())).c_str());
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + 2, std::round(y + (rh - 1 - fs) * 0.5f)), x1 - x0 - 2, P.textDim, b);
    y += rh + sp::kGap;
    const float hw = std::floor((x1 - x0 - sp::kGap) * 0.5f);
    if (button(a, "##twspread", "Spread out", ImVec2(x0, y), ImVec2(x0 + hw, y + rh - 1), false, "Space the tracks evenly from the centre outward, each keeping its distance"))
      startPreview(a, PreviewKind::Spread);
    if (button(a, "##twsound", "By sound", ImVec2(x0 + hw + sp::kGap, y), ImVec2(x1, y + rh - 1), false,
               "Arrange by sound: place each track by what it is (shown first, nothing changes until Apply)"))
      startPreview(a, PreviewKind::Sound);
    return;
  }
  const std::vector<ObjectId> ids = idsOf(sel);
  bool allMuted = true, allSolo = true;
  for (const Tok* t : sel) allMuted = allMuted && t->muted, allSolo = allSolo && t->solo;
  if (sel.size() == 1) {
    const Tok& t = *sel.front();
    const MixerTrack& m = s.mixer[t.index];
    nameField(a, ImVec2(x0, y), ImVec2(x1, y + rh - 1), trk::hex2(static_cast<int>(t.index)), theme::content(m.color), t.name, true, t.muted);
    y += rh + sp::kGap;
    int fx = 0;
    for (const auto& sl : m.slots) fx += sl.plugin.type.empty() ? 0 : 1;
    char b[96];
    std::snprintf(b, sizeof b, fx == 1 ? i18n::tr("Insert %s \xC2\xB7 1 effect") : i18n::tr("Insert %s \xC2\xB7 %d effects"), trk::hex2(static_cast<int>(t.index)).c_str(), fx);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + 2, std::round(y + (rh - 1 - fs) * 0.5f)), x1 - x0 - 2, P.textDim, b);
  } else {
    std::string names;
    for (const Tok* t : sel) names += (names.empty() ? "" : ", ") + t->name;
    char b[32];
    std::snprintf(b, sizeof b, "%s", trk::hex2(static_cast<int>(sel.size())).c_str());
    nameField(a, ImVec2(x0, y), ImVec2(x1, y + rh - 1), b, 0, i18n::tr("tracks selected"), true, false);
    y += rh + sp::kGap;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + 2, std::round(y + (rh - 1 - fs) * 0.5f)), x1 - x0 - 2, P.textDim, names.c_str());
  }
  y += rh + sp::kGap;
  const auto at = three(y);
  if (button(a, "##twmute", "Mute", at[0], ImVec2(at[0].x + bw, y + rh - 1), allMuted, sel.size() == 1 ? "Mute this track" : "Mute or unmute all of them"))
    muteTracks(a, ids);
  if (button(a, "##twsolo", "Solo", at[1], ImVec2(at[1].x + bw, y + rh - 1), allSolo, sel.size() == 1 ? "Hear only this track (and what feeds it)" : "Solo all of them"))
    soloTracks(a, ids);
  if (sel.size() == 1) {
    if (button(a, "##twfx", "Effects", at[2], ImVec2(x1, y + rh - 1), false, "This track's effects")) openEffects(a, ids.front());
  } else if (button(a, "##twspread2", "Spread", at[2], ImVec2(x1, y + rh - 1), false, "Space only these tracks evenly from the centre")) {
    startPreview(a, PreviewKind::Spread);
  }
}

// "Place": left / right, distance, width, focus (one track: bars; several: steps for all; the room: its space).
void boxPlace(App& a, ImVec2 p0, ImVec2 p1, const std::vector<const Tok*>& sel) {
  const Song& s = song(a);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, w = p1.x - p0.x - 2 * sp::kPad;
  float y = p0.y + sp::kPad;
  boxTitle(a, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), sel.empty() ? "Space" : "Place");
  y += rh + sp::kGap;
  const float labelW = trackerview::labelColumn(a, {"Left / right", "Distance", "Width", "Focus", "Space size", "Depth", "Room width"});
  const float valueW = std::round(std::min(w * 0.36f, monoFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, "Very wide").x + 12));
  const bool heard = shownStage(a).hasRoom();
  char b[48];
  if (sel.empty()) {
    const int r = shownRoom(a);
    const MixerTrack* room = heard ? mixops::stageRoom(s, r) : nullptr;
    static bool sizeOn = false, depthOn = false, widthOn = false;
    auto bar = [&](const char* id, const char* label, double v, double lo, double hi, double def, const std::string& value, bool on, bool* merge, auto set) {
      ImDrawList* d2 = ImGui::GetWindowDrawList();
      d2->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), on ? pal().textDim : pal().textFaint, i18n::tr(label));
      if (!on) ImGui::BeginDisabled();
      double nv = 0;
      if (trackerview::valueBar(id, ImVec2(x0 + labelW, y), ImVec2(x0 + w - valueW - sp::kGap, y + rh - 1), v, lo, hi, false, &nv) && on) {
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) nv = def;
        set(nv, *merge);
        *merge = true;
      }
      if (ImGui::IsItemDeactivated()) *merge = false, a.doc.endGesture();
      if (!on) ImGui::EndDisabled();
      readout(a, d2, ImVec2(x0 + w - valueW, y), ImVec2(x0 + w, y + rh - 1), value, !on);
      y += rh + sp::kGap;
    };
    bar("##tpsize", "Space size", mixops::roomSize(s, r), 0, 1, 0.4, room ? i18n::tr(st::roomSizeWord(mixops::roomSize(s, r))) : i18n::tr("None"), room != nullptr,
        &sizeOn, [&](double v, bool m) { mixops::setRoomSize(a.doc, v, m, r); });
    bar("##tpdepth", "Depth", shownStage(a).depthStrength, 0, 1, 0.5, room ? i18n::tr(st::strengthWord(shownStage(a).depthStrength)) : i18n::tr("Not heard"),
        room != nullptr, &depthOn, [&](double v, bool m) { mixops::setDepthStrength(a.doc, v, m, r); });
    std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(shownStage(a).width * 100.0f)));
    bar("##tpwidth", "Room width", shownStage(a).width, 0, 1.5, 1.0, b, true, &widthOn, [&](double v, bool m) { mixops::setRoomWidth(a.doc, v, m, r); });
    if (room) {
      paramRow(a, "##tproomvol", "Level", mixerParam(room->id, MixerParam::Volume), false, dbShort(room->volume), ImVec2(x0, y), w, labelW, valueW,
               "The space's level for the whole room");
    }
    return;
  }
  const std::vector<ObjectId> ids = idsOf(sel);
  if (sel.size() == 1) {
    const Tok& t = *sel.front();
    const ObjectId id = t.id;
    const bool dragged = view().dragId == id;
    const double pan = dragged ? view().dragPan : t.pan, depth = dragged ? view().dragDepth : t.depth;
    paramRow(a, "##tppan", "Left / right", mixerParam(id, MixerParam::Pan), true, panShort(pan), ImVec2(x0, y), w, labelW, valueW, "Left and right on the stage (its pan)");
    y += rh + sp::kGap;
    paramRow(a, "##tpdist", "Distance", mixerParam(id, MixerParam::Depth), false, i18n::tr(st::distanceWord(depth)), ImVec2(x0, y), w, labelW, valueW,
             heard ? "Near or far: further back is quieter, duller and has more of the room" : "Near or far (kept, not heard: pick a space)");
    y += rh + sp::kGap;
    std::snprintf(b, sizeof b, "%.0f%%", st::widthPercent(t.sep));
    paramRow(a, "##tpsep", "Width", mixerParam(id, MixerParam::StereoSep), true, b, ImVec2(x0, y), w, labelW, valueW, "Its stereo width (the diamond under it)");
    y += rh + sp::kGap;
    paramRow(a, "##tpfocus", "Focus", mixerParam(id, MixerParam::Focus), true, i18n::tr(st::focusWord(t.focus)), ImVec2(x0, y), w, labelW, valueW,
             heard ? "Soft to present" : "Soft to present (kept, not heard: pick a space)");
    return;
  }
  // Several: each arrow moves every one from where it is (the keys' steps).
  auto word = [&](double Tok::*f, auto fn) { return same(sel, f) ? fn(sel.front()->*f) : std::string(i18n::tr("Mixed")); };
  int d = stepRow(a, "##tspan", "Left / right", word(&Tok::pan, [](double v) { return panShort(v); }), ImVec2(x0, y), w, labelW,
                  "Moves them all left or right, keeping their spacing");
  if (d) nudgeParam(a, ids, static_cast<int>(MixerParam::Pan), 0.05 * d, -1.0, 1.0, "move on stage");
  y += rh + sp::kGap;
  d = stepRow(a, "##tsdist", "Distance", word(&Tok::depth, [](double v) { return std::string(i18n::tr(st::distanceWord(v))); }), ImVec2(x0, y), w, labelW,
              "Moves them all nearer or further, each from its place");
  if (d) nudgeParam(a, ids, static_cast<int>(MixerParam::Depth), 0.25 * d, 0.0, 1.0, "move on stage");
  y += rh + sp::kGap;
  d = stepRow(a, "##tssep", "Width", word(&Tok::sep, [](double v) { return std::string(i18n::tr(st::widthWord(v))); }), ImVec2(x0, y), w, labelW,
              "Narrower or wider, each from its own width");
  if (d) nudgeParam(a, ids, static_cast<int>(MixerParam::StereoSep), 0.05 * d, -1.0, 1.0, "width on stage");
  y += rh + sp::kGap;
  d = stepRow(a, "##tsfocus", "Focus", word(&Tok::focus, [](double v) { return std::string(i18n::tr(st::focusWord(v))); }), ImVec2(x0, y), w, labelW,
              "Softer or more present, each from its own focus");
  if (d) nudgeParam(a, ids, static_cast<int>(MixerParam::Focus), 0.1 * d, -1.0, 1.0, "focus on stage");
}

// "Level": the fader, the send to the space and the meter.
void boxLevel(App& a, ImVec2 p0, ImVec2 p1, const std::vector<const Tok*>& sel) {
  const Song& s = song(a);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, w = p1.x - p0.x - 2 * sp::kPad;
  float y = p0.y + sp::kPad;
  boxTitle(a, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), "Level");
  y += rh + sp::kGap;
  const float labelW = trackerview::labelColumn(a, {"Level", "Space", "Meter"});
  const float valueW = std::round(std::min(w * 0.36f, monoFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, "-24.0 dB").x + 12));
  int roomIdx = -1;
  const MixerTrack* room = mixops::stageRoom(s, shownRoom(a));
  if (room) roomIdx = s.mixerIndex(room->id);
  if (sel.empty()) {  // the room: its space, picked here as on the title row
    const float bw = w;
    const std::string type = std::string(i18n::tr("Space")) + ": " + i18n::tr(mixops::roomTypeName(shownStage(a).roomType));
    if (button(a, "##tlspace", type.c_str(), ImVec2(x0, y), ImVec2(x0 + bw, y + rh - 1), ImGui::IsPopupOpen("##stageroom"),
               "The space this room plays in: distance and focus are heard once a space is picked"))
      openRoomPopover();
    y += rh + sp::kGap;
    if (room) {
      dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), pal().textDim, i18n::tr("Meter"));
      meterRow(a, ImVec2(x0 + labelW, y), ImVec2(x0 + w, y + rh - 1), room->id);
    }
    return;
  }
  const std::vector<ObjectId> ids = idsOf(sel);
  if (sel.size() == 1) {
    const Tok& t = *sel.front();
    const MixerTrack& m = s.mixer[t.index];
    paramRow(a, "##tlvol", "Level", mixerParam(m.id, MixerParam::Volume), false, levelText(t.volume), ImVec2(x0, y), w, labelW, valueW, "The fader (drag; double-click: the default)");
    y += rh + sp::kGap;
    bool sends = false;
    if (room)
      for (const auto& sd : m.sends) sends = sends || (sd.to == room->id && sd.stageRoom);
    if (sends) {
      paramRow(a, "##tlsend", "Space", sendParam(m.id, roomIdx), false, dbText(mixops::sendGain(s, m.id, room->id), false), ImVec2(x0, y), w, labelW, valueW,
               "How much of it goes to the space");
    } else {
      dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), pal().textDim, i18n::tr("Space"));
      if (button(a, "##tladdroom", "Send to the space", ImVec2(x0 + labelW, y), ImVec2(x0 + w, y + rh - 1), false,
                 "Send this track into the room (the space track is made the first time)")) {
        if (!room) {
          mixops::ensureStageRoom(a.doc, {m.id}, "add the room", shownRoom(a));
        } else {
          a.doc.beginGroup("send to the room");
          mixops::addRoomSends(a.doc, {m.id}, shownRoom(a));
          a.doc.endGroup();
        }
      }
    }
    y += rh + sp::kGap;
    dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), pal().textDim, i18n::tr("Meter"));
    meterRow(a, ImVec2(x0 + labelW, y), ImVec2(x0 + w, y + rh - 1), m.id);
    return;
  }
  const int d = stepRow(a, "##tslevel", "Level", i18n::tr("Together"), ImVec2(x0, y), w, labelW, "Louder or quieter, all together");
  if (d) stepLevel(a, ids, d);
  y += rh + sp::kGap;
  dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), pal().textDim, i18n::tr("Meter"));
  meterRow(a, ImVec2(x0 + labelW, y), ImVec2(x0 + w, y + rh - 1), ids.front());
}

// Spread out, Arrange by sound or a preset shown before it is applied: what it does, Cancel and Apply.
void boxPreview(App& a, ImVec2 p0, ImVec2 p1, bool column) {
  const auto& P = pal();
  const Preview& pv = view().preview;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, x1 = p1.x - sp::kPad;
  float y = p0.y + sp::kPad;
  const char* title = pv.kind == PreviewKind::Spread ? "Spread out" : pv.kind == PreviewKind::Sound ? "Arrange by sound" : st::preset(pv.preset).name;
  boxTitle(a, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), title);
  char b[160];
  if (pv.kind == PreviewKind::Sound) std::snprintf(b, sizeof b, i18n::tr("%zu placed, %d not sure"), pv.ids.size() - static_cast<size_t>(pv.unsure), pv.unsure);
  else std::snprintf(b, sizeof b, i18n::tr("%zu tracks"), pv.ids.size());
  const float tw = (a.fontBold ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(title)).x;
  theme::textEllipsis(dl, monoFont(a), fs, ImVec2(x0 + tw + 12, std::round(y + (rh - 1 - fs) * 0.5f)), std::max(1.0f, x1 - x0 - tw - 12), trackerview::valueInk(), b);
  y += rh + sp::kGap;
  const char* desc = pv.kind == PreviewKind::Spread ? i18n::tr("Evenly from the centre outward, left and right in turn, in mixer order. Every track keeps its distance.")
                     : pv.kind == PreviewKind::Sound
                         ? i18n::tr("Read from what plays on each track first, then how it sounds, then its name. Hover a track to see why. Tracks marked ? stay in the middle for you to place.")
                         : i18n::tr(st::preset(pv.preset).hint);
  const float bw = std::ceil(std::max(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Cancel")).x, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Apply")).x)) + 28;
  const float textR = column ? x1 : x1 - 2 * bw - 2 * sp::kGroup;
  const float wrapH = std::max(rh, (column ? p1.y - rh - 2 * sp::kPad : p1.y - sp::kPad) - y);
  dl->PushClipRect(ImVec2(x0, y), ImVec2(textR, y + wrapH), true);
  dl->AddText(a.fontRegular, fs, ImVec2(x0 + 2, y), P.textDim, desc, nullptr, textR - x0 - 4);
  dl->PopClipRect();
  const float by = column ? p1.y - sp::kPad - (rh - 1) : std::round((p0.y + p1.y - rh) * 0.5f);
  if (button(a, "##tpcancel", "Cancel", ImVec2(x1 - 2 * bw - sp::kGap, by), ImVec2(x1 - bw - sp::kGap, by + rh - 1), false, "Leave every track where it is (Esc)")) cancelPreview();
  if (button(a, "##tpapply", "Apply", ImVec2(x1 - bw, by), ImVec2(x1, by + rh - 1), true, "Move the tracks there (one undo step)")) applyPreview(a);
}

}  // namespace

bool trackerLook(App& a) { return trackerpages::active(a); }

void trackerColours() {
  const auto& P = pal();
  View& v = view();
  v.bg = P.surface950;
  v.stage = theme::mix(P.surface950, P.surface700, 0.42f);
  v.tray = P.surface700;
}

void trackerBevel(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool sunken) {
  const auto& P = pal();
  const float dpp = pixelgrid::frameScale(), w = pixelgrid::whole(1.0f, dpp);
  mn = ImVec2(pixelgrid::edge(mn.x, dpp), pixelgrid::edge(mn.y, dpp));
  mx = ImVec2(pixelgrid::edge(mx.x, dpp), pixelgrid::edge(mx.y, dpp));
  const ImU32 lit = theme::withAlpha(P.paper, 0.30f), shade = theme::withAlpha(P.ink, 0.45f);
  const ImU32 tl = sunken ? shade : lit, br = sunken ? lit : shade;
  dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + w), tl);
  dl->AddRectFilled(mn, ImVec2(mn.x + w, mx.y), tl);
  dl->AddRectFilled(ImVec2(mn.x, mx.y - w), mx, br);
  dl->AddRectFilled(ImVec2(mx.x - w, mn.y + w), mx, br);
}

// The floor in the well: the stage's trapezoid a step lighter than the well, hard edges (a lit back
// edge, a shaded front), the five distances as dotted rows with their words at the left, the centre line
// and the pan scale (L100 .. C .. R100, monospaced) along the back edge.
void trackerFloor(App& a, ImDrawList* dl) {
  const auto& P = pal();
  const View& v = view();
  const st::Geo& g = v.g;
  const trackerview::Aliased hard(dl);
  const float s = g.scale, bulge = st::kBulge * s;
  const ImVec2 tl(g.cx - g.backHalf, g.backY), trr(g.cx + g.backHalf, g.backY), br(g.cx + g.frontHalf, g.frontY), bl(g.cx - g.frontHalf, g.frontY);
  const ImVec2 ctrl(g.cx, g.frontY + 2.0f * bulge);
  const bool open = !shownStage(a).hasRoom();
  dl->PathLineTo(tl);
  dl->PathLineTo(trr);
  dl->PathLineTo(br);
  dl->PathBezierQuadraticCurveTo(ctrl, bl, 24);
  dl->PathFillConvex(open ? theme::mix(v.stage, v.bg, 0.5f) : v.stage);
  const ImU32 edgeLit = theme::mix(P.surface700, P.text, 0.30f), edgeDim = theme::mix(P.surface700, P.text, 0.12f);
  dl->AddLine(tl, trr, edgeLit);
  dl->AddLine(tl, bl, edgeDim);
  dl->AddLine(trr, br, edgeDim);
  if (!open) {
    dl->PathLineTo(br);
    dl->PathBezierQuadraticCurveTo(ctrl, bl, 24);
    dl->PathStroke(edgeLit, 0, 1.0f);
  }
  // The distance rows (where a track's foot stands at each of the five distances).
  static const char* kD[] = {i18n::N_("Front"), i18n::N_("Near"), i18n::N_("Middle"), i18n::N_("Far"), i18n::N_("Back")};
  const float fs = std::round(fsz() * 0.85f);
  ImFont* mono = monoFont(a);
  for (int k = 0; k < 5; ++k) {
    const double d = k / 4.0;
    const float y = std::round(g.token(0, d).y1);
    if (y <= g.backY + 2 || y >= g.frontY - 1) continue;
    const float half = g.halfAt(y);
    for (float x = g.cx - half + 3; x < g.cx + half - 3; x += 6) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 2, y + 1), theme::withAlpha(P.text, 0.10f));
    const char* w = i18n::tr(kD[k]);
    const float tw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, w).x;
    const float lx = g.cx - half - 8 - tw;
    if (lx > v.body0.x + 4) dl->AddText(a.fontRegular, fs, ImVec2(std::round(lx), std::round(y - fs * 0.5f)), P.textFaint, w);
  }
  dl->AddLine(ImVec2(g.cx, g.backY + 3), ImVec2(g.cx, g.frontY + bulge), theme::withAlpha(P.text, 0.10f));
  // The pan scale over the back edge.
  if (g.backY - v.body0.y >= fs + 4) {
    for (int k = -2; k <= 2; ++k) {
      const double pan = k * 0.5;
      const float x = std::round(g.cx + static_cast<float>(pan) * g.backHalf);
      dl->AddRectFilled(ImVec2(x, g.backY - 4), ImVec2(x + 1, g.backY), edgeLit);
      const std::string t = panShort(pan);
      const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x;
      dl->AddText(mono, fs, ImVec2(std::round(x - tw * 0.5f), std::round(g.backY - 5 - fs)), k == 0 ? P.textDim : P.textFaint, t.c_str());
    }
  }
  if (v.roomOpen) {  // the space popover is open: the room's outline
    const float o = 8.0f * s;
    dl->AddRect(ImVec2(tl.x - o, tl.y - o), ImVec2(br.x + o, g.frontY + bulge + o), P.hot);
  }
}

// The listener: a square head facing the stage, its two cups, in the hard pixel style.
void trackerListener(ImDrawList* dl) {
  const auto& P = pal();
  const st::Geo& g = view().g;
  const trackerview::Aliased hard(dl);
  const float s = g.scale, r = std::round(11.0f * s);
  const ImVec2 c(std::round(g.cx), std::round(g.listenerY));
  dl->AddTriangleFilled(ImVec2(c.x, c.y - r - 6 * s), ImVec2(c.x - 5 * s, c.y - r), ImVec2(c.x + 5 * s, c.y - r), P.accent);
  dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), P.accent);
  trackerBevel(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
  const float cw = std::round(5 * s), ch = std::round(14 * s);
  for (float sx : {-1.0f, 1.0f}) {
    const float x = sx < 0 ? c.x - r - 1 - cw : c.x + r + 1;
    dl->AddRectFilled(ImVec2(x, c.y - ch * 0.5f), ImVec2(x + cw, c.y + ch * 0.5f), P.surface600);
    trackerBevel(dl, ImVec2(x, c.y - ch * 0.5f), ImVec2(x + cw, c.y + ch * 0.5f));
  }
}

// ---- the console column: Arrange, the selection's boxes, the groups
namespace {

float boxHeight(int rows) { return sp::kPad * 2 + rowH() + static_cast<float>(rows) * (rowH() + sp::kGap) - sp::kGap; }
float foldedHeight() { return sp::kPad * 2 + rowH() - 1; }
float groupRowH() { return std::round(fsz() + 4); }
constexpr int kGroupRowsWanted = 6;  // the Groups list keeps room for this many rows; boxes fold first

// The column's boxes as the user left them ("ui.stage.trackerFold": a(uto), o(pen) or f(olded) each).
enum Box { kArrange, kTrack, kPlace, kLevel, kBoxes };
char userFold(App& a, int box) {
  const std::string s = a.settings.getString("ui.stage.trackerFold");
  const char c = box < static_cast<int>(s.size()) ? s[static_cast<size_t>(box)] : 'a';
  return c == 'o' || c == 'f' ? c : 'a';
}
void setUserFold(App& a, int box, char c) {
  std::string s = a.settings.getString("ui.stage.trackerFold");
  s.resize(kBoxes, 'a');
  s[static_cast<size_t>(box)] = c;
  a.settings.set("ui.stage.trackerFold", s);
}

// The fold button at the right end of a box's title row ("-" open, "+" folded). True when clicked.
bool foldButton(App& a, int box, ImVec2 p0, ImVec2 p1, bool folded) {
  const float rh = rowH(), x1 = p1.x - sp::kPad, y = p0.y + sp::kPad;
  ImGui::PushID(box);
  const bool c = button(a, "##fold", folded ? "+" : "-", ImVec2(x1 - (rh - 1), y), ImVec2(x1, y + rh - 1), false,
                        folded ? "Show this box" : "Fold this box to its title (more room for the groups)");
  ImGui::PopID();
  (void)p1;
  return c;
}

// A folded box: its title and, in the monospaced face at the right, what it holds.
void foldedBox(App& a, ImVec2 p0, ImVec2 p1, const char* title, const std::string& summary) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, x1 = p1.x - sp::kPad - rh - sp::kGroup, ty = std::round(p0.y + sp::kPad + (rh - 1 - fs) * 0.5f);
  boxTitle(a, ImVec2(x0, ty), title);
  const float tw = (a.fontBold ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(title)).x;
  monoRight(a, dl, x0 + tw + sp::kGroup * 2, x1, ty, trackerview::valueInk(), summary);
}

const char* whoTitle(const std::vector<const Tok*>& sel) { return sel.empty() ? "Room" : sel.size() == 1 ? "Track" : "Tracks"; }

std::string summaryOf(App& a, int box, const std::vector<const Tok*>& sel) {
  const char* space = i18n::tr(mixops::roomTypeName(shownStage(a).roomType));
  char b[96];
  switch (box) {
    case kArrange:
      return space;
    case kTrack:
      if (sel.empty()) return trk::hex2(shownRoom(a) + 1) + " " + roomops::roomName(song(a), shownRoom(a));
      if (sel.size() == 1) return numbered(a, sel.front()->index);
      std::snprintf(b, sizeof b, "%s %s", trk::hex2(static_cast<int>(sel.size())).c_str(), i18n::tr("tracks"));
      return b;
    case kPlace:
      if (sel.empty()) return space;
      if (sel.size() == 1) {
        std::snprintf(b, sizeof b, "%s %s %.0f%%", panShort(sel.front()->pan).c_str(), i18n::tr(st::distanceWord(sel.front()->depth)),
                      st::widthPercent(sel.front()->sep));
        return b;
      }
      return same(sel, &Tok::depth) ? std::string(i18n::tr(st::distanceWord(sel.front()->depth))) : std::string(i18n::tr("Mixed"));
    default:
      if (sel.size() == 1) return levelText(sel.front()->volume);
      if (sel.empty())
        if (const MixerTrack* room = mixops::stageRoom(song(a), shownRoom(a))) return dbShort(room->volume);
      return "";
  }
}

const char* boxTitleOf(int box, const std::vector<const Tok*>& sel) {
  switch (box) {
    case kArrange: return "Arrange";
    case kTrack: return whoTitle(sel);
    case kPlace: return sel.empty() ? "Space" : "Place";
    default: return "Level";
  }
}

// The Arrange box: the stage's buttons in pairs.
void boxArrange(App& a, ImVec2 p0, ImVec2 p1) {
  View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, x1 = p1.x - sp::kPad, hw = std::floor((x1 - x0 - sp::kGap) * 0.5f);
  float by = p0.y + sp::kPad;
  boxTitle(a, ImVec2(x0, std::round(by + (rh - 1 - fs) * 0.5f)), "Arrange");
  by += rh + sp::kGap;
  auto pair = [&](const char* id0, const std::string& l0, bool on0, const char* h0, const char* id1, const std::string& l1, bool on1, const char* h1, int* hit) {
    *hit = 0;
    if (button(a, id0, l0.c_str(), ImVec2(x0, by), ImVec2(x0 + hw, by + rh - 1), on0, h0)) *hit = 1;
    if (button(a, id1, l1.c_str(), ImVec2(x0 + hw + sp::kGap, by), ImVec2(x1, by + rh - 1), on1, h1)) *hit = 2;
    if (std::string(id1) == "##tcspace") v.spaceAt = ImVec2(x0 + hw + sp::kGap, by + rh);
    by += rh + sp::kGap;
  };
  int hit = 0;
  pair("##tcpresets", std::string(i18n::tr("Presets")), ImGui::IsPopupOpen("##stagepresets"), "Stage presets, Spread out and Arrange by sound", "##tcspace",
       std::string(i18n::tr(mixops::roomTypeName(shownStage(a).roomType))), ImGui::IsPopupOpen("##stageroom"),
       "The space this room plays in: distance and focus are heard once a space is picked", &hit);
  if (hit == 1) openPresetsPopover();
  if (hit == 2) openRoomPopover();
  pair("##tcspread", i18n::tr("Spread out"), v.preview.kind == PreviewKind::Spread, "Space the tracks evenly from the centre outward, each keeping its distance",
       "##tcsound", i18n::tr("By sound"), v.preview.kind == PreviewKind::Sound, "Arrange by sound: place each track by what it is (shown first, nothing changes until Apply)",
       &hit);
  if (hit == 1) startPreview(a, PreviewKind::Spread);
  if (hit == 2) startPreview(a, PreviewKind::Sound);
  pair("##tcpick", std::string(i18n::tr("Playlist")), ImGui::IsPopupOpen("##stagepickmenu"),
       "Tracks from the playlist selection or a marker area: select them, add them to the stage, move them to a room or arrange them", "##tcrooms", i18n::tr("Rooms"),
       roomsPanelOpen(a), "The rooms column beside the stage: show, add, solo and manage the rooms", &hit);
  if (hit == 1) ImGui::OpenPopup("##stagepickmenu");
  if (hit == 2) setRoomsPanelOpen(a, !roomsPanelOpen(a));
  pair("##tcmenu", i18n::tr("Menu"), ImGui::IsPopupOpen("##stagetrkmenu"), "The stage's options, pan law, room extras and the mixer's options", "##tcrouting",
       i18n::tr("Routing"), a.mixerView.showRouting, "Routing overview: every route between the tracks", &hit);
  if (hit == 1) ImGui::OpenPopup("##stagetrkmenu");
  if (hit == 2) a.mixerView.showRouting = !a.mixerView.showRouting;
}

double gGroupScroll = 0;

// The groups: each spot that holds two or more tracks (a click selects its tracks).
void boxGroups(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rh = rowH(), fs = fsz(), lh = groupRowH();
  bevelBox(dl, p0, p1);
  const float x0 = p0.x + sp::kPad, x1 = p1.x - sp::kPad;
  float gy = p0.y + sp::kPad;
  boxTitle(a, ImVec2(x0, std::round(gy + (rh - 1 - fs) * 0.5f)), "Groups");
  monoRight(a, dl, x0, x1, std::round(gy + (rh - 1 - fs) * 0.5f), P.textDim, trk::hex2(static_cast<int>(v.groups.size())));
  gy += rh + sp::kGap;
  const ImVec2 w0(x0, gy), w1(x1, p1.y - sp::kPad);
  const int fit = std::max(0, static_cast<int>(std::floor((w1.y - w0.y - 4) / lh)));
  v.groupRowsFit = fit;
  const int n = static_cast<int>(v.groups.size());
  const bool bar = n > fit && fit >= 2;
  const float sbw = bar ? std::round(fs + 2) : 0.0f;
  bevelBox(dl, w0, ImVec2(w1.x - sbw, w1.y), true);
  gGroupScroll = std::clamp(gGroupScroll, 0.0, static_cast<double>(std::max(0, n - fit)));
  if (bar) {
    trackerview::vScrollBar(a, "##tgscroll", ImVec2(w1.x - sbw, w0.y), w1, &gGroupScroll, fit, n, 1, "Scroll the groups");
    if (ImGui::IsMouseHoveringRect(w0, w1) && ImGui::GetIO().MouseWheel != 0)
      gGroupScroll = std::clamp(gGroupScroll - ImGui::GetIO().MouseWheel, 0.0, static_cast<double>(n - fit));
  }
  const float r1 = w1.x - sbw - 1;
  dl->PushClipRect(ImVec2(w0.x + 1, w0.y + 1), ImVec2(r1, w1.y - 1), true);
  float ry = w0.y + 2;
  if (v.groups.empty()) dl->AddText(a.fontRegular, fs, ImVec2(w0.x + 6, ry + 2), P.textFaint, i18n::tr("No two tracks share a spot"));
  ImFont* mono = monoFont(a);
  for (size_t gi = static_cast<size_t>(std::lround(gGroupScroll)); gi < v.groups.size() && ry + lh <= w1.y; ++gi) {
    const Grp& gr = v.groups[gi];
    bool any = false, all = true;
    for (size_t k : gr.toks) any = any || v.toks[k].selected, all = all && v.toks[k].selected;
    ImGui::SetCursorScreenPos(ImVec2(w0.x + 1, ry));
    ImGui::PushID(static_cast<int>(gi));
    const bool click = ImGui::InvisibleButton("##grp", ImVec2(std::max(1.0f, r1 - w0.x - 1), lh));
    const bool hov = ImGui::IsItemHovered();
    ui::Hint("Click: select the tracks at this spot");
    ImGui::PopID();
    if (all) dl->AddRectFilled(ImVec2(w0.x + 1, ry), ImVec2(r1, ry + lh), theme::withAlpha(P.hot, 0.85f));
    else if (hov || any) dl->AddRectFilled(ImVec2(w0.x + 1, ry), ImVec2(r1, ry + lh), theme::withAlpha(P.surface600, 0.7f));
    const ImU32 ink = all ? theme::contrastText(P.hot) : P.text;
    const std::string num = trk::hex2(static_cast<int>(gi + 1));
    dl->AddText(mono, fs, ImVec2(w0.x + 6, ry + 2), all ? ink : P.textDim, num.c_str());
    const float nx = w0.x + 6 + mono->CalcTextSizeA(fs, FLT_MAX, 0, "00 ").x;
    const std::string where = panShort(gr.spot.pan) + " " + i18n::tr(st::distanceWord(gr.spot.depth));
    const float ww = mono->CalcTextSizeA(fs, FLT_MAX, 0, where.c_str()).x;
    const std::string name = v.toks[gr.toks.front()].name + " +" + std::to_string(gr.toks.size() - 1);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ry + 2), std::max(1.0f, r1 - 7 - ww - 8 - nx), ink, name.c_str());
    dl->AddText(mono, fs, ImVec2(std::round(r1 - 5 - ww), ry + 2), all ? ink : trackerview::valueInk(), where.c_str());
    if (click) {
      a.mixerView.sel.clear();
      for (size_t k : gr.toks) a.mixerView.sel.insert(v.toks[k].id);
      a.selMixer = v.toks[gr.toks.front()].id;
    }
    ry += lh;
  }
  dl->PopClipRect();
}

}  // namespace

// The console column: Arrange (the stage's buttons in pairs), the selection's boxes (Track, Place, Level;
// or the preview with Cancel and Apply), then the groups. On a short window the boxes fold to their
// title rows (Arrange first, then Level, Track, Place) until the groups have room for six rows; a box
// the user folded or opened with its button stays so ("ui.stage.trackerFold").
void trackerSide(App& a) {
  View& v = view();
  const ImVec2 p0 = v.side0, p1 = v.side1;
  v.folded = 0, v.groupRowsFit = 0, v.spaceAt = ImVec2(0, 0);
  if (p1.x <= p0.x) return;
  const std::vector<const Tok*> sel = chosen(a);
  const bool preview = v.preview.active();
  const float full[kBoxes] = {boxHeight(4), boxHeight(3), boxHeight(4), boxHeight(3)};
  const float groupsMin = sp::kPad * 2 + rowH() + sp::kGap + kGroupRowsWanted * groupRowH() + 4;
  bool fold[kBoxes];
  char user[kBoxes];
  for (int b = 0; b < kBoxes; ++b) user[b] = userFold(a, b), fold[b] = user[b] == 'f';
  auto total = [&]() {
    float h = 0;
    for (int b = 0; b < kBoxes; ++b) {
      if (preview && b != kArrange) continue;
      h += (fold[b] ? foldedHeight() : full[b]) + sp::kGap;
    }
    if (preview) h += boxHeight(6) + sp::kGap;
    return h + groupsMin;
  };
  bool autoFold[kBoxes] = {false, false, false, false};
  for (int b : {kArrange, kLevel, kTrack, kPlace}) {
    if (total() <= p1.y - p0.y) break;
    if (user[b] == 'a' && !(preview && b != kArrange)) fold[b] = autoFold[b] = true;
  }
  float y = p0.y;
  for (int b = 0; b < kBoxes; ++b) {
    if (preview && b == kTrack) {  // the preview takes the boxes' place
      const float ph = std::min(p1.y - y, boxHeight(6));
      if (ph > boxHeight(2)) boxPreview(a, ImVec2(p0.x, y), ImVec2(p1.x, y + ph), true);
      y += ph + sp::kGap;
    }
    if (preview && b != kArrange) continue;
    const float h = fold[b] ? foldedHeight() : full[b];
    if (y + h > p1.y) break;
    const ImVec2 b0(p0.x, y), b1(p1.x, y + h);
    if (fold[b]) {
      foldedBox(a, b0, b1, boxTitleOf(b, sel), summaryOf(a, b, sel));
      v.folded |= 1 << b;
    } else if (b == kArrange) {
      boxArrange(a, b0, b1);
    } else if (b == kTrack) {
      boxWho(a, b0, b1, sel);
    } else if (b == kPlace) {
      boxPlace(a, b0, b1, sel);
    } else {
      boxLevel(a, b0, b1, sel);
    }
    if (foldButton(a, b, b0, b1, fold[b])) {
      // Back to automatic when the click asks for what the column would do by itself.
      const bool want = !fold[b];
      setUserFold(a, b, want == autoFold[b] ? 'a' : want ? 'f' : 'o');
    }
    y += h + sp::kGap;
  }
  if (p1.y - y >= sp::kPad * 2 + rowH() + sp::kGap + 2 * groupRowH() + 4) boxGroups(a, ImVec2(p0.x, y), p1);
}

// ---- the popovers and the first-run card in the Tracker look

namespace {

// A tiny stage in a sunken box: where a preset puts a typical band (square marks, hard edges).
void presetDiagram(ImDrawList* dl, ImVec2 mn, ImVec2 mx, int preset) {
  const auto& P = pal();
  const View& v = view();
  const trackerview::Aliased hard(dl);
  dl->AddRectFilled(mn, mx, P.surface950);
  bevelBox(dl, mn, mx, true);
  const float w = mx.x - mn.x, h = mx.y - mn.y;
  const ImVec2 tl(mn.x + w * 0.27f, mn.y + 4), trr(mx.x - w * 0.27f, mn.y + 4), br(mx.x - 4, mx.y - 8), bl(mn.x + 4, mx.y - 8);
  dl->AddQuadFilled(tl, trr, br, bl, v.stage);
  dl->AddLine(tl, trr, theme::mix(P.surface700, P.text, 0.30f));
  dl->AddRectFilled(ImVec2(std::round(mn.x + w * 0.5f) - 2, mx.y - 5), ImVec2(std::round(mn.x + w * 0.5f) + 2, mx.y - 2), P.accent);
  static const st::Role band[] = {st::Role::LeadVocal, st::Role::Kick, st::Role::Bass, st::Role::Snare, st::Role::Hats,
                                  st::Role::Keys,      st::Role::Guitar, st::Role::Pad, st::Role::Fx};
  static const uint32_t cols[] = {0xC0607A, 0xC0603A, 0x7A60C0, 0xC08050, 0xB0A040, 0x50A080, 0xC09050, 0x5080C0, 0x8090A0};
  const std::vector<st::Role> roles(std::begin(band), std::end(band));
  const std::vector<st::Pos> at = st::arrangePreset(preset, roles);
  for (size_t k = 0; k < at.size(); ++k) {
    const float d = static_cast<float>(at[k].depth);
    const float y = (bl.y - 3) + (tl.y + 3 - (bl.y - 3)) * d;
    const float half = ((br.x - bl.x) * 0.5f - 5) + (((trr.x - tl.x) * 0.5f - 3) - ((br.x - bl.x) * 0.5f - 5)) * d;
    const float x = std::round(mn.x + w * 0.5f + static_cast<float>(at[k].pan) * half);
    const float s = d > 0.5f ? 3.0f : 4.0f;
    dl->AddRectFilled(ImVec2(x - std::floor(s * 0.5f), std::round(y) - std::floor(s * 0.5f)), ImVec2(x + std::ceil(s * 0.5f), std::round(y) + std::ceil(s * 0.5f)),
                      theme::content(cols[k]));
  }
  (void)h;
}

// One row of the presets popover: a picture, the name (lit: the first choice) and its hint under it.
bool menuRow(App& a, const char* id, const char* title, const char* hint, float w, const std::function<void(ImDrawList*, ImVec2, ImVec2)>& pic, bool lit) {
  const auto& P = pal();
  const float fs = fsz(), h = std::round(fs * 2 + 12), pw = std::round(fs * 4.2f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::withAlpha(P.surface600, 0.9f));
  pic(dl, ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + 3 + pw, p.y + h - 3));
  const float tx = p.x + pw + 10;
  dl->AddText(a.fontRegular, fs, ImVec2(tx, p.y + 5), lit ? P.hot : P.text, title);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(tx, p.y + 7 + fs), p.x + w - 4 - tx, P.textDim, hint);
  if (clicked) ImGui::CloseCurrentPopup();
  return clicked;
}

// A groove across a popover (between its parts): a dark line over a light one.
void groove(ImDrawList* dl, float x0, float x1, float y) {
  dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 1), theme::withAlpha(IM_COL32(0, 0, 0, 255), 0.45f));
  dl->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + 2), theme::withAlpha(pal().paper, 0.12f));
}

}  // namespace

void trackerPresets(App& a) {
  const auto& P = pal();
  View& v = view();
  const float W = std::round(fsz() * 27);
  auto glyph = [&P](ui::Icon ic) {
    return [&P, ic](ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
      dl->AddRectFilled(mn, mx, P.surface700);
      bevelBox(dl, mn, mx);
      ui::drawIcon(dl, ic, ImVec2(std::round((mn.x + mx.x) * 0.5f), std::round((mn.y + mx.y) * 0.5f)), 14, P.text);
    };
  };
  boxTitle(a, ImGui::GetCursorScreenPos(), "Presets");
  ImGui::Dummy(ImVec2(W, fsz() + 4));
  if (menuRow(a, "##parrange", i18n::tr("Arrange by sound..."), i18n::tr("Places each track by what it is and how it sounds"), W, glyph(ui::Icon::Star), true))
    startPreview(a, PreviewKind::Sound);
  if (menuRow(a, "##pspread", i18n::tr("Spread out"), i18n::tr("Evenly from the centre; each track keeps its distance"), W, glyph(ui::Icon::SwapArrows), false))
    startPreview(a, PreviewKind::Spread);
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    groove(ImGui::GetWindowDrawList(), p.x, p.x + W, p.y + 2);
    ImGui::Dummy(ImVec2(W, 6));
  }
  for (int k = 0; k < st::kPresetCount; ++k) {
    ImGui::PushID(k);
    const st::Preset& pr = st::preset(k);
    if (menuRow(a, "##preset", i18n::tr(pr.name), i18n::tr(pr.hint), W, [k](ImDrawList* dl, ImVec2 mn, ImVec2 mx) { presetDiagram(dl, mn, mx, k); }, false)) {
      v.preview = presetPreview(a, k);
      applyPreview(a);
    }
    if (ImGui::IsItemHovered()) {  // the stage shows where it would put them
      if (v.hoverPreview.preset != k || v.hoverPreview.kind != PreviewKind::Preset) v.hoverPreview = presetPreview(a, k);
      v.hoverPresetFrame = ImGui::GetFrameCount();
    }
    ImGui::PopID();
  }
}

void trackerRoomPopover(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const View& v = view();
  const MixerTrack* room = shownStage(a).hasRoom() ? mixops::stageRoom(s, shownRoom(a)) : nullptr;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = fsz(), rh = rowH(), W = std::round(fs * 21);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  float y = p.y;
  boxTitle(a, ImVec2(p.x, std::round(y + (rh - 1 - fs) * 0.5f)), "Space");
  y += rh + sp::kGap;
  std::vector<ObjectId> all;
  for (const Tok& t : v.toks) all.push_back(t.id);
  // The spaces in a sunken list: No space first, then the rooms; the hovered one's hint under it.
  const float lh = std::round(fs + 6);
  const float listH = lh * mixops::kRoomTypes + 4 + 4;
  bevelBox(dl, ImVec2(p.x, y), ImVec2(p.x + W, y + listH), true);
  float ry = y + 2;
  int hovered = -1;
  for (int k = 0; k < mixops::kRoomTypes; ++k) {
    if (k == 1) groove(dl, p.x + 2, p.x + W - 2, ry + 1), ry += 4;
    ImGui::PushID(k);
    const bool cur = shownStage(a).roomType == k;
    if (trackerview::radioRow(a, "##rt", i18n::tr(mixops::roomTypeName(k)), ImVec2(p.x + 2, ry), ImVec2(p.x + W - 2, ry + lh), cur))
      mixops::setRoomType(a.doc, k, all, shownRoom(a));
    if (ImGui::IsItemHovered()) hovered = k;
    ImGui::PopID();
    ry += lh;
  }
  y += listH + sp::kGap;
  {
    const int shown = hovered >= 0 ? hovered : shownStage(a).roomType;
    const char* h = i18n::tr(mixops::roomTypeHint(shown));
    dl->AddText(a.fontRegular, fs, ImVec2(p.x + 2, y + 2), P.textDim, h, nullptr, W - 4);
    y += a.fontRegular->CalcTextSizeA(fs, FLT_MAX, W - 4, h).y + sp::kGroup + 2;
  }
  groove(dl, p.x, p.x + W, y);
  y += sp::kGroup;
  // The space's level and how far back the stage feels.
  const float labelW = trackerview::labelColumn(a, {"Amount", "Back"});
  const float valueW = std::round(monoFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, "-24.0 dB").x + 12);
  if (room) {
    paramRow(a, "##troomamt", "Amount", mixerParam(room->id, MixerParam::Volume), false, dbShort(room->volume), ImVec2(p.x, y), W, labelW, valueW,
             "The space's level for the whole room");
  } else {
    dl->AddText(a.fontRegular, fs, ImVec2(p.x, std::round(y + (rh - 1 - fs) * 0.5f)), P.textFaint, i18n::tr("Amount"));
    readout(a, dl, ImVec2(p.x + labelW, y), ImVec2(p.x + W, y + rh - 1), i18n::tr("No space"), true);
  }
  y += rh + sp::kGap;
  dl->AddText(a.fontRegular, fs, ImVec2(p.x, std::round(y + (rh - 1 - fs) * 0.5f)), room ? P.textDim : P.textFaint, i18n::tr("Back"));
  static const char* strengths[] = {i18n::N_("Gentle"), i18n::N_("Natural"), i18n::N_("Strong")};
  static const char* hints[] = {"The back of the stage is 6 dB quieter", "The back of the stage is 9 dB quieter and duller",
                                "The back of the stage is 12 dB quieter and much duller"};
  const int si = std::clamp(static_cast<int>(std::lround(shownStage(a).depthStrength * 2.0)), 0, 2);
  const float bw = std::floor((W - labelW - 2 * sp::kGap) / 3.0f);
  if (!room) ImGui::BeginDisabled();
  for (int k = 0; k < 3; ++k) {
    ImGui::PushID(k);
    const float bx = p.x + labelW + static_cast<float>(k) * (bw + sp::kGap);
    if (button(a, "##str", i18n::tr(strengths[k]), ImVec2(bx, y), ImVec2(k == 2 ? p.x + W : bx + bw, y + rh - 1), si == k, hints[k]) && room)
      mixops::setDepthStrength(a.doc, k * 0.5, false, shownRoom(a));
    ImGui::PopID();
  }
  if (!room) ImGui::EndDisabled();
  y += rh + sp::kGroup;
  groove(dl, p.x, p.x + W, y);
  y += sp::kGroup;
  if (room) {
    if (button(a, "##openroom", i18n::tr("Open the space track"), ImVec2(p.x, y), ImVec2(p.x + W, y + rh - 1), false, "Its strip in the Mix page")) {
      selectOnly(a, room->id);
      setView(a, false);
      ImGui::CloseCurrentPopup();
    }
  } else {
    dl->AddText(a.fontRegular, fs, ImVec2(p.x, std::round(y + (rh - 1 - fs) * 0.5f)), P.textFaint, i18n::tr("Picking a space adds a space track"));
  }
  y += rh;
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(W, y - p.y));
}

void trackerFirstRun(App& a) {
  const auto& P = pal();
  View& v = view();
  const st::Geo& g = v.g;
  const float fs = fsz(), rh = rowH();
  const float cw = std::round(fs * 9.5f), ch = std::round(fs * 6), W = 3 * cw + 2 * sp::kGroup + 2 * sp::kGroup * 2;
  const float H = std::round(sp::kGroup * 2 + rh + sp::kGroup + ch + fs + 8 + sp::kGroup + rh + sp::kGroup * 2);
  const ImVec2 at(std::round(g.cx - W * 0.5f), std::round(g.backY + std::min(110.0f, std::max(10.0f, (g.frontY - g.backY) * 0.2f))));
  ImGui::SetCursorScreenPos(at);
  v.cardOpen = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::BeginChild("##stagecard", ImVec2(W, H), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const trackerview::Aliased hard(dl);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), P.surface800);
    bevelBox(dl, o, ImVec2(o.x + W, o.y + H));
    const float x0 = o.x + sp::kGroup * 2, x1 = o.x + W - sp::kGroup * 2;
    float y = o.y + sp::kGroup * 2;
    boxTitle(a, ImVec2(x0, std::round(y + (rh - 1 - fs) * 0.5f)), "The stage");
    if (button(a, "##cardclose", "x", ImVec2(x1 - (rh - 1), y), ImVec2(x1, y + rh - 1), false, "Close this card")) a.settings.set("ui.stage.welcomed", true);
    y += rh + sp::kGroup;
    static const char* caps[] = {i18n::N_("Left and right"), i18n::N_("Back is further away"), i18n::N_("Drag to move")};
    const ImU32 tile = theme::content(0xC08050), ar = P.textDim;
    for (int k = 0; k < 3; ++k) {
      const ImVec2 mn(x0 + static_cast<float>(k) * (cw + sp::kGroup), y), mx(mn.x + cw, y + ch);
      dl->AddRectFilled(mn, mx, P.surface950);
      bevelBox(dl, mn, mx, true);
      const float w = cw, h = ch, cx = std::round(mn.x + w * 0.5f);
      dl->AddQuadFilled(ImVec2(mn.x + w * 0.22f, mn.y + 6), ImVec2(mx.x - w * 0.22f, mn.y + 6), ImVec2(mx.x - 6, mx.y - 14), ImVec2(mn.x + 6, mx.y - 14), v.stage);
      dl->AddRectFilled(ImVec2(cx - 3, mx.y - 10), ImVec2(cx + 3, mx.y - 4), P.accent);
      const float t = std::round(h * 0.18f), my = std::round(mn.y + h * 0.42f);
      if (k == 0) {
        dl->AddRectFilled(ImVec2(cx - t * 0.5f, my - t * 0.5f), ImVec2(cx + t * 0.5f, my + t * 0.5f), tile);
        for (float sx : {-1.0f, 1.0f}) {
          const float ax = cx + sx * (t * 0.5f + 6), bx = cx + sx * (w * 0.36f);
          dl->AddLine(ImVec2(ax, my), ImVec2(bx, my), ar);
          dl->AddTriangleFilled(ImVec2(bx + sx * 3, my), ImVec2(bx - sx * 3, my - 4), ImVec2(bx - sx * 3, my + 4), ar);
        }
      } else if (k == 1) {
        dl->AddRectFilled(ImVec2(cx - w * 0.2f - t * 0.5f, my), ImVec2(cx - w * 0.2f + t * 0.5f, my + t), tile);
        dl->AddRectFilled(ImVec2(cx + w * 0.15f, mn.y + 10), ImVec2(cx + w * 0.15f + t * 0.6f, mn.y + 10 + t * 0.6f), tile);
        dl->AddLine(ImVec2(cx - w * 0.08f, my - 2), ImVec2(cx + w * 0.1f, mn.y + 14 + t * 0.3f), ar);
      } else {
        const ImVec2 g0(cx - w * 0.28f, my - t * 0.5f), g1(g0.x + t, g0.y + t);
        for (float x = g0.x; x < g1.x; x += 3) dl->AddRectFilled(ImVec2(x, g0.y), ImVec2(x + 1, g0.y + 1), ar), dl->AddRectFilled(ImVec2(x, g1.y - 1), ImVec2(x + 1, g1.y), ar);
        for (float yy = g0.y; yy < g1.y; yy += 3) dl->AddRectFilled(ImVec2(g0.x, yy), ImVec2(g0.x + 1, yy + 1), ar), dl->AddRectFilled(ImVec2(g1.x - 1, yy), ImVec2(g1.x, yy + 1), ar);
        dl->AddLine(ImVec2(g1.x + 4, my), ImVec2(cx + w * 0.1f, my), ar);
        dl->AddTriangleFilled(ImVec2(cx + w * 0.1f + 4, my), ImVec2(cx + w * 0.1f - 2, my - 4), ImVec2(cx + w * 0.1f - 2, my + 4), ar);
        dl->AddRectFilled(ImVec2(cx + w * 0.14f, my - t * 0.5f), ImVec2(cx + w * 0.14f + t, my + t * 0.5f), tile);
      }
      const char* cap = i18n::tr(caps[k]);
      const float tw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, cap).x;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(std::round(mn.x + std::max(0.0f, (cw - tw) * 0.5f)), mx.y + 4), cw, P.textDim, cap);
    }
    y += ch + fs + 8 + sp::kGroup;
    const float b1w = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Arrange for me")).x) + 24;
    const float b2w = std::ceil(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Place them myself")).x) + 24;
    if (button(a, "##cardarrange", "Arrange for me", ImVec2(x0, y), ImVec2(x0 + b1w, y + rh - 1), true, "Place each track by what it is (shown first: Apply or Cancel)")) {
      a.settings.set("ui.stage.welcomed", true);
      startPreview(a, PreviewKind::Sound);
    }
    if (button(a, "##cardmyself", "Place them myself", ImVec2(x0 + b1w + sp::kGap, y), ImVec2(x0 + b1w + sp::kGap + b2w, y + rh - 1), false,
               "Close this card and drag the tracks yourself"))
      a.settings.set("ui.stage.welcomed", true);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// A footprint: a flattened diamond on the floor (as wide as the track sounds, as deep as the floor's
// perspective there), in hard pixel edges.
void trackerFoot(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col) {
  const trackerview::Aliased hard(dl);
  c = ImVec2(std::round(c.x), std::round(c.y));
  rx = std::round(rx), ry = std::max(2.0f, std::round(ry));
  dl->AddQuadFilled(ImVec2(c.x - rx, c.y), ImVec2(c.x, c.y - ry), ImVec2(c.x + rx, c.y), ImVec2(c.x, c.y + ry), col);
}

// The same diamond's outline, dashed (where a dragged track will stand).
void trackerFootDashed(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col) {
  const trackerview::Aliased hard(dl);
  const ImVec2 pts[5] = {ImVec2(c.x - rx, c.y), ImVec2(c.x, c.y - ry), ImVec2(c.x + rx, c.y), ImVec2(c.x, c.y + ry), ImVec2(c.x - rx, c.y)};
  for (int e = 0; e < 4; ++e) {
    const ImVec2 a0 = pts[e], a1 = pts[e + 1];
    const float len = std::hypot(a1.x - a0.x, a1.y - a0.y);
    for (float t = 0; t < len; t += 7) {
      const float t1 = std::min(len, t + 4);
      dl->AddLine(ImVec2(a0.x + (a1.x - a0.x) * t / len, a0.y + (a1.y - a0.y) * t / len), ImVec2(a0.x + (a1.x - a0.x) * t1 / len, a0.y + (a1.y - a0.y) * t1 / len), col);
    }
  }
}

// A width handle: a small raised square (half size `r`) with a ring of the floor round it.
void trackerHandle(ImDrawList* dl, ImVec2 c, float r, ImU32 col, ImU32 ring) {
  const trackerview::Aliased hard(dl);
  c = ImVec2(std::round(c.x), std::round(c.y));
  r = std::max(2.0f, std::round(r * 0.85f));
  dl->AddRectFilled(ImVec2(c.x - r - 2, c.y - r - 2), ImVec2(c.x + r + 2, c.y + r + 2), ring);
  dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col);
  trackerBevel(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
}

}  // namespace gl::stageui::detail
