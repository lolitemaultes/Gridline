// Gridline — mixer: toolbar (layout, rows, meters, views), strips (master,
// docked and wrapped inserts, groups, extended slot lists, waveforms,
// routing arrows with send knobs), selected-track inspector with effect
// slots, sends and strip switches, routing overview and the effect editor.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include "CollabUi.h"
#include "CursorAnchor.h"
#include "../core/I18n.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "PluginUI.h"
#include "Windows.h"
#include "ContextMenus.h"
#include "HostedUi.h"
#include "TrackIcons.h"
#include "../core/MixerLayout.h"
#include "../core/MixerOps.h"
#include "../core/WheelRoute.h"
#include "../core/WavWriter.h"
#include "../engine/Render.h"
#include "BrowserUi.h"
#include "../engine/StripEq.h"
#include "../core/SmoothSnap.h"
#include "SmoothScrollUi.h"

namespace gl {

using theme::pal;

namespace {
float gMixerStripsScroll = 0;     // J-83 dev check: the strips pane's scroll as drawn last
bool gMixerWheelScrolls = false;  // J-83: the plain wheel scrolls the mixer (thin layout / setting)
}  // namespace
float mixerStripsScrollY() { return gMixerStripsScroll; }
bool mixerWheelScrolls() { return gMixerWheelScrolls; }

// Generic parameter panel used by effects and instruments: knobs for
// continuous values, segmented buttons for choices, toggles for switches.
void pluginParamGrid(App& a, const PluginInfo& info, const std::function<ParamId(int)>& pidOf, float width) {
  const Song& s = a.doc.song();
  const float cell = 74.0f;
  int perRow = std::max(1, static_cast<int>(width / cell));
  int col = 0;
  for (size_t k = 0; k < info.params.size(); ++k) {
    const ParamDesc& d = info.params[k];
    ParamId pid = pidOf(static_cast<int>(k));
    ImGui::PushID(static_cast<int>(k));
    if (d.kind == ParamKind::Choice) {
      if (col) ImGui::NewLine();
      col = 0;
      ui::Label(d.name.c_str());
      int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
      if (d.choices.size() <= 6) {
        for (size_t i = 0; i < d.choices.size(); ++i) {
          if (i) ImGui::SameLine(0, 2);
          if (ui::TextButton(d.choices[i].c_str(), static_cast<int>(i) == cur)) a.doc.setParam(pid, static_cast<double>(i), false);
        }
      } else {
        if (ui::BeginCombo("##c", d.format(cur).c_str(), 160)) {
          for (size_t i = 0; i < d.choices.size(); ++i)
            if (ImGui::Selectable(d.choices[i].c_str(), static_cast<int>(i) == cur)) a.doc.setParam(pid, static_cast<double>(i), false);
          ImGui::EndCombo();
        }
      }
      ImGui::PopID();
      continue;
    }
    if (col > 0) ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    float x = ImGui::GetCursorPosX();
    if (d.kind == ParamKind::Toggle) {
      bool on = getParam(s, pid).value_or(0) >= 0.5;
      ImGui::SetCursorPosX(x + 4);
      ImGui::Dummy(ImVec2(0, 4));
      ImGui::SetCursorPosX(x + 4);
      if (ui::TextButton(on ? "On" : "Off", on, cell - 12)) a.doc.setParam(pid, on ? 0 : 1, false);
      ImGui::SetCursorPosX(x + 2);
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
      ImGui::TextColored(theme::vec(pal().textDim), "%s", d.name.c_str());
      ImGui::PopFont();
    } else {
      ImGui::SetCursorPosX(x + (cell - 36) * 0.5f);
      bool bip = d.min < 0 && d.max > 0;
      ui::ParamKnob("k", pid, 18.0f, nullptr, bip);
      std::string v = getParam(s, pid) ? d.format(*getParam(s, pid)) : "";
      ImGui::PushFont(a.fontMono, 10.5f);
      ImVec2 ts = ImGui::CalcTextSize(v.c_str());
      ImGui::SetCursorPosX(x + (cell - ts.x) * 0.5f);
      ImGui::TextColored(theme::vec(pal().text), "%s", v.c_str());
      ImGui::PopFont();
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
      ts = ImGui::CalcTextSize(d.name.c_str());
      ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
      ImGui::TextColored(theme::vec(pal().textDim), "%s", d.name.c_str());
      ImGui::PopFont();
    }
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    col = (col + 1) % perRow;
    ImGui::PopID();
  }
}


namespace {

// Small square toggle used for mute / solo on the strips.
bool stripToggle(const char* id, const char* label, bool on, ImU32 onCol, ImVec2 size, const char* hint) {
  const auto& P = pal();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool r = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = on ? onCol : hov ? P.surface600 : P.surface900;
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, theme::kRadiusSm);
  if (!on) dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), P.line, theme::kRadiusSm);
  App& a = app();
  ImVec2 ts = a.fontBold->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, label);
  dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(std::round(p.x + (size.x - ts.x) * 0.5f), std::round(p.y + (size.y - ts.y) * 0.5f)),
              on ? theme::contrastText(onCol) : hov ? P.text : P.textDim, label);
  ui::Hint(hint);
  ui::Tooltip(hint);
  return r;
}

// Effect picker list (categories as small labels).
void effectList(App& a, ObjectId track, int slot, const std::string& current) {
  if (ImGui::Selectable(i18n::tr("(empty)"), current.empty())) a.doc.setSlotPlugin(track, slot, "");
  std::string cat;
  for (const auto& p : pluginCatalog()) {
    if (p.kind != PluginKind::Effect) continue;
    if (p.category != cat) {
      cat = p.category;
      ImGui::Spacing();
      ui::Label(cat.c_str());
    }
    if (ImGui::Selectable(p.name.c_str(), p.type == current)) {
      a.doc.setSlotPlugin(track, slot, p.type);
      a.fxEditor = {track, slot};
    }
  }
}

// Compiled mixer track (latencies) of the running program, or nullptr.
const ProgMixer* progMixer(App& a, ObjectId id) {
  const auto& prog = a.engine.program();
  if (!prog) return nullptr;
  for (const auto& m : prog->mixer)
    if (m.id == id) return &m;
  return nullptr;
}

// "2.9ms" / "12ms" for a latency in samples.
std::string latencyLabel(int samples, double sr) {
  char b[24];
  const double ms = 1000.0 * samples / std::max(1.0, sr);
  std::snprintf(b, sizeof b, ms < 9.95 ? "%.1fms" : "%.0fms", ms);
  return b;
}

// Drag payload of an effect slot (inspector rows and extended strips).
struct SlotDrag {
  ObjectId track;
  int slot;
};

// Q-8: wheel reorder state. `slotWheel`: the effect the last notch moved and
// where the pointer was (it stays the target while the pointer rests);
// `slotGlide`: the last move, for drawing the rows gliding into place.
struct SlotWheel {
  ObjectId track = kNoId;
  int slot = -1;
  ImVec2 at;
  double time = 0;
  float acc = 0;  // trackpad deltas: one move per whole notch
};
SlotWheel slotWheel;
struct SlotGlide {
  ObjectId track = kNoId;
  int from = 0, to = 0;
  double t0 = -1;
};
SlotGlide slotGlide;

void wheelSlot(App& a, ObjectId track, int slot, const ImGuiIO& io) {
  if (slotWheel.track != track || slotWheel.slot != slot) slotWheel.acc = 0;
  slotWheel.acc += io.MouseWheel;
  const int notches = static_cast<int>(slotWheel.acc);
  if (notches == 0) {
    slotWheel.track = track, slotWheel.slot = slot, slotWheel.at = io.MousePos, slotWheel.time = ImGui::GetTime();
    return;
  }
  slotWheel.acc -= static_cast<float>(notches);
  const int to = mixops::stepSlot(a.doc, track, slot, -notches);  // wheel up = up the chain
  if (to != slot) {
    if (a.fxEditor == std::make_pair(track, slot)) a.fxEditor.second = to;
    slotGlide = SlotGlide{track, slot, to, ImGui::GetTime()};
    a.setHint(i18n::tr("Effect moved (wheel over a slot moves its effect)"));
  }
  slotWheel = SlotWheel{track, to, io.MousePos, ImGui::GetTime(), slotWheel.acc};
}

// Drawn offset (in rows) of slot `k` while the last move glides into place.
float slotGlideOffset(ObjectId track, int k) {
  if (slotGlide.track != track || slotGlide.t0 < 0 || !scrollui::smoothSnap()) return 0;
  const double dt = ImGui::GetTime() - slotGlide.t0;
  if (dt > smooth::settleTime() * 2) return 0;
  const int f = slotGlide.from, t = slotGlide.to;
  int prev = k;  // where slot k's content was drawn before the move
  if (k == t) prev = f;
  else if (t < f && k > t && k <= f) prev = k - 1;
  else if (t > f && k >= f && k < t) prev = k + 1;
  if (prev == k) return 0;
  return static_cast<float>(smooth::approach(static_cast<double>(prev), static_cast<double>(k), dt) - k);
}

// Moves / copies an effect dropped on (track, slot); Ctrl copies.
void dropSlot(App& a, const SlotDrag& from, ObjectId track, int slot) {
  const bool copy = ImGui::GetIO().KeyCtrl;
  if (from.track == track && from.slot == slot) return;
  if (!mixops::moveSlotTo(a.doc, from.track, from.slot, track, slot, copy))
    a.notify("No room: every effect slot below that one is taken", true);
  else
    a.selMixer = track;
}

// First free slot of a track (-1 = full).
int freeSlot(const MixerTrack& t) {
  for (int k = 0; k < kEffectSlots; ++k)
    if (t.slots[static_cast<size_t>(k)].plugin.empty()) return k;
  return -1;
}

// Strip geometry per mixer layout.
struct StripDims {
  float w, knobR, meterW, faderW;
};
StripDims stripDims(int layout) {
  switch (layout) {
    case MixerViewState::Compact: return {54.0f, 7.5f, 9.0f, 18.0f};
    case MixerViewState::ExtraLarge: return {96.0f, 11.0f, 14.0f, 26.0f};
    default: return {66.0f, 9.0f, 11.0f, 22.0f};
  }
}
constexpr float kExtSlotRowH = 13.0f;
constexpr float kWaveH = 24.0f;

// Meter with peak / RMS / both modes, optional peak hold and a dB scale.
void stripMeter(ImGuiID key, float pl, float pr, float rl, float rr, ImVec2 size, const MixerViewState& v, bool scale) {
  struct State {
    float l = 0, r = 0, rl = 0, rr = 0, holdL = 0, holdR = 0;
    double holdT = 0;
  };
  static std::map<ImGuiID, State> states;
  State& s = states[key];
  const float decay = std::exp(-ImGui::GetIO().DeltaTime * 6.0f);
  s.l = std::max(pl, s.l * decay);
  s.r = std::max(pr, s.r * decay);
  s.rl = std::max(rl, s.rl * decay);
  s.rr = std::max(rr, s.rr * decay);
  const double now = ImGui::GetTime();
  if (s.l >= s.holdL || s.r >= s.holdR || now - s.holdT > 1.5) {
    s.holdL = s.l, s.holdR = s.r, s.holdT = now;
  }
  const auto& P = pal();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto toPos = [](float g) {
    float db = g > 1e-6f ? 20.0f * std::log10(g) : -100.0f;
    return std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
  };
  const float zero = toPos(1.0f), warn = toPos(0.5f);
  const float w = std::floor((size.x - 1) * 0.5f);
  for (int ch = 0; ch < 2; ++ch) {
    const float pk = toPos(ch == 0 ? s.l : s.r), rm = toPos(ch == 0 ? s.rl : s.rr), hold = toPos(ch == 0 ? s.holdL : s.holdR);
    const float v0 = v.meterMode == MixerViewState::Rms ? rm : pk;
    ImVec2 a0(p.x + static_cast<float>(ch) * (w + 1), p.y), b0(a0.x + w, p.y + size.y);
    dl->AddRectFilled(a0, b0, P.surface950, 1);
    auto yAt = [&](float f) { return b0.y - f * size.y; };
    auto bar = [&](float val, float alpha, float x0, float x1) {
      const float top = yAt(val);
      dl->AddRectFilled(ImVec2(x0, std::max(top, yAt(warn))), ImVec2(x1, b0.y), theme::withAlpha(P.meterLow, alpha));
      if (val > warn) dl->AddRectFilled(ImVec2(x0, std::max(top, yAt(zero))), ImVec2(x1, yAt(warn)), theme::withAlpha(P.meterMid, alpha));
      if (val > zero) dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, yAt(zero)), theme::withAlpha(P.meterHigh, alpha));
    };
    if (v.meterMode == MixerViewState::PeakRms) {
      bar(pk, 0.45f, a0.x, b0.x);  // peak as a dim body, RMS solid in front
      bar(rm, 1.0f, a0.x, b0.x);
    } else {
      bar(v0, 1.0f, a0.x, b0.x);
    }
    if (v.peakHold && hold > 0.01f) {
      const float hy = std::round(yAt(hold));
      dl->AddLine(ImVec2(a0.x, hy), ImVec2(b0.x, hy), hold > zero ? P.danger : P.text, 1);
    }
  }
  const float zy = std::round(p.y + size.y - zero * size.y);
  dl->AddLine(ImVec2(p.x - 2, zy), ImVec2(p.x, zy), P.textFaint);
  if (scale) {  // master: dB ticks on the right
    App& a = app();
    static const float dense[] = {6, 0, -6, -12, -18, -24, -36, -48}, sparse[] = {0, -12, -24, -48};
    const bool roomy = size.y >= 200;
    float lastY = -1e9f;
    for (float db : roomy ? std::vector<float>(dense, dense + 8) : std::vector<float>(sparse, sparse + 4)) {
      const float f = std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f);
      const float y = std::round(p.y + size.y - f * size.y);
      if (y - lastY < 11.0f) continue;  // J-38: labels never overlap on short meters
      lastY = y;
      dl->AddLine(ImVec2(p.x + size.x + 1, y), ImVec2(p.x + size.x + 4, y), P.textFaint);
      char b[8];
      std::snprintf(b, sizeof b, "%d", static_cast<int>(db));
      dl->AddText(a.fontMono, 9.0f, ImVec2(p.x + size.x + 5, y - 5), db == 0.0f ? P.textDim : P.textFaint, b);
    }
  }
}

// Post-fader waveform (scope tap) in a small sunken box.
void stripWave(App& a, ObjectId id, ImVec2 p, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.field, theme::kRadiusSm);
  auto rt = a.builder.mixerRuntime(id);
  if (!rt) return;
  static float buf[2048];
  const size_t n = 2048;
  if (!rt->tap.read(buf, n)) return;
  const int cols = std::max(1, static_cast<int>(size.x) - 2);
  const float mid = p.y + size.y * 0.5f, hh = size.y * 0.5f - 2;
  for (int c = 0; c < cols; ++c) {
    size_t i0 = static_cast<size_t>(c) * n / static_cast<size_t>(cols), i1 = static_cast<size_t>(c + 1) * n / static_cast<size_t>(cols);
    float lo = 0, hi = 0;
    for (size_t i = i0; i < i1; ++i) lo = std::min(lo, buf[i]), hi = std::max(hi, buf[i]);
    const float x = p.x + 1 + static_cast<float>(c);
    dl->AddLine(ImVec2(x, mid - std::min(1.0f, hi) * hh), ImVec2(x, mid - std::max(-1.0f, lo) * hh + 1), theme::withAlpha(P.meterLow, 0.85f));
  }
}

// Effect slot list on a strip (extended view): LED, name, mini mix knob.
void stripSlots(App& a, const MixerTrack& t, ImVec2 p, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool knobs = w >= 60;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kExtSlotRowH * kEffectSlots + 2), P.field, theme::kRadiusSm);
  for (int k = 0; k < kEffectSlots; ++k) {
    const EffectSlot& es = t.slots[static_cast<size_t>(k)];
    const PluginInfo* info = findPlugin(es.plugin.type);
    ImGui::PushID(1000 + k);
    const float y = p.y + 1 + kExtSlotRowH * static_cast<float>(k);
    const float nameW = w - (knobs ? 16.0f : 4.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, y));
    ImGui::InvisibleButton("xs", ImVec2(nameW, kExtSlotRowH));
    const bool hov = ImGui::IsItemHovered();
    // Q-8: wheel moves the effect. J-83: in a scrolling mixer only after a
    // click on the slot or with the wheel-adjust modifier (the plain wheel scrolls).
    const ui::WheelGate wg = ui::wheelGate(ui::wheelPaneScrolls() || keys::modifierExact("mixer.slotWheelMove"));
    if (wg.scale > 0) {
      if (!ui::wheelPaneScrolls()) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
      if (info && ImGui::GetIO().MouseWheel != 0) wheelSlot(a, t.id, k, ImGui::GetIO());
    }
    if (hov) dl->AddRectFilled(ImVec2(p.x + 1, y), ImVec2(p.x + nameW, y + kExtSlotRowH), P.surface700, 1);
    if (ImGui::IsItemClicked()) {
      a.selMixer = t.id;
      if (info) a.fxEditor = {t.id, k};
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##xslot");
    if (ImGui::BeginPopup("##xslot")) {
      mixerSlotMenu(a, t.id, k);
      ImGui::EndPopup();
    }
    if (info) {
      ui::Hint(info->name + "   (click: open \xC2\xB7 wheel: move up / down \xC2\xB7 drag: move, Ctrl copies \xC2\xB7 right-click: slot menu)");
      if (ImGui::BeginDragDropSource()) {
        SlotDrag sd{t.id, k};
        ImGui::SetDragDropPayload("GL_SLOT", &sd, sizeof sd);
        ui::DragPreview(ui::Icon::Mixer, ("Move " + info->name).c_str());
        ImGui::EndDragDropSource();
      }
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SLOT")) dropSlot(a, *static_cast<const SlotDrag*>(pl->Data), t.id, k);
      browserui::DropPlace place;  // J-97: effects only (instruments are refused with a reason), browser files too
      place.mixer = t.id;
      place.slot = k;
      browserui::acceptPluginDrop(a, browse::DropTarget::MixerSlot, place);
      browserui::acceptFileDrop(a, browse::DropTarget::MixerSlot, place);
      ImGui::EndDragDropTarget();
    }
    dl->AddCircleFilled(ImVec2(p.x + 5, y + kExtSlotRowH * 0.5f), 2.0f, info ? (es.enabled ? P.led : P.surface600) : P.surface800, 6);
    if (info)
      theme::textEllipsis(dl, a.fontRegular, 10.0f, ImVec2(p.x + 10, y + 1), nameW - 11, es.enabled ? P.text : P.textFaint, info->name.c_str());
    if (info && knobs) {
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - 14, y));
      ui::ParamKnob("xm", slotMixParam(t.id, k), 5.5f);
    }
    ImGui::PopID();
  }
}

// Shared look of the strips (selection flags, view options) for one frame.
struct StripCtx {
  const MixerTrack* focus = nullptr;  // focused track (routing arrows point from it)
  StripDims dims;
  float extraTop = 0;  // extended slot list + waveform height
  bool masterScale = true;  // J-38: the master's dB scale (dropped first in a very thin mixer)
};

void selectTrack(App& a, ObjectId id) {
  MixerViewState& v = a.mixerView;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl || io.KeySuper) {
    if (v.sel.empty() && a.selMixer != kNoId) v.sel.insert(a.selMixer);
    if (!v.sel.erase(id)) v.sel.insert(id);
  } else if (io.KeyShift && a.selMixer != kNoId) {
    const Song& s = a.doc.song();
    std::vector<size_t> order = mixops::displayOrder(s);
    int i0 = -1, i1 = -1;
    for (size_t k = 0; k < order.size(); ++k) {
      if (s.mixer[order[k]].id == a.selMixer) i0 = static_cast<int>(k);
      if (s.mixer[order[k]].id == id) i1 = static_cast<int>(k);
    }
    if (i0 >= 0 && i1 >= 0)
      for (int k = std::min(i0, i1); k <= std::max(i0, i1); ++k) v.sel.insert(s.mixer[order[static_cast<size_t>(k)]].id);
  } else {
    v.sel = {id};
  }
  a.selMixer = id;
  a.editFocus = App::EditFocus::Mixer;
}

void drawStrip(App& a, size_t i, ImVec2 p, float stripW, float stripH, const StripCtx& cx0) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const MixerTrack& t = s.mixer[i];
  const MixerViewState& v = a.mixerView;
  const StripDims& D = cx0.dims;
  const MixerTrack* sel = cx0.focus;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(static_cast<int>(t.id));
  canchor::publishRect(canchor::MixerStrip, t.id, 0, p, ImVec2(p.x + stripW, p.y + stripH));  // J-111d: collaborators' pointers
  const bool isSel = t.id == a.selMixer;
  const bool inSel = isSel || v.sel.count(t.id);
  const float cx = p.x + stripW * 0.5f;
  const ImU32 tc = theme::content(t.color);
  // Insert strip: slate body (lighter when selected) with a darker rim, a
  // colour-tinted name plate on top and a sunken meter / fader well.
  const ImU32 bodyC = inSel ? theme::mix(P.surface700, P.paper, 0.04f) : P.surface800;
  dl->AddRectFilled(p, ImVec2(p.x + stripW, p.y + stripH), theme::mix(bodyC, P.ink, 0.35f), theme::kRadius);
  dl->AddRectFilled(p, ImVec2(p.x + stripW, p.y + stripH - 1), bodyC, theme::kRadius);
  if (isSel)
    dl->AddRect(p, ImVec2(p.x + stripW, p.y + stripH), theme::withAlpha(P.hot, 0.55f), theme::kRadius);
  else if (inSel)
    dl->AddRect(p, ImVec2(p.x + stripW, p.y + stripH), theme::withAlpha(P.hot, 0.3f), theme::kRadius);
  if (ImU32 fc = 0; const float fl = collabui::flash(a, "mixer/#" + std::to_string(t.id), &fc))  // J-111: a collaborator's change
    dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + stripW + 1, p.y + stripH + 1), theme::withAlpha(fc, fl), theme::kRadius, 0, 2.0f);
  const float plateH = 36.0f;
  dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + stripW - 2, p.y + plateH), theme::mix(P.field, tc, 0.22f), theme::kRadiusSm);
  dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + stripW - 2, p.y + 4), tc, theme::kRadiusSm, ImDrawFlags_RoundCornersTop);

  // Header (click: select, Ctrl/Shift: multi-select, right-click: menu, drop target for effects).
  ImGui::SetCursorScreenPos(p);
  if (ImGui::InvisibleButton("hdr", ImVec2(stripW, plateH))) selectTrack(a, t.id);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && i > 0) {
    ObjectId id = t.id;
    menus::openPrompt("Rename mixer track", mixerTrackName(s, i), [&a, id](const std::string& n) {
      a.doc.editMixerTrack(id, "rename mixer track", [&](MixerTrack& m) { m.name = n; });
    });
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    if (!v.sel.count(t.id)) a.mixerView.sel = {t.id};
    a.selMixer = t.id;
    ImGui::OpenPopup("##stripmenu");
  }
  const ProgMixer* pm = progMixer(a, t.id);
  const int latOwn = pm ? pm->latOwn : 0;
  const float cpu = a.perf.insertTotal(t.id);
  {
    std::string h = mixerTrackName(s, i);
    char b[96];
    if (latOwn > 0) {
      std::snprintf(b, sizeof b, "   latency %d smp (%s)", latOwn, latencyLabel(latOwn, a.engine.sampleRate()).c_str());
      h += b;
    }
    if (cpu > 0) {
      std::snprintf(b, sizeof b, "   CPU %.1f%%", cpu * 100.0f);
      h += b;
    }
    if (t.disabled) h += "   (disabled: input passes through)";
    if (!t.group.empty()) h += "   group " + t.group;
    ui::Hint(h + "   (click: select \xC2\xB7 Ctrl/Shift: add to selection \xC2\xB7 double-click: rename \xC2\xB7 right-click: track menu \xC2\xB7 drop an effect here)");
  }
  if (ImGui::BeginDragDropTarget()) {
    {  // J-97: effects into the first free slot (instruments refused with a reason)
      browserui::DropPlace place;
      place.mixer = t.id;
      browserui::acceptPluginDrop(a, browse::DropTarget::MixerTrack, place);
    }
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SLOT")) {
      int k = freeSlot(t);
      if (k >= 0) dropSlot(a, *static_cast<const SlotDrag*>(pl->Data), t.id, k);
      else a.notify("All effect slots are in use", true);
    }
    {  // browser files (J-16): impulse responses, effect chains, effect presets
      browserui::DropPlace place;
      place.mixer = t.id;
      browserui::acceptFileDrop(a, browse::DropTarget::MixerTrack, place);
    }
    ImGui::EndDragDropTarget();
  }
  if (ImGui::BeginPopup("##stripmenu")) {
    mixerTrackMenu(a, i);
    ImGui::EndPopup();
  }
  std::string num = i == 0 ? "M" : std::to_string(i);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + 6, p.y + 6), isSel ? P.hot : P.textFaint, num.c_str());
  float numW = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, num.c_str()).x;
  if (t.icon > 0) trackicons::draw(dl, t.icon, ImVec2(std::round(p.x + 6 + numW + 9), p.y + 12), 11.0f, isSel ? P.hot : P.textDim);
  int fxCount = 0;
  for (const auto& sl : t.slots) fxCount += !sl.plugin.empty();
  // Right of the number: the track's own latency when it has some, else its effect count.
  float rightX = p.x + stripW - 6;
  if ((latOwn > 0 || fxCount) && (t.icon == 0 || stripW >= 60)) {
    std::string fx = latOwn > 0 ? latencyLabel(latOwn, a.engine.sampleRate()) : std::to_string(fxCount) + "fx";
    float fw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, fx.c_str()).x;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + stripW - fw - 6, p.y + 6), latOwn > 0 ? P.accent : P.textDim, fx.c_str());
    rightX = p.x + stripW - fw - 10;
  }
  if (i > 0 && t.input >= 0) {  // J-8: the track's audio input, after the number (when it fits)
    std::string in = "in" + (t.inputStereo ? std::to_string(t.input + 1) + "-" + std::to_string(t.input + 2) : std::to_string(t.input + 1));
    const float x0 = p.x + 6 + numW + 5 + (t.icon > 0 ? 16.0f : 0.0f);
    const float w = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, in.c_str()).x;
    if (x0 + w <= rightX) dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x0, p.y + 6), t.armed ? P.danger : P.textDim, in.c_str());
  }
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption, ImVec2(p.x + 6, p.y + 19), stripW - 10, isSel && P.dark ? P.paper : P.text,
                      mixerTrackName(s, i).c_str());
  // CPU bar under the name (insert + its effects). Square-root scale, full at
  // 10% of a core, so small loads still show.
  if (a.engine.profiling()) {
    const float x0 = p.x + 6, x1 = p.x + stripW - 6, y = p.y + plateH - 4;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 2), P.surface950, 1);
    if (cpu > 0)
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + std::max(1.0f, (x1 - x0) * std::sqrt(std::min(1.0f, cpu / 0.10f))), y + 2),
                        cpu >= 0.10f ? P.danger : P.textDim, 1);
  }

  // Mute lamp (green = on), disable switch (master too) and solo.
  const float lampY = p.y + plateH + 10;
  {
    ImGui::SetCursorScreenPos(ImVec2(p.x + 4, lampY - 8));
    if (ImGui::InvisibleButton("m", ImVec2(18, 16))) a.doc.setParam(mixerParam(t.id, MixerParam::Mute), t.muted ? 0 : 1, false);
    ui::Hint(t.muted ? "Track is muted: click to unmute" : "Track is on: click to mute");
    ui::Tooltip(t.muted ? "Unmute" : "Mute");
    float lvl = 0;
    if (auto rt = a.builder.mixerRuntime(t.id)) lvl = std::max(rt->peakL.load(), rt->peakR.load());
    ui::Led(dl, ImVec2(p.x + 12, lampY), 4.5f, !t.muted, std::min(1.0f, lvl * 2));
  }
  if (i > 0) {
    const ImVec2 bs(stripW >= 60 ? 20.0f : 16.0f, 14);
    ImGui::SetCursorScreenPos(ImVec2(p.x + stripW - bs.x - 4, lampY - bs.y * 0.5f));
    if (stripToggle("s", "S", t.solo, P.hot, bs, "Solo (keeps tracks routed to and from it)"))
      a.doc.editMixerTrack(t.id, "solo mixer track", [](MixerTrack& m) { m.solo = !m.solo; });
  }
  // Pan & stereo separation.
  const float kr = D.knobR;
  const float knobY = lampY + 10;
  ImGui::SetCursorScreenPos(ImVec2(std::round(cx - kr * 2 - 2), knobY));
  ui::ParamKnob("pan", mixerParam(t.id, MixerParam::Pan), kr, nullptr, true);
  ImGui::SetCursorScreenPos(ImVec2(std::round(cx + 2), knobY));
  ui::ParamKnob("sep", mixerParam(t.id, MixerParam::StereoSep), kr, nullptr, true);
  float y = knobY + kr * 2 + 8;
  // Extended view: effect slot list; waveform view.
  if (v.extended) {
    stripSlots(a, t, ImVec2(p.x + 3, y), stripW - 6);
    y += kExtSlotRowH * kEffectSlots + 6;
  }
  if (v.waveforms) {
    stripWave(a, t.id, ImVec2(p.x + 3, y), ImVec2(stripW - 6, kWaveH));
    y += kWaveH + 4;
  }

  // Meter pair + fader in a sunken well.
  const bool scale = i == 0 && v.masterScale && cx0.masterScale;
  const float fy = y + 2, bottomH = 44;
  const float fh = std::max(40.0f, p.y + stripH - fy - bottomH);
  float pl = 0, pr = 0, rl = 0, rr = 0;
  if (auto rt = a.builder.mixerRuntime(t.id)) {
    pl = rt->peakL.exchange(0);
    pr = rt->peakR.exchange(0);
    rl = rt->rmsL.exchange(0);
    rr = rt->rmsR.exchange(0);
  }
  const float meterW = D.meterW, faderW = D.faderW, groupW = meterW + 5 + faderW;
  const float gx = std::round(cx - groupW * 0.5f - (scale ? 8.0f : 0.0f));
  dl->AddRectFilled(ImVec2(gx - 3, fy - 4), ImVec2(gx + groupW + 3, fy + fh + 4), P.dark ? P.field : theme::mix(P.surface600, P.lineStrong, 0.4f), theme::kRadiusSm);
  if (scale) {  // master: fader left, meter right with its scale
    ImGui::SetCursorScreenPos(ImVec2(gx, fy));
    ui::ParamFader("fader", mixerParam(t.id, MixerParam::Volume), ImVec2(faderW, fh));
    ImGui::SetCursorScreenPos(ImVec2(gx + faderW + 5, fy));
    stripMeter(ImGui::GetID("meter"), pl, pr, rl, rr, ImVec2(meterW, fh), v, true);
  } else {
    ImGui::SetCursorScreenPos(ImVec2(gx, fy));
    stripMeter(ImGui::GetID("meter"), pl, pr, rl, rr, ImVec2(meterW, fh), v, false);
    ImGui::SetCursorScreenPos(ImVec2(gx + meterW + 5, fy));
    ui::ParamFader("fader", mixerParam(t.id, MixerParam::Volume), ImVec2(faderW, fh));
  }
  {
    double g = std::pow(t.volume / 0.8, 2.0);
    char buf[16];
    if (g < 1e-4)
      std::snprintf(buf, sizeof buf, "-inf");
    else
      std::snprintf(buf, sizeof buf, "%+.1f", 20 * std::log10(g));
    ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, buf);
    const bool armRow = i > 0 && (t.armed || t.input >= 0 || ImGui::IsMouseHoveringRect(p, ImVec2(p.x + stripW, p.y + stripH)));
    // With the record arm shown, the level moves right of the input name.
    const float vx = armRow ? std::round(p.x + stripW - ts.x - 5) : std::round(cx - ts.x * 0.5f);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(vx, fy + fh + 7), P.textDim, buf);
    if (armRow) stripArmButton(a, t, ImVec2(p.x + 3, fy + fh + 6));  // J-8: disk record
  }
  // Disabled: hatch over the well, "OFF" on the plate.
  if (t.disabled) {
    dl->AddRectFilled(ImVec2(p.x + 2, fy - 4), ImVec2(p.x + stripW - 2, fy + fh + 4), theme::withAlpha(P.ink, 0.45f));
    dl->PushClipRect(ImVec2(p.x + 2, fy - 4), ImVec2(p.x + stripW - 2, fy + fh + 4), true);
    for (float hx = p.x - fh; hx < p.x + stripW; hx += 7)
      dl->AddLine(ImVec2(hx, fy + fh + 4), ImVec2(hx + fh + 8, fy - 4), theme::withAlpha(P.textFaint, 0.25f));
    dl->PopClipRect();
    const char* off = "OFF";
    ImVec2 ts = a.fontBold->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, off);
    dl->AddRectFilled(ImVec2(cx - ts.x * 0.5f - 4, fy + fh * 0.5f - 8), ImVec2(cx + ts.x * 0.5f + 4, fy + fh * 0.5f + 8), P.surface900, 2);
    dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(std::round(cx - ts.x * 0.5f), std::round(fy + fh * 0.5f - ts.y * 0.5f)), P.danger, off);
  }
  // Route from the focused track to this one: arrow switch (lit when routed)
  // plus a send-level knob once routed; right-click for the send menu.
  if (sel && sel->id != t.id) {
    const Send* send = nullptr;
    for (const auto& sd : sel->sends)
      if (sd.to == t.id) send = &sd;
    const bool routed = send != nullptr;
    const std::string why = routed ? std::string() : mixops::routeProblem(s, sel->id, t.id);
    const bool possible = routed || why.empty();
    const float ry = p.y + stripH - 20;
    ImVec2 q(p.x + 4, ry);
    const float arrowW = routed ? stripW * 0.5f - 4 : stripW - 8;
    ImGui::SetCursorScreenPos(q);
    if (ImGui::InvisibleButton("route", ImVec2(arrowW, 16))) {
      if (possible) a.doc.setSend(sel->id, t.id, routed ? 0.0f : 1.0f);
      else a.notify(why, true);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##sendmenu");
    if (ImGui::BeginPopup("##sendmenu")) {
      mixerSendMenu(a, sel->id, t.id);
      ImGui::EndPopup();
    }
    bool hov = ImGui::IsItemHovered();
    bool stripHov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + stripW, p.y + stripH));
    const bool sc = routed && send->sidechain;
    ImU32 c = routed ? (sc ? P.accent : P.hot) : possible ? (hov ? P.text : P.textFaint) : P.surface600;
    if (routed || stripHov) ui::drawIcon(dl, ui::Icon::Up, ImVec2(std::round(q.x + arrowW * 0.5f), q.y + 8), 11, c);
    if (routed && (sc || send->preFader)) {
      const char* tag = sc ? "SC" : "PRE";
      dl->AddText(a.fontMono, 9.0f, ImVec2(q.x, q.y - 10), sc ? P.accent : P.textDim, tag);
    }
    std::string from = mixerTrackName(s, static_cast<size_t>(s.mixerIndex(sel->id)));
    ui::Hint(possible ? (routed ? (sc ? "Sidechain from " : "Send from ") + from + " to this track: click to remove \xC2\xB7 right-click: send options"
                                : "Send " + from + " to this track (right-click: route only / sidechain / pre-fader)")
                      : why);
    if (routed) {
      ParamDesc d;
      d.name = "Send level";
      d.min = 0.0, d.max = 1.25, d.def = 1.0;
      double val = send->level;
      bool released = false;
      ImGui::SetCursorScreenPos(ImVec2(std::round(p.x + stripW * 0.75f - 8), ry - 1));
      if (ui::Knob("sendlvl", &val, d, 8.0f, nullptr, false, &released)) a.doc.setSend(sel->id, t.id, std::max(0.0001f, static_cast<float>(val)));
      if (released) a.doc.endGesture();
    }
  }
  ImGui::PopID();
}

// Minimum strip height for the current view options.
float minStripHeight(const MixerViewState& v) {
  return 176.0f + (v.extended ? kExtSlotRowH * kEffectSlots + 6 : 0.0f) + (v.waveforms ? kWaveH + 4 : 0.0f);
}

// Routing overview: every track as a node, columns by route depth (master on
// the right), curves for sends (dashed: sidechain, dot: pre-fader).
void drawRoutingOverview(App& a) {
  MixerViewState& v = a.mixerView;
  if (!v.showRouting) return;
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(i18n::tr("Mixer routing###mixrouting"), &v.showRouting, ImGuiWindowFlags_NoDocking)) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Click a track to select it. Only tracks with routes or channels are shown."));
  mixops::RouteLayout L = mixops::routeLayout(s);
  // Tracks worth showing: master, tracks with channels, effects or routes other than the default.
  std::vector<bool> shown(s.mixer.size(), false);
  if (!shown.empty()) shown[0] = true;
  for (const auto& c : s.channels) {
    int k = s.mixerIndex(c.mixerTrack);
    if (k >= 0) shown[static_cast<size_t>(k)] = true;
  }
  for (const auto& e : L.edges)
    if (e.to != 0 || e.sidechain || e.preFader || e.level != 1.0f) shown[static_cast<size_t>(e.from)] = shown[static_cast<size_t>(e.to)] = true;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (freeSlot(s.mixer[i]) != 0 || s.mixer[i].id == a.selMixer) shown[i] = true;
  // Compact the columns that hold shown tracks.
  std::vector<int> colMap(static_cast<size_t>(L.columns), -1);
  int cols = 0;
  for (int c = 0; c < L.columns; ++c)
    for (size_t i = 0; i < s.mixer.size(); ++i)
      if (shown[i] && L.column[i] == c) {
        colMap[static_cast<size_t>(c)] = cols++;
        break;
      }
  const float nodeW = 128, nodeH = 24, colGap = 70, rowGap = 8;
  std::vector<int> rowOf(s.mixer.size(), 0), perCol(static_cast<size_t>(std::max(1, cols)), 0);
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (shown[i]) rowOf[i] = perCol[static_cast<size_t>(colMap[static_cast<size_t>(L.column[i])])]++;
  int maxRows = 1;
  for (int n : perCol) maxRows = std::max(maxRows, n);
  ImGui::BeginChild("##routes", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  auto nodePos = [&](size_t i) {
    const int c = colMap[static_cast<size_t>(L.column[i])];
    const float yy = i == 0 ? (static_cast<float>(maxRows) * (nodeH + rowGap) - nodeH) * 0.5f : static_cast<float>(rowOf[i]) * (nodeH + rowGap);
    return ImVec2(o.x + 8 + static_cast<float>(c) * (nodeW + colGap), o.y + 8 + yy);
  };
  // Edges under the nodes.
  for (const auto& e : L.edges) {
    const size_t f = static_cast<size_t>(e.from), t = static_cast<size_t>(e.to);
    if (!shown[f] || !shown[t]) continue;
    ImVec2 a0 = nodePos(f), b0 = nodePos(t);
    ImVec2 p0(a0.x + nodeW, a0.y + nodeH * 0.5f), p3(b0.x, b0.y + nodeH * 0.5f);
    const float dx = std::max(30.0f, (p3.x - p0.x) * 0.5f);
    const bool hot = s.mixer[f].id == a.selMixer || s.mixer[t].id == a.selMixer;
    const ImU32 col = e.sidechain ? P.accent : hot ? P.hot : theme::withAlpha(P.textDim, 0.8f);
    const float th = 1.0f + std::min(1.25f, e.level) * 1.2f;
    if (e.sidechain) {  // dashed
      ImVec2 prev = p0;
      for (int k = 1; k <= 24; ++k) {
        const float u = static_cast<float>(k) / 24.0f, w = 1 - u;
        ImVec2 q(w * w * w * p0.x + 3 * w * w * u * (p0.x + dx) + 3 * w * u * u * (p3.x - dx) + u * u * u * p3.x,
                 w * w * w * p0.y + 3 * w * w * u * p0.y + 3 * w * u * u * p3.y + u * u * u * p3.y);
        if (k % 2) dl->AddLine(prev, q, col, th);
        prev = q;
      }
    } else {
      dl->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p3.x - dx, p3.y), p3, col, th);
    }
    dl->AddTriangleFilled(ImVec2(p3.x, p3.y), ImVec2(p3.x - 6, p3.y - 4), ImVec2(p3.x - 6, p3.y + 4), col);
    if (e.preFader) dl->AddCircleFilled(ImVec2(p0.x + 5, p0.y), 3.0f, P.text, 8);
  }
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    if (!shown[i]) continue;
    const MixerTrack& t = s.mixer[i];
    ImVec2 q = nodePos(i);
    ImGui::SetCursorScreenPos(q);
    ImGui::PushID(static_cast<int>(t.id));
    if (ImGui::InvisibleButton("n", ImVec2(nodeW, nodeH))) selectTrack(a, t.id);
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      a.selMixer = t.id;
      ImGui::OpenPopup("##nodemenu");
    }
    if (ImGui::BeginPopup("##nodemenu")) {
      mixerTrackMenu(a, i);
      ImGui::EndPopup();
    }
    const bool selT = t.id == a.selMixer || v.sel.count(t.id);
    dl->AddRectFilled(q, ImVec2(q.x + nodeW, q.y + nodeH), selT ? P.surface600 : hov ? P.surface700 : P.surface800, theme::kRadius);
    dl->AddRectFilled(q, ImVec2(q.x + 4, q.y + nodeH), theme::rgb(t.color), theme::kRadius, ImDrawFlags_RoundCornersLeft);
    if (t.icon) trackicons::draw(dl, t.icon, ImVec2(q.x + 14, q.y + nodeH * 0.5f), 11, P.textDim);
    std::string label = (i == 0 ? std::string("M  ") : std::to_string(i) + "  ") + mixerTrackName(s, i);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + (t.icon ? 24.0f : 10.0f), q.y + 5), nodeW - 30, t.muted || t.disabled ? P.textFaint : P.text,
                        label.c_str());
    if (t.disabled) dl->AddText(a.fontMono, 9.0f, ImVec2(q.x + nodeW - 22, q.y + 7), P.danger, "OFF");
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(static_cast<float>(cols) * (nodeW + colGap) + 16, static_cast<float>(maxRows) * (nodeH + rowGap) + 16));
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace

// Tracks armed for disk recording (MixerTrack::armed, J-8), in mixer order.
static std::vector<ObjectId> armedTracks(const Song& s) {
  std::vector<ObjectId> out;
  for (const auto& m : s.mixer)
    if (m.armed) out.push_back(m.id);
  return out;
}

std::vector<ObjectId> mixerSelection(App& a) {
  const Song& s = a.doc.song();
  std::set<ObjectId> sel = a.mixerView.sel;
  if (a.selMixer != kNoId && !sel.count(a.selMixer)) sel = {a.selMixer};
  std::vector<ObjectId> out = mixops::inMixerOrder(s, sel);
  if (out.empty() && s.mixerTrack(a.selMixer)) out.push_back(a.selMixer);
  return out;
}

void renderMixerStems(App& a, const std::vector<ObjectId>& tracks) {
  if (tracks.empty()) {
    a.notify("No tracks to render: select or arm mixer tracks first", true);
    return;
  }
  std::vector<ObjectId> ids = tracks;
  std::string base = a.doc.song().info.title.empty() ? std::string("Stems") : a.doc.song().info.title;
  menus::fileDialog(a, "Render tracks to wave files (pick a folder and a base name)", true, {".wav"}, base,
                    [&a, ids](const std::string& path) {
                      captureHostedStates(a);
                      namespace fs = std::filesystem;
                      fs::path pth(path);
                      std::string dir = pth.parent_path().string(), stem = pth.stem().string();
                      std::error_code ec;
                      fs::create_directories(dir, ec);
                      const Song& song = a.doc.song();
                      const int sr = static_cast<int>(a.engine.sampleRate());
                      int files = 0;
                      for (const auto& job : mixops::stemJobs(song, ids, dir, stem)) {
                        RenderOptions o;
                        o.sampleRate = sr;
                        o.tail = song.barTicks();
                        AudioBuffer buf = renderSong(mixops::stemSong(song, job.track), o);
                        if (buf.frames == 0) continue;
                        if (writeWav(job.file, buf, sr, 24)) ++files;
                      }
                      a.notify(files ? "Rendered " + std::to_string(files) + " track(s) to " + dir : std::string("Nothing rendered: the song is empty"),
                               files == 0);
                    });
}

void mixerSendMenu(App& a, ObjectId from, ObjectId to) {
  const Song& s = a.doc.song();
  const MixerTrack* src = s.mixerTrack(from);
  if (!src || !s.mixerTrack(to)) return;
  const Send* sd = nullptr;
  for (const auto& x : src->sends)
    if (x.to == to) sd = &x;
  ImGui::TextDisabled("%s \xE2\x86\x92 %s", mixerTrackName(s, static_cast<size_t>(s.mixerIndex(from))).c_str(),
                      mixerTrackName(s, static_cast<size_t>(s.mixerIndex(to))).c_str());
  std::string err;
  auto go = [&](mixops::RouteMode m) {
    if (!mixops::route(a.doc, {from}, {to}, m, &err)) a.notify(err.empty() ? "Already routed that way" : err, !err.empty());
  };
  if (ImGui::MenuItem(i18n::tr("Route to this track"), nullptr, sd && !sd->sidechain)) go(mixops::RouteMode::Send);
  if (ImGui::MenuItem(i18n::tr("Route to this track only"))) go(mixops::RouteMode::Only);
  if (ImGui::MenuItem(i18n::tr("Sidechain to this track"), nullptr, sd && sd->sidechain)) go(mixops::RouteMode::Sidechain);
  if (ImGui::MenuItem(i18n::tr("Sidechain to this track only"))) go(mixops::RouteMode::SidechainOnly);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Pre-fader send"), nullptr, sd && sd->preFader, sd != nullptr)) mixops::setSendPreFader(a.doc, from, to, !sd->preFader);
  if (ImGui::MenuItem(i18n::tr("Remove route"), nullptr, false, sd != nullptr)) a.doc.setSend(from, to, 0.0f);
}

void mixerOptionsMenu(App& a) {
  MixerViewState& v = a.mixerView;
  if (ImGui::BeginMenu(i18n::tr("Disk recording"))) {
    if (ImGui::MenuItem(i18n::tr("Render armed tracks to wave files..."), keys::label("mixer.renderArmed"), false, !armedTracks(a.doc.song()).empty()))
      renderMixerStems(a, armedTracks(a.doc.song()));
    if (ImGui::MenuItem(i18n::tr("Render selected tracks to wave files..."), keys::label("mixer.renderSelected"))) renderMixerStems(a, mixerSelection(a));
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Arm selected tracks")))
      for (ObjectId id : mixerSelection(a))
        if (const MixerTrack* t = a.doc.song().mixerTrack(id); t && !t->armed) toggleArm(a, id);
    if (ImGui::MenuItem(i18n::tr("Disarm all tracks"), nullptr, false, !armedTracks(a.doc.song()).empty()))
      for (ObjectId id : armedTracks(a.doc.song())) toggleArm(a, id);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Plugin delay compensation"))) {
    bool pdc = a.builder.pdc();
    if (ImGui::MenuItem(i18n::tr("Automatic"), nullptr, pdc)) {
      a.builder.setPdc(!pdc);
      a.syncEngine();
    }
    if (ImGui::MenuItem(i18n::tr("Reset manual latency on all tracks"))) {
      int n = mixops::resetManualLatency(a.doc);
      a.notify(n ? "Reset manual latency on " + std::to_string(n) + " track(s)" : std::string("No track has a manual latency"));
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("View"))) {
    ImGui::MenuItem(i18n::tr("Waveforms"), keys::label("mixer.waveforms"), &v.waveforms);
    ImGui::MenuItem(i18n::tr("Routing overview"), nullptr, &v.showRouting);
    ImGui::MenuItem(i18n::tr("Effect slots on strips (extended)"), nullptr, &v.extended);
    ImGui::Separator();
    static const char* layouts[] = {i18n::N_("Compact strips"), i18n::N_("Normal strips"), i18n::N_("Wide strips")};
    for (int k = 0; k < 3; ++k)
      if (ImGui::MenuItem(i18n::tr(layouts[k]), nullptr, v.layout == k)) a.settings.set("ui.mixerLayout", k);
    if (ImGui::BeginMenu(i18n::tr("Rows of tracks"))) {
      for (int r = 1; r <= 4; ++r)
        if (ImGui::MenuItem((std::to_string(r) + " " + (r == 1 ? i18n::tr("row") : i18n::tr("rows"))).c_str(), nullptr, v.rows == r)) v.rows = r;
      ImGui::EndMenu();
    }
    ImGui::Separator();
    static const char* meters[] = {i18n::N_("Peak meters"), i18n::N_("RMS meters"), i18n::N_("Peak + RMS meters")};
    for (int k = 0; k < 3; ++k)
      if (ImGui::MenuItem(i18n::tr(meters[k]), nullptr, v.meterMode == k)) v.meterMode = k;
    ImGui::MenuItem(i18n::tr("Peak hold"), nullptr, &v.peakHold);
    ImGui::MenuItem(i18n::tr("Master meter scale"), nullptr, &v.masterScale);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Add tracks"))) {
    if (ImGui::MenuItem(i18n::tr("Add one"))) {
      ObjectId id = a.doc.addMixerTrack();
      if (id) a.selMixer = id;
    }
    if (ImGui::MenuItem(i18n::tr("Add multiple...")))
      menus::openPrompt("Add mixer tracks (how many?)", "4", [&a](const std::string& n) {
        int k = std::atoi(n.c_str());
        if (k > 0) mixops::insertTracks(a.doc, static_cast<int>(a.doc.song().mixer.size()), k);
      });
    if (ImGui::MenuItem(i18n::tr("Increase number of tracks to...")))
      menus::openPrompt("Number of insert tracks", std::to_string(a.doc.song().mixer.size() + 7), [&a](const std::string& n) {
        int added = mixops::growTo(a.doc, std::atoi(n.c_str()));
        a.notify(added ? "Added " + std::to_string(added) + " track(s)" : std::string("The mixer already has that many tracks"));
      });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Trim unused tracks"))) {
    int n = ops::trimUnusedMixerTracks(a.doc);
    a.notify(n ? "Removed " + std::to_string(n) + " unused track(s)" : std::string("No unused tracks at the end"));
  }
}

// Strip EQ at the bottom of the inspector, laid out after the reference
// mixer's EQ block: a band view (response curve + band handles) beside three
// vertical gain sliders, with frequency and bandwidth wheels under them.
static void stripEqPanel(App& a, const MixerTrack& t, float iw) {
  const theme::Palette& P = pal();
  ImGui::Dummy(ImVec2(0, theme::kS2));
  ui::Label("EQUALIZER");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static const char* bandNames[3] = {"Low", "Mid", "High"};
  static const uint32_t bandCol[3] = {0xFFBE5D, 0x7AE4A3, 0x80ACFF};
  auto desc = [](const char* name, const char* unit, double lo, double hi, double def, bool log) {
    ParamDesc d;
    d.name = name, d.unit = unit, d.min = lo, d.max = hi, d.def = def;
    d.scale = log ? ParamScale::Log : ParamScale::Linear;
    return d;
  };
  const ParamDesc gainD = desc("Gain", "dB", -18, 18, 0, false);
  const ParamDesc freqD = desc("Freq", "Hz", 20, 20000, 1000, true);
  const ParamDesc qD = desc("Q", "", 0.1, 10, kStripEqDefQ, true);
  auto edit = [&](size_t bi, int field, float nv) {
    const uint64_t key = (static_cast<uint64_t>(t.id) << 8) | static_cast<uint64_t>(0xE0 + bi);
    a.doc.editMixerTrack(t.id, "change strip EQ", [bi, field, nv](MixerTrack& m) {
      (field == 0 ? m.eqGain : field == 1 ? m.eqFreq : m.eqQ)[bi] = nv;
    }, key + static_cast<uint64_t>(field) * 3);
  };
  const float colW = 26.0f, slidersW = colW * 3, h = 88.0f;
  const float gw = std::max(80.0f, iw - slidersW - theme::kS2);
  ImVec2 p = ImGui::GetCursorScreenPos();
  // Band view.
  {
    ImVec2 q(p.x + gw, p.y + h);
    dl->AddRectFilled(p, q, P.field, theme::kRadiusSm);
    const float mid = p.y + h * 0.5f;
    for (double f : {100.0, 1000.0, 10000.0}) {
      float x = p.x + static_cast<float>(std::log(f / 20.0) / std::log(1000.0)) * gw;
      dl->AddLine(ImVec2(x, p.y + 1), ImVec2(x, q.y - 1), theme::withAlpha(P.lineStrong, 0.35f));
    }
    for (float db : {-12.0f, 12.0f}) {
      float y = std::round(mid - db / 18.0f * (h * 0.5f - 4));
      dl->AddLine(ImVec2(p.x + 1, y), ImVec2(q.x - 1, y), theme::withAlpha(P.lineStrong, 0.25f));
    }
    dl->AddLine(ImVec2(p.x + 1, mid), ImVec2(q.x - 1, mid), theme::withAlpha(P.lineStrong, 0.6f));
    StripEqSettings st;
    st.gain = t.eqGain;
    st.freq = t.eqFreq;
    st.q = t.eqQ;
    const int pts = std::max(16, static_cast<int>(gw / 3));
    std::vector<ImVec2> line;
    line.reserve(static_cast<size_t>(pts) + 1);
    for (int i = 0; i <= pts; ++i) {
      double f = 20.0 * std::pow(1000.0, static_cast<double>(i) / pts);
      double db = st.active() ? 20.0 * std::log10(std::max(1e-6, stripEqMagnitude(st, f, 48000.0))) : 0.0;
      float y = mid - static_cast<float>(std::clamp(db, -18.0, 18.0) / 18.0) * (h * 0.5f - 4);
      line.push_back(ImVec2(p.x + gw * static_cast<float>(i) / pts, y));
    }
    dl->PushClipRect(p, q, true);
    for (size_t i = 0; i + 1 < line.size(); ++i)  // soft fill between the curve and 0 dB
      dl->AddQuadFilled(line[i], line[i + 1], ImVec2(line[i + 1].x, mid), ImVec2(line[i].x, mid), theme::withAlpha(P.knobFill, 0.10f));
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), st.active() ? P.knobFill : P.textFaint, 0, 1.5f);
    for (int b = 0; b < 3; ++b) {  // band handles
      const size_t bi = static_cast<size_t>(b);
      float x = p.x + static_cast<float>(std::log(std::max(20.0f, t.eqFreq[bi]) / 20.0) / std::log(1000.0)) * gw;
      float y = mid - std::clamp(t.eqGain[bi], -18.0f, 18.0f) / 18.0f * (h * 0.5f - 4);
      dl->AddCircleFilled(ImVec2(x, y), 3.5f, theme::rgb(bandCol[b]), 12);
      dl->AddCircle(ImVec2(x, y), 3.5f, theme::withAlpha(P.ink, 0.6f), 12);
    }
    dl->PopClipRect();
    dl->AddRect(p, q, theme::withAlpha(P.ink, 0.4f), theme::kRadiusSm);
  }
  // Gain sliders (centre-zero rails, band-coloured fill), one per band.
  for (int b = 0; b < 3; ++b) {
    const size_t bi = static_cast<size_t>(b);
    ImGui::PushID(b);
    const float sx = p.x + gw + theme::kS2 + colW * static_cast<float>(b) + (colW - 10) * 0.5f;
    ImVec2 r0(sx, p.y), r1(sx + 10, p.y + h);
    ImGui::SetCursorScreenPos(ImVec2(sx - 6, p.y));
    ImGui::InvisibleButton("gain", ImVec2(22, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ui::WheelGate wg = ui::wheelGate();  // J-83: a scrolling mixer scrolls over the slider
    const float travel = h - 8;
    float g = t.eqGain[bi];
    if (act && wg.dragOk && ImGui::GetIO().MouseDelta.y != 0)
      edit(bi, 0, std::clamp(g - ImGui::GetIO().MouseDelta.y / travel * 36.0f * (ImGui::GetIO().KeyShift ? 0.2f : 1.0f), -18.0f, 18.0f));
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit(bi, 0, 0.0f);
    if (wg.scale > 0 && ImGui::GetIO().MouseWheel != 0) edit(bi, 0, std::clamp(g + ImGui::GetIO().MouseWheel * 0.5f * wg.scale, -18.0f, 18.0f));
    if (wg.hint) ui::Tooltip("Drag to change, Ctrl+wheel to fine-tune");
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    const float cxs = (r0.x + r1.x) * 0.5f, mid = p.y + h * 0.5f;
    const float y = mid - std::clamp(g, -18.0f, 18.0f) / 18.0f * (travel * 0.5f);
    dl->AddRectFilled(ImVec2(cxs - 2, p.y + 4), ImVec2(cxs + 2, p.y + h - 4), P.field, 2);
    const ImU32 fc = act ? theme::rgb(0xFB7962) : theme::rgb(bandCol[b]);
    dl->AddRectFilled(ImVec2(cxs - 2, std::min(mid, y)), ImVec2(cxs + 2, std::max(mid, y)), fc, 2);
    dl->AddLine(ImVec2(cxs - 6, mid), ImVec2(cxs + 6, mid), theme::withAlpha(P.textFaint, 0.6f));
    dl->AddRectFilled(ImVec2(cxs - 6, y - 3), ImVec2(cxs + 6, y + 3), hov || act ? P.knobFill : theme::mix(P.textDim, P.surface600, 0.3f), 2);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s gain: %+.1f dB", bandNames[b], static_cast<double>(g));
    if (hov || act) a.setHint(std::string(buf) + "   (drag \xC2\xB7 Shift fine \xC2\xB7 double-click reset)");
    if (act) ImGui::SetTooltip(i18n::tr("%+.1f dB"), static_cast<double>(g));
    ImGui::PopID();
  }
  // Frequency and bandwidth wheels under the sliders; values under the band view.
  const float wy = p.y + h + 6;
  for (int b = 0; b < 3; ++b) {
    const size_t bi = static_cast<size_t>(b);
    ImGui::PushID(b);
    const float kx = p.x + gw + theme::kS2 + colW * static_cast<float>(b) + (colW - 18) * 0.5f;
    auto knob = [&](const char* id, const ParamDesc& d, float cur, float y, int field) {
      ImGui::SetCursorScreenPos(ImVec2(kx, y));
      double v = cur;
      bool released = false;
      if (ui::Knob(id, &v, d, 9.0f, nullptr, false, &released)) edit(bi, field, static_cast<float>(d.clamp(v)));
      if (released) a.doc.endGesture();
    };
    ParamDesc fd = freqD;
    fd.def = kStripEqDefFreq[bi];
    fd.name = std::string(bandNames[b]) + " frequency";
    ParamDesc bd = qD;
    bd.name = std::string(bandNames[b]) + " bandwidth (Q)";
    knob("f", fd, t.eqFreq[bi], wy, 1);
    knob("q", bd, t.eqQ[bi], wy + 22, 2);
    ImGui::PopID();
  }
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + gw - 34, wy + 4), P.textFaint, "FREQ");
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + gw - 34, wy + 26), P.textFaint, "BW");
  for (int b = 0; b < 3; ++b) {
    const size_t bi = static_cast<size_t>(b);
    char buf[40];
    float fz = t.eqFreq[bi];
    if (fz >= 1000.0f)
      std::snprintf(buf, sizeof buf, "%+.1f  %.1fk", static_cast<double>(t.eqGain[bi]), static_cast<double>(fz) / 1000.0);
    else
      std::snprintf(buf, sizeof buf, "%+.1f  %.0f", static_cast<double>(t.eqGain[bi]), static_cast<double>(fz));
    const float ly = wy + static_cast<float>(b) * 14.0f;
    dl->AddCircleFilled(ImVec2(p.x + 4, ly + 6), 2.5f, theme::rgb(bandCol[b]), 8);
    theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(p.x + 11, ly), gw - 50, P.textDim, buf);
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, wy + 46));
  ImGui::Dummy(ImVec2(iw, 0));
}

// Mixer toolbar: options menu, layout, rows, extended view, waveforms,
// meter mode, peak hold, routing overview, selection summary.
static void mixerToolbar(App& a) {
  MixerViewState& v = a.mixerView;
  const auto& P = pal();
  ui::BeginToolbar("##mixtb");
  menuButton("mixmenu", "Mixer menu: disk recording, delay compensation, view, tracks", [&a]() { mixerOptionsMenu(a); });
  ImGui::SameLine(0, 6);
  // J-28: as a thin column the strips are compact and wrap by themselves;
  // strip width and rows stay in the mixer menu.
  const bool column = ImGui::GetWindowWidth() < mixlayout::kColumnBelow;
  // J-38: in a thin column the toolbar drops what doesn't fit (meters / hold
  // stay in the mixer menu) instead of clipping it at the edge.
  auto room = [] { return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetCursorScreenPos().x - ImGui::GetStyle().WindowPadding.x; };
  if (!column) {
    static const char* layouts[] = {i18n::N_("Compact"), i18n::N_("Normal"), i18n::N_("Wide")};
    static const char* layoutHints[] = {"Compact strips", "Normal strips", "Wide strips"};
    if (ui::Segmented("##layout", layouts, 3, &v.layout, 0, 0, layoutHints)) a.settings.set("ui.mixerLayout", v.layout);
    ui::Hint("Strip width: compact, wide or extra large");
    ImGui::SameLine(0, 6);
    static const char* rows[] = {"1", "2", "3", "4"};
    int r = v.rows - 1;
    if (ui::Segmented("##rows", rows, 4, &r, 22)) v.rows = r + 1;
    ui::Hint("Rows of tracks: inserts wrap into this many rows");
    ImGui::SameLine(0, 6);
    ui::ToolbarDivider();
    ImGui::SameLine(0, 6);
  }
  if (ui::TextButton("Slots", v.extended, 0, "Show every effect slot (name and mix) on the strips")) v.extended = !v.extended;
  ImGui::SameLine(0, 4);
  if (ui::ToggleIcon("wave", ui::Icon::Wave, v.waveforms, ImGui::GetFrameHeight(), "Waveforms on the strips (Alt+W)")) v.waveforms = !v.waveforms;
  static const char* meters[] = {i18n::N_("Peak"), i18n::N_("RMS"), i18n::N_("Both")};
  const ImGuiStyle& sty = ImGui::GetStyle();
  float meterSegW = 0;
  float meterMax = 0;  // Segmented makes every segment as wide as the widest (translated) label
  for (const char* m : meters) meterMax = std::max(meterMax, ImGui::CalcTextSize(i18n::tr(m)).x + sty.FramePadding.x * 2);
  meterSegW = meterMax * 3 + 4;
  const float holdW = ImGui::CalcTextSize(i18n::tr("Hold")).x + sty.FramePadding.x * 2 + 2,
              routeW = ImGui::CalcTextSize(i18n::tr("Routing")).x + sty.FramePadding.x * 2 + 18;
  ImGui::SameLine(0, 6);
  // J-46: decided once from the room at the first optional group (the routing
  // check used to run after "Hold" had ended the line, where the cursor is
  // back at the left edge, so "Routing" was always drawn and clipped).
  mixlayout::ToolbarFit fit{true, true, true};
  if (column) fit = mixlayout::toolbarFit(room(), meterSegW, 4 + holdW, 12 + routeW);
  if (fit.meters) {
    ui::Segmented("##meter", meters, 3, &v.meterMode, 0, 0, nullptr);
    ui::Hint("Meters: peak, RMS (average level) or both");
    ImGui::SameLine(0, 4);
    if (fit.hold && ui::TextButton("Hold", v.peakHold, 0, "Meters hold their peaks for a moment")) v.peakHold = !v.peakHold;
  }
  if (fit.routing) {
    ImGui::SameLine(0, 6);
    ui::ToolbarDivider();
    ImGui::SameLine(0, 6);
    if (ui::TextButton("Routing", v.showRouting, 0, "Routing overview: every route between the tracks")) v.showRouting = !v.showRouting;
  }
  const auto selIds = mixerSelection(a);
  // Status texts after the buttons: dropped in a thin column when they would clip.
  auto status = [&](ImU32 col, const std::string& t) {
    ImGui::SameLine(0, 10);
    if (column && room() < ImGui::CalcTextSize(t.c_str()).x) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(col), "%s", t.c_str());
  };
  if (selIds.size() > 1) status(P.textDim, std::to_string(selIds.size()) + " tracks selected");
  if (const size_t armed = armedTracks(a.doc.song()).size()) status(P.danger, std::to_string(armed) + " armed");
  ui::EndToolbar();
}

void drawMixer(App& a) {
  if (!a.showMixer) {
    drawRoutingOverview(a);
    return;
  }
  if (!ui::BeginDockWindow("Mixer", &a.showMixer, ImVec2(1000, 400))) {
    ui::EndDockWindow();
    drawRoutingOverview(a);
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  MixerViewState& v = a.mixerView;
  if (!s.mixerTrack(a.selMixer) && !s.mixer.empty()) a.selMixer = s.masterId();
  for (auto it = v.sel.begin(); it != v.sel.end();) it = s.mixerTrack(*it) ? std::next(it) : v.sel.erase(it);
  if (!v.layoutOverride) v.layout = std::clamp(static_cast<int>(a.settings.getNumber("ui.mixerLayout")), 0, 2);  // View > Mixer layout
  const MixerTrack* sel = s.mixerTrack(a.selMixer);
  // Mixer shortcuts while the window has focus.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
    if (keys::pressed("mixer.renderArmed")) renderMixerStems(a, armedTracks(s));
    if (keys::pressed("mixer.renderSelected")) renderMixerStems(a, mixerSelection(a));
    if (keys::pressed("mixer.waveforms")) v.waveforms = !v.waveforms;
  }
  mixerToolbar(a);
  const float W = ImGui::GetContentRegionAvail().x, Hm = ImGui::GetContentRegionAvail().y;
  // J-28: docked as a thin column (the default workspace) the mixer stacks:
  // compact strips wrap into as many rows as needed (vertical scrolling, every
  // strip whole and readable) and the inspector sits below them when the
  // column is tall enough.
  // J-38: the thin layout comes from mixlayout::compute (core/MixerLayout.h):
  // strips flow into rows that fill the width (master included), rows keep a
  // usable fader height (scrolling rather than squashing), the inspector stacks
  // below when there is room for it.
  const bool column = W < mixlayout::kColumnBelow;
  const bool groups = !mixops::groupNames(s).empty();
  const std::vector<size_t> order = mixops::displayOrder(s);
  mixlayout::Layout ml;
  if (column) {
    mixlayout::Input in;
    in.width = W, in.height = Hm;
    in.strips = static_cast<int>(order.size());
    in.masterScaleW = v.masterScale ? 16.0f : 0.0f;
    in.masterExtra = 8.0f + in.masterScaleW;
    in.minStripH = minStripHeight(v);
    in.bandH = groups ? 14.0f : 0.0f;
    in.padding = theme::kS2;
    ml = mixlayout::compute(in);
  }
  // J-83: the thin mixer scrolls with the wheel; its faders, knobs and slots
  // take the wheel only after a click or with Ctrl (Settings > Interface).
  // Read every frame: resizing across the breakpoint flips it at once.
  const bool wheelScrolls = wheelroute::paneScrolls(wheelroute::modeFromSetting(a.settings.getNumber("ui.mixerWheel")), column);
  gMixerWheelScrolls = wheelScrolls;
  ui::WheelPolicyScope wheelScope(wheelScrolls);
  const bool showInspector = !column || ml.showInspector;
  const float inspectorW = std::clamp(W * 0.27f, 240.0f, 320.0f);
  const float inspectorH = column ? ml.inspectorH : 0.0f;

  // ---------------- strips ----------------
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS2, theme::kS2));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.dark ? P.well : P.surface900));
  ImGui::BeginChild("##strips", ImVec2(showInspector && !column ? -inspectorW : 0, showInspector && column ? -inspectorH : 0),
                    ImGuiChildFlags_AlwaysUseWindowPadding, column ? 0 : ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  gMixerStripsScroll = ImGui::GetScrollY();
  {
    StripCtx ctx;
    ctx.focus = sel;
    ctx.dims = stripDims(column ? static_cast<int>(ml.stripW >= 66.0f ? MixerViewState::Wide : MixerViewState::Compact) : v.layout);
    ctx.masterScale = !column || ml.masterScale;
    const float stripW = ctx.dims.w, masterW = stripW + 8 + (v.masterScale ? 16.0f : 0.0f), gap = 2.0f;
    const float bandH = groups ? 14.0f : 0.0f;
    const int rows = std::clamp(v.rows, 1, 4);
    if (column) {  // J-38: flowed rows (see above)
      const ImVec2 o = ImGui::GetCursorScreenPos();
      std::vector<size_t> flow;
      for (size_t i : order)
        if (i == 0 || std::clamp(s.mixer[i].dock, -1, 1) < 0) flow.push_back(i);  // left dock first (master leads)
      for (size_t i : order)
        if (i != 0 && std::clamp(s.mixer[i].dock, -1, 1) >= 0) flow.push_back(i);
      const bool cull = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetDragDropPayload();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (size_t k = 0; k < flow.size() && k < ml.cells.size(); ++k) {
        const mixlayout::Cell& c = ml.cells[k];
        const ImVec2 p(o.x + c.x, o.y + static_cast<float>(c.row) * ml.rowPitch + bandH);
        const MixerTrack& t = s.mixer[flow[k]];
        if (groups && !t.group.empty()) {  // group band: a rule above the strip, the label where a run starts
          const ImU32 gc = theme::mix(P.textDim, theme::rgb(t.color), 0.5f);
          dl->AddRectFilled(ImVec2(p.x, p.y - bandH + 9), ImVec2(p.x + c.w + gap, p.y - bandH + 11), gc);
          const bool startRun = k == 0 || s.mixer[flow[k - 1]].group != t.group || ml.cells[k - 1].row != c.row;
          if (startRun) {
            const ImVec2 ts = a.fontMono->CalcTextSizeA(9.0f, 1e9f, 0, t.group.c_str());
            dl->AddRectFilled(ImVec2(p.x, p.y - bandH), ImVec2(p.x + std::min(ts.x + 8, c.w), p.y - bandH + 11), P.surface800, 2);
            dl->PushClipRect(ImVec2(p.x, p.y - bandH), ImVec2(p.x + c.w, p.y), true);
            dl->AddText(a.fontMono, 9.0f, ImVec2(p.x + 4, p.y - bandH), P.text, t.group.c_str());
            dl->PopClipRect();
          }
        }
        if (!cull || ImGui::IsRectVisible(p, ImVec2(p.x + c.w, p.y + ml.rowH))) drawStrip(a, flow[k], p, c.w, ml.rowH, ctx);
      }
      ImGui::SetCursorScreenPos(ImVec2(o.x + ml.addX + 4, o.y + static_cast<float>(ml.addRow) * ml.rowPitch + bandH));
      if (ui::IconButton("addtrack", ui::Icon::Plus, false, 28, "Add a mixer track")) {
        ObjectId id = a.doc.addMixerTrack();
        if (id) a.selMixer = id;
      }
      ImGui::SetCursorScreenPos(o);
      const int rowsAll = std::max(ml.rows, ml.addRow + 1);
      ImGui::Dummy(ImVec2(ml.contentW, static_cast<float>(rowsAll) * ml.rowPitch - mixlayout::kRowGap));
    } else {
    const float avail = ImGui::GetContentRegionAvail().y - (ImGui::GetScrollMaxX() > 0 ? 8.0f : 0.0f);
    const float rowH = column ? minStripHeight(v)
                              : std::max(minStripHeight(v), (avail - static_cast<float>(rows) * bandH - static_cast<float>(rows - 1) * 6.0f) / static_cast<float>(rows));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Sections: left dock (master first), middle (wraps into rows), right dock.
    std::vector<size_t> left, middle, right;
    for (size_t i : mixops::displayOrder(s)) {
      const int dock = i == 0 ? -1 : std::clamp(s.mixer[i].dock, -1, 1);
      (dock < 0 ? left : dock > 0 && !column ? right : middle).push_back(i);
    }
    struct Placed {
      size_t i;
      ImVec2 p;
      float w;
    };
    std::vector<Placed> placed;
    float x = o.x;
    for (size_t k = 0; k < left.size(); ++k) {
      const size_t i = left[k];
      const float w = i == 0 ? masterW : stripW;
      placed.push_back({i, ImVec2(x, o.y + bandH), w});
      x += w + gap;
    }
    if (!left.empty()) {  // divider after the left dock
      x += theme::kS3 - gap;
      dl->AddLine(ImVec2(x - theme::kS3 * 0.5f - 0.5f, o.y + bandH + 8), ImVec2(x - theme::kS3 * 0.5f - 0.5f, o.y + bandH + rowH - 8), P.line);
    }
    const size_t perRow = (middle.size() + static_cast<size_t>(rows) - 1) / static_cast<size_t>(rows);
    const float midX = x;
    float midEnd = midX;
    for (size_t k = 0; k < middle.size(); ++k) {
      const size_t row = perRow ? k / perRow : 0, col = perRow ? k % perRow : 0;
      ImVec2 p(midX + static_cast<float>(col) * (stripW + gap), o.y + static_cast<float>(row) * (rowH + bandH + 6.0f) + bandH);
      placed.push_back({middle[k], p, stripW});
      midEnd = std::max(midEnd, p.x + stripW);
    }
    x = midEnd + gap;
    // Add-track button after the middle section.
    ImGui::SetCursorScreenPos(ImVec2(x + theme::kS1, o.y + bandH));
    if (ui::IconButton("addtrack", ui::Icon::Plus, false, 28, "Add a mixer track")) {
      ObjectId id = a.doc.addMixerTrack();
      if (id) a.selMixer = id;
    }
    x += 40;
    if (!right.empty()) {
      dl->AddLine(ImVec2(x - 6.5f, o.y + bandH + 8), ImVec2(x - 6.5f, o.y + bandH + rowH - 8), P.line);
      for (size_t i : right) {
        placed.push_back({i, ImVec2(x, o.y + bandH), stripW});
        x += stripW + gap;
      }
    }
    // Group bands above the strips: one label per run of neighbours.
    if (groups)
      for (size_t k = 0; k < placed.size(); ++k) {
        const MixerTrack& t = s.mixer[placed[k].i];
        if (t.group.empty()) continue;
        const bool startRun = k == 0 || s.mixer[placed[k - 1].i].group != t.group || placed[k - 1].p.y != placed[k].p.y;
        const ImVec2 p(placed[k].p.x, placed[k].p.y - bandH);
        const ImU32 gc = theme::mix(P.textDim, theme::rgb(t.color), 0.5f);
        dl->AddRectFilled(ImVec2(p.x, p.y + 9), ImVec2(p.x + placed[k].w + gap, p.y + 11), gc);
        if (startRun) {
          ImVec2 ts = a.fontMono->CalcTextSizeA(9.0f, 1e9f, 0, t.group.c_str());
          dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ts.x + 8, p.y + 11), P.surface800, 2);
          dl->AddText(a.fontMono, 9.0f, ImVec2(p.x + 4, p.y), P.text, t.group.c_str());
        }
      }
    // Strips scrolled out of view are skipped (hundreds of inserts: J-25) unless
    // a menu is open (it may belong to one of them) or a drag is under way.
    const bool cull = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetDragDropPayload();
    for (const auto& pl : placed)
      if (!cull || ImGui::IsRectVisible(pl.p, ImVec2(pl.p.x + pl.w, pl.p.y + rowH))) drawStrip(a, pl.i, pl.p, pl.w, rowH, ctx);
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(x - o.x + 8, static_cast<float>(rows) * (rowH + bandH + 6.0f) - 6.0f));
    }
  }
  ImGui::EndChild();

  // ---------------- inspector ----------------
  if (showInspector) {
    if (!column) ImGui::SameLine(0, 0);
    ImVec2 ip = ImGui::GetCursorScreenPos();
    if (column)
      ImGui::GetWindowDrawList()->AddLine(ip, ImVec2(ip.x + ImGui::GetContentRegionAvail().x, ip.y), P.line);
    else
      ImGui::GetWindowDrawList()->AddLine(ip, ImVec2(ip.x, ip.y + ImGui::GetContentRegionAvail().y), P.line);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS1, theme::kS1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    ImGui::BeginChild("##inspector", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    if (sel) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      size_t si = static_cast<size_t>(s.mixerIndex(sel->id));
      const float iw = ImGui::GetContentRegionAvail().x;
      // Header: colour, icon, name, role.
      {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + 20), theme::rgb(sel->color), 2);
        float nx = p.x + 12;
        if (sel->icon) {
          trackicons::draw(dl, sel->icon, ImVec2(p.x + 20, p.y + 11), 14, P.text);
          nx += 20;
        }
        std::string role = si == 0 ? "Master" : "Insert " + std::to_string(si);
        if (role == mixerTrackName(s, si)) role.clear();  // don't repeat the default name
        float rw = role.empty() ? 0.0f : a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, role.c_str()).x;
        theme::textEllipsis(dl, a.fontBold, theme::kFontTitle, ImVec2(nx, p.y + 2), iw - rw - (nx - p.x) - 12, P.text, mixerTrackName(s, si).c_str());
        if (!role.empty()) dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + iw - rw, p.y + 6), P.textFaint, role.c_str());
        ImGui::Dummy(ImVec2(iw, 22));
      }
      // Latency (own effects, manual offset, input) and CPU.
      const ProgMixer* pm = progMixer(a, sel->id);
      {
        const double sr = a.engine.sampleRate();
        std::string line;
        if (pm && pm->latOwn > 0)
          line = "Latency " + std::to_string(pm->latOwn) + " smp (" + latencyLabel(pm->latOwn, sr) + ")";
        else
          line = "No latency";
        if (pm && pm->latIn > 0) line += " \xC2\xB7 input " + latencyLabel(pm->latIn, sr);
        if (pm && !a.builder.pdc()) line += " \xC2\xB7 compensation off";
        if (a.engine.profiling()) {
          char b[32];
          std::snprintf(b, sizeof b, " \xC2\xB7 CPU %.1f%%", a.perf.insertTotal(sel->id) * 100.0f);
          line += b;
        }
        ImGui::PushFont(a.fontMono, theme::kFontLabel);
        ImGui::TextColored(theme::vec(P.textDim), "%s", line.c_str());
        ImGui::PopFont();
        ui::Hint("Latency this track's effects (and its manual setting) add, and how late its input arrives (delay compensation lines up every path)");
      }
      ImGui::Dummy(ImVec2(0, theme::kS1));
      mixerInputPanel(a, *sel, iw);  // J-8: audio input, monitoring, arm
      ui::Label("EFFECTS");
      const float rowH = 24.0f;
      // Q-8: the mouse wheel over an effect's slot moves the effect up / down
      // (the reference: plain wheel, the pointer then follows the effect, so
      // the next notches keep moving it; here the moved effect stays the
      // wheel's target while the pointer rests). Rows glide to their new place.
      ImGuiIO& wio = ImGui::GetIO();
      // J-83: in a scrolling mixer the slot list scrolls with the plain wheel;
      // a slot moves only after a click on it (wheel focus) or with Ctrl, and
      // a running pane scroll never moves one (see core/WheelRoute.h).
      const bool thinWheel = ui::wheelPaneScrolls();
      const bool wheelMove = wio.MouseWheel != 0 && (thinWheel ? !ui::wheelScrolling() : keys::modifierExact("mixer.slotWheelMove"));
      bool wheelDone = false;
      if (wheelMove && (!thinWheel || ui::wheelModifierHeld() || ui::wheelFocusActive()) && slotWheel.track == sel->id && slotWheel.slot >= 0 && ImGui::GetTime() - slotWheel.time < 2.0 &&
          std::fabs(wio.MousePos.x - slotWheel.at.x) + std::fabs(wio.MousePos.y - slotWheel.at.y) < 3.0f &&
          ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        wheelSlot(a, sel->id, slotWheel.slot, wio);
        wheelDone = true;
        if (thinWheel) ui::wheelClaim();
      }
      for (int k = 0; k < kEffectSlots; ++k) {
        const EffectSlot& es = sel->slots[static_cast<size_t>(k)];
        ImGui::PushID(k);
        const PluginInfo* info = findPlugin(es.plugin.type);
        const ImVec2 rowBase = ImGui::GetCursorScreenPos();
        ImVec2 q(rowBase.x, rowBase.y + slotGlideOffset(sel->id, k) * (rowH + 2));
        ImGui::SetCursorScreenPos(q);
        // Slot number + enable LED.
        char nb[4];
        std::snprintf(nb, sizeof nb, "%d", k + 1);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q.x, std::round(rowBase.y + (rowH - theme::kFontLabel) * 0.5f)), info ? P.textDim : P.textFaint,
                    nb);  // the slot number stays: only the effect glides
        ImGui::SetCursorScreenPos(ImVec2(q.x + 12, q.y));
        if (ImGui::InvisibleButton("en", ImVec2(16, rowH)) && info) ops::setSlotEnabled(a.doc, sel->id, k, !es.enabled);
        if (info && ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##enmenu");
        if (ImGui::BeginPopup("##enmenu")) {  // the slot's on/off switch: automation and remote links
          knobContextMenu(a, slotEnableParam(sel->id, k));
          ImGui::EndPopup();
        }
        ui::Hint(info ? (es.enabled ? "Bypass this effect (right-click: link the switch to a controller)" : "Enable this effect") : "Empty slot");
        ui::Led(dl, ImVec2(q.x + 20, q.y + rowH * 0.5f), 4.0f, info && es.enabled);
        // Slot button: opens the effect list.
        const float tail = info ? 20 + 4 + 22 + 4 : 0;
        const float bw = iw - 32 - tail;
        ImVec2 b0(q.x + 32, q.y + 1);
        ImGui::SetCursorScreenPos(b0);
        ImGui::InvisibleButton("slot", ImVec2(bw, rowH - 2));
        bool hov = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##fxlist");
        const ui::WheelGate wg = ui::wheelGate(thinWheel || keys::modifierExact("mixer.slotWheelMove"));
        if (wg.scale > 0) {
          if (!thinWheel) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the panel does not scroll under a slot (the gate claims it when thin)
          if (wheelMove && !wheelDone && info) {
            wheelSlot(a, sel->id, k, wio);
            wheelDone = true;
          }
        }
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##fxlist");
        if (info && hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.fxEditor = {sel->id, k};
        const ImU32 slotBg = info ? theme::mix(P.field, P.surface800, 0.35f) : P.field;
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + rowH - 2), open || hov ? theme::mix(slotBg, P.surface700, 0.6f) : slotBg, theme::kRadiusSm);
        dl->AddRect(b0, ImVec2(b0.x + bw, b0.y + rowH - 2), theme::withAlpha(P.ink, 0.35f), theme::kRadiusSm);
        // Latency this slot adds, right-aligned before the chevron.
        float latW = 0;
        if (info && pm && static_cast<size_t>(k) < pm->slots.size() && pm->slots[static_cast<size_t>(k)].latency > 0) {
          std::string ls = latencyLabel(pm->slots[static_cast<size_t>(k)].latency, a.engine.sampleRate());
          latW = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, ls.c_str()).x + 6;
          dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(b0.x + bw - 20 - latW + 6, std::round(b0.y + (rowH - 2 - theme::kFontLabel) * 0.5f)),
                      P.accent, ls.c_str());
        }
        theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(b0.x + 8, std::round(b0.y + (rowH - 2 - 13) * 0.5f) - 1), bw - 26 - latW,
                            info ? (es.enabled ? P.text : P.textFaint) : P.textFaint, info ? info->name.c_str() : i18n::tr("Empty slot"));
        ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(b0.x + bw - 11, b0.y + (rowH - 2) * 0.5f), 10, P.textFaint);
        if (float pr = 0; info && collabui::fileLoading(a, es.plugin.data, &pr))  // J-111: an impulse response still arriving
          collabui::drawLoading(dl, ImVec2(b0.x + 2, b0.y + rowH - 6), ImVec2(b0.x + bw - 2, b0.y + rowH - 3), pr, true);
        ui::Hint(info ? "Click to replace \xC2\xB7 double-click to open \xC2\xB7 right-click for options \xC2\xB7 wheel moves it up / down \xC2\xB7 drag to reorder or onto another track (Ctrl copies)"
                      : "Click to insert an effect (or drop one from the browser)");
        ImGui::SetNextWindowSizeConstraints(ImVec2(bw, 0), ImVec2(320, 460));
        if (ImGui::BeginPopup("##fxlist")) {
          effectList(a, sel->id, k, es.plugin.type);
          ImGui::EndPopup();
        }
        if (ImGui::BeginPopupContextItem("##slotmenu")) {
          mixerSlotMenu(a, sel->id, k);
          ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_SLOT")) dropSlot(a, *static_cast<const SlotDrag*>(pl->Data), sel->id, k);
          {  // browser files (J-16); plugins: effects only (J-97)
            browserui::DropPlace place;
            place.mixer = sel->id;
            place.slot = k;
            browserui::acceptPluginDrop(a, browse::DropTarget::MixerSlot, place);
            browserui::acceptFileDrop(a, browse::DropTarget::MixerSlot, place);
          }
          ImGui::EndDragDropTarget();
        }
        if (info && ImGui::BeginDragDropSource()) {
          SlotDrag sd{sel->id, k};
          ImGui::SetDragDropPayload("GL_SLOT", &sd, sizeof sd);
          ui::DragPreview(ui::Icon::Mixer, ("Move " + info->name + " (Ctrl: copy)").c_str());
          ImGui::EndDragDropSource();
        }
        if (info) {
          ImGui::SetCursorScreenPos(ImVec2(b0.x + bw + 4, q.y + (rowH - 20) * 0.5f));
          ui::ParamKnob("mix", slotMixParam(sel->id, k), 10.0f);
          ImGui::SetCursorScreenPos(ImVec2(b0.x + bw + 4 + 20 + 4, q.y + 2));
          bool editing = pluginWindowShown(a, PluginTarget{sel->id, k});
          if (ui::EyeToggle("edit", editing, 22)) togglePluginWindow(a, PluginTarget{sel->id, k});  // Q-6
        }
        ImGui::SetCursorScreenPos(ImVec2(rowBase.x, rowBase.y + rowH + 2));
        ImGui::Dummy(ImVec2(iw, 0));
        ImGui::PopID();
      }
      if (si > 0) {
        ImGui::Dummy(ImVec2(0, theme::kS2));
        ui::Label("SENDS");
        for (const auto& sd : sel->sends) {
          int ti = s.mixerIndex(sd.to);
          if (ti < 0) continue;
          ImGui::PushID(static_cast<int>(sd.to));
          ImGui::AlignTextToFramePadding();
          std::string nm = mixerTrackName(s, static_cast<size_t>(ti));
          ImGui::TextUnformatted(theme::fitText(nm.c_str(), 80));
          ImGui::SameLine(92);
          float lvl = sd.level;
          ImGui::SetNextItemWidth(std::max(50.0f, iw - 92 - 24 - 2 * 32));
          if (ImGui::DragFloat("##lvl", &lvl, 0.005f, 0.0f, 1.25f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) a.doc.setSend(sel->id, sd.to, std::max(lvl, 0.0001f));
          if (ImGui::IsItemDeactivated()) a.doc.endGesture();
          ImGui::SameLine(0, theme::kS1);
          if (ui::TextButton("Pre", sd.preFader, 28, "Pre-fader send: taps the track before its volume and pan"))
            mixops::setSendPreFader(a.doc, sel->id, sd.to, !sd.preFader);
          ImGui::SameLine(0, 2);
          if (ui::TextButton("SC", sd.sidechain, 28, "Sidechain only: feeds the destination's sidechain input, no audio"))
            a.doc.setSendSidechain(sel->id, sd.to, !sd.sidechain);
          ImGui::SameLine(0, 2);
          if (ui::IconButton("x", ui::Icon::Close, false, 20, "Remove send")) a.doc.setSend(sel->id, sd.to, 0.0f);
          ImGui::PopID();
        }
        if (sel->sends.empty()) ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("Not routed: this track is silent"));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Use the arrows under the strips to add sends (right-click an arrow for route-only, sidechain and pre-fader)."));
        ImGui::PopTextWrapPos();
        if (!a.selChannels.empty()) {
          ImGui::Dummy(ImVec2(0, theme::kS1));
          if (ui::TextButton("Route selected channels here")) {
            a.doc.beginGroup("route channels to mixer track");
            for (ObjectId c : a.selChannels) a.doc.routeChannel(c, sel->id);
            a.doc.endGroup();
          }
        }
      }
      ImGui::Dummy(ImVec2(0, theme::kS2));
      ui::Label("STRIP");
      const std::vector<ObjectId> ids = mixerSelection(a);
      if (ui::TextButton("Polarity", sel->invertPolarity, 0, "Reverse polarity (selected tracks)")) mixops::setPolarity(a.doc, ids, !sel->invertPolarity);
      ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton("Swap L/R", sel->swapLR, 0, "Swap left and right (selected tracks)")) mixops::setSwap(a.doc, ids, !sel->swapLR);
      ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton("Mono", mixops::isMono(*sel), 0, "Make the track mono (stereo separation fully merged) or stereo again"))
        mixops::setMono(a.doc, ids, !mixops::isMono(*sel));
      if (ui::TextButton("Disable", sel->disabled, 0, "Disable: the input passes straight through (no effects, EQ, fader) and costs no CPU"))
        mixops::setDisabled(a.doc, ids, !sel->disabled);
      ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton("Threaded", sel->threaded, 0, "Allow threaded processing (off: the track always runs on the audio thread)"))
        mixops::setThreaded(a.doc, ids, !sel->threaded);
      // Manual track latency.
      {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Latency"));
        ImGui::SameLine(92);
        int lat = sel->latencyOffset;
        ImGui::SetNextItemWidth(std::max(60.0f, iw - 92 - 80));
        if (ImGui::DragInt("##lat", &lat, 4.0f, 0, kMaxManualLatency, "%d smp"))
          a.doc.editMixerTrack(sel->id, "set track latency", [lat](MixerTrack& m) { m.latencyOffset = std::clamp(lat, 0, kMaxManualLatency); },
                               (static_cast<uint64_t>(sel->id) << 8) | 0xA7);
        if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        ui::Hint("Manual latency: tells delay compensation this track is late by this much (e.g. external hardware)");
        ImGui::SameLine(0, theme::kS1);
        ImGui::PushFont(a.fontMono, theme::kFontLabel);
        ImGui::TextColored(theme::vec(P.textFaint), "%s", latencyLabel(sel->latencyOffset, a.engine.sampleRate()).c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, theme::kS1);
        menuButton("latmenu", "Track latency: reset, set in ms / samples / beats", [&a, id = sel->id]() { mixerLatencyMenu(a, id); });
      }
      stripEqPanel(a, *sel, iw);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
  }
  ui::EndDockWindow();
  drawRoutingOverview(a);
}

void mixerLatencyMenu(App& a, ObjectId id) {
  const MixerTrack* t = a.doc.song().mixerTrack(id);
  if (!t) return;
  const double sr = a.engine.sampleRate();
  if (ImGui::MenuItem(i18n::tr("Reset"), nullptr, false, t->latencyOffset != 0)) mixops::setLatencyOffset(a.doc, id, 0);
  if (ImGui::MenuItem(i18n::tr("Set in ms..."))) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", 1000.0 * t->latencyOffset / sr);
    menus::openPrompt("Track latency (ms)", b, [&a, id, sr](const std::string& v) {
      mixops::setLatencyOffset(a.doc, id, mixops::latencyFromMs(std::atof(v.c_str()), sr));
    });
  }
  if (ImGui::MenuItem(i18n::tr("Set in samples...")))
    menus::openPrompt("Track latency (samples)", std::to_string(t->latencyOffset),
                      [&a, id](const std::string& v) { mixops::setLatencyOffset(a.doc, id, std::atoi(v.c_str())); });
  if (ImGui::MenuItem(i18n::tr("Set in beats..."))) {
    menus::openPrompt("Track latency (beats)", "0.25", [&a, id, sr](const std::string& v) {
      mixops::setLatencyOffset(a.doc, id, mixops::latencyFromBeats(std::atof(v.c_str()), a.doc.song().tempo, sr));
    });
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Reset manual latency on all tracks"))) mixops::resetManualLatency(a.doc);
}

void drawEffectEditor(App& a) {
  routePluginWindows(a);  // installed plugins open as themselves
  if (a.fxEditor.first == kNoId) return;
  const Song& s = a.doc.song();
  const MixerTrack* t = s.mixerTrack(a.fxEditor.first);
  int k = a.fxEditor.second;
  if (!t || k < 0 || k >= kEffectSlots) {
    a.fxEditor = {kNoId, -1};
    return;
  }
  const PluginInfo* info = findPlugin(t->slots[static_cast<size_t>(k)].plugin.type);
  if (!info) {
    a.fxEditor = {kNoId, -1};
    return;
  }
  PluginTarget target{t->id, k};
  std::string title = info->name + "  \u00b7  " + mixerTrackName(s, static_cast<size_t>(s.mixerIndex(t->id))) + "  \u00b7  slot " +
                      std::to_string(k + 1) + "###fxeditor";
  float width = info->editorWidth > 0 ? info->editorWidth : (info->type == "gl.eq" ? 780.0f : 600.0f);
  bool open = true;
  ui::NextFloatingWindowSize(width + 24);
  {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
    ui::KeepWindowInViewport();
    ui::MarkDetachable();  // J-114: pops out / docks back like the panels
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowAppearing()) {
      a.focusPluginOwner = target.owner;  // controller template knobs follow the focused plugin
      a.focusPluginSlot = target.slot;
    }
    menuButton("fxmenu", "Effect menu: presets, state, slot", [&a, target]() { pluginWindowMenu(a, target.owner, target.slot); });
    ImGui::SameLine(0, 8);
    drawPluginHeader(a, target);
    drawPluginBody(a, target, width);
  }
  ImGui::End();
  if (!open) a.fxEditor = {kNoId, -1};
}

}  // namespace gl
