// Gridline — "platter" pack: the waveform panel and the decks.
//
// Waveform panel (full width): deck A's whole track, its scrolling waveform, a
// phase meter, deck B's scrolling waveform and whole track. Waveforms are
// coloured by band (low, mid, high), with the beat grid and bar numbers, the
// cue, hot cue flags, the loop and the slip ghost; both scroll under one centre
// playhead so the beat grids line up when the decks are in time. Drag a
// waveform to scratch, scroll to zoom, click a whole track to jump.
//
// A deck: header (badge, track, tempo and key, time left, effective BPM), body
// (pitch fader on the outer edge, the platter, then sync / key lock / modes and
// the motor dials next to the mixer), CUE and PLAY with eight hot cue pads, and
// the loop row. Deck B mirrors deck A.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../app/Look.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Song.h"
#include "../../engine/AudioEngine.h"
#include "platter_ui.h"

namespace gl::packs::platter::pv {

namespace {

namespace fs = std::filesystem;
using theme::pal;
constexpr float kPi = 3.14159265358979f;

// Waveform panel rows.
constexpr float kOvH = 22, kWaveH = 84, kPhaseH = 16, kRowGap = 4;
// Deck rows.
constexpr float kPad = 12, kHeadH = 50, kBodyH = 280, kPerfH = 84, kLoopH = 30, kGap = 10;

double srcRateOf(const DeckModel& m) { return m.srcRate > 0 ? m.srcRate : 44100.0; }

// Band colours of the waveforms (low, mid, high), tuned per theme.
ImU32 bandLo(float al) { return theme::isDark() ? theme::rgb(0x3E8EDE, al) : theme::rgb(0x2266B8, al); }
ImU32 bandMid(float al) { return theme::isDark() ? theme::rgb(0xF2A73B, al) : theme::rgb(0xC97A10, al); }
ImU32 bandHi(float al) { return theme::isDark() ? theme::rgb(0xF4F1EA, al) : theme::rgb(0x2A2C33, al); }

// Band peaks over [f0, f1) source frames (0..1).
void bandsAt(const Bands& b, double f0, double f1, float& lo, float& mid, float& hi) {
  lo = mid = hi = 0;
  const auto nb = static_cast<int64_t>(b.lo.size());
  if (nb == 0) return;
  int64_t b0 = static_cast<int64_t>(std::floor(f0 / kBandBlock));
  int64_t b1 = std::max(b0 + 1, static_cast<int64_t>(std::ceil(f1 / kBandBlock)));
  if (b1 <= 0 || b0 >= nb) return;
  b0 = std::max<int64_t>(0, b0), b1 = std::min(nb, b1);
  uint8_t l = 0, m = 0, h = 0;
  for (int64_t i = b0; i < b1; ++i) {
    const auto k = static_cast<size_t>(i);
    l = std::max(l, b.lo[k]), m = std::max(m, b.mid[k]), h = std::max(h, b.hi[k]);
  }
  lo = l / 255.0f, mid = m / 255.0f, hi = h / 255.0f;
}

// One column of a band waveform at x, centred on cy, half height hh.
void bandColumn(ImDrawList* dl, float x, float cy, float hh, float lo, float mid, float hi, float al) {
  // the lows are the body, the mids a core inside it, the highs the bright tips
  const float hl = std::max(0.5f, lo * hh), hm = mid * hh * 0.66f, hhi = hi * hh * 0.38f;
  dl->AddLine(ImVec2(x, cy - hl), ImVec2(x, cy + hl + 1), bandLo(al));
  if (hm > 0.5f) dl->AddLine(ImVec2(x, cy - hm), ImVec2(x, cy + hm + 1), bandMid(al * 0.9f));
  if (hhi > 0.5f) dl->AddLine(ImVec2(x, cy - hhi), ImVec2(x, cy + hhi + 1), bandHi(al * 0.75f));
}

// ------------------------------------------------------------------ empty / analysing lane
void drawEmptyLane(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  dl->AddRect(p0, p1, P.line, theme::kRadius);
  const DeckUi& u = e.d[static_cast<size_t>(d)];
  const float cy = (p0.y + p1.y) * 0.5f;
  // deck tag
  const ImVec2 b0(p0.x + 8, cy - 11), b1(p0.x + 30, cy + 11);
  dl->AddRectFilled(b0, b1, deckColor(d, 0.18f), theme::kRadius);
  dl->AddRect(b0, b1, deckColor(d, 0.6f), theme::kRadius);
  textC(dl, a.fontBold, theme::kFontCaption, ImVec2((b0.x + b1.x) * 0.5f, cy), deckColor(d), d == 0 ? "A" : "B");
  const float x = p0.x + 44;
  const bool analysing = !u.path.empty() && u.job && u.job->state == 0;
  const bool failed = !u.path.empty() && u.job && u.job->state == 2;
  if (analysing) {
    const float p = std::clamp(u.job->progress.load(), 0.0f, 1.0f);
    theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(x, cy - 18), p1.x - x - 20, P.text, fs::path(u.path).filename().string().c_str());
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy + 1), P.textDim, p < 0.5f ? i18n::tr("Listening for the beats") : i18n::tr("Laying the beat grid"));
    const float bw = std::min(260.0f, p1.x - x - 200);
    const ImVec2 q0(x + 190, cy + 6), q1(x + 190 + bw, cy + 10);
    dl->AddRectFilled(q0, q1, P.surface900, 2);
    dl->AddRectFilled(q0, ImVec2(q0.x + bw * p, q1.y), deckColor(d), 2);
    return;
  }
  const char* title = failed ? i18n::tr("This file cannot be read") : d == 0 ? i18n::tr("Drop a track on deck A") : i18n::tr("Drop a track on deck B");
  dl->AddText(a.fontBold, theme::kFontBody, ImVec2(x, cy - 18), P.text, title);
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy + 1), P.textDim,
              failed ? i18n::tr("Try another file: WAV, AIFF, FLAC, MP3 or OGG.")
                     : i18n::tr("Its tempo and beats are found and it plays in time. The padlock lets it run free."));
  char id[16];
  std::snprintf(id, sizeof id, "##pload%d", d);
  if (button(id, ImVec2(p1.x - 150, cy - 14), ImVec2(138, 28), i18n::N_("Load a track..."), false, nullptr, static_cast<int>(ui::Icon::Open)))
    openLoadDialog(a, t, d);
}

// ------------------------------------------------------------------ whole track
void drawOverview(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const DeckUi& u = e.d[static_cast<size_t>(d)];
  const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const float lx = p0.x + 28;  // deck tag on the left
  const ImVec2 b0(p0.x + 3, p0.y + 3), b1(p0.x + 23, p1.y - 3);
  dl->AddRectFilled(b0, b1, deckColor(d, 0.2f), theme::kRadius);
  textC(dl, a.fontBold, theme::kFontLabel, ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), deckColor(d), d == 0 ? "A" : "B");
  const float w = p1.x - lx - 64, h = p1.y - p0.y, cy = (p0.y + p1.y) * 0.5f;
  const double total = static_cast<double>(u.job->peaks.frames);
  const double fpp = total / std::max(1.0f, w);
  const float playX = lx + static_cast<float>(lv.pos / fpp);
  const bool bands = u.bands && u.bands->ready.load(std::memory_order_acquire);
  for (float x = 0; x < w; x += 1) {
    const bool played = lx + x < playX;
    const float al = played ? 0.38f : 0.95f;
    if (bands) {
      float lo, mid, hi;
      bandsAt(*u.bands, x * fpp, (x + 1) * fpp, lo, mid, hi);
      bandColumn(dl, lx + x + 0.5f, cy, h * 0.5f - 2, lo, mid, hi, al);
    }
  }
  auto xOf = [&](double f) { return lx + static_cast<float>(f / fpp); };
  if (lv.loopIn >= 0 && lv.loopOut > lv.loopIn)
    dl->AddRectFilled(ImVec2(xOf(lv.loopIn), p0.y + 1), ImVec2(std::max(xOf(lv.loopOut), xOf(lv.loopIn) + 2), p1.y - 1),
                      theme::withAlpha(hot(), lv.loopOn ? 0.30f : 0.12f));
  for (int k = 0; k < kHotCues; ++k)
    if (lv.hot[static_cast<size_t>(k)] >= 0) {
      const float x = std::round(xOf(lv.hot[static_cast<size_t>(k)]));
      dl->AddRectFilled(ImVec2(x - 1, p0.y + 1), ImVec2(x + 1, p0.y + 7), hotColor(k));
    }
  {
    const float x = std::round(xOf(lv.cue));
    dl->AddTriangleFilled(ImVec2(x - 4, p1.y - 1), ImVec2(x + 4, p1.y - 1), ImVec2(x, p1.y - 6), cueColor());
  }
  dl->AddLine(ImVec2(playX, p0.y + 1), ImVec2(playX, p1.y - 1), P.playhead, 2);
  // click / drag: jump there
  ImGui::SetCursorScreenPos(ImVec2(lx, p0.y));
  char id[16];
  std::snprintf(id, sizeof id, "##pover%d", d);
  ImGui::InvisibleButton(id, ImVec2(w, h));
  if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("The whole track: click to jump"));
  if (ImGui::IsItemActive() && (ImGui::IsItemActivated() || ImGui::IsMouseDragging(0, 2))) {
    const double f = std::clamp((ImGui::GetIO().MousePos.x - lx) * fpp, 0.0, total);
    send(a, t, d, CmdSeek, 0, f);
  }
  // time left at the deck's tempo (a scratch or a throw does not change it)
  const double rate = lv.tempo > 0 && bpmOf(m) > 0 ? lv.tempo / bpmOf(m) : 1.0;
  const double left = std::max(0.0, total - lv.pos) / srcRateOf(m) / std::max(0.05, rate);
  const std::string tl = fmtTime(-left);
  textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 8, cy - 7), P.textDim, tl.c_str());
}

// ------------------------------------------------------------------ scrolling waveform
void drawWave(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DeckUi& u = e.d[static_cast<size_t>(d)];
  const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const float w = p1.x - p0.x, h = p1.y - p0.y, cy = std::round((p0.y + p1.y) * 0.5f);
  const float cx = std::round(p0.x + w * 0.5f);
  const double sr = srcRateOf(m);
  const double fpp = e.zoomSeconds * sr / w;
  const double pos = lv.pos;
  auto xOf = [&](double f) { return cx + static_cast<float>((f - pos) / fpp); };
  dl->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);
  // loop
  if (lv.loopIn >= 0 && lv.loopOut > lv.loopIn)
    dl->AddRectFilled(ImVec2(xOf(lv.loopIn), p0.y + 1), ImVec2(xOf(lv.loopOut), p1.y - 1), theme::withAlpha(hot(), lv.loopOn ? 0.16f : 0.06f));
  // beat grid (bars brighter, numbered)
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double b0 = std::ceil(beatAt(m, pos - (cx - p0.x) * fpp)), b1 = std::floor(beatAt(m, pos + (p1.x - cx) * fpp));
  for (double b = b0; b <= b1 && b - b0 < 400; b += 1) {
    const float x = std::round(xOf(frameAt(m, b)));
    const int rel = static_cast<int>(std::lround(b)) - m.grid.bar1;
    const bool bar = ((rel % bpb) + bpb) % bpb == 0;
    dl->AddLine(ImVec2(x + 0.5f, p0.y + 1), ImVec2(x + 0.5f, p1.y - 1), theme::withAlpha(P.text, bar ? 0.26f : 0.08f), 1);
    if (bar) {
      char nb[16];
      std::snprintf(nb, sizeof nb, "%d", rel / bpb + 1);
      dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(x + 4, p1.y - 14), theme::withAlpha(P.text, 0.40f), nb);
    }
  }
  // the waveform: three bands
  if (u.bands && u.bands->ready.load(std::memory_order_acquire)) {
    const Bands& bd = *u.bands;
    for (float x = p0.x + 1; x < p1.x - 1; x += 1) {
      const double f0 = pos + (x - cx) * fpp;
      if (f0 + fpp < 0 || f0 > static_cast<double>(bd.frames)) continue;
      float lo, mid, hi;
      bandsAt(bd, f0, f0 + fpp, lo, mid, hi);
      bandColumn(dl, x + 0.5f, cy, h * 0.5f - 7, lo, mid, hi, x < cx ? 0.55f : 1.0f);
    }
  }
  // cue + hot cue flags
  {
    const float x = std::round(xOf(lv.cue));
    dl->AddLine(ImVec2(x + 0.5f, p0.y + 1), ImVec2(x + 0.5f, p1.y - 1), theme::withAlpha(cueColor(), 0.8f), 1.5f);
    dl->AddTriangleFilled(ImVec2(x - 5, p0.y + 1), ImVec2(x + 6, p0.y + 1), ImVec2(x + 0.5f, p0.y + 8), cueColor());
  }
  for (int k = 0; k < kHotCues; ++k) {
    const double f = lv.hot[static_cast<size_t>(k)];
    if (f < 0) continue;
    const float x = std::round(xOf(f));
    const ImU32 hc = hotColor(k);
    dl->AddLine(ImVec2(x + 0.5f, p0.y + 1), ImVec2(x + 0.5f, p1.y - 1), theme::withAlpha(hc, 0.75f), 1.5f);
    dl->AddRectFilled(ImVec2(x, p0.y + 3), ImVec2(x + 14, p0.y + 16), hc, 2, ImDrawFlags_RoundCornersRight);
    char nb[4];
    std::snprintf(nb, sizeof nb, "%d", k + 1);
    textC(dl, a.fontBold, theme::kFontLabel, ImVec2(x + 7, p0.y + 9.5f), theme::contrastText(hc), nb);
  }
  if (lv.slipping && lv.slipPos >= 0) {
    const float x = std::round(xOf(lv.slipPos));
    for (float y = p0.y + 2; y < p1.y - 2; y += 6) dl->AddLine(ImVec2(x, y), ImVec2(x, y + 3), P.text, 1.5f);
  }
  dl->PopClipRect();
  // playhead (shared centre line)
  dl->AddLine(ImVec2(cx, p0.y + 1), ImVec2(cx, p1.y - 1), P.playhead, 2);
  // deck tag, BPM and the take that plays
  {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", lv.tempo > 0 ? lv.tempo : bpmOf(m));
    const ImVec2 q0(p0.x + 6, p0.y + 6), q1(p0.x + 26, p0.y + 24);
    dl->AddRectFilled(q0, q1, deckColor(d, 0.22f), theme::kRadius);
    textC(dl, a.fontBold, theme::kFontLabel, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), deckColor(d), d == 0 ? "A" : "B");
    const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, b);
    dl->AddRectFilled(ImVec2(q1.x + 3, q0.y), ImVec2(q1.x + ts.x + 13, q1.y), theme::withAlpha(P.well, 0.8f), theme::kRadius);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(q1.x + 8, q0.y + 3), P.text, b);
  }
  // drag = the hand on the record; wheel = zoom
  ImGui::SetCursorScreenPos(p0);
  char id[16];
  std::snprintf(id, sizeof id, "##pwave%d", d);
  ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hv = ImGui::IsItemHovered();
  if (hv && !ImGui::IsItemActive()) ui::Tooltip(i18n::tr("Drag the waveform to scratch. Scroll to zoom both waveforms."));
  if (hv && ImGui::GetIO().MouseWheel != 0) e.zoomSeconds = std::clamp(e.zoomSeconds * std::pow(0.85, ImGui::GetIO().MouseWheel), 1.0, 32.0);
  if (ImGui::IsItemActivated()) {
    u.waveDrag = true;
    u.waveGrabX = ImGui::GetIO().MousePos.x;
    send(a, t, d, CmdHandDown);
  }
  if (u.waveDrag && ImGui::IsItemActive()) {
    const double frames = -(ImGui::GetIO().MousePos.x - u.waveGrabX) * fpp;
    send(a, t, d, CmdHandMove, 0, frames / (sr * kSecondsPerRev));
  }
  if (u.waveDrag && ImGui::IsItemDeactivated()) {
    u.waveDrag = false;
    send(a, t, d, CmdHandUp);
  }
  if (hv || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
}

// ------------------------------------------------------------------ phase meter
// Each deck's bar as cells (the beat it is on lit), and the offset between the decks.
void drawPhase(App& a, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView* lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  caps(dl, a, ImVec2(p0.x + 2, p0.y + 2), P.textFaint, i18n::tr("PHASE"));
  const float x0 = p0.x + 56, x1 = p1.x - 96;
  const float rowH = (p1.y - p0.y - 2) * 0.5f;
  for (int d = 0; d < kDecks; ++d) {
    const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
    const int bpb = std::max(1, m.grid.beatsPerBar);
    const float y0 = p0.y + d * (rowH + 2), y1 = y0 + rowH;
    const float cw = (x1 - x0) / bpb;
    const bool live = lv[d].loaded && deckReady(e, d);
    const double rel = lv[d].beat - m.grid.bar1;
    const int cur = live ? ((static_cast<int>(std::floor(rel)) % bpb) + bpb) % bpb : -1;
    for (int k = 0; k < bpb; ++k) {
      const ImVec2 q0(x0 + k * cw + 1, y0), q1(x0 + (k + 1) * cw - 1, y1);
      const bool lit = k == cur;
      dl->AddRectFilled(q0, q1, lit ? deckColor(d, lv[d].playing ? 0.95f : 0.55f) : theme::withAlpha(P.surface600, 0.55f), 1.5f);
    }
  }
  // the offset: B's phase against A's, in beats (0 = in time)
  if (deckReady(e, 0) && deckReady(e, 1) && lv[0].loaded && lv[1].loaded) {
    double off = (lv[1].beat - std::floor(lv[1].beat)) - (lv[0].beat - std::floor(lv[0].beat));
    if (off > 0.5) off -= 1;
    if (off < -0.5) off += 1;
    char b[32];
    std::snprintf(b, sizeof b, "%s%.2f", off >= 0 ? "+" : "", off);
    const bool inTime = std::fabs(off) < 0.02;
    textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 2, p0.y + 2), inTime ? P.led : P.textDim, b);
  }
}

// ------------------------------------------------------------------ transport buttons
void roundButton(App& a, const PluginTarget& t, int d, const char* id, ImVec2 c, float r, const char* label, int control, bool lit, ImU32 hue,
                 const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
  if (control == CtlPlay) traceRect(d == 0 ? "playA" : "playB", ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
  ImGui::InvisibleButton(id, ImVec2(2 * r, 2 * r));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (ImGui::IsItemActivated()) press(a, t, d, control, true);
  if (ImGui::IsItemDeactivated()) press(a, t, d, control, false);
  if (hv) ui::Tooltip(i18n::tr(hint));
  skin::gradientCircle(dl, ImVec2(c.x, c.y + 2), r + 1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.3f));
  skin::gradientCircle(dl, c, r, ac ? P.surface600 : (hv ? theme::mix(P.surface600, P.paper, 0.05f) : P.surface700), P.surface900);
  dl->AddCircle(c, r - 0.5f, lit ? hue : theme::withAlpha(hue, 0.45f), 48, lit ? 2.5f : 1.5f);
  if (lit) dl->AddCircle(c, r + 3, theme::withAlpha(hue, 0.22f), 48, 3);
  // the face: CUE in letters, PLAY as the play / pause glyph (the hardware's own markings)
  const ImU32 g = lit ? hue : P.text;
  if (control == CtlPlay) {
    const float s = r / 26.0f;
    dl->AddTriangleFilled(ImVec2(c.x - 10 * s, c.y - 8 * s), ImVec2(c.x - 10 * s, c.y + 8 * s), ImVec2(c.x + 1 * s, c.y), g);
    dl->AddRectFilled(ImVec2(c.x + 4 * s, c.y - 8 * s), ImVec2(c.x + 7 * s, c.y + 8 * s), g);
    dl->AddRectFilled(ImVec2(c.x + 10 * s, c.y - 8 * s), ImVec2(c.x + 13 * s, c.y + 8 * s), g);
  } else {
    textC(dl, a.fontBold, 15, c, g, label);
  }
}

// ------------------------------------------------------------------ the platter
void drawPlatter(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 c, float R, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DeckUi& u = e.d[static_cast<size_t>(d)];
  const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
  const double turns = lv.valid ? lv.pos / (srcRateOf(m) * kSecondsPerRev) : 0.0;
  const float ang = static_cast<float>((turns - std::floor(turns)) * 2 * kPi) - kPi * 0.5f;
  // the strobe: the rim's dots stand still at exactly 1.0x and drift as the speed leaves it
  {
    const double dt = std::min(0.1, static_cast<double>(ImGui::GetIO().DeltaTime));
    if (lv.playing && std::fabs(lv.speed - 1.0) < 0.12) u.strobe += (lv.speed - 1.0) * dt * 4.0;
    else u.strobe = turns;
  }
  // track progress ring (outside the platter): played part, cue and hot cue ticks
  const float Rr = R + 6;
  const double total = static_cast<double>(std::max<int64_t>(1, m.srcFrames));
  dl->AddCircle(c, Rr, theme::withAlpha(P.surface600, 0.9f), 96, 3);
  if (lv.loaded) {
    const float frac = static_cast<float>(std::clamp(lv.pos / total, 0.0, 1.0));
    if (frac > 0.001f) {
      dl->PathArcTo(c, Rr, -kPi * 0.5f, -kPi * 0.5f + frac * 2 * kPi, std::max(8, static_cast<int>(96 * frac)));
      dl->PathStroke(deckColor(d), 0, 3.0f);
    }
    auto tick = [&](double f, ImU32 col) {
      const float th = static_cast<float>(f / total) * 2 * kPi - kPi * 0.5f;
      dl->AddLine(ImVec2(c.x + std::cos(th) * (Rr - 4), c.y + std::sin(th) * (Rr - 4)), ImVec2(c.x + std::cos(th) * (Rr + 4), c.y + std::sin(th) * (Rr + 4)),
                  col, 2);
    };
    tick(lv.cue, cueColor());
    for (int k = 0; k < kHotCues; ++k)
      if (lv.hot[static_cast<size_t>(k)] >= 0) tick(lv.hot[static_cast<size_t>(k)], hotColor(k));
  }
  // aluminium platter with strobe dots
  skin::gradientCircle(dl, ImVec2(c.x, c.y + 3), R + 1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.45f));
  skin::gradientCircle(dl, c, R, theme::rgb(0xC9CCD2), theme::rgb(0x6C7079));
  dl->AddCircle(c, R - 0.5f, theme::rgb(0xFFFFFF, 0.35f), 96, 1);
  const int dots = std::clamp(static_cast<int>(R * 0.75f), 48, 120);
  const float sAng = static_cast<float>((u.strobe - std::floor(u.strobe)) * 2 * kPi);
  for (int k = 0; k < dots; ++k) {
    const float th = sAng + k * 2 * kPi / dots;
    const ImVec2 q(c.x + std::cos(th) * (R - 4.5f), c.y + std::sin(th) * (R - 4.5f));
    dl->AddCircleFilled(q, 1.3f, theme::rgb(0x23252B, 0.85f), 6);
  }
  // the record: grooves and fixed light reflections
  const float rv = R - 10;
  dl->AddCircleFilled(c, rv, theme::rgb(0x101114), 96);
  for (float rr = rv - 3; rr > rv * 0.40f; rr -= 2.6f) dl->AddCircle(c, rr, theme::rgb(0x2B2D35, 0.55f), 96, 1);
  for (float rr : {rv * 0.62f, rv * 0.81f}) dl->AddCircle(c, rr, theme::rgb(0x07080A), 96, 2.5f);  // track gaps
  dl->PathArcTo(c, rv * 0.70f, -kPi * 0.88f, -kPi * 0.64f, 24);
  dl->PathStroke(theme::rgb(0xFFFFFF, 0.075f), 0, rv * 0.52f);
  dl->PathArcTo(c, rv * 0.70f, kPi * 0.12f, kPi * 0.36f, 24);
  dl->PathStroke(theme::rgb(0xFFFFFF, 0.05f), 0, rv * 0.52f);
  // the label (turns with the record): deck colour, its letter, a marker
  const float rl = rv * 0.36f;
  skin::gradientCircle(dl, c, rl, deckColor(d), theme::mix(deckColor(d), theme::rgb(0x000000), 0.35f));
  const ImVec2 dir(std::cos(ang), std::sin(ang));
  {
    const ImVec2 lc(c.x - dir.x * rl * 0.52f, c.y - dir.y * rl * 0.52f);
    textC(dl, a.fontBold, std::max(11.0f, rl * 0.42f), lc, theme::rgb(0x101114, 0.85f), d == 0 ? "A" : "B");
  }
  dl->AddLine(ImVec2(c.x + dir.x * rl * 0.30f, c.y + dir.y * rl * 0.30f), ImVec2(c.x + dir.x * rl * 0.92f, c.y + dir.y * rl * 0.92f),
              theme::rgb(0x101114, 0.85f), 3);
  dl->AddLine(ImVec2(c.x + dir.x * (rl + 3), c.y + dir.y * (rl + 3)), ImVec2(c.x + dir.x * (rv - 2), c.y + dir.y * (rv - 2)), theme::rgb(0xFFFFFF, 0.5f), 2);
  dl->AddCircleFilled(c, 4.5f, theme::rgb(0xC9CCD2), 16);
  dl->AddCircle(c, 4.5f, theme::rgb(0x000000, 0.4f), 16, 1);
  if (lv.touched) dl->AddCircle(c, R + 1.5f, hot(), 96, 2.5f);
  // the hand: drag the record round
  ImGui::SetCursorScreenPos(ImVec2(c.x - R, c.y - R));
  char id[16];
  std::snprintf(id, sizeof id, "##pplat%d", d);
  ImGui::InvisibleButton(id, ImVec2(2 * R, 2 * R));
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const float dx = mp.x - c.x, dy = mp.y - c.y;
  const bool inside = dx * dx + dy * dy <= R * R;
  if (ImGui::IsItemHovered() && inside && !u.platterDrag) ui::Tooltip(i18n::tr("Drag the record round to scratch: clockwise plays, back plays backwards."));
  if (ImGui::IsItemActivated() && inside) {
    u.platterDrag = true;
    u.dragTurns = 0;
    u.lastAngle = std::atan2(dy, dx);
    send(a, t, d, CmdHandDown);
  }
  if (u.platterDrag && ImGui::IsItemActive()) {
    const double an = std::atan2(dy, dx);
    double dd = an - u.lastAngle;
    if (dd > kPi) dd -= 2 * kPi;
    if (dd < -kPi) dd += 2 * kPi;
    if (dx * dx + dy * dy > 16) u.dragTurns += dd / (2 * kPi);  // (no wild turns through the centre)
    u.lastAngle = an;
    send(a, t, d, CmdHandMove, 0, u.dragTurns);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  if (u.platterDrag && ImGui::IsItemDeactivated()) {
    u.platterDrag = false;
    send(a, t, d, CmdHandUp);
  }
}

// ------------------------------------------------------------------ pitch fader (down = faster, like the hardware)
void drawPitch(App& a, const PluginTarget& t, int d, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int kp = deckParam(d, DPitch), kr = deckParam(d, DPitchRange);
  const double v = param(a, t, kp);
  const int range = static_cast<int>(std::lround(param(a, t, kr)));
  const double pct = v * pitchRangeOf(range) * 100.0;
  const float cx = std::round((p0.x + p1.x) * 0.5f);
  char b[24];
  std::snprintf(b, sizeof b, "%s%.1f", pct >= 0 ? "+" : "", pct);
  textC(dl, a.fontMono, theme::kFontLabel, ImVec2(cx, p0.y + 6), std::fabs(pct) < 0.05 ? P.textDim : P.text, b);
  const float t0 = p0.y + 24, t1 = p1.y - 36;
  const float len = t1 - t0;
  // scale: centre line long, quarter ticks
  for (int k = 0; k <= 8; ++k) {
    const float y = std::round(t0 + len * k / 8.0f);
    const float half = k == 4 ? 9.0f : (k % 2 ? 3.0f : 5.0f);
    dl->AddLine(ImVec2(cx - 12 - half, y), ImVec2(cx - 12, y), k == 4 ? P.textDim : P.textFaint, 1);
    dl->AddLine(ImVec2(cx + 12, y), ImVec2(cx + 12 + half, y), k == 4 ? P.textDim : P.textFaint, 1);
  }
  skin::insetField(dl, ImVec2(cx - 3, t0 - 4), ImVec2(cx + 3, t1 + 4), P.surface950, 3);
  const float yv = t0 + static_cast<float>((v + 1) * 0.5) * len;  // top = -range, bottom = +range
  const float ymid = t0 + len * 0.5f;
  if (std::fabs(yv - ymid) > 1) dl->AddRectFilled(ImVec2(cx - 1.5f, std::min(ymid, yv)), ImVec2(cx + 1.5f, std::max(ymid, yv)), deckColor(d, 0.9f), 1);
  // the cap
  const ImVec2 q0(cx - 15, yv - 10), q1(cx + 15, yv + 10);
  ImGui::SetCursorScreenPos(ImVec2(cx - 20, t0 - 10));
  char id[16];
  std::snprintf(id, sizeof id, "##ppitch%d", d);
  ImGui::InvisibleButton(id, ImVec2(40, len + 20));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? theme::mix(P.surface700, P.paper, 0.06f) : P.surface700, 3);
  dl->AddRect(q0, q1, P.line, 3);
  dl->AddLine(ImVec2(q0.x + 3, yv), ImVec2(q1.x - 3, yv), std::fabs(v) < 1e-3 ? P.led : P.paper, 2);
  if (hv || ac) ui::Tooltip(i18n::tr("Pitch fader: down plays faster, up slower (double-click: centre). With sync on it is not used."));
  (void)t.pid(kp);
  if (ac && ImGui::IsMouseDragging(0, 0)) {
    const double dv = ImGui::GetIO().MouseDelta.y / len * 2.0 * (ImGui::GetIO().KeyShift ? 0.1 : 1.0);
    pui::setParamValue(a, t, kp, std::clamp(v + dv, -1.0, 1.0), true);
  }
  if (hv && ImGui::IsMouseDoubleClicked(0)) pui::setParamValue(a, t, kp, 0.0, false);
  if (hv && ImGui::GetIO().MouseWheel != 0) pui::setParamValue(a, t, kp, std::clamp(v - ImGui::GetIO().MouseWheel * 0.02, -1.0, 1.0), true);
  // range: click steps through 4 / 8 / 16 / 50 %
  static const char* const kRanges[4] = {"\xC2\xB1" "4%", "\xC2\xB1" "8%", "\xC2\xB1" "16%", "\xC2\xB1" "50%"};
  std::snprintf(id, sizeof id, "##prng%d", d);
  if (button(id, ImVec2(p0.x, p1.y - 24), ImVec2(p1.x - p0.x, 24), kRanges[std::clamp(range, 0, 3)], false,
             i18n::N_("Pitch fader range: click for the next one")))
    pui::setParamValue(a, t, kr, (range + 1) % RangeCount, false);
}

// ------------------------------------------------------------------ sync / key / modes / motor
void drawControls(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = p1.x - p0.x;
  float y = p0.y;
  char id[24];
  // the sync lock: locked = in time with the project (or the other deck); click to unlock
  {
    const int ks = deckParam(d, DSync);
    const bool sync = on(a, t, ks);
    const ImVec2 q0(p0.x, y), q1(p1.x, y + 42);
    std::snprintf(id, sizeof id, "##psync%d", d);
    ImGui::SetCursorScreenPos(q0);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, 42));
    const bool hv = ImGui::IsItemHovered();
    const bool toDeck = static_cast<int>(std::lround(param(a, t, PSyncTo))) == SyncDeck;
    if (hv)
      ui::Tooltip(sync ? i18n::tr("Locked: tempo and beat follow the project (or the other deck, see Sync to). Click the padlock to let the track run at its own tempo.")
                       : i18n::tr("Running at the track's own tempo (pitch fader). Click to lock it again."));
    ImU32 bg = hv ? P.surface700 : P.surface800;
    if (sync) bg = theme::mix(bg, hot(), 0.06f);
    skin::raisedFace(dl, q0, q1, bg, theme::kRadius);
    dl->AddRect(q0, q1, sync ? theme::withAlpha(hot(), 0.6f) : P.line, theme::kRadius);
    const ImU32 fg = sync ? hot() : P.textDim;
    padlock(dl, ImVec2(q0.x + 18, q0.y + 19), 17, fg, !sync);
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(q0.x + 34, q0.y + 6), sync ? hot() : P.text, i18n::tr("SYNC"));
    const char* sub = !sync ? i18n::tr("own tempo") : lv.following ? (d == 0 ? i18n::tr("to deck B") : i18n::tr("to deck A")) : toDeck ? i18n::tr("lead") : i18n::tr("project");
    theme::textEllipsis(dl, a.fontRegular, theme::kFontLabel, ImVec2(q0.x + 34, q0.y + 22), w - 38, P.textDim, sub);
    if (clicked) pui::setParamValue(a, t, ks, sync ? 0.0 : 1.0, false);
    y = q1.y + 6;
  }
  // key lock
  {
    const int kk = deckParam(d, DKeyLock);
    const bool kl = on(a, t, kk);
    std::snprintf(id, sizeof id, "##pkey%d", d);
    if (button(id, ImVec2(p0.x, y), ImVec2(w, 28), i18n::N_("KEY LOCK"), kl, i18n::N_("Keep the track's pitch when its tempo changes (off while you scratch)")))
      flip(a, t, kk);
    look::led(dl, ImVec2(p0.x + 10, y + 14), 2.5f, kl && lv.keylockActive, kl && lv.keylockActive ? 1.0f : 0.0f);
    y += 34;
  }
  // modes
  struct Mode {
    const char* label;
    int param;
    const char* hint;
  };
  static const Mode kModes[3] = {
      {i18n::N_("VINYL"), DVinyl, i18n::N_("On: touching the jog wheel's top grabs the record. Off: it nudges the tempo, like a CD player.")},
      {i18n::N_("SLIP"), DSlip, i18n::N_("The track keeps running underneath while you scratch, loop or reverse; let go and it carries on in time.")},
      {i18n::N_("QUANT"), DQuantize, i18n::N_("Quantize: cues, loops and jumps land on the beat")}};
  for (int k = 0; k < 3; ++k) {
    const int kp = deckParam(d, kModes[k].param);
    std::snprintf(id, sizeof id, "##pmode%d%d", d, k);
    if (button(id, ImVec2(p0.x, y), ImVec2(w, 24), kModes[k].label, on(a, t, kp), kModes[k].hint)) flip(a, t, kp);
    y += 28;
  }
  // the motor (start, brake), then the hand (feel, jog sensitivity)
  const float kh = pui::knobCellHeight(pui::KnobSmall);
  y = std::max(y + 4, p1.y - 2 * kh);
  const float cell = std::floor(w / 2);
  pui::knobAt(a, t, deckParam(d, DStartTime), ImVec2(p0.x, y), i18n::tr("Start"), pui::KnobSmall, cell);
  pui::knobAt(a, t, deckParam(d, DStopTime), ImVec2(p0.x + cell, y), i18n::tr("Brake"), pui::KnobSmall, cell);
  pui::knobAt(a, t, deckParam(d, DFeel), ImVec2(p0.x, y + kh), i18n::tr("Hand"), pui::KnobSmall, cell);
  pui::knobAt(a, t, deckParam(d, DJog), ImVec2(p0.x + cell, y + kh), i18n::tr("Jog"), pui::KnobSmall, cell);
  (void)e;
}

// ------------------------------------------------------------------ hot cues
void drawPads(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DeckUi& u = e.d[static_cast<size_t>(d)];
  const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
  const float gap = 6;
  const float pw = std::floor((p1.x - p0.x - 3 * gap) / 4), ph = std::floor((p1.y - p0.y - gap) / 2);
  const float dt = ImGui::GetIO().DeltaTime;
  for (int k = 0; k < kHotCues; ++k) {
    const ImVec2 q0(p0.x + (k % 4) * (pw + gap), p0.y + (k / 4) * (ph + gap)), q1(q0.x + pw, q0.y + ph);
    const double f = lv.hot[static_cast<size_t>(k)];
    const bool set = f >= 0;
    char id[16];
    std::snprintf(id, sizeof id, "##phot%d%d", d, k);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton(id, ImVec2(pw, ph), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
    const bool clear = ImGui::GetIO().KeyShift || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (ImGui::IsItemActivated()) {
      u.padGlow[k] = 1.0;
      if (clear && set) press(a, t, d, CtlHotClear1 + k, true), press(a, t, d, CtlHotClear1 + k, false);
      else if (!clear) press(a, t, d, CtlHot1 + k, true);
    }
    if (ImGui::IsItemDeactivated() && !clear) press(a, t, d, CtlHot1 + k, false);
    if (hv) ui::Tooltip(set ? i18n::tr("Jump to this hot cue (hold while stopped to preview). Shift+click or right-click clears it.") : i18n::tr("Set a hot cue here"));
    u.padGlow[k] = std::max(0.0, u.padGlow[k] - dt * 3.0);
    const ImU32 hc = hotColor(k);
    const int state = (hv ? look::Hover : 0) | (ac ? look::Active : 0) | (set ? look::On : 0);
    look::pad(dl, q0, q1, "", set ? std::max(0.5f, static_cast<float>(u.padGlow[k])) : std::max(0.06f, static_cast<float>(u.padGlow[k]) * 0.5f), state, hc);
    if (!set) dl->AddRectFilled(ImVec2(q0.x + 6, q1.y - 5), ImVec2(q1.x - 6, q1.y - 3), theme::withAlpha(hc, 0.45f), 1);
    char nb[4];
    std::snprintf(nb, sizeof nb, "%d", k + 1);
    const ImU32 fg = set ? theme::contrastText(theme::mix(P.surface700, hc, 0.7f)) : theme::withAlpha(hc, 0.95f);
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(q0.x + 7, q0.y + 4), fg, nb);
    if (set && pw > 54) {
      const std::string tm = fmtTime(f / srcRateOf(m));
      textR(dl, a.fontMono, theme::kFontLabel, ImVec2(q1.x - 6, q1.y - 16), theme::withAlpha(fg, 0.85f), tm.c_str());
    }
  }
}

// ------------------------------------------------------------------ loops
void drawLoops(App& a, const PluginTarget& t, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const int kl = deckParam(d, DLoopSize);
  const int size = static_cast<int>(std::lround(param(a, t, kl)));
  // proportions: size stepper | in out auto | jumps
  const float u = std::min(1.0f, w / 400.0f);
  const float sb = std::round(30 * u), sf = std::round(64 * u), lb = std::round(48 * u), jb = std::round(36 * u), g = 4, G = std::round(12 * u);
  float x = p0.x;
  char id[24];
  std::snprintf(id, sizeof id, "##phalf%d", d);
  if (button(id, ImVec2(x, p0.y), ImVec2(sb, h), "", false, i18n::N_("Halve the loop size (and a running loop)"), static_cast<int>(ui::Icon::ChevronLeft))) {
    if (lv.loopOn) press(a, t, d, CtlLoopHalve, true);
    pui::setParamValue(a, t, kl, std::max(0, size - 1), false);
  }
  x += sb + g;
  {
    const ImVec2 f0(x, p0.y), f1(x + sf, p1.y);
    skin::insetField(dl, f0, f1, P.field, theme::kRadius);
    textC(dl, a.fontMono, theme::kFontNumber, ImVec2((f0.x + f1.x) * 0.5f, (f0.y + f1.y) * 0.5f), lv.loopOn ? hot() : P.text, loopLabel(size));
    (void)t.pid(kl);
    x += sf + g;
  }
  std::snprintf(id, sizeof id, "##pdouble%d", d);
  if (button(id, ImVec2(x, p0.y), ImVec2(sb, h), "", false, i18n::N_("Double the loop size (and a running loop)"), static_cast<int>(ui::Icon::Right))) {
    if (lv.loopOn) press(a, t, d, CtlLoopDouble, true);
    pui::setParamValue(a, t, kl, std::min(kLoopSizes - 1, size + 1), false);
  }
  x += sb + G;
  std::snprintf(id, sizeof id, "##pin%d", d);
  if (button(id, ImVec2(x, p0.y), ImVec2(lb, h), i18n::N_("IN"), false, i18n::N_("Loop in: mark the start here"))) press(a, t, d, CtlLoopIn, true);
  x += lb + g;
  std::snprintf(id, sizeof id, "##pout%d", d);
  if (button(id, ImVec2(x, p0.y), ImVec2(lb, h), i18n::N_("OUT"), false, i18n::N_("Loop out: loop from the in point to here (again: exit)")))
    press(a, t, d, CtlLoopOut, true);
  x += lb + g;
  std::snprintf(id, sizeof id, "##pauto%d", d);
  if (button(id, ImVec2(x, p0.y), ImVec2(lb, h), "", lv.loopOn, i18n::N_("Loop the next beats (the size beside it); again to exit"),
             static_cast<int>(ui::Icon::LoopArrows)))
    press(a, t, d, CtlLoopAuto, true);
  // beat jumps, at the far end
  float jx = p1.x - 2 * jb - g;
  std::snprintf(id, sizeof id, "##pjb%d", d);
  if (button(id, ImVec2(jx, p0.y), ImVec2(jb, h), "", false, i18n::N_("Jump back by the loop size"), static_cast<int>(ui::Icon::Left)))
    press(a, t, d, CtlJumpBack, true);
  jx += jb + g;
  std::snprintf(id, sizeof id, "##pjf%d", d);
  if (button(id, ImVec2(jx, p0.y), ImVec2(jb, h), "", false, i18n::N_("Jump ahead by the loop size"), static_cast<int>(ui::Icon::Right)))
    press(a, t, d, CtlJumpFwd, true);
  if (w > 380) caps(dl, a, ImVec2(p1.x - 2 * jb - g - 40, p0.y + h * 0.5f - 7), P.textFaint, i18n::tr("JUMP"));
}

}  // namespace

float wavesHeight(bool two) { return two ? 2 * (kOvH + kRowGap + kWaveH) + kPhaseH + 2 * kRowGap : kOvH + kRowGap + kWaveH; }

void drawWaves(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float W, const LiveView* lv) {
  const bool two = e.m.twoDecks();
  float y = p0.y;
  auto lane = [&](int d, bool overviewFirst) {
    const ImVec2 o0(p0.x, y);
    if (!deckReady(e, d)) {
      drawEmptyLane(a, t, e, d, o0, ImVec2(p0.x + W, y + kOvH + kRowGap + kWaveH));
      y += kOvH + kRowGap + kWaveH;
      return;
    }
    if (overviewFirst) {
      drawOverview(a, t, e, d, o0, ImVec2(p0.x + W, y + kOvH), lv[d]);
      y += kOvH + kRowGap;
      drawWave(a, t, e, d, ImVec2(p0.x, y), ImVec2(p0.x + W, y + kWaveH), lv[d]);
      y += kWaveH;
    } else {
      drawWave(a, t, e, d, o0, ImVec2(p0.x + W, y + kWaveH), lv[d]);
      y += kWaveH + kRowGap;
      drawOverview(a, t, e, d, ImVec2(p0.x, y), ImVec2(p0.x + W, y + kOvH), lv[d]);
      y += kOvH;
    }
  };
  lane(0, true);
  if (!two) return;
  y += kRowGap;
  drawPhase(a, e, ImVec2(p0.x, y), ImVec2(p0.x + W, y + kPhaseH), lv);
  y += kPhaseH + kRowGap;
  lane(1, false);
}

float deckPanelHeight() { return kPad + kHeadH + kGap + kBodyH + kGap + kPerfH + kGap + kLoopH + kPad; }

void drawDeck(App& a, const PluginTarget& t, Ed& e, int d, ImVec2 p0, ImVec2 p1, const LiveView& lv, bool mirror) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const DeckUi& u = e.d[static_cast<size_t>(d)];
  const DeckModel& m = e.m.deck[static_cast<size_t>(d)];
  dl->AddRectFilled(p0, p1, P.surface800, theme::kRadiusLg);
  dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
  const float x0 = p0.x + kPad, x1 = p1.x - kPad, W = x1 - x0;
  float y = p0.y + kPad;
  // ---- header: badge, track and facts; time left and BPM
  {
    const ImVec2 b0(x0, y + 4), b1(x0 + 30, y + 34);
    dl->AddRectFilled(b0, b1, deckColor(d), theme::kRadius + 1);
    textC(dl, a.fontBold, 17, ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), theme::rgb(0x101114), d == 0 ? "A" : "B");
    const bool wide = W > 400;
    const float readW = wide ? 196.0f : 96.0f;
    const float tx = b1.x + 10, tw = W - (tx - x0) - readW - 36;
    const std::string name = u.path.empty() ? i18n::tr("No track") : fs::path(u.path).stem().string();
    theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(tx, y + 2), tw, u.path.empty() ? P.textDim : P.text, name.c_str());
    std::string facts;
    char b[96];
    if (u.path.empty()) {
      facts = i18n::tr("Drop a track here, or load one");
    } else if (!deckReady(e, d) || !m.analysed) {
      facts = i18n::tr("Finding the tempo and the beats...");
    } else {
      std::snprintf(b, sizeof b, "%.2f %s", bpmOf(m), i18n::tr("BPM"));
      facts = b;
      const std::string k = keyName(m.grid.key, m.grid.minor);
      if (!k.empty()) facts += "  \xC2\xB7  " + k;
      facts += "  \xC2\xB7  " + fmtTime(static_cast<double>(m.srcFrames) / srcRateOf(m));
    }
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(tx, y + 22), tw, P.textDim, facts.c_str());
    // load
    char id[16];
    std::snprintf(id, sizeof id, "##pld%d", d);
    if (button(id, ImVec2(x1 - readW - 30, y + 6), ImVec2(26, 26), "", false, d == 0 ? i18n::N_("Load a track onto deck A") : i18n::N_("Load a track onto deck B"),
               static_cast<int>(ui::Icon::Open)))
      openLoadDialog(a, t, d);
    // readouts: time left, then the effective BPM with the pitch offset
    const float rx = x1;
    const double bpm = bpmOf(m);
    const double shown = lv.loaded && lv.tempo > 0 ? lv.tempo : bpm;
    std::snprintf(b, sizeof b, "%.2f", u.path.empty() ? 0.0 : shown);
    textR(dl, a.fontMonoBold, 20, ImVec2(rx, y + 1), u.path.empty() ? P.textFaint : P.text, u.path.empty() ? "--.--" : b);
    const double pct = bpm > 0 && !u.path.empty() ? (shown / bpm - 1.0) * 100.0 : 0.0;
    std::snprintf(b, sizeof b, "%s%.2f%%", pct >= 0 ? "+" : "", pct);
    textR(dl, a.fontMono, theme::kFontLabel, ImVec2(rx, y + 26), std::fabs(pct) < 0.005 ? P.textFaint : P.textDim, b);
    if (wide) {
      const double rate = lv.tempo > 0 && bpm > 0 ? lv.tempo / bpm : 1.0;
      const double left = lv.loaded ? std::max(0.0, static_cast<double>(m.srcFrames) - lv.pos) / srcRateOf(m) / std::max(0.05, rate) : 0.0;
      const std::string tl = lv.loaded ? fmtTime(-left) : std::string("-:--.-");
      textR(dl, a.fontMonoBold, 20, ImVec2(rx - 92, y + 1), lv.loaded && left < 30 && left > 0 ? P.danger : (lv.loaded ? P.text : P.textFaint), tl.c_str());
      textR(dl, a.fontMono, theme::kFontLabel, ImVec2(rx - 92, y + 26), P.textFaint, i18n::tr("REMAIN"));
    }
    y += kHeadH;
    dl->AddLine(ImVec2(x0, y + kGap * 0.5f), ImVec2(x1, y + kGap * 0.5f), theme::withAlpha(P.line, 0.7f));
    y += kGap;
  }
  // ---- body: pitch | platter | controls (mirrored on deck B)
  {
    const float pitchW = 46, ctlW = std::clamp(std::floor(W * 0.27f), 104.0f, 124.0f), g = 10;
    const float platArea = W - pitchW - ctlW - 2 * g;
    const float R = std::floor(std::min(platArea, kBodyH - 6) * 0.5f) - 8;
    // group the three and centre them when the deck is wide (one-deck layout)
    const float group = pitchW + g + 2 * (R + 8) + g + ctlW;
    const float gx = x0 + std::max(0.0f, std::floor((W - group) * 0.5f));
    const float px = mirror ? gx + group - pitchW : gx;
    const float cxl = mirror ? gx : gx + group - ctlW;
    const float platX = mirror ? gx + ctlW + g : gx + pitchW + g;
    drawPitch(a, t, d, ImVec2(px, y), ImVec2(px + pitchW, y + kBodyH));
    drawPlatter(a, t, e, d, ImVec2(std::round(platX + R + 8), std::round(y + kBodyH * 0.5f)), R, lv);
    drawControls(a, t, e, d, ImVec2(cxl, y), ImVec2(cxl + ctlW, y + kBodyH), lv);
    y += kBodyH + kGap;
  }
  // ---- CUE / PLAY and the hot cue pads
  {
    const float r = std::min(26.0f, kPerfH * 0.5f - 6);
    const float bw = 2 * (2 * r + 8);
    roundButton(a, t, d, d == 0 ? "##pcue0" : "##pcue1", ImVec2(x0 + r + 2, y + kPerfH * 0.5f), r, i18n::tr("CUE"), CtlCue, lv.cueLit, cueColor(),
                i18n::N_("Stopped: set the cue here, or hold to preview from it. Playing: back to the cue."));
    roundButton(a, t, d, d == 0 ? "##pplay0" : "##pplay1", ImVec2(x0 + 3 * r + 12, y + kPerfH * 0.5f), r, i18n::tr("PLAY"), CtlPlay, lv.playing,
                theme::content(0x7FD45B), i18n::N_("Start or stop the motor (the record spins up and brakes like a turntable)"));
    drawPads(a, t, e, d, ImVec2(x0 + bw + 8, y), ImVec2(x1, y + kPerfH), lv);
    y += kPerfH + kGap;
  }
  // ---- loop row
  drawLoops(a, t, d, ImVec2(x0, y), ImVec2(x1, y + kLoopH), lv);
}

}  // namespace gl::packs::platter::pv
