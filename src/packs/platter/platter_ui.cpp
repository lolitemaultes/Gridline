// Gridline — "platter" pack: Platter editor frame. A DJ setup in one channel:
// two decks and a mixer between them (or one deck). Drop a track on a deck
// (tempo and beat grid are found and the deck follows the project's tempo, or
// the other deck; the padlock unlocks it), then play, cue, scratch and mix with
// the mouse, the keyboard's keys, automation or a DJ controller. The first time
// the setup opens it asks whether a controller drives it; everything is
// remappable per deck and for the mixer, and the mapping saves with the
// project or as a preset file. This file holds the controller question, the top
// bar, the mapping drawer, the waveform bands and the shared helpers; the decks
// are in platter_deck_ui.cpp, the mixer in platter_mixer_ui.cpp.
#include "platter_ui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../../app/BrowserUi.h"
#include "../../app/DevSignal.h"
#include "../../app/ExternalDragUi.h"
#include "../../app/Look.h"
#include "../../app/Midi.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/Program.h"
#include "../../third_party/imgui/imgui_internal.h"
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
    i18n::N_("Master level"),
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
ImU32 accent() { return pal().accent; }
ImU32 hot() { return pal().hot; }

ImU32 deckColor(int d, float alpha) {
  // deck identity (badges, platter labels, playheads on the overviews): cyan and violet,
  // clear of the accent, the hot role and the waveform's band colours
  return theme::content(d == 0 ? 0x3FC6D9u : 0xB98CF5u, alpha);
}

ImU32 hotColor(int k, float alpha) {
  // eight hues spaced for slate panels (pads, flags on the waveform)
  static const uint32_t kHues[kHotCues] = {0xF2716B, 0xF2A73B, 0xD9C450, 0x9FD45B, 0x3CC8B4, 0x5BAEF2, 0x9C82F5, 0xE673B8};
  return theme::content(kHues[static_cast<size_t>(std::clamp(k, 0, kHotCues - 1))], alpha);
}
ImU32 cueColor() { return theme::content(0xF2A73B); }

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

std::string keyName(int key, bool minor) {
  static const char* const n[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
  if (key < 0 || key > 11) return "";
  return std::string(n[key]) + (minor ? "m" : "");
}

void caps(ImDrawList* dl, App& a, ImVec2 p, ImU32 col, const char* s) { dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p.x), std::round(p.y)), col, s); }
void capsRow(ImDrawList* dl, App& a, float x, float y0, float h, ImU32 col, const char* s) {
  const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, s);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(x), std::round(y0 + h * 0.5f - ts.y * 0.5f)), col, s);
}
float captionY(float y0, float h) {
  const float th = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, "Ag").y;
  return std::round(y0 + h * 0.5f - th * 0.5f);
}

ImVec2 panel(ImDrawList* dl, App& a, ImVec2 p0, ImVec2 p1, const char* title, const char* caption) {
  const auto& P = pal();
  dl->AddRectFilled(p0, p1, P.surface800, theme::kRadiusLg);
  dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
  if (title && *title) caps(dl, a, ImVec2(p0.x + 12, p0.y + 8), P.textDim, title);
  if (caption && *caption) textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 12, p0.y + 8), P.textFaint, caption);
  return ImVec2(p0.x + 12, p0.y + (title && *title ? 28.0f : 12.0f));
}

bool button(const char* id, ImVec2 p0, ImVec2 size, const char* text, bool on, const char* hint, int icon, bool primary) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (hv && hint) ui::Tooltip(i18n::tr(hint));
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  // lit = the hot role on a slate face (glyph, hairline and a faint wash), never a big saturated fill
  ImU32 bg = primary ? theme::mix(accent(), P.paper, hv ? 0.08f : 0.0f) : (ac ? P.surface600 : hv ? P.surface700 : P.surface800);
  if (on && !primary) bg = theme::mix(bg, hot(), hv ? 0.09f : 0.05f);
  skin::raisedFace(dl, p0, p1, bg, theme::kRadius);
  dl->AddRect(p0, p1, on ? theme::withAlpha(hot(), 0.6f) : primary ? theme::mix(accent(), P.ink, 0.25f) : P.line, theme::kRadius);
  const ImU32 fg = primary ? theme::contrastText(bg) : on ? hot() : (hv ? P.text : P.textDim);
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  ImFont* f = ImGui::GetFont();
  if (icon >= 0 && (!text || !*text)) {
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), c, std::min(size.x, size.y) * 0.55f, fg);
  } else if (icon >= 0) {
    const float isz = std::min(14.0f, size.y * 0.55f);
    const char* s = i18n::tr(text);
    const ImVec2 ts = f->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, s);
    const float total = isz + 5 + ts.x;
    const float x0 = c.x - total * 0.5f;
    ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2(x0 + isz * 0.5f, c.y), isz, fg);
    dl->AddText(f, theme::kFontCaption, ImVec2(std::round(x0 + isz + 5), std::round(c.y - ts.y * 0.5f)), fg, s);
  } else if (text) {
    textC(dl, f, theme::kFontCaption, c, fg, i18n::tr(text));
  }
  return clicked;
}

bool pills(App& a, const char* id, ImVec2 p0, float w, float h, const char* const* labels, int count, int& cur, const char* const* hints) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float r = std::min(h * 0.5f, theme::kRadiusLg + 1.0f);
  skin::insetField(dl, p0, ImVec2(p0.x + w, p0.y + h), P.field, r);
  dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), P.line, r);
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
    if (i == cur) {
      skin::raisedFace(dl, q0, q1, P.surface600, r - 1);
      dl->AddRect(q0, q1, theme::withAlpha(P.text, 0.12f), r - 1);
    } else if (hv) {
      dl->AddRectFilled(q0, q1, theme::withAlpha(P.surface700, 0.8f), r - 1);
    }
    const char* s = i18n::tr(labels[i]);
    const ImU32 col = i == cur ? P.text : (hv ? P.text : P.textDim);
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

// ------------------------------------------------------------------ parameters
double param(App& a, const PluginTarget& t, int k) { return pui::paramValue(a, t, k); }
bool on(App& a, const PluginTarget& t, int k) { return param(a, t, k) >= 0.5; }
void flip(App& a, const PluginTarget& t, int k) { pui::setParamValue(a, t, k, on(a, t, k) ? 0.0 : 1.0, false); }

// ------------------------------------------------------------------ waveform bands
namespace {

void computeBands(const SamplePtr& s, const std::shared_ptr<Bands>& b) {
  const auto n = static_cast<int64_t>(s->frames());
  const double sr = s->sampleRate > 0 ? s->sampleRate : 44100.0;
  const size_t blocks = static_cast<size_t>((n + kBandBlock - 1) / kBandBlock);
  std::vector<float> lo(blocks), mid(blocks), hi(blocks);
  // two-pole low-pass at 220 Hz and high-pass at 2.6 kHz (TPT state variable), mid = what is left
  auto g = [&](double hz) { return std::tan(3.14159265358979 * std::min(hz, sr * 0.45) / sr); };
  const double gl = g(220), gh = g(2600), k = 1.41421356;
  double l1 = 0, l2 = 0, h1 = 0, h2 = 0;
  float peak = 1e-6f;
  for (int64_t i = 0; i < n; ++i) {
    const double x = 0.5 * (static_cast<double>(s->left[static_cast<size_t>(i)]) + static_cast<double>(s->right[static_cast<size_t>(i)]));
    // low
    double a1 = 1.0 / (1.0 + gl * (gl + k)), v1 = a1 * l1 + gl * a1 * (x - l2), v2 = l2 + gl * v1;
    l1 = 2 * v1 - l1, l2 = 2 * v2 - l2;
    const double low = v2;
    // high
    a1 = 1.0 / (1.0 + gh * (gh + k)), v1 = a1 * h1 + gh * a1 * (x - h2), v2 = h2 + gh * v1;
    h1 = 2 * v1 - h1, h2 = 2 * v2 - h2;
    const double high = x - k * v1 - v2;
    const double m = x - low - high;
    const auto bi = static_cast<size_t>(i / kBandBlock);
    lo[bi] = std::max(lo[bi], static_cast<float>(std::fabs(low)));
    mid[bi] = std::max(mid[bi], static_cast<float>(std::fabs(m)));
    hi[bi] = std::max(hi[bi], static_cast<float>(std::fabs(high)));
    peak = std::max(peak, static_cast<float>(std::fabs(x)));
  }
  auto q = [&](const std::vector<float>& v, std::vector<uint8_t>& out, float gain) {
    out.resize(v.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = static_cast<uint8_t>(std::clamp(std::lround(std::sqrt(std::min(1.0f, v[i] * gain / peak)) * 255.0), 0L, 255L));
  };
  // (square root: quiet parts stay visible; highs are small in level, lifted a little)
  q(lo, b->lo, 1.0f);
  q(mid, b->mid, 1.2f);
  q(hi, b->hi, 2.0f);
  b->frames = n;
  b->sampleRate = sr;
  b->ready.store(true, std::memory_order_release);
}

}  // namespace

std::shared_ptr<const Bands> bandsFor(const SamplePtr& s, const std::string& path) {
  static std::mutex mu;
  static std::map<std::string, std::shared_ptr<Bands>> cache;
  if (!s || s->frames() == 0) return nullptr;
  std::lock_guard<std::mutex> lk(mu);
  const std::string key = path + "|" + std::to_string(s->frames());
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  if (cache.size() > 16) cache.erase(cache.begin());
  auto b = std::make_shared<Bands>();
  cache[key] = b;
  std::thread([s, b] { computeBands(s, b); }).detach();
  return b;
}

// ------------------------------------------------------------------ model edits
void commit(App& a, const PluginTarget& t, Ed& e, const Model& m, const char* label, uint64_t mergeKey) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  PluginState st = *sp;
  st.data = formatData(m.trackOf(0), m);
  setPluginState(a, t, st, i18n::tr(label), mergeKey);
  e.m = m;
  e.data = st.data;
}

Generator* deckGen(App& a, const PluginTarget& t) {
  if (!t.isChannel()) return nullptr;
  auto rt = a.builder.channelRuntime(t.owner);
  return rt && rt->gen ? rt->gen.get() : nullptr;
}

LiveView live(App& a, const PluginTarget& t, int deck) {
  LiveView v;
  if (Generator* g = deckGen(a, t)) liveView(g, deck, v);
  return v;
}
MixView mixLive(App& a, const PluginTarget& t) {
  MixView v;
  if (Generator* g = deckGen(a, t)) mixView(g, v);
  return v;
}

void send(App& a, const PluginTarget& t, int deck, uint8_t kind, uint8_t control, double v, double w) {
  Generator* g = deckGen(a, t);
  if (!g) return;
  Cmd c;
  c.kind = kind;
  c.a = control;
  c.deck = static_cast<uint8_t>(deck & 1);
  c.v = v;
  c.w = w;
  command(g, c);
}

void press(App& a, const PluginTarget& t, int deck, int control, bool down) {
  send(a, t, deck, CmdButton, static_cast<uint8_t>(control), down ? 1 : 0);
}

std::string mappingDir(App& a) { return (fs::path(a.configDir()) / "presets" / "Platter mappings").string(); }

bool deckReady(const Ed& e, int d) {
  const DeckUi& u = e.d[static_cast<size_t>(d)];
  return !u.path.empty() && u.job && u.job->state == 1 && u.job->peaks.frames > 0;
}

void loadOnDeck(App& a, const PluginTarget& t, int deck, const std::string& path) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || path.empty()) return;
  PluginState st = *sp;
  if (deck == 0) {
    const size_t nl = st.data.find('\n');
    st.data = nl == std::string::npos ? path : path + st.data.substr(nl);  // keep the setup
    setPluginState(a, t, st, i18n::tr("Load a track"));
    if (t.isChannel()) a.doc.renameChannel(t.owner, fs::path(path).stem().string());
    return;
  }
  std::string first;
  Model m;
  parseData(st.data, first, m);
  loadTrack(m, deck, path, DeckModel{});  // grid and cues come with the analysis
  if (!m.twoDecks()) m.layout = LayoutTwo;
  st.data = formatData(m.trackOf(0), m);
  setPluginState(a, t, st, i18n::tr("Load a track on deck B"));
}

void openLoadDialog(App& a, const PluginTarget& t, int deck) {
  Ed& e = ed(t);
  const std::string& cur = e.d[static_cast<size_t>(deck & 1)].path;
  a.fileDialog = FileDialogState{};
  a.fileDialog.open = true;
  a.fileDialog.title = deck == 0 ? i18n::tr("Load a track onto deck A") : i18n::tr("Load a track onto deck B");
  a.fileDialog.dir = cur.empty() ? (std::getenv("HOME") ? std::getenv("HOME") : ".") : fs::path(cur).parent_path().string();
  a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".aif", ".aiff", ".wv"};
  const PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt, deck](const std::string& p) { loadOnDeck(a, tt, deck, p); };
}

namespace {

std::vector<std::string> midiInputs(App& a) { return a.midi ? a.midi->inputs : std::vector<std::string>{}; }

// ------------------------------------------------------------------ top bar
void drawTopBar(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, const LiveView* lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  const float h = 30;
  float x = o.x;
  // controller chip: what drives the setup (click: the controller question)
  {
    const Profile* pf = findProfile(m.map.profile);
    std::string chip;
    if (pf && std::string(pf->id) != "generic") {
      chip = pf->name;
      if (pf->sides && m.map.side != kSideBoth) chip += m.map.side ? std::string(" \xC2\xB7 ") + i18n::tr("right") : std::string(" \xC2\xB7 ") + i18n::tr("left");
    } else if (!m.map.bindings.empty()) {
      chip = i18n::tr("Own mapping");
    } else {
      chip = i18n::tr("No controller");
    }
    const float cw = std::min(230.0f, a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, chip.c_str()).x + 54);
    const ImVec2 c0(x, o.y), c1(x + cw, o.y + h);
    ImGui::SetCursorScreenPos(c0);
    const bool clicked = ImGui::InvisibleButton("##pctl", ImVec2(cw, h));
    const bool hv = ImGui::IsItemHovered();
    if (hv) ui::Tooltip(i18n::tr("Choose the DJ controller that drives the setup"));
    skin::insetField(dl, c0, c1, hv ? P.surface700 : P.field, theme::kRadius);
    dl->AddRect(c0, c1, P.line, theme::kRadius);
    // activity lamp: lit briefly when the setup hears its controller
    static std::unordered_map<uint64_t, std::pair<uint32_t, double>> seen;
    auto& s = seen[t.key()];
    const double now = ImGui::GetTime();
    if (lv[0].rawSeq != s.first) s = {lv[0].rawSeq, now};
    const bool activity = lv[0].rawSeq != 0 && now - s.second < 0.15;
    look::led(dl, ImVec2(c0.x + 14, (c0.y + c1.y) * 0.5f), 3.5f, activity, activity ? 1.0f : 0.0f);
    ui::drawIcon(dl, ui::Icon::CatController, ImVec2(c0.x + 30, (c0.y + c1.y) * 0.5f), 14, P.textDim);
    theme::textEllipsis(dl, ImGui::GetFont(), theme::kFontCaption, ImVec2(c0.x + 42, captionY(c0.y, c1.y - c0.y)), cw - 48, P.text, chip.c_str());
    if (clicked) e.setup = true, e.setupInit = false;
    x = c1.x + 6;
  }
  if (button("##pmap", ImVec2(x, o.y), ImVec2(96, h), i18n::N_("Mapping"), e.drawer,
             i18n::N_("Show every control and the MIDI it answers to, per deck and for the mixer: learn, clear, presets"),
             static_cast<int>(ui::Icon::Settings)))
    e.drawer = !e.drawer;
  x += 96 + 14;
  // right side: keys mode and layout
  float rx = o.x + W;
  {
    static const char* const kLayouts[2] = {i18n::N_("2 decks"), i18n::N_("1 deck")};
    static const char* const kLayoutHints[2] = {i18n::N_("Two decks and the mixer (the whole controller)"),
                                                i18n::N_("One deck and its channel strip (deck B is off)")};
    int cur = m.twoDecks() ? 0 : 1;
    rx -= 150;
    if (pills(a, "##playout", ImVec2(rx, o.y + 2), 150, h - 4, kLayouts, 2, cur, kLayoutHints)) {
      Model nm = m;
      nm.layout = cur == 0 ? LayoutTwo : LayoutOne;
      commit(a, t, e, nm, i18n::N_("Deck layout"));
    }
    rx -= 12;
    static const char* const kModes[2] = {i18n::N_("Gate"), i18n::N_("Trigger")};
    static const char* const kHints[2] = {i18n::N_("A hot cue key plays while held"), i18n::N_("A hot cue key starts the deck; it keeps playing")};
    rx -= 140;
    paramPills(a, t, PNoteMode, ImVec2(rx, o.y + 2), 140, h - 4, kModes, 2, kHints);
    rx -= 40;
    capsRow(dl, a, rx, o.y + 2, h - 4, P.textFaint, i18n::tr("KEYS"));
    rx -= 14;
  }
  // recording: arm, record as automation or take, then a chip per take (hold to hear it)
  const bool capturing = lv[0].capturing || lv[1].capturing;
  {
    const bool armed = m.armed;
    const ImVec2 q0(x, o.y), q1(x + 132, o.y + h);
    ImGui::SetCursorScreenPos(q0);
    traceRect("arm", q0, q1);
    if (ImGui::InvisibleButton("##parm", ImVec2(q1.x - q0.x, h))) {
      Model nm = m;
      nm.armed = !nm.armed;
      commit(a, t, e, nm, nm.armed ? i18n::N_("Arm deck recording") : i18n::N_("Disarm deck recording"));
    }
    const bool hv = ImGui::IsItemHovered();
    if (hv)
      ui::Tooltip(i18n::tr("While Gridline records, an armed setup records what you play: both decks, scratches, cuts, cues and the mixer. "
                           "A pattern that already holds a pass of this setup is kept: the new pass goes into a new pattern."));
    skin::raisedFace(dl, q0, q1, hv ? P.surface700 : P.surface800, theme::kRadius);
    dl->AddRect(q0, q1, P.line, theme::kRadius);
    const bool blink = capturing && std::fmod(ImGui::GetTime(), 1.0) < 0.5;
    const ImU32 lamp = armed ? (blink ? theme::withAlpha(P.danger, 0.35f) : P.danger) : theme::withAlpha(P.textFaint, 0.6f);
    dl->AddCircleFilled(ImVec2(q0.x + 14, o.y + h * 0.5f), 5, lamp, 16);
    dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(q0.x + 26, captionY(o.y, h)), armed ? P.text : P.textDim,
                capturing ? i18n::tr("Recording") : armed ? i18n::tr("Record armed") : i18n::tr("Record off"));
    x = q1.x + 6;
  }
  {
    // J-159: what a pass becomes
    static const char* const kAs[2] = {i18n::N_("Automation"), i18n::N_("Take")};
    static const char* const kAsHints[2] = {
        i18n::N_("A pass becomes pattern data you can see and edit: a start note, a note per button press and a lane per control "
                 "that moved (scratches included). Played back unedited it is exactly the pass."),
        i18n::N_("A pass becomes one take: a single note in the pattern that replays it as played, with no controller.")};
    int cur = m.recordAs == RecTake ? 1 : 0;
    traceRect("recas", ImVec2(x, o.y + 2), ImVec2(x + 168, o.y + h - 2));
    if (pills(a, "##precas", ImVec2(x, o.y + 2), 168, h - 4, kAs, 2, cur, kAsHints)) {
      Model nm = m;
      nm.recordAs = cur == 1 ? RecTake : RecAutomation;
      commit(a, t, e, nm, cur == 1 ? i18n::N_("Record deck passes as takes") : i18n::N_("Record deck passes as automation"));
    }
    x += 168 + 8;
  }
  if (m.recordAs != RecTake) {
    // J-179: what a song-mode pass becomes on the playlist
    static const char* const kSong[2] = {i18n::N_("Pattern"), i18n::N_("Clips")};
    static const char* const kSongHints[2] = {
        i18n::N_("Song mode: a pass becomes one new pattern holding its automation clips, placed on the playlist where you played it. "
                 "The curves show on the pattern's clip; double-click one to edit it."),
        i18n::N_("Song mode: a pass becomes one automation clip per control on the playlist, grouped on tracks next to each other.")};
    const float lw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, i18n::tr("SONG")).x + 8;
    capsRow(dl, a, x, o.y + 2, h - 4, P.textFaint, i18n::tr("SONG"));
    x += lw;
    int cur = m.songClips ? 1 : 0;
    traceRect("songas", ImVec2(x, o.y + 2), ImVec2(x + 140, o.y + h - 2));
    if (pills(a, "##psongas", ImVec2(x, o.y + 2), 140, h - 4, kSong, 2, cur, kSongHints)) {
      Model nm = m;
      nm.songClips = cur == 1;
      commit(a, t, e, nm, cur == 1 ? i18n::N_("Record song passes as playlist clips") : i18n::N_("Record song passes as a pattern"));
    }
    x += 140 + 8;
  }
  if (m.takes.empty()) {
    theme::textEllipsis(dl, ImGui::GetFont(), theme::kFontCaption, ImVec2(x + 4, captionY(o.y, h)), std::max(0.0f, rx - x - 8), P.textFaint,
                        m.recordAs == RecTake ? i18n::tr("No takes yet: record in Gridline and play")
                                              : i18n::tr("Passes become automation clips you can edit"));
    return;
  }
  static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  for (size_t i = 0; i < m.takes.size(); ++i) {
    const Take& tk = m.takes[i];
    char b[64];
    std::snprintf(b, sizeof b, "%s%d  %.1fs", kNames[tk.key % 12], tk.key / 12 - 1, tk.seconds());
    const float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, b).x + 22;
    if (x + tw > rx - 4) {
      char more[16];
      std::snprintf(more, sizeof more, "+%zu", m.takes.size() - i);
      caps(dl, a, ImVec2(x + 2, o.y + 9), P.textDim, more);
      break;
    }
    const ImVec2 q0(x, o.y + 3), q1(x + tw, o.y + h - 3);
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##ptake", ImVec2(tw, q1.y - q0.y));
    const bool hv = ImGui::IsItemHovered(), playing = lv[0].take == tk.key || lv[1].take == tk.key;
    if (ImGui::IsItemActivated() && t.isChannel()) a.engine.noteOn(t.owner, tk.key, 0.9f);
    if (ImGui::IsItemDeactivated() && t.isChannel()) a.engine.noteOff(t.owner, tk.key);
    if (hv) {
      std::string tip = tk.name.empty() ? i18n::tr("Take") : tk.name;
      tip += "\n";
      tip += i18n::tr("Hold to hear the take. Right-click to delete it.");
      ui::Tooltip(tip.c_str());
    }
    if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##ptakemenu");
    bool gone = false;
    if (ImGui::BeginPopup("##ptakemenu")) {
      if (ImGui::MenuItem(i18n::tr("Delete take"))) {
        Model nm = m;
        nm.takes.erase(nm.takes.begin() + static_cast<std::ptrdiff_t>(i));
        pruneFiles(nm);
        commit(a, t, e, nm, i18n::N_("Delete take"));
        gone = true;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    if (gone) return;
    const float rr = (q1.y - q0.y) * 0.5f;
    dl->AddRectFilled(q0, q1, playing ? theme::mix(P.surface700, hot(), 0.18f) : hv ? P.surface700 : P.surface800, rr);
    dl->AddRect(q0, q1, playing ? hot() : P.line, rr);
    dl->AddCircleFilled(ImVec2(q0.x + 9, (q0.y + q1.y) * 0.5f), 2.5f, playing ? hot() : P.textFaint, 8);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q0.x + 15, q0.y + 5), playing ? P.text : P.textDim, b);
    x += tw + 5;
  }
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
    e.setupLayout = e.m.layout;
    e.setupSide = e.m.map.side == 1 ? 1 : 0;
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
  const float cx = std::round((p0.x + p1.x) * 0.5f);
  const int n = static_cast<int>(prof.size());
  const float gap = 12;
  const float cardW = std::min(236.0f, std::floor((p1.x - p0.x - 48 - (n - 1) * gap) / n)), cardH = 104;
  const float contentH = 70 + 30 + 34 + cardH + 24 + 40 + 52 + 32;
  float y = std::round(p0.y + std::max(28.0f, (p1.y - p0.y - contentH) * 0.45f));
  // picture: two small decks and a mixer, drawn
  {
    for (int d = 0; d < 2; ++d) {
      const ImVec2 c(cx + (d == 0 ? -46.0f : 46.0f), y + 28);
      skin::gradientCircle(dl, c, 26, theme::mix(P.surface600, P.paper, 0.1f), P.surface900);
      dl->AddCircleFilled(c, 21, theme::rgb(0x111216), 40);
      for (int k = 0; k < 3; ++k) dl->AddCircle(c, 10.0f + k * 3.5f, theme::withAlpha(P.textFaint, 0.3f), 40, 1);
      dl->AddCircleFilled(c, 7, deckColor(d), 24);
    }
    dl->AddRectFilled(ImVec2(cx - 13, y + 4), ImVec2(cx + 13, y + 52), P.surface900, 3);
    dl->AddRect(ImVec2(cx - 13, y + 4), ImVec2(cx + 13, y + 52), P.line, 3);
    for (int k = 0; k < 2; ++k) dl->AddRectFilled(ImVec2(cx - 8 + k * 11, y + 22), ImVec2(cx - 3 + k * 11, y + 30), P.surface600, 1);
    dl->AddRectFilled(ImVec2(cx - 8, y + 42), ImVec2(cx + 8, y + 46), P.textDim, 1);
    y += 70;
  }
  textC(dl, a.fontBold, 22, ImVec2(cx, y), P.text, i18n::tr("Use a DJ controller?"));
  y += 30;
  textC(dl, a.fontRegular, theme::kFontBody, ImVec2(cx, y), P.textDim,
        i18n::tr("Pick your controller: both of its decks and its mixer are mapped. Every control can be remapped later."));
  y += 34;
  // profile cards
  const float rowW = n * cardW + (n - 1) * gap;
  float x = std::round(cx - rowW * 0.5f);
  for (int i = 0; i < n; ++i) {
    const Profile& p = prof[static_cast<size_t>(i)];
    const ImVec2 q0(x, y), q1(x + cardW, y + cardH);
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##card", ImVec2(cardW, cardH))) e.setupProfile = i;
    const bool hv = ImGui::IsItemHovered(), sel = e.setupProfile == i;
    ImGui::PopID();
    skin::raisedFace(dl, q0, q1, sel ? theme::mix(P.surface700, accent(), 0.10f) : hv ? P.surface700 : P.surface800, theme::kRadiusLg);
    dl->AddRect(q0, q1, sel ? accent() : P.line, theme::kRadiusLg, 0, sel ? 2.0f : 1.0f);
    const bool generic = std::string(p.id) == "generic";
    ui::drawIcon(dl, generic ? ui::Icon::CatMidi : ui::Icon::CatController, ImVec2(q0.x + 22, q0.y + 22), 16, sel ? accent() : P.textDim);
    theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(q0.x + 38, q0.y + 13), cardW - 48, P.text,
                        generic ? i18n::tr("Another controller") : p.name);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 14, q0.y + 42), P.textDim, i18n::tr(p.note), nullptr, cardW - 28);
    x += cardW + gap;
  }
  y += cardH + 24;
  const Profile& pick = prof[static_cast<size_t>(std::clamp(e.setupProfile, 0, n - 1))];
  // layout (one deck: which side plays it) + MIDI input
  const float rowX = std::round(cx - 330);
  capsRow(dl, a, rowX, y, 28, P.textDim, i18n::tr("LAYOUT"));
  static const char* const kLayouts[2] = {i18n::N_("Two decks"), i18n::N_("One deck")};
  int lay = e.setupLayout == LayoutOne ? 1 : 0;
  if (pills(a, "##slay", ImVec2(rowX + 62, y), 190, 28, kLayouts, 2, lay)) e.setupLayout = lay ? LayoutOne : LayoutTwo;
  if (e.setupLayout == LayoutOne && pick.sides) {
    static const char* const kSides[2] = {i18n::N_("Left"), i18n::N_("Right")};
    pills(a, "##sside", ImVec2(rowX + 262, y), 110, 28, kSides, 2, e.setupSide);
  }
  capsRow(dl, a, cx + 70, y, 28, P.textDim, i18n::tr("MIDI INPUT"));
  ImGui::SetCursorScreenPos(ImVec2(cx + 150, y + 2));
  const std::string preview = e.setupDevice.empty() ? i18n::tr("Any device") : e.setupDevice;
  if (a.midi) {
    if (ui::BeginCombo("##pdev", preview.c_str(), 180)) {
      if (ImGui::Selectable(i18n::tr("Any device"), e.setupDevice.empty())) e.setupDevice.clear();
      for (const auto& in : inputs)
        if (ImGui::Selectable(in.c_str(), e.setupDevice == in)) e.setupDevice = in;
      ImGui::EndCombo();
    }
  } else {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(cx + 150, y + 7), P.textFaint, i18n::tr("The host's MIDI input"));
  }
  y += 52;
  // answers
  const float bw = 230, bh = 36;
  traceRect("none", ImVec2(cx - bw - 8, y), ImVec2(cx - 8, y + bh));
  traceRect("use", ImVec2(cx + 8, y), ImVec2(cx + 8 + bw, y + bh));
  if (button("##pnone", ImVec2(cx - bw - 8, y), ImVec2(bw, bh), i18n::N_("No controller"), false,
             i18n::N_("Play the decks with the mouse, the keys and automation. Map controls yourself any time."))) {
    Model m = e.m;
    m.asked = true;
    m.map = Mapping{};
    m.layout = e.setupLayout;
    commit(a, t, e, m, i18n::N_("Setup without a controller"));
    e.setup = false;
  }
  if (button("##puse", ImVec2(cx + 8, y), ImVec2(bw, bh), i18n::N_("Use this controller"), false,
             i18n::N_("Map the decks and the mixer to the controller's jog wheels, buttons, faders and pads"), static_cast<int>(ui::Icon::Check), true)) {
    Model m = e.m;
    m.asked = true;
    m.layout = e.setupLayout;
    m.map = profileMapping(pick.id, e.setupLayout == LayoutOne && pick.sides ? e.setupSide : kSideBoth);
    m.map.device = e.setupDevice;
    commit(a, t, e, m, i18n::N_("Setup controller"));
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
        const Binding b = learnBinding(e.learn, s, d1, d2, ci.type == TypeRelative, e.learnDeck);
        Model nm = m;
        auto& bs = nm.map.bindings;
        bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const Binding& x) { return x.control == e.learn && x.deck == b.deck; }), bs.end());
        bs.push_back(b);
        if (nm.map.profile.empty()) nm.map.profile = "generic";
        nm.asked = true;
        commit(a, t, e, nm, i18n::N_("Learn a control"));
        e.learn = -1;
      }
    }
  }
  learningNow() = learningNow() || e.learn >= 0;
  // left column: controller, jog, LEDs, presets
  const float colW = 240;
  float y = c.y + 2;
  const float x = c.x;
  if (button("##pctl2", ImVec2(x, y), ImVec2(colW, 28), i18n::N_("Controller..."), false, i18n::N_("Pick a controller profile and the layout"),
             static_cast<int>(ui::Icon::CatController)))
    e.setup = true, e.setupInit = false;
  y += 36;
  caps(dl, a, ImVec2(x, y + 6), P.textDim, i18n::tr("JOG TICKS / TURN"));
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
             i18n::N_("Light the controller's buttons and pads from the decks"))) {
    Model nm = m;
    nm.map.feedback = !nm.map.feedback;
    commit(a, t, e, nm, i18n::N_("LED feedback"));
  }
  y += 34;
  // presets
  caps(dl, a, ImVec2(x, y), P.textDim, i18n::tr("MAPPING PRESETS"));
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
          if (mp.device.empty()) mp.device = nm.map.device;  // keep the input the setup listens to
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
  // right: the target tabs, then every control of that target with its bindings
  const float lx = x + colW + 16;
  static const char* const kTabs[3] = {i18n::N_("Deck A"), i18n::N_("Deck B"), i18n::N_("Mixer")};
  pills(a, "##ptabs", ImVec2(lx, c.y), 300, 26, kTabs, 3, e.drawerTab);
  const int tab = e.drawerTab;
  const ImVec2 l0(lx, c.y + 34), l1(p1.x - 12, p1.y - 12);
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
    if (ci.mixer != (tab == 2)) continue;
    const int deck = tab == 1 ? 1 : 0;
    ++row;
    if (std::strcmp(group, ci.group) != 0) {
      group = ci.group;
      const ImVec2 g = ImGui::GetCursorScreenPos();
      caps(cl, a, ImVec2(g.x + 8, g.y + 6), P.textDim, i18n::tr(ci.group));
      ImGui::Dummy(ImVec2(listW, 20));
    }
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const bool learning = e.learn == k && e.learnDeck == deck;
    if (learning) cl->AddRectFilled(r0, ImVec2(r0.x + listW, r0.y + rowH), theme::withAlpha(hot(), 0.14f), theme::kRadius);
    else if (row % 2 == 0) cl->AddRectFilled(r0, ImVec2(r0.x + listW, r0.y + rowH), theme::withAlpha(P.surface800, 0.5f), theme::kRadius);
    cl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(r0.x + 8, r0.y + 5), P.text, i18n::tr(ci.name));
    std::string bt;
    for (const Binding& b : m.map.bindings)
      if (b.control == k && (ci.mixer || b.deck == deck)) bt += (bt.empty() ? "" : ",  ") + bindingText(b);
    if (learning) bt = i18n::tr("Move or press the control on your controller... (Esc cancels)");
    theme::textEllipsis(cl, bt.empty() ? a.fontRegular : a.fontMono, theme::kFontLabel, ImVec2(r0.x + 170, r0.y + 6), listW - 170 - 110,
                        bt.empty() ? P.textFaint : (learning ? P.text : P.textDim), bt.empty() ? i18n::tr("not mapped") : bt.c_str());
    ImGui::PushID(k + 1000 * deck);
    if (button("##learn", ImVec2(r0.x + listW - 104, r0.y + 2), ImVec2(64, rowH - 4), learning ? i18n::N_("Cancel") : i18n::N_("Learn"), learning,
               i18n::N_("Bind this control to the next thing you touch on the controller"))) {
      if (learning) {
        e.learn = -1;
      } else {
        e.learn = k;
        e.learnDeck = deck;
        e.learnSeq = lv.rawSeq;
      }
    }
    if (button("##clr", ImVec2(r0.x + listW - 34, r0.y + 2), ImVec2(28, rowH - 4), "", false, i18n::N_("Remove this control's bindings"),
               static_cast<int>(ui::Icon::Trash))) {
      Model nm = m;
      auto& bs = nm.map.bindings;
      bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const Binding& b) { return b.control == k && (ci.mixer || b.deck == deck); }), bs.end());
      commit(a, t, e, nm, i18n::N_("Clear a control"));
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH));
    ImGui::Dummy(ImVec2(listW, 0));
  }
  ImGui::EndChild();
}

void traceRect(const char* name, ImVec2 p0, ImVec2 p1) {
  static const bool on = std::getenv("GRIDLINE_PLATTER_TRACE") != nullptr;
  if (!on) return;
  static std::map<std::string, std::string> last;
  char b[160];
  std::snprintf(b, sizeof b, "platter: rect %s %d %d %d %d", name, static_cast<int>(p0.x), static_cast<int>(p0.y), static_cast<int>(p1.x), static_cast<int>(p1.y));
  std::string& l = last[name];
  if (l == b) return;
  l = b;
  std::printf("%s\n", b);
  std::fflush(stdout);
}

namespace {

// Dev hook (GRIDLINE_PLATTER_DEVJOG=<file>, J-155 checks): once a take starts
// capturing, a thread sends the file's controller MIDI ("<ms> <status> <d1> <d2>"
// per line, bytes in hex) through the hub at those times, as a driver thread would.
void devJog(const Ed& e, const LiveView* lv) {
  static bool started = false;
  const char* f = std::getenv("GRIDLINE_PLATTER_DEVJOG");
  if (started || !f || !*f || !(lv[0].capturing || lv[1].capturing)) return;
  started = true;
  struct Msg {
    double ms = 0;
    unsigned s = 0, a = 0, b = 0;
  };
  std::vector<Msg> msgs;
  if (FILE* fp = std::fopen(f, "r")) {
    Msg m;
    while (std::fscanf(fp, "%lf %x %x %x", &m.ms, &m.s, &m.a, &m.b) == 4) msgs.push_back(m);
    std::fclose(fp);
  }
  const uint32_t dev = hub::deviceId(e.m.map.device);
  std::thread([msgs, dev] {
    const auto t0 = std::chrono::steady_clock::now();
    for (const Msg& m : msgs) {
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(m.ms * 1000)));
      hub::push(dev, static_cast<uint8_t>(m.s), static_cast<uint8_t>(m.a), static_cast<uint8_t>(m.b));
    }
    std::printf("platter: devjog sent %zu\n", msgs.size());
    std::fflush(stdout);
  }).detach();
}

// Dev hook (GRIDLINE_PLATTER_TRACE, J-160): the file on each deck ("platter: tracks A=<file> B=<file>").
void traceTracks(const Ed& e) {
  static const bool on = std::getenv("GRIDLINE_PLATTER_TRACE") != nullptr;
  if (!on) return;
  static std::string last;
  const std::string b = "platter: tracks A=" + fs::path(e.m.trackOf(0)).filename().string() + " B=" + fs::path(e.m.trackOf(1)).filename().string();
  if (b == last) return;
  last = b;
  std::printf("%s\n", b.c_str());
  std::fflush(stdout);
}

void trace(App& a, const PluginTarget& t, Ed& e, const char* state, const LiveView* lv) {
  static const bool on = std::getenv("GRIDLINE_PLATTER_TRACE") != nullptr;
  devJog(e, lv);
  if (std::string(state) == "ready") devsig::reach("platter-ready");
  if (lv[0].playing || lv[1].playing) devsig::reach("platter-playing");
  if (lv[0].capturing || lv[1].capturing) devsig::reach("platter-capturing");
  if (!on) return;
  {  // J-197: the jog platter ticks each deck took (the dev jog's are all there)
    static uint64_t lastW[2] = {~uint64_t{0}, ~uint64_t{0}};
    if (lv[0].wheelTicks != lastW[0] || lv[1].wheelTicks != lastW[1]) {
      lastW[0] = lv[0].wheelTicks, lastW[1] = lv[1].wheelTicks;
      std::printf("platter: wheel ticks A %llu B %llu\n", static_cast<unsigned long long>(lastW[0]), static_cast<unsigned long long>(lastW[1]));
      std::fflush(stdout);
    }
  }
  {  // J-204: hot cue presses and releases the pads sent, the presses the deck acted on, where it is
    static std::string lastHot;
    char hb[200];
    int setA = 0;
    for (double h : lv[0].hot) setA += h >= 0 ? 1 : 0;
    std::snprintf(hb, sizeof hb, "platter: hot A down %u up %u acted %u set %d playing %d pos %.0f h1 %.0f h2 %.0f", e.d[0].padDowns, e.d[0].padUps,
                  lv[0].hotTaps, setA, lv[0].playing ? 1 : 0, lv[0].playing ? -1.0 : lv[0].pos, lv[0].hot[0], lv[0].hot[1]);
    devsig::reach("platter-hot-" + std::to_string(lv[0].hotTaps) + (lv[0].playing ? "-playing" : "-stopped"));
    if (hb != lastHot) {
      lastHot = hb;
      std::printf("%s\n", hb);
      std::fflush(stdout);
    }
  }
  char b[320];
  std::snprintf(b, sizeof b,
                "platter: state %s layout %d asked %d mapped %zu A[bpm %.2f playing %d speed %.3f] B[bpm %.2f playing %d] xf %.2f takes %zu armed %d as %d capturing %d", state,
                e.m.layout, e.m.asked ? 1 : 0, e.m.map.bindings.size(), bpmOf(e.m.deck[0]), lv[0].playing ? 1 : 0, lv[0].speed,
                bpmOf(e.m.deck[1]), lv[1].playing ? 1 : 0, param(a, t, PXfader), e.m.takes.size(), e.m.armed ? 1 : 0, e.m.recordAs,
                lv[0].capturing ? 1 : 0);
  if (b != e.lastTrace) {
    std::printf("%s\n", b);
    std::fflush(stdout);
    e.lastTrace = b;
  }
}

// Dev hooks (screenshots / checks): tracks to load, answer the question, open the drawer, the layout.
void devHooks(App& a, const PluginTarget& t, Ed& e, const PluginState*& sp) {
  if (e.demoDone) return;
  e.demoDone = true;
  if (const char* d = std::getenv("GRIDLINE_PLATTER_DRAWER"); d && *d) {
    e.drawer = true;
    e.drawerTab = std::clamp(std::atoi(d) - 1, 0, 2);
  }
  if (const char* h = std::getenv("GRIDLINE_PLATTER_HOTS"); h && *h) e.demoHots = 3;
  if (const char* p = std::getenv("GRIDLINE_PLATTER_PLAY"); p && *p)
    e.demoPlay = (std::strchr(p, 'A') ? 1 : 0) | (std::strchr(p, 'B') ? 2 : 0);
  const char* demo = std::getenv("GRIDLINE_PLATTER_DEMO");
  const char* demoB = std::getenv("GRIDLINE_PLATTER_DEMO_B");
  if (!((demo && *demo) || (demoB && *demoB)) || !pathOf(sp->data).empty()) return;
  PluginState st = *sp;
  Model m;
  std::string p;
  parseData(sp->data, p, m);
  if (const char* ask = std::getenv("GRIDLINE_PLATTER_PROFILE"); ask && *ask) {
    m.asked = true;
    if (std::string(ask) != "none") m.map = profileMapping(ask, kSideBoth);
  }
  if (const char* l = std::getenv("GRIDLINE_PLATTER_LAYOUT"); l && *l == '1') m.layout = LayoutOne;
  if (const char* n = std::getenv("GRIDLINE_PLATTER_BARS"); n && *n)  // J-159 checks: the pattern's length
    a.doc.editPattern(a.curPattern, "demo", [&](Pattern& x) { x.bars = std::clamp(std::atoi(n), 1, 64); });
  if (demo && *demo) loadTrack(m, 0, demo, DeckModel{});
  if (demoB && *demoB) loadTrack(m, 1, demoB, DeckModel{});
  st.data = formatData(m.trackOf(0), m);
  setPluginState(a, t, st, "demo");
  sp = t.state(a.doc.song());
}

// J-160: a deck takes a dragged track anywhere on its lane (waveform and overview) and
// its panel: browser and desktop files, playlist clip sources, channel rack rows that
// hold a sample. It loads like the deck's load button (analysis, undo, collaboration).
// Being smaller than the plugin window's own drop zone (which loads deck A), it wins
// while the pointer is over it; only the deck under the pointer lights up.
void deckDropTarget(App& a, const PluginTarget& t, int deck, const ImRect* rects, int n) {
  for (int i = 0; i < n; ++i) {  // dev hook (GRIDLINE_PLATTER_TRACE): where each deck takes drops
    char nm[16];
    std::snprintf(nm, sizeof nm, "drop%c%d", deck == 0 ? 'A' : 'B', i);
    traceRect(nm, rects[i].Min, rects[i].Max);
  }
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl) return;
  std::string why;
  const std::string path = browserui::draggedAudioFile(a, pl, &why);
  if (path.empty() && why.empty()) return;  // not audio: the window's own drop zone decides
  for (int i = 0; i < n; ++i) {
    char id[24];
    std::snprintf(id, sizeof id, "##pdrop%d.%d", deck, i);
    if (!ImGui::BeginDragDropTargetCustom(rects[i], ImGui::GetID(id))) continue;
    if (path.empty()) {
      ImGui::SetTooltip("%s", why.c_str());
      a.setHint(why);
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    } else if (const ImGuiPayload* x =
                   ImGui::AcceptDragDropPayload(pl->DataType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
      if (x->Delivery) {
        xdragui::markConsumed();
        loadOnDeck(a, t, deck, path);
      } else {
        if (x->Preview) {  // the whole deck: its lane and its panel
          ImDrawList* fg = ImGui::GetForegroundDrawList();
          for (int k = 0; k < n; ++k) {
            fg->AddRectFilled(rects[k].Min, rects[k].Max, deckColor(deck, 0.10f), theme::kRadiusLg);
            fg->AddRect(rects[k].Min, rects[k].Max, deckColor(deck), theme::kRadiusLg, 0, 2.5f);
          }
        }
        char tip[512];
        std::snprintf(tip, sizeof tip, deck == 0 ? i18n::tr("Load %s onto deck A") : i18n::tr("Load %s onto deck B"),
                      fs::path(path).filename().string().c_str());
        ImGui::SetTooltip("%s", tip);
        a.setHint(tip);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
    }
    ImGui::EndDragDropTarget();
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
  devHooks(a, t, e, sp);
  if (!sp) return true;
  // parse the state when it changed (undo, drops, loads)
  if (!e.parsed || sp->data != e.data) {
    e.parsed = true;
    e.data = sp->data;
    std::string first;
    Model m;
    parseData(sp->data, first, m);
    e.m = std::move(m);
    if (!e.m.asked) e.setup = true, e.setupInit = false;
  }
  // per deck: the analysis gives the grid (and the peaks), the bands colour the waveforms
  for (int d = 0; d < kDecks; ++d) {
    DeckUi& u = e.d[static_cast<size_t>(d)];
    u.path = e.m.trackOf(d);
    if (u.path.empty()) {
      u.job.reset();
      u.bands.reset();
      continue;
    }
    if (!u.job || u.job->path != u.path) u.job = seltmann::requestAnalysis(u.path), u.bands.reset();
    if (u.job->state != 1 || !u.job->sample) continue;
    if (!u.bands) u.bands = bandsFor(u.job->sample, u.path);
    const DeckModel& dm = e.m.deck[static_cast<size_t>(d)];
    const bool sameFile = dm.srcFrames == static_cast<int64_t>(u.job->sample->frames());
    if (!sameFile || !dm.analysed) {
      DeckModel fresh = deckModel(*u.job->sample, u.job->result, true);
      if (sameFile) {  // keep the cues, take the grid
        DeckModel keep = dm;
        keep.grid = fresh.grid;
        keep.analysed = true;
        fresh = keep;
      }
      Model nm = e.m;
      fresh.file = dm.file;
      nm.deck[static_cast<size_t>(d)] = fresh;
      commit(a, t, e, nm, i18n::N_("Analyse the track"));
    }
    if ((e.demoHots & (1 << d)) && e.m.deck[static_cast<size_t>(d)].analysed) {  // dev hook: a few hot cues on bars
      e.demoHots &= ~(1 << d);
      Model nm = e.m;
      DeckModel& x = nm.deck[static_cast<size_t>(d)];
      const int bpb = std::max(1, x.grid.beatsPerBar);
      const int bars[4] = {1, 3, 4, 6};
      const int keys[4] = {0, 1, 3, 5 + d};
      for (int k = 0; k < 4; ++k) x.hot[static_cast<size_t>(keys[k])] = frameAt(x, static_cast<double>(x.grid.bar1 + bars[k] * bpb));
      commit(a, t, e, nm, "demo");
    }
  }
  const bool two = e.m.twoDecks();
  LiveView lv[kDecks];
  for (int d = 0; d < kDecks; ++d) lv[d] = live(a, t, d);
  const MixView mv = mixLive(a, t);
  // dev hook: start decks once they are ready
  for (int d = 0; d < kDecks; ++d)
    if ((e.demoPlay & (1 << d)) && deckReady(e, d) && lv[d].loaded) {
      press(a, t, d, CtlPlay, true);
      e.demoPlay &= ~(1 << d);
    }

  // width: the channel window gives the width that fits the screen (J-158: down to editorMinWidth,
  // narrower windows reflow), the Suite zooms to fit
  const float W = std::floor(std::max(width, 1.0f));
  // J-158: in a short window the waveforms, the platters and the pads give way (down to the
  // most compact layout) before the window has to scroll; the Suite zooms instead.
  {
    setSqueeze(0);
    float f = 0;
    if (!a.embedded) {
      const float natural = 40 + wavesHeight(two) + 10 + deckPanelHeight() + 6;
      setSqueeze(1);
      const float compact = 40 + wavesHeight(two) + 10 + deckPanelHeight() + 6;
      const ImGuiWindow* win = ImGui::GetCurrentWindow();
      const float avail = ui::EditorArea().y - ImGui::GetCursorPosY() - win->WindowPadding.y - ImGui::GetStyle().ItemSpacing.y -
                          (win->ScrollbarX ? ImGui::GetStyle().ScrollbarSize : 0.0f);
      if (natural > compact) f = std::clamp((natural - avail) / (natural - compact), 0.0f, 1.0f);
    }
    setSqueeze(f);
  }
  const ImVec2 o = ImGui::GetCursorScreenPos();
  drawTopBar(a, t, e, o, W, lv);
  float y = o.y + 30 + 10;
  const float gap = 10;
  const float wavesH = wavesHeight(two);
  const float mainH = deckPanelHeight();
  if (e.setup) {
    const float H = wavesH + gap + mainH;
    drawSetup(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + H));
    y += H;
    ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
    ImGui::Dummy(ImVec2(W, 1));
    trace(a, t, e, "setup", lv);
    return true;
  }
  drawWaves(a, t, e, ImVec2(o.x, y), W, lv);
  const float wavesY = y, laneH = wavesHeight(false);  // one deck's lane (the drop targets)
  y += wavesH + gap;
  // decks and the mixer
  if (two) {
    const float mixW = std::round(std::clamp(W * 0.235f, 252.0f, 300.0f));
    const float deckW = std::floor((W - mixW - 2 * gap) * 0.5f);
    const float bx = o.x + W - deckW;
    drawDeck(a, t, e, 0, ImVec2(o.x, y), ImVec2(o.x + deckW, y + mainH), lv[0], false);
    drawMixer(a, t, e, ImVec2(o.x + deckW + gap, y), ImVec2(bx - gap, y + mainH), lv, mv, true);
    drawDeck(a, t, e, 1, ImVec2(bx, y), ImVec2(o.x + W, y + mainH), lv[1], true);
    const ImRect a0[2] = {ImRect(o.x, wavesY, o.x + W, wavesY + laneH), ImRect(o.x, y, o.x + deckW, y + mainH)};
    const ImRect b0[2] = {ImRect(o.x, wavesY + wavesH - laneH, o.x + W, wavesY + wavesH), ImRect(bx, y, o.x + W, y + mainH)};
    deckDropTarget(a, t, 0, a0, 2);
    deckDropTarget(a, t, 1, b0, 2);
  } else {
    const float mixW = 168;
    const float deckW = W - mixW - gap;
    drawDeck(a, t, e, 0, ImVec2(o.x, y), ImVec2(o.x + deckW, y + mainH), lv[0], false);
    drawMixer(a, t, e, ImVec2(o.x + deckW + gap, y), ImVec2(o.x + W, y + mainH), lv, mv, false);
    const ImRect a0[2] = {ImRect(o.x, wavesY, o.x + W, wavesY + wavesH), ImRect(o.x, y, o.x + deckW, y + mainH)};
    deckDropTarget(a, t, 0, a0, 2);
  }
  y += mainH;
  if (e.drawer) {
    y += gap;
    const float drawerH = 300;
    drawDrawer(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + drawerH), lv[0]);
    y += drawerH;
  } else {
    e.learn = -1;
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, y + 6));
  ImGui::Dummy(ImVec2(W, 1));
  traceTracks(e);
  bool ready = deckReady(e, 0) || deckReady(e, 1);
  for (int d = 0; d < kDecks; ++d)
    if (!e.d[static_cast<size_t>(d)].path.empty() && !deckReady(e, d) && (d == 0 || two)) ready = false;
  trace(a, t, e, ready ? "ready" : (e.d[0].path.empty() && e.d[1].path.empty()) ? "empty" : "loading", lv);
  return true;
}

}  // namespace gl::packs::platter
