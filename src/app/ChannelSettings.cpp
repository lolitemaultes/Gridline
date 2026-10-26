// Gridline — channel settings window: strip, routing, instrument parameters,
// sample loading and waveform for the sampler.
#include <algorithm>
#include "KeyLight.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/PluginParams.h"
#include "../engine/ChannelIns.h"
#include "App.h"
#include "HostedUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "PluginUI.h"
#include "BrowserUi.h"
#include "Windows.h"
#include "../packs/modulation/modulation_ui.h"
#include "../core/Platform.h"
#include "ChannelStrip.h"
#include "Viewports.h"
#include "SmoothScrollUi.h"
#include <map>

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

static void drawWaveform(App& a, const SamplePtr& smp, ImVec2 size, float startFrac) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.surface950, 6);
  if (!smp || smp->frames() == 0) {
    const char* msg = "Drop an audio file here or click Load";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), P.textFaint, msg);
    return;
  }
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
  float sx = p.x + startFrac * size.x;
  if (startFrac > 0) dl->AddRectFilled(p, ImVec2(sx, p.y + size.y), theme::withAlpha(P.surface950, 0.6f));
  dl->AddLine(ImVec2(sx, p.y), ImVec2(sx, p.y + size.y), P.accent, 1.5f);
  char info[96];
  std::snprintf(info, sizeof info, "%.2f s  ·  %.0f Hz", smp->seconds(), smp->sampleRate);
  ImGui::PushFont(a.fontMono, 10.5f);
  dl->AddText(ImVec2(p.x + 8, p.y + 6), P.textDim, info);
  ImGui::PopFont();
}

// ---- Channel strip state (ChannelStrip.h): per application, whether the open
// window shows it, its slide and the content height measured when it was last
// drawn; dev checks read StripDev.
namespace {
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
// layer children and display group of a channel.
static void drawNoteSettings(App& a, ObjectId id) {
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
  // One knob cell (knob, value, name). `set` writes the value into the channel.
  auto knob = [&](const char* key, double v, const ParamDesc& d, bool bipolar, const std::function<void(Channel&, double)>& set,
                  const char* valueText = nullptr) {
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
    ts = ImGui::CalcTextSize(d.name.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(P.textDim), "%s", d.name.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPosX(x);
    ImGui::Dummy(ImVec2(cell, 0));
    ImGui::EndGroup();
    ImGui::PopID();
  };
  auto toggle = [&](const char* label, bool on, const char* hint, const std::function<void(Channel&)>& flip) {
    if (ui::TextButton(label, on, 80, hint)) a.doc.editChannel(id, std::string("toggle ") + label, flip);
  };
  char buf[48];

  // Tuning & time.
  ImGui::BeginGroup();
  ui::Label("TUNE & TIME");
  knob("fine", c->fine, desc("Fine", "ct", -200, 200, 0), true, [](Channel& ch, double v) { ch.fine = static_cast<int>(std::lround(v)); });
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(c->swingMix * 100)));
  knob("swing", c->swingMix, desc("Swing", "", 0, 1, 1), false, [](Channel& ch, double v) { ch.swingMix = static_cast<float>(v); }, buf);
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%.3f beat", c->timeShift);
  knob("shift", c->timeShift, desc("Shift", "", 0, 1, 0), false, [](Channel& ch, double v) { ch.timeShift = static_cast<float>(v); }, buf);
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS4);
  // Polyphony.
  ImGui::BeginGroup();
  ui::Label("POLYPHONY");
  std::snprintf(buf, sizeof buf, "%s", c->polyphony == 0 ? "all" : std::to_string(c->polyphony).c_str());
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
  ImGui::SameLine(0, theme::kS4);
  // Cut groups.
  ImGui::BeginGroup();
  ui::Label("CUT GROUP");
  std::snprintf(buf, sizeof buf, "%s", c->cutGroup ? std::to_string(c->cutGroup).c_str() : "off");
  knob("cut", c->cutGroup, desc("Cuts", "", 0, kMaxCutGroups, 0), false, [](Channel& ch, double v) { ch.cutGroup = static_cast<int>(std::lround(v)); }, buf);
  ui::Hint("Notes of this channel cut channels that are cut by this group");
  ImGui::SameLine(0, 0);
  std::snprintf(buf, sizeof buf, "%s", c->cutBy ? std::to_string(c->cutBy).c_str() : "off");
  knob("cutby", c->cutBy, desc("Cut by", "", 0, kMaxCutGroups, 0), false, [](Channel& ch, double v) { ch.cutBy = static_cast<int>(std::lround(v)); }, buf);
  ui::Hint("This channel is cut by notes of channels in this group (same number in both = cuts itself)");
  ImGui::EndGroup();

  ImGui::Dummy(ImVec2(0, theme::kS1));
  // Echo.
  ImGui::BeginGroup();
  ui::Label("ECHO");
  std::snprintf(buf, sizeof buf, "%s", c->echo.count ? std::to_string(c->echo.count).c_str() : "off");
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
  knob("epitch", c->echo.pitch, desc("Pitch", "ct", -1200, 1200, 0), true, [](Channel& ch, double v) { ch.echo.pitch = static_cast<int>(std::lround(v)); });
  ImGui::SameLine(0, 0);
  knob("emodx", c->echo.modX, desc("Mod X per echo", "%", -1, 1, 0), true, [](Channel& ch, double v) { ch.echo.modX = static_cast<float>(v); });
  ImGui::SameLine(0, 0);
  knob("emody", c->echo.modY, desc("Mod Y per echo", "%", -1, 1, 0), true, [](Channel& ch, double v) { ch.echo.modY = static_cast<float>(v); });
  ImGui::SameLine(0, 4);
  toggle("Ping-pong", c->echo.pingPong, "Echoes alternate between the pan position and its mirror",
         [](Channel& ch) { ch.echo.pingPong = !ch.echo.pingPong; });
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS4);
  // Layer + display group.
  ImGui::BeginGroup();
  ui::Label("LAYER");
  std::string preview = c->layer.empty() ? "No children" : std::to_string(c->layer.size()) + " children";
  if (ui::BeginCombo("##layer", preview.c_str(), 150)) {
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
    const char* modes[] = {"All", "Random"};
    int m = c->layerMode;
    const char* hints[] = {"Every child plays each note", "One child, picked at random, plays each note"};
    if (ui::Segmented("##lmode", modes, 2, &m, 75, 0, hints)) a.doc.editChannel(id, "layer mode", [&](Channel& ch) { ch.layerMode = m; });
  }
  ImGui::Dummy(ImVec2(0, 2));
  ui::Label("DISPLAY GROUP");
  const char* gname = c->group >= 0 && static_cast<size_t>(c->group) < s.channelGroups.size() ? s.channelGroups[static_cast<size_t>(c->group)].c_str() : "None";
  if (ui::BeginCombo("##dgroup", gname, 150)) {
    if (ImGui::Selectable(i18n::tr("None"), c->group < 0)) a.doc.editChannel(id, "ungroup channel", [](Channel& ch) { ch.group = -1; });
    for (size_t g = 0; g < s.channelGroups.size(); ++g)
      if (ImGui::Selectable((s.channelGroups[g] + "##dg" + std::to_string(g)).c_str(), c->group == static_cast<int>(g)))
        a.doc.editChannel(id, "group channel", [&](Channel& ch) { ch.group = static_cast<int>(g); });
    ImGui::EndCombo();
  }
  ui::Hint("Display group shown by the channel rack's group filter");
  ImGui::EndGroup();
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
  ui::ParamKnob("pitch", channelParam(id, ChannelParam::Pitch), 16.0f, "Pitch", true);
  ImGui::EndGroup();
  ImGui::SameLine(0, theme::kS6);
  ImGui::BeginGroup();
  ui::Label("PREVIEW");
  {
    // Two octaves (C3..B4) filling the remaining width; click or drag to audition.
    static int held = -1;
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
    if (ImGui::IsItemActive() && hit >= 0 && held != hit) {
      if (held >= 0) a.engine.noteOff(id, held);
      a.engine.noteOn(id, hit, 0.8f);
      held = hit;
    }
    if (!ImGui::IsItemActive() && held >= 0) {
      a.engine.noteOff(id, held);
      held = -1;
    }
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
    } else if (what == "close") {
      a.closePluginWindows();
    }
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

void drawChannelSettings(App& a) {
  routePluginWindows(a);  // installed plugins open as themselves
  stripDev().open = false;
  const Song& s = a.doc.song();
  const Channel* c = a.settingsChannel == kNoId ? nullptr : s.channel(a.settingsChannel);
  if (!c) {
    a.settingsChannel = kNoId;
    stripState(a).open = false;  // closed: the strip is hidden when it opens again
    stripDevFrame();             // (dev) "window closed"
    return;
  }
  const PluginInfo* info = findPlugin(c->plugin.type);
  std::string title = c->name + "###channelsettings";
  bool open = true;
  stripOpened(a, c->id);  // opened (or another channel): the strip starts hidden
  // Wide instrument editors (PluginInfo::editorWidth) widen the window.
  const float winW = std::max(624.0f, (info && info->editorWidth > 0 ? info->editorWidth : 600.0f) + 24.0f);
  ui::NextFloatingWindowSize(winW);
  {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  }
  viewports::openAttached(title.c_str());  // opens inside the main window, never popped out by itself
  bool vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
  ui::KeepWindowInViewport();
  ui::MarkDetachable();  // J-114: pops out / docks back like the panels
  if (!vis) {
    ImGui::End();
    if (!open) a.settingsChannel = kNoId;
    return;
  }
  const ObjectId id = c->id;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowAppearing()) {
    a.focusPluginOwner = id;  // controller template knobs follow the focused plugin
    a.focusPluginSlot = -1;
  }

  // Header: colour, name, instrument, routing.
  const auto& P = pal();
  uint32_t col = c->color;
  const float fh = ImGui::GetFrameHeight();
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
    std::string cat;
    for (const auto& p : pluginCatalog()) {
      if (p.kind != PluginKind::Generator) continue;
      if (p.category != cat) {
        cat = p.category;
        ui::Label(cat.c_str());
      }
      if (ImGui::Selectable(p.name.c_str(), p.type == c->plugin.type)) a.doc.setChannelPlugin(id, p.type);
    }
    ImGui::EndCombo();
  }
  ui::Hint("Instrument");
  ImGui::SameLine(0, theme::kS2);
  int mi = std::max(0, s.mixerIndex(c->mixerTrack));
  std::string mlabel = mi == 0 ? "Master" : std::to_string(mi) + "  " + mixerTrackName(s, static_cast<size_t>(mi));
  if (ui::BeginCombo("##route", ("\xE2\x86\x92 " + mlabel).c_str(), std::max(120.0f, ImGui::GetContentRegionAvail().x - fh - theme::kS2))) {
    for (size_t i = 0; i < s.mixer.size(); ++i) {
      std::string l = (i == 0 ? std::string("Master") : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##" + std::to_string(i);
      if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == mi)) a.doc.routeChannel(id, s.mixer[i].id);
    }
    ImGui::EndCombo();
  }
  ui::Hint("Mixer track this channel plays through");
  // The channel strip's show / hide eye (the mixer's eye icon and button).
  ImGui::SameLine(0, theme::kS2);
  StripState& strip = stripState(a);
  const bool stripOn = strip.shown;
  if (ui::IconButton("##strip", ui::eyeIcon(stripOn), stripOn, fh,
                     stripOn ? "Hide the channel strip (volume, pan, pitch, keyboard)" : "Show the channel strip (volume, pan, pitch, keyboard)"))
    strip.shown = !stripOn;
  StripDev& sd = stripDev();
  sd.eyeMin = ImGui::GetItemRectMin();
  sd.eyeMax = ImGui::GetItemRectMax();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0, theme::kS1));

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
  if (tab == 2) {
    drawChannelEnvelope(a, id);
    ImGui::End();
    if (!open) a.settingsChannel = kNoId;
    return;
  }
  if (tab == 1) {
    drawNoteSettings(a, id);
    ImGui::End();
    if (!open) a.settingsChannel = kNoId;
    return;
  }

  // Sample (sampler only).
  if (info && info->type == "gl.sampler") {  // other sample instruments have their own sample UI
    SamplePtr smp = c->plugin.data.empty() ? nullptr : sampleCache().request(c->plugin.data);  // J-30: non-blocking
    float startFrac = c->plugin.params.size() > pp::SmpStart ? c->plugin.params[pp::SmpStart] : 0;
    drawWaveform(a, smp, ImVec2(600, 110), startFrac);
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
      a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3"};
      a.fileDialog.onOk = [&a, id](const std::string& p) {
        a.doc.editChannel(id, "load sample", [&](Channel& x) { x.plugin.data = p; });
        if (!sampleCache().get(p)) a.notify("Cannot decode " + p, true);
      };
    }
    ImGui::SameLine();
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
    menuButton("plmenu", "Instrument menu: presets, state, channel", [&a, id]() { pluginWindowMenu(a, id, -1); });
    ImGui::SameLine(0, 8);
    drawPluginHeader(a, target);
    drawPluginBody(a, target, info->editorWidth > 0 ? info->editorWidth : 600.0f);
  }
  packs::modulation::drawChannelNoteFx(a, id, 600.0f);  // note processors (arpeggiator, chords, ...)
  ImGui::End();
  if (!open) a.settingsChannel = kNoId;
}

}  // namespace gl
