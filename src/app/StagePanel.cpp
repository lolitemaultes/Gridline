// Gridline - the Stage's control panel (one track, several, or the whole stage), Spread out and Arrange by
// sound with their preview, the presets and room popovers, the effects popover and the first-run card
// (docs/stage-view.md). The stage itself is app/StageView.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/MixerOps.h"
#include "../core/StageArrange.h"
#include "../core/StageMap.h"
#include "../engine/Program.h"
#include "../engine/Scope.h"
#include "App.h"
#include "MixerMeters.h"
#include "StageView.h"
#include "StageViewInternal.h"
#include "../core/StageRoomOps.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::stageui::detail {

using theme::pal;

// ---------------------------------------------------------------- words
std::string panText(double pan) {
  const int pc = static_cast<int>(std::lround(std::fabs(pan) * 100.0));
  if (pc == 0) return i18n::tr("Centre");
  char b[48];
  std::snprintf(b, sizeof b, pan < 0 ? i18n::tr("%d%% left") : i18n::tr("%d%% right"), pc);
  return b;
}

std::string dbText(double gain, bool sign) {
  if (gain <= 1e-5) return "-inf dB";
  const double db = 20.0 * std::log10(gain);
  char b[32];
  std::snprintf(b, sizeof b, sign ? "%+.1f dB" : "%.0f dB", std::fabs(db) < 0.05 ? 0.0 : db);
  return b;
}

std::string levelText(double volume) {
  const double db = st::levelDb(volume);
  if (!std::isfinite(db)) return "-inf dB";
  char b[32];
  std::snprintf(b, sizeof b, "%.1f dB", std::fabs(db) < 0.05 ? 0.0 : db);
  return b;
}

// ---------------------------------------------------------------- roles
namespace {

void playsOn(const Song& s, const MixerTrack& t, std::vector<std::string>& types, std::vector<std::string>& words) {
  for (const auto& c : s.channels) {
    if (c.mixerTrack != t.id) continue;
    types.push_back(c.plugin.type);
    words.push_back(c.name);
    if (!c.plugin.data.empty()) {  // a sample's file name says a lot ("kick_04.wav")
      const size_t slash = c.plugin.data.find_last_of("/\\");
      words.push_back(slash == std::string::npos ? c.plugin.data : c.plugin.data.substr(slash + 1));
    }
  }
}

struct Heard {
  st::FeatureAcc acc;
  size_t last = 0;
  bool primed = false;
};
std::map<ObjectId, Heard> gHeard;
double widthPercentOf(const MixerTrack& t) { return st::widthPercent(t.stereoSep) / 200.0; }
std::vector<float> gBuf;

}  // namespace

st::Guess guessOf(App& a, size_t index) {
  const Song& s = a.doc.song();
  if (index >= s.mixer.size()) return {};
  const MixerTrack& t = s.mixer[index];
  std::vector<std::string> types, words;
  playsOn(s, t, types, words);
  st::Features f;
  bool heard = false;
  if (auto it = gHeard.find(t.id); it != gHeard.end()) {
    f = it->second.acc.result();
    f.width = std::clamp(widthPercentOf(t), 0.0, 1.0);
    heard = f.enough();
  }
  return st::guessRole(types, words, heard ? &f : nullptr, t.name);
}

int autoIcon(App& a, size_t index) {
  const Song& s = a.doc.song();
  if (index >= s.mixer.size()) return 0;
  const MixerTrack& t = s.mixer[index];
  std::vector<std::string> types, words;
  playsOn(s, t, types, words);
  st::Role r = st::roleFromPlays(types, words);
  if (r == st::Role::Unknown) r = st::roleFromName(t.name);
  return st::roleIcon(r);
}

void feedAnalysis(App& a) {
  if (!a.engine.playing()) return;
  View& v = view();
  if (gBuf.size() < ScopeTap::kSize) gBuf.resize(ScopeTap::kSize);
  for (const Tok& t : v.toks) {
    const auto rt = a.builder.mixerRuntime(t.id);
    if (!rt) continue;
    Heard& h = gHeard[t.id];
    const size_t w = rt->tap.written();
    if (!h.primed || w < h.last) {
      h.acc.reset(a.engine.sampleRate());
      h.last = w;
      h.primed = true;
      continue;
    }
    const size_t n = std::min(w - h.last, ScopeTap::kSize);
    h.last = w;
    if (n == 0 || !rt->tap.read(gBuf.data(), n)) continue;
    h.acc.add(gBuf.data(), static_cast<int>(n));
  }
}

// ---------------------------------------------------------------- arranging
namespace {

std::vector<ObjectId> arrangeScope(App& a) {
  const View& v = view();
  std::vector<ObjectId> sel;
  for (ObjectId id : selection(a))
    if (tokOf(id)) sel.push_back(id);
  std::vector<ObjectId> out;
  for (const Tok& t : v.toks)  // mixer order
    if (sel.size() < 2 || std::find(sel.begin(), sel.end(), t.id) != sel.end()) out.push_back(t.id);
  return out;
}

std::vector<st::Role> rolesOf(App& a, const std::vector<ObjectId>& ids, std::vector<st::Guess>* why) {
  std::vector<st::Role> roles;
  for (ObjectId id : ids) {
    const Tok* t = tokOf(id);
    const st::Guess g = t ? guessOf(a, t->index) : st::Guess{};
    roles.push_back(g.role);
    if (why) why->push_back(g);
  }
  return roles;
}

}  // namespace

Preview presetPreview(App& a, int preset) {
  Preview p;
  p.kind = PreviewKind::Preset;
  p.preset = preset;
  p.ids = arrangeScope(a);
  const std::vector<st::Role> roles = rolesOf(a, p.ids, nullptr);
  const std::vector<st::Pos> to = st::arrangePreset(preset, roles);
  for (size_t k = 0; k < p.ids.size(); ++k) p.to.push_back({p.ids[k], to[k].pan, to[k].depth});
  return p;
}

void startPreview(App& a, PreviewKind kind, int preset) {
  View& v = view();
  v.hoverPreview = Preview{};
  if (kind == PreviewKind::Preset) {
    v.preview = presetPreview(a, preset);
    return;
  }
  Preview p;
  p.kind = kind;
  p.ids = arrangeScope(a);
  if (p.ids.empty()) return;
  if (kind == PreviewKind::Spread) {
    std::vector<st::Pos> pts;
    for (ObjectId id : p.ids)
      if (const MixerTrack* t = a.doc.song().mixerTrack(id)) pts.push_back({id, t->pan, t->depth});
    const st::Geo& g = v.g;
    const double minSpacing = static_cast<double>(st::kTokenFront * g.scale * g.tokenScale + st::kGap * g.scale) / std::max(1.0, static_cast<double>(g.reach(0.5)));
    p.to = st::spreadOut(pts, minSpacing);
  } else {
    const std::vector<st::Role> roles = rolesOf(a, p.ids, &p.why);
    const std::vector<st::Pos> to = st::arrangeByRole(roles);
    for (size_t k = 0; k < p.ids.size(); ++k) p.to.push_back({p.ids[k], to[k].pan, to[k].depth});
    for (const st::Guess& g : p.why) p.unsure += g.source == 0 ? 1 : 0;
  }
  v.preview = std::move(p);
}

void cancelPreview() { view().preview = Preview{}; }

void applyPreview(App& a) {
  View& v = view();
  const Preview p = v.preview;
  v.preview = Preview{};
  if (!p.active()) return;
  const char* label = p.kind == PreviewKind::Spread ? "spread out" : p.kind == PreviewKind::Sound ? "arrange by sound" : "stage preset";
  a.doc.endGesture();
  a.doc.beginGroup(label);
  for (size_t k = 0; k < p.ids.size() && k < p.to.size(); ++k) {
    const MixerTrack* t = a.doc.song().mixerTrack(p.ids[k]);
    if (!t || t->locked) continue;
    if (std::fabs(t->pan - p.to[k].pan) > 1e-9) a.doc.setParam(mixerParam(p.ids[k], MixerParam::Pan), p.to[k].pan, false);
    if (std::fabs(t->depth - p.to[k].depth) > 1e-9) a.doc.setParam(mixerParam(p.ids[k], MixerParam::Depth), p.to[k].depth, false);
  }
  if (p.kind == PreviewKind::Preset) {
    const st::Preset& pr = st::preset(p.preset);
    mixops::setRoomType(a.doc, pr.roomType, p.ids, shownRoom(a));
    mixops::addRoomSends(a.doc, p.ids, shownRoom(a));
    mixops::setDepthStrength(a.doc, pr.strength, false, shownRoom(a));
  }
  a.doc.endGroup();
  a.doc.endGesture();
  if (p.kind == PreviewKind::Preset) a.notify(i18n::tr("Placed the tracks and set the space. Undo takes it all back."));
}

void openEffects(App& a, ObjectId id) {
  (void)a;
  View& v = view();
  v.fxTrack = id;
  v.wantFx = true;
}

// ---------------------------------------------------------------- panel pieces
namespace {

constexpr float kKnobR = 19.0f;
constexpr float kIdentityW = 252.0f;

ParamDesc lin(const char* name, double lo, double hi, double def) {
  ParamDesc d;
  d.name = name;
  d.min = lo, d.max = hi, d.def = def;
  return d;
}

void centredText(ImDrawList* dl, ImFont* f, float fs, float cx, float y, ImU32 col, const std::string& t, float maxW) {
  const float w = f->CalcTextSizeA(fs, 1e9f, 0, t.c_str()).x;
  if (w <= maxW) dl->AddText(f, fs, ImVec2(std::round(cx - w * 0.5f), std::round(y)), col, t.c_str());
  else theme::textEllipsis(dl, f, fs, ImVec2(std::round(cx - maxW * 0.5f), std::round(y)), maxW, col, t.c_str());
}

// One knob cell: the caption, the control (drawn by `control` at the knob's top left), the value in
// words and a dim line under it.
void cell(App& a, float cx, float y0, float w, const char* caption, const std::string& value, const std::string& sub,
          const std::function<void(ImVec2)>& control) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  centredText(dl, a.fontRegular, theme::kFontCaption, cx, y0 + 14, P.textDim, caption, w - 8);
  control(ImVec2(std::round(cx - kKnobR), std::round(y0 + 57 - kKnobR)));
  centredText(dl, a.fontBold, 13.0f, cx, y0 + 87, P.text, value, w - 8);
  centredText(dl, a.fontRegular, theme::kFontLabel, cx, y0 + 107, P.textFaint, sub, w - 8);
}

void paramKnob(const char* id, ParamId pid, bool bipolar, ImVec2 at) {
  ImGui::SetCursorScreenPos(at);
  ui::ParamKnob(id, pid, kKnobR, nullptr, bipolar);
}

void meter(App& a, ObjectId id, ImVec2 at, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const mixmeters::Reading r = mixmeters::get(a, id);
  const float pk[2] = {r.peakL, r.peakR};
  for (int k = 0; k < 2; ++k) {
    const ImVec2 mn(at.x + static_cast<float>(k) * 6.0f, at.y), mx(mn.x + 4, at.y + h);
    dl->AddRectFilled(mn, mx, P.well, 1);
    const float db = pk[k] > 1e-5f ? 20.0f * std::log10(pk[k]) : -90.0f;
    const float n = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
    if (n > 0) dl->AddRectFilled(ImVec2(mn.x, mx.y - h * n), mx, db > -1.0f ? P.meterHigh : db > -12.0f ? P.meterMid : P.meterLow, 1);
  }
}

bool smallButton(App& a, const char* id, const char* label, bool on, ImU32 onCol, const char* hint, ui::Icon icon = ui::Icon::Count) {
  const auto& P = pal();
  const float h = 24, fs = theme::kFontCaption;
  const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, label).x;
  const bool hasIcon = icon != ui::Icon::Count;
  const float w = tw + 20 + (hasIcon ? 16 : 0);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = hov ? P.surface600 : P.surface700, fg = P.text;
  if (on) bg = theme::mix(P.surface700, onCol, hov ? 0.30f : 0.22f), fg = onCol;
  if (hasIcon && !on) bg = theme::mix(P.surface700, P.hot, hov ? 0.16f : 0.10f), fg = P.hot;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, theme::kRadius);
  float x = p.x + 10;
  if (hasIcon) {
    ui::drawIcon(dl, icon, ImVec2(x + 5, p.y + h * 0.5f), 11, fg);
    x += 16;
  }
  dl->AddText(a.fontRegular, fs, ImVec2(x, std::round(p.y + (h - fs) * 0.5f - 1)), fg, label);
  if (hint) ui::Hint(hint);
  return ImGui::IsItemClicked();
}

void identityTile(App& a, ImDrawList* dl, const Tok* t, ImVec2 at, float side) {
  const auto& P = pal();
  if (t) {
    Tok f = *t;
    f.depth = 0, f.dim = false, f.muted = false;
    drawFace(a, dl, f, at, side, 1.0f);
  } else {
    dl->AddRectFilled(at, ImVec2(at.x + side, at.y + side), P.surface700, side * 0.25f);
    ui::drawIcon(dl, ui::Icon::Stage, ImVec2(at.x + side * 0.5f, at.y + side * 0.5f), 16, P.hot);
  }
}

// Relative edits of several tracks (a knob turned from 0: each track moves from where it was).
struct Rel {
  bool on = false;
  std::vector<ObjectId> ids;
  std::vector<double> start;
  double v = 0;
};
Rel gRel[6];

bool allEqual(const std::vector<double>& x) {
  for (double y : x)
    if (std::fabs(y - x.front()) > 1e-4) return false;
  return true;
}

void relKnob(App& a, int k, const std::vector<ObjectId>& ids, const ParamDesc& d, bool bipolar, ImVec2 at,
             const std::function<double(ObjectId)>& read, const std::function<void(ObjectId, double start, double v)>& write) {
  Rel& r = gRel[k];
  if (!r.on) r.v = d.def;
  ImGui::SetCursorScreenPos(at);
  bool released = false;
  char id[16];
  std::snprintf(id, sizeof id, "##rel%d", k);
  double v = r.v;
  const bool ch = ui::Knob(id, &v, d, kKnobR, nullptr, bipolar, &released);
  if (ch) {
    if (!r.on) {
      r.on = true;
      r.ids = ids;
      r.start.clear();
      for (ObjectId x : ids) r.start.push_back(read(x));
    }
    r.v = v;
    for (size_t i = 0; i < r.ids.size(); ++i) write(r.ids[i], r.start[i], v);
  }
  if (released) {
    r.on = false;
    r.v = d.def;
    a.doc.endGesture();
  }
}

ObjectId roomIndexOf(App& a, const Song& s, int* index) {
  const MixerTrack* room = mixops::stageRoom(s, shownRoom(a));
  if (!room) return kNoId;
  if (index) *index = s.mixerIndex(room->id);
  return room->id;
}

bool hasRoomSend(const MixerTrack& t, ObjectId room) {
  for (const auto& sd : t.sends)
    if (sd.to == room && sd.stageRoom) return true;
  return false;
}

// A narrow mixer: the panel keeps who it is about and its buttons; the knobs need the full width
// (the same settings are on the mixer's strips).
bool compactPanel(ImVec2 p0, ImVec2 p1) { return p1.x - p0.x < kIdentityW + 6 * 70.0f; }

// ---- one track
void panelOne(App& a, const Tok& t, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const MixerTrack& m = s.mixer[t.index];
  ImDrawList* dl = ImGui::GetWindowDrawList();
  identityTile(a, dl, &t, ImVec2(p0.x + 16, p0.y + 14), 36);
  theme::textEllipsis(dl, a.fontBold, 14.0f, ImVec2(p0.x + 62, p0.y + 16), kIdentityW - 70, P.text, t.name.c_str());
  int fx = 0;
  for (const auto& sl : m.slots) fx += sl.plugin.type.empty() ? 0 : 1;
  char b[96];
  std::snprintf(b, sizeof b, fx == 1 ? i18n::tr("Insert %zu \xC2\xB7 1 effect") : i18n::tr("Insert %zu \xC2\xB7 %d effects"), t.index, fx);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(p0.x + 62, p0.y + 35), kIdentityW - 70, P.textDim, b);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p1.y - 14 - 24));
  if (smallButton(a, "##pmute", i18n::tr("Mute"), m.muted, P.muteLamp, "Mute this track")) a.doc.setParam(mixerParam(m.id, MixerParam::Mute), m.muted ? 0.0 : 1.0, false);
  ImGui::SameLine(0, 6);
  if (smallButton(a, "##psolo", i18n::tr("Solo"), m.solo, P.solo, "Hear only this track (and what feeds it)")) {
    const bool on = !m.solo;
    a.doc.editMixerTrack(m.id, "solo mixer track", [on](MixerTrack& x) { x.solo = on; });
  }
  ImGui::SameLine(0, 6);
  if (smallButton(a, "##pfx", i18n::tr("Effects"), false, P.hot, "This track's effects", ui::Icon::Star)) openEffects(a, m.id);
  if (compactPanel(p0, p1)) return;
  // The cells.
  const float x0 = p0.x + kIdentityW + 16, x1 = p1.x - 16, w = (x1 - x0) / 6.0f;
  auto cx = [&](int k) { return x0 + w * (static_cast<float>(k) + 0.5f); };
  // While it is dragged: where the drop puts it.
  const bool dragged = view().dragId == t.id;
  const double pan = dragged ? view().dragPan : t.pan, depth = dragged ? view().dragDepth : t.depth;
  cell(a, cx(0), p0.y, w, i18n::tr("Left / right"), panText(pan), "", [&](ImVec2 at) { paramKnob("##ppan", mixerParam(m.id, MixerParam::Pan), true, at); });
  const bool heard = shownStage(a).hasRoom();  // with no room, distance and focus are kept but not heard
  cell(a, cx(1), p0.y, w, i18n::tr("Distance"), i18n::tr(st::distanceWord(depth)), heard ? i18n::tr(st::distanceHint(depth)) : i18n::tr("not heard: no space"),
       [&](ImVec2 at) { paramKnob("##pdepth", mixerParam(m.id, MixerParam::Depth), false, at); });
  std::snprintf(b, sizeof b, "%.0f%%", st::widthPercent(t.sep));
  cell(a, cx(2), p0.y, w, i18n::tr("Width"), i18n::tr(st::widthWord(t.sep)), b, [&](ImVec2 at) { paramKnob("##psep", mixerParam(m.id, MixerParam::StereoSep), true, at); });
  cell(a, cx(3), p0.y, w, i18n::tr("Focus"), i18n::tr(st::focusWord(t.focus)), heard ? i18n::tr(st::focusHint(t.focus)) : i18n::tr("not heard: no space"),
       [&](ImVec2 at) { paramKnob("##pfocus", mixerParam(m.id, MixerParam::Focus), true, at); });
  int roomIdx = -1;
  const ObjectId room = roomIndexOf(a, s, &roomIdx);
  const bool sends = room != kNoId && hasRoomSend(m, room);
  cell(a, cx(4), p0.y, w, i18n::tr("Space"), sends ? dbText(mixops::sendGain(s, m.id, room), false) : std::string(i18n::tr("Off")),
       sends ? i18n::tr("send to the space") : i18n::tr("no space send"), [&](ImVec2 at) {
         if (sends) {
           paramKnob("##proom", sendParam(m.id, roomIdx), false, at);
           return;
         }
         ImGui::SetCursorScreenPos(ImVec2(at.x - 8, at.y + 7));
         if (smallButton(a, "##paddroom", i18n::tr("Add"), false, P.hot, "Send this track into the room (the Room track is made the first time)")) {
           if (room == kNoId) mixops::ensureStageRoom(a.doc, {m.id}, "add the room", shownRoom(a));
           else {
             a.doc.beginGroup("send to the room");
             mixops::addRoomSends(a.doc, {m.id}, shownRoom(a));
             a.doc.endGroup();
           }
         }
       });
  cell(a, cx(5), p0.y, w, i18n::tr("Level"), levelText(t.volume), "", [&](ImVec2 at) {
    paramKnob("##pvol", mixerParam(m.id, MixerParam::Volume), false, at);
    meter(a, m.id, ImVec2(at.x + kKnobR * 2 + 10, at.y - 2), kKnobR * 2 + 4);
  });
}

// ---- several tracks
void panelSeveral(App& a, const std::vector<const Tok*>& toks, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (size_t k = std::min<size_t>(3, toks.size()); k-- > 0;) identityTile(a, dl, toks[k], ImVec2(p0.x + 16 + 13.0f * static_cast<float>(k), p0.y + 16), 28);
  const float tx = p0.x + 16 + 13.0f * static_cast<float>(std::min<size_t>(3, toks.size()) - 1) + 36;
  char b[64];
  std::snprintf(b, sizeof b, i18n::tr("%zu tracks"), toks.size());
  dl->AddText(a.fontBold, 14.0f, ImVec2(tx, p0.y + 14), P.text, b);
  std::string names;
  for (const Tok* t : toks) names += (names.empty() ? "" : ", ") + t->name;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(tx, p0.y + 33), p0.x + kIdentityW - 8 - tx, P.textDim, names.c_str());
  std::vector<ObjectId> ids;
  for (const Tok* t : toks) ids.push_back(t->id);
  bool allMuted = true, allSolo = true;
  for (const Tok* t : toks) allMuted = allMuted && t->muted, allSolo = allSolo && t->solo;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p1.y - 14 - 24));
  if (smallButton(a, "##smute", i18n::tr("Mute"), allMuted, P.muteLamp, "Mute or unmute all of them")) {
    a.doc.beginGroup(allMuted ? "unmute mixer tracks" : "mute mixer tracks");
    for (ObjectId id : ids) a.doc.setParam(mixerParam(id, MixerParam::Mute), allMuted ? 0.0 : 1.0, false);
    a.doc.endGroup();
  }
  ImGui::SameLine(0, 6);
  if (smallButton(a, "##ssolo", i18n::tr("Solo"), allSolo, P.solo, "Solo all of them")) {
    a.doc.beginGroup("solo mixer track");
    for (ObjectId id : ids) a.doc.editMixerTrack(id, "solo mixer track", [&](MixerTrack& x) { x.solo = !allSolo; });
    a.doc.endGroup();
  }
  ImGui::SameLine(0, 6);
  if (smallButton(a, "##sspread", i18n::tr("Spread out"), false, P.hot, "Space only these tracks evenly from the centre")) startPreview(a, PreviewKind::Spread);
  if (compactPanel(p0, p1)) return;
  const float x0 = p0.x + kIdentityW + 16, x1 = p1.x - 16, w = (x1 - x0) / 6.0f;
  auto cx = [&](int k) { return x0 + w * (static_cast<float>(k) + 0.5f); };
  auto get = [&](MixerParam p) {
    std::vector<double> v;
    for (ObjectId id : ids)
      if (const auto x = getParam(s, mixerParam(id, p))) v.push_back(*x);
    return v;
  };
  auto setP = [&](ObjectId id, MixerParam p, double v) {
    const auto cur = getParam(a.doc.song(), mixerParam(id, p));
    if (cur && std::fabs(*cur - v) > 1e-9) a.doc.setParam(mixerParam(id, p), v, true);
  };
  auto readP = [&](MixerParam p) { return [&a, p](ObjectId id) { return getParam(a.doc.song(), mixerParam(id, p)).value_or(0.0); }; };
  char vb[48];
  // Left / right and distance: the group moves together, keeping its spacing.
  std::snprintf(vb, sizeof vb, "%+.0f%%", gRel[0].v * 100.0);
  cell(a, cx(0), p0.y, w, i18n::tr("Left / right"), gRel[0].on ? std::string(vb) : std::string(i18n::tr("Together")), i18n::tr("keeps the spacing"), [&](ImVec2 at) {
    relKnob(a, 0, ids, lin("Left / right", -1, 1, 0), true, at, readP(MixerParam::Pan),
            [&](ObjectId id, double st0, double v) { setP(id, MixerParam::Pan, std::clamp(st0 + v, -1.0, 1.0)); });
  });
  cell(a, cx(1), p0.y, w, i18n::tr("Distance"), gRel[1].on ? std::string(i18n::tr(st::distanceWord(std::clamp(0.5 + gRel[1].v, 0.0, 1.0)))) : std::string(i18n::tr("Together")),
       i18n::tr("each from its place"), [&](ImVec2 at) {
         relKnob(a, 1, ids, lin("Distance", -1, 1, 0), true, at, readP(MixerParam::Depth),
                 [&](ObjectId id, double st0, double v) { setP(id, MixerParam::Depth, std::clamp(st0 + v, 0.0, 1.0)); });
       });
  const std::vector<double> seps = get(MixerParam::StereoSep), focs = get(MixerParam::Focus);
  std::snprintf(vb, sizeof vb, "x%.2f", gRel[2].v);
  cell(a, cx(2), p0.y, w, i18n::tr("Width"), gRel[2].on ? std::string(vb) : allEqual(seps) ? std::string(i18n::tr(st::widthWord(seps.front()))) : std::string(i18n::tr("Mixed")),
       i18n::tr("scales each one"), [&](ImVec2 at) {
         relKnob(a, 2, ids, lin("Width", 0, 2, 1), false, at, readP(MixerParam::StereoSep), [&](ObjectId id, double st0, double v) {
           setP(id, MixerParam::StereoSep, std::clamp(st::widthPercent(st0) * v / 100.0 - 1.0, -1.0, 1.0));
         });
       });
  cell(a, cx(3), p0.y, w, i18n::tr("Focus"), allEqual(focs) ? std::string(i18n::tr(st::focusWord(focs.front()))) : std::string(i18n::tr("Mixed")), "", [&](ImVec2 at) {
    relKnob(a, 3, ids, lin("Focus", -2, 2, 0), true, at, readP(MixerParam::Focus),
            [&](ObjectId id, double st0, double v) { setP(id, MixerParam::Focus, std::clamp(st0 + v, -1.0, 1.0)); });
  });
  int roomIdx = -1;
  const ObjectId room = roomIndexOf(a, s, &roomIdx);
  std::vector<double> sendG;
  for (ObjectId id : ids) sendG.push_back(room != kNoId ? mixops::sendGain(s, id, room) : 0.0);
  std::snprintf(vb, sizeof vb, "%+.1f dB", gRel[4].v);
  cell(a, cx(4), p0.y, w, i18n::tr("Space"), gRel[4].on ? std::string(vb) : room == kNoId ? std::string(i18n::tr("Off")) : allEqual(sendG) ? dbText(sendG.front(), false) : std::string(i18n::tr("Mixed")),
       "", [&](ImVec2 at) {
         if (room == kNoId) ImGui::BeginDisabled();
         relKnob(a, 4, ids, lin("Room", -24, 24, 0), true, at,
                 [&a, room](ObjectId id) { return room != kNoId ? mixops::sendGain(a.doc.song(), id, room) : 0.0; },
                 [&a, room](ObjectId id, double st0, double v) {
                   if (room != kNoId && st0 > 0) a.doc.setSend(id, room, static_cast<float>(std::min(2.0, st0 * std::pow(10.0, v / 20.0))));
                 });
         if (room == kNoId) ImGui::EndDisabled();
       });
  std::snprintf(vb, sizeof vb, "%+.1f dB", gRel[5].v);
  cell(a, cx(5), p0.y, w, i18n::tr("Level"), vb, i18n::tr("all together"), [&](ImVec2 at) {
    const auto vd = describeParam(s, mixerParam(ids.front(), MixerParam::Volume));
    const double hi = vd ? vd->max : 1.25;
    relKnob(a, 5, ids, lin("Level", -24, 12, 0), true, at, readP(MixerParam::Volume),
            [&](ObjectId id, double st0, double v) { setP(id, MixerParam::Volume, std::clamp(st0 * std::pow(10.0, v / 40.0), 0.0, hi)); });
    meter(a, ids.front(), ImVec2(at.x + kKnobR * 2 + 10, at.y - 2), kKnobR * 2 + 4);
  });
}

// ---- the whole stage (nothing selected)
void panelStage(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  identityTile(a, dl, nullptr, ImVec2(p0.x + 16, p0.y + 14), 36);
  {  // K-224: with rooms, the panel is about the room shown
    const std::string title = roomops::roomCount(s) > 1 ? roomops::roomName(s, shownRoom(a)) : std::string(i18n::tr("Whole stage"));
    theme::textEllipsis(dl, a.fontBold, 14.0f, ImVec2(p0.x + 62, p0.y + 16), kIdentityW - 70, P.text, title.c_str());
  }
  char b[96];
  std::snprintf(b, sizeof b, i18n::tr("%zu tracks \xC2\xB7 nothing selected"), v.toks.size());
  theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(p0.x + 62, p0.y + 35), kIdentityW - 70, P.textDim, b);
  if (compactPanel(p0, p1)) {
    // Narrow: the two buttons under the identity; the room is picked from the toolbar.
    const float bw = std::min(p1.x - p0.x - 32, 220.0f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p0.y + 60));
    if (ui::PrimaryButton(i18n::tr("Spread out"), bw, "Space the tracks evenly from the centre outward, each keeping its distance")) startPreview(a, PreviewKind::Spread);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p0.y + 94));
    if (ui::TextButton(i18n::tr("Arrange by sound..."), false, bw, "Place each track by what it is: shown first, nothing changes until Apply"))
      startPreview(a, PreviewKind::Sound);
    return;
  }
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 16, p1.y - 14 - 16), kIdentityW - 24, P.textFaint, i18n::tr("Click a track to set it on its own"));
  const float rightW = 184;
  const float x0 = p0.x + kIdentityW + 16, x1 = p1.x - rightW - 16, w = (x1 - x0) / 4.0f;
  dl->AddLine(ImVec2(p1.x - rightW, p0.y + 14), ImVec2(p1.x - rightW, p1.y - 14), P.line);
  auto cx = [&](int k) { return x0 + w * (static_cast<float>(k) + 0.5f); };
  // No room: the knobs of the room are shown but off.
  const MixerTrack* room = shownStage(a).hasRoom() ? mixops::stageRoom(s, shownRoom(a)) : nullptr;
  // K-270: the space itself is picked from the toolbar's Space button (the same list, per room, one undo
  // step each); the panel keeps the space's size, the depth, the room width and the space level.
  static bool sizeOn = false, depthOn = false;
  cell(a, cx(0), p0.y, w, i18n::tr("Space size"), room ? i18n::tr(st::roomSizeWord(mixops::roomSize(s, shownRoom(a)))) : i18n::tr("No space"), i18n::tr("small room to hall"), [&](ImVec2 at) {
    if (!room) ImGui::BeginDisabled();
    double val = mixops::roomSize(s, shownRoom(a));
    bool rel = false;
    ImGui::SetCursorScreenPos(at);
    if (ui::Knob("##proomsize", &val, lin("Room size", 0, 1, 0.4), kKnobR, nullptr, false, &rel)) mixops::setRoomSize(a.doc, val, sizeOn, shownRoom(a)), sizeOn = true;
    if (rel) sizeOn = false, a.doc.endGesture();
    if (!room) ImGui::EndDisabled();
  });
  cell(a, cx(1), p0.y, w, i18n::tr("Depth"), room ? i18n::tr(st::strengthWord(shownStage(a).depthStrength)) : i18n::tr("Not heard"),
       room ? i18n::tr("how far back it feels") : i18n::tr("pick a space to hear depth"), [&](ImVec2 at) {
    if (!room) ImGui::BeginDisabled();
    double val = shownStage(a).depthStrength;
    bool rel = false;
    ImGui::SetCursorScreenPos(at);
    if (ui::Knob("##pstrength", &val, lin("Depth", 0, 1, 0.5), kKnobR, nullptr, false, &rel)) mixops::setDepthStrength(a.doc, val, depthOn, shownRoom(a)), depthOn = true;
    if (rel) depthOn = false, a.doc.endGesture();
    if (!room) ImGui::EndDisabled();
  });
  // K-224: the room's own width (every track in it; 100 %: as each track says).
  static bool widthOn = false;
  char wb[32];
  std::snprintf(wb, sizeof wb, "%d%%", static_cast<int>(std::lround(shownStage(a).width * 100.0f)));
  cell(a, cx(2), p0.y, w, i18n::tr("Room width"), wb, i18n::tr("narrow to very wide"), [&](ImVec2 at) {
    double val = shownStage(a).width;
    bool rel = false;
    ImGui::SetCursorScreenPos(at);
    if (ui::Knob("##proomwidth", &val, lin("Room width", 0, 1.5, 1.0), kKnobR, nullptr, false, &rel))
      mixops::setRoomWidth(a.doc, val, widthOn, shownRoom(a)), widthOn = true;
    if (rel) widthOn = false, a.doc.endGesture();
  });
  cell(a, cx(3), p0.y, w, i18n::tr("Space level"), room ? levelText(room->volume) : std::string(i18n::tr("No space")), i18n::tr("for the whole room"), [&](ImVec2 at) {
    if (room) paramKnob("##proomvol", mixerParam(room->id, MixerParam::Volume), false, at);
    else {
      ImGui::BeginDisabled();
      double z = 0;
      ImGui::SetCursorScreenPos(at);
      ui::Knob("##proomvol0", &z, lin("Room level", 0, 1, 0), kKnobR, nullptr, false);
      ImGui::EndDisabled();
    }
  });
  const float bw = rightW - 32;
  ImGui::SetCursorScreenPos(ImVec2(p1.x - rightW + 16, p0.y + 38));
  if (ui::PrimaryButton(i18n::tr("Spread out"), bw, "Space the tracks evenly from the centre outward, each keeping its distance")) startPreview(a, PreviewKind::Spread);
  ImGui::SetCursorScreenPos(ImVec2(p1.x - rightW + 16, p0.y + 72));
  if (ui::TextButton(i18n::tr("Arrange by sound..."), false, bw, "Place each track by what it is: shown first, nothing changes until Apply"))
    startPreview(a, PreviewKind::Sound);
}

// ---- the preview strip: Cancel and Apply
void panelPreview(App& a, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  const View& v = view();
  const Preview& pv = v.preview;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const char* title = pv.kind == PreviewKind::Spread ? i18n::tr("Spread out") : pv.kind == PreviewKind::Sound ? i18n::tr("Arrange by sound") : i18n::tr(st::preset(pv.preset).name);
  char b[160];
  if (pv.kind == PreviewKind::Sound) std::snprintf(b, sizeof b, i18n::tr("%zu placed, %d not sure"), pv.ids.size() - static_cast<size_t>(pv.unsure), pv.unsure);
  else std::snprintf(b, sizeof b, i18n::tr("%zu tracks"), pv.ids.size());
  const float y = p0.y + 36;
  ui::drawIcon(dl, ui::Icon::Star, ImVec2(p0.x + 24, y + 8), 14, P.hot);
  dl->AddText(a.fontBold, 14.0f, ImVec2(p0.x + 40, y), P.text, title);
  const float tw = a.fontBold->CalcTextSizeA(14.0f, 1e9f, 0, title).x;
  const bool compact = compactPanel(p0, p1);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 50 + tw, y + 2), p1.x - 16 - (p0.x + 50 + tw), P.textDim, b);
  const char* desc = pv.kind == PreviewKind::Spread
                         ? i18n::tr("Evenly from the centre outward, left and right in turn, in mixer order. Every track keeps its distance.")
                     : pv.kind == PreviewKind::Sound
                         ? i18n::tr("Read from what plays on each track first, then how it sounds, then its name. Hover a track to see why. Tracks marked ? stay in the middle for you to place.")
                         : i18n::tr(st::preset(pv.preset).hint);
  const float right = compact ? p1.x - 8 : p1.x - 200;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 16, y + 24), right - p0.x - 24, P.textDim, desc);
  if (pv.kind == PreviewKind::Sound && pv.unsure > 0) {
    const ImVec2 q(p0.x + 23, y + 55);
    dl->AddCircleFilled(q, 7, P.surface600, 16);
    const ImVec2 ts = a.fontBold->CalcTextSizeA(10, 1e9f, 0, "?");
    dl->AddText(a.fontBold, 10, ImVec2(std::round(q.x - ts.x * 0.5f), std::round(q.y - ts.y * 0.5f)), P.text, "?");
    std::snprintf(b, sizeof b, i18n::tr("%d to place yourself"), pv.unsure);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x + 38, y + 48), P.text, b);
  }
  const float cy = compact ? p1.y - 14 - 24 : (p0.y + p1.y) * 0.5f - 12;  // narrow: the buttons on the bottom row
  const float applyW = ImGui::CalcTextSize(i18n::tr("Apply")).x + 24, cancelW = ImGui::CalcTextSize(i18n::tr("Cancel")).x + 24;
  ImGui::SetCursorScreenPos(ImVec2(p1.x - 16 - applyW - 10 - cancelW, cy));
  if (ui::TextButton("Cancel", false, cancelW, "Leave every track where it is (Esc)")) cancelPreview();
  ImGui::SetCursorScreenPos(ImVec2(p1.x - 16 - applyW, cy));
  if (ui::PrimaryButton(i18n::tr("Apply"), applyW, "Move the tracks there (one undo step)")) applyPreview(a);
}

}  // namespace

void drawPanel(App& a) {
  View& v = view();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = v.panel0, p1 = v.panel1;
  dl->AddRectFilled(p0, p1, v.stage, 10);
  if (v.preview.active()) {
    panelPreview(a, p0, p1);
    return;
  }
  if (!compactPanel(p0, p1)) dl->AddLine(ImVec2(p0.x + kIdentityW, p0.y + 14), ImVec2(p0.x + kIdentityW, p1.y - 14), pal().line);
  std::vector<const Tok*> sel;
  for (const Tok& t : v.toks)
    if (t.selected) sel.push_back(&t);
  if (sel.size() > 1) {
    panelSeveral(a, sel, p0, p1);
    return;
  }
  // One track: the focused one when it stands on the stage.
  const Tok* one = tokOf(a.selMixer);
  if (one && (sel.empty() || sel.front() == one)) panelOne(a, *one, p0, p1);
  else panelStage(a, p0, p1);
}

// ---------------------------------------------------------------- presets
namespace {

// A tiny stage with where a preset puts a typical band.
void presetDiagram(App& a, ImDrawList* dl, ImVec2 mn, int preset) {
  (void)a;
  const auto& P = pal();
  const View& v = view();
  const float w = 56, h = 36;
  dl->AddRectFilled(mn, ImVec2(mn.x + w, mn.y + h), P.dark ? P.surface950 : P.surface800, 5);
  const ImVec2 tl(mn.x + 15, mn.y + 5), trr(mn.x + w - 15, mn.y + 5), br(mn.x + w - 5, mn.y + h - 9), bl(mn.x + 5, mn.y + h - 9);
  dl->AddQuadFilled(tl, trr, br, bl, v.stage);
  dl->AddCircleFilled(ImVec2(mn.x + w * 0.5f, mn.y + h - 4), 2.2f, P.accent, 10);
  static const st::Role band[] = {st::Role::LeadVocal, st::Role::Kick, st::Role::Bass, st::Role::Snare, st::Role::Hats,
                                  st::Role::Keys,      st::Role::Guitar, st::Role::Pad, st::Role::Fx};
  static const uint32_t cols[] = {0xC0607A, 0xC0603A, 0x7A60C0, 0xC08050, 0xB0A040, 0x50A080, 0xC09050, 0x5080C0, 0x8090A0};
  const std::vector<st::Role> roles(std::begin(band), std::end(band));
  const std::vector<st::Pos> at = st::arrangePreset(preset, roles);
  for (size_t k = 0; k < at.size(); ++k) {
    const float d = static_cast<float>(at[k].depth);
    const float y = (bl.y - 3) + (tl.y + 3 - (bl.y - 3)) * d;
    const float half = ((br.x - bl.x) * 0.5f - 5) + (((trr.x - tl.x) * 0.5f - 3) - ((br.x - bl.x) * 0.5f - 5)) * d;
    const float x = mn.x + w * 0.5f + static_cast<float>(at[k].pan) * half;
    const float s = 4.6f - 1.4f * d;
    dl->AddRectFilled(ImVec2(x - s * 0.5f, y - s * 0.5f), ImVec2(x + s * 0.5f, y + s * 0.5f), theme::content(cols[k]), 1);
  }
}

bool menuRow(App& a, const char* id, const char* title, const char* hint, const std::function<void(ImDrawList*, ImVec2)>& icon, bool hot) {
  const auto& P = pal();
  const float w = 330, h = 46;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Selectable(id, false, 0, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hot && !hov) dl->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + w + 4, p.y + h + 2), theme::mix(theme::popupBg(P), P.hot, 0.12f), 6);
  icon(dl, ImVec2(p.x + 4, p.y + 5));
  dl->AddText(a.fontBold, 13.0f, ImVec2(p.x + 72, p.y + 6), hot ? P.hot : P.text, title);
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(p.x + 72, p.y + 25), P.textDim, hint);
  return clicked;
}

}  // namespace

void presetsMenu(App& a) {
  const auto& P = pal();
  View& v = view();
  auto glyph = [&](ui::Icon ic) {
    return [&P, ic](ImDrawList* dl, ImVec2 mn) {
      dl->AddRectFilled(mn, ImVec2(mn.x + 56, mn.y + 36), P.dark ? P.surface950 : P.surface800, 5);
      ui::drawIcon(dl, ic, ImVec2(mn.x + 28, mn.y + 18), 16, P.text);
    };
  };
  if (menuRow(a, "##parrange", i18n::tr("Arrange by sound..."), i18n::tr("Places each track by what it is and how it sounds"), glyph(ui::Icon::Star), true))
    startPreview(a, PreviewKind::Sound);
  if (menuRow(a, "##pspread", i18n::tr("Spread out"), i18n::tr("Evenly from the centre; each track keeps its distance"), glyph(ui::Icon::SwapArrows), false))
    startPreview(a, PreviewKind::Spread);
  ImGui::Separator();
  for (int k = 0; k < st::kPresetCount; ++k) {
    ImGui::PushID(k);
    const st::Preset& pr = st::preset(k);
    if (menuRow(a, "##preset", i18n::tr(pr.name), i18n::tr(pr.hint), [&a, k](ImDrawList* dl, ImVec2 mn) { presetDiagram(a, dl, mn, k); }, false)) {
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

// ---------------------------------------------------------------- the room
void roomPopover(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const View& v = view();
  const MixerTrack* room = shownStage(a).hasRoom() ? mixops::stageRoom(s, shownRoom(a)) : nullptr;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float W = 300;
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddText(a.fontBold, 14.0f, ImVec2(p.x, p.y + 2), P.text, i18n::tr("Space"));
  std::vector<ObjectId> all;
  for (const Tok& t : v.toks) all.push_back(t.id);
  // One list: No room first, then the rooms. A row's hint shows under the list while it is hovered.
  const float rowH = 24;
  float y = p.y + 24;
  int hovered = -1;
  for (int k = 0; k < mixops::kRoomTypes; ++k) {
    if (k == 1) {
      dl->AddLine(ImVec2(p.x, y + 3), ImVec2(p.x + W, y + 3), P.line);
      y += 7;
    }
    const ImVec2 mn(p.x - 4, y);
    ImGui::SetCursorScreenPos(mn);
    ImGui::PushID(k);
    ImGui::InvisibleButton("##rt", ImVec2(W + 8, rowH));
    const bool hov = ImGui::IsItemHovered(), cur = shownStage(a).roomType == k;
    if (hov) hovered = k;
    if (ImGui::IsItemClicked()) mixops::setRoomType(a.doc, k, all, shownRoom(a));
    ImGui::PopID();
    if (hov) dl->AddRectFilled(mn, ImVec2(mn.x + W + 8, mn.y + rowH), P.surface700, 4);
    const ImU32 fg = cur ? P.hot : P.text;
    if (cur) dl->AddCircleFilled(ImVec2(p.x + 6, y + rowH * 0.5f), 3.5f, k == 0 ? P.textDim : P.led, 12);
    dl->AddText(ImVec2(p.x + 18, y + (rowH - ImGui::GetFontSize()) * 0.5f), fg, i18n::tr(mixops::roomTypeName(k)));
    y += rowH;
  }
  {
    const int shown = hovered >= 0 ? hovered : shownStage(a).roomType;
    y += 4;
    const float wrap = W;
    const char* h = i18n::tr(mixops::roomTypeHint(shown));
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x, y), P.textDim, h, nullptr, wrap);
    y += a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, wrap, h).y + 10;
  }
  dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + W, y), P.line);
  y += 10;
  // Amount: the room track's level.
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x, y), P.textDim, i18n::tr("Amount"));
  if (room) {
    const ParamId pid = mixerParam(room->id, MixerParam::Volume);
    const auto d = describeParam(s, pid);
    const double val = room->volume;
    const float sx0 = p.x + 120, sw = 150, sy = y + 8;
    ImGui::SetCursorScreenPos(ImVec2(sx0 - 6, sy - 8));
    ImGui::InvisibleButton("##roomamt", ImVec2(sw + 12, 16));
    if (d && ImGui::IsItemActive()) {
      const double n = std::clamp((ImGui::GetIO().MousePos.x - sx0) / sw, 0.0f, 1.0f);
      const double nv = d->fromNormalized(n);
      if (std::fabs(nv - val) > 1e-9) a.doc.setParam(pid, nv, true);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    const float n = d ? static_cast<float>(d->toNormalized(val)) : 0.5f;
    dl->AddRectFilled(ImVec2(sx0, sy - 1.5f), ImVec2(sx0 + sw, sy + 1.5f), P.field, 2);
    dl->AddRectFilled(ImVec2(sx0, sy - 1.5f), ImVec2(sx0 + sw * n, sy + 1.5f), P.hot, 2);
    dl->AddCircleFilled(ImVec2(sx0 + sw * n, sy), 6.5f, P.knobFill, 16);
    const std::string t = levelText(val);
    const float w = ImGui::CalcTextSize(t.c_str()).x;
    dl->AddText(ImVec2(p.x + W - w, y), P.text, t.c_str());
  } else {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x + 120, y), P.textFaint, i18n::tr("No space"));
  }
  // How far back the stage feels.
  y += 30;
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x, y), P.textDim, i18n::tr("Back of the stage"));
  ImGui::SetCursorScreenPos(ImVec2(p.x, y + 20));
  static const char* strengths[] = {i18n::N_("Gentle"), i18n::N_("Natural"), i18n::N_("Strong")};
  static const char* hints[] = {"The back of the stage is 6 dB quieter", "The back of the stage is 9 dB quieter and duller",
                                "The back of the stage is 12 dB quieter and much duller"};
  int si = std::clamp(static_cast<int>(std::lround(shownStage(a).depthStrength * 2.0)), 0, 2);
  if (!room) ImGui::BeginDisabled();
  if (ui::Segmented("##strength", strengths, 3, &si, (W - 4) / 3.0f, 0, hints)) mixops::setDepthStrength(a.doc, si * 0.5, false, shownRoom(a));
  if (!room) ImGui::EndDisabled();
  y += 20 + ImGui::GetFrameHeight() + 10;
  dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + W, y), P.line);
  y += 8;
  ImGui::SetCursorScreenPos(ImVec2(p.x, y));
  if (room) {
    if (ImGui::Selectable("##openroom", false, 0, ImVec2(W, 22))) {
      selectOnly(a, room->id);
      setView(a, false);
      ImGui::CloseCurrentPopup();
    }
    ui::drawIcon(dl, ui::Icon::Star, ImVec2(p.x + 7, y + 11), 11, P.hot);
    dl->AddText(ImVec2(p.x + 22, y + 3), P.hot, i18n::tr("Open the space track"));
  } else {
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(p.x, y + 3), P.textFaint, i18n::tr("Picking a space adds a space track"));
    ImGui::Dummy(ImVec2(W, 22));
  }
}

// ---------------------------------------------------------------- effects
void effectsPopover(App& a, ObjectId id) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const int i = s.mixerIndex(id);
  if (i < 0) {
    ImGui::CloseCurrentPopup();
    return;
  }
  const MixerTrack& t = s.mixer[static_cast<size_t>(i)];
  ImGui::PushFont(a.fontBold);
  ImGui::TextUnformatted(mixerTrackName(s, static_cast<size_t>(i)).c_str());
  ImGui::PopFont();
  ImGui::Separator();
  int firstEmpty = -1, n = 0;
  for (int k = 0; k < static_cast<int>(t.slots.size()); ++k) {
    const EffectSlot& es = t.slots[static_cast<size_t>(k)];
    const PluginInfo* info = findPlugin(es.plugin.type);
    if (!info) {
      if (firstEmpty < 0) firstEmpty = k;
      continue;
    }
    ++n;
    ImGui::PushID(k);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##fx", a.fxEditor == std::make_pair(id, k), 0, ImVec2(240, 20))) a.fxEditor = {id, k};
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##fxslot");
    if (ImGui::BeginPopup("##fxslot")) {
      mixerSlotMenu(a, id, k);
      ImGui::EndPopup();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 10), 3, es.enabled ? P.led : P.surface600, 10);
    dl->AddText(ImVec2(p.x + 16, p.y + 2), es.enabled ? P.text : P.textFaint, info->name.c_str());
    ImGui::PopID();
  }
  if (n == 0) ImGui::TextDisabled("%s", i18n::tr("No effects on this track yet"));
  if (firstEmpty >= 0 && ImGui::BeginMenu(i18n::tr("Add an effect"))) {
    mixerSlotMenu(a, id, firstEmpty);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Show its strip in the mix"))) {
    selectOnly(a, id);
    setView(a, false);
  }
}

// ---------------------------------------------------------------- first run
void firstRunCard(App& a) {
  const auto& P = pal();
  View& v = view();
  const st::Geo& g = v.g;
  const float W = 438, H = 224;
  const ImVec2 at(std::round(g.cx - W * 0.5f), std::round(g.backY + std::min(110.0f, std::max(10.0f, (g.frontY - g.backY) * 0.2f))));
  ImGui::SetCursorScreenPos(at);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(theme::popupBg(P)));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.lineStrong));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  v.cardOpen = true;
  if (ImGui::BeginChild("##stagecard", ImVec2(W, H), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddText(a.fontBold, 15.0f, ImVec2(o.x + 22, o.y + 22), P.text, i18n::tr("The stage"));
    ImGui::SetCursorScreenPos(ImVec2(o.x + W - 30, o.y + 18));
    ImGui::InvisibleButton("##cardclose", ImVec2(16, 16));
    ui::drawIcon(dl, ui::Icon::Close, ImVec2(o.x + W - 22, o.y + 26), 10, ImGui::IsItemHovered() ? P.text : P.textDim);
    if (ImGui::IsItemClicked()) a.settings.set("ui.stage.welcomed", true);
    static const char* caps[] = {i18n::N_("Left and right"), i18n::N_("Back is further away"), i18n::N_("Drag to move")};
    const ImU32 tile = theme::content(0xC08050);
    for (int k = 0; k < 3; ++k) {
      const ImVec2 mn(o.x + 22 + static_cast<float>(k) * 132, o.y + 56);
      const ImVec2 mx(mn.x + 120, mn.y + 78);
      dl->AddRectFilled(mn, mx, P.dark ? P.surface950 : P.surface800, 6);
      dl->AddQuadFilled(ImVec2(mn.x + 26, mn.y + 8), ImVec2(mx.x - 26, mn.y + 8), ImVec2(mx.x - 8, mx.y - 18), ImVec2(mn.x + 8, mx.y - 18), v.stage);
      dl->AddCircleFilled(ImVec2(mn.x + 60, mx.y - 8), 3.5f, P.accent, 12);
      const ImU32 ar = P.textDim;
      if (k == 0) {
        dl->AddRectFilled(ImVec2(mn.x + 53, mn.y + 30), ImVec2(mn.x + 67, mn.y + 44), tile, 3);
        dl->AddLine(ImVec2(mn.x + 24, mn.y + 37), ImVec2(mn.x + 46, mn.y + 37), ar, 1.5f);
        dl->AddTriangleFilled(ImVec2(mn.x + 22, mn.y + 37), ImVec2(mn.x + 28, mn.y + 33), ImVec2(mn.x + 28, mn.y + 41), ar);
        dl->AddLine(ImVec2(mn.x + 74, mn.y + 37), ImVec2(mn.x + 96, mn.y + 37), ar, 1.5f);
        dl->AddTriangleFilled(ImVec2(mn.x + 98, mn.y + 37), ImVec2(mn.x + 92, mn.y + 33), ImVec2(mn.x + 92, mn.y + 41), ar);
      } else if (k == 1) {
        dl->AddRectFilled(ImVec2(mn.x + 34, mn.y + 37), ImVec2(mn.x + 50, mn.y + 53), tile, 3);
        dl->AddRectFilled(ImVec2(mn.x + 72, mn.y + 12), ImVec2(mn.x + 82, mn.y + 22), tile, 2);
        dl->AddLine(ImVec2(mn.x + 54, mn.y + 36), ImVec2(mn.x + 68, mn.y + 25), ar, 1.5f);
      } else {
        dashedRect(dl, ImVec2(mn.x + 26, mn.y + 34), ImVec2(mn.x + 40, mn.y + 48), P.textDim, 1.0f, 3, 2);
        dl->AddLine(ImVec2(mn.x + 46, mn.y + 41), ImVec2(mn.x + 64, mn.y + 41), ar, 1.5f);
        dl->AddTriangleFilled(ImVec2(mn.x + 68, mn.y + 41), ImVec2(mn.x + 62, mn.y + 37), ImVec2(mn.x + 62, mn.y + 45), ar);
        dl->AddRectFilled(ImVec2(mn.x + 72, mn.y + 30), ImVec2(mn.x + 86, mn.y + 44), tile, 3);
      }
      centredText(dl, a.fontRegular, theme::kFontCaption, mn.x + 60, mx.y + 8, P.textDim, i18n::tr(caps[k]), 128);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x + 22, o.y + 172));
    if (ui::PrimaryButton(i18n::tr("Arrange for me"), 0, "Place each track by what it is (shown first: Apply or Cancel)")) {
      a.settings.set("ui.stage.welcomed", true);
      startPreview(a, PreviewKind::Sound);
    }
    ImGui::SameLine(0, 8);
    if (ui::TextButton("Place them myself", false, 0, "Close this card and drag the tracks yourself")) a.settings.set("ui.stage.welcomed", true);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

}  // namespace gl::stageui::detail
