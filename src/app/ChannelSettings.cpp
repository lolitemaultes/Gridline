// Gridline — channel settings window: strip, routing, instrument parameters,
// sample loading and waveform for the sampler.
#include <algorithm>
#include "MacroUi.h"
#include "LayoutStyleUi.h"
#include "EditorZoomUi.h"
#include "KeyGlide.h"
#include "KeyLight.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include "../core/ClipWindow.h"
#include "ClipEditor.h"
#include "../core/Macros.h"
#include "../core/I18n.h"
#include "../core/WindowPlacement.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/PluginParams.h"
#include "../engine/ChannelIns.h"
#include "App.h"
#include "LogicPlugins.h"
#include "GamepadUi.h"
#include "LayoutPresets.h"
#include "../core/MixerOps.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginScan.h"
#include "HostedUi.h"
#include "PluginWindowBar.h"
#include "Theme.h"
#include "Widgets.h"
#include "PluginUI.h"
#include "BrowserUi.h"
#include "Windows.h"
#include "../packs/modulation/modulation_ui.h"
#include "../packs/PacksUi.h"
#include "../core/Platform.h"
#include "ChannelStrip.h"
#include "HostedBodies.h"
#include "ChannelRoute.h"
#include "LayerEditor.h"
#include "../core/LayerRack.h"
#include "../core/Project.h"
#include "../engine/ClipSource.h"
#include "TrackerEditors.h"
#include "Viewports.h"
#include "SmoothScrollUi.h"
#include "../../third_party/imgui/imgui_internal.h"
#include <map>

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

// K-85: the whole file with the channel's start and end handles. Dragging a
// handle trims what the channel plays: its notes and its audio clips in the
// playlist (engine/ClipSource.h), so the page and the playlist agree. The same
// two parameters as the sampler editor's markers; they never cross.
static void drawWaveform(App& a, ObjectId channel, const SamplePtr& smp, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##chwave", size);  // (also the drop target for a new sample)
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.surface950, 6);
  if (!smp || smp->frames() == 0) {
    const char* msg = i18n::tr("Drop an audio file here or click Load");
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), P.textFaint, msg);
    return;
  }
  const Channel* ch = a.doc.song().channel(channel);
  double startFrac = 0, endFrac = 1;
  if (ch) channelTrim(*ch, startFrac, endFrac);
  const PluginTarget target{channel, -1};
  // Drag: the handle nearest the pointer (within 8 px) moves, kept apart from the other one.
  static int dragging = -1;
  const ImGuiIO& io = ImGui::GetIO();
  const float sx = p.x + static_cast<float>(startFrac) * size.x, ex = p.x + static_cast<float>(endFrac) * size.x;
  int near = -1;
  if (hovered || active) {
    const float ds = std::fabs(io.MousePos.x - sx), de = std::fabs(io.MousePos.x - ex);
    if (std::min(ds, de) <= 8.0f) near = ds <= de ? 0 : 1;
  }
  if (ImGui::IsItemActivated()) dragging = near;
  if (active && dragging >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.5f)) {
    const double v = clampTrimHandle(dragging == 1, (io.MousePos.x - p.x) / size.x, dragging == 1 ? startFrac : endFrac, smp->seconds());
    a.doc.setParam(target.pid(dragging == 1 ? pp::SmpEnd : pp::SmpStart), v, true);
  }
  if (ImGui::IsItemDeactivated()) {
    dragging = -1;
    a.doc.endGesture();
  }
  if (near >= 0 || (active && dragging >= 0)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (hovered)
    a.setHint(i18n::tr("Drag the start or end handle: the channel's notes and its audio clips in the playlist play only the part between them"));
  float mid = p.y + size.y * 0.5f;
  size_t n = smp->frames();
  int w = static_cast<int>(size.x);
  for (int x = 0; x < w; ++x) {
    size_t a0 = n * static_cast<size_t>(x) / static_cast<size_t>(w), a1 = std::max(a0 + 1, n * static_cast<size_t>(x + 1) / static_cast<size_t>(w));
    float lo = 0, hi = 0;
    for (size_t i = a0; i < a1 && i < n; i += std::max<size_t>(1, (a1 - a0) / 64)) {
      float v = (smp->left[i] + smp->right[i]) * 0.5f;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    dl->AddLine(ImVec2(p.x + x, mid - hi * size.y * 0.46f), ImVec2(p.x + x, mid - lo * size.y * 0.46f + 1), P.text);
  }
  // The parts the handles cut away, dimmed; each handle a line with a small tab at the top.
  if (startFrac > 0) dl->AddRectFilled(p, ImVec2(sx, p.y + size.y), theme::withAlpha(P.surface950, 0.6f), 6, ImDrawFlags_RoundCornersLeft);
  if (endFrac < 1) dl->AddRectFilled(ImVec2(ex, p.y), ImVec2(p.x + size.x, p.y + size.y), theme::withAlpha(P.surface950, 0.6f), 6, ImDrawFlags_RoundCornersRight);
  for (int k = 0; k < 2; ++k) {
    const float x = k ? ex : sx;
    const bool lit = near == k || (active && dragging == k);
    const ImU32 col = lit ? P.accent : theme::withAlpha(P.accent, 0.75f);
    dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size.y), col, lit ? 2.0f : 1.5f);
    const float tw = 6.0f, th = 7.0f;
    if (k == 0) dl->AddTriangleFilled(ImVec2(x, p.y), ImVec2(x + tw, p.y), ImVec2(x, p.y + th), col);
    else dl->AddTriangleFilled(ImVec2(x, p.y), ImVec2(x - tw, p.y), ImVec2(x, p.y + th), col);
  }
  char info[128];
  const double secs = smp->seconds();
  if (startFrac > 0 || endFrac < 1)
    std::snprintf(info, sizeof info, "%.2f s  ·  %.0f Hz  ·  %s %.3f - %.3f s", secs, smp->sampleRate, i18n::tr("plays"), startFrac * secs, endFrac * secs);
  else
    std::snprintf(info, sizeof info, "%.2f s  ·  %.0f Hz", secs, smp->sampleRate);
  ImGui::PushFont(a.fontMono, 10.5f);
  dl->AddText(ImVec2(p.x + 10, p.y + 6), P.textDim, info);
  ImGui::PopFont();
}

// ---- Channel strip state (ChannelStrip.h): per application, whether the open
// window shows it, its slide and the content height measured when it was last
// drawn; dev checks read StripDev.
namespace {

ImVec2 devPlace;        // (dev) plugwin at: where the channel window goes
int devPlaceFrames = 0;  // and for how many more frames

ObjectId& pendingFreeRoute(const App& a) {  // J-195: per application, the channel to route to a free mixer track next frame
  static std::map<const App*, ObjectId> m;
  return m.emplace(&a, kNoId).first->second;
}
struct StripDev {
  float frac = 0, shownH = 0, fullH = 0, keyW = 0, belowY = 0;
  bool moving = false, drawn = false, open = false, shown = false;
  int dumpLeft = 0, dumpFrame = 0;
  ImVec2 eyeMin, eyeMax, clipMin, clipMax, keysMin, keysMax, volMin, volMax, winPos, winSize;
};
StripDev& stripDev() {
  static StripDev d;
  return d;
}
struct StripState {
  chstrip::Reveal reveal;
  float contentH = 0;
  bool shown = false;        // the eye's choice for the open window (never saved)
  bool open = false;         // the window was open the last time drawChannelSettings ran
  ObjectId channel = kNoId;  // the channel it was open for
  int devSet = -1;           // dev "strip on|off": applied when the window is next drawn
};
StripState& stripState(App& a) {
  static std::map<const App*, StripState> m;
  StripState& st = m[&a];
  if (!st.reveal.ready) st.reveal.reset(false);
  return st;
}
// Every frame the window is open: opened (or now showing another channel), the
// strip starts hidden, without a slide.
void stripOpened(App& a, ObjectId channel) {
  StripState& st = stripState(a);
  if (!st.open || st.channel != channel) {
    st.shown = false;
    st.reveal.reset(false);
  }
  st.open = true;
  st.channel = channel;
  if (st.devSet >= 0) {
    st.shown = st.devSet == 1;
    st.reveal.reset(st.shown);
    st.devSet = -1;
  }
}
// Dev check ("strip [frames]"): the strip's state as drawn this frame.
void stripDevPrint(int frame) {
  const StripDev& d = stripDev();
  if (!d.open) {
    std::printf("strip frame %d: window closed\n", frame);
  } else {
    std::printf("strip frame %d: on %d frac %.4f moving %d shown %.1f full %.1f drawn %d eye %.1f %.1f %.1f %.1f window %.1f %.1f %.1f %.1f below %.1f\n",
                frame, d.shown ? 1 : 0, static_cast<double>(d.frac), d.moving ? 1 : 0, static_cast<double>(d.shownH), static_cast<double>(d.fullH),
                d.drawn ? 1 : 0, static_cast<double>(d.eyeMin.x), static_cast<double>(d.eyeMin.y), static_cast<double>(d.eyeMax.x),
                static_cast<double>(d.eyeMax.y), static_cast<double>(d.winPos.x), static_cast<double>(d.winPos.y), static_cast<double>(d.winSize.x),
                static_cast<double>(d.winSize.y), static_cast<double>(d.belowY));
    if (d.drawn)
      std::printf("  clip %.1f %.1f %.1f %.1f keys %.1f %.1f %.1f %.1f keyw %.1f vol %.1f %.1f %.1f %.1f\n", static_cast<double>(d.clipMin.x),
                  static_cast<double>(d.clipMin.y), static_cast<double>(d.clipMax.x), static_cast<double>(d.clipMax.y), static_cast<double>(d.keysMin.x),
                  static_cast<double>(d.keysMin.y), static_cast<double>(d.keysMax.x), static_cast<double>(d.keysMax.y), static_cast<double>(d.keyW),
                  static_cast<double>(d.volMin.x), static_cast<double>(d.volMin.y), static_cast<double>(d.volMax.x), static_cast<double>(d.volMax.y));
  }
  std::fflush(stdout);
}
void stripDevFrame() {
  StripDev& d = stripDev();
  if (d.dumpLeft <= 0) return;
  --d.dumpLeft;
  stripDevPrint(d.dumpFrame++);
}
chstrip::Reveal& stripReveal(App& a) { return stripState(a).reveal; }
float& stripContentH(App& a) {
  float& h = stripState(a).contentH;
  // Before the strip was first drawn: a label row (ui::Label) over the 48 px
  // keyboard (the knobs are as tall). Exact at any scale; should a knob style
  // ever make it taller, the first drawn frame measures it.
  if (h <= 0) {
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    const float label = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    h = label + ImGui::GetStyle().ItemSpacing.y + 48.0f;
  }
  return h;
}
}  // namespace

// ---- Note settings: tuning, time, polyphony / porta, cut groups, echo,
// layer children and display group of a channel. `part` -1: all of them (Windows.h).
void drawNoteSettingsPart(App& a, ObjectId id, int part) { drawNoteSettingsPart(a, id, part, nullptr); }

void drawNoteSettingsPart(App& a, ObjectId id, int part, const NoteGrid* grid) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(id);
  if (!c) return;
  const auto& P = pal();

  const float cell = 66;
  auto desc = [](const char* name, const char* unit, double mn, double mx, double def) {
    ParamDesc d;
    d.name = name;
    d.unit = unit;
    d.min = mn;
    d.max = mx;
    d.def = def;
    return d;
  };
  // One knob cell (knob, value, name). `set` writes the value into the channel. A name wider than
  // its cell shows `shortName` instead (the full name in its tooltip and the hint bar).
  int gridKnob = 0, gridToggle = 0, gridList = 0;  // (on the grid: the part's knobs, toggles and lists so far)
  auto knob = [&](const char* key, double v, const ParamDesc& d, bool bipolar, const std::function<void(Channel&, double)>& set,
                  const char* valueText = nullptr, const char* shortName = nullptr) {
    // (the name under the cell, the full one in a tooltip when it is the short one)
    auto nameFor = [&](ImFont* f, float fs, float room) -> const char* {
      if (!shortName || f->CalcTextSizeA(fs, 1e9f, 0, d.name.c_str()).x <= room) return d.name.c_str();
      return i18n::tr(shortName);
    };
    auto fullNameTip = [&](const char* shown, ImVec2 p, ImVec2 sz) {
      if (shown == d.name.c_str()) return;
      ImGui::SetCursorScreenPos(p);
      ImGui::InvisibleButton("##name", ImVec2(std::max(1.0f, sz.x), std::max(1.0f, sz.y)));
      ui::Tooltip(d.name.c_str());
    };
    if (grid) {  // the module grid: the name above, the knob, its value below, centred in the cell
      float gw = 64;
      const ImVec2 at = grid->place(0, gridKnob++, gw);
      const float r = grid->knobRadius, cx = std::round(at.x + gw * 0.5f);
      ImGui::PushID(key);
      ImDrawList* gdl = ImGui::GetWindowDrawList();
      ImFont* uf = ImGui::GetFont();
      auto centred = [&](ImFont* f, const std::string& t, float y, ImU32 col) {
        const float tw = f->CalcTextSizeA(11.0f, 1e9f, 0, t.c_str()).x;
        if (tw > gw) theme::textEllipsis(gdl, f, 11.0f, ImVec2(at.x, y), gw, col, t.c_str());
        else gdl->AddText(f, 11.0f, ImVec2(std::round(cx - tw * 0.5f), y), col, t.c_str());
      };
      const char* shown = nameFor(uf, 11.0f, gw);
      centred(uf, shown, at.y, P.textDim);
      {
        const float nw = std::min(gw, uf->CalcTextSizeA(11.0f, 1e9f, 0, shown).x);
        fullNameTip(shown, ImVec2(std::round(cx - nw * 0.5f), at.y), ImVec2(nw, 12.0f));
      }
      ImGui::SetCursorScreenPos(ImVec2(cx - r, at.y + grid->knobOff));
      bool rel = false;
      double nv = v;
      if (ui::Knob("k", &nv, d, r, nullptr, bipolar, &rel)) {
        std::string label = std::string("change channel ") + d.name;
        for (auto& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        a.doc.editChannel(id, label, [&](Channel& ch) { set(ch, d.clamp(nv)); }, 0x4E53000000000000ull ^ (static_cast<uint64_t>(id) << 8) ^ std::hash<std::string>{}(key));
      }
      if (shortName && ImGui::IsItemHovered() && !ImGui::IsItemActive()) ui::Tooltip(d.name.c_str());
      if (rel) a.doc.endGesture();
      centred(a.fontMono ? a.fontMono : uf, valueText ? std::string(valueText) : d.format(v), at.y + grid->valueOff, P.text);
      ImGui::PopID();
      return;
    }
    ImGui::PushID(key);
    ImGui::BeginGroup();
    float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
    bool rel = false;
    double nv = v;
    if (ui::Knob("k", &nv, d, 14, nullptr, bipolar, &rel)) {
      std::string label = std::string("change channel ") + d.name;
      for (auto& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      a.doc.editChannel(id, label, [&](Channel& ch) { set(ch, d.clamp(nv)); }, 0x4E53000000000000ull ^ (static_cast<uint64_t>(id) << 8) ^ std::hash<std::string>{}(key));
    }
    if (rel) a.doc.endGesture();
    std::string val = valueText ? valueText : d.format(v);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(val.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.text), "%s", val.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.0f);
    // A name wider than its cell ("Mod X per echo") shows its short form, the full one in a tooltip;
    // hunt-1119: one still wider is set a little smaller to fit, on the same line.
    const char* shown = nameFor(ImGui::GetFont(), 11.0f, cell - 4);
    ts = ImGui::CalcTextSize(shown);
    if (ts.x > cell - 4) {
      ImGui::PopFont();
      ImGui::PushFont(nullptr, std::max(8.0f, 11.0f * (cell - 4) / ts.x));
      ts = ImGui::CalcTextSize(shown);
    }
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.textDim), "%s", shown);
    if (shown != d.name.c_str()) ui::Tooltip(d.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
  };
  auto toggle = [&](const char* label, bool on, const char* hint, const std::function<void(Channel&)>& flip) {
    float tw = 80;
    if (grid) ImGui::SetCursorScreenPos(grid->place(1, gridToggle++, tw));
    if (ui::TextButton(label, on, tw, hint)) a.doc.editChannel(id, std::string("toggle ") + label, flip);
  };
  // A list: on the grid at its place, its name in its preview ("Pressure: Level"); else at the cursor.
  auto listAt = [&](const char* name, const char* value, float& w) -> std::string {
    if (!grid) return value;
    ImGui::SetCursorScreenPos(grid->place(2, gridList++, w));
    return name && *name ? std::string(name) + ": " + value : std::string(value);
  };
  char buf[48];
  // The Audio clip window draws each part in a card of its own (its caption the part's heading).
  const bool all = part < 0;
  auto on = [&](int k) { return all || part == k; };
  auto heading = [&](const char* t) {
    if (all) ui::Label(t);
  };

  // Tuning & time.
  if (on(0)) {
  ImGui::BeginGroup();
  heading("TUNE & TIME");
  ui::SetNextKnobStep(1);
  knob("fine", c->fine, desc("Fine", "ct", -200, 200, 0), true, [](Channel& ch, double v) { ch.fine = static_cast<int>(std::lround(v)); });
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%d st", c->bendRange);
  ui::SetNextKnobStep(1);
  knob("bendrange", c->bendRange, desc("Bend range", "st", 1, kChannelBendRangeMax, 2), false,
       [](Channel& ch, double v) { ch.bendRange = std::clamp(static_cast<int>(std::lround(v)), 1, kChannelBendRangeMax); }, buf);
  ui::Hint("Semitones a full pitch bend means to this instrument (the usual synth default is 2). Plugins without their "
           "own note tuning get the channel pitch and bend lines as pitch bend over this range: set the plugin to the same");
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(c->swingMix * 100)));
  knob("swing", c->swingMix, desc("Swing", "", 0, 1, 1), false, [](Channel& ch, double v) { ch.swingMix = static_cast<float>(v); }, buf);
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%.3f beat", c->timeShift);
  knob("shift", c->timeShift, desc("Shift", "", 0, 1, 0), false, [](Channel& ch, double v) { ch.timeShift = static_cast<float>(v); }, buf);
  ImGui::EndGroup();
  }
  if (all) ImGui::SameLine(0, theme::kS4);
  // Polyphony.
  if (on(1)) {
  ImGui::BeginGroup();
  heading("POLYPHONY");
  std::snprintf(buf, sizeof buf, "%s", c->polyphony == 0 ? "all" : std::to_string(c->polyphony).c_str());
  ui::SetNextKnobStep(1);
  knob("voices", c->polyphony, desc("Voices", "", 0, 64, 0), false, [](Channel& ch, double v) { ch.polyphony = static_cast<int>(std::lround(v)); }, buf);
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%.0f ms", c->portaTime * 1000.0);
  knob("porta", c->portaTime, desc("Glide", "s", 0, 2, 0.1), false, [](Channel& ch, double v) { ch.portaTime = static_cast<float>(v); }, buf);
  ImGui::SameLine(0, 4);
  ImGui::BeginGroup();
  toggle("Mono", c->mono, "One note at a time: a new note replaces the sounding one (legato glide with Porta)",
         [](Channel& ch) { ch.mono = !ch.mono; });
  toggle("Porta", c->porta, "Every note glides from the previous note's pitch over the glide time",
         [](Channel& ch) { ch.porta = !ch.porta; });
  ImGui::EndGroup();
  ImGui::EndGroup();
  }
  if (all) ImGui::SameLine(0, theme::kS4);
  // Cut groups. fix-1119: on the next line when the row has no room for them (pitch-1118's Bend range
  // knob widened the row past the window's 600 px: Cut by was drawn under its right edge, out of reach).
  if (all && ImGui::GetContentRegionAvail().x < 2 * cell) {
    ImGui::NewLine();
  }
  if (on(2)) {
  ImGui::BeginGroup();
  heading("CUT GROUP");
  std::snprintf(buf, sizeof buf, "%s", c->cutGroup ? std::to_string(c->cutGroup).c_str() : "off");
  ui::SetNextKnobStep(1);
  knob("cut", c->cutGroup, desc("Cuts", "", 0, kMaxCutGroups, 0), false, [](Channel& ch, double v) { ch.cutGroup = static_cast<int>(std::lround(v)); }, buf);
  ui::Hint("Notes of this channel cut channels that are cut by this group");
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%s", c->cutBy ? std::to_string(c->cutBy).c_str() : "off");
  ui::SetNextKnobStep(1);
  knob("cutby", c->cutBy, desc("Cut by", "", 0, kMaxCutGroups, 0), false, [](Channel& ch, double v) { ch.cutBy = static_cast<int>(std::lround(v)); }, buf);
  ui::Hint("This channel is cut by notes of channels in this group (same number in both = cuts itself)");
  ImGui::EndGroup();

  }
  if (all) ImGui::Dummy(ImVec2(0, theme::kS1));
  // K-144: per-note expression (MPE controllers, note pressure and timbre lanes).
  if (on(3)) {
  ImGui::BeginGroup();
  heading(i18n::tr("EXPRESSION"));
  {
    static const char* uses[] = {i18n::N_("Instrument's choice"), i18n::N_("Off"), i18n::N_("Level"), i18n::N_("Brightness")};
    auto useCombo = [&](const char* key, const char* name, ExprUse cur, const char* hint, ExprUse ExprSettings::*field) {
      float lw = 150;
      if (!grid) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(P.textDim), "%s", name);
        ImGui::SameLine(0, theme::kS1);
      }
      const std::string pv = listAt(name, i18n::tr(uses[static_cast<int>(cur)]), lw);
      if (ui::BeginCombo(key, pv.c_str(), lw)) {
        for (int k = 0; k < 4; ++k)
          if (ImGui::Selectable(i18n::tr(uses[k]), k == static_cast<int>(cur)) && k != static_cast<int>(cur))
            a.doc.editChannel(id, "change channel expression", [k, field](Channel& ch) { ch.expr.*field = static_cast<ExprUse>(k); });
        ImGui::EndCombo();
      }
      ui::Hint(hint);
    };
    toggle(i18n::tr("Note bend"), c->expr.pitch, i18n::tr("Per-note pitch from an MPE controller bends each note on its own"),
           [](Channel& ch) { ch.expr.pitch = !ch.expr.pitch; });
    if (all) ImGui::SameLine(0, theme::kS2);
    useCombo("##xpress", i18n::tr("Pressure"), c->expr.pressure, i18n::tr("What pressing harder on a held note does"), &ExprSettings::pressure);
    if (all) ImGui::SameLine(0, theme::kS2);
    useCombo("##xtimbre", i18n::tr("Timbre"), c->expr.timbre, i18n::tr("What sliding on a held note (CC 74) does"), &ExprSettings::timbre);
    if (c->plugin.type == "gl.midiout") {  // K-99: MPE out
      const bool on = static_cast<int>(c->plugin.params.size()) > pp::MoMpe && c->plugin.params[pp::MoMpe] > 0.5f;
      if (all) ImGui::SameLine(0, theme::kS2);
      float mw = 80;
      if (grid) ImGui::SetCursorScreenPos(grid->place(1, gridToggle++, mw));
      if (ui::TextButton(i18n::tr("MPE out"), on, mw,
                         i18n::tr("Every note on its own MIDI channel (2 to 16) with its own pitch bend, pressure and CC 74; channel 1 is the master")))
        a.doc.setParam(ParamId{id, kChannelPluginBase + static_cast<uint32_t>(pp::MoMpe)}, on ? 0.0 : 1.0, false);
    }
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS2);
  std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(c->expr.amount * 100)));
  knob("xamount", c->expr.amount, desc("Amount", "", 0, 1, 1), false, [](Channel& ch, double v) { ch.expr.amount = static_cast<float>(v); }, buf);
  }
  if (all) ImGui::Dummy(ImVec2(0, theme::kS1));
  // Echo.
  if (on(4)) {
  ImGui::BeginGroup();
  heading("ECHO");
  std::snprintf(buf, sizeof buf, "%s", c->echo.count ? std::to_string(c->echo.count).c_str() : "off");
  ui::SetNextKnobStep(1);
  knob("ecount", c->echo.count, desc("Echoes", "", 0, 10, 0), false, [](Channel& ch, double v) { ch.echo.count = static_cast<int>(std::lround(v)); }, buf);
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(c->echo.feedback * 100)));
  knob("efb", c->echo.feedback, desc("Feedback", "", 0, 1, 0.5), false, [](Channel& ch, double v) { ch.echo.feedback = static_cast<float>(v); }, buf);
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%.2f beat", c->echo.time);
  knob("etime", c->echo.time, desc("Time", "", 0.0625, 4, 0.375), false, [](Channel& ch, double v) { ch.echo.time = static_cast<float>(v); }, buf);
  ImGui::SameLine(0, 0);
  knob("epan", c->echo.pan, desc("Pan", "pan", -1, 1, 0), true, [](Channel& ch, double v) { ch.echo.pan = static_cast<float>(v); });
  ImGui::SameLine(0, 0);
  ui::SetNextKnobStep(1);
  knob("epitch", c->echo.pitch, desc("Pitch", "ct", -1200, 1200, 0), true, [](Channel& ch, double v) { ch.echo.pitch = static_cast<int>(std::lround(v)); });
  ImGui::SameLine(0, 8);  // (the two long names, "Mod X per echo" and "Mod Y per echo": their short forms where they do not fit)
  knob("emodx", c->echo.modX, desc("Mod X per echo", "%", -1, 1, 0), true, [](Channel& ch, double v) { ch.echo.modX = static_cast<float>(v); }, nullptr,
       i18n::N_("Mod X/echo"));
  ImGui::SameLine(0, 8);
  knob("emody", c->echo.modY, desc("Mod Y per echo", "%", -1, 1, 0), true, [](Channel& ch, double v) { ch.echo.modY = static_cast<float>(v); }, nullptr,
       i18n::N_("Mod Y/echo"));
  ImGui::SameLine(0, 4);
  toggle("Ping-pong", c->echo.pingPong, "Echoes alternate between the pan position and its mirror",
         [](Channel& ch) { ch.echo.pingPong = !ch.echo.pingPong; });
  ImGui::EndGroup();
  }
  if (all) ImGui::SameLine(0, theme::kS4);
  // hunt-1119: on the next line when the row has no room for its 150 px lists (the echo row filled the
  // window's width: Layer and Display group were cut off at its right edge, out of reach).
  if (all && ImGui::GetContentRegionAvail().x < 150) {
    ImGui::NewLine();
  }
  // Layer + display group.
  if (on(5)) {
  ImGui::BeginGroup();
  if (!grid) ui::Label("LAYER");
  float lw = 150;
  const std::string preview = listAt(i18n::tr("Layer"), c->layer.empty() ? "No children" : (std::to_string(c->layer.size()) + " children").c_str(), lw);
  if (ui::BeginCombo("##layer", preview.c_str(), lw)) {
    for (const auto& o : s.channels) {
      if (o.id == id) continue;
      bool on = std::find(c->layer.begin(), c->layer.end(), o.id) != c->layer.end();
      if (ImGui::MenuItem((o.name + "##l" + std::to_string(o.id)).c_str(), nullptr, on))
        a.doc.editChannel(id, on ? "remove layer child" : "add layer child", [&](Channel& ch) {
          if (on)
            ch.layer.erase(std::remove(ch.layer.begin(), ch.layer.end(), o.id), ch.layer.end());
          else if (ch.layer.size() < static_cast<size_t>(kMaxLayerChildren))
            ch.layer.push_back(o.id);
        });
    }
    ImGui::EndCombo();
  }
  ui::Hint("Layer: this channel's notes also play on the checked channels");
  {
    const char* modes[] = {"All", "Random", "Cycle", "No repeat"};  // K-169: the layer editor has the zones
    int m = c->layerMode;
    const char* hints[] = {"Every child plays each note", "One child, picked at random, plays each note", "The children take turns, one note each",
                           "A random child each note, never the same one twice in a row"};
    float mw = 150;
    const std::string mpv = listAt(i18n::tr("Mode"), i18n::tr(modes[std::clamp(m, 0, 3)]), mw);
    if (ui::BeginCombo("##lmode", mpv.c_str(), mw)) {
      for (int k = 0; k < 4; ++k) {
        if (ImGui::Selectable(i18n::tr(modes[k]), k == m) && k != m) a.doc.editChannel(id, "layer mode", [k](Channel& ch) { ch.layerMode = k; });
        ui::Hint(hints[k]);
      }
      ImGui::EndCombo();
    }
    ui::Hint("How the children share the notes");
  }
  if (!grid) {
    ImGui::Dummy(ImVec2(0, 2));
    ui::Label("DISPLAY GROUP");
  }
  const char* gname = c->group >= 0 && static_cast<size_t>(c->group) < s.channelGroups.size() ? s.channelGroups[static_cast<size_t>(c->group)].c_str() : "None";
  float gw = 150;
  const std::string gpv = listAt(i18n::tr("Group"), gname, gw);
  if (ui::BeginCombo("##dgroup", gpv.c_str(), gw)) {
    if (ImGui::Selectable(i18n::tr("None"), c->group < 0)) a.doc.editChannel(id, "ungroup channel", [](Channel& ch) { ch.group = -1; });
    for (size_t g = 0; g < s.channelGroups.size(); ++g)
      if (ImGui::Selectable((s.channelGroups[g] + "##dg" + std::to_string(g)).c_str(), c->group == static_cast<int>(g)))
        a.doc.editChannel(id, "group channel", [&](Channel& ch) { ch.group = static_cast<int>(g); });
    ImGui::EndCombo();
  }
  ui::Hint("Display group shown by the channel rack's group filter");
  ImGui::EndGroup();
  }
  if (all) ImGui::Dummy(ImVec2(0, theme::kS1));
  // MIDI input port: the channel plays what MIDI Out channels and plugins send on that port.
  if (on(5)) {
  ImGui::BeginGroup();
  if (!grid) ui::Label(i18n::tr("MIDI INPUT"));
  {
    auto portName = [](int p) { return p < 0 ? std::string(i18n::tr("None")) : i18n::tr("Port") + std::string(" ") + std::to_string(p); };
    float pw = 150;
    const std::string ppv = listAt(i18n::tr("MIDI in"), portName(c->midiInPort).c_str(), pw);
    if (ui::BeginCombo("##midiin", ppv.c_str(), pw)) {
      for (int p = -1; p < 64; ++p)
        if (ImGui::Selectable(portName(p).c_str(), p == c->midiInPort) && p != c->midiInPort)
          a.doc.editChannel(id, "MIDI input port", [p](Channel& ch) { ch.midiInPort = p; });
      ImGui::EndCombo();
    }
    ui::Hint(i18n::tr("This channel plays what MIDI Out channels and plugins send on this port: notes, controllers, pitch bend and program changes"));
    if (c->midiInPort >= 0) {
      std::vector<std::pair<ObjectId, ObjectId>> loops;
      const auto listeners = midiPortListeners(s, &loops);
      const int me = s.channelIndex(id);
      std::string from;
      for (size_t i = 0; i < listeners.size(); ++i)
        if (std::find(listeners[i].begin(), listeners[i].end(), me) != listeners[i].end())
          from += (from.empty() ? "" : ", ") + s.channels[i].name;
      bool loop = false;
      for (const auto& l : loops) loop = loop || l.second == id;
      if (grid) {  // (in the note text's place)
        float tw = 200;
        const ImVec2 at = grid->place(3, 0, tw);
        ImGui::SetCursorScreenPos(at);
        ImGui::PushTextWrapPos(at.x + tw);
      } else {
        ImGui::SameLine(0, theme::kS2);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0);
      }
      if (loop)
        ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("A sender on this port also listens to this channel: that link is left out"));
      else if (from.empty())
        ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Nothing sends on this port yet"));
      else
        ImGui::TextColored(theme::vec(P.textDim), i18n::tr("From: %s"), from.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::EndGroup();
  }
}

// The strip's content: volume / pan / pitch knobs and a two-octave preview
// keyboard, as one group (its height is the strip's content height).
static void drawStripContent(App& a, ObjectId id, StripDev& sd) {
  const auto& P = pal();
  ImGui::BeginGroup();
  ImGui::BeginGroup();
  ui::Label("CHANNEL");
  ui::ParamKnob("vol", channelParam(id, ChannelParam::Volume), 16.0f, "Volume");
  sd.volMin = ImGui::GetItemRectMin();
  sd.volMax = ImGui::GetItemRectMax();
  ImGui::SameLine(0, theme::kS4);
  ui::ParamKnob("pan", channelParam(id, ChannelParam::Pan), 16.0f, "Pan", true);
  ImGui::SameLine(0, theme::kS4);
  ui::SetNextKnobStep(1);  // whole cents (Shift: fine drag; double-click: 0, then type a value)
  ui::ParamKnob("pitch", channelParam(id, ChannelParam::Pitch), 16.0f, "Pitch", true);
  ui::Hint("Channel pitch in cents (100 = a semitone). Shift-drag for fine steps, double-click to reset or type a value. "
           "Plugins without their own note tuning follow it through pitch bend: set their bend range to the channel's "
           "Bend range (2 semitones unless changed)");
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS6);
  ImGui::BeginGroup();
  ui::Label("PREVIEW");
  {
    // Two octaves (C3..B4) filling the remaining width; click or drag to audition.
    int held = -1;
    constexpr int kWhite = 14, kBase = 48;
    const int whiteSemis[] = {0, 2, 4, 5, 7, 9, 11};
    const float kw = std::floor(std::clamp((ImGui::GetContentRegionAvail().x - 2) / kWhite, 14.0f, 28.0f));
    const float kh = 48.0f, bw = std::round(kw * 0.62f), bh = std::round(kh * 0.6f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##keys", ImVec2(kw * kWhite, kh));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto blackX = [&](int whiteIndex) { return p.x + kw * static_cast<float>(whiteIndex) - bw * 0.5f; };
    // Black keys sit after these white keys within an octave (C#, D#, F#, G#, A#).
    const int blackAfter[] = {1, 2, 4, 5, 6};
    const int blackSemi[] = {1, 3, 6, 8, 10};
    int hit = -1;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImVec2 m = ImGui::GetIO().MousePos;
      int wi = std::clamp(static_cast<int>((m.x - p.x) / kw), 0, kWhite - 1);
      hit = kBase + (wi / 7) * 12 + whiteSemis[wi % 7];
      if (m.y - p.y < bh)
        for (int o = 0; o < 2; ++o)
          for (int k = 0; k < 5; ++k) {
            float bx = blackX(o * 7 + blackAfter[k]);
            if (m.x >= bx && m.x < bx + bw) hit = kBase + o * 12 + blackSemi[k];
          }
    }
    // While held, the key under the pointer sounds: a drag plays one key at a time (KeyGlide.h).
    static keyglide::Held glide;
    held = keyglide::followItem(a, id, hit, glide, 0.8f);
    keyglide::devNote("chstrip", p, ImVec2(p.x + kw * kWhite, p.y + kh), kBase, kWhite, held);
    const ImU32 white = P.keyWhite, whiteHov = theme::mix(P.keyWhite, P.paper, 0.6f), black = P.keyBlack;
    // J-77: keys sounding on the channel (playback, MIDI, editors) light up and fade out.
    const keylight::ChannelLights* lit = keylight::lights(a, id);
    auto litMix = [&](ImU32 base, int note) {
      const float lv = lit ? lit->key(note) : 0.0f;
      return lv > 0.004f ? theme::mix(base, P.hot, 0.85f * lv) : base;
    };
    for (int i = 0; i < kWhite; ++i) {
      int note = kBase + (i / 7) * 12 + whiteSemis[i % 7];
      ImVec2 k0(p.x + kw * i, p.y), k1(k0.x + kw - 1, p.y + kh);
      ImU32 fill = held == note ? P.hot : litMix(hit == note ? whiteHov : white, note);
      dl->AddRectFilled(k0, k1, fill, 3, ImDrawFlags_RoundCornersBottom);
      if (i % 7 == 0) {
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, "C%d", note / 12 - 1);
        ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel - 1, 1e9f, 0, lbl);
        dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(std::round(k0.x + (kw - 1 - ts.x) * 0.5f), k1.y - ts.y - 3),
                    held == note ? P.ink : theme::mix(P.keyWhite, P.keyBlack, 0.45f), lbl);
      }
    }
    for (int o = 0; o < 2; ++o)
      for (int k = 0; k < 5; ++k) {
        int note = kBase + o * 12 + blackSemi[k];
        float bx = blackX(o * 7 + blackAfter[k]);
        ImU32 fill = held == note ? P.hot : litMix(hit == note ? theme::mix(P.keyBlack, P.paper, 0.15f) : black, note);
        dl->AddRectFilled(ImVec2(bx, p.y), ImVec2(bx + bw, p.y + bh), fill, 2, ImDrawFlags_RoundCornersBottom);
      }
    ui::Hint("Click or drag across the keys to audition the instrument (C3..B4)");
    sd.keysMin = p;
    sd.keysMax = ImVec2(p.x + kw * kWhite, p.y + kh);
    sd.keyW = kw;
  }
  ImGui::EndGroup();
  ImGui::EndGroup();
}

bool pluginWindowDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  if (cmd == "plugwin") {
    // plugwin chan <index> | fx <mixer index> <slot> | close: opens a channel's
    // window or an insert's editor, or closes every plugin window.
    const Song& s = a.doc.song();
    const std::string what = args.empty() ? "" : args[0];
    if (what == "chan" && args.size() > 1) {
      const size_t ci = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      if (ci < s.channels.size()) a.settingsChannel = s.channels[ci].id;
    } else if (what == "fx" && args.size() > 2) {
      const size_t mi = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      if (mi < s.mixer.size()) a.fxEditor = {s.mixer[mi].id, std::atoi(args[2].c_str())};
    } else if (what == "at" && args.size() > 2) {  // at <x> <y>: the channel window's top-left corner (tests)
      devPlace = ImVec2(static_cast<float>(std::atof(args[1].c_str())), static_cast<float>(std::atof(args[2].c_str())));
      devPlaceFrames = 4;
    } else if (what == "pick" && args.size() > 3) {  // 0.7.8: pick <mixer index> <slot> <type>: chosen in the slot's effect list (no engine sync first)
      const size_t mi = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      const int k = std::atoi(args[2].c_str());
      devRecordHostedDraws(true);
      if (mi < s.mixer.size()) {
        const ObjectId id = s.mixer[mi].id;
        if (a.doc.setSlotPlugin(id, k, args[3])) pui::pluginReplaced(PluginTarget{id, k});
        a.fxEditor = {id, k};
        a.selMixer = id;  // its inspector rows show it too
        a.settingsChannel = kNoId;
      }
    } else if (what == "moveto" && args.size() > 2) {  // 0.7.8: moveto <x> <y>: the first plugin window to that place of the main window
      const bool ok = devMoveHostedEditor(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()));
      std::printf("plugwin frame %d: moveto %s\n", frame, ok ? "done" : "no window");
      std::fflush(stdout);
    } else if (what == "dialog") {  // 0.7.8: a dialog (Save as template, as File > Save as template...)
      a.showSaveTemplate = true;
    } else if (what == "nodialog") {
      a.showSaveTemplate = false;
    } else if (what == "notify" && args.size() > 1) {  // 0.7.8: notify <text>: a notification card
      a.notify(args[1]);
    } else if (what == "step" && args.size() > 3) {  // 0.7.8: step <mixer index> <slot> <steps>: the effect moved down (up: negative), as the wheel does
      const size_t mi = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      if (mi < s.mixer.size()) {
        const ObjectId id = s.mixer[mi].id;
        const int k = std::atoi(args[2].c_str());
        const int to = mixops::stepSlot(a.doc, id, k, std::atoi(args[3].c_str()));
        if (a.fxEditor == std::make_pair(id, k)) a.fxEditor.second = to;
        std::printf("plugwin frame %d: step %d -> %d\n", frame, k, to);
        std::fflush(stdout);
      }
    } else if (what == "drawn") {  // 0.7.8: what the hosted plugin panels showed since "pick" (or the last "drawn")
      std::string line;
      for (const std::string& d : devTakeHostedDraws()) line += " [" + d + "]";
      std::printf("plugwin frame %d: drawn%s\n", frame, line.c_str());
      std::fflush(stdout);
    } else if (what == "scan" && args.size() > 2) {  // 0.7.8: scan <format> <bundle>: one plugin bundle known this run (in every layout)
      std::string bundle = args[2];  // (a path with spaces arrives in pieces)
      for (size_t k = 3; k < args.size(); ++k) bundle += " " + args[k];
      const host::Format* f = host::findFormat(args[1]);
      const host::ScanResult r = f ? f->scanInProcess(bundle) : host::ScanResult{};
      if (r.ok) host::registerScanResult(r);
      std::printf("plugwin frame %d: scan ok %d classes %zu\n", frame, r.ok ? 1 : 0, r.classes.size());
      std::fflush(stdout);
    } else if (what == "inst" && args.size() > 2) {  // 0.7.8: inst <channel index> <type>: the channel's instrument replaced, its window opened
      const size_t ci = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      if (ci < s.channels.size()) {
        const ObjectId id = s.channels[ci].id;
        devRecordHostedDraws(true);
        if (a.doc.setChannelPlugin(id, args[2])) pui::pluginReplaced(PluginTarget{id, -1});
        a.settingsChannel = id;
        a.fxEditor = {kNoId, -1};
        a.selectChannel(id);
      }
    } else if (what == "place" && args.size() > 1) {  // 0.7.8: place inside|own [<mixer index> <slot>]: the plugin put there, as its menu / bar do (else the open effect's, else the channel's)
      PluginTarget t = a.fxEditor.first != kNoId ? PluginTarget{a.fxEditor.first, a.fxEditor.second}
                                                     : PluginTarget{a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel, -1};
      if (const size_t mi = args.size() > 3 ? static_cast<size_t>(std::max(0, std::atoi(args[2].c_str()))) : s.mixer.size(); mi < s.mixer.size())
        t = PluginTarget{s.mixer[mi].id, std::atoi(args[3].c_str())};
      hostedui::putInterfaceInside(a, t, args[1] == "inside");
    } else if (what == "dockkey" && args.size() > 1) {  // 0.7.8: dockkey <kind> [space]: a view of the docked interface takes the keyboard
      const bool ok = hostedui::devDockedKey(std::atoi(args[1].c_str()), args.size() > 2 && args[2] == "space");
      std::printf("plugwin frame %d: dockkey %s %s\n", frame, args[1].c_str(), ok ? "done" : "no interface inside");
      std::fflush(stdout);
    } else if (what == "winshot" && args.size() > 1) {  // 0.7.8: winshot <png>: the main window as on screen (docked interfaces included)
      const bool ok = hostedui::devCaptureMainWindow(args[1]);
      std::printf("plugwin frame %d: winshot %s\n", frame, ok ? "written" : "not possible here");
      std::fflush(stdout);
    } else if (what == "bar" && args.size() > 1) {  // 0.7.8: bar <button>: a button of an installed plugin's own window's bar pressed
      pluginbar::devPress(args[1]);
    } else if (what == "cover" && args.size() > 1) {  // 0.7.8: cover <frames>: a tooltip over the interface inside Gridline
      hostedui::devCoverDocked(std::atoi(args[1].c_str()));
    } else if (what == "playing") {  // 0.7.8: the transport
      std::printf("plugwin frame %d: playing %d\n", frame, a.engine.playing() ? 1 : 0);
      std::fflush(stdout);
    } else if (what == "close") {
      a.closePluginWindows();
    } else if (what == "info") {  // K-9: open plugin windows and their native windows to stdout
      devPrintPluginWindows(frame);
    } else if (what == "fxtoggle" && args.size() > 2) {  // 0.7.8: fxtoggle <mixer index> <slot>: the effect's window button (mixer slot, Clips device card)
      const size_t mi = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
      if (mi < s.mixer.size()) togglePluginWindow(a, PluginTarget{s.mixer[mi].id, std::atoi(args[2].c_str())});
    } else if (what == "toggle" && args.size() > 1) {  // toggle <plugin type>: the channel button of the first channel with it clicked
      for (const auto& c : s.channels)
        if (c.plugin.type == args[1]) {
          togglePluginWindow(a, PluginTarget{c.id, -1});
          break;
        }
    } else if (what == "drag" && args.size() > 3) {  // drag <x> <y> <seconds>: the mouse dragged in the first one's interface
      const bool ok = devDragHostedEditor(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()), std::atof(args[3].c_str()));
      std::printf("plugwin frame %d: drag %s\n", frame, ok ? "started" : "not supported here");
      std::fflush(stdout);
    }
    return true;
  }
  if (cmd == "hostedparam" && args.size() >= 2) {
    // K-9: hostedparam chan <index> [<k> <value>] | fx <mixer index> <slot> [<k> <value>]: sets a parameter
    // of a hosted plugin as a knob does, then prints the instance's state and every value the song holds.
    const Song& s = a.doc.song();
    const bool isChan = args[0] == "chan";
    const size_t i = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
    const size_t rest = isChan ? 2 : 3;
    PluginTarget t{};
    const std::vector<float>* vals = nullptr;
    if (isChan && i < s.channels.size()) {
      t = {s.channels[i].id, -1};
      vals = &s.channels[i].plugin.params;
    } else if (!isChan && args.size() >= 3 && i < s.mixer.size()) {
      const int slot = std::atoi(args[2].c_str());
      t = {s.mixer[i].id, slot};
      if (slot >= 0 && slot < kEffectSlots) vals = &s.mixer[i].slots[static_cast<size_t>(slot)].plugin.params;
    }
    if (!vals) {
      std::printf("hostedparam frame %d: no such target\n", frame);
      std::fflush(stdout);
      return true;
    }
    if (args.size() >= rest + 2) {
      const int k = std::atoi(args[rest].c_str());
      a.doc.setParam(isChan ? channelPluginParam(t.owner, k) : slotParam(t.owner, t.slot, k), std::strtof(args[rest + 1].c_str(), nullptr), false);
      a.syncEngine();
    }
    host::HostedInstance* hi = hostedInstanceFor(a, t);
    std::printf("hostedparam frame %d: instance %d sandboxed %d crashed %d latency %d open %d params %zu\n", frame, hi ? 1 : 0, hi && hi->sandboxed() ? 1 : 0,
                hi && hi->crashed() ? 1 : 0, hi ? hi->totalLatency() : 0, hostedEditorOpen(t) ? 1 : 0, vals->size());
    for (size_t k = 0; k < vals->size(); ++k) std::printf("  param %zu %.6f\n", k, static_cast<double>((*vals)[k]));
    std::fflush(stdout);
    return true;
  }
  if (cmd == "layered") {  // K-169: the layer editor's last frame (text boxes, overlaps, lanes, macros) to stdout
    // layered load <n> | file <.gllayer>: adds a built-in layer / a preset file at the end of the rack (as a
    // browser drop does) and opens its window; layered view keys|velocity: the zone view.
    if (!args.empty() && (args[0] == "load" || args[0] == "file") && args.size() > 1) {
      Json doc;
      std::string err;
      if (args[0] == "load") {
        const auto f = layer::factoryPresets();
        const size_t n = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
        if (n < f.size()) doc = f[n].second;
      } else {
        layer::readPreset(args[1], doc, &err);
      }
      const ObjectId l = doc.is(Json::Type::Object) ? layer::loadPreset(a.doc, doc, -1, &err) : kNoId;
      if (l != kNoId) {
        a.selectChannel(l);
        a.settingsChannel = l;
      }
      a.syncEngine();
      std::printf("layered frame %d: loaded %d index %d %s\n", frame, l != kNoId ? 1 : 0, a.doc.song().channelIndex(l), err.c_str());
      std::fflush(stdout);
      return true;
    }
    if (!args.empty() && args[0] == "save" && args.size() > 1) {  // layered save <file>: the selected layer as a preset file
      bool ok = false;
      for (const auto& c : a.doc.song().channels)
        if (c.isLayer() && c.id == a.settingsChannel) ok = writeFileAtomic(args[1], jsonStringify(layer::presetToJson(a.doc.song(), c.id)) + "\n");
      std::printf("layered frame %d: saved %d\n", frame, ok ? 1 : 0);
      std::fflush(stdout);
      return true;
    }
    const layerui::DevState& d = layerui::devState();
    std::printf("layered frame %d: drawn %d texts %d overlaps %d box %.0f %.0f %.0f %.0f lanes %zu\n", frame, d.frame, d.texts, d.overlaps, d.minX, d.minY,
                d.maxX, d.maxY, d.lanes.size());
    for (const auto& o : d.overlapping) std::printf("  overlap %s\n", o.c_str());
    for (size_t k = 0; k < d.lanes.size(); ++k)
      std::printf("  lane %zu %.0f %.0f %.0f %.0f zone %.0f %.0f\n", k, d.lanes[k].x0, d.lanes[k].y0, d.lanes[k].x1, d.lanes[k].y1, d.lanes[k].zx0,
                  d.lanes[k].zx1);
    for (int k = 0; k < 8; ++k) std::printf("  macro %d %.0f %.0f\n", k, d.macroX[k], d.macroY[k]);
    const Song& s = a.doc.song();
    for (const auto& c : s.channels)
      if (c.isLayer()) {
        std::printf("  layer %s mode %d children %zu zones %zu links", c.name.c_str(), c.layerMode, c.layer.size(), c.layerZones.size());
        int links = 0;
        for (const auto& l : s.modLinks) links += l.sourceOwner == c.id && l.sourceSlot == kLayerMacroSlot;
        std::printf(" %d\n", links);
        for (const auto& z : c.layerZones)
          std::printf("    zone %llu keys %d %d fades %d %d vel %d %d transpose %d solo %d\n", static_cast<unsigned long long>(z.child), z.keyLo, z.keyHi,
                      z.keyFadeLo, z.keyFadeHi, z.velLo, z.velHi, z.transpose, z.solo ? 1 : 0);
      }
    std::fflush(stdout);
    return true;
  }
  if (cmd == "macros") {  // K-245: the macro panel's last frame and every macro link, to stdout
    // macros link c|m <channel or mixer index> <parameter index> <macro>: links a parameter of that
    // channel (plugin parameter k = kChannelPluginBase + k) or mixer track to the owner's macro, as
    // the menu does; macros learn c|m <index> <macro>: starts learning (the next moved control).
    const Song& s = a.doc.song();
    auto ownerOf = [&](const std::string& kind, const std::string& ix) -> ObjectId {
      const size_t i = static_cast<size_t>(std::max(0, std::atoi(ix.c_str())));
      if (kind == "c") return i < s.channels.size() ? s.channels[i].id : kNoId;
      return i < s.mixer.size() ? s.mixer[i].id : kNoId;
    };
    if (args.size() >= 5 && args[0] == "link") {
      const ObjectId o = ownerOf(args[1], args[2]);
      const bool ok = o != kNoId && macro::link(a.doc, ParamId{o, static_cast<uint32_t>(std::atoi(args[3].c_str()))}, o, std::atoi(args[4].c_str()));
      std::printf("macros frame %d: linked %d\n", frame, ok ? 1 : 0);
    } else if (args.size() >= 2 && args[0] == "open") {  // open <channel index>: its window on the Misc page (in the Tracker, as "Open plugin window")
      const ObjectId o = ownerOf("c", args[1]);
      if (o != kNoId) {
        if (layoutui::active() == layout::Preset::Tracker) trackereditors::openPluginWindow(a, o);
        else a.settingsChannel = o;
        a.channelTab = 1;
      }
      std::printf("macros frame %d: opened %d\n", frame, o != kNoId ? 1 : 0);
    } else if (args.size() >= 4 && args[0] == "learn") {
      macroui::devLearn(a, ownerOf(args[1], args[2]), std::atoi(args[3].c_str()));
      std::printf("macros frame %d: learning\n", frame);
    } else {
      const macroui::DevState& d = macroui::devState();
      std::printf("macros frame %d: drawn %d owner %llu selected %d learning %d box %.0f %.0f %.0f %.0f assigned %zu\n", frame, d.frame,
                  static_cast<unsigned long long>(d.owner), d.selected, d.learning, d.x0, d.y0, d.x1, d.y1, macro::assignedMacros(s).size());
      for (int k = 0; k < 8; ++k) std::printf("  knob %d %.0f %.0f\n", k, d.knobX[k], d.knobY[k]);
      for (const auto& l : s.modLinks)
        if (l.sourceSlot == kLayerMacroSlot)
          std::printf("  link %s -> %s range %.3f %.3f\n", controllerOutputName(s, l.sourceOwner, l.sourceSlot, l.sourceOutput).c_str(),
                      paramDisplayName(s, ParamId{l.owner, l.index}).c_str(), static_cast<double>(l.min), static_cast<double>(l.max));
    }
    std::fflush(stdout);
    return true;
  }
  if (cmd != "strip") return false;
  // strip [on|off] [frames]: "on" / "off" show or hide the strip at once (no
  // slide) when the window is next drawn, a test set-up; then the state drawn
  // this frame and the next `frames` frames (the dump comes from the drawing,
  // after this frame's input).
  size_t n = 0;
  if (!args.empty() && (args[0] == "on" || args[0] == "off")) {
    stripState(a).devSet = args[0] == "on" ? 1 : 0;
    n = 1;
  }
  StripDev& d = stripDev();
  d.dumpLeft = 1 + (args.size() <= n ? 0 : std::max(0, std::atoi(args[n].c_str())));
  d.dumpFrame = frame;
  return true;
}

// K-112 (R3): the channel settings' content, drawn by its window or by a layout part's own frame.
void drawChannelSettingsBody(App& a, ObjectId channel, const BodyFrame& f) {
  const layoutui::BaseBorderScope border;  // K-112b: plugin windows keep their outlined controls
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  if (!c) return;
  hostbody::drawn(hostbody::Body::ChannelSettings, c->id, -1);
  const PluginInfo* info = findPlugin(c->plugin.type);
  const float editorW = std::floor(f.width);
  const ObjectId id = c->id;
  if (f.zoom) edzoomui::markRows("channelsettings");  // K-216: the window's own rows (fit report; not in a layout part)
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowAppearing()) {
    a.focusPluginOwner = id;  // controller template knobs follow the focused plugin
    a.focusPluginSlot = -1;
  }

  // Header: colour, name, instrument, routing. K-279: the user may hide it (instrument menu, or
  // right-click the header) to give the plugin page the room; the window title keeps the name.
  const auto& P = pal();
  const float fh = ImGui::GetFrameHeight();
  const bool showHeader = !info || a.settings.getBool("ui.channelHeader");
  StripState& strip = stripState(a);
  StripDev& sd = stripDev();
  if (showHeader) {
    uint32_t col = c->color;
    if (ui::ColorSwatch("col", &col, ImVec2(fh, fh))) a.doc.editChannel(id, "change channel colour", [&](Channel& x) { x.color = col; });
    ui::Hint("Channel colour");
    ImGui::SameLine(0, theme::kS2);
    static char nameBuf[128];
    static ObjectId nameFor = kNoId;
    if (nameFor != id || !ImGui::IsAnyItemActive()) {
      std::snprintf(nameBuf, sizeof nameBuf, "%s", c->name.c_str());
      nameFor = id;
    }
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit()))
      if (c->name != nameBuf) a.doc.renameChannel(id, nameBuf);
    ui::FocusRing();
    ui::Hint("Channel name");
    ImGui::SameLine(0, theme::kS2);
    if (ui::BeginCombo("##inst", info ? info->name.c_str() : (c->isLayer() ? "Layer" : "None"), 170)) {
      for (const CatalogGroup& g : catalogGroups(PluginKind::Generator)) {  // each category once
        ui::Label(g.category.c_str());
        for (const PluginInfo* p : g.items)
          if (ImGui::Selectable(p->name.c_str(), p->type == c->plugin.type) && a.doc.setChannelPlugin(id, p->type))
            pui::pluginReplaced(PluginTarget{id, -1}), a.showNewInstrument(id);  // J-213: the preset selector starts over; K-125
      }
      ImGui::EndCombo();
    }
    ui::Hint("Instrument");
    ImGui::SameLine(0, theme::kS2);
    // J-195: room for the free-track button and the strip eye after the list (K-147: fits a 600 px
    // window). The box is the one the Audio clip window's header has too (ChannelRoute.h).
    if (chanroute::box(a, id, "##route", std::max(100.0f, ImGui::GetContentRegionAvail().x - 2 * (fh + theme::kS2)), "chroute")) pendingFreeRoute(a) = id;
    // J-195: route this channel to the next free mixer track (named after it).
    ImGui::SameLine(0, theme::kS2);
    if (chanroute::button(a, id, fh)) pendingFreeRoute(a) = id;
    devNoteRect("chroutefree");
    // The channel strip's show / hide eye (the mixer's eye icon and button).
    ImGui::SameLine(0, theme::kS2);
    const bool stripOn = strip.shown;
    if (ui::IconButton("##strip", ui::eyeIcon(stripOn), stripOn, fh,
                       stripOn ? "Hide the channel strip (volume, pan, pitch, keyboard)" : "Show the channel strip (volume, pan, pitch, keyboard)"))
      strip.shown = !stripOn;
    sd.eyeMin = ImGui::GetItemRectMin();
    sd.eyeMax = ImGui::GetItemRectMax();
    if (info && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        ImGui::IsMouseHoveringRect(ImVec2(ImGui::GetWindowPos().x, sd.eyeMin.y), ImVec2(sd.eyeMax.x + 4, sd.eyeMax.y)))
      ImGui::OpenPopup("##chheadermenu");
    if (ImGui::BeginPopup("##chheadermenu")) {
      if (ImGui::MenuItem(i18n::tr("Hide this header"))) a.settings.set("ui.channelHeader", false);
      ui::Hint("Colour, name, instrument and mixer track; show it again from the instrument menu");
      ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }

  // Strip: volume / pan / pitch + a two-octave preview keyboard, shown with the
  // header's eye (ChannelStrip.h): it slides down from under the header into a
  // clipped area that grows with it, and back up when hidden.
  chstrip::Reveal& rv = stripReveal(a);
  const double now = ImGui::GetTime();
  rv.set(strip.shown, now, scrollui::animations());
  const float frac = static_cast<float>(rv.at(now));
  const float sp = ImGui::GetStyle().ItemSpacing.y;
  // The strip's full height: its content (measured when drawn) and the gaps
  // around it, as the window had it before the strip could hide.
  float& contentH = stripContentH(a);
  const float fullH = contentH + theme::kS1 + 2 * sp;
  const float shownH = std::round(frac * fullH);
  sd.frac = frac;
  sd.moving = rv.moving(now);
  sd.shownH = shownH;
  sd.fullH = fullH;
  sd.drawn = false;
  if (shownH >= 1.0f) {
    ImGui::BeginChild("##chstrip", ImVec2(ImGui::GetContentRegionAvail().x, shownH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    sd.drawn = true;
    sd.clipMin = ImGui::GetWindowPos();
    sd.clipMax = ImVec2(sd.clipMin.x + ImGui::GetWindowWidth(), sd.clipMin.y + shownH);
    ImGui::SetCursorPosY(shownH - fullH);  // slid up by the part not shown yet
    drawStripContent(a, id, sd);
    contentH = ImGui::GetItemRectSize().y;
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - sp);  // the area is exactly shownH high
  }
  sd.open = true;
  sd.shown = strip.shown;
  sd.winPos = ImGui::GetWindowPos();
  sd.winSize = ImGui::GetWindowSize();
  sd.belowY = ImGui::GetCursorScreenPos().y;  // where the tabs start (moves with the strip)
  stripDevFrame();

  // Studio channel window: the instrument page and the channel's misc
  // functions (tuning, polyphony, cut groups, echo, layering) on tabs.
  int& tab = a.channelTab;
  // J-72: instruments that run the channel envelopes get the envelope page
  // (tab 2, shown between the instrument and the misc page).
  const bool envPage = info && insSupported(info->type);
  if (!envPage && tab == 2) tab = 0;
  // K-223: the tab row's start, its width and where its tabs end: the plugin page's header row
  // (menu, presets, A/B) sits on it, after the tabs, when it fits there (one row less in height).
  const ImVec2 tabRow = ImGui::GetCursorScreenPos();
  const float tabRowRight = tabRow.x + ImGui::GetContentRegionAvail().x;
  {
    const char* tabs[] = {"Plugin", "Envelope", "Misc"};
    const char* hints[] = {"The instrument's editor, its sample and note processors",
                           "Envelopes, LFOs and the filter this channel applies to every note",
                           "Tuning, timing, polyphony, cut groups, echo, layering and display group of this channel"};
    if (envPage) {
      static const int kOf[3] = {0, 2, 1}, kAt[3] = {0, 2, 1};  // visual position <-> tab id
      int vis = kAt[std::clamp(tab, 0, 2)];
      if (ui::TabStrip("##chtabs", tabs, 3, &vis, hints)) tab = kOf[vis];
    } else {
      const char* two[] = {tabs[0], tabs[2]};
      const char* twoHints[] = {hints[0], hints[2]};
      ui::TabStrip("##chtabs", two, 2, &tab, twoHints);
    }
  }
  const float tabsEnd = ImGui::GetItemRectMax().x;  // (the strip ends with a spacer as wide as its tabs)
  if (tab == 2) {
    drawChannelEnvelope(a, id);
    return;
  }
  if (tab == 1) {
    drawNoteSettingsPart(a, id, -1);
    ImGui::Dummy(ImVec2(0, theme::kS2));  // K-245: the channel's macro knobs
    macroui::drawPanel(a, id, std::min(editorW, 600.0f));
    // K-279: the note processors (arpeggiator, chords, ...) live on the Misc page; J-151: not for
    // instruments that leave them out (while none is set)
    if (const Channel* cc = a.doc.song().channel(id); !info || info->noteFxPanel || (cc && !cc->noteFx.empty())) {
      ImGui::Dummy(ImVec2(0, theme::kS1));
      packs::modulation::drawChannelNoteFx(a, id, 600.0f);
    }
    return;
  }

  // Sample (sampler only).
  if (info && info->type == "gl.sampler") {  // other sample instruments have their own sample UI
    SamplePtr smp = c->plugin.data.empty() ? nullptr : sampleCache().request(c->plugin.data);  // J-30: non-blocking
    drawWaveform(a, id, smp, ImVec2(600, 64));  // K-85: the whole-file overview with the start / end handles (the editor below zooms into the region and its loop)
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
        std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
        if (isAudioFile(path)) browserui::loadChannelSample(a, id, path, "load sample");  // J-97: same name policy as the rack
      }
      ImGui::EndDragDropTarget();
    }
    if (ui::TextButton("Load sample...", false, 0, "Choose an audio file for this channel")) {
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.title = "Load sample";
      a.fileDialog.dir = c->plugin.data.empty() ? (gl::platform::homeEnv() ? gl::platform::homeEnv() : ".") : fs::path(c->plugin.data).parent_path().string();
      a.fileDialog.exts = audioFileExtensions();
      a.fileDialog.onOk = [&a, id](const std::string& p) {
        a.doc.editChannel(id, "load sample", [&](Channel& x) { x.plugin.data = p; });
        if (!sampleCache().get(p)) a.notify("Cannot decode " + p, true);
      };
    }
    ImGui::SameLine();
    if (!c->plugin.data.empty() && smp) {
      // Opens the sample in Loupe; its edits come back to this channel (one undo step each).
      if (ui::TextButton("Edit in Loupe", false, 0, "Edit this sample in Loupe: every edit comes back to this channel"))
        packs::openAudioEditor(a, c->plugin.data, id);
      ImGui::SameLine();
    }
    if (!c->plugin.data.empty()) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", fs::path(c->plugin.data).filename().string().c_str());
      if (!smp) {
        ImGui::SameLine();
        ImGui::TextColored(theme::vec(pal().danger), "%s", i18n::tr("missing"));
      }
    }
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, theme::kS1));
  }

  // Instrument editor.
  if (info) {
    PluginTarget target{id, -1};
    // K-223: the header row goes on the tab row when it fits beside the tabs (the menu button and
    // the header's own width, known before it is drawn so the window keeps one height from its first
    // frame; the sampler's sample row comes between them, so it keeps its own row).
    const float need = ImGui::GetFrameHeight() + 8 + pluginHeaderWidth(a, *info);
    // K-286: never a two-row header (the Smooth layout's): its second row would lie over the editor
    const bool onTabs = info->type != "gl.sampler" && !pluginHeaderTwoRows(a) && tabsEnd + theme::kS6 + need <= tabRowRight;
    const ImVec2 below = ImGui::GetCursorScreenPos();
    if (onTabs) ImGui::SetCursorScreenPos(ImVec2(tabsEnd + theme::kS6, tabRow.y + std::floor((28.0f - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::BeginGroup();
    menuButton("plmenu", "Instrument menu: presets, state, channel", [&a, id]() { pluginWindowMenu(a, id, -1); });
    ImGui::SameLine(0, 8);
    drawPluginHeader(a, target, !onTabs);
    ImGui::EndGroup();
    if (onTabs) ImGui::SetCursorScreenPos(below);
    if (logicplug::controlsView(a, info->type)) {  // K-112b: the plain controls list
      logicplug::drawControls(a, target, editorW);
    } else {
      if (f.zoom) edzoomui::begin(a, "channelsettings");  // K-210b: the editor's region (the only part the editor size zooms)
      drawPluginBody(a, target, editorW);
      if (f.zoom) edzoomui::end(a, "channelsettings");
    }
  }
  if (!info) {  // K-169: the layer editor (children, zones, macros, layer presets)
    if (f.zoom) edzoomui::begin(a, "channelsettings");
    layerui::drawEditor(a, id, editorW);
    if (f.zoom) edzoomui::end(a, "channelsettings");
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));  // K-279: a margin under the editor (the note processors moved to Misc)
}

// The sampler channel whose settings went to the Audio clip window (kNoId: none).
static ObjectId gClipWindowChannel = kNoId;

void drawChannelSettings(App& a) {
  // K-106 cp22 (dev): the frames a window showed in the Tracker unasked, and the frames the gate below kept one shut
  trackereditors::watchWindows(a, a.settingsChannel != kNoId && trackereditors::ownsChannelWindow(a) &&
                                      !hostbody::claimed(hostbody::Body::ChannelSettings));
  routePluginWindows(a);  // installed plugins open as themselves
  // J-195: the free-track route asked for last frame (run before the window
  // takes its pointers into the song, which the route changes).
  if (const ObjectId r = std::exchange(pendingFreeRoute(a), kNoId); r != kNoId && r == a.settingsChannel) chanroute::routeToFree(a, r);
  stripDev().open = false;
  // 0.7.8: a sampler's settings shown in the Audio clip window close with the channel settings
  // (another channel's settings asked for, or closed by any path): the window closes too.
  if (gClipWindowChannel != kNoId && a.settingsChannel != gClipWindowChannel) {
    clipeditor::closeChannel(a, gClipWindowChannel);
    gClipWindowChannel = kNoId;
  }
  const Song& s = a.doc.song();
  const Channel* c = a.settingsChannel == kNoId ? nullptr : s.channel(a.settingsChannel);
  if (!c) {
    a.settingsChannel = kNoId;
    gClipWindowChannel = kNoId;
    stripState(a).open = false;  // closed: the strip is hidden when it opens again
    stripDevFrame();             // (dev) "window closed"
    return;
  }
  // K-106 cp22: in the Tracker the lower band hosts the settings; the window opens only from "Open plugin
  // window". A click that opened them in the band (the list, Instrument, Sample, a double-click, the keys)
  // drew this window for a frame or more before the band's Device chain claimed them (it draws first next
  // frame, or later in this one).
  if (trackereditors::ownsChannelWindow(a)) return;
  if (hostbody::claimed(hostbody::Body::ChannelSettings)) return;  // K-112: a layout part draws it in its own frame
  if (logicplug::hidden()) return;  // K-112b: hidden (V), still open
  // 0.7.8: a sampler channel's settings live in the Audio clip window (its Sample, Playback,
  // Envelope, Sample FX and Channel tabs), with or without a clip of it: the window opens there,
  // and the channel's settings count as open while it shows the channel (closed with it).
  {
    ObjectId& redirected = gClipWindowChannel;
    if (clipeditor::takesChannel(a, c->id)) {
      const ObjectId id = c->id;
      if (redirected != id) {
        redirected = id;
        stripState(a).open = false;
        clipeditor::openChannel(a, id, a.channelTab == 1 ? clipwin::TabChannel : clipwin::TabPlayback);
      } else if (!clipeditor::showsChannel(a, id)) {  // the window closed, or shows another channel now
        redirected = kNoId;
        a.settingsChannel = kNoId;
      }
      return;
    }
    redirected = kNoId;
  }
  const PluginInfo* info = findPlugin(c->plugin.type);
  std::string title = c->name + "###channelsettings";
  bool open = true;
  stripOpened(a, c->id);  // opened (or another channel): the strip starts hidden
  // Wide instrument editors (PluginInfo::editorWidth) widen the window.
  float editorW = info && info->editorWidth > 0 ? info->editorWidth : 600.0f;
  if (info && isHostedType(info->type)) editorW = hostedui::dockedWidth(a, PluginTarget{c->id, -1}, editorW);  // 0.7.8: its own interface inside
  if (!info) editorW = layerui::editorWidth();  // K-169: an instrument-less channel is a layer: the layer editor
  if (info && info->editorMinWidth > 0) {  // J-151: reflowing editors follow a narrower main window (J-158: as far as they fit)
    const ImVec2 area = ui::EditorArea();
    editorW = place::fitEditor(editorW, info->editorMinWidth, 24.0f + ImGui::GetStyle().ScrollbarSize, area.x, area.y).content;
  }
  editorW = std::floor(editorW);
  const float winW = std::max(624.0f, editorW + 24.0f);
  // K-147: the channel's own rows and the note processors need kChannelMinW at least (K-210b: at the
  // interface size; the zoomed region holds the editor alone).
  constexpr float kChannelMinW = 600.0f;
  // 0.7.8: the zoomed region is the editor's own width, so an editor narrower than the rows sits in
  // the middle of the window (it sat at the left of a region as wide as the rows, the room all at
  // its right).
  const float regionW = editorW;
  {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (devPlaceFrames > 0) {  // (dev) plugwin at <x> <y>: the top-left corner held there for a few frames
      ImGui::SetNextWindowPos(devPlace, ImGuiCond_Always);
      --devPlaceFrames;
    }
  }
  // K-19: the window's own size on top of the interface scale (installed plugins keep theirs).
  const bool zoomable = !info || !isHostedType(info->type);
  // K-210b: only the editor itself zooms; the channel's own rows above and below it (kChannelMinW wide) keep the interface size
  edzoomui::prepare(a, "channelsettings", info ? info->type : std::string("(layer)"), zoomable ? regionW : winW, zoomable, kChannelMinW);
  // K-79: the performance preset docks the channel settings in its bottom panel (they scroll there).
  // K-199: a Game mode session shows one window: built-in editors dock with the main view there.
  const bool gameDock = layoutui::active() != layout::Preset::Live && zoomable && gamepadui::dockPlugins(a);
  // N53-06 (checkpoint 54): in Clips the window docks only where it is docked; floating (the device's
  // window button) it sizes to its editor as everywhere, capped to the work area, the rest scrolling.
  const ImGuiWindow* was = ImGui::FindWindowByName(title.c_str());
  const bool docks = (layoutui::active() == layout::Preset::Live && was && was->DockIsActive) || gameDock;
  if (gameDock) gamepadui::nextPluginWindowDocked(a);
  const int fitFlags =  // J-158: capped to the work area, the rest scrolls
      docks ? 0 : zoomable ? edzoomui::nextWindowSize("channelsettings") : ui::NextFloatingWindowSize(winW);
  viewports::openAttached(title.c_str());  // opens inside the main window, never popped out by itself
  // K-112b: the title bar's underline (drawn by Begin from the frame outline) stays as in every layout
  bool vis = false;
  {
    const layoutui::BaseBorderScope border;
    vis = ImGui::Begin(title.c_str(), &open, docks ? ImGuiWindowFlags_HorizontalScrollbar : (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | fitFlags));
  }
  ui::KeepWindowInViewport();
  ui::MarkDetachable();  // J-114: pops out / docks back like the panels
  if (!vis) {
    ImGui::End();
    if (!open) a.settingsChannel = kNoId;
    return;
  }
  drawChannelSettingsBody(a, c->id, BodyFrame{editorW, true, ""});
  ImGui::End();
  if (!open) a.settingsChannel = kNoId;
}

}  // namespace gl
