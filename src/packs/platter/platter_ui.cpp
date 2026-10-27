// Gridline — "platter" pack: Platter editor frame. One deck of a DJ setup: drop
// a track on it (tempo and beat grid are found and the deck follows the
// project's tempo; the padlock unlocks it), then play, cue and scratch it with
// the mouse, the keyboard's keys, automation or a DJ controller. The first time
// a deck opens it asks whether a controller drives it (which one, which side);
// everything is remappable and the mapping saves with the project or as a preset
// file. This file holds the controller question, the top bar, the mapping
// drawer and the shared helpers; the deck itself is in platter_deck_ui.cpp.
#include "platter_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include "../../app/DevSignal.h"
#include "../../app/Midi.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"

namespace gl::packs::platter {

namespace {
// Control names, groups and profile notes live in the DSP tables (no I18n there): marked here
// so the extractor sees them; the UI translates them with i18n::tr.
[[maybe_unused]] const char* const kTableText[] = {
    i18n::N_("Play / pause"),
    i18n::N_("Transport"),
    i18n::N_("Cue"),
    i18n::N_("Sync"),
    i18n::N_("Key lock"),
    i18n::N_("Modes"),
    i18n::N_("Shift"),
    i18n::N_("Jog touch"),
    i18n::N_("Jog"),
    i18n::N_("Jog platter"),
    i18n::N_("Jog side (nudge)"),
    i18n::N_("Jog search"),
    i18n::N_("Pitch fader"),
    i18n::N_("Mixer"),
    i18n::N_("Channel fader"),
    i18n::N_("Crossfader"),
    i18n::N_("EQ high"),
    i18n::N_("EQ mid"),
    i18n::N_("EQ low"),
    i18n::N_("Filter"),
    i18n::N_("Trim"),
    i18n::N_("Hot cue 1"),
    i18n::N_("Pads"),
    i18n::N_("Hot cue 2"),
    i18n::N_("Hot cue 3"),
    i18n::N_("Hot cue 4"),
    i18n::N_("Hot cue 5"),
    i18n::N_("Hot cue 6"),
    i18n::N_("Hot cue 7"),
    i18n::N_("Hot cue 8"),
    i18n::N_("Clear hot cue 1"),
    i18n::N_("Clear hot cue 2"),
    i18n::N_("Clear hot cue 3"),
    i18n::N_("Clear hot cue 4"),
    i18n::N_("Clear hot cue 5"),
    i18n::N_("Clear hot cue 6"),
    i18n::N_("Clear hot cue 7"),
    i18n::N_("Clear hot cue 8"),
    i18n::N_("Loop in"),
    i18n::N_("Loop"),
    i18n::N_("Loop out"),
    i18n::N_("Auto loop"),
    i18n::N_("Loop / 2"),
    i18n::N_("Loop x 2"),
    i18n::N_("Beat jump back"),
    i18n::N_("Beat jump forward"),
    i18n::N_("Slip"),
    i18n::N_("Vinyl mode"),
    i18n::N_("Quantize"),
    i18n::N_("Reverse (held)"),
    i18n::N_("Back to start"),
    i18n::N_("Pitch bend -"),
    i18n::N_("Pitch bend +"),
    i18n::N_("Track loaded"),
    i18n::N_("Two decks, eight pads per deck, 128 jog ticks per turn."),
    i18n::N_("Two decks, four pads per deck, two-band EQ, 248 jog ticks per turn."),
    i18n::N_("Two decks, eight pads per deck, 1024 jog ticks per turn."),
    i18n::N_("Nothing is mapped: teach each control with MIDI learn."),
};
}  // namespace

bool& learningNow();  // platter_tick_ui.cpp

namespace pv {

namespace fs = std::filesystem;
using theme::pal;

Ed& ed(const PluginTarget& t) {
  static std::unordered_map<uint64_t, std::unique_ptr<Ed>> m;
  auto& p = m[t.key()];
  if (!p) p = std::make_unique<Ed>();
  return *p;
}

// ------------------------------------------------------------------ colours / text
ImU32 accent() { return theme::litRole(pui::brandRole(BrandFamily::Sample)); }

ImU32 hotColor(int k, float alpha) {
  // eight hues spaced for slate panels (pads, flags on the waveform)
  static const uint32_t kHues[kHotCues] = {0xF2716B, 0xF2A73B, 0xD9C450, 0x9FD45B, 0x3CC8B4, 0x5BAEF2, 0x9C82F5, 0xE673B8};
  return theme::content(kHues[static_cast<size_t>(std::clamp(k, 0, kHotCues - 1))], alpha);
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
  const bool neg = s < 0;
  s = std::fabs(s);
  char b[32];
  const int m = static_cast<int>(s / 60);
  std::snprintf(b, sizeof b, "%s%d:%04.1f", neg ? "-" : "", m, s - m * 60);
  return b;
}

ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface800, theme::kRadiusLg);
  dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
  if (title && *title) {
    dl->AddRectFilled(ImVec2(p0.x + 8, p0.y + 9), ImVec2(p0.x + 10, p0.y + 17), accent(), 1);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p0.x + 15, p0.y + 7), P.textDim, title);
  }
  if (caption && *caption) textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 9, p0.y + 7), P.textFaint, caption);
  return ImVec2(p0.x + 10, p0.y + (title && *title ? 26.0f : 10.0f));
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
    dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(x0 + isz + 5), std::round(c.y - ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, s).y * 0.5f)), fg, s);
  } else if (text) {
    textC(dl, ImGui::GetFont(), theme::kFontCaption, c, fg, i18n::tr(text));
  }
  return clicked;
}

bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur, const char* const* hints) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface900, h * 0.5f);
  dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), P.line, h * 0.5f);
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
    const ImVec2 q0(x + 2, p0.y + 2), q1(x + cw - 2, p0.y + h - 2);
    if (i == cur) dl->AddRectFilled(q0, q1, accent(), (h - 4) * 0.5f);
    else if (hv) dl->AddRectFilled(q0, q1, P.surface700, (h - 4) * 0.5f);
    const char* s = i18n::tr(labels[i]);
    const ImU32 col = i == cur ? theme::contrastText(accent()) : (hv ? P.text : P.textDim);
    const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, s);
    if (ts.x <= cw - 6) textC(dl, a.fontMono, theme::kFontLabel, ImVec2(x + cw * 0.5f, p0.y + h * 0.5f), col, s);
    else theme::textEllipsis(dl, a.fontMono, theme::kFontLabel, ImVec2(x + 4, p0.y + (h - ts.y) * 0.5f), cw - 8, col, s);
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

void padlock(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool open) {
  // body
  const ImVec2 b0(c.x - s * 0.42f, c.y - s * 0.05f), b1(c.x + s * 0.42f, c.y + s * 0.5f);
  dl->AddRectFilled(b0, b1, col, s * 0.1f);
  // shackle: an arc over the body; open = lifted and swung to the right
  const float r = s * 0.27f;
  const ImVec2 sc(c.x + (open ? s * 0.3f : 0.0f), c.y - s * (open ? 0.22f : 0.08f));
  dl->PathArcTo(sc, r, 3.14159265f, 2 * 3.14159265f, 12);
  dl->PathStroke(col, 0, s * 0.11f);
  dl->AddLine(ImVec2(sc.x + r, sc.y), ImVec2(sc.x + r, b0.y + (open ? -s * 0.02f : 0.0f)), col, s * 0.11f);
  if (!open) dl->AddLine(ImVec2(sc.x - r, sc.y), ImVec2(sc.x - r, b0.y), col, s * 0.11f);
  else dl->AddLine(ImVec2(sc.x - r, sc.y), ImVec2(sc.x - r, sc.y + s * 0.1f), col, s * 0.11f);
  // keyhole
  const ImU32 hole = theme::contrastText(col);
  dl->AddCircleFilled(ImVec2(c.x, c.y + s * 0.17f), s * 0.07f, hole);
  dl->AddRectFilled(ImVec2(c.x - s * 0.025f, c.y + s * 0.18f), ImVec2(c.x + s * 0.025f, c.y + s * 0.34f), hole);
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

Generator* deckGen(App& a, const PluginTarget& t) {
  if (!t.isChannel()) return nullptr;
  auto rt = a.builder.channelRuntime(t.owner);
  return rt && rt->gen ? rt->gen.get() : nullptr;
}

LiveView live(App& a, const PluginTarget& t) {
  LiveView v;
  if (Generator* g = deckGen(a, t)) liveView(g, v);
  return v;
}

void send(App& a, const PluginTarget& t, uint8_t kind, uint8_t control, double v, double w) {
  Generator* g = deckGen(a, t);
  if (!g) return;
  Cmd c;
  c.kind = kind;
  c.a = control;
  c.v = v;
  c.w = w;
  command(g, c);
}

void press(App& a, const PluginTarget& t, int control, bool down) { send(a, t, CmdButton, static_cast<uint8_t>(control), down ? 1 : 0); }

std::string mappingDir(App& a) { return (fs::path(a.configDir()) / "presets" / "Platter mappings").string(); }

void openLoadDialog(App& a, const PluginTarget& t) {
  Ed& e = ed(t);
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::tr("Load a track onto the deck");
  a.fileDialog.dir = e.path.empty() ? (std::getenv("HOME") ? std::getenv("HOME") : ".") : fs::path(e.path).parent_path().string();
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"};
  const PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& p) {
    if (const PluginState* sp = tt.state(a.doc.song())) {
      PluginState st = *sp;
      const size_t nl = st.data.find('\n');
      st.data = nl == std::string::npos ? p : p + st.data.substr(nl);  // keep the deck's set-up
      setPluginState(a, tt, st, i18n::tr("Load a track"));
      if (tt.isChannel()) a.doc.renameChannel(tt.owner, fs::path(p).stem().string());
    }
  };
}

namespace {

std::vector<std::string> midiInputs(App& a) { return a.midi ? a.midi->inputs : std::vector<std::string>{}; }

// ------------------------------------------------------------------ top bar
void drawTopBar(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  float x = o.x;
  // deck chip: the side it plays (A / B when on the crossfader)
  {
    const int side = static_cast<int>(std::lround(pui::paramValue(a, t, PXfSide)));
    const char* k = side == XfA ? i18n::tr("DECK A") : side == XfB ? i18n::tr("DECK B") : i18n::tr("DECK");
    const float tw = a.fontBold->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, k).x;
    const ImVec2 c0(x, o.y + 11), c1(x + tw + 22, o.y + 33);
    dl->AddRectFilled(c0, c1, theme::withAlpha(accent(), 0.16f), 11);
    dl->AddRect(c0, c1, theme::withAlpha(accent(), 0.55f), 11);
    textC(dl, a.fontBold, theme::kFontCaption, ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), theme::isDark() ? accent() : P.text, k);
    x = c1.x + 12;
  }
  const float rightW = 360;
  const float nameW = std::max(140.0f, o.x + W - rightW - x - 12);
  const std::string name = e.path.empty() ? i18n::tr("No track loaded") : fs::path(e.path).filename().string();
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody + 1, ImVec2(x, o.y + 5), nameW, e.path.empty() ? P.textDim : P.text, name.c_str());
  {
    std::string facts;
    char b[96];
    if (e.path.empty()) {
      facts = i18n::tr("Drop a track here, or load one");
    } else if (!e.hasModel || !m.analysed) {
      facts = i18n::tr("Finding the tempo and the beats...");
    } else {
      std::snprintf(b, sizeof b, "%.2f %s", bpmOf(m), i18n::tr("BPM"));
      facts += b;
      facts += "  \xC2\xB7  " + fmtTime(m.srcFrames / std::max(1.0, m.srcRate));
      std::snprintf(b, sizeof b, "  \xC2\xB7  %d/4  \xC2\xB7  %zu %s", std::max(1, m.grid.beatsPerBar), m.takes.size(),
                    m.takes.size() == 1 ? i18n::tr("take") : i18n::tr("takes"));
      facts += b;
      if (lv.capturing) facts += std::string("  \xC2\xB7  ") + i18n::tr("recording a take");
    }
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x, o.y + 25), nameW, P.textDim, facts.c_str());
  }
  // right: controller, mapping drawer, load
  float rx = o.x + W;
  rx -= 92;
  if (button("##pload", ImVec2(rx, o.y + 7), ImVec2(92, 30), i18n::N_("Load..."), false, i18n::N_("Load a track onto the deck"),
             static_cast<int>(ui::Icon::Open)))
    openLoadDialog(a, t);
  rx -= 100;
  if (button("##pmap", ImVec2(rx, o.y + 7), ImVec2(94, 30), i18n::N_("Mapping"), e.drawer,
             i18n::N_("Show every deck control and the MIDI it answers to: learn, clear, presets"), static_cast<int>(ui::Icon::Settings)))
    e.drawer = !e.drawer;
  // controller chip: what drives this deck (click: the controller question)
  const Profile* pf = findProfile(m.map.profile);
  std::string chip;
  if (pf && std::string(pf->id) != "generic") {
    chip = pf->name;
    if (pf->sides) chip += m.map.side ? std::string(" \xC2\xB7 ") + i18n::tr("right") : std::string(" \xC2\xB7 ") + i18n::tr("left");
  } else if (!m.map.bindings.empty()) {
    chip = i18n::tr("Own mapping");
  } else {
    chip = i18n::tr("No controller");
  }
  const float cw = std::min(170.0f, a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, chip.c_str()).x + 44);
  rx -= cw + 6;
  const ImVec2 c0(rx, o.y + 7), c1(rx + cw, o.y + 37);
  ImGui::SetCursorScreenPos(c0);
  const bool clicked = ImGui::InvisibleButton("##pctl", ImVec2(cw, 30));
  const bool hv = ImGui::IsItemHovered();
  if (hv) ui::Tooltip(i18n::tr("Choose the DJ controller that drives this deck"));
  skin::insetField(dl, c0, c1, hv ? P.surface700 : P.field, theme::kRadius);
  dl->AddRect(c0, c1, P.line, theme::kRadius);
  // activity lamp: lit briefly when the deck hears its controller
  static std::unordered_map<uint64_t, std::pair<uint32_t, double>> seen;
  auto& s = seen[t.key()];
  const double now = ImGui::GetTime();
  if (lv.rawSeq != s.first) s = {lv.rawSeq, now};
  const bool activity = lv.rawSeq != 0 && now - s.second < 0.15;
  const ImU32 lamp = activity ? P.led : theme::withAlpha(P.textFaint, 0.5f);
  dl->AddCircleFilled(ImVec2(c0.x + 13, (c0.y + c1.y) * 0.5f), 3.5f, lamp);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(c0.x + 24, c0.y + 9), cw - 30, P.text, chip.c_str());
  if (clicked) e.setup = true, e.setupInit = false;
}

}  // namespace

// ------------------------------------------------------------------ controller question
void drawSetup(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& prof = profiles();
  const auto inputs = midiInputs(a);
  if (!e.setupInit) {
    e.setupInit = true;
    e.setupProfile = 0;
    e.setupSide = e.m.map.side;
    e.setupDevice = e.m.map.device;
    for (size_t i = 0; i < prof.size(); ++i)
      if (e.m.map.profile == prof[i].id) e.setupProfile = static_cast<int>(i);
    if (e.m.map.profile.empty())  // first time: a connected controller picks its profile
      for (const auto& in : inputs)
        if (const std::string id = profileForDevice(in); !id.empty()) {
          for (size_t i = 0; i < prof.size(); ++i)
            if (id == prof[i].id) e.setupProfile = static_cast<int>(i);
          e.setupDevice = in;
          break;
        }
  }
  skin::insetField(dl, p0, p1, P.well, theme::kRadiusLg);
  dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
  const float cx = (p0.x + p1.x) * 0.5f;
  const float contentH = 64 + 26 + 30 + 104 + 18 + 44 + 36 + 32;  // picture .. answers + cancel
  float y = p0.y + std::max(34.0f, (p1.y - p0.y - contentH) * 0.42f);
  // picture: a small deck (platter + fader), drawn
  {
    const ImVec2 c(cx - 14, y + 24);
    dl->AddCircleFilled(c, 24, P.surface900, 48);
    for (int k = 0; k < 4; ++k) dl->AddCircle(c, 9.0f + k * 3.6f, theme::withAlpha(P.textFaint, 0.35f), 40, 1);
    dl->AddCircleFilled(c, 6, accent(), 24);
    dl->AddLine(c, ImVec2(c.x + 17, c.y - 17), P.paper, 1.6f);
    dl->AddRectFilled(ImVec2(cx + 22, y + 2), ImVec2(cx + 26, y + 46), P.surface900, 2);
    dl->AddRectFilled(ImVec2(cx + 17, y + 18), ImVec2(cx + 31, y + 25), P.surface600, 2);
    y += 64;
  }
  textC(dl, a.fontBold, 22, ImVec2(cx, y), P.text, i18n::tr("Use a DJ controller for this deck?"));
  y += 26;
  textC(dl, a.fontRegular, theme::kFontBody, ImVec2(cx, y), P.textDim,
        i18n::tr("Pick the controller and the deck side this channel plays. Every control can be remapped later."));
  y += 30;
  // profile cards
  const int n = static_cast<int>(prof.size());
  const float cardW = 236, cardH = 104, gap = 12;
  const float rowW = n * cardW + (n - 1) * gap;
  float x = cx - rowW * 0.5f;
  for (int i = 0; i < n; ++i) {
    const Profile& p = prof[static_cast<size_t>(i)];
    const ImVec2 q0(x, y), q1(x + cardW, y + cardH);
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##card", ImVec2(cardW, cardH))) e.setupProfile = i;
    const bool hv = ImGui::IsItemHovered(), sel = e.setupProfile == i;
    ImGui::PopID();
    skin::raisedFace(dl, q0, q1, sel ? theme::mix(P.surface700, accent(), 0.14f) : hv ? P.surface700 : P.surface800, theme::kRadiusLg);
    dl->AddRect(q0, q1, sel ? accent() : P.line, theme::kRadiusLg, 0, sel ? 2.0f : 1.0f);
    const bool generic = std::string(p.id) == "generic";
    ui::drawIcon(dl, generic ? ui::Icon::CatMidi : ui::Icon::CatController, ImVec2(q0.x + 20, q0.y + 21), 16,
                       sel ? accent() : P.textDim);
    theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(q0.x + 36, q0.y + 12), cardW - 46, P.text,
                        generic ? i18n::tr("Another controller") : p.name);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 14, q0.y + 40), P.textDim, i18n::tr(p.note), nullptr,
                cardW - 28);
    x += cardW + gap;
  }
  y += cardH + 18;
  const Profile& pick = prof[static_cast<size_t>(std::clamp(e.setupProfile, 0, n - 1))];
  // side + device
  const float rowX = cx - 300;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(rowX, y + 8), P.textDim, i18n::tr("DECK SIDE"));
  static const char* const kSides[2] = {i18n::N_("Left deck"), i18n::N_("Right deck")};
  if (pick.sides) {
    pills(a, "##side", ImVec2(rowX + 84, y), 200, 26, kSides, 2, e.setupSide);
  } else {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(rowX + 84, y + 6), P.textFaint, i18n::tr("Any side: you teach it each control"));
  }
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(cx + 20, y + 8), P.textDim, i18n::tr("MIDI INPUT"));
  ImGui::SetCursorScreenPos(ImVec2(cx + 100, y + 1));
  const std::string preview = e.setupDevice.empty() ? i18n::tr("Any device") : e.setupDevice;
  if (a.midi) {
    if (ui::BeginCombo("##pdev", preview.c_str(), 200)) {
      if (ImGui::Selectable(i18n::tr("Any device"), e.setupDevice.empty())) e.setupDevice.clear();
      for (const auto& in : inputs)
        if (ImGui::Selectable(in.c_str(), e.setupDevice == in)) e.setupDevice = in;
      ImGui::EndCombo();
    }
  } else {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(cx + 100, y + 6), P.textFaint, i18n::tr("The host's MIDI input"));
  }
  y += 44;
  // answers
  const float bw = 230, bh = 36;
  if (button("##pnone", ImVec2(cx - bw - 8, y), ImVec2(bw, bh), i18n::N_("No controller"), false,
             i18n::N_("Play the deck with the mouse, the keys and automation. Map controls yourself any time."))) {
    Model m = e.m;
    m.asked = true;
    m.map = Mapping{};
    commit(a, t, e, m, i18n::N_("Deck without a controller"));
    e.setup = false;
  }
  if (button("##puse", ImVec2(cx + 8, y), ImVec2(bw, bh), i18n::N_("Use this controller"), false,
             i18n::N_("Map the deck to the controller's jog wheel, buttons, faders and pads"), static_cast<int>(ui::Icon::Check), true)) {
    Model m = e.m;
    m.asked = true;
    m.map = profileMapping(pick.id, pick.sides ? e.setupSide : 0);
    m.map.device = e.setupDevice;
    commit(a, t, e, m, i18n::N_("Deck controller"));
    e.setup = false;
    if (std::string(pick.id) == "generic") e.drawer = true;  // learn the controls next
  }
  if (e.m.asked) {
    y += bh + 12;
    ImGui::SetCursorScreenPos(ImVec2(cx - 40, y));
    if (ImGui::InvisibleButton("##pcancel", ImVec2(80, 20))) e.setup = false;
    textC(dl, a.fontRegular, theme::kFontCaption, ImVec2(cx, y + 10), ImGui::IsItemHovered() ? P.text : P.textFaint, i18n::tr("Cancel"));
  }
}

// ------------------------------------------------------------------ mapping drawer
void drawDrawer(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  Model& m = e.m;
  const Profile* pf = findProfile(m.map.profile);
  std::string cap = pf ? pf->name : i18n::tr("Own mapping");
  if (!m.map.device.empty()) cap += "  \xC2\xB7  " + m.map.device;
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("CONTROLLER MAPPING"), cap.c_str());
  // learning: the next message from the controller binds the control
  if (e.learn >= 0) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) e.learn = -1;
    else if (lv.rawSeq != e.learnSeq) {
      e.learnSeq = lv.rawSeq;
      const uint8_t s = lv.raw[0], d1 = lv.raw[1], d2 = lv.raw[2];
      const int kind = s & 0xF0;
      const ControlInfo& ci = controlInfo(e.learn);
      const bool usable = (kind == 0x90 && d2 > 0) || kind == 0xB0;
      // buttons learn from a press; relative controls from a CC turn
      if (usable && !(ci.type == TypeRelative && kind != 0xB0)) {
        Binding b = learnBinding(e.learn, s, d1, d2, ci.type == TypeRelative);
        Model nm = m;
        auto& bs = nm.map.bindings;
        bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const Binding& x) { return x.control == e.learn; }), bs.end());
        bs.push_back(b);
        if (nm.map.profile.empty()) nm.map.profile = "generic";
        nm.asked = true;
        commit(a, t, e, nm, i18n::N_("Learn a control"));
        e.learn = -1;
      }
    }
  }
  learningNow() = learningNow() || e.learn >= 0;
  // left column: device, jog, LEDs, presets
  const float colW = 250;
  float y = c.y + 2;
  const float x = c.x;
  if (button("##pctl2", ImVec2(x, y), ImVec2(colW, 28), i18n::N_("Controller..."), false, i18n::N_("Pick a controller profile and the deck side"),
             static_cast<int>(ui::Icon::CatController)))
    e.setup = true, e.setupInit = false;
  y += 36;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x, y + 6), P.textDim, i18n::tr("JOG TICKS / TURN"));
  {
    char b[16];
    std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(m.map.ticksPerRev)));
    const ImVec2 f0(x + 130, y), f1(x + colW, y + 24);
    skin::insetField(dl, f0, f1, P.field, theme::kRadius);
    textC(dl, a.fontMono, theme::kFontNumber, ImVec2((f0.x + f1.x) * 0.5f, (f0.y + f1.y) * 0.5f), P.text, b);
    ImGui::SetCursorScreenPos(f0);
    ImGui::InvisibleButton("##ptpr", ImVec2(f1.x - f0.x, 24));
    if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("How many steps the jog wheel sends per turn (drag or scroll). A smaller number scratches faster."));
    double v = m.map.ticksPerRev;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0)) v -= ImGui::GetIO().MouseDelta.y * 2;
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) v += ImGui::GetIO().MouseWheel * 8;
    v = std::clamp(std::round(v), 16.0, 4096.0);
    if (v != m.map.ticksPerRev) {
      Model nm = m;
      nm.map.ticksPerRev = v;
      commit(a, t, e, nm, i18n::N_("Jog resolution"), 0x9A7700ull ^ t.key());
    }
  }
  y += 32;
  if (button("##pled", ImVec2(x, y), ImVec2(colW, 26), m.map.feedback ? i18n::N_("LED feedback: on") : i18n::N_("LED feedback: off"), m.map.feedback,
             i18n::N_("Light the controller's buttons and pads from the deck"))) {
    Model nm = m;
    nm.map.feedback = !nm.map.feedback;
    commit(a, t, e, nm, i18n::N_("LED feedback"));
  }
  y += 34;
  // presets
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x, y), P.textDim, i18n::tr("MAPPING PRESETS"));
  y += 16;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::SetNextItemWidth(colW - 66);
  ImGui::InputTextWithHint("##pname", i18n::tr("Preset name"), e.presetName, sizeof e.presetName);
  if (button("##psave", ImVec2(x + colW - 60, y), ImVec2(60, 24), i18n::N_("Save"), false, i18n::N_("Save this mapping as a preset file")) &&
      e.presetName[0]) {
    if (saveMappingPreset(mappingDir(a), e.presetName, m.map)) a.notify(i18n::tr("Mapping preset saved"));
    else a.notify(i18n::tr("The mapping preset could not be saved"), true);
  }
  y += 30;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  if (ui::BeginCombo("##pload", i18n::tr("Load a preset..."), colW)) {
    const auto files = mappingPresetFiles(mappingDir(a));
    if (files.empty()) ImGui::TextDisabled("%s", i18n::tr("No presets saved yet"));
    for (const auto& f : files)
      if (ImGui::Selectable(fs::path(f).stem().string().c_str())) {
        Mapping mp;
        if (loadMappingPreset(f, mp)) {
          Model nm = m;
          if (mp.device.empty()) mp.device = nm.map.device;  // keep the input this deck listens to
          nm.map = mp;
          nm.asked = true;
          commit(a, t, e, nm, i18n::N_("Load mapping preset"));
        }
      }
    ImGui::EndCombo();
  }
  y += 32;
  const float half = (colW - 6) * 0.5f;
  if (button("##preset", ImVec2(x, y), ImVec2(half, 24), i18n::N_("Reset"), false, i18n::N_("Back to the profile's own mapping")) && pf) {
    Model nm = m;
    const std::string dev = nm.map.device;
    nm.map = profileMapping(pf->id, nm.map.side);
    nm.map.device = dev;
    commit(a, t, e, nm, i18n::N_("Reset mapping"));
  }
  if (button("##pclear", ImVec2(x + half + 6, y), ImVec2(half, 24), i18n::N_("Clear all"), false, i18n::N_("Remove every binding (learn from scratch)"))) {
    Model nm = m;
    nm.map.bindings.clear();
    nm.map.leds.clear();
    commit(a, t, e, nm, i18n::N_("Clear mapping"));
  }
  // right: every control with its bindings
  const ImVec2 l0(x + colW + 16, c.y), l1(p1.x - 10, p1.y - 10);
  skin::insetField(dl, l0, l1, P.well, theme::kRadius);
  ImGui::SetCursorScreenPos(ImVec2(l0.x + 1, l0.y + 1));
  ImGui::BeginChild("##pmaplist", ImVec2(l1.x - l0.x - 2, l1.y - l0.y - 2), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const float rowH = 24, listW = l1.x - l0.x - 16;
  // the controls, one heading per group in a fixed order
  static const std::vector<int> order = [] {
    std::vector<int> v;
    for (const char* grp : {"Transport", "Jog", "Mixer", "Pads", "Loop", "Modes"})
      for (int c = 1; c < CtlCount; ++c)
        if (controlInfo(c).type != TypeOutput && std::strcmp(controlInfo(c).group, grp) == 0) v.push_back(c);
    return v;
  }();
  const char* group = "";
  int row = 0;
  for (const int k : order) {
    const ControlInfo& ci = controlInfo(k);
    ++row;
    if (std::strcmp(group, ci.group) != 0) {
      group = ci.group;
      const ImVec2 g = ImGui::GetCursorScreenPos();
      cl->AddText(a.fontMono, theme::kFontLabel, ImVec2(g.x + 8, g.y + 6), accent(), i18n::tr(ci.group));
      ImGui::Dummy(ImVec2(listW, 20));
    }
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const bool learning = e.learn == k;
    if (learning) cl->AddRectFilled(r0, ImVec2(r0.x + listW, r0.y + rowH), theme::withAlpha(accent(), 0.16f), theme::kRadius);
    else if (row % 2 == 0) cl->AddRectFilled(r0, ImVec2(r0.x + listW, r0.y + rowH), theme::withAlpha(P.surface800, 0.5f), theme::kRadius);
    cl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 8, r0.y + 5), P.text, i18n::tr(ci.name));
    std::string bt;
    for (const Binding& b : m.map.bindings)
      if (b.control == k) bt += (bt.empty() ? "" : ",  ") + bindingText(b);
    if (learning) bt = i18n::tr("Move or press the control on your controller... (Esc cancels)");
    theme::textEllipsis(cl, bt.empty() ? a.fontRegular : a.fontMono, theme::kFontLabel, ImVec2(r0.x + 170, r0.y + 6), listW - 170 - 110,
                        bt.empty() ? P.textFaint : (learning ? P.text : P.textDim), bt.empty() ? i18n::tr("not mapped") : bt.c_str());
    ImGui::PushID(k);
    if (button("##learn", ImVec2(r0.x + listW - 104, r0.y + 2), ImVec2(64, rowH - 4), learning ? i18n::N_("Cancel") : i18n::N_("Learn"), learning,
               i18n::N_("Bind this control to the next thing you touch on the controller"))) {
      if (learning) {
        e.learn = -1;
      } else {
        e.learn = k;
        e.learnSeq = lv.rawSeq;
      }
    }
    if (button("##clr", ImVec2(r0.x + listW - 34, r0.y + 2), ImVec2(28, rowH - 4), "", false, i18n::N_("Remove this control's bindings"),
               static_cast<int>(ui::Icon::Trash))) {
      Model nm = m;
      auto& bs = nm.map.bindings;
      bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const Binding& x) { return x.control == k; }), bs.end());
      commit(a, t, e, nm, i18n::N_("Clear a control"));
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH));
    ImGui::Dummy(ImVec2(listW, 0));
  }
  ImGui::EndChild();
}

namespace {

void trace(Ed& e, const char* state, const LiveView& lv) {
  static const bool on = std::getenv("GRIDLINE_PLATTER_TRACE") != nullptr;
  if (std::string(state) == "ready") devsig::reach("platter-ready");
  if (lv.playing) devsig::reach("platter-playing");
  if (!on) return;
  char b[256];
  std::snprintf(b, sizeof b, "platter: state %s bpm %.2f asked %d playing %d pos %.0f speed %.3f takes %zu", state, e.hasModel ? bpmOf(e.m) : 0.0,
                e.m.asked ? 1 : 0, lv.playing ? 1 : 0, lv.pos, lv.speed, e.m.takes.size());
  if (b != e.lastTrace) {
    std::printf("%s\n", b);
    std::fflush(stdout);
    e.lastTrace = b;
  }
}

}  // namespace

}  // namespace pv

// ================================================================== editor

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  using namespace pv;
  Ed& e = ed(t);
  static bool cacheSet = false;
  if (!cacheSet) {
    seltmann::setAnalysisCacheDir((std::filesystem::path(a.configDir()) / "cache" / "seltmann").string());
    cacheSet = true;
  }
  // dev hooks (screenshots / checks): a track to load, answer the question, open the drawer
  if (!e.demoDone) {
    e.demoDone = true;
    if (const char* d = std::getenv("GRIDLINE_PLATTER_DRAWER"); d && *d == '1') e.drawer = true;
    if (const char* demo = std::getenv("GRIDLINE_PLATTER_DEMO"); demo && *demo && pathOf(sp->data).empty()) {
      PluginState st = *sp;
      Model m;
      std::string p;
      parseData(sp->data, p, m);
      if (const char* ask = std::getenv("GRIDLINE_PLATTER_PROFILE"); ask && *ask) {
        m.asked = true;
        if (std::string(ask) != "none") {
          m.map = profileMapping(ask, 0);
        }
      }
      st.data = formatData(demo, m);
      setPluginState(a, t, st, "demo");
      sp = t.state(a.doc.song());
      if (!sp) return true;
    }
  }
  // parse the state when it changed (undo, drops, loads)
  if (!e.parsed || sp->data != e.data) {
    e.parsed = true;
    e.data = sp->data;
    Model m;
    e.hasModel = parseData(sp->data, e.path, m);
    e.m = e.hasModel ? std::move(m) : Model{};
    if (!e.m.asked) e.setup = true, e.setupInit = false;
  }
  // the analysis gives the grid (and the peaks the waveforms draw)
  if (!e.path.empty() && (!e.job || e.job->path != e.path)) e.job = seltmann::requestAnalysis(e.path);
  if (e.job && e.job->state == 1 && e.job->sample) {
    const bool sameFile = e.hasModel && e.m.srcFrames == static_cast<int64_t>(e.job->sample->frames());
    if (!e.hasModel || !sameFile || !e.m.analysed) {
      const Model fresh = defaultModel(*e.job->sample, e.job->result, true);
      Model nm;
      if (sameFile) {  // keep the cues and the takes, take the grid
        nm = e.m;
        nm.grid = fresh.grid;
        nm.analysed = true;
      } else {
        nm = carryOver(e.m, fresh);
      }
      commit(a, t, e, nm, i18n::N_("Analyse the track"));
    }
  }

  const float W = std::max(width, 1100.0f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const LiveView lv = live(a, t);
  drawTopBar(a, t, e, o, W, lv);
  float y = o.y + 44 + 8;
  if (e.setup) {
    const float H = deckHeight();
    drawSetup(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + H));
    y += H;
    ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
    ImGui::Dummy(ImVec2(W, 1));
    trace(e, "setup", lv);
    return true;
  }
  y += drawDeck(a, t, e, ImVec2(o.x, y), W, lv);
  if (e.drawer) {
    y += 10;
    const float drawerH = 300;
    drawDrawer(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + drawerH), lv);
    y += drawerH;
  } else {
    e.learn = -1;
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
  ImGui::Dummy(ImVec2(W, 1));
  trace(e, e.path.empty() ? "empty" : "ready", lv);
  return true;
}

}  // namespace gl::packs::platter
