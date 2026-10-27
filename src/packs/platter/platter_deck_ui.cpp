// Gridline — "platter" pack: the deck. Rows, top to bottom: the whole track
// (click to jump), the scrolling waveform with the beat grid, cues and loop
// (drag it to scratch, scroll to zoom), then transport and tempo, the platter
// (drag it round to scratch), the pitch fader, the mixer strip, then hot cue
// pads, loops and beat jumps, the crossfader, the motor and hand feel, and the
// recorded takes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

// Row heights.
constexpr float kOverviewH = 34, kWaveH = 112, kDeckH = 300, kBottomH = 124, kTakesH = 36, kGap = 10;

double srcRateOf(const Ed& e) { return e.m.srcRate > 0 ? e.m.srcRate : 44100.0; }
bool ready(const Ed& e) { return e.hasModel && e.job && e.job->state == 1 && e.job->peaks.frames > 0; }
double param(App& a, const PluginTarget& t, int k) { return pui::paramValue(a, t, k); }
bool on(App& a, const PluginTarget& t, int k) { return param(a, t, k) >= 0.5; }
void flip(App& a, const PluginTarget& t, int k) { pui::setParamValue(a, t, k, on(a, t, k) ? 0.0 : 1.0, false); }

std::string noteName(int midi) {
  static const char* const n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  midi = std::clamp(midi, 0, 127);
  return std::string(n[midi % 12]) + std::to_string(midi / 12 - 1);
}

// Peak column over [f0, f1) source frames: min, max, rms.
void column(const seltmann::Peaks& pk, double f0, double f1, float& mn, float& mx, float& rms) {
  mn = mx = rms = 0;
  const auto nb = static_cast<int64_t>(pk.mx.size());
  if (nb == 0) return;
  int64_t b0 = static_cast<int64_t>(std::floor(f0 / seltmann::kPeakBlock));
  int64_t b1 = std::max(b0 + 1, static_cast<int64_t>(std::ceil(f1 / seltmann::kPeakBlock)));
  if (b1 <= 0 || b0 >= nb) return;
  b0 = std::max<int64_t>(0, b0), b1 = std::min(nb, b1);
  double sq = 0;
  for (int64_t b = b0; b < b1; ++b) {
    mn = std::min(mn, pk.mn[static_cast<size_t>(b)]);
    mx = std::max(mx, pk.mx[static_cast<size_t>(b)]);
    sq += static_cast<double>(pk.rms[static_cast<size_t>(b)]) * pk.rms[static_cast<size_t>(b)];
  }
  rms = static_cast<float>(std::sqrt(sq / static_cast<double>(b1 - b0)));
}

// ------------------------------------------------------------------ empty / analysing
void drawDrop(App& a, const PluginTarget& t, Ed& e, ImVec2 z0, ImVec2 z1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  const bool dragging = pl && (pl->IsDataType("GL_FILE") || pl->IsDataType("GL_FILES") || pl->IsDataType("GL_ITEM") || pl->IsDataType("GL_CLIPSRC"));
  const bool over = dragging && ImGui::IsMouseHoveringRect(z0, z1, false);
  skin::insetField(dl, z0, z1, over ? theme::mix(P.well, accent(), 0.08f) : P.well, theme::kRadiusLg);
  const ImU32 dash = theme::withAlpha(accent(), over ? 0.95f : dragging ? 0.7f : 0.38f);
  const float in = 10, seg = 9, gap = 7;
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
  const ImVec2 c((z0.x + z1.x) * 0.5f, (z0.y + z1.y) * 0.5f);
  const bool analysing = !e.path.empty() && e.job && e.job->state == 0;
  const bool failed = !e.path.empty() && e.job && e.job->state == 2;
  if (analysing) {
    const float p = std::clamp(e.job->progress.load(), 0.0f, 1.0f);
    textC(dl, a.fontBold, 18, ImVec2(c.x, c.y - 24), P.text, fs::path(e.path).filename().string().c_str());
    textC(dl, a.fontRegular, theme::kFontCaption, ImVec2(c.x, c.y), P.textDim,
          p < 0.5f ? i18n::tr("Listening for the beats") : i18n::tr("Laying the beat grid"));
    const float bw = 280;
    const ImVec2 b0(c.x - bw * 0.5f, c.y + 18), b1(c.x + bw * 0.5f, c.y + 23);
    dl->AddRectFilled(b0, b1, P.surface900, 2.5f);
    dl->AddRectFilled(b0, ImVec2(b0.x + bw * p, b1.y), accent(), 2.5f);
    return;
  }
  if (over) {
    textC(dl, a.fontBold, 20, c, P.text, i18n::tr("Release to load the track"));
    return;
  }
  textC(dl, a.fontBold, 20, ImVec2(c.x, c.y - 30), P.text, failed ? i18n::tr("This file cannot be read") : i18n::tr("Drop a track on the deck"));
  textC(dl, a.fontRegular, theme::kFontCaption, ImVec2(c.x, c.y - 6), P.textDim,
        failed ? i18n::tr("Try another file: WAV, AIFF, FLAC, MP3 or OGG.")
               : i18n::tr("Its tempo and beats are found and it plays in time with the project. The padlock lets it run free."));
  if (button("##pload2", ImVec2(c.x - 70, c.y + 14), ImVec2(140, 30), i18n::N_("Load a track..."), false, nullptr,
             static_cast<int>(ui::Icon::Open), true))
    openLoadDialog(a, t);
}

// ------------------------------------------------------------------ whole track
void drawOverview(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const seltmann::Peaks& pk = e.job->peaks;
  const float w = p1.x - p0.x, h = p1.y - p0.y, cy = (p0.y + p1.y) * 0.5f;
  const double total = static_cast<double>(pk.frames);
  const double fpp = total / std::max(1.0f, w - 4);
  const float playX = p0.x + 2 + static_cast<float>(lv.pos / fpp);
  float peak = 1e-4f;
  for (float v : pk.mx) peak = std::max(peak, v);
  for (float x = 0; x < w - 4; x += 1) {
    float mn, mx, rms;
    column(pk, x * fpp, (x + 1) * fpp, mn, mx, rms);
    const float s = (h * 0.5f - 3) / peak;
    const bool played = p0.x + 2 + x < playX;
    const ImU32 col = played ? theme::withAlpha(P.textFaint, 0.6f) : theme::mix(accent(), P.textDim, 0.35f);
    dl->AddLine(ImVec2(p0.x + 2 + x + 0.5f, cy - mx * s), ImVec2(p0.x + 2 + x + 0.5f, cy - mn * s + 1), col);
  }
  auto xOf = [&](double f) { return p0.x + 2 + static_cast<float>(f / fpp); };
  if (lv.loopIn >= 0 && lv.loopOut > lv.loopIn)
    dl->AddRectFilled(ImVec2(xOf(lv.loopIn), p0.y + 1), ImVec2(xOf(lv.loopOut), p1.y - 1), theme::withAlpha(accent(), lv.loopOn ? 0.28f : 0.10f));
  for (int k = 0; k < kHotCues; ++k)
    if (lv.hot[static_cast<size_t>(k)] >= 0) {
      const float x = xOf(lv.hot[static_cast<size_t>(k)]);
      dl->AddRectFilled(ImVec2(x - 1, p0.y + 1), ImVec2(x + 1, p0.y + 7), hotColor(k));
    }
  {
    const float x = xOf(lv.cue);
    dl->AddTriangleFilled(ImVec2(x - 4, p1.y - 1), ImVec2(x + 4, p1.y - 1), ImVec2(x, p1.y - 6), theme::content(0xF2A73B));
  }
  dl->AddLine(ImVec2(playX, p0.y + 1), ImVec2(playX, p1.y - 1), P.playhead, 2);
  // click / drag: jump there
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##pover", ImVec2(w, h));
  if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("The whole track: click to jump"));
  if (ImGui::IsItemActive() && (ImGui::IsItemActivated() || ImGui::IsMouseDragging(0, 2))) {
    const double f = std::clamp((ImGui::GetIO().MousePos.x - p0.x - 2) * fpp, 0.0, total);
    send(a, t, CmdSeek, 0, f);
  }
  // time left
  // time left at the deck's tempo (not the platter's speed of the moment: a scratch or a throw does not change it)
  const double rate = lv.tempo > 0 && bpmOf(e.m) > 0 ? lv.tempo / bpmOf(e.m) : 1.0;
  const double left = std::max(0.0, total - lv.pos) / srcRateOf(e) / std::max(0.05, rate);
  const std::string tl = fmtTime(-left);
  const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, tl.c_str());
  dl->AddRectFilled(ImVec2(p1.x - ts.x - 12, p0.y + 3), ImVec2(p1.x - 3, p0.y + 16), theme::withAlpha(P.well, 0.85f), 3);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p1.x - ts.x - 7, p0.y + 3), P.text, tl.c_str());
}

// ------------------------------------------------------------------ scrolling waveform
void drawWave(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const seltmann::Peaks& pk = e.job->peaks;
  const float w = p1.x - p0.x, h = p1.y - p0.y, cy = (p0.y + p1.y) * 0.5f;
  const float cx = std::round(p0.x + w * 0.5f);
  const double sr = srcRateOf(e);
  const double fpp = e.zoomSeconds * sr / w;
  const double pos = lv.pos;
  auto xOf = [&](double f) { return cx + static_cast<float>((f - pos) / fpp); };
  dl->PushClipRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), true);
  // loop
  if (lv.loopIn >= 0 && lv.loopOut > lv.loopIn)
    dl->AddRectFilled(ImVec2(xOf(lv.loopIn), p0.y + 1), ImVec2(xOf(lv.loopOut), p1.y - 1), theme::withAlpha(accent(), lv.loopOn ? 0.22f : 0.08f));
  // beat grid (bars brighter, numbered)
  const Model& m = e.m;
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double b0 = std::ceil(beatAt(m, pos - (cx - p0.x) * fpp)), b1 = std::floor(beatAt(m, pos + (p1.x - cx) * fpp));
  for (double b = b0; b <= b1 && b - b0 < 400; b += 1) {
    const float x = std::round(xOf(frameAt(m, b)));
    const int rel = static_cast<int>(std::lround(b)) - m.grid.bar1;
    const bool bar = ((rel % bpb) + bpb) % bpb == 0;
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), bar ? theme::withAlpha(P.text, 0.30f) : theme::withAlpha(P.text, 0.10f), 1);
    if (bar) {
      char nb[16];
      std::snprintf(nb, sizeof nb, "%d", rel / bpb + 1);
      dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(x + 3, p0.y + 2), theme::withAlpha(P.text, 0.45f), nb);
    }
  }
  // the waveform: outline in the accent, body by loudness
  float peak = 1e-4f;
  for (float v : pk.mx) peak = std::max(peak, v);
  const float s = (h * 0.5f - 6) / peak;
  for (float x = p0.x + 1; x < p1.x - 1; x += 1) {
    const double f0 = pos + (x - cx) * fpp;
    if (f0 + fpp < 0 || f0 > static_cast<double>(pk.frames)) continue;
    float mn, mx, rms;
    column(pk, f0, f0 + fpp, mn, mx, rms);
    const bool past = x < cx;
    const ImU32 body = past ? theme::mix(accent(), P.well, 0.45f) : accent();
    dl->AddLine(ImVec2(x + 0.5f, cy - mx * s), ImVec2(x + 0.5f, cy - mn * s + 1), theme::withAlpha(body, past ? 0.55f : 0.85f));
    const float r = rms * s * 1.4f;
    dl->AddLine(ImVec2(x + 0.5f, cy - r), ImVec2(x + 0.5f, cy + r + 1), theme::withAlpha(P.paper, past ? 0.25f : 0.45f));
  }
  // cue + hot cue flags
  {
    const float x = std::round(xOf(lv.cue));
    const ImU32 cc = theme::content(0xF2A73B);
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), theme::withAlpha(cc, 0.8f), 1.5f);
    dl->AddTriangleFilled(ImVec2(x - 6, p1.y - 1), ImVec2(x + 6, p1.y - 1), ImVec2(x, p1.y - 9), cc);
  }
  for (int k = 0; k < kHotCues; ++k) {
    const double f = lv.hot[static_cast<size_t>(k)];
    if (f < 0) continue;
    const float x = std::round(xOf(f));
    const ImU32 hc = hotColor(k);
    dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), theme::withAlpha(hc, 0.8f), 1.5f);
    dl->AddRectFilled(ImVec2(x, p0.y + 14), ImVec2(x + 15, p0.y + 28), hc, 2, ImDrawFlags_RoundCornersRight);
    char nb[4];
    std::snprintf(nb, sizeof nb, "%d", k + 1);
    textC(dl, a.fontBold, theme::kFontLabel, ImVec2(x + 7.5f, p0.y + 21), theme::contrastText(hc), nb);
  }
  if (lv.slipping && lv.slipPos >= 0) {
    const float x = std::round(xOf(lv.slipPos));
    for (float y = p0.y + 2; y < p1.y - 2; y += 6) dl->AddLine(ImVec2(x, y), ImVec2(x, y + 3), P.text, 1.5f);
  }
  dl->PopClipRect();
  // playhead
  dl->AddLine(ImVec2(cx, p0.y + 1), ImVec2(cx, p1.y - 1), P.playhead, 2);
  dl->AddTriangleFilled(ImVec2(cx - 6, p0.y + 1), ImVec2(cx + 6, p0.y + 1), ImVec2(cx, p0.y + 8), P.playhead);
  dl->AddTriangleFilled(ImVec2(cx - 6, p1.y - 1), ImVec2(cx + 6, p1.y - 1), ImVec2(cx, p1.y - 8), P.playhead);
  // drag = the hand on the record; wheel = zoom
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##pwave", ImVec2(w, h));
  const bool hv = ImGui::IsItemHovered();
  if (hv && !ImGui::IsItemActive()) ui::Tooltip(i18n::tr("Drag the waveform to scratch. Scroll to zoom."));
  if (hv && ImGui::GetIO().MouseWheel != 0) e.zoomSeconds = std::clamp(e.zoomSeconds * std::pow(0.85, ImGui::GetIO().MouseWheel), 1.0, 32.0);
  if (ImGui::IsItemActivated()) {
    e.waveDrag = true;
    e.waveGrabX = ImGui::GetIO().MousePos.x;
    send(a, t, CmdHandDown);
  }
  if (e.waveDrag && ImGui::IsItemActive()) {
    const double frames = -(ImGui::GetIO().MousePos.x - e.waveGrabX) * fpp;
    send(a, t, CmdHandMove, 0, frames / (sr * kSecondsPerRev));
  }
  if (e.waveDrag && ImGui::IsItemDeactivated()) {
    e.waveDrag = false;
    send(a, t, CmdHandUp);
  }
  if (hv || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
}

// ------------------------------------------------------------------ transport + tempo
void bigButton(App& a, const PluginTarget& t, const char* id, ImVec2 c, float r, const char* label, int control, bool lit, ImU32 hue,
               const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
  ImGui::InvisibleButton(id, ImVec2(2 * r, 2 * r));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (ImGui::IsItemActivated()) press(a, t, control, true);
  if (ImGui::IsItemDeactivated()) press(a, t, control, false);
  if (hv) ui::Tooltip(i18n::tr(hint));
  skin::gradientCircle(dl, ImVec2(c.x, c.y + 2), r, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.35f));
  skin::gradientCircle(dl, c, r, ac ? P.surface600 : (hv ? theme::mix(P.surface600, P.paper, 0.06f) : P.surface700), P.surface900);
  dl->AddCircle(c, r - 0.5f, lit ? hue : theme::withAlpha(hue, 0.55f), 48, lit ? 2.5f : 1.5f);
  if (lit) dl->AddCircle(c, r + 3, theme::withAlpha(hue, 0.25f), 48, 3);
  // the face: CUE in letters, PLAY as the play / pause glyph (the hardware's own markings)
  const ImU32 g = lit ? hue : P.text;
  if (control == CtlPlay) {
    dl->AddTriangleFilled(ImVec2(c.x - 11, c.y - 9), ImVec2(c.x - 11, c.y + 9), ImVec2(c.x + 1, c.y), g);
    dl->AddRectFilled(ImVec2(c.x + 4, c.y - 9), ImVec2(c.x + 7, c.y + 9), g);
    dl->AddRectFilled(ImVec2(c.x + 10, c.y - 9), ImVec2(c.x + 13, c.y + 9), g);
  } else {
    textC(dl, a.fontBold, 17, c, g, label);
  }
}

void drawTransport(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("TEMPO"), nullptr);
  const float w = p1.x - p0.x - 20;
  const bool sync = on(a, t, PSync);
  // tempo display
  const ImVec2 d0(c.x, c.y), d1(c.x + w, c.y + 72);
  skin::insetField(dl, d0, d1, P.field, theme::kRadius);
  const double bpm = e.hasModel ? bpmOf(e.m) : 0.0;
  const double shown = lv.valid && lv.loaded ? lv.tempo : bpm;
  char b[48];
  std::snprintf(b, sizeof b, "%.2f", shown);
  textC(dl, a.fontBold, 30, ImVec2((d0.x + d1.x) * 0.5f, d0.y + 24), e.hasModel ? P.text : P.textFaint, e.hasModel ? b : "--.--");
  const double pct = bpm > 0 ? (shown / bpm - 1.0) * 100.0 : 0.0;
  std::snprintf(b, sizeof b, "%s%.2f %%", pct >= 0 ? "+" : "", pct);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(d0.x + 8, d1.y - 18), std::fabs(pct) < 0.005 ? P.textFaint : P.text, b);
  std::snprintf(b, sizeof b, "%s %.1f", i18n::tr("TRACK"), bpm);
  textR(dl, a.fontMono, theme::kFontLabel, ImVec2(d1.x - 8, d1.y - 18), P.textDim, b);
  // the sync lock: locked = in time with the project; click the padlock to unlock
  float y = d1.y + 10;
  {
    const ImVec2 q0(c.x, y), q1(c.x + w, y + 38);
    ImGui::SetCursorScreenPos(q0);
    const bool clicked = ImGui::InvisibleButton("##psync", ImVec2(w, 38));
    const bool hv = ImGui::IsItemHovered();
    if (hv)
      ui::Tooltip(sync ? i18n::tr("Locked to the project's tempo and beat. Click the padlock to let the track run at its own tempo.")
                       : i18n::tr("Running at the track's own tempo (pitch fader). Click to lock it to the project again."));
    const ImU32 bg = sync ? theme::mix(accent(), P.surface700, hv ? 0.0f : 0.12f) : (hv ? P.surface700 : P.surface800);
    skin::raisedFace(dl, q0, q1, bg, theme::kRadius);
    dl->AddRect(q0, q1, sync ? theme::mix(accent(), P.ink, 0.3f) : P.line, theme::kRadius);
    const ImU32 fg = sync ? theme::contrastText(bg) : P.text;
    padlock(dl, ImVec2(q0.x + 22, q0.y + 18), 20, fg, !sync);
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(q0.x + 42, q0.y + 5), fg, sync ? i18n::tr("SYNC") : i18n::tr("UNLOCKED"));
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(q0.x + 42, q0.y + 20), theme::withAlpha(fg, 0.75f),
                sync ? i18n::tr("follows the project") : i18n::tr("own tempo"));
    if (clicked) pui::setParamValue(a, t, PSync, sync ? 0.0 : 1.0, false);
    y = q1.y + 8;
  }
  // key lock
  {
    const bool kl = on(a, t, PKeyLock);
    if (button("##pkey", ImVec2(c.x, y), ImVec2(w, 28), kl ? i18n::N_("Key lock on") : i18n::N_("Key lock off"), kl,
               i18n::N_("Keep the track's pitch when its tempo changes (off while you scratch)"), static_cast<int>(ui::Icon::Lock)))
      flip(a, t, PKeyLock);
    if (kl && lv.keylockActive) dl->AddCircleFilled(ImVec2(c.x + w - 10, y + 14), 3, P.led);
    y += 36;
  }
  // CUE and PLAY
  const float r = 31;
  const float by = p1.y - r - 14;
  bigButton(a, t, "##pcue", ImVec2(c.x + w * 0.25f + 2, by), r, i18n::tr("CUE"), CtlCue, lv.cueLit, theme::content(0xF2A73B),
            i18n::N_("Stopped: set the cue here, or hold to preview from it. Playing: back to the cue."));
  bigButton(a, t, "##pplay", ImVec2(c.x + w * 0.75f - 2, by), r, i18n::tr("PLAY"), CtlPlay, lv.playing, theme::content(0x7FD45B),
            i18n::N_("Start or stop the motor (the record spins up and brakes like a turntable)"));
  (void)y;
}

// ------------------------------------------------------------------ the platter
void drawPlatter(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c0 = panel(dl, a, p0, p1, i18n::tr("PLATTER"), lv.touched ? i18n::tr("HAND ON") : nullptr);
  const float modesH = 30;
  const float avail = std::min(p1.x - p0.x - 24, p1.y - c0.y - modesH - 14);
  const float R = std::floor(avail * 0.5f);
  const ImVec2 c((p0.x + p1.x) * 0.5f, c0.y + R + 2);
  const double turns = lv.valid ? lv.pos / (srcRateOf(e) * kSecondsPerRev) : 0.0;
  const float ang = static_cast<float>((turns - std::floor(turns)) * 2 * kPi) - kPi * 0.5f;
  // platter rim with strobe dots
  skin::gradientCircle(dl, ImVec2(c.x, c.y + 3), R + 1, theme::withAlpha(P.ink, 0.0f), theme::withAlpha(P.ink, 0.4f));
  skin::gradientCircle(dl, c, R, theme::mix(P.surface600, P.paper, 0.10f), P.surface900);
  const int dots = 72;
  for (int k = 0; k < dots; ++k) {
    const float th = ang + k * 2 * kPi / dots;
    const ImVec2 d(c.x + std::cos(th) * (R - 5), c.y + std::sin(th) * (R - 5));
    dl->AddCircleFilled(d, 1.4f, theme::withAlpha(P.text, k % 2 ? 0.25f : 0.55f), 8);
  }
  // the record: deep black with grooves and a soft sheen
  const float rv = R - 11;
  const ImU32 vinyl = theme::rgb(0x111216), groove = theme::rgb(0x2A2C33);
  dl->AddCircleFilled(c, rv, vinyl, 96);
  for (float rr = rv - 4; rr > rv * 0.42f; rr -= 3.2f) dl->AddCircle(c, rr, theme::withAlpha(groove, 0.55f), 96, 1);
  dl->PathArcTo(c, rv * 0.72f, -kPi * 0.92f, -kPi * 0.62f, 24);
  dl->PathStroke(theme::rgb(0xFFFFFF, 0.07f), 0, rv * 0.5f);
  dl->PathArcTo(c, rv * 0.72f, kPi * 0.08f, kPi * 0.38f, 24);
  dl->PathStroke(theme::rgb(0xFFFFFF, 0.05f), 0, rv * 0.5f);
  // centre label (turns with the record) and the marker
  const float rl = rv * 0.38f;
  skin::gradientCircle(dl, c, rl, theme::mix(accent(), P.paper, 0.12f), theme::mix(accent(), P.ink, 0.25f));
  const ImVec2 dir(std::cos(ang), std::sin(ang));
  dl->AddLine(ImVec2(c.x + dir.x * rl * 0.35f, c.y + dir.y * rl * 0.35f), ImVec2(c.x + dir.x * rl * 0.92f, c.y + dir.y * rl * 0.92f),
              theme::contrastText(accent()), 3);
  dl->AddLine(ImVec2(c.x + dir.x * rl * 1.08f, c.y + dir.y * rl * 1.08f), ImVec2(c.x + dir.x * (rv - 3), c.y + dir.y * (rv - 3)),
              theme::rgb(0xFFFFFF, 0.55f), 2);
  dl->AddCircleFilled(c, 5, P.surface600, 16);
  dl->AddCircle(c, 5, theme::rgb(0xFFFFFF, 0.35f), 16, 1);
  if (lv.touched) dl->AddCircle(c, R + 3, accent(), 96, 2.5f);
  // the hand: drag the record round
  ImGui::SetCursorScreenPos(ImVec2(c.x - R, c.y - R));
  ImGui::InvisibleButton("##pplatter", ImVec2(2 * R, 2 * R));
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const float dx = mp.x - c.x, dy = mp.y - c.y;
  const bool inside = dx * dx + dy * dy <= R * R;
  if (ImGui::IsItemHovered() && inside && !e.platterDrag) ui::Tooltip(i18n::tr("Drag the record round to scratch: clockwise plays, back plays backwards."));
  if (ImGui::IsItemActivated() && inside) {
    e.platterDrag = true;
    e.dragTurns = 0;
    e.lastAngle = std::atan2(dy, dx);
    send(a, t, CmdHandDown);
  }
  if (e.platterDrag && ImGui::IsItemActive()) {
    const double an = std::atan2(dy, dx);
    double d = an - e.lastAngle;
    if (d > kPi) d -= 2 * kPi;
    if (d < -kPi) d += 2 * kPi;
    if (dx * dx + dy * dy > 16) e.dragTurns += d / (2 * kPi);  // (no wild turns through the centre)
    e.lastAngle = an;
    send(a, t, CmdHandMove, 0, e.dragTurns);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  if (e.platterDrag && ImGui::IsItemDeactivated()) {
    e.platterDrag = false;
    send(a, t, CmdHandUp);
  }
  // modes
  const float my = p1.y - modesH - 8;
  const float bw = (p1.x - p0.x - 20 - 12) / 3;
  struct Mode {
    const char* id;
    const char* label;
    int param;
    const char* hint;
  };
  static const Mode kModes[3] = {
      {"##pvinyl", i18n::N_("VINYL"), PVinyl, i18n::N_("On: touching the jog wheel's top grabs the record. Off: it nudges the tempo, like a CD player.")},
      {"##pslip", i18n::N_("SLIP"), PSlip, i18n::N_("The track keeps running underneath while you scratch, loop or reverse; let go and it carries on in time.")},
      {"##pquant", i18n::N_("QUANTIZE"), PQuantize, i18n::N_("Cues, loops and jumps land on the beat")}};
  for (int k = 0; k < 3; ++k) {
    const Mode& md = kModes[k];
    if (button(md.id, ImVec2(p0.x + 10 + k * (bw + 6), my), ImVec2(bw, modesH - 4), md.label, on(a, t, md.param), md.hint)) flip(a, t, md.param);
  }
}

// ------------------------------------------------------------------ pitch fader (down = faster, like the hardware)
void drawPitch(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("PITCH"), nullptr);
  const float w = p1.x - p0.x;
  const double v = param(a, t, PPitch);
  const int range = static_cast<int>(std::lround(param(a, t, PPitchRange)));
  const double pct = v * pitchRangeOf(range) * 100.0;
  char b[24];
  std::snprintf(b, sizeof b, "%s%.2f%%", pct >= 0 ? "+" : "", pct);
  textC(dl, a.fontMono, theme::kFontLabel, ImVec2((p0.x + p1.x) * 0.5f, c.y + 4), std::fabs(pct) < 0.005 ? P.textDim : P.text, b);
  const float tx = std::round((p0.x + p1.x) * 0.5f);
  const float t0 = c.y + 22, t1 = p1.y - 64;
  const float len = t1 - t0;
  // scale
  for (int k = 0; k <= 8; ++k) {
    const float y = t0 + len * k / 8.0f;
    const float half = k == 4 ? 14.0f : (k % 2 ? 6.0f : 10.0f);
    dl->AddLine(ImVec2(tx - 20 - half * 0.5f, y), ImVec2(tx - 20 + half * 0.5f, y), k == 4 ? P.text : P.textFaint, 1);
  }
  skin::insetField(dl, ImVec2(tx - 4, t0 - 4), ImVec2(tx + 4, t1 + 4), P.surface950, 3);
  const float yv = t0 + static_cast<float>((v + 1) * 0.5) * len;  // top = -range, bottom = +range
  const float ymid = t0 + len * 0.5f;
  dl->AddRectFilled(ImVec2(tx - 2, std::min(ymid, yv)), ImVec2(tx + 2, std::max(ymid, yv)), accent(), 2);
  // the cap
  const ImVec2 q0(tx - 17, yv - 9), q1(tx + 17, yv + 9);
  ImGui::SetCursorScreenPos(ImVec2(tx - 22, t0 - 10));
  ImGui::InvisibleButton("##ppitch", ImVec2(44, len + 20));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? theme::mix(P.surface700, P.paper, 0.06f) : P.surface700, 3);
  dl->AddRect(q0, q1, P.line, 3);
  dl->AddLine(ImVec2(q0.x + 3, yv), ImVec2(q1.x - 3, yv), std::fabs(v) < 1e-3 ? theme::content(0x7FD45B) : P.paper, 2);
  if (hv || ac) ui::Tooltip(i18n::tr("Pitch fader: down plays faster, up slower (double-click: centre). With sync on it nudges the tempo off the project's."));
  (void)t.pid(PPitch);
  if (ac && ImGui::IsMouseDragging(0, 0)) {
    const double dv = ImGui::GetIO().MouseDelta.y / len * 2.0 * (ImGui::GetIO().KeyShift ? 0.1 : 1.0);
    pui::setParamValue(a, t, PPitch, std::clamp(v + dv, -1.0, 1.0), true);
  }
  if (hv && ImGui::IsMouseDoubleClicked(0)) pui::setParamValue(a, t, PPitch, 0.0, false);
  if (hv && ImGui::GetIO().MouseWheel != 0) pui::setParamValue(a, t, PPitch, std::clamp(v - ImGui::GetIO().MouseWheel * 0.02, -1.0, 1.0), true);
  // range
  static const char* const kRanges[4] = {"4%", "8%", "16%", "50%"};
  int cur = range;
  if (pills(a, "##prange", ImVec2(p0.x + 8, p1.y - 52), w - 16, 20, kRanges, 2, cur)) pui::setParamValue(a, t, PPitchRange, cur, false);
  int cur2 = range - 2;
  if (pills(a, "##prange2", ImVec2(p0.x + 8, p1.y - 28), w - 16, 20, kRanges + 2, 2, cur2)) pui::setParamValue(a, t, PPitchRange, cur2 + 2, false);
}

// ------------------------------------------------------------------ mixer strip
void drawMixer(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("MIXER"), nullptr);
  const float cellW = pui::knobCellWidth(pui::KnobMedium), cellH = pui::knobCellHeight(pui::KnobMedium);
  const float faderW = 112;
  const float kw = p1.x - p0.x - 20 - faderW;
  const float colW = kw / 4;
  auto at = [&](int col, float y) { return ImVec2(std::round(c.x + col * colW + (colW - cellW) * 0.5f), std::round(y)); };
  // EQ row and the filter row
  const float gv = std::max(6.0f, (p1.y - 12 - c.y - 2 * cellH) / 3);  // rows spread over the strip's height
  const float r1 = c.y + gv * 0.6f, r2 = r1 + cellH + gv * 1.2f;
  pui::knobAt(a, t, PEqHigh, at(0, r1), i18n::tr("High"));
  pui::knobAt(a, t, PEqMid, at(1, r1), i18n::tr("Mid"));
  pui::knobAt(a, t, PEqLow, at(2, r1), i18n::tr("Low"));
  pui::knobAt(a, t, PTrim, at(3, r1), i18n::tr("Trim"));
  pui::knobAt(a, t, PFilter, at(0, r2), i18n::tr("Filter"));
  pui::knobAt(a, t, PFilterRes, at(1, r2), i18n::tr("Res"));
  // EQ kills: a click drops a band out, a second click brings it back
  const float ky = r2 + 8;
  static const char* const kKill[3] = {i18n::N_("HI"), i18n::N_("MID"), i18n::N_("LOW")};
  const int bands[3] = {PEqHigh, PEqMid, PEqLow};
  const float kx = c.x + 2 * colW + 8, kbw = (kw - 2 * colW - 16) / 3;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(kx, ky - 2), P.textDim, i18n::tr("KILL"));
  for (int k = 0; k < 3; ++k) {
    const bool killed = param(a, t, bands[k]) <= kEqKillDb + 0.01;
    char id[16];
    std::snprintf(id, sizeof id, "##pkill%d", k);
    if (button(id, ImVec2(kx + k * kbw, ky + 14), ImVec2(kbw - 4, 26), kKill[k], killed, i18n::N_("Kill the band (click again to bring it back)")))
      pui::setParamValue(a, t, bands[k], killed ? 0.0 : kEqKillDb, false);
  }
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(kx, ky + 50), P.textDim, i18n::tr("FADER CURVE"));
  static const char* const kCurves[3] = {i18n::N_("Smooth"), i18n::N_("Linear"), i18n::N_("Sharp")};
  static const char* const kCurveHints[3] = {i18n::N_("Soft start: fine control at the bottom"), i18n::N_("Straight"),
                                             i18n::N_("Full level a sixth of the way up: fast cuts")};
  paramPills(a, t, PFaderCurve, ImVec2(kx, ky + 64), kw - 2 * colW - 20, 22, kCurves, 3, kCurveHints);
  // channel fader + meter
  const float fx = p1.x - 10 - faderW;
  dl->AddLine(ImVec2(fx - 6, c.y), ImVec2(fx - 6, p1.y - 12), P.line, 1);
  const float fh = p1.y - c.y - 58;
  ImGui::SetCursorScreenPos(ImVec2(fx + 14, c.y + 4));
  pui::vslider(a, t, PVolume, i18n::tr("Fader"), ImVec2(40, fh));
  const float lvl = lv.level > 1e-6f ? 20.0f * std::log10(lv.level) : -90.0f;
  pui::levelMeter(dl, ImVec2(fx + 72, c.y + 4), ImVec2(fx + 84, c.y + 4 + fh), lvl);
}

// ------------------------------------------------------------------ hot cues
void drawPads(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("HOT CUES"), i18n::tr("SHIFT+CLICK CLEARS"));
  const float gap = 6;
  const float pw = (p1.x - p0.x - 20 - 3 * gap) / 4, ph = (p1.y - c.y - 10 - gap) / 2;
  for (int k = 0; k < kHotCues; ++k) {
    const ImVec2 q0(c.x + (k % 4) * (pw + gap), c.y + (k / 4) * (ph + gap)), q1(q0.x + pw, q0.y + ph);
    const double f = lv.hot[static_cast<size_t>(k)];
    const bool set = f >= 0;
    char id[16];
    std::snprintf(id, sizeof id, "##phot%d", k);
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton(id, ImVec2(pw, ph), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
    const bool clear = ImGui::GetIO().KeyShift || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (ImGui::IsItemActivated()) {
      if (clear && set) press(a, t, CtlHotClear1 + k, true), press(a, t, CtlHotClear1 + k, false);
      else if (!clear) press(a, t, CtlHot1 + k, true);
    }
    if (ImGui::IsItemDeactivated() && !clear) press(a, t, CtlHot1 + k, false);
    if (hv) ui::Tooltip(set ? i18n::tr("Jump to this hot cue (hold while stopped to preview). Shift+click clears it.")
                            : i18n::tr("Set a hot cue here"));
    const ImU32 hc = hotColor(k);
    if (set) {
      skin::raisedFace(dl, q0, q1, ac ? theme::mix(hc, P.paper, 0.2f) : (hv ? theme::mix(hc, P.paper, 0.08f) : theme::withAlpha(hc, 0.85f)), theme::kRadius);
    } else {
      skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? P.surface700 : P.surface900, theme::kRadius);
      dl->AddRect(q0, q1, theme::withAlpha(hc, 0.55f), theme::kRadius);
    }
    char nb[4];
    std::snprintf(nb, sizeof nb, "%d", k + 1);
    const ImU32 fg = set ? theme::contrastText(hc) : theme::withAlpha(hc, 0.9f);
    dl->AddText(a.fontBold, theme::kFontBody, ImVec2(q0.x + 8, q0.y + 5), fg, nb);
    if (set) {
      const std::string tm = fmtTime(f / srcRateOf(e));
      textR(dl, a.fontMono, theme::kFontLabel, ImVec2(q1.x - 7, q1.y - 16), theme::withAlpha(fg, 0.85f), tm.c_str());
    }
  }
}

// ------------------------------------------------------------------ loops
void drawLoops(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("LOOP"), nullptr);
  const float w = p1.x - p0.x - 20;
  const int size = static_cast<int>(std::lround(param(a, t, PLoopSize)));
  // size: < 4 beats >
  const float sy = c.y;
  if (button("##phalf", ImVec2(c.x, sy), ImVec2(34, 26), "", false, i18n::N_("Halve the loop size (and a running loop)"),
             static_cast<int>(ui::Icon::ChevronLeft))) {
    if (lv.loopOn) press(a, t, CtlLoopHalve, true);
    pui::setParamValue(a, t, PLoopSize, std::max(0, size - 1), false);
  }
  {
    const ImVec2 f0(c.x + 40, sy), f1(c.x + w - 40, sy + 26);
    skin::insetField(dl, f0, f1, theme::pal().field, theme::kRadius);
    char b[32];
    std::snprintf(b, sizeof b, "%s %s", loopLabel(size), size <= 3 ? i18n::tr("beat") : i18n::tr("beats"));
    textC(dl, a.fontMono, theme::kFontNumber, ImVec2((f0.x + f1.x) * 0.5f, (f0.y + f1.y) * 0.5f), theme::pal().text, b);
    (void)t.pid(PLoopSize);
  }
  if (button("##pdouble", ImVec2(c.x + w - 34, sy), ImVec2(34, 26), "", false, i18n::N_("Double the loop size (and a running loop)"),
             static_cast<int>(ui::Icon::Right))) {
    if (lv.loopOn) press(a, t, CtlLoopDouble, true);
    pui::setParamValue(a, t, PLoopSize, std::min(kLoopSizes - 1, size + 1), false);
  }
  // in / out / auto
  const float by = sy + 34, bw = (w - 12) / 3;
  if (button("##pin", ImVec2(c.x, by), ImVec2(bw, 28), i18n::N_("IN"), false, i18n::N_("Loop in: mark the start here"))) press(a, t, CtlLoopIn, true);
  if (button("##pout", ImVec2(c.x + bw + 6, by), ImVec2(bw, 28), i18n::N_("OUT"), false, i18n::N_("Loop out: loop from the in point to here (again: exit)")))
    press(a, t, CtlLoopOut, true);
  if (button("##pauto", ImVec2(c.x + 2 * (bw + 6), by), ImVec2(bw, 28), "", lv.loopOn, i18n::N_("Loop the next beats (the size above); again to exit"),
             static_cast<int>(ui::Icon::LoopArrows)))
    press(a, t, CtlLoopAuto, true);
  // beat jump
  const float jy = by + 36, jw = (w - 6) / 2;
  if (button("##pjb", ImVec2(c.x, jy), ImVec2(jw, 26), i18n::N_("Jump back"), false, i18n::N_("Jump back by the loop size"),
             static_cast<int>(ui::Icon::Left)))
    press(a, t, CtlJumpBack, true);
  if (button("##pjf", ImVec2(c.x + jw + 6, jy), ImVec2(jw, 26), i18n::N_("Jump ahead"), false, i18n::N_("Jump ahead by the loop size"),
             static_cast<int>(ui::Icon::Right)))
    press(a, t, CtlJumpFwd, true);
}

// ------------------------------------------------------------------ crossfader
void drawXfader(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("CROSSFADER"), nullptr);
  const float w = p1.x - p0.x - 20;
  static const char* const kSides[3] = {i18n::N_("Thru"), i18n::N_("A"), i18n::N_("B")};
  static const char* const kSideHints[3] = {i18n::N_("The crossfader does not touch this deck"), i18n::N_("This deck is side A (left)"),
                                            i18n::N_("This deck is side B (right)")};
  paramPills(a, t, PXfSide, ImVec2(c.x, c.y), w, 22, kSides, 3, kSideHints);
  // the fader: A left, B right
  const float ty = c.y + 46;
  const float tx0 = c.x + 16, tx1 = c.x + w - 16;
  dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(c.x, ty - 7), P.textDim, "A");
  textR(dl, a.fontBold, theme::kFontLabel, ImVec2(c.x + w, ty - 7), P.textDim, "B");
  skin::insetField(dl, ImVec2(tx0, ty - 3), ImVec2(tx1, ty + 3), P.surface950, 3);
  const ParamId pid = t.pid(PXfader);
  const PluginInfo* info = findPlugin(kType);
  double v = param(a, t, PXfader);
  ImGui::SetCursorScreenPos(ImVec2(tx0 - 10, ty - 13));
  ImGui::InvisibleButton("##pxf", ImVec2(tx1 - tx0 + 20, 26));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (info) pui::dragParamTrack(a, pid, info->params[PXfader], v, tx0, tx1 - tx0, true);
  const float xv = tx0 + static_cast<float>(v) * (tx1 - tx0);
  const ImVec2 q0(xv - 8, ty - 12), q1(xv + 8, ty + 12);
  skin::raisedFace(dl, q0, q1, ac ? P.surface600 : hv ? theme::mix(P.surface700, P.paper, 0.06f) : P.surface700, 3);
  dl->AddRect(q0, q1, P.line, 3);
  dl->AddLine(ImVec2(xv, q0.y + 3), ImVec2(xv, q1.y - 3), P.paper, 2);
  // curve
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(c.x, ty + 24), P.textDim, i18n::tr("CURVE"));
  ImGui::SetCursorScreenPos(ImVec2(c.x + 54, ty + 20));
  pui::hslider(a, t, PXfCurve, i18n::tr("Smooth / cut"), w - 54);
}

// ------------------------------------------------------------------ feel
void drawFeel(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = panel(dl, a, p0, p1, i18n::tr("FEEL"), nullptr);
  const float w = p1.x - p0.x - 20;
  const float cellW = pui::knobCellWidth(pui::KnobSmall);
  const float colW = w / 3;
  auto at = [&](int col) { return ImVec2(std::round(c.x + col * colW + (colW - cellW) * 0.5f), std::round(c.y - 2)); };
  pui::knobAt(a, t, PStartTime, at(0), i18n::tr("Start"), pui::KnobSmall);
  pui::knobAt(a, t, PStopTime, at(1), i18n::tr("Brake"), pui::KnobSmall);
  pui::knobAt(a, t, PFeel, at(2), i18n::tr("Hand"), pui::KnobSmall);
  static const char* const kModes[2] = {i18n::N_("Gate"), i18n::N_("Trigger")};
  static const char* const kHints[2] = {i18n::N_("A hot cue key plays while held"), i18n::N_("A hot cue key starts the deck; it keeps playing")};
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(c.x, p1.y - 30), theme::pal().textDim, i18n::tr("KEYS"));
  paramPills(a, t, PNoteMode, ImVec2(c.x + 40, p1.y - 34), w - 40, 22, kModes, 2, kHints);
}

// ------------------------------------------------------------------ takes
void drawTakes(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.surface900, theme::kRadiusLg);
  dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
  const float cy = (p0.y + p1.y) * 0.5f;
  // arm
  const bool armed = e.m.armed;
  {
    const ImVec2 q0(p0.x + 6, p0.y + 5), q1(p0.x + 170, p1.y - 5);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##parm", ImVec2(q1.x - q0.x, q1.y - q0.y))) {
      Model nm = e.m;
      nm.armed = !nm.armed;
      commit(a, t, e, nm, nm.armed ? i18n::N_("Arm take recording") : i18n::N_("Disarm take recording"));
    }
    const bool hv = ImGui::IsItemHovered();
    if (hv) ui::Tooltip(i18n::tr("While Gridline records, an armed deck records what you play on it (scratches, cuts, cues) as a take: "
                                 "a note in the pattern that replays it exactly, with no controller."));
    skin::raisedFace(dl, q0, q1, hv ? P.surface700 : P.surface800, theme::kRadius);
    const bool blink = lv.capturing && std::fmod(ImGui::GetTime(), 1.0) < 0.5;
    dl->AddCircleFilled(ImVec2(q0.x + 14, cy), 5.5f, armed ? (blink ? theme::withAlpha(P.danger, 0.4f) : P.danger) : theme::withAlpha(P.textFaint, 0.6f));
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(q0.x + 26, cy - 8), armed ? P.text : P.textDim,
                lv.capturing ? i18n::tr("Recording a take") : armed ? i18n::tr("Takes armed") : i18n::tr("Takes off"));
  }
  float x = p0.x + 182;
  if (e.m.takes.empty()) {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy - 8), P.textFaint,
                i18n::tr("No takes yet: arm, press record in Gridline and play the deck. Each pass becomes a note on keys C2 to B3."));
    return;
  }
  for (size_t i = 0; i < e.m.takes.size(); ++i) {
    const Take& tk = e.m.takes[i];
    char b[64];
    std::snprintf(b, sizeof b, "%s  %s  %.1f s", noteName(tk.key).c_str(), tk.name.empty() ? i18n::tr("Take") : tk.name.c_str(), tk.seconds());
    const float tw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, b).x + 24;
    if (x + tw > p1.x - 6) break;
    const ImVec2 q0(x, p0.y + 6), q1(x + tw, p1.y - 6);
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetCursorScreenPos(q0);
    ImGui::InvisibleButton("##ptake", ImVec2(tw, q1.y - q0.y));
    const bool hv = ImGui::IsItemHovered(), playing = lv.take == tk.key;
    if (ImGui::IsItemActivated() && t.isChannel()) a.engine.noteOn(t.owner, tk.key, 0.9f);
    if (ImGui::IsItemDeactivated() && t.isChannel()) a.engine.noteOff(t.owner, tk.key);
    if (hv) ui::Tooltip(i18n::tr("Hold to hear the take. Right-click to delete it."));
    if (hv && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##ptakemenu");
    if (ImGui::BeginPopup("##ptakemenu")) {
      if (ImGui::MenuItem(i18n::tr("Delete take"))) {
        Model nm = e.m;
        nm.takes.erase(nm.takes.begin() + static_cast<std::ptrdiff_t>(i));
        commit(a, t, e, nm, i18n::N_("Delete take"));
        ImGui::EndPopup();
        ImGui::PopID();
        return;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    dl->AddRectFilled(q0, q1, playing ? theme::withAlpha(accent(), 0.35f) : hv ? P.surface700 : P.surface800, (q1.y - q0.y) * 0.5f);
    dl->AddRect(q0, q1, playing ? accent() : P.line, (q1.y - q0.y) * 0.5f);
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 12, cy - 8), P.text, b);
    x += tw + 6;
  }
}

}  // namespace

float deckHeight() { return kOverviewH + 6 + kWaveH + kGap + kDeckH + kGap + kBottomH + 8 + kTakesH; }

float drawDeck(App& a, const PluginTarget& t, Ed& e, ImVec2 o, float W, const LiveView& lv) {
  float y = o.y;
  if (!ready(e)) {
    drawDrop(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + kOverviewH + 6 + kWaveH));
  } else {
    drawOverview(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + kOverviewH), lv);
    drawWave(a, t, e, ImVec2(o.x, y + kOverviewH + 6), ImVec2(o.x + W, y + kOverviewH + 6 + kWaveH), lv);
  }
  y += kOverviewH + 6 + kWaveH + kGap;
  // the deck row
  const float tW = 214, pW = 304, fW = 96;
  const float mW = W - tW - pW - fW - 3 * kGap;
  float x = o.x;
  drawTransport(a, t, e, ImVec2(x, y), ImVec2(x + tW, y + kDeckH), lv);
  x += tW + kGap;
  drawPlatter(a, t, e, ImVec2(x, y), ImVec2(x + pW, y + kDeckH), lv);
  x += pW + kGap;
  drawPitch(a, t, ImVec2(x, y), ImVec2(x + fW, y + kDeckH));
  x += fW + kGap;
  drawMixer(a, t, ImVec2(x, y), ImVec2(x + mW, y + kDeckH), lv);
  y += kDeckH + kGap;
  // pads, loop, crossfader, feel
  const float padsW = std::round(W * 0.34f), loopW = std::round(W * 0.22f), xfW = std::round(W * 0.22f);
  const float feelW = W - padsW - loopW - xfW - 3 * kGap;
  x = o.x;
  drawPads(a, t, e, ImVec2(x, y), ImVec2(x + padsW, y + kBottomH), lv);
  x += padsW + kGap;
  drawLoops(a, t, ImVec2(x, y), ImVec2(x + loopW, y + kBottomH), lv);
  x += loopW + kGap;
  drawXfader(a, t, ImVec2(x, y), ImVec2(x + xfW, y + kBottomH));
  x += xfW + kGap;
  drawFeel(a, t, ImVec2(x, y), ImVec2(x + feelW, y + kBottomH));
  y += kBottomH + 8;
  drawTakes(a, t, e, ImVec2(o.x, y), ImVec2(o.x + W, y + kTakesH), lv);
  y += kTakesH;
  return y - o.y;
}

}  // namespace gl::packs::platter::pv
