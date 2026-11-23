// Gridline — "seltmann" pack: Seltmann editor (0.7.8 Ribbon). One calm flow: drop audio (or load
// it, or record a take), it is analysed on its own, pick a layout, play it and drag the loop out.
// This file holds the frame (the height from the room the editor has; drop zone / progress), the
// header (the file and what it is, the tempo with its fixes, Follow, the Sound / Grid / Loops cards,
// Load, Record take, the height), the action row (play, chop size, the tool, drag handles, make
// loops, send to project, linked), the Compact row, the hint line, the grid tools of the Grid card,
// the shared helpers, auditions, layout previews, loop renders, the linked pattern and the editor's
// own keys. The waveform lives in seltmann_wave_ui.cpp, the sequence in seltmann_seq_ui.cpp, the
// layout pickers and the keys strip in seltmann_parts_ui.cpp, the cards in seltmann_cards_ui.cpp.
// Everything is drawn as vectors with Theme roles.
#include "seltmann_ui.h"
#include "../../core/ThreadName.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>
#include "../../../third_party/imgui/imgui_internal.h"
#include "../../app/AudioRecordUi.h"
#include "../../app/EditorZoomUi.h"
#include "../../app/Keys.h"
#include "../../core/KeyCaps.h"
#include "../../core/Settings.h"
#include "../../core/TrackReceipt.h"
#include "../../app/DevSignal.h"
#include "../../app/Look.h"
#include "../../app/BrowserUi.h"
#include "../../app/ExternalDragUi.h"
#include "../../core/BrowserIndex.h"
#include "../../core/ExternalDrop.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"

namespace gl::packs::seltmann {

namespace fs = std::filesystem;
using theme::pal;

namespace sui {

Ed& ed(const PluginTarget& t) {
  static std::unordered_map<uint64_t, std::unique_ptr<Ed>> m;
  auto& p = m[t.key()];
  if (!p) p = std::make_unique<Ed>();
  return *p;
}

// ------------------------------------------------------------------ colours / text
ImU32 accent() { return theme::litRole(pui::brandRole(BrandFamily::Sample)); }

ImU32 slotColor(int slot, float alpha) {
  // J-152: eight calm hues (muted, tuned to read on the slate and on the light surfaces), ordered so
  // neighbours sit far apart on the wheel; colour follows the letter (A, I, Q ... share a hue).
  static const uint32_t kDark[8] = {0xE3A857, 0x4FB8A8, 0x958AE6, 0xE07C72, 0x9CC26A, 0x60A6DE, 0xD47FAE, 0xCDB65E};
  static const uint32_t kLight[8] = {0xC27F22, 0x23887A, 0x6A5CCB, 0xC4524A, 0x5F8C2E, 0x3479B5, 0xAE4F84, 0x977F22};
  if (slot < 0) return theme::withAlpha(pal().textFaint, alpha);
  return theme::rgb((theme::isDark() ? kDark : kLight)[slot % 8], alpha);
}

ImU32 regionColor(int region, float alpha) { return slotColor(region, alpha); }

ImU32 regionFace(int region, float amount) {
  // A quiet tinted face: the panel surface with a share of the region's hue.
  const auto& P = pal();
  return theme::mix(theme::isDark() ? P.surface700 : P.surface800, regionColor(region), amount);
}

int sliceSlot(const Model& m, int slice) {
  for (int k = 0; k < static_cast<int>(m.pattern.slots.size()); ++k)
    if (m.pattern.slots[static_cast<size_t>(k)] == slice) return k;
  return -1;
}

void textC(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* s) {
  const ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0, s);
  dl->AddText(f, size, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), col, s);
}
void textR(ImDrawList* dl, ImFont* f, float size, ImVec2 rt, ImU32 col, const char* s) {
  const ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0, s);
  dl->AddText(f, size, ImVec2(std::round(rt.x - ts.x), std::round(rt.y)), col, s);
}

std::string fmtTime(double s) {
  if (s < 0) s = 0;
  char b[32];
  const int m = static_cast<int>(s / 60);
  std::snprintf(b, sizeof b, "%d:%04.1f", m, s - m * 60);
  return b;
}

std::string noteName(int midi) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  midi = std::clamp(midi, 0, 127);
  return std::string(n[midi % 12]) + std::to_string(midi / 12 - 1);
}

const char* keyName(int key, bool minor) {
  static const char* const maj[12] = {i18n::N_("C major"),  i18n::N_("C# major"), i18n::N_("D major"),  i18n::N_("Eb major"),
                                      i18n::N_("E major"),  i18n::N_("F major"),  i18n::N_("F# major"), i18n::N_("G major"),
                                      i18n::N_("Ab major"), i18n::N_("A major"),  i18n::N_("Bb major"), i18n::N_("B major")};
  static const char* const mnr[12] = {i18n::N_("C minor"),  i18n::N_("C# minor"), i18n::N_("D minor"),  i18n::N_("Eb minor"),
                                      i18n::N_("E minor"),  i18n::N_("F minor"),  i18n::N_("F# minor"), i18n::N_("G minor"),
                                      i18n::N_("G# minor"), i18n::N_("A minor"),  i18n::N_("Bb minor"), i18n::N_("B minor")};
  if (key < 0 || key > 11) return i18n::N_("Unknown");
  return minor ? mnr[key] : maj[key];
}

// ------------------------------------------------------------------ small controls
ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption, float strip) {
  // J-149: the Gridline panel (pui::beginPanel): surface, hairline border, and with a
  // title the soft title strip with the accent tick, the mono title and a caption.
  const auto& P = pal();
  const float r = 7;
  if (look::styled()) look::hookPanel(dl, p0, p1, P.surface800);
  else {
    dl->AddRectFilled(p0, p1, P.surface800, r);
    dl->AddRect(p0, p1, theme::mix(P.surface800, P.text, 0.08f), r);
  }
  if (!(title && *title)) return ImVec2(p0.x + 10, p0.y + 10);
  skin::gradientRect(dl, ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + strip), theme::mix(P.surface800, P.surface700, 0.75f),
                     theme::mix(P.surface800, P.surface700, 0.35f), r - 1, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(p0.x + 1, p0.y + strip), ImVec2(p1.x - 1, p0.y + strip), theme::mix(P.surface800, P.text, 0.06f));
  const float cy = std::round(p0.y + strip * 0.5f);
  dl->AddRectFilled(ImVec2(p0.x + 10, cy - 5), ImVec2(p0.x + 12, cy + 4), accent(), 1);
  ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontMono;
  dl->AddText(f, 10.5f, ImVec2(p0.x + 18, cy - 7), P.text, title);
  if (caption && *caption) textR(dl, f, 10.5f, ImVec2(p1.x - 12, cy - 7), P.textFaint, caption);
  return ImVec2(p0.x + 10, p0.y + strip + 8);
}

float inlineLabel(ImDrawList* dl, App& a, ImVec2 p, float h, const char* text) {
  const char* s = i18n::tr(text);
  ImFont* f = a.fontMono;
  const float fs = theme::kFontLabel - 1;
  const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, s);
  if (dl) dl->AddText(f, fs, ImVec2(std::round(p.x), std::round(p.y + (h - ts.y) * 0.5f)), pal().textFaint, s);
  return std::round(ts.x + 8);
}

bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur, const char* const* hints,
           bool mono) {
  // J-149: drawn like pui::segmented (the Gridline segmented selector): an inset
  // track, the chosen segment lit, the J-129 control style when one is set.
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const bool styled = look::styled();
  if (styled) look::hookInset(dl, p0, p1, 5, P.surface950);
  else {
    dl->AddRectFilled(p0, p1, P.surface950, 5);
    dl->AddRect(p0, p1, theme::mix(P.surface950, P.text, 0.08f), 5);
  }
  const float cw = w / std::max(1, count);
  bool changed = false;
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    const float x = p0.x + cw * i;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    if (ImGui::InvisibleButton("##pill", ImVec2(cw, h)) && cur != i) cur = i, changed = true;
    const bool hv = ImGui::IsItemHovered();
    if (hv && hints && hints[i]) ui::Tooltip(i18n::tr(hints[i]));
    const bool on = i == cur;
    const ImVec2 q0(std::round(x + 2), p0.y + 2), q1(std::round(x + cw - 2), p0.y + h - 2);
    if (styled) {
      if (on || hv) look::hookFace(dl, q0, q1, 4, (on ? look::On : 0) | (hv ? look::Hover : 0), accent(), false);
    } else if (on) {
      skin::raisedFace(dl, q0, q1, accent(), 4);
    } else if (hv) {
      dl->AddRectFilled(q0, q1, P.surface700, 4);
    }
    ImFont* f = mono ? a.fontMono : a.fontRegular;
    const float fs = mono ? theme::kFontLabel : theme::kFontCaption;
    const char* s = i18n::tr(labels[i]);
    const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0, s);
    const ImU32 col = on ? (styled ? P.hot : theme::contrastText(accent())) : (hv ? P.text : P.textDim);
    if (ts.x <= cw - 6) textC(dl, f, fs, ImVec2(x + cw * 0.5f, p0.y + h * 0.5f), col, s);
    else theme::textEllipsis(dl, f, fs, ImVec2(x + 4, p0.y + (h - ts.y) * 0.5f), cw - 8, col, s);
    // a hairline between unlit neighbours
    if (i > 0 && !on && i - 1 != cur && !styled)
      dl->AddLine(ImVec2(std::round(x) + 0.5f, p0.y + 6), ImVec2(std::round(x) + 0.5f, p1.y - 6), theme::mix(P.surface950, P.text, 0.07f));
    ImGui::PopID();
  }
  ImGui::PopID();
  return changed;
}

bool paramPills(App& a, const PluginTarget& t, int param, ImVec2 p0, float w, float h, const char* const* labels, int count,
                const char* const* hints) {
  int cur = static_cast<int>(std::lround(pui::paramValue(a, t, param)));
  (void)t.pid(param);
  char id[24];
  std::snprintf(id, sizeof id, "##pp%d", param);
  if (pills(a, id, p0, w, h, labels, count, cur, hints)) {
    pui::setParamValue(a, t, param, cur, false);
    return true;
  }
  return false;
}

bool button(const char* id, ImVec2 p0, ImVec2 size, const char* text, bool on, const char* hint, int icon, bool primary) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (hv && hint) ui::Tooltip(i18n::tr(hint));
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  ImU32 bg = on ? accent() : primary ? theme::mix(accent(), P.surface700, hv ? 0.05f : 0.18f) : (ac ? P.surface600 : hv ? P.surface700 : P.surface800);
  if (on && hv) bg = theme::mix(accent(), P.paper, 0.12f);
  skin::raisedFace(dl, p0, p1, bg, theme::kRadius);
  dl->AddRect(p0, p1, on ? theme::mix(accent(), P.ink, 0.3f) : P.line, theme::kRadius);
  const ImU32 fg = on || primary ? theme::contrastText(bg) : (hv ? P.text : P.textDim);
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  if (icon >= 0 && (!text || !*text)) {
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), c, std::min(size.x, size.y) * 0.55f, fg);
  } else if (icon >= 0) {
    const float isz = std::min(14.0f, size.y * 0.55f);
    const char* s = i18n::tr(text);
    const float tw = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, s).x;
    const float total = isz + 5 + tw;
    const float x0 = c.x - total * 0.5f;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2(x0 + isz * 0.5f, c.y), isz, fg);
    dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(x0 + isz + 5), std::round(c.y - theme::kFontCaption * 0.62f)), fg, s);
  } else if (text) {
    textC(dl, ImGui::GetFont(), theme::kFontCaption, c, fg, i18n::tr(text));
  }
  return clicked;
}

// `step` > 0: the value lands on whole steps of it (1: whole numbers), a wheel notch moves one.
bool fieldKnob(App& a, const char* id, ImVec2 tl, float& v, float mn, float mx, float def, const char* label, const char* fmt, bool* released,
               bool bipolar, float step) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cell = 54, r = 13;
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton(id, ImVec2(cell, 2 * r + 30));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  ParamDesc d;  // J-136: the shared reset / type-in over this free value
  d.name = i18n::tr(label), d.min = mn, d.max = mx, d.def = def;
  const double pressed = ui::ValueAtPress(v);
  const bool deactivated = ImGui::IsItemDeactivated();
  bool changed = false;
  const float span = mx - mn;
  auto onStep = [&](float x) { return step > 0 ? std::clamp(mn + std::round((x - mn) / step) * step, mn, mx) : x; };
  // The shared knob drag (Widgets.h): it runs on its own position, so a value the caller keeps
  // whole (the choke group) follows a slow drag too.
  ui::KnobDragPos(span > 0 ? static_cast<double>((v - mn) / span) : 0.0);
  if (ac && ImGui::IsMouseDragging(0, 0) && ImGui::GetIO().MouseDelta.y != 0) {
    const double nn = ui::KnobDragMove(-ImGui::GetIO().MouseDelta.y / (ImGui::GetIO().KeyShift ? 600.0 : 150.0));
    const float nv = onStep(static_cast<float>(mn + nn * span));
    if (nv != v) v = nv, changed = true;
  }
  if (hv && ImGui::GetIO().MouseWheel != 0) {
    const float wheel = ImGui::GetIO().MouseWheel;
    v = step > 0 ? onStep(v + (wheel > 0 ? step : -step)) : std::clamp(v + wheel * span / 50.0f, mn, mx);
    changed = true;
    if (released) *released = true;
  }
  ImGui::PushID(id);
  double dv = v;
  bool set = false, wantType = false;
  ParamDoubleClick dbl = ParamDoubleClick::Type;
  if (ui::DoubleClicked(d, pressed, &dbl)) {  // reset; at the default, type a value
    if (dbl == ParamDoubleClick::Reset) dv = def, set = true, ui::KnobDragSet(span > 0 ? static_cast<double>((def - mn) / span) : 0.0);
    else wantType = true;
  }
  if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##fkmenu");
  if (ImGui::BeginPopup("##fkmenu")) {
    ImGui::TextDisabled("%s", d.name.c_str());
    ImGui::Separator();
    if (ui::ResetMenuItem(d, v)) dv = def, set = true;
    if (ImGui::MenuItem(i18n::tr("Type a value..."))) wantType = true;
    ImGui::EndPopup();
  }
  if (wantType) ImGui::OpenPopup("##type");
  if (ui::ValueTypePopup(d, &dv)) set = true;
  ImGui::PopID();
  if (set) {
    v = onStep(static_cast<float>(dv));
    changed = true;
    if (released) *released = true;
  }
  if (released && deactivated) *released = true;
  char vb[32];
  std::snprintf(vb, sizeof vb, fmt, static_cast<double>(v));
  if (hv || ac) ui::Tooltip(vb);
  const ImVec2 c(tl.x + cell * 0.5f, tl.y + r + 2);
  skin::KnobPaint kp;
  kp.rail = P.surface900;
  kp.fill = accent();
  kp.cap = P.surface600;
  kp.pointer = P.paper;
  kp.a0 = 3.14159265f * 0.75f;
  kp.a1 = 3.14159265f * 2.25f;
  const float n = (v - mn) / std::max(1e-6f, mx - mn);
  kp.an = kp.a0 + (kp.a1 - kp.a0) * n;
  kp.from = bipolar ? (kp.a0 + kp.a1) * 0.5f : kp.a0;
  kp.hot = hv;
  kp.active = ac;
  skin::knobFace(dl, c, r, kp);
  textC(dl, a.fontMono, theme::kFontLabel, ImVec2(c.x, tl.y + 2 * r + 9), hv || ac ? P.text : P.textDim, vb);
  textC(dl, a.fontRegular, theme::kFontLabel, ImVec2(c.x, tl.y + 2 * r + 22), P.textFaint, i18n::tr(label));
  return changed;
}

void grip(ImDrawList* dl, ImVec2 c, ImU32 col) {  // grip-dots.svg on the old px grid (5.5 px per unit)
  ui::drawIconGlyph(dl, ui::Icon::GripDots, c, 5.5f, 0.0f, col);
}

// ------------------------------------------------------------------ model edits
void commit(App& a, const PluginTarget& t, Ed& e, const Model& m, const char* label, uint64_t mergeKey) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  PluginState st = *sp;
  st.data = formatData(e.path, m);
  setPluginState(a, t, st, i18n::tr(label), mergeKey);
  e.m = m;
  e.data = st.data;
  e.hasModel = true;
}

void commitWith(App& a, const PluginTarget& t, Ed& e, const Model& m, const std::vector<std::pair<int, float>>& params, const char* label) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  PluginState st = *sp;
  st.data = formatData(e.path, m);
  for (const auto& [k, v] : params)
    if (k >= 0 && k < static_cast<int>(st.params.size())) st.params[static_cast<size_t>(k)] = v;
  setPluginState(a, t, st, i18n::tr(label));
  e.m = m;
  e.data = st.data;
  e.hasModel = true;
}

SamplePtr sampleOf(Ed& e) {
  if (e.job && e.job->state == 1) return e.job->sample;
  return e.path.empty() ? nullptr : sampleCache().request(e.path);
}

double paramOf(App& a, const PluginTarget& t, int k) { return pui::paramValue(a, t, k); }
int baseNote(App& a, const PluginTarget& t) { return std::clamp(static_cast<int>(std::lround(paramOf(a, t, SmBaseNote))), 12, 115); }

LiveView live(App& a, const PluginTarget& t) {
  LiveView v;
  if (!t.isChannel()) return v;
  if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen) liveView(rt->gen.get(), v);
  return v;
}

// ------------------------------------------------------------------ audition
void auditionStart(App& a, const PluginTarget& t, Ed& e, int slice, float vel) {
  auditionStop(a, t, e);
  if (!t.isChannel() || slice < 0) return;
  const int base = baseNote(a, t);
  int key = -1;
  if (static_cast<int>(std::lround(paramOf(a, t, SmKeyMap))) == KeySlots) {
    const int s = sliceSlot(e.m, slice);
    if (s >= 0) key = base + s;
  } else {
    key = base + slice;
  }
  if (key >= 0 && key <= 127) {
    a.engine.noteOn(t.owner, key, std::clamp(vel, 0.05f, 1.0f));
    e.auditionKey = key;
  } else {
    // not on a key: play a rendered copy
    const std::string f = renderItem(a, t, e, "slice:" + std::to_string(slice), "");
    if (!f.empty()) a.previewSample(f);
  }
}

void auditionStop(App& a, const PluginTarget& t, Ed& e) {
  if (e.auditionKey >= 0 && t.isChannel()) a.engine.noteOff(t.owner, e.auditionKey);
  e.auditionKey = -1;
}

void togglePattern(App& a, const PluginTarget& t, Ed& e) {
  if (!t.isChannel()) return;
  if (e.pattern.on) {
    if (e.patternKey >= 0) a.engine.noteOff(t.owner, e.patternKey);  // (the key it started with: the base note may have moved)
    e.pattern.stop();
    e.patternKey = -1;
    return;
  }
  // Starts from step 1 (the instrument restarts the pattern on its key).
  e.patternKey = baseNote(a, t) - 12;
  a.engine.ownNoteOn(t.owner, e.patternKey, 0.9f);  // J-112: the editor's own Play (plugin playback)
  e.pattern.start(a.engine);
}

// J-110: the Play button follows what plays. Stop (once or twice), Space,
// Tools > Stop all sound, a MIDI / remote Stop and a focus loss move the
// engine's stop epoch (the engine released the pattern's key); the
// instrument's running flag covers the rest (killed, replaced, ended).
void syncPattern(App& a, const PluginTarget& t, Ed& e, const LiveView& lv) {
  const RunLatch::End end = e.pattern.sync(a.engine, lv.patternRunning);
  if (end == RunLatch::End::None) return;
  if (end == RunLatch::End::Finished && e.patternKey >= 0 && t.isChannel()) a.engine.noteOff(t.owner, e.patternKey);
  e.patternKey = -1;
  if (end == RunLatch::End::Stopped) e.auditionKey = -1;  // (released by the engine too)
}

// ------------------------------------------------------------------ renders
std::string loopsDir(App& a) { return seltmannLoopsDir(a); }

namespace {

std::string cleanName(std::string s) {
  for (char& c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.')) c = '_';
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s.empty() ? std::string("loop") : s;
}

uint64_t hashOf(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

RenderOpts optsOf(App& a, const PluginState& st) {
  RenderOpts o;
  o.sampleRate = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : 44100.0;
  o.tempo = a.doc.song().tempo;
  o.params = st.params;
  o.sync = st.params.size() > SmSync ? static_cast<int>(std::lround(st.params[SmSync])) : SyncStretch;
  o.stretchMode = st.params.size() > SmStretchMode ? static_cast<int>(std::lround(st.params[SmStretchMode])) : 0;
  return o;
}

// The file name and renderer of an item.
bool renderTo(const std::string& item, const Model& m, SamplePtr src, const RenderOpts& o, const std::string& file) {
  std::vector<float> L, R;
  bool ok = false;
  if (item == "pattern") ok = renderPatternLoop(m, src, o, L, R);
  else if (item.rfind("slice:", 0) == 0) ok = renderSlice(m, src, std::atoi(item.c_str() + 6), o, L, R);
  else if (item.rfind("span:", 0) == 0) {
    double b0 = 0, b1 = 0;
    std::sscanf(item.c_str() + 5, "%lf:%lf", &b0, &b1);
    ok = renderSpan(m, src, b0, b1, o, L, R);
  }
  if (!ok || L.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(file).parent_path(), ec);
  if (!writeLoopWav(file, L, R, o.sampleRate)) return false;
  if (src && !src->path.empty() && src->sampleRate > 0) {  // J-219: where the file comes from (Track Receipt)
    receipt::Origin og;
    og.source = src->path;
    og.kind = "slice";
    const int k = item.rfind("slice:", 0) == 0 ? std::atoi(item.c_str() + 6) : -1;
    if (k >= 0 && k < static_cast<int>(m.slices.size())) {
      const Slice& sl = m.slices[static_cast<size_t>(k)];
      const double a = static_cast<double>(sl.start) / src->sampleRate, len = static_cast<double>(sl.end - sl.start) / src->sampleRate;
      og.start = a + len * std::clamp(static_cast<double>(sl.offset), 0.0, 0.9);
      og.end = std::min(a + len, og.start + len * std::clamp(static_cast<double>(sl.length), 0.05, 1.0));
      og.speed = (og.end - og.start) / std::max(1e-6, static_cast<double>(L.size()) / o.sampleRate);
      og.reverse = sl.reverse;
    } else {
      og.kind = "render";
      og.rangeKnown = false;
    }
    receipt::writeOrigin(file, og);
  }
  return true;
}

}  // namespace

std::string renderItem(App& a, const PluginTarget& t, Ed& e, const std::string& item, const std::string& label) {
  const PluginState* sp = t.state(a.doc.song());
  SamplePtr src = sampleOf(e);
  if (!sp || !src || !e.hasModel) return "";
  const RenderOpts o = optsOf(a, *sp);
  char tb[32];
  std::snprintf(tb, sizeof tb, "%.6g", o.tempo);
  const std::string key = item + "|" + std::to_string(hashOf(sp->data)) + "|" + tb + "|" + std::to_string(hashOf(std::string(
                                                                                                reinterpret_cast<const char*>(sp->params.data()),
                                                                                                sp->params.size() * sizeof(float))));
  auto it = e.rendered.find(key);
  std::error_code ec;
  if (it != e.rendered.end() && fs::exists(it->second, ec)) return it->second;
  const std::string stem = fs::path(e.path).stem().string();
  std::string name;
  if (item == "midi") {
    name = cleanName(stem + " - " + e.m.pattern.name) + ".mid";
    const std::string file = (fs::path(loopsDir(a)) / name).string();
    fs::create_directories(loopsDir(a), ec);
    const auto notes = patternNotes(e.m, baseNote(a, t), static_cast<float>(paramOf(a, t, SmSwing)), static_cast<float>(paramOf(a, t, SmGate)));
    if (!writePatternMidi(file, notes, o.tempo)) return "";
    e.rendered[key] = file;
    return file;
  }
  char bpm[32];
  std::snprintf(bpm, sizeof bpm, " %.0f bpm", o.sync == SyncOriginal ? e.m.grid.bpm : o.tempo);
  name = cleanName(stem + " - " + (label.empty() ? item : label) + bpm) + ".wav";
  const std::string file = (fs::path(loopsDir(a)) / name).string();
  if (!renderTo(item, e.m, src, o, file)) return "";
  e.rendered[key] = file;
  return file;
}

void makeLoops(App& a, const PluginTarget& t, Ed& e) {
  const PluginState* sp = t.state(a.doc.song());
  SamplePtr src = sampleOf(e);
  if (!sp || !src || !e.hasModel || (e.loopJob && !e.loopJob->finished)) return;
  struct Item {
    std::string item, file;
  };
  std::vector<Item> items;
  const std::string stem = fs::path(e.path).stem().string();
  const RenderOpts o = optsOf(a, *sp);
  char bpm[32];
  std::snprintf(bpm, sizeof bpm, " %.0f bpm", o.sync == SyncOriginal ? e.m.grid.bpm : o.tempo);
  const fs::path dir = loopsDir(a);
  items.push_back({"pattern", (dir / (cleanName(stem + " - " + e.m.pattern.name + bpm) + ".wav")).string()});
  const int bpb = std::max(1, e.m.grid.beatsPerBar);
  for (size_t i = 0; i < e.m.grid.loops.size() && i < 8; ++i) {
    const auto& l = e.m.grid.loops[i];
    const int bar = static_cast<int>(std::floor(static_cast<double>(l.firstBeat - e.m.grid.bar1) / bpb)) + 1;
    char nm[96];
    std::snprintf(nm, sizeof nm, "phrase %d bars at bar %d", l.bars, bar);
    char it[64];
    std::snprintf(it, sizeof it, "span:%d:%d", l.firstBeat, l.firstBeat + l.bars * bpb);
    items.push_back({it, (dir / (cleanName(stem + " - " + nm + bpm) + ".wav")).string()});
  }
  for (size_t i = 0; i < e.m.grid.sections.size() && i < 16; ++i) {
    const auto& s = e.m.grid.sections[i];
    char nm[96];
    std::snprintf(nm, sizeof nm, "section %s%zu", slotName(s.label), i + 1);
    char it[64];
    std::snprintf(it, sizeof it, "span:%d:%d", s.firstBeat, s.endBeat);
    items.push_back({it, (dir / (cleanName(stem + " - " + nm + bpm) + ".wav")).string()});
  }
  auto job = std::make_shared<LoopJob>();
  job->total = static_cast<int>(items.size());
  e.loopJob = job;
  const Model m = e.m;
  static std::vector<std::thread> threads;  // joined when the editor library unloads (static destruction)
  struct Joiner {
    ~Joiner() {
      for (auto& th : threads)
        if (th.joinable()) th.join();
    }
  };
  static Joiner joiner;
  (void)joiner;
  threads.emplace_back([job, items, m, src, o] { gl::threads::setName("gl-seltmann-job");
    std::vector<std::string> out;
    for (const auto& it : items) {
      if (renderTo(it.item, m, src, o, it.file)) out.push_back(it.file);
      ++job->done;
    }
    job->files = out;
    job->finished = true;
  });
}

void sendToProject(App& a, const PluginTarget& t, Ed& e) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || !e.hasModel || !t.isChannel()) return;
  const std::vector<float> params = sp->params;
  ObjectId pid = kNoId;
  const Song& song = a.doc.song();
  const double beats = patternBars(e.m, params, 1);
  const Tick len = static_cast<Tick>(std::llround(beats * song.ppq));
  // the first playlist track that is free over [0, len)
  int track = 0;
  for (; track < kMaxPlaylistTracks; ++track) {
    bool busy = false;
    for (const auto& c : song.playlist)
      if (c.track == track && c.start < len && c.start + std::max<Tick>(c.length, 1) > 0) busy = true;
    if (!busy) break;
  }
  a.doc.editSong(i18n::tr("Send pattern to project"), [&](Song& s) { pid = sendPatternToSong(s, t.owner, e.m, params, 1, track, 0, true); });
  if (pid != kNoId) {
    a.selectPattern(pid);
    a.notify(std::string(i18n::tr("Added the pattern to the project: ")) + e.m.pattern.name);
  }
}

void dragOut(App& a, const PluginTarget& t, Ed& e, const std::string& item, const std::string& label) {
  if (!ImGui::IsItemActive() || !ImGui::IsMouseDragging(0, 4.0f)) return;
  // render once, when the drag starts (cached by content)
  const std::string f = renderItem(a, t, e, item, label);
  if (f.empty()) return;
  browserui::fileDragSource(f, label.empty() ? nullptr : label.c_str());
}


const char* kindLabel(int kind) {
  switch (kind) {
    case KindBreak: return i18n::tr("Break");
    case KindLoop: return i18n::tr("Loop");
    case KindSong: return i18n::tr("Song");
    case KindOneShot: return i18n::tr("One-shot");
    case KindAmbient: return i18n::tr("Ambient");
    default: return i18n::tr("Audio");
  }
}

void groupLabel(ImDrawList* dl, App& a, ImVec2 p, const char* text) {
  dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(std::round(p.x + 2), std::round(p.y)), pal().textFaint, i18n::tr(text));
}

float dragChip(App& a, const PluginTarget& t, Ed& e, const char* id, ImVec2 p0, float h, const char* text, const std::string& item,
               const std::string& label, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, text).x;
  const float w = std::round(tw + 30);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (hv && !ac && hint) ui::Tooltip(i18n::tr(hint));
  if (hv || ac) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  dragOut(a, t, e, item, label);
  const ImVec2 p1(p0.x + w, p0.y + h);
  skin::raisedFace(dl, p0, p1, hv ? P.surface700 : P.surface800, theme::kRadius);
  dl->AddRect(p0, p1, hv ? theme::withAlpha(accent(), 0.8f) : P.line, theme::kRadius);
  grip(dl, ImVec2(p0.x + 11, p0.y + h * 0.5f), hv ? accent() : P.textDim);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p0.x + 21), std::round(p0.y + (h - theme::kFontLabel) * 0.5f - 1)), hv ? P.text : P.textDim,
              text);
  return w;
}

void applyLayout(App& a, const PluginTarget& t, Ed& e, const Preset& p, bool play) {
  Model m = e.m;
  auto params = applyPreset(m, sampleOf(e).get(), p, e.seed);
  resolveSlots(m);  // 0.7.8: one letter system: the steps name their regions
  commitWith(a, t, e, m, params, (std::string(i18n::tr("Layout")) + ": " + p.name).c_str());
  e.selSlice = 0;
  e.selStep = -1;
  if (play && !e.pattern.on) togglePattern(a, t, e);
}

void openLoadDialog(App& a, const PluginTarget& t, Ed& e) {
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::tr("Load audio into Seltmann");
  a.fileDialog.dir = e.path.empty() ? (std::getenv("HOME") ? std::getenv("HOME") : ".") : fs::path(e.path).parent_path().string();
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"};
  const PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& p) { loadAudio(a, tt, p); };
}

void loadAudio(App& a, const PluginTarget& t, const std::string& path, const std::string& name) {
  if (const PluginState* sp = t.state(a.doc.song())) {
    PluginState st = *sp;
    st.data = path;
    setPluginState(a, t, st, i18n::tr("Load audio"));
    if (t.isChannel()) a.doc.renameChannel(t.owner, name.empty() ? fs::path(path).stem().string() : name);
  }
}

namespace {

// Rechop after a chop change, one undo step.
void commitChop(App& a, const PluginTarget& t, Ed& e, Model nm, const char* label) {
  rechopKeepPattern(nm, sampleOf(e).get());
  commit(a, t, e, nm, label);
  e.selSlice = std::clamp(e.selSlice, 0, std::max(0, static_cast<int>(nm.slices.size()) - 1));
}

const char* chopSummary(const Model& m) {
  static char b[64];
  switch (m.chop.mode) {
    case ChopGrid:
      std::snprintf(b, sizeof b, "%s%s", divisionName(m.chop.division), m.chop.hitSnap ? i18n::tr(" on the hits") : "");
      break;
    case ChopTransients: std::snprintf(b, sizeof b, "%s", i18n::tr("at the hits")); break;
    case ChopSections: std::snprintf(b, sizeof b, "%s", i18n::tr("by section")); break;
    case ChopEqual: std::snprintf(b, sizeof b, "%d %s", m.chop.count, i18n::tr("equal parts")); break;
    case ChopGrains: std::snprintf(b, sizeof b, "%.0f ms %s", static_cast<double>(m.chop.grainMs), i18n::tr("grains")); break;
    default: std::snprintf(b, sizeof b, "%s", i18n::tr("by hand")); break;
  }
  return b;
}

// ------------------------------------------------------------------ empty / analysing
void drawEmpty(App& a, const PluginTarget& t, Ed& e, ImVec2 z0, ImVec2 z1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  e.zone0 = z0, e.zone1 = z1;
  // a file being dragged over the window lights the zone (the whole window takes the drop)
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  const bool dragging = pl && (pl->IsDataType("GL_FILE") || pl->IsDataType("GL_FILES") || pl->IsDataType("GL_ITEM") ||
                               pl->IsDataType("GL_CLIPSRC"));  // J-110: recordings from the playlist picker
  const bool over = dragging && ImGui::IsMouseHoveringRect(z0, z1, false);
  skin::insetField(dl, z0, z1, over ? theme::mix(P.well, accent(), 0.08f) : P.well, theme::kRadiusLg);
  // dashed outline
  const ImU32 dash = theme::withAlpha(accent(), over ? 0.95f : dragging ? 0.7f : 0.38f);
  const float in = 14, seg = 9, gap = 7;
  for (float x = z0.x + in; x < z1.x - in; x += seg + gap) {
    const float x2 = std::min(x + seg, z1.x - in);
    dl->AddLine(ImVec2(x, z0.y + in), ImVec2(x2, z0.y + in), dash, 1.5f);
    dl->AddLine(ImVec2(x, z1.y - in), ImVec2(x2, z1.y - in), dash, 1.5f);
  }
  for (float y = z0.y + in; y < z1.y - in; y += seg + gap) {
    const float y2 = std::min(y + seg, z1.y - in);
    dl->AddLine(ImVec2(z0.x + in, y), ImVec2(z0.x + in, y2), dash, 1.5f);
    dl->AddLine(ImVec2(z1.x - in, y), ImVec2(z1.x - in, y2), dash, 1.5f);
  }
  const ImVec2 c((z0.x + z1.x) * 0.5f, (z0.y + z1.y) * 0.5f - 20);
  const bool analysing = e.job && e.job->state == 0 && !e.path.empty();
  const bool failed = e.job && e.job->state == 2;
  // the picture: a waveform cut into four coloured slices (it fills while analysing)
  const float prog = analysing ? std::clamp(e.job->progress.load(), 0.0f, 1.0f) : 1.0f;
  for (int i = -28; i <= 28; ++i) {
    const float h = 34.0f * (0.3f + 0.7f * std::fabs(std::sin(i * 0.47f + 0.6f)) * std::exp(-std::fabs(static_cast<float>(i)) / 24.0f));
    const float x = c.x + i * 6.0f;
    const int slot = (i + 28) / 15;
    const bool lit = !analysing || (i + 28) / 57.0f <= prog;
    dl->AddRectFilled(ImVec2(x - 1.6f, c.y - 64 - h), ImVec2(x + 1.6f, c.y - 64 + h), lit ? slotColor(slot, 0.9f) : theme::withAlpha(P.textFaint, 0.35f), 1.6f);
  }
  if (analysing) {
    const float p = prog;
    const char* stage = p < 0.12f ? i18n::tr("Reading the file") : p < 0.5f ? i18n::tr("Listening for the hits") : p < 0.62f ? i18n::tr("Following the beat")
                        : p < 0.9f ? i18n::tr("Finding the bars and sections") : i18n::tr("Chopping");
    textC(dl, a.fontBold, 20, ImVec2(c.x, c.y + 6), P.text, fs::path(e.path).filename().string().c_str());
    textC(dl, a.fontRegular, theme::kFontBody, ImVec2(c.x, c.y + 34), P.textDim, stage);
    const float bw = 280;
    const ImVec2 b0(c.x - bw * 0.5f, c.y + 56), b1(c.x + bw * 0.5f, c.y + 61);
    dl->AddRectFilled(b0, b1, P.surface900, 2.5f);
    dl->AddRectFilled(b0, ImVec2(b0.x + bw * p, b1.y), accent(), 2.5f);
    return;
  }
  if (over) {
    textC(dl, a.fontBold, 22, ImVec2(c.x, c.y + 10), P.text, i18n::tr("Release to analyse"));
    return;
  }
  textC(dl, a.fontBold, 22, ImVec2(c.x, c.y + 6), P.text, failed ? i18n::tr("This file cannot be read") : i18n::tr("Drop audio here"));
  textC(dl, a.fontRegular, theme::kFontBody, ImVec2(c.x, c.y + 36), P.textDim,
        failed ? i18n::tr("Try another file: WAV, AIFF, FLAC, MP3 or OGG.")
               : i18n::tr("Breaks, loops, full songs, anything. Seltmann finds the tempo and the bars and chops it for you."));
  textC(dl, a.fontRegular, theme::kFontCaption, ImVec2(c.x, c.y + 58), P.textFaint,
        i18n::tr("From Finder or Explorer, Tracklib, the browser, the playlist or the audio editor"));
  // J-218: Load and Record side by side, the same size
  if (button("##sload2", ImVec2(c.x - 155, c.y + 82), ImVec2(150, 34), i18n::N_("Load audio..."), false, nullptr,
             static_cast<int>(ui::Icon::Open), true))
    openLoadDialog(a, t, e);
  e.recEntry = ImVec2(c.x + 80, c.y + 99);
  if (button("##srec2", ImVec2(c.x + 5, c.y + 82), ImVec2(150, 34), i18n::N_("Record..."), false,
             i18n::N_("Record from an audio input (a microphone, an instrument) and load the recording here"),
             static_cast<int>(ui::Icon::Record), true))
    openRecorder(a, t);
}

// ------------------------------------------------------------------ header (what it is)
// J-152: one 36 px row: the kind chip, the file name over its facts, the tempo with the
// /2 and x2 fix, Load and Edit. Every control is kHeaderCtl tall on the row's centre line.
constexpr float kHeaderCtl = 28;

void drawBpm(App& a, const PluginTarget& t, Ed& e, ImVec2 f0, bool fixes) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Analysis& g = e.m.grid;
  const ImVec2 f1(f0.x + 104, f0.y + kHeaderCtl);
  skin::insetField(dl, f0, f1, P.field, theme::kRadius);
  char b[16];
  std::snprintf(b, sizeof b, "%.2f", g.bpm);
  ImGui::SetCursorScreenPos(f0);
  ImGui::InvisibleButton("##sbpm", ImVec2(f1.x - f0.x, f1.y - f0.y));
  const bool hv = ImGui::IsItemHovered();
  if (hv) {
    std::string tip = i18n::tr("The tempo. Double-click to type it; the grid keeps bar 1 where it is.");
    if (g.nameBpm > 0) tip += std::string("\n") + i18n::tr("The file name's tempo helped.");
    if (g.loopFit) tip += std::string("\n") + i18n::tr("Fitted to the loop's length (whole bars).");
    ui::Tooltip(tip.c_str());
  }
  if (hv && ImGui::IsMouseDoubleClicked(0)) {
    e.typingBpm = true;
    std::snprintf(e.bpmText, sizeof e.bpmText, "%.2f", g.bpm);
  }
  if (e.typingBpm) {
    ImGui::SetCursorScreenPos(ImVec2(f0.x + 4, f0.y + 2));
    ImGui::SetNextItemWidth(f1.x - f0.x - 8);
    ImGui::SetKeyboardFocusHere();
    if (ImGui::InputText("##sbpmtype", e.bpmText, sizeof e.bpmText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
      const double v = std::atof(e.bpmText);
      if (v >= 30 && v <= 300 && !g.beats.empty()) {
        Model m = e.m;
        gridSetBpm(m.grid, v, m.grid.beats[static_cast<size_t>(std::clamp(m.grid.bar1, 0, static_cast<int>(m.grid.beats.size()) - 1))]);
        m.grid.loopFit = false;
        rechopKeepPattern(m, sampleOf(e).get());
        commit(a, t, e, m, i18n::N_("Set the tempo"));
      }
      e.typingBpm = false;
    }
    if (ImGui::IsItemDeactivated()) e.typingBpm = false;
  } else {
    ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontBold;
    const float fs = 16;
    dl->AddText(f, fs, ImVec2(f0.x + 9, std::round(f0.y + (kHeaderCtl - fs) * 0.5f) - 1), hv ? P.paper : P.text, b);
    textR(dl, a.fontMono, theme::kFontLabel - 1, ImVec2(f1.x - 8, f0.y + 9), P.textFaint, "BPM");
  }
  // how sure the analysis is: a hairline under the digits
  const float c = e.m.analysed ? static_cast<float>(std::clamp(g.confidence, 0.0, 1.0)) : 0.0f;
  dl->AddRectFilled(ImVec2(f0.x + 6, f1.y - 4), ImVec2(f1.x - 6, f1.y - 2), P.surface900, 1);
  dl->AddRectFilled(ImVec2(f0.x + 6, f1.y - 4), ImVec2(f0.x + 6 + (f1.x - f0.x - 12) * c, f1.y - 2),
                    c > 0.6f ? P.meterLow : c > 0.3f ? P.meterMid : P.meterHigh, 1);
  if (!fixes) return;
  float x = f1.x + 4;
  if (button("##shalf", ImVec2(x, f0.y), ImVec2(30, kHeaderCtl), "/2", false, i18n::N_("Half the tempo: the analysis heard double time"))) {
    Model m = e.m;
    gridHalve(m.grid);
    rechopKeepPattern(m, sampleOf(e).get());
    commit(a, t, e, m, i18n::N_("Half tempo"));
  }
  x += 34;
  if (button("##sdbl", ImVec2(x, f0.y), ImVec2(30, kHeaderCtl), "x2", false, i18n::N_("Double the tempo: the analysis heard half time"))) {
    Model m = e.m;
    gridDouble(m.grid);
    rechopKeepPattern(m, sampleOf(e).get());
    commit(a, t, e, m, i18n::N_("Double tempo"));
  }
}

// (trace) a row whose controls ran into each other: what was left between its two ends
void crowded(Ed& e, const char* row, float left) {
  if (left < 0) e.crowd += std::string(e.crowd.empty() ? "" : " ") + row;
}

// ------------------------------------------------------------------ header (what it is, how it follows)
// 0.7.8 Ribbon: left the kind, the file and its facts; right the tempo with its fixes and Tap, how
// it follows the project (Stretch / Resample / Off), the Sound / Grid / Loops cards, Load, Record
// take and the height.
void drawHeader(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H, bool song) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  const float cy = o.y + H * 0.5f, cTop = std::round(cy - kHeaderCtl * 0.5f);
  const bool wide = W >= 1180, narrow = W < 1100;  // (narrow: a height menu, shorter Follow and card buttons, no Tap)
  float rx = o.x + W;
  // the height (Auto follows the room the editor has): pills, or a menu where the header is narrow
  if (wide) {
    static const char* const l[4] = {i18n::N_("Auto"), i18n::N_("Compact"), i18n::N_("Standard"), i18n::N_("Full")};
    static const char* const h[4] = {i18n::N_("The height follows the room the editor has (remembered for you, not in the project)"),
                                     i18n::N_("One row and the ribbon: for a device band"), i18n::N_("The ribbon, the tools and the layout cards"),
                                     i18n::N_("Everything: the layout library and the keys")};
    const int choice = heightChoice(a.settings.getString("seltmann.height"));
    int cur = choice + 1;
    const float w = wide ? 236.0f : 168.0f;
    rx -= w;
    if (pills(a, "##sheight", ImVec2(rx, cTop), w, kHeaderCtl, l, 4, cur, h) && cur >= 0) {
      a.settings.set("seltmann.height", heightChoiceName(cur - 1));
      a.settings.save(Settings::defaultPath());
    }
    e.btns.push_back({"heightauto", ImVec2(rx + w * 0.125f, cTop + kHeaderCtl * 0.5f)});
    e.btns.push_back({"heightcompact", ImVec2(rx + w * 0.375f, cTop + kHeaderCtl * 0.5f)});
    e.btns.push_back({"heightstandard", ImVec2(rx + w * 0.625f, cTop + kHeaderCtl * 0.5f)});
    e.btns.push_back({"heightfull", ImVec2(rx + w * 0.875f, cTop + kHeaderCtl * 0.5f)});
  } else {
    static const char* const names[4] = {i18n::N_("Auto"), i18n::N_("Compact"), i18n::N_("Standard"), i18n::N_("Full")};
    const int choice = heightChoice(a.settings.getString("seltmann.height"));
    const float w = 92;
    rx -= w;
    char lb[48];
    std::snprintf(lb, sizeof lb, "%s   ", i18n::tr(names[std::clamp(choice + 1, 0, 3)]));  // (room for the arrow)
    if (button("##sheightmenu", ImVec2(rx, cTop), ImVec2(w, kHeaderCtl), lb, false,
               i18n::N_("The editor's height: Auto follows the room it has (remembered for you, not in the project)")))
      ImGui::OpenPopup("##sheightpop");
    {  // the menu's arrow
      const float ax = rx + w - 14, ay = cTop + kHeaderCtl * 0.5f;
      dl->AddTriangleFilled(ImVec2(ax - 4, ay - 2), ImVec2(ax + 4, ay - 2), ImVec2(ax, ay + 3), P.textDim);
    }
    noteBtn(e, "height");
    ImGui::SetNextWindowPos(ImVec2(rx, cTop + kHeaderCtl + 2));
    if (ImGui::BeginPopup("##sheightpop")) {
      for (int i = 0; i < 4; ++i)
        if (ImGui::MenuItem(i18n::tr(names[i]), nullptr, choice == i - 1)) {
          a.settings.set("seltmann.height", heightChoiceName(i - 1));
          a.settings.save(Settings::defaultPath());
        }
      ImGui::EndPopup();
    }
  }
  rx -= 8;
  {
    const float w = wide ? 112.0f : kHeaderCtl + 6;
    rx -= w;
    if (button("##srectake", ImVec2(rx, cTop), ImVec2(w, kHeaderCtl), wide ? i18n::N_("Record take") : "", recorderOpenFor(t),
               i18n::N_("Record a new take into this instance (keep the chop or analyse it fresh)"), static_cast<int>(ui::Icon::Record)))
      openRecorder(a, t);
    noteBtn(e, "rectake");
  }
  const float loadW = narrow ? 62.0f : 76.0f;
  rx -= 6 + loadW;
  if (button("##sload", ImVec2(rx, cTop), ImVec2(loadW, kHeaderCtl), i18n::N_("Load..."), false, i18n::N_("Load a song, a loop or any audio file")))
    openLoadDialog(a, t, e);
  rx -= 10;
  // the cards of the header: Sound, Grid, Loops
  {
    struct C {
      int card;
      const char* id;
      const char* text;
      const char* hint;
      const char* trace;
    };
    static const C cs[3] = {{CardLoops, "##scloops", i18n::N_("Loops"), i18n::N_("The pattern, the best phrases and the sections to drag out"), "loops"},
                            {CardGrid, "##scgrid", i18n::N_("Grid"), i18n::N_("Tap the tempo, nudge the grid, meter, bar 1, analyse again"), "grid"},
                            {CardSound, "##scsound", i18n::N_("Sound"), i18n::N_("Keys, speed, base note, the envelope, filter and mix"), "sound"}};
    for (const C& c : cs) {
      const float w = narrow ? 54.0f : 60.0f;
      rx -= w;
      if (button(c.id, ImVec2(rx, cTop), ImVec2(w, kHeaderCtl), c.text, e.card == c.card, c.hint))
        toggleCard(e, c.card, ImVec2(rx + w * 0.5f, cTop + kHeaderCtl), true);
      noteBtn(e, c.trace);
      rx -= 4;
    }
  }
  rx -= 8;
  // how it follows the project
  {
    static const char* const l[3] = {i18n::N_("Stretch"), i18n::N_("Resample"), i18n::N_("Off")};
    static const char* const h[3] = {i18n::N_("Follow the project tempo time-stretched: the pitch stays"),
                                     i18n::N_("Follow the project tempo sped up or slowed down: the pitch follows (vinyl)"),
                                     i18n::N_("Play at the sample's own tempo")};
    const int sync = static_cast<int>(std::lround(paramOf(a, t, SmSync)));
    int cur = sync == SyncResample ? 1 : sync == SyncOriginal ? 2 : 0;
    const float w = narrow ? 172.0f : 204.0f;
    rx -= w;
    if (pills(a, "##sfollow", ImVec2(rx, cTop), w, kHeaderCtl, l, 3, cur, h)) pui::setParamValue(a, t, SmSync, cur == 1 ? SyncResample : cur == 2 ? SyncOriginal : SyncStretch, false);
    e.btns.push_back({"followstretch", ImVec2(rx + w / 6, cTop + kHeaderCtl * 0.5f)});
    e.btns.push_back({"followresample", ImVec2(rx + w * 0.5f, cTop + kHeaderCtl * 0.5f)});
    e.btns.push_back({"followoff", ImVec2(rx + w * 5 / 6, cTop + kHeaderCtl * 0.5f)});
    if (wide) {
      const float lw = inlineLabel(nullptr, a, ImVec2(0, 0), 0, i18n::N_("FOLLOW"));
      rx -= lw;
      inlineLabel(dl, a, ImVec2(rx, cTop), kHeaderCtl, i18n::N_("FOLLOW"));
    }
  }
  rx -= 12;
  // the tempo, its fixes, Tap (narrow: Tap is in the Grid card)
  if (!narrow) rx -= 44;
  if (!narrow && button("##stap", ImVec2(rx, cTop), ImVec2(44, kHeaderCtl), i18n::N_("Tap"), false,
                        i18n::N_("Tap along on the beat; after four taps the tempo follows (bar 1 stays)"))) {
    Analysis& g = e.m.grid;
    if (!e.taps.empty() && a.now - e.taps.back() > 2.0) e.taps.clear();
    e.taps.push_back(a.now);
    const double bpm = tapTempo(e.taps);
    if (bpm > 0 && e.taps.size() >= 4 && !g.beats.empty()) {
      Model nm = e.m;
      gridSetBpm(nm.grid, bpm, nm.grid.beats[static_cast<size_t>(std::clamp(nm.grid.bar1, 0, static_cast<int>(nm.grid.beats.size()) - 1))]);
      nm.grid.loopFit = false;
      rechopKeepPattern(nm, sampleOf(e).get());
      commit(a, t, e, nm, i18n::N_("Tap tempo"), 0x5E17A9ull);
    }
  }
  rx -= 4;
  const float bpmW = 104 + 4 + 30 + 4 + 30;
  rx -= bpmW;
  drawBpm(a, t, e, ImVec2(rx, cTop), true);
  const float tempoX = rx;
  // left: kind chip, the name, the facts
  float x = o.x;
  {
    const char* k = kindLabel(m.grid.kind);
    const float tw = a.fontBold->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, k).x;
    const ImVec2 c0(x, std::round(cy - 10)), c1(x + std::round(tw) + 16, std::round(cy + 10));
    dl->AddRectFilled(c0, c1, theme::withAlpha(accent(), 0.14f), theme::kRadius);
    dl->AddRect(c0, c1, theme::withAlpha(accent(), 0.5f), theme::kRadius);
    textC(dl, a.fontBold, theme::kFontLabel, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), theme::isDark() ? accent() : P.text, k);
    ImGui::SetCursorScreenPos(c0);
    ImGui::InvisibleButton("##skind", ImVec2(c1.x - c0.x, c1.y - c0.y));
    if (ImGui::IsItemHovered())
      ui::Tooltip(i18n::tr("What Seltmann heard. It picks the automatic chop: breaks on their hits, loops and songs by beats and bars, "
                           "one-shots at their transients."));
    x = c1.x + 10;
  }
  const float room = tempoX - 16 - x;
  crowded(e, "header", room - 40);
  const std::string file = fs::path(e.path).filename().string();
  const float nameW = std::min(room, a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, file.c_str()).x);
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(x, std::round(cy - theme::kFontBody * 0.6f)), std::max(40.0f, room), P.text, file.c_str());
  x += nameW + 10;
  if (tempoX - 16 - x > 60) {
    // The facts in order of use; a part that does not fit whole is left out with those after it
    // (0.7.8: no word cut in half while there is room for the parts before it); only a first part
    // too long for the room is ellipsized.
    std::vector<std::string> parts;
    char b[64];
    const double secs = m.srcFrames / std::max(1.0, m.srcRate);
    if (m.grid.kind != KindOneShot && m.grid.kind != KindAmbient) {
      const int bars = static_cast<int>(std::lround(regionBars(m)));
      std::snprintf(b, sizeof b, "%d %s", bars, bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
      parts.push_back(std::string(b) + (song ? std::string(" ") + i18n::tr("chopped") : std::string()));
    }
    parts.push_back(fmtTime(secs));
    std::snprintf(b, sizeof b, "%d %s", static_cast<int>(m.slices.size()), m.slices.size() == 1 ? i18n::tr("region") : i18n::tr("regions"));
    parts.push_back(b);
    parts.push_back(chopSummary(m));
    if (m.grid.key >= 0 && m.grid.kind != KindBreak && m.grid.keyConfidence >= 0.25) parts.push_back(i18n::tr(keyName(m.grid.key, m.grid.minor)));
    const float avail = tempoX - 16 - x;
    std::string facts;
    for (const std::string& p : parts) {
      const std::string next = facts.empty() ? p : facts + " \xC2\xB7 " + p;
      if (!facts.empty() && a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, next.c_str()).x > avail) break;
      facts = next;
    }
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x, std::round(cy - theme::kFontCaption * 0.6f)), avail, P.textDim, facts.c_str());
  }
  (void)song;
}

// ------------------------------------------------------------------ action row (what to do)
// Play; how it is chopped (More... opens every chop mode); the tool; then everything that takes it
// out (drag the loop or the notes, make loop files, send the pattern to the project, linked).
void drawActions(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ch = 28, top = std::round(o.y + (H - ch) * 0.5f);
  const bool labels = W >= 1150, narrow = W < 1060;  // (narrow: shorter pills, a shorter Send)
  float x = o.x;
  e.playBtn = ImVec2(x + 28, top + ch * 0.5f);
  if (button("##splay", ImVec2(x, top), ImVec2(56, ch), "", e.pattern.on,
             e.pattern.on ? i18n::N_("Stop the pattern") : i18n::N_("Play the pattern (or hold a key in the octave below the base note)"),
             static_cast<int>(e.pattern.on ? ui::Icon::Stop : ui::Icon::Play), !e.pattern.on))
    togglePattern(a, t, e);
  x += 56 + 16;
  // ---- chop size
  {
    if (labels) x += inlineLabel(dl, a, ImVec2(x, top), ch, i18n::N_("CHOP"));
    static const char* const l[7] = {i18n::N_("Auto"), i18n::N_("1 bar"), "1/2", "1/4", "1/8", "1/16", i18n::N_("Hits")};
    static const char* const h[7] = {i18n::N_("The size that suits the material: breaks on their hits in eighths or sixteenths, loops and songs by beats and bars"),
                                     i18n::N_("One region per bar"), i18n::N_("Half a bar per region"), i18n::N_("One beat per region"),
                                     i18n::N_("Eighth notes"), i18n::N_("Sixteenth notes"), i18n::N_("Cut at the hits (drum hits, note starts)")};
    int cur = chopSizeOf(e.m);
    const float w = labels ? 316.0f : narrow ? 252.0f : 280.0f;
    if (pills(a, "##schopsize", ImVec2(x, top), w, ch, l, 7, cur, h, false) && cur >= 0) setChopSize(a, t, e, cur);
    for (int i = 0; i < 7; ++i) {
      static const char* const tn[7] = {"chopauto", "chop1bar", "chophalf", "chopbeat", "chop8", "chop16", "chophits"};
      e.btns.push_back({tn[i], ImVec2(x + w * (i + 0.5f) / 7, top + ch * 0.5f)});
    }
    x += w + 4;
    if (button("##schopmore", ImVec2(x, top), ImVec2(64, ch), i18n::N_("More..."), e.card == CardChop,
               i18n::N_("Every chop mode: grid, hits, sections, equal parts, grains, by hand; snapping")))
      toggleCard(e, CardChop, ImVec2(x + 32, top + ch), true);
    noteBtn(e, "chopmore");
    x += 64 + 16;
  }
  // ---- the tool: what a click in the waveform does
  {
    if (labels) x += inlineLabel(dl, a, ImVec2(x, top), ch, i18n::N_("TOOL"));
    static const char* const l[3] = {i18n::N_("Play"), i18n::N_("Cut"), i18n::N_("Grid")};
    static const char* const h[3] = {i18n::N_("Click a region to play it and open its card"),
                                     i18n::N_("Double-click to add a cut, drag a cut to move it, right-click a cut to remove it"),
                                     i18n::N_("Drag the bar-1 flag to move the grid, Alt+drag a bar line to stretch it")};
    const float w = narrow ? 126.0f : 156.0f;
    pills(a, "##stool", ImVec2(x, top), w, ch, l, 3, e.tool, h);
    e.btns.push_back({"toolplay", ImVec2(x + w / 6, top + ch * 0.5f)});
    e.btns.push_back({"toolcut", ImVec2(x + w * 0.5f, top + ch * 0.5f)});
    e.btns.push_back({"toolgrid", ImVec2(x + w * 5 / 6, top + ch * 0.5f)});
    x += w + 16;
  }
  // ---- right: send to project (linked), make loops, drag handles
  float rx = o.x + W;
  LinkState ls = LinkNone;
  const ObjectId lp = linkedPatternOf(a, t, &ls);
  if (ls == LinkEdited) {
    rx -= 64;
    if (button("##srelink", ImVec2(rx, top), ImVec2(64, ch), i18n::N_("Relink"), false,
               i18n::N_("The sent pattern was edited in the piano roll, so it stopped following. Relink: it follows the sequence again (your edits go)")))
      relink(a, t);
    noteBtn(e, "relink");
    const char* bt = i18n::tr("edited");
    const float bw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, bt).x + 14;
    rx -= 4 + bw;
    const ImVec2 b0(rx, top + 6), b1(rx + bw, top + ch - 6);
    dl->AddRectFilled(b0, b1, theme::withAlpha(P.meterMid, 0.22f), theme::kRadius);
    dl->AddRect(b0, b1, theme::withAlpha(P.meterMid, 0.8f), theme::kRadius);
    textC(dl, a.fontMono, theme::kFontLabel, ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), P.text, bt);
    rx -= 6;
  }
  {
    const char* sl = ls == LinkFollowing ? (narrow ? i18n::N_("Send \xC2\xB7 linked") : i18n::N_("Send to project \xC2\xB7 linked"))
                                         : (narrow ? i18n::N_("Send") : i18n::N_("Send to project"));
    const float sw = std::round(a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, i18n::tr(sl)).x + 40);
    rx -= sw;
    if (button("##ssend", ImVec2(rx, top), ImVec2(sw, ch), sl, false,
               ls == LinkFollowing ? i18n::N_("The sent pattern follows the sequence until its notes are edited. Click to send another one")
                                   : i18n::N_("Add a pattern with these steps as notes on this channel and place it in the playlist; it follows later edits of the sequence"),
               static_cast<int>(ui::Icon::Pattern), true))
      sendToProject(a, t, e);
    noteBtn(e, "send");
  }
  (void)lp;
  rx -= 8 + 104;
  if (button("##smake", ImVec2(rx, top), ImVec2(104, ch), i18n::N_("Make loops"), false,
             i18n::N_("Render the pattern, the best phrases and the sections as loops at the project tempo into the \"Seltmann loops\" folder")))
    makeLoops(a, t, e);
  if (e.loopJob && !e.loopJob->finished) {
    const float pr = static_cast<float>(e.loopJob->done) / std::max(1, e.loopJob->total.load());
    dl->AddRectFilled(ImVec2(rx + 6, top + ch - 4), ImVec2(rx + 98, top + ch - 2), P.surface900, 1);
    dl->AddRectFilled(ImVec2(rx + 6, top + ch - 4), ImVec2(rx + 6 + 92 * pr, top + ch - 2), accent(), 1);
  }
  {
    const float mw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "MIDI").x + 30);
    rx -= 10 + mw;
    dragChip(a, t, e, "##smidi", ImVec2(rx, top), ch, "MIDI", "midi", e.m.pattern.name + " notes",
             i18n::N_("Drag the pattern's notes into the piano roll or the playlist"));
    const float lw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "LOOP").x + 30);
    rx -= 6 + lw;
    dragChip(a, t, e, "##sploop", ImVec2(rx, top), ch, "LOOP", "pattern", e.m.pattern.name,
             i18n::N_("Drag the pattern out as an audio loop at the project tempo (playlist, channel rack, browser)"));
    if (labels && rx - x > 80) {
      const float dw = inlineLabel(nullptr, a, ImVec2(0, 0), 0, i18n::N_("DRAG OUT"));
      inlineLabel(dl, a, ImVec2(rx - dw, top), ch, i18n::N_("DRAG OUT"));
    }
  }
  crowded(e, "actions", rx - x);
}

// ------------------------------------------------------------------ Compact: one row
// Play, what it is, the tempo, a short chop choice, the layout, the drag handles and a menu with
// everything else (the cards, load, record, make loops, send, the tool, follow and the height).
void drawCompactRow(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ch = 26, top = std::round(o.y + (H - ch) * 0.5f);
  float x = o.x;
  e.playBtn = ImVec2(x + 22, top + ch * 0.5f);
  if (button("##splay", ImVec2(x, top), ImVec2(44, ch), "", e.pattern.on, e.pattern.on ? i18n::N_("Stop the pattern") : i18n::N_("Play the pattern"),
             static_cast<int>(e.pattern.on ? ui::Icon::Stop : ui::Icon::Play), !e.pattern.on))
    togglePattern(a, t, e);
  x += 44 + 10;
  // right to left: menu, MIDI, LOOP, layout, chop, tempo
  float rx = o.x + W - 30;
  ImGui::SetCursorScreenPos(ImVec2(rx, top));
  if (ui::IconButton("##smore", ui::Icon::More, false, ch, i18n::tr("More: the cards, load, record, make loops, send to project, the tool, follow, the height")))
    ImGui::OpenPopup("##scompactmenu");
  noteBtn(e, "menu");
  const float mw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "MIDI").x + 30);
  rx -= 6 + mw;
  dragChip(a, t, e, "##smidi", ImVec2(rx, top), ch, "MIDI", "midi", e.m.pattern.name + " notes", i18n::N_("Drag the pattern's notes into the piano roll or the playlist"));
  const float lw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "LOOP").x + 30);
  rx -= 4 + lw;
  dragChip(a, t, e, "##sploop", ImVec2(rx, top), ch, "LOOP", "pattern", e.m.pattern.name, i18n::N_("Drag the pattern out as an audio loop at the project tempo"));
  const float pw = std::clamp(W * 0.16f, 120.0f, 180.0f);
  rx -= 10 + pw;
  drawLayoutPicker(a, t, e, ImVec2(rx, top), pw, ch);
  {
    static const char* const l[3] = {i18n::N_("Auto"), "1/8", "1/16"};
    static const int map[3] = {0, 4, 5};
    const int full = chopSizeOf(e.m);
    int cur = full == 0 ? 0 : full == 4 ? 1 : full == 5 ? 2 : -1;
    const float w = 132;
    rx -= 10 + w;
    if (pills(a, "##schopmini", ImVec2(rx, top), w, ch, l, 3, cur) && cur >= 0) setChopSize(a, t, e, map[cur]);
  }
  rx -= 10 + 104;
  drawBpm(a, t, e, ImVec2(rx, top), false);
  crowded(e, "compactrow", rx - 10 - x - 40);
  // the kind and the file in what is left
  {
    const char* k = kindLabel(e.m.grid.kind);
    const float tw = a.fontBold->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, k).x;
    const float cy = top + ch * 0.5f;
    if (rx - x > tw + 60) {
      const ImVec2 c0(x, std::round(cy - 10)), c1(x + std::round(tw) + 14, std::round(cy + 10));
      dl->AddRectFilled(c0, c1, theme::withAlpha(accent(), 0.14f), theme::kRadius);
      dl->AddRect(c0, c1, theme::withAlpha(accent(), 0.5f), theme::kRadius);
      textC(dl, a.fontBold, theme::kFontLabel, ImVec2((c0.x + c1.x) * 0.5f, cy), theme::isDark() ? accent() : P.text, k);
      x = c1.x + 8;
    }
    char facts[64];
    std::snprintf(facts, sizeof facts, "  %d %s", static_cast<int>(e.m.slices.size()), i18n::tr("regions"));
    const std::string s = fs::path(e.path).filename().string() + facts;
    theme::textEllipsis(dl, a.fontBold, theme::kFontCaption, ImVec2(x, std::round(cy - theme::kFontCaption * 0.6f)), rx - 10 - x, P.text, s.c_str());
  }
  // the menu
  if (ImGui::BeginPopup("##scompactmenu")) {
    const ImVec2 at = ImGui::GetIO().MousePos;
    if (ImGui::MenuItem(i18n::tr("Sound..."))) openCard(e, CardSound, at, true);
    if (ImGui::MenuItem(i18n::tr("Grid..."))) openCard(e, CardGrid, at, true);
    if (ImGui::MenuItem(i18n::tr("Loops..."))) openCard(e, CardLoops, at, true);
    if (ImGui::MenuItem(i18n::tr("Chop..."))) openCard(e, CardChop, at, true);
    if (ImGui::MenuItem(i18n::tr("Steps..."))) openCard(e, CardSteps, at, true);
    if (!e.takes.empty() && ImGui::MenuItem(i18n::tr("Takes..."))) openCard(e, CardTakes, at, true);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Half the tempo (/2)"))) {
      Model m = e.m;
      gridHalve(m.grid);
      rechopKeepPattern(m, sampleOf(e).get());
      commit(a, t, e, m, i18n::N_("Half tempo"));
    }
    if (ImGui::MenuItem(i18n::tr("Double the tempo (x2)"))) {
      Model m = e.m;
      gridDouble(m.grid);
      rechopKeepPattern(m, sampleOf(e).get());
      commit(a, t, e, m, i18n::N_("Double tempo"));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Load..."))) openLoadDialog(a, t, e);
    if (ImGui::MenuItem(i18n::tr("Record take"))) openRecorder(a, t);
    if (ImGui::MenuItem(i18n::tr("Make loops"))) makeLoops(a, t, e);
    if (ImGui::MenuItem(i18n::tr("Send to project"))) sendToProject(a, t, e);
    LinkState ls = LinkNone;
    linkedPatternOf(a, t, &ls);
    if (ls == LinkEdited && ImGui::MenuItem(i18n::tr("Relink the sent pattern"))) relink(a, t);
    if (ls != LinkNone && ImGui::MenuItem(i18n::tr("Unlink the sent pattern"))) unlink(a, t);
    ImGui::Separator();
    if (ImGui::BeginMenu(i18n::tr("Tool"))) {
      static const char* const l[3] = {i18n::N_("Play"), i18n::N_("Cut"), i18n::N_("Grid")};
      for (int i = 0; i < 3; ++i)
        if (ImGui::MenuItem(i18n::tr(l[i]), nullptr, e.tool == i)) e.tool = i;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Follow"))) {
      const int sync = static_cast<int>(std::lround(paramOf(a, t, SmSync)));
      if (ImGui::MenuItem(i18n::tr("Stretch"), nullptr, sync == SyncStretch)) pui::setParamValue(a, t, SmSync, SyncStretch, false);
      if (ImGui::MenuItem(i18n::tr("Resample"), nullptr, sync == SyncResample)) pui::setParamValue(a, t, SmSync, SyncResample, false);
      if (ImGui::MenuItem(i18n::tr("Off"), nullptr, sync == SyncOriginal)) pui::setParamValue(a, t, SmSync, SyncOriginal, false);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Height"))) {
      static const char* const l[4] = {i18n::N_("Auto"), i18n::N_("Compact"), i18n::N_("Standard"), i18n::N_("Full")};
      const int choice = heightChoice(a.settings.getString("seltmann.height"));
      for (int i = 0; i < 4; ++i)
        if (ImGui::MenuItem(i18n::tr(l[i]), nullptr, choice == i - 1)) {
          a.settings.set("seltmann.height", heightChoiceName(i - 1));
          a.settings.save(Settings::defaultPath());
        }
      ImGui::EndMenu();
    }
    editorKeyMenu(a, t, e);
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ the hint line
// What the tool does in the waveform on the left, the editor's own keys on the right.
void drawHint(App& a, Ed& e, ImVec2 o, float W, float H) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const char* how = e.tool == ToolCut    ? i18n::tr("Cut: double-click the waveform to add a cut, drag a cut to move it, right-click a cut to remove it")
                    : e.tool == ToolGrid ? i18n::tr("Grid: drag the bar-1 flag to move the grid, Alt+drag a bar line to stretch it")
                                         : i18n::tr("Play: click a region to play it and open its card. Wheel: pitch. Drag a cut to resize, a block to move it");
  std::string keysText;
  {
    struct K {
      int a, b;
      const char* text;
    };
    static const K ks[5] = {{KeyPrevRegion, KeyNextRegion, i18n::N_("region")}, {KeySplit, -1, i18n::N_("split")}, {KeyJoin, -1, i18n::N_("join")},
                            {KeyChop1, KeyChop6, i18n::N_("chop")}, {KeyPlayRegion, -1, i18n::N_("play")}};
    for (const K& k : ks) {
      if (!editorKeyFree(editorChord(k.a))) continue;
      std::string c = keycaps::keyName(editorChord(k.a).key);
      if (k.b >= 0 && editorKeyFree(editorChord(k.b))) c += k.b == KeyNextRegion ? std::string(" ") + keycaps::keyName(editorChord(k.b).key) : std::string("-") + keycaps::keyName(editorChord(k.b).key);
      keysText += (keysText.empty() ? "" : "  \xC2\xB7  ") + c + " " + i18n::tr(k.text);
    }
  }
  const float kw = keysText.empty() ? 0.0f : a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, keysText.c_str()).x;
  const float y = std::round(o.y + (H - theme::kFontCaption) * 0.5f - 1);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(o.x + 2, y), W - kw - 24, P.textDim, how);
  if (kw > 0 && kw < W * 0.5f) textR(dl, a.fontMono, theme::kFontLabel, ImVec2(o.x + W - 2, y + 1), P.textFaint, keysText.c_str());
}

// ------------------------------------------------------------------ songs: section buttons (P11)
void drawSectionButtons(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  float x = o.x;
  x += inlineLabel(dl, a, ImVec2(x, o.y), H, i18n::N_("SECTIONS"));
  SamplePtr smp = sampleOf(e);
  const double N = static_cast<double>(m.srcFrames);
  for (size_t i = 0; i < m.grid.sections.size() && i < 32; ++i) {
    const auto& s = m.grid.sections[i];
    char lb[24];
    std::snprintf(lb, sizeof lb, "%s%zu", slotName(s.label), i + 1);
    const float w = std::round(a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, lb).x + 22);
    if (x + w > o.x + W) break;
    const int64_t f0 = static_cast<int64_t>(std::llround(std::max(0.0, frameAt(m.grid, s.firstBeat))));
    const int64_t f1 = static_cast<int64_t>(std::llround(std::min(N, frameAt(m.grid, s.endBeat))));
    const bool on = m.regionStart == f0 && m.regionEnd == f1;
    char id[24];
    std::snprintf(id, sizeof id, "##ssec%zu", i);
    if (button(id, ImVec2(x, o.y), ImVec2(w, H), lb, on, i18n::N_("Chop this section (the box in the whole-song strip takes its bars)")) && !on) {
      Model nm = e.m;
      nm.regionStart = f0;
      nm.regionEnd = f1;
      if (nm.chop.mode == ChopManual) nm.chop.mode = ChopGrid;
      rechopKeepPattern(nm, smp.get());
      commit(a, t, e, nm, i18n::N_("Select a section"));
      e.selSlice = 0;
    }
    dl->AddRectFilled(ImVec2(x + 3, o.y + H - 3), ImVec2(x + w - 3, o.y + H - 1), slotColor(s.label, 0.9f), 1);
    x += w + 4;
  }
  textR(dl, a.fontRegular, theme::kFontCaption, ImVec2(o.x + W, o.y + (H - theme::kFontCaption) * 0.5f - 1), P.textFaint,
        x < o.x + W - 360 ? i18n::tr("Double-click a section in the strip to chop it; the box keeps whole bars") : "");
}

// ------------------------------------------------------------------ dev trace
void trace(Ed& e, const char* state, const LiveView& lv) {
  static const bool on = std::getenv("GRIDLINE_SELTMANN_TRACE") != nullptr;
  if (!on) return;
  // Nothing while the window is still finding its place (its first frames): the traced points are
  // where a pointer can reach them only once it stays put.
  {
    const ImGuiWindow* root = ImGui::GetCurrentWindowRead()->RootWindow;
    const ImVec2 at = root ? root->Pos : ImVec2(0, 0);
    const bool moved = at.x != e.tracePos.x || at.y != e.tracePos.y;
    e.tracePos = at;
    if (moved) return;
  }
  char b[512];
  // K-147: traced points are where they show on screen (the editor may be drawn reduced or magnified)
  auto scr = [](ImVec2 q) { return edzoomui::toScreenHere(q); };
  std::snprintf(b, sizeof b, "seltmann: state %s kind %s bpm %.2f slices %zu preset \"%s\" pattern %d playing %d running %d", state,
                e.hasModel ? kindName(e.m.grid.kind) : "-", e.hasModel ? e.m.grid.bpm : 0.0, e.hasModel ? e.m.slices.size() : 0,
                e.hasModel ? e.m.preset.c_str() : "", e.pattern.on ? 1 : 0, e.pattern.on && lv.step >= 0 ? 1 : 0, lv.patternRunning ? 1 : 0);
  std::string line = b;
  if (e.hasModel) {  // J-149: the sequence as letters, and the regions
    std::string ord;
    for (const auto& st : e.m.pattern.steps) {
      const int k = stepSlice(e.m.pattern, st, static_cast<int>(e.m.slices.size()));
      ord += (ord.empty() ? "" : " ") + regionName(k);
      if (ord.size() > 160) break;
    }
    line += " order \"" + ord + "\"";
    // J-185: the slice pitches, the start, the step unit and the first cuts
    std::string pit;
    for (size_t k = 0; k < e.m.slices.size(); ++k)
      if (std::fabs(e.m.slices[k].pitch) >= 0.005f) pit += (pit.empty() ? "" : " ") + regionName(static_cast<int>(k)) + ":" + pitchText(e.m.slices[k].pitch);
    line += " pitch \"" + pit + "\" start " + regionName(sequenceStartRegion(e.m));
    std::snprintf(b, sizeof b, " unit %.6g chop %d cuts", e.m.pattern.unitBeats, e.m.chop.mode);
    line += b;
    for (size_t k = 1; k < e.m.slices.size() && k < 5; ++k) line += " " + std::to_string(e.m.slices[k].start);
    // the steps' own pitches ("<step number>:<semitones>", 1 = the first step)
    std::string sp;
    for (size_t i = 0; i < e.m.pattern.steps.size(); ++i)
      if (std::fabs(e.m.pattern.steps[i].pitch) >= 0.005f) sp += (sp.empty() ? "" : " ") + std::to_string(i + 1) + ":" + pitchText(e.m.pattern.steps[i].pitch);
    line += " stepst \"" + sp + "\"";
    // J-214: the steps' own lengths ("<step number>:<beats>"), where the steps end and one cycle (beats)
    std::string ln;
    for (size_t i = 0; i < e.m.pattern.steps.size(); ++i)
      if (e.m.pattern.steps[i].beats > 0) {
        std::snprintf(b, sizeof b, "%s%zu:%.6g", ln.empty() ? "" : " ", i + 1, e.m.pattern.steps[i].beats);
        ln += b;
        if (ln.size() > 200) break;
      }
    std::snprintf(b, sizeof b, " lens \"%s\" stepsbeats %.6g cycle %.6g", ln.c_str(), e.m.pattern.stepsBeats(), cycleBeats(e.m));
    line += b;
  }
  std::snprintf(b, sizeof b, "\nseltmann: body %.0f %.0f", e.body.x, e.body.y);  // J-152: the measured size
  line += b;
  if (std::string(state) == "empty") {
    std::snprintf(b, sizeof b, "\nseltmann: zone %.0f %.0f %.0f %.0f", scr(e.zone0).x, scr(e.zone0).y, scr(e.zone1).x, scr(e.zone1).y);
    line += b;
    std::snprintf(b, sizeof b, "\nseltmann: recentry %.0f %.0f", scr(e.recEntry).x, scr(e.recEntry).y);  // J-218
    line += b;
  }
  if (std::string(state) == "ready") {
    for (size_t i = 0; i < e.cards.size() && i < 3; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: card \"%s\" %.0f %.0f", e.cards[i].first.c_str(), scr(e.cards[i].second).x, scr(e.cards[i].second).y);
      line += b;
    }
    std::snprintf(b, sizeof b, "\nseltmann: play %.0f %.0f", scr(e.playBtn).x, scr(e.playBtn).y);
    line += b;
    for (size_t i = 0; i < e.seqCentres.size() && i < 24; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: seq %zu %.0f %.0f", i, scr(e.seqCentres[i]).x, scr(e.seqCentres[i]).y);
      line += b;
    }
    // J-185: the START flag, the cut handles (waveform, lane), the pads' pitch readouts, the open region menu
    std::snprintf(b, sizeof b, "\nseltmann: startflag %.0f %.0f", scr(e.startFlag).x, scr(e.startFlag).y);
    line += b;
    for (size_t i = 0; i < e.cutPts.size() && i < 4; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: cut %zu %.0f %.0f", i + 1, scr(e.cutPts[i]).x, scr(e.cutPts[i]).y);
      line += b;
    }
    for (size_t i = 0; i < e.laneLenPts.size() && i < 24; ++i) {  // J-214: the step length handles (right edges)
      std::snprintf(b, sizeof b, "\nseltmann: steplen %zu %.0f %.0f", i, scr(e.laneLenPts[i]).x, scr(e.laneLenPts[i]).y);
      line += b;
    }
    for (size_t i = 0; i < e.padChips.size() && i < 4; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: padchip %zu %.0f %.0f", i, scr(e.padChips[i]).x, scr(e.padChips[i]).y);
      line += b;
    }
    for (const auto& [tag, pt] : e.btns) {  // 0.7.8: named controls (tools, header cards, chop sizes, ...)
      std::snprintf(b, sizeof b, "\nseltmann: btn %s %.0f %.0f", tag.c_str(), scr(pt).x, scr(pt).y);
      line += b;
    }
    for (const auto& [tag, pt] : e.menuPts) {
      std::snprintf(b, sizeof b, "\nseltmann: menu %s %.0f %.0f", tag.c_str(), scr(pt).x, scr(pt).y);
      line += b;
    }
  }
  // J-108: scripted runs wait for these ("waitfor seltmann-ready|seltmann-playing <seconds>")
  if (std::string(state) == "ready") devsig::reach("seltmann-ready");
  if (e.pattern.on && lv.step >= 0) devsig::reach("seltmann-playing");
  if (std::string(state) == "ready" && !e.pattern.on && !lv.patternRunning) devsig::reach("seltmann-stopped");  // J-110
  if (line != e.lastTrace) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    e.lastTrace = line;
  }
}

}  // namespace

// ------------------------------------------------------------------ 0.7.8 shared pieces
void noteBtn(Ed& e, const char* name) {
  const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
  e.btns.push_back({name, ImVec2(std::round((a0.x + a1.x) * 0.5f), std::round((a0.y + a1.y) * 0.5f))});
}

int chopSizeOf(const Model& m) {
  static const int divs[6] = {-1, Div1Bar, DivHalf, DivBeat, Div8th, Div16th};
  if (m.chop.autoSize) return 0;
  if (m.chop.mode == ChopTransients) return 6;
  if (m.chop.mode == ChopGrid)
    for (int i = 1; i < 6; ++i)
      if (m.chop.division == divs[i]) return i;
  return -1;
}

void setChopSize(App& a, const PluginTarget& t, Ed& e, int which) {
  static const int divs[6] = {-1, Div1Bar, DivHalf, DivBeat, Div8th, Div16th};
  if (which < 0 || which > 6 || which == chopSizeOf(e.m)) return;
  Model nm = e.m;
  nm.chop.autoSize = which == 0;
  if (which >= 1 && which <= 5) {
    nm.chop.mode = ChopGrid;
    nm.chop.division = divs[which];
    nm.chop.hitSnap = nm.grid.kind == KindBreak;
  } else if (which == 6) {
    nm.chop.mode = ChopTransients;
  }
  commitChop(a, t, e, nm, i18n::N_("Chop size"));
}

// ---- layout previews: the layout rendered on the current chop, played as a loop while it is held
namespace {
std::vector<std::thread>& previewThreads() {
  static std::vector<std::thread> threads;  // joined when the editor library unloads (static destruction)
  struct Joiner {
    ~Joiner() {
      for (auto& th : threads)
        if (th.joinable()) th.join();
    }
  };
  static Joiner joiner;
  (void)joiner;
  // finished ones are joined as new ones start (a preview renders in a fraction of a second)
  return threads;
}

void tracePreview(Ed& e, const char* what) {
  static const bool on = std::getenv("GRIDLINE_SELTMANN_TRACE") != nullptr;
  if (!on) return;
  std::printf("seltmann: preview %s \"%s\"\n", what, e.previewName.c_str());
  std::fflush(stdout);
}
}  // namespace

void previewStart(App& a, const PluginTarget& t, Ed& e, const Preset& p) {
  const PluginState* sp = t.state(a.doc.song());
  SamplePtr src = sampleOf(e);
  if (!sp || !src || !e.hasModel) return;
  previewStop(a, t, e);
  if (e.pattern.on) {  // the pattern pauses while a layout is heard
    togglePattern(a, t, e);
    e.previewResume = true;
  }
  e.previewName = p.name;
  auto job = std::make_shared<PreviewJob>();
  e.previewJob = job;
  RenderOpts o = optsOf(a, *sp);
  const Model base = e.m;
  const Preset pr = p;
  const uint32_t seed = e.seed;
  auto& threads = previewThreads();
  for (auto it = threads.begin(); it != threads.end();) {
    if (it->joinable() && threads.size() > 4) {
      it->join();
      it = threads.erase(it);
    } else {
      ++it;
    }
  }
  threads.emplace_back([job, base, src, pr, seed, o]() mutable { gl::threads::setName("gl-seltmann-job");
    std::vector<std::pair<int, float>> params;
    const Model pv = previewModel(base, src.get(), pr, seed, &params);
    for (const auto& [k, v] : params)
      if (k >= 0 && k < static_cast<int>(o.params.size())) o.params[static_cast<size_t>(k)] = v;
    std::vector<float> L, R;
    if (renderPatternLoop(pv, src, o, L, R) && !L.empty()) {
      auto s = std::make_shared<Sample>();
      s->sampleRate = o.sampleRate;
      s->left = std::move(L);
      s->right = std::move(R);
      job->sample = s;
    }
    job->done = true;
  });
  tracePreview(e, "start");
}

void previewStop(App& a, const PluginTarget& t, Ed& e) {
  if (e.previewName.empty()) return;
  tracePreview(e, "stop");
  if (e.previewPlaying) a.engine.preview(nullptr);
  e.previewPlaying = false;
  e.previewName.clear();
  e.previewJob.reset();
  if (e.previewResume && !e.pattern.on) togglePattern(a, t, e);
  e.previewResume = false;
}

void previewTick(App& a, const PluginTarget& t, Ed& e) {
  if (!e.previewName.empty() && e.previewJob && e.previewJob->done && !e.previewPlaying) {
    if (e.previewJob->sample) {
      a.engine.preview(e.previewJob->sample, true, 0);
      e.previewPlaying = true;
      tracePreview(e, "playing");
    }
  }
  // the item being heard is no longer held or under the pointer: stop
  if (!e.previewName.empty() && !e.previewSeen) previewStop(a, t, e);
  e.previewSeen = false;
  if (!ImGui::IsMouseDown(0) && e.previewHeld) e.previewHeld = false;
}

void keepLayout(App& a, const PluginTarget& t, Ed& e, const Preset& p) {
  previewStop(a, t, e);
  // a sequence made by hand stays under Recent as "Custom (yours)"
  if (!isNaturalOrder(e.m) && (e.m.preset.empty() || e.m.pattern.name == kEditedName)) {
    e.custom = e.m;
    e.hasCustom = true;
  }
  const bool again = p.name == e.m.preset && p.gen;
  if (again) ++e.seed;  // a generated layout clicked again: another take
  applyLayout(a, t, e, p, true);
  prefs(a, e).used(p.name);
  savePrefs(a, e);
}

void restoreCustom(App& a, const PluginTarget& t, Ed& e) {
  if (!e.hasCustom) return;
  previewStop(a, t, e);
  commit(a, t, e, e.custom, i18n::N_("Back to your own sequence"));
  e.selStep = -1;
  if (!e.pattern.on) togglePattern(a, t, e);
}

// ---- takes (S0h): a new take into a loaded instance
void useTake(App& a, const PluginTarget& t, const std::string& path, bool keepChop, int64_t frames, double rate) {
  Ed& e = ed(t);
  bool known = false;
  for (const Take& tk : e.takes) known = known || tk.path == path;
  if (!known) e.takes.push_back({path, frames, rate});
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  if (keepChop && e.hasModel && frames > 0 && rate > 0) {
    const Model m = keepChopOnTake(e.m, frames, rate);
    PluginState st = *sp;
    st.data = formatData(path, m);
    setPluginState(a, t, st, i18n::tr("Use a take (keep the chop)"));
  } else {
    PluginState st = *sp;
    st.data = path;
    setPluginState(a, t, st, i18n::tr("Use a take"));
  }
  if (std::getenv("GRIDLINE_SELTMANN_TRACE")) {
    std::printf("seltmann: take \"%s\" keep %d takes %zu\n", path.c_str(), keepChop ? 1 : 0, e.takes.size());
    std::fflush(stdout);
  }
}

// ---- linked send (S0g)
ObjectId linkedPatternOf(App& a, const PluginTarget& t, LinkState* st) {
  if (st) *st = LinkNone;
  if (!t.isChannel()) return kNoId;
  const Song& s = a.doc.song();
  for (auto it = s.patterns.rbegin(); it != s.patterns.rend(); ++it)
    if (it->linkChannel == t.owner) {
      if (st) *st = linkState(*it, t.owner);
      return it->id;
    }
  return kNoId;
}

namespace {
// The model and parameters of a channel (parsed; cached by the state's data).
bool channelModel(const Channel& c, Model& m) {
  static std::string lastData;
  static Model lastModel;
  static bool lastOk = false;
  if (c.plugin.data != lastData) {
    lastData = c.plugin.data;
    std::string path;
    lastModel = Model{};
    lastOk = parseData(c.plugin.data, path, lastModel);
  }
  m = lastModel;
  return lastOk;
}
}  // namespace

void relink(App& a, const PluginTarget& t) {
  const ObjectId pid = linkedPatternOf(a, t);
  const Channel* c = a.doc.song().channel(t.owner);
  Model m;
  if (pid == kNoId || !c || !channelModel(*c, m)) return;
  const std::vector<float> params = c->plugin.params;
  a.doc.editPattern(pid, i18n::tr("Relink the sent pattern"), [&](gl::Pattern& p) { refreshLinked(a.doc.song(), p, t.owner, m, params, true); });
}

void unlink(App& a, const PluginTarget& t) {
  const ObjectId pid = linkedPatternOf(a, t);
  if (pid == kNoId) return;
  a.doc.editPattern(pid, i18n::tr("Unlink the sent pattern"), [&](gl::Pattern& p) {
    p.linkChannel = kNoId;
    p.linkHash = 0;
    p.linkCycles = 1;
  });
}

// Each frame: a linked pattern that still follows takes its channel's sequence. The change joins the
// edit that caused it (a sequence edit, a parameter), so one edit stays one undo step and syncs as one.
void linkTick(App& a) {
  const Song& s = a.doc.song();
  bool any = false;
  for (const auto& p : s.patterns) any = any || p.linkChannel != kNoId;
  if (!any) return;
  // after an undo / redo the song is as it was: nothing to follow
  const uint64_t serial = a.doc.history().stateSerial();
  if (serial == a.doc.moveSerial()) return;
  static std::map<ObjectId, std::string> checked;  // pattern -> what it was last checked against
  for (const auto& p : s.patterns) {
    if (p.linkChannel == kNoId) continue;
    const Channel* c = s.channel(p.linkChannel);
    if (!c || c->plugin.type != kType) continue;
    char key[96];
    std::snprintf(key, sizeof key, "|%llu|%d|%lld|%d", static_cast<unsigned long long>(p.linkHash), s.ppq, static_cast<long long>(s.barTicks()),
                  p.linkCycles);
    std::string k = c->plugin.data + key + std::string(reinterpret_cast<const char*>(c->plugin.params.data()), c->plugin.params.size() * sizeof(float));
    auto it = checked.find(p.id);
    if (it != checked.end() && it->second == k) continue;
    checked[p.id] = k;
    if (linkState(p, p.linkChannel) != LinkFollowing) continue;
    Model m;
    if (!channelModel(*c, m)) continue;
    gl::Pattern np = p;
    if (!refreshLinked(s, np, p.linkChannel, m, c->plugin.params)) continue;
    const ObjectId pid = p.id;
    a.doc.amendStep(serial, [&] { a.doc.editPattern(pid, i18n::tr("Follow the sequence"), [&](gl::Pattern& q) { q = np; }); });
    break;  // (the song changed: the next frame looks at the others)
  }
}

// ---- editor keys (S0i): only keys no key set uses
namespace {
void editorAction(App& a, const PluginTarget& t, Ed& e, int key) {
  const int n = static_cast<int>(e.m.slices.size());
  if (n == 0) return;
  switch (key) {
    case KeyPrevRegion:
    case KeyNextRegion: {
      e.selSlice = std::clamp(e.selSlice + (key == KeyNextRegion ? 1 : -1), 0, n - 1);
      if (e.card == CardRegion || e.card == CardStep) e.card = CardRegion, e.selStep = -1;
      auditionStart(a, t, e, e.selSlice);
      break;
    }
    case KeyPlayRegion: auditionStart(a, t, e, e.selSlice); break;
    case KeyPlayPattern: togglePattern(a, t, e); break;
    case KeySplit: splitRegionUi(a, t, e, e.selSlice, 2); break;
    case KeyJoin:
      if (n > 1) deleteRegionUi(a, t, e, e.selSlice);
      break;
    case KeyReverseStep: {
      const int s = e.selStep >= 0 ? e.selStep : firstStepOf(e.m, e.selSlice);
      if (s < 0 || s >= static_cast<int>(e.m.pattern.steps.size())) break;
      Model nm = e.m;
      nm.pattern.steps[static_cast<size_t>(s)].reverse = !nm.pattern.steps[static_cast<size_t>(s)].reverse;
      markEdited(nm);
      commit(a, t, e, nm, i18n::N_("Play the step backwards"));
      break;
    }
    case KeyToolPlay: e.tool = ToolPlay; break;
    default:
      if (key >= KeyChop1 && key <= KeyChop6) setChopSize(a, t, e, key - KeyChop1);
      break;
  }
}
}  // namespace

void editorKeys(App& a, const PluginTarget& t, Ed& e) {
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || keys::capturing() || !e.hasModel) return;
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && e.card == CardNone) return;  // (or its card has the focus)
  static bool freeKeys[KeyCount];
  static bool known = false;
  if (!known) {
    for (int k = 0; k < KeyCount; ++k) freeKeys[k] = editorKeyFree(editorChord(k));
    known = true;
  }
  uint8_t mods = 0;
  if (io.KeyShift) mods |= keys::kShift;
  if (io.KeyCtrl) mods |= keys::kCtrl;
  if (io.KeyAlt) mods |= keys::kAlt;
  if (io.KeySuper) mods |= keys::kCmd;
  for (int k = 0; k < KeyCount; ++k) {
    if (!freeKeys[k]) continue;
    const keys::Chord c = editorChord(k);
    if (c.mods != mods) continue;
    if (a.typingKeyboard && keys::playsOnTypingKeyboard(c)) continue;  // the typing keyboard plays it
    if (k == KeyToolPlay && e.card != CardNone) continue;              // Esc closes the card first
    if (ImGui::IsKeyPressed(keys::toImGui(c.key), false)) {
      if (std::getenv("GRIDLINE_SELTMANN_TRACE")) {
        std::printf("seltmann: key %s\n", editorKeyLabel(k));
        std::fflush(stdout);
      }
      editorAction(a, t, e, k);
    }
  }
}

void editorKeyMenu(App& a, const PluginTarget& t, Ed& e) {
  if (!ImGui::BeginMenu(i18n::tr("Region and keys"))) return;
  for (int k = 0; k < KeyCount; ++k) {
    const keys::Chord c = editorChord(k);
    const std::string sc = editorKeyFree(c) ? keys::shown(c) : std::string();
    if (ImGui::MenuItem(i18n::tr(editorKeyLabel(k)), sc.empty() ? nullptr : sc.c_str())) editorAction(a, t, e, k);
  }
  ImGui::EndMenu();
}

// ------------------------------------------------------------------ grid tools (Grid card)
void drawGridTools(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Analysis& g = e.m.grid;
  float x = p0.x;
  const float y = p0.y;
  // tap
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("TAP"));
  if (button("##stap", ImVec2(x, y + 16), ImVec2(64, 40), i18n::N_("TAP"), false,
             i18n::N_("Tap along on the beat; after four taps the tempo follows (bar 1 stays)"))) {
    if (!e.taps.empty() && a.now - e.taps.back() > 2.0) e.taps.clear();
    e.taps.push_back(a.now);
    const double bpm = tapTempo(e.taps);
    if (bpm > 0 && e.taps.size() >= 4 && !g.beats.empty()) {
      Model m = e.m;
      gridSetBpm(m.grid, bpm, m.grid.beats[static_cast<size_t>(std::clamp(m.grid.bar1, 0, static_cast<int>(m.grid.beats.size()) - 1))]);
      m.grid.loopFit = false;
      rechopKeepPattern(m, sampleOf(e).get());
      commit(a, t, e, m, i18n::N_("Tap tempo"), 0x5E17A9ull);
    }
  }
  if (!e.taps.empty() && a.now - e.taps.back() < 2.0) {
    char b[24];
    std::snprintf(b, sizeof b, "%zu", e.taps.size());
    dl->AddCircleFilled(ImVec2(x + 60, y + 19), 7, accent(), 12);
    textC(dl, a.fontMono, theme::kFontLabel - 2, ImVec2(x + 60, y + 19), theme::contrastText(accent()), b);
  }
  x += 82;
  // nudge
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("NUDGE THE GRID"));
  {
    struct N {
      const char* id;
      const char* txt;
      double ms;
      const char* hint;
    };
    static const N ns[4] = {{"##sn0", "-10", -10, i18n::N_("Move the grid 10 ms earlier")},
                            {"##sn1", "-1", -1, i18n::N_("Move the grid 1 ms earlier")},
                            {"##sn2", "+1", 1, i18n::N_("Move the grid 1 ms later")},
                            {"##sn3", "+10", 10, i18n::N_("Move the grid 10 ms later")}};
    for (int k = 0; k < 4; ++k)
      if (button(ns[k].id, ImVec2(x + k * 42, y + 16), ImVec2(38, 40), ns[k].txt, false, ns[k].hint)) {
        Model m = e.m;
        gridNudge(m.grid, ns[k].ms * 0.001 * m.srcRate);
        rechopKeepPattern(m, sampleOf(e).get());
        commit(a, t, e, m, i18n::N_("Nudge the grid"), 0x5E17AAull);
      }
  }
  x += 4 * 42 + 18;
  // meter
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("METER"));
  {
    static const char* const ml[3] = {"3/4", "4/4", "6/8"};
    static const char* const mh[3] = {i18n::N_("Three beats per bar"), i18n::N_("Four beats per bar"), i18n::N_("Two dotted beats per bar (compound)")};
    int cur = g.beatsPerBar == 3 ? 0 : (g.beatsPerBar == 2 && g.compound) ? 2 : 1;
    if (pills(a, "##smeter", ImVec2(x, y + 16), 132, 28, ml, 3, cur, mh, true)) {
      Model m = e.m;
      gridSetMeter(m.grid, cur == 0 ? 3 : cur == 2 ? 2 : 4);
      m.grid.compound = cur == 2;
      rechopKeepPattern(m, sampleOf(e).get());
      commit(a, t, e, m, i18n::N_("Set the meter"));
    }
  }
  x += 150;
  // key
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("KEY"));
  dl->AddText(a.fontBold, theme::kFontBody, ImVec2(x + 2, y + 22), g.key >= 0 ? P.text : P.textDim, i18n::tr(keyName(g.key, g.minor)));
  x += 110;
  // how sure the analysis is
  groupLabel(dl, a, ImVec2(x, y), i18n::N_("CONFIDENCE"));
  {
    struct C {
      const char* label;
      double v;
    };
    const C cs[3] = {{i18n::N_("Tempo"), g.confidence}, {i18n::N_("Bar 1"), g.downbeatConfidence}, {i18n::N_("Meter"), g.meterConfidence}};
    for (int k = 0; k < 3; ++k) {
      const float yy = y + 16 + k * 14;
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(x, yy), P.textDim, i18n::tr(cs[k].label));
      const float v = e.m.analysed ? static_cast<float>(std::clamp(cs[k].v, 0.0, 1.0)) : 0.0f;
      const ImVec2 b0(x + 50, yy + 5), b1(x + 120, yy + 9);
      dl->AddRectFilled(b0, b1, P.surface900, 2);
      dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * v, b1.y), v > 0.6f ? P.meterLow : v > 0.3f ? P.meterMid : P.meterHigh, 2);
    }
  }
  x += 140;
  if (button("##sreana", ImVec2(x, y + 16), ImVec2(130, 40), i18n::N_("Analyse again"), false,
             i18n::N_("Run the tempo, beat and section analysis again (keeps the layout)"), static_cast<int>(ui::Icon::Loop))) {
    forgetAnalysis(e.path);
    e.job = requestAnalysis(e.path);
    e.reanalyse = true;
  }
  // help
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, std::min(p1.y - 18, y + 74)), P.textDim,
              i18n::tr("In the waveform: drag the bar-1 flag to move the whole grid, Alt+drag a bar line to stretch the grid between it and bar 1."));
}

}  // namespace sui

// ================================================================== editor

namespace sui {
// 0.7.8 (K-347): the editor's per-frame work before drawing (the model parsed from the state, the
// analysis, the first chop, the dev hooks, finished renders), shared by the Ribbon and the Tracker's
// own screens. False when the state is gone.
bool prepare(App& a, const PluginTarget& t) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  Ed& e = ed(t);
  static bool cacheSet = false;
  if (!cacheSet) {
    setAnalysisCacheDir((fs::path(a.configDir()) / "cache" / "seltmann").string());
    cacheSet = true;
  }
  // dev hooks (screenshots / checks): a file to load, a layout to apply, play the pattern, a card, the tool
  if (!e.demoDone) {
    e.demoDone = true;
    if (const char* tool = std::getenv("GRIDLINE_SELTMANN_TOOL"); tool && *tool) e.tool = std::clamp(std::atoi(tool), 0, ToolCount - 1);
    if (const char* tab = std::getenv("GRIDLINE_SELTMANN_TAB"); tab && *tab) e.presetTab = std::clamp(std::atoi(tab), 0, TabCount + 1);
    if (const char* demo = std::getenv("GRIDLINE_SELTMANN_DEMO"); demo && *demo && sp->data.empty()) {
      PluginState st = *sp;
      st.data = demo;
      setPluginState(a, t, st, "demo");
      sp = t.state(a.doc.song());
      if (!sp) return false;
    }
  }
  // parse the state when it changed (undo, drops, loads)
  if (sp->data != e.data) {
    e.data = sp->data;
    const std::string before = e.path;
    Model m;
    e.hasModel = parseData(sp->data, e.path, m);
    if (e.hasModel) e.m = std::move(m);
    if (e.path != before) {
      e.selSlice = 0, e.selStep = -1, e.rendered.clear(), e.keyPage = 0;
      e.job.reset();
      if (e.pattern.on) togglePattern(a, t, e);
      previewStop(a, t, e);
      closeCard(e);
    }
  }
  if (!e.path.empty() && (!e.job || e.job->path != e.path)) e.job = requestAnalysis(e.path);
  // a finished analysis makes the first model (or replaces the grid after "analyse again")
  if (e.job && e.job->state == 1 && e.job->sample && (!e.hasModel || e.reanalyse)) {
    Model m = defaultModel(*e.job->sample, e.job->result);
    if (e.reanalyse && e.hasModel) {
      // keep the chop settings and the pattern, take the new grid
      Model k = e.m;
      k.grid = m.grid;
      k.analysed = true;
      rechopKeepPattern(k, e.job->sample.get());
      m = k;
    }
    e.reanalyse = false;
    commit(a, t, e, m, i18n::N_("Analyse"));
  }
  if (e.hasModel) {
    if (const char* lay = std::getenv("GRIDLINE_SELTMANN_LAYOUT"); lay && *lay && e.m.preset != lay)
      for (const auto& p : factoryPresets())
        if (p.name == lay) applyLayout(a, t, e, p, false);
    if (const char* pl = std::getenv("GRIDLINE_SELTMANN_PLAY"); pl && *pl == '1' && !e.autoPlayed && !e.pattern.on && e.job && e.job->state == 1) {
      e.autoPlayed = true;  // (once: after a Stop the button stays stopped)
      togglePattern(a, t, e);
    }
  }
  if (e.loopJob && e.loopJob->finished) {
    char b[160];
    std::snprintf(b, sizeof b, "%s %zu %s", i18n::tr("Made"), e.loopJob->files.size(), i18n::tr("loops in the \"Seltmann loops\" folder"));
    a.notify(b);
    browserui::refreshIndex(a);
    e.loopJob.reset();
  }

  return true;
}

const char* chopSummaryText(const Model& m) { return chopSummary(m); }

void wellDropTarget(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl) return;
  std::string file;
  if (pl->IsDataType("GL_FILES") || xdragui::ownsPayload(pl))
    for (const xdrop::Item& it : xdragui::items())
      if (file.empty() && !it.dir && !it.pending && !it.path.empty() && browse::kindOf(it.path) == browse::Kind::Audio) file = it.path;
  if (file.empty()) file = browserui::draggedAudioFile(a, pl);
  if (file.empty()) return;
  if (!ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##seltdrop"))) return;
  if (const ImGuiPayload* x = ImGui::AcceptDragDropPayload(pl->DataType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    if (x->Delivery) {
      xdragui::markConsumed();
      loadAudio(a, t, file);
    } else {
      ImGui::GetForegroundDrawList()->AddRect(p0, p1, accent(), 0, 0, 2.0f);
    }
  }
  ImGui::EndDragDropTarget();
}
}  // namespace sui

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  using namespace sui;
  Ed& e = ed(t);
  static const char* devCard = std::getenv("GRIDLINE_SELTMANN_CARD");
  static const char* devHeight = std::getenv("GRIDLINE_SELTMANN_HEIGHT");
  static const char* devPreview = std::getenv("GRIDLINE_SELTMANN_PREVIEW");
  if (!prepare(a, t)) return true;
  // ---- S0a: the height. Floating (its own window, sized to the editor): Full unless the person
  // chose another; hosted in a layout's region: by the room the region gives.
  const ImGuiWindow* root = ImGui::GetCurrentWindowRead()->RootWindow;
  const bool floating = root && (root->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
  const float hostH = floating ? 0.0f : ImGui::GetContentRegionAvail().y;
  int choice = heightChoice(a.settings.getString("seltmann.height"));
  if (devHeight && *devHeight) choice = heightChoice(devHeight);
  e.height = heightClassFor(hostH, choice);
  e.hosted = !floating;
  {
    const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    e.win0 = w->InnerClipRect.Min, e.win1 = w->InnerClipRect.Max;
    if (e.win1.x - e.win0.x < 200) e.win0 = w->Pos, e.win1 = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
  }

  const float W = std::max(width, 640.0f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const LiveView lv = live(a, t);
  syncPattern(a, t, e, lv);
  e.btns.clear();
  e.crowd.clear();
  // heights at UI scale 1 (the waveform takes what the room leaves: no overlap at any size)
  const float gap = 8;
  const bool hasModel = e.hasModel;
  const bool song = hasModel && (e.m.grid.kind == KindSong || barCount(e.m) > 16);
  const bool strip = song && !e.stripHidden && e.height != HeightCompact;
  const float stripH = strip ? 40.0f + 6.0f : 0.0f, secH = strip && !e.m.grid.sections.empty() ? 26.0f + 6.0f : 0.0f;
  float waveH = 0, seqH = 0, bodyH = 0;
  float fixedH = 0;
  switch (e.height) {
    case HeightCompact:
      seqH = 24;
      fixedH = 30 + 6 + seqH + 4 + 16;
      waveH = std::clamp(hostH > 0 ? hostH - fixedH - 8 : 66.0f, 44.0f, 96.0f);
      break;
    case HeightStandard:
      seqH = 32;
      fixedH = 36 + gap + stripH + seqH + secH + gap + 32 + 6 + 18 + 6 + 84;
      waveH = std::clamp(hostH > 0 ? hostH - fixedH - 8 : 128.0f, 88.0f, 170.0f);
      break;
    default:
      seqH = 34;
      fixedH = 36 + gap + stripH + seqH + secH + gap + 32 + 6 + 18 + gap + 52;
      waveH = std::clamp(hostH > 0 ? hostH - fixedH - 8 : 200.0f, 140.0f, 280.0f);
      break;
  }
  bodyH = fixedH + waveH;
  if (!e.hasModel && e.path.empty()) devAutoRecord(a, t);  // K-145 dev checks
  if (!e.hasModel && e.path.empty() && recorderOpenFor(t)) {
    // J-218: the recorder in place of the drop zone, the same height
    const float rh = std::max(bodyH, e.height == HeightCompact ? 150.0f : 300.0f);
    drawRecorder(a, t, e, o, ImVec2(o.x + W, o.y + rh));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rh + 6));
    ImGui::Dummy(ImVec2(W, 1));
    e.body = ImVec2(W, rh + 6);
    trace(e, "recorder", lv);
    return true;
  }
  if (!e.hasModel) {
    // drop zone (empty), progress (analysing) or a message (unreadable): the same height as the editor
    drawEmpty(a, t, e, o, ImVec2(o.x + W, o.y + bodyH));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + bodyH + 6));
    ImGui::Dummy(ImVec2(W, 1));
    e.body = ImVec2(W, bodyH + 6);
    trace(e, e.path.empty() ? "empty" : (e.job && e.job->state == 2) ? "error" : "analysing", lv);
    return true;
  }
  editorKeys(a, t, e);
  if (devCard && *devCard && e.card == CardNone && !e.cardTouched && e.job && e.job->state == 1) {
    static bool devCardDone = false;
    if (!devCardDone) {
      devCardDone = true;
      openCard(e, std::clamp(std::atoi(devCard), 1, static_cast<int>(CardTakes)), ImVec2(o.x + W * 0.4f, o.y + 36 + gap + waveH + seqH), true);
    }
  }
  float y = o.y;
  float x0 = o.x, cw = W;  // the ribbon's column
  std::string rows;        // (trace: each row's name and its top and bottom, for the no-overlap checks)
  auto row = [&](const char* name, float r0, float r1) {
    char rb[64];
    const ImVec2 q0 = edzoomui::toScreenHere(ImVec2(x0, r0)), q1 = edzoomui::toScreenHere(ImVec2(x0 + cw, r1));
    std::snprintf(rb, sizeof rb, "%s%s %.0f %.0f %.0f %.0f", rows.empty() ? "" : "; ", name, q0.x, q0.y, q1.x, q1.y);
    rows += rb;
  };
  if (e.height == HeightCompact) {
    drawCompactRow(a, t, e, o, W, 30);
    row("compactrow", y, y + 30);
    y += 30 + 6;
  } else {
    drawHeader(a, t, e, o, W, 36, song);
    row("header", y, y + 36);
    y += 36 + gap;
    if (e.height == HeightFull && !recorderOpenFor(t)) {
      // B's library list beside the strip, the waveform and the sequence; the rows under them span the width
      const float libW = W >= 1240 ? 252.0f : 212.0f;
      const float libBottom = y + stripH + waveH + seqH + secH;
      drawLibrary(a, t, e, ImVec2(o.x, y), ImVec2(o.x + libW, libBottom));
      char rb[64];
      const ImVec2 q0 = edzoomui::toScreenHere(ImVec2(o.x, y)), q1 = edzoomui::toScreenHere(ImVec2(o.x + libW, libBottom));
      std::snprintf(rb, sizeof rb, "; library %.0f %.0f %.0f %.0f", q0.x, q0.y, q1.x, q1.y);
      rows += rb;
      x0 = o.x + libW + 10;
      cw = W - libW - 10;
    }
  }
  if (recorderOpenFor(t)) {
    // 0.7.8: a new take into this instance, over the ribbon (Back returns)
    const float rh = std::max(150.0f, o.y + bodyH - y);
    drawRecorder(a, t, e, ImVec2(x0, y), ImVec2(x0 + cw, y + rh));
    ImGui::SetCursorScreenPos(ImVec2(o.x, std::max(o.y + bodyH, y + rh) + 6));
    ImGui::Dummy(ImVec2(W, 1));
    e.body = ImVec2(W, std::max(bodyH, y + rh - o.y) + 6);
    trace(e, "recorder", lv);
    return true;
  }
  if (strip) {
    drawStrip(a, t, e, ImVec2(x0, y), ImVec2(x0 + cw, y + 40), lv);
    row("strip", y, y + 40);
    y += 40 + 6;
  }
  drawWave(a, t, e, ImVec2(x0, y), ImVec2(x0 + cw, y + waveH), lv);
  row("wave", y, y + waveH);
  y += waveH;
  drawSequence(a, t, e, ImVec2(x0, y), ImVec2(x0 + cw, y + seqH), lv);
  row("sequence", y, y + seqH);
  y += seqH;
  if (secH > 0) {
    y += 6;
    drawSectionButtons(a, t, e, ImVec2(x0, y), cw, 26);
    row("sections", y, y + 26);
    y += 26;
  }
  x0 = o.x, cw = W;  // (Full: back to the whole width under the library)
  if (e.height == HeightCompact) {
    y += 4;
    drawHint(a, e, ImVec2(x0, y), cw, 16);
    row("hint", y, y + 16);
    y += 16;
  } else {
    y += gap;
    drawActions(a, t, e, ImVec2(x0, y), cw, 32);
    row("actions", y, y + 32);
    y += 32 + 6;
    drawHint(a, e, ImVec2(x0, y), cw, 18);
    row("hint", y, y + 18);
    y += 18;
    if (e.height == HeightStandard) {
      y += 6;
      drawLayoutStrip(a, t, e, ImVec2(x0, y), cw);
      row("layouts", y, y + 84);
      y += 84;
    } else {
      y += gap;
      drawKeys(a, t, e, ImVec2(x0, y), ImVec2(x0 + cw, y + 52), lv);
      row("keys", y, y + 52);
      y += 52;
    }
  }
  y = std::max(y, o.y + bodyH);
  drawRegionMenu(a, t, e);
  previewTick(a, t, e);
  if (devPreview && *devPreview && e.previewName.empty()) {  // dev: hear a layout (screenshots of a preview)
    if (const Preset* p = findLayout(a, e, devPreview)) previewStart(a, t, e, *p);
  }
  if (devPreview && *devPreview) e.previewSeen = true;
  drawCard(a, t, e, lv);
  ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
  ImGui::Dummy(ImVec2(W, 1));
  e.body = ImVec2(W, y + 6 - o.y);
  if (std::getenv("GRIDLINE_SELTMANN_TRACE")) {
    static const char* const hn[3] = {"compact", "standard", "full"};
    char b[320];
    // the room it had (window or region: its visible rect) and each row; host 0 = its own window
    // (on screen: the room, the card and the rows as they show; the room's window is not zoomed itself)
    const ImVec2 c0 = edzoomui::toScreenHere(e.card0), c1 = edzoomui::toScreenHere(e.card1);
    std::snprintf(b, sizeof b, "seltmann: height %s host %.0f room %.0f %.0f %.0f %.0f tool %d card %d %.0f %.0f %.0f %.0f takes %zu", hn[e.height], hostH, e.win0.x,
                  e.win0.y, e.win1.x, e.win1.y, e.tool, e.card, c0.x, c0.y, c1.x, c1.y, e.takes.size());
    const std::string text = std::string(b) + "\nseltmann: rows " + rows + (e.crowd.empty() ? "" : "\nseltmann: crowded " + e.crowd);
    static std::map<uint64_t, std::string> last;
    std::string& l = last[t.key()];
    if (l != text) {
      l = text;
      std::printf("%s\n", text.c_str());
      std::fflush(stdout);
    }
  }
  trace(e, "ready", lv);
  return true;
}

}  // namespace gl::packs::seltmann
