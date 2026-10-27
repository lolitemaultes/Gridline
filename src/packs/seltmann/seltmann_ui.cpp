// Gridline — "seltmann" pack: Seltmann editor. One calm flow: drop audio (or
// load it), it is analysed on its own, pick a layout, play it and drag the loop
// out. This file holds the frame (drop zone / progress, the top bar with the
// file and what it is, the key controls: tempo with the x2 / /2 fix, chop
// size, tempo mode, play, drag handles, make loops, send to project), the
// grid tools of the Edit drawer, the shared helpers, auditions, loop renders
// and "send to project". The waveform lives in seltmann_wave_ui.cpp, the layout
// cards, the pads and the Edit drawer in seltmann_parts_ui.cpp. Everything is
// drawn as vectors with Theme roles.
#include "seltmann_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include "../../app/AudioRecordUi.h"
#include "../../app/DevSignal.h"
#include "../../app/Look.h"
#include "../../app/BrowserUi.h"
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

bool fieldKnob(App& a, const char* id, ImVec2 tl, float& v, float mn, float mx, float def, const char* label, const char* fmt, bool* released,
               bool bipolar) {
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
  if (ac && ImGui::IsMouseDragging(0, 0)) {
    const float dy = -ImGui::GetIO().MouseDelta.y;
    const float span = mx - mn;
    const float nv = std::clamp(v + dy * span / (ImGui::GetIO().KeyShift ? 600.0f : 150.0f), mn, mx);
    if (nv != v) v = nv, changed = true;
  }
  if (hv && ImGui::GetIO().MouseWheel != 0) {
    v = std::clamp(v + ImGui::GetIO().MouseWheel * (mx - mn) / 50.0f, mn, mx);
    changed = true;
    if (released) *released = true;
  }
  ImGui::PushID(id);
  double dv = v;
  bool set = false, wantType = false;
  ParamDoubleClick dbl = ParamDoubleClick::Type;
  if (ui::DoubleClicked(d, pressed, &dbl)) {  // reset; at the default, type a value
    if (dbl == ParamDoubleClick::Reset) dv = def, set = true;
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
    v = static_cast<float>(dv);
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
void auditionStart(App& a, const PluginTarget& t, Ed& e, int slice) {
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
    a.engine.noteOn(t.owner, key, 0.9f);
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
  return writeLoopWav(file, L, R, o.sampleRate);
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
  threads.emplace_back([job, items, m, src, o] {
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
  a.doc.editSong(i18n::tr("Send pattern to project"), [&](Song& s) { pid = sendPatternToSong(s, t.owner, e.m, params, 1, track, 0); });
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
  a.fileDialog.onOk = [&a, tt](const std::string& p) {
    if (const PluginState* sp = tt.state(a.doc.song())) {
      PluginState st = *sp;
      st.data = p;
      setPluginState(a, tt, st, i18n::tr("Load audio"));
      if (tt.isChannel()) a.doc.renameChannel(tt.owner, fs::path(p).stem().string());
    }
  };
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
  if (button("##sload2", ImVec2(c.x - 75, c.y + 82), ImVec2(150, 34), i18n::N_("Load audio..."), false, nullptr, static_cast<int>(ui::Icon::Open),
             true))
    openLoadDialog(a, t, e);
}

// ------------------------------------------------------------------ header (what it is)
// J-152: one 36 px row: the kind chip, the file name over its facts, the tempo with the
// /2 and x2 fix, Load and Edit. Every control is kHeaderCtl tall on the row's centre line.
constexpr float kHeaderCtl = 28;

void drawBpm(App& a, const PluginTarget& t, Ed& e, ImVec2 f0) {
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

void drawTopBar(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H, bool song) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  const float cy = o.y + H * 0.5f, cTop = std::round(cy - kHeaderCtl * 0.5f);
  // right to left: Edit, Load, (songs) the strip toggle, the tempo
  float rx = o.x + W;
  rx -= 84;
  if (button("##sedit", ImVec2(rx, cTop), ImVec2(84, kHeaderCtl), i18n::N_("Edit"), e.drawer,
             i18n::N_("Show the advanced controls: the slice inspector, manual cuts, the steps, the beat grid, the sound and the loop list"),
             static_cast<int>(ui::Icon::Settings)))
    e.drawer = !e.drawer;
  rx -= 8 + 96;
  if (button("##sload", ImVec2(rx, cTop), ImVec2(96, kHeaderCtl), i18n::N_("Load..."), false, i18n::N_("Load a song, a loop or any audio file"),
             static_cast<int>(ui::Icon::Open)))
    openLoadDialog(a, t, e);
  if (song) {
    rx -= 8 + kHeaderCtl;
    ImGui::SetCursorScreenPos(ImVec2(rx, cTop));
    if (ui::ToggleIcon("##sstrip", e.stripHidden ? ui::Icon::EyeOff : ui::Icon::Eye, !e.stripHidden, kHeaderCtl,
                       e.stripHidden ? i18n::tr("Show the whole song above the waveform") : i18n::tr("Hide the whole-song strip")))
      e.stripHidden = !e.stripHidden;
  }
  rx -= 24;
  // a hairline between the file and the actions
  dl->AddLine(ImVec2(rx + 12, cTop + 4), ImVec2(rx + 12, cTop + kHeaderCtl - 4), P.line, 1);
  const float bpmW = 104 + 4 + 30 + 4 + 30;
  rx -= bpmW;
  drawBpm(a, t, e, ImVec2(rx, cTop));
  const float tempoX = rx;
  // left: kind chip, then the name over the facts
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
    x = c1.x + 12;
  }
  const float nameW = std::max(120.0f, tempoX - 24 - x);
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(x, std::round(cy - 17)), nameW, P.text, fs::path(e.path).filename().string().c_str());
  {
    std::string facts;
    char b[64];
    const double secs = m.srcFrames / std::max(1.0, m.srcRate);
    if (m.grid.kind != KindOneShot && m.grid.kind != KindAmbient) {
      const int bars = static_cast<int>(std::lround(regionBars(m)));
      std::snprintf(b, sizeof b, "%d %s", bars, bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
      facts += b;
      facts += song ? std::string(" ") + i18n::tr("chopped") : "";
      facts += "  \xC2\xB7  ";
    }
    facts += fmtTime(secs);
    if (m.grid.key >= 0 && m.grid.kind != KindBreak && m.grid.keyConfidence >= 0.25)  // drums have no key worth showing
      facts += std::string("  \xC2\xB7  ") + i18n::tr(keyName(m.grid.key, m.grid.minor));
    std::snprintf(b, sizeof b, "  \xC2\xB7  %d %s", static_cast<int>(m.slices.size()), m.slices.size() == 1 ? i18n::tr("slice") : i18n::tr("slices"));
    facts += b;
    facts += std::string("  \xC2\xB7  ") + chopSummary(m);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x, std::round(cy + 2)), nameW, P.textDim, facts.c_str());
  }
}

// ------------------------------------------------------------------ action bar (what to do)
// J-152: one row in the order of the work: play it, how it is chopped, then everything that takes
// it out (drag the loop or the notes, make loop files, send the pattern to the project). Group
// labels sit left of their control; every control is 28 px on the row's centre line.
void drawControls(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, float H) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ch = 28, top = std::round(o.y + (H - ch) * 0.5f);
  float x = o.x;
  // ---- play
  e.playBtn = ImVec2(x + 28, top + ch * 0.5f);
  if (button("##splay", ImVec2(x, top), ImVec2(56, ch), "", e.pattern.on,
             e.pattern.on ? i18n::N_("Stop the pattern") : i18n::N_("Play the pattern (or hold a key in the octave below the base note)"),
             static_cast<int>(e.pattern.on ? ui::Icon::Stop : ui::Icon::Play), !e.pattern.on))
    togglePattern(a, t, e);
  x += 56 + 20;
  // ---- chop size
  {
    x += inlineLabel(dl, a, ImVec2(x, top), ch, i18n::N_("CHOP"));
    static const char* const labels[6] = {i18n::N_("Auto"), i18n::N_("1 bar"), "1/2", "1/4", "1/8", "1/16"};
    static const char* const hints[6] = {i18n::N_("The size that suits the material: breaks on their hits in eighths or sixteenths, loops and songs by beats and bars"),
                                         i18n::N_("One slice per bar"), i18n::N_("Half a bar per slice"), i18n::N_("One beat per slice"),
                                         i18n::N_("Eighth notes"), i18n::N_("Sixteenth notes")};
    static const int divs[6] = {-1, Div1Bar, DivHalf, DivBeat, Div8th, Div16th};
    const Model& m = e.m;
    int cur = -1;
    if (m.chop.autoSize) cur = 0;
    else if (m.chop.mode == ChopGrid)
      for (int i = 1; i < 6; ++i)
        if (m.chop.division == divs[i]) cur = i;
    const float w = 288;
    if (pills(a, "##schopsize", ImVec2(x, top), w, ch, labels, 6, cur, hints, false) && cur >= 0) {
      Model nm = e.m;
      nm.chop.autoSize = cur == 0;
      if (cur > 0) {
        nm.chop.mode = ChopGrid;
        nm.chop.division = divs[cur];
        nm.chop.hitSnap = nm.grid.kind == KindBreak;
      }
      commitChop(a, t, e, nm, i18n::N_("Chop size"));
    }
    x += w + 20;
  }
  // ---- tempo mode
  {
    x += inlineLabel(dl, a, ImVec2(x, top), ch, i18n::N_("PLAY AT"));
    static const char* const labels[2] = {i18n::N_("Project"), i18n::N_("Original")};
    static const char* const hints[2] = {i18n::N_("Follow the project tempo (time-stretched; Edit > Sound has Resample)"),
                                         i18n::N_("Play at the sample's own tempo")};
    const int sync = static_cast<int>(std::lround(paramOf(a, t, SmSync)));
    int cur = sync == SyncOriginal ? 1 : 0;
    if (pills(a, "##stempo", ImVec2(x, top), 160, ch, labels, 2, cur, hints))
      pui::setParamValue(a, t, SmSync, cur == 1 ? SyncOriginal : SyncStretch, false);
  }
  // ---- right: drag handles, make loops, send to project
  float rx = o.x + W;
  rx -= 136;
  if (button("##ssend", ImVec2(rx, top), ImVec2(136, ch), i18n::N_("Send to project"), false,
             i18n::N_("Add a pattern with these steps as notes on this channel and place it in the playlist"), static_cast<int>(ui::Icon::Pattern),
             true))
    sendToProject(a, t, e);
  rx -= 8 + 112;
  if (button("##smake", ImVec2(rx, top), ImVec2(112, ch), i18n::N_("Make loops"), false,
             i18n::N_("Render the pattern, the best phrases and the sections as loops at the project tempo into the \"Seltmann loops\" folder"),
             static_cast<int>(ui::Icon::Save)))
    makeLoops(a, t, e);
  if (e.loopJob && !e.loopJob->finished) {
    const float pr = static_cast<float>(e.loopJob->done) / std::max(1, e.loopJob->total.load());
    dl->AddRectFilled(ImVec2(rx + 6, top + ch - 4), ImVec2(rx + 106, top + ch - 2), P.surface900, 1);
    dl->AddRectFilled(ImVec2(rx + 6, top + ch - 4), ImVec2(rx + 6 + 100 * pr, top + ch - 2), accent(), 1);
  }
  {
    // drag handles: the pattern as audio and as notes
    const float mw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "MIDI").x + 30);
    rx -= 12 + mw;
    dragChip(a, t, e, "##smidi", ImVec2(rx, top), ch, "MIDI", "midi", e.m.pattern.name + " notes",
             i18n::N_("Drag the pattern's notes into the piano roll or the playlist"));
    const float lw = std::round(a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, "LOOP").x + 30);
    rx -= 6 + lw;
    dragChip(a, t, e, "##sploop", ImVec2(rx, top), ch, "LOOP", "pattern", e.m.pattern.name,
             i18n::N_("Drag the pattern out as an audio loop at the project tempo (playlist, channel rack, browser)"));
    const float dw = inlineLabel(nullptr, a, ImVec2(0, 0), 0, i18n::N_("DRAG OUT"));
    inlineLabel(dl, a, ImVec2(rx - dw, top), ch, i18n::N_("DRAG OUT"));
  }
}

// ------------------------------------------------------------------ dev trace
void trace(Ed& e, const char* state, const LiveView& lv) {
  static const bool on = std::getenv("GRIDLINE_SELTMANN_TRACE") != nullptr;
  if (!on) return;
  char b[512];
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
  }
  std::snprintf(b, sizeof b, "\nseltmann: body %.0f %.0f", e.body.x, e.body.y);  // J-152: the measured size
  line += b;
  if (std::string(state) == "empty") {
    std::snprintf(b, sizeof b, "\nseltmann: zone %.0f %.0f %.0f %.0f", e.zone0.x, e.zone0.y, e.zone1.x, e.zone1.y);
    line += b;
  }
  if (std::string(state) == "ready") {
    for (size_t i = 0; i < e.cards.size() && i < 3; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: card \"%s\" %.0f %.0f", e.cards[i].first.c_str(), e.cards[i].second.x, e.cards[i].second.y);
      line += b;
    }
    std::snprintf(b, sizeof b, "\nseltmann: play %.0f %.0f", e.playBtn.x, e.playBtn.y);
    line += b;
    for (size_t i = 0; i < e.seqCentres.size() && i < 24; ++i) {
      std::snprintf(b, sizeof b, "\nseltmann: seq %zu %.0f %.0f", i, e.seqCentres[i].x, e.seqCentres[i].y);
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

// ------------------------------------------------------------------ grid tools (drawer)
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
    char b[16];
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

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  using namespace sui;
  Ed& e = ed(t);
  static bool cacheSet = false;
  if (!cacheSet) {
    setAnalysisCacheDir((fs::path(a.configDir()) / "cache" / "seltmann").string());
    cacheSet = true;
  }
  // dev hooks (screenshots / checks): a file to load, a layout to apply, play the pattern, the drawer
  if (!e.demoDone) {
    e.demoDone = true;
    if (const char* d = std::getenv("GRIDLINE_SELTMANN_DRAWER"); d && *d) {
      e.drawer = true;
      e.drawerTab = std::clamp(std::atoi(d), 0, DrawCount - 1);
    }
    if (const char* tab = std::getenv("GRIDLINE_SELTMANN_TAB"); tab && *tab) e.presetTab = std::clamp(std::atoi(tab), 0, TabCount - 1);
    if (const char* demo = std::getenv("GRIDLINE_SELTMANN_DEMO"); demo && *demo && sp->data.empty()) {
      PluginState st = *sp;
      st.data = demo;
      setPluginState(a, t, st, "demo");
      sp = t.state(a.doc.song());
      if (!sp) return true;
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
      e.selSlice = 0, e.selStep = -1, e.rendered.clear(), e.padPage = 0;
      e.job.reset();
      if (e.pattern.on) togglePattern(a, t, e);
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

  const float W = std::max(width, 1040.0f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const LiveView lv = live(a, t);
  syncPattern(a, t, e, lv);
  // J-152: the body, top to bottom in the order of the work (heights at UI scale 1):
  //   header 36 (what it is) | waveform 128 + sequence 80 (the hero, one time axis) |
  //   action bar 32 (play, chop, take it out) | tools 200 (layouts | pads) | Edit drawer 132..236 (per tab, closed by default)
  const float headH = 36, waveH = 128, seqH = 80, barH = 32, toolsH = 200, gap = 8;
  const float bodyH = headH + gap + waveH + seqH + gap + barH + gap + toolsH;
  if (!e.hasModel) {
    // drop zone (empty), progress (analysing) or a message (unreadable): the same height as the editor
    drawEmpty(a, t, e, o, ImVec2(o.x + W, o.y + bodyH));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + bodyH + 6));
    ImGui::Dummy(ImVec2(W, 1));
    e.body = ImVec2(W, bodyH + 6);
    trace(e, e.path.empty() ? "empty" : (e.job && e.job->state == 2) ? "error" : "analysing", lv);
    return true;
  }
  const bool song = e.m.grid.kind == KindSong || barCount(e.m) > 16;
  drawTopBar(a, t, e, o, W, headH, song);
  float y = o.y + headH + gap;
  if (song && !e.stripHidden) {
    drawStrip(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + 28), lv);
    y += 28 + 6;
  }
  drawWave(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + waveH), lv);
  y += waveH;
  drawSequence(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + seqH), lv);  // J-149, under the waveform (J-152)
  y += seqH + gap;
  drawControls(a, t, e, ImVec2(o.x, y), W, barH);
  y += barH + gap;
  const float leftW = std::round((W - gap) * 0.5f);
  drawPresets(a, t, e, ImVec2(o.x, y), ImVec2(o.x + leftW, y + toolsH));
  drawPads(a, t, e, ImVec2(o.x + leftW + gap, y), ImVec2(o.x + W, y + toolsH), lv);
  y += toolsH;
  if (e.drawer) {
    y += gap;
    const float drawerH = drawerHeight(e);
    drawDrawer(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + drawerH), lv);
    y += drawerH;
  }
  drawRegionMenu(a, t, e);
  ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
  ImGui::Dummy(ImVec2(W, 1));
  e.body = ImVec2(W, y + 6 - o.y);
  trace(e, "ready", lv);
  return true;
}

}  // namespace gl::packs::seltmann
