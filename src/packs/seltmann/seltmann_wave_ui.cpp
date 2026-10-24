// Gridline — "seltmann" pack: the editor's waveform parts. The waveform (the
// region with its bar grid and the slices coloured by slot letter; cut and grid
// editing open up in the Edit drawer's Chop / Grid tabs), the slim whole-song
// strip (sections and the region being chopped, songs only, can be hidden) and
// the chop settings of the drawer (mode, division / sensitivity / count / grain
// length, snapping).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::sui {

using theme::pal;

namespace {

// Min / max / RMS of the source over [f0, f1) (peaks when coarse, samples when fine).
void range(const Ed& e, const Sample* s, double f0, double f1, float& mn, float& mx, float& rms) {
  mn = mx = rms = 0;
  if (f1 <= f0) return;
  const Peaks* pk = e.job && e.job->state == 1 ? &e.job->peaks : nullptr;
  if (pk && !pk->mx.empty() && f1 - f0 >= kPeakBlock) {
    const auto b0 = static_cast<int64_t>(f0 / kPeakBlock), b1 = std::max(b0 + 1, static_cast<int64_t>(std::ceil(f1 / kPeakBlock)));
    bool first = true;
    double sq = 0;
    int cnt = 0;
    for (int64_t b = std::max<int64_t>(0, b0); b < b1 && b < static_cast<int64_t>(pk->mx.size()); ++b) {
      if (first) mn = pk->mn[static_cast<size_t>(b)], mx = pk->mx[static_cast<size_t>(b)], first = false;
      mn = std::min(mn, pk->mn[static_cast<size_t>(b)]);
      mx = std::max(mx, pk->mx[static_cast<size_t>(b)]);
      if (b < static_cast<int64_t>(pk->rms.size())) sq += static_cast<double>(pk->rms[static_cast<size_t>(b)]) * pk->rms[static_cast<size_t>(b)], ++cnt;
    }
    rms = cnt ? static_cast<float>(std::sqrt(sq / cnt)) : 0.f;
    return;
  }
  if (!s) return;
  const int64_t n = static_cast<int64_t>(s->frames());
  const int64_t a = std::clamp<int64_t>(static_cast<int64_t>(f0), 0, n), b = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(f1)), a, n);
  bool first = true;
  double sq = 0;
  const bool st = !s->mono() && s->right.size() == s->left.size();
  for (int64_t i = a; i < b; ++i) {
    float l = s->left[static_cast<size_t>(i)];
    float r = st ? s->right[static_cast<size_t>(i)] : l;
    if (first) mn = std::min(l, r), mx = std::max(l, r), first = false;
    mn = std::min(mn, std::min(l, r));
    mx = std::max(mx, std::max(l, r));
    sq += 0.25 * (static_cast<double>(l) + r) * (static_cast<double>(l) + r);
  }
  rms = b > a ? static_cast<float>(std::sqrt(sq / static_cast<double>(b - a))) : 0.f;
}

int barOfFrame(const Model& m, double f) {
  const int bpb = std::max(1, m.grid.beatsPerBar);
  return static_cast<int>(std::lround((beatAt(m.grid, f) - m.grid.bar1) / bpb));
}

// Horizontal slider over a float (UI value). Returns true on release after a change.
bool hslider(App& a, const char* id, ImVec2 p0, float w, float& v, float mn, float mx, const char* fmt, const char* label, const char* hint) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lw = label ? a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, i18n::tr(label)).x + 8 : 0;
  if (label) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p0.x, p0.y + 4), P.textDim, i18n::tr(label));
  const float x0 = p0.x + lw, tw = w - lw - 46;
  ImGui::SetCursorScreenPos(ImVec2(x0, p0.y));
  ImGui::InvisibleButton(id, ImVec2(tw, 22));
  const bool hv = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
  if (hv && hint) ui::Tooltip(i18n::tr(hint));
  if (ac) v = std::clamp(mn + (mx - mn) * (ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, tw), mn, mx);
  const float n = (v - mn) / std::max(1e-6f, mx - mn);
  const float cy = p0.y + 11;
  dl->AddRectFilled(ImVec2(x0, cy - 2), ImVec2(x0 + tw, cy + 2), P.surface900, 2);
  dl->AddRectFilled(ImVec2(x0, cy - 2), ImVec2(x0 + tw * n, cy + 2), accent(), 2);
  dl->AddCircleFilled(ImVec2(x0 + tw * n, cy), hv || ac ? 6.5f : 5.5f, P.paper, 16);
  dl->AddCircle(ImVec2(x0 + tw * n, cy), hv || ac ? 6.5f : 5.5f, accent(), 16, 1.5f);
  char b[24];
  std::snprintf(b, sizeof b, fmt, static_cast<double>(v));
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x0 + tw + 8, p0.y + 5), P.text, b);
  return ImGui::IsItemDeactivated();
}

}  // namespace

// ------------------------------------------------------------------ whole-song strip
void drawStrip(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const ImVec2 g0(p0.x + 8, p0.y + 3), g1(p1.x - 8, p1.y - 3);
  const float w = g1.x - g0.x;
  SamplePtr smp = sampleOf(e);
  const double N = e.hasModel ? static_cast<double>(e.m.srcFrames) : (smp ? static_cast<double>(smp->frames()) : 0.0);
  const float bandY0 = g0.y, bandY1 = g0.y + 4, waveY0 = g0.y + 6, waveY1 = g1.y - 1;
  const float mid = (waveY0 + waveY1) * 0.5f, half = (waveY1 - waveY0) * 0.5f;
  if (N <= 0) {
    if (e.job && e.job->state == 0) textC(dl, a.fontRegular, theme::kFontCaption, ImVec2((g0.x + g1.x) * 0.5f, mid), P.textDim, i18n::tr("Decoding..."));
    return;
  }
  auto X = [&](double f) { return g0.x + static_cast<float>(f / N) * w; };
  // region (live while dragging)
  double ra = static_cast<double>(e.m.regionStart), rb = static_cast<double>(e.m.regionEnd);
  if (e.dragRegion) ra = e.dragA, rb = e.dragB;
  // waveform
  const bool havePeaks = e.job && e.job->state == 1;
  if (havePeaks) {
    for (int px = 0; px < static_cast<int>(w); ++px) {
      const double f0 = N * px / w, f1 = N * (px + 1) / w;
      float mn, mx, rms;
      range(e, smp.get(), f0, f1, mn, mx, rms);
      const bool in = e.hasModel && f1 > ra && f0 < rb;
      const ImU32 col = in ? accent() : theme::withAlpha(P.textDim, 0.85f);
      const float x = g0.x + px + 0.5f;
      dl->AddLine(ImVec2(x, mid - std::max(0.5f, mx * half)), ImVec2(x, mid - std::min(-0.5f, mn * half)), theme::withAlpha(col, 0.38f), 1.0f);
      const float rh = std::max(0.5f, std::min(1.0f, rms * 1.41f) * half);
      dl->AddLine(ImVec2(x, mid - rh), ImVec2(x, mid + rh), col, 1.0f);
    }
  } else if (e.job && e.job->state == 0) {
    const float p = e.job->progress.load();
    char b[64];
    std::snprintf(b, sizeof b, "%s  %d%%", i18n::tr("Finding the tempo, the beats and the sections"), static_cast<int>(p * 100));
    textC(dl, a.fontRegular, theme::kFontCaption, ImVec2((g0.x + g1.x) * 0.5f, mid), P.textDim, b);
  }
  if (!e.hasModel) return;
  const Model& m = e.m;
  // section bands
  for (size_t i = 0; i < m.grid.sections.size(); ++i) {
    const auto& s = m.grid.sections[i];
    const float x0 = X(frameAt(m.grid, s.firstBeat)), x1 = X(frameAt(m.grid, s.endBeat));
    if (x1 - x0 < 2) continue;
    dl->AddRectFilled(ImVec2(x0 + 1, bandY0), ImVec2(x1 - 1, bandY1), slotColor(s.label, 0.75f), 1.5f);
  }
  // bar ticks + labels
  const int bars = barCount(m);
  if (bars > 0) {
    const float barPx = w * static_cast<float>((barStartFrame(m, 1) - barStartFrame(m, 0)) / N);
    int every = 1;
    while (every * barPx < 34 && every < 256) every *= 2;
    int tickEvery = 1;
    while (tickEvery * barPx < 5 && tickEvery < 64) tickEvery *= 2;
    for (int b = 0; b <= bars; b += tickEvery) {
      const float x = std::round(X(barStartFrame(m, b))) + 0.5f;
      const bool lab = b % every == 0;
      dl->AddLine(ImVec2(x, g1.y - (lab ? 4.0f : 2.0f)), ImVec2(x, g1.y), theme::withAlpha(P.textDim, lab ? 0.6f : 0.3f), 1);
    }
  }
  // region
  {
    const float x0 = X(ra), x1 = std::max(X(rb), x0 + 2);
    dl->AddRectFilled(ImVec2(x0, waveY0 - 1), ImVec2(x1, g1.y), theme::withAlpha(accent(), 0.10f), 2);
    dl->AddRect(ImVec2(x0, waveY0 - 1), ImVec2(x1, g1.y), theme::withAlpha(accent(), 0.9f), 2, 0, 1.5f);
    for (float hx : {x0, x1}) dl->AddRectFilled(ImVec2(hx - 2, mid - 6), ImVec2(hx + 2, mid + 6), P.paper, 2);
  }
  // playhead
  if (lv.srcFrame >= 0) {
    const float x = X(lv.srcFrame);
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), theme::withAlpha(P.playhead, 0.95f), 1.5f);
  }
  // interaction: drag edges / move / draw a new region (whole bars); double-click a section band
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##sov", ImVec2(w, g1.y - g0.y));
  const bool hv = ImGui::IsItemHovered();
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const double fm = std::clamp(static_cast<double>((mp.x - g0.x) / w) * N, 0.0, N);
  const float rx0 = X(e.m.regionStart), rx1 = X(e.m.regionEnd);
  if (hv && !ImGui::IsItemActive()) {
    if (std::fabs(mp.x - rx0) < 6 || std::fabs(mp.x - rx1) < 6) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    char b[64];
    std::snprintf(b, sizeof b, "%s %d  ·  %s", i18n::tr("Bar"), barOfFrame(m, fm) + 1, fmtTime(fm / m.srcRate).c_str());
    ui::Tooltip((std::string(b) + "\n" +
                 i18n::tr("Drag the box or its edges to pick the bars to chop, drag elsewhere for a new one, double-click a section to take it"))
                    .c_str());
  }
  if (ImGui::IsItemActivated()) {
    e.dragA = static_cast<double>(e.m.regionStart);
    e.dragB = static_cast<double>(e.m.regionEnd);
    e.dragGrab = fm;
    if (std::fabs(mp.x - rx0) < 6) e.dragRegion = 1;
    else if (std::fabs(mp.x - rx1) < 6) e.dragRegion = 2;
    else if (mp.x > rx0 && mp.x < rx1 && mp.y > bandY1) e.dragRegion = 3;
    else e.dragRegion = 4;
  }
  if (ImGui::IsItemActive() && e.dragRegion) {
    const int bpbBars = std::max(0, bars);
    auto snapBar = [&](double f) { return std::clamp(barOfFrame(m, f), 0, std::max(0, bpbBars)); };
    const int ba = barOfFrame(m, static_cast<double>(e.m.regionStart)), bb = barOfFrame(m, static_cast<double>(e.m.regionEnd));
    int na = ba, nb = bb;
    if (e.dragRegion == 1) na = std::min(snapBar(fm), bb - 1);
    else if (e.dragRegion == 2) nb = std::max(snapBar(fm), ba + 1);
    else if (e.dragRegion == 3) {
      const int d = barOfFrame(m, fm) - barOfFrame(m, e.dragGrab);
      const int len = bb - ba;
      na = std::clamp(ba + d, 0, std::max(0, bpbBars - len));
      nb = na + len;
    } else {
      const int g = snapBar(e.dragGrab), c = snapBar(fm);
      na = std::min(g, c), nb = std::max(g, c) + 1;
    }
    e.dragA = std::max(0.0, barStartFrame(m, na));
    e.dragB = std::min(N, barStartFrame(m, std::max(na + 1, nb)));
    ImGui::SetMouseCursor(e.dragRegion == 3 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
  }
  if (ImGui::IsItemDeactivated() && e.dragRegion) {
    const bool moved = std::llabs(static_cast<int64_t>(e.dragA) - e.m.regionStart) > 1 || std::llabs(static_cast<int64_t>(e.dragB) - e.m.regionEnd) > 1;
    if (moved) {
      Model nm = e.m;
      nm.regionStart = static_cast<int64_t>(std::llround(e.dragA));
      nm.regionEnd = static_cast<int64_t>(std::llround(e.dragB));
      if (nm.chop.mode == ChopManual) nm.chop.mode = ChopGrid;
      rechopKeepPattern(nm, smp.get());
      commit(a, t, e, nm, i18n::N_("Select the region"));
      e.selSlice = 0;
    }
    e.dragRegion = 0;
  }
  if (hv && ImGui::IsMouseDoubleClicked(0))
    for (const auto& s : m.grid.sections)
      if (fm >= frameAt(m.grid, s.firstBeat) && fm < frameAt(m.grid, s.endBeat)) {
        Model nm = e.m;
        nm.regionStart = static_cast<int64_t>(std::llround(std::max(0.0, frameAt(m.grid, s.firstBeat))));
        nm.regionEnd = static_cast<int64_t>(std::llround(std::min(N, frameAt(m.grid, s.endBeat))));
        rechopKeepPattern(nm, smp.get());
        commit(a, t, e, nm, i18n::N_("Select a section"));
        e.dragRegion = 0;
        break;
      }
}

// ------------------------------------------------------------------ chop settings (drawer)
void drawChopSettings(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, float W) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!e.hasModel) return;
  SamplePtr smp = sampleOf(e);
  Model& m = e.m;
  float x = p0.x;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x, p0.y + 6), P.textFaint, i18n::tr("CHOP"));
  x += 38;
  static const char* const modes[ChopModeCount] = {i18n::N_("Grid"), i18n::N_("Hits"), i18n::N_("Sections"), i18n::N_("Equal"), i18n::N_("Grains"),
                                                    i18n::N_("Manual")};
  static const char* const modeHints[ChopModeCount] = {
      i18n::N_("Cut on the beat grid: bars, beats or subdivisions"), i18n::N_("Cut at the transients (drum hits, note starts)"),
      i18n::N_("Cut where the song's sections change"), i18n::N_("Cut into equal pieces"),
      i18n::N_("Microsampling: cut into short grains of a fixed length"), i18n::N_("Keep the cuts placed by hand")};
  int mode = m.chop.mode;
  if (pills(a, "##schopmode", ImVec2(x, p0.y + 1), 372, 24, modes, ChopModeCount, mode, modeHints)) {
    Model nm = m;
    nm.chop.mode = mode;
    nm.chop.autoSize = false;
    rechopKeepPattern(nm, smp.get());
    commit(a, t, e, nm, i18n::N_("Chop mode"));
  }
  x += 384;
  const float snapW = 214;
  const float ctrlW = p0.x + W - snapW - 12 - x;
  switch (m.chop.mode) {
    case ChopGrid: {
      static const char* const divs[DivCount] = {"8", "4", "2", "1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T"};
      static const char* const divHints[DivCount] = {i18n::N_("8 bars"),       i18n::N_("4 bars"),         i18n::N_("2 bars"),
                                                     i18n::N_("1 bar"),        i18n::N_("Half a bar"),     i18n::N_("Beats (quarter notes)"),
                                                     i18n::N_("Eighth notes"), i18n::N_("Sixteenth notes"), i18n::N_("Thirty-second notes"),
                                                     i18n::N_("Sixty-fourth notes"), i18n::N_("Eighth-note triplets"), i18n::N_("Sixteenth-note triplets")};
      int d = m.chop.division;
      if (pills(a, "##sdiv", ImVec2(x, p0.y + 1), ctrlW, 24, divs, DivCount, d, divHints, true)) {
        Model nm = m;
        nm.chop.division = d;
        nm.chop.autoSize = false;
        rechopKeepPattern(nm, smp.get());
        commit(a, t, e, nm, i18n::N_("Chop division"));
      }
      break;
    }
    case ChopTransients: {
      float v = m.chop.sensitivity;
      const bool rel = hslider(a, "##ssens", ImVec2(x, p0.y + 1), ctrlW, v, 0.0f, 1.0f, "%.2f", i18n::N_("Sensitivity"),
                               i18n::N_("More sensitive: more, softer hits become cuts"));
      if (v != m.chop.sensitivity || rel) {
        m.chop.sensitivity = v;
        if (rel) {
          Model nm = m;
          rechopKeepPattern(nm, smp.get());
          commit(a, t, e, nm, i18n::N_("Hit sensitivity"));
        }
      }
      break;
    }
    case ChopEqual: {
      float v = static_cast<float>(m.chop.count);
      const bool rel = hslider(a, "##scount", ImVec2(x, p0.y + 1), ctrlW, v, 2.0f, 64.0f, "%.0f", i18n::N_("Slices"), nullptr);
      const int c = static_cast<int>(std::lround(v));
      if (c != m.chop.count || rel) {
        Model nm = m;
        nm.chop.count = c;
        rechopKeepPattern(nm, smp.get());
        commit(a, t, e, nm, i18n::N_("Slice count"), 0x5E1700ull);
      }
      break;
    }
    case ChopGrains: {
      float v = m.chop.grainMs;
      const bool rel = hslider(a, "##sgrain", ImVec2(x, p0.y + 1), ctrlW, v, 10.0f, 250.0f, "%.0f ms", i18n::N_("Grain"),
                               i18n::N_("Microsampling grain length (20 to 120 ms is typical)"));
      if (v != m.chop.grainMs) m.chop.grainMs = v;
      if (rel) {
        Model nm = m;
        rechopKeepPattern(nm, smp.get());
        commit(a, t, e, nm, i18n::N_("Grain length"));
      }
      break;
    }
    default: {
      const char* s = m.chop.mode == ChopSections ? i18n::tr("Cuts follow the detected sections") : i18n::tr("Double-click the waveform to add a cut, drag a cut to move it, right-click to remove it");
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x + 2, p0.y + 6), ctrlW, P.textDim, s);
      break;
    }
  }
  // snap + zero crossings
  const float sx = p0.x + W - snapW;
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(sx, p0.y + 6), P.textFaint, i18n::tr("SNAP"));
  static const char* const snaps[4] = {i18n::N_("Off"), i18n::N_("Grid"), i18n::N_("Hit"), i18n::N_("Zero")};
  static const char* const snapHints[4] = {i18n::N_("Cuts go where you drop them"), i18n::N_("Cuts snap to sixteenths of the beat grid"),
                                           i18n::N_("Cuts snap to the nearest transient"), i18n::N_("Cuts snap to the nearest zero crossing")};
  int sn = m.chop.snap;
  if (pills(a, "##ssnap", ImVec2(sx + 38, p0.y + 1), 146, 24, snaps, 4, sn, snapHints)) {
    Model nm = m;
    nm.chop.snap = sn;
    commit(a, t, e, nm, i18n::N_("Snap"));
  }
  ImGui::SetCursorScreenPos(ImVec2(sx + 188, p0.y + 1));
  if (ui::IconButton("##szc", ui::Icon::ZeroCross, m.chop.zeroCross, 24, i18n::tr("Move every cut to the nearest zero crossing (no clicks)"),
                     accent())) {
    Model nm = m;
    nm.chop.zeroCross = !nm.chop.zeroCross;
    rechopKeepPattern(nm, smp.get());
    commit(a, t, e, nm, i18n::N_("Zero-crossing cuts"));
  }
}

// ------------------------------------------------------------------ waveform
void drawWave(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadiusLg);
  if (!e.hasModel) {
    textC(dl, a.fontRegular, theme::kFontCaption, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), P.textDim,
          i18n::tr("The chop appears here once the analysis is done"));
    return;
  }
  SamplePtr smp = sampleOf(e);
  const Model& m = e.m;
  const ImVec2 g0(p0.x + 8, p0.y + 18), g1(p1.x - 8, p1.y - 8);
  const float w = g1.x - g0.x;
  const double ra = static_cast<double>(m.regionStart), rb = static_cast<double>(std::max(m.regionEnd, m.regionStart + 1));
  auto X = [&](double f) { return g0.x + static_cast<float>((f - ra) / (rb - ra)) * w; };
  auto F = [&](float x) { return ra + (rb - ra) * static_cast<double>((x - g0.x) / w); };
  const float mid = (g0.y + g1.y) * 0.5f, half = (g1.y - g0.y) * 0.5f - 2;
  const int n = static_cast<int>(m.slices.size());
  // cut and grid editing open up in the Edit drawer (Chop / Grid tabs); otherwise a click plays a slice
  const bool editCuts = e.drawer && e.drawerTab == DrawChop;
  const bool editGrid = e.drawer && (e.drawerTab == DrawGrid || e.drawerTab == DrawChop);
  // slice tints
  for (int k = 0; k < n; ++k) {
    const auto& s = m.slices[static_cast<size_t>(k)];
    const int slot = sliceSlot(m, k);
    const float x0 = X(static_cast<double>(s.start)), x1 = X(static_cast<double>(s.end));
    const bool sel = k == e.selSlice, playing = k == lv.slice;
    const float al = playing ? 0.26f : sel ? 0.18f : (k % 2 ? 0.05f : 0.09f);
    dl->AddRectFilled(ImVec2(x0, p0.y + 2), ImVec2(x1, p1.y - 2), slot >= 0 ? slotColor(slot, al) : theme::withAlpha(P.paper, al * 0.6f));
    if (sel) dl->AddRectFilled(ImVec2(x0 + 1, p0.y + 2), ImVec2(x1 - 1, p0.y + 5), slot >= 0 ? slotColor(slot) : accent(), 1);
  }
  // waveform coloured per slice
  int k = 0;
  for (int px = 0; px < static_cast<int>(w); ++px) {
    const double f0 = F(g0.x + px), f1 = F(g0.x + px + 1);
    while (k + 1 < n && m.slices[static_cast<size_t>(k + 1)].start <= f0) ++k;
    float mn, mx, rms;
    range(e, smp.get(), f0, f1, mn, mx, rms);
    const int slot = n ? sliceSlot(m, k) : -1;
    const ImU32 col = slot >= 0 ? slotColor(slot, 1.0f) : theme::withAlpha(P.text, 0.8f);
    const float x = g0.x + px + 0.5f;
    dl->AddLine(ImVec2(x, mid - std::max(0.5f, mx * half)), ImVec2(x, mid - std::min(-0.5f, mn * half)), theme::withAlpha(col, 0.45f), 1.0f);
    const float rh = std::max(0.5f, std::min(1.0f, rms * 1.41f) * half);
    dl->AddLine(ImVec2(x, mid - rh), ImVec2(x, mid + rh), col, 1.0f);
  }
  dl->AddLine(ImVec2(g0.x, mid), ImVec2(g1.x, mid), theme::withAlpha(P.paper, 0.10f), 1);
  // beat grid + bar numbers
  const int bpb = std::max(1, m.grid.beatsPerBar);
  const double b0 = std::ceil(beatAt(m.grid, ra) - 1e-6), b1 = beatAt(m.grid, rb);
  const float beatPx = w * static_cast<float>(1.0 / std::max(1e-6, b1 - beatAt(m.grid, ra)));
  for (double b = b0; b <= b1 + 1e-6; b += 1) {
    const float x = std::round(X(frameAt(m.grid, b))) + 0.5f;
    const long long rel = static_cast<long long>(std::llround(b)) - m.grid.bar1;
    const bool bar = ((rel % bpb) + bpb) % bpb == 0;
    if (!bar && beatPx < 7) continue;
    dl->AddLine(ImVec2(x, bar ? p0.y + 2 : g0.y), ImVec2(x, p1.y - 2), bar ? theme::withAlpha(P.paper, 0.28f) : theme::withAlpha(P.paper, 0.09f), 1);
    if (bar && b < b1 - 1e-6) {
      char bb[24];
      std::snprintf(bb, sizeof bb, "%lld", (rel - ((rel % bpb) + bpb) % bpb) / bpb + 1);
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 3, p0.y + 3), P.textDim, bb);
    }
    if (beatPx >= 48)
      for (int q = 1; q < 4; ++q) {
        const float xs = X(frameAt(m.grid, b + q * 0.25));
        if (xs < g1.x) dl->AddLine(ImVec2(xs, g1.y - 5), ImVec2(xs, g1.y), theme::withAlpha(P.paper, 0.12f), 1);
      }
  }
  // the body: select / audition a slice, double-click adds a cut, Alt+drag a bar line stretches the grid
  ImGui::SetCursorScreenPos(ImVec2(g0.x, p0.y + 2));
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##sdetail", ImVec2(w, p1.y - p0.y - 4));
  const bool hv = ImGui::IsItemHovered();
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const double fm = std::clamp(F(mp.x), ra, rb);
  int under = 0;
  for (int q = 0; q < n; ++q)
    if (m.slices[static_cast<size_t>(q)].start <= fm) under = q;
  if (ImGui::IsItemActivated()) {
    if (ImGui::GetIO().KeyAlt && editGrid) {
      // nearest bar line
      const double bf = beatAt(m.grid, fm);
      const double nearest = m.grid.bar1 + std::round((bf - m.grid.bar1) / bpb) * bpb;
      if (std::fabs(X(frameAt(m.grid, nearest)) - mp.x) < 8) e.dragBeat = static_cast<int>(nearest);
    } else {
      e.selSlice = under;
      auditionStart(a, t, e, under);
    }
  }
  if (ImGui::IsItemActive() && e.dragBeat >= 0) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    Model nm = e.m;
    gridStretchTo(nm.grid, e.dragBeat, fm);
    commit(a, t, e, nm, i18n::N_("Stretch the grid"), 0x5E17B0ull);
  }
  if (ImGui::IsItemDeactivated()) {
    if (e.dragBeat >= 0) {
      Model nm = e.m;
      rechopKeepPattern(nm, smp.get());
      commit(a, t, e, nm, i18n::N_("Stretch the grid"), 0x5E17B0ull);
    }
    e.dragBeat = -1;
    auditionStop(a, t, e);
  }
  if (editCuts && hv && ImGui::IsMouseDoubleClicked(0) && !ImGui::GetIO().KeyAlt) {
    Model nm = e.m;
    const int at = addCut(nm, snapFrame(nm, smp.get(), static_cast<int64_t>(fm), nm.chop.snap));
    if (at >= 0) {
      commit(a, t, e, nm, i18n::N_("Add a cut"));
      e.selSlice = at;
    }
  }
  if (hv && !ImGui::IsItemActive()) {
    const auto& s = m.slices.empty() ? Slice{} : m.slices[static_cast<size_t>(under)];
    char b[128];
    const int slot = sliceSlot(m, under);
    std::snprintf(b, sizeof b, "%s %d%s%s  ·  %.3f s", i18n::tr("Slice"), under + 1, slot >= 0 ? "  ·  " : "", slot >= 0 ? slotName(slot) : "",
                  static_cast<double>(s.end - s.start) / m.srcRate);
    const char* how = editCuts ? i18n::tr("Click: play. Double-click: add a cut. Drag a cut to move it, right-click it to remove it.")
                      : editGrid ? i18n::tr("Click: play. Drag the bar-1 flag to move the grid, Alt+drag a bar line to stretch it.")
                                 : i18n::tr("Click a slice to play it. Edit > Chop cuts by hand.");
    ui::Tooltip((std::string(b) + "\n" + how).c_str());
    ui::Tooltip(b);
  }
  // bar 1 flag (drag moves the whole grid)
  {
    const double f1 = frameAt(m.grid, m.grid.bar1);
    if (f1 >= ra && f1 <= rb) {
      const float x = X(f1);
      dl->AddRectFilled(ImVec2(x, p0.y + 2), ImVec2(x + 16, p0.y + 15), accent(), 2);
      textC(dl, a.fontMonoBold ? a.fontMonoBold : a.fontBold, theme::kFontLabel, ImVec2(x + 8, p0.y + 8.5f), theme::contrastText(accent()), "1");
      dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), accent(), 1.5f);
      ImGui::SetCursorScreenPos(ImVec2(x, p0.y + 2));
      if (editGrid) ImGui::InvisibleButton("##sbar1", ImVec2(16, 13));
      else ImGui::Dummy(ImVec2(16, 13));
      if (editGrid && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ui::Tooltip(i18n::tr("Bar 1: drag to move the whole grid onto the first downbeat"));
      }
      if (editGrid && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 1)) {
        Model nm = e.m;
        gridMoveDownbeat(nm.grid, fm);
        commit(a, t, e, nm, i18n::N_("Move bar 1"), 0x5E17B1ull);
      }
      if (editGrid && ImGui::IsItemDeactivated()) {
        Model nm = e.m;
        rechopKeepPattern(nm, smp.get());
        commit(a, t, e, nm, i18n::N_("Move bar 1"), 0x5E17B1ull);
      }
    }
  }
  // cut markers (thin lines; handles while editing cuts)
  for (int q = 1; q < n && !editCuts; ++q) {
    const float x = std::round(X(static_cast<double>(m.slices[static_cast<size_t>(q)].start))) + 0.5f;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, p1.y - 2), theme::withAlpha(P.paper, 0.35f), 1.0f);
  }
  for (int q = 1; q < n && editCuts; ++q) {
    const auto& s = m.slices[static_cast<size_t>(q)];
    const float x = std::round(X(static_cast<double>(s.start))) + 0.5f;
    ImGui::PushID(q);
    ImGui::SetCursorScreenPos(ImVec2(x - 4, p0.y + 2));
    ImGui::InvisibleButton("##scut", ImVec2(8, p1.y - p0.y - 4));
    const bool h = ImGui::IsItemHovered(), ac = ImGui::IsItemActive();
    if (h || ac) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (h && !ac) ui::Tooltip(i18n::tr("Drag to move the cut, right-click to remove it"));
    if (ac && ImGui::IsMouseDragging(0, 1)) {
      Model nm = e.m;
      moveCut(nm, q, snapFrame(nm, smp.get(), static_cast<int64_t>(fm), nm.chop.snap));
      commit(a, t, e, nm, i18n::N_("Move a cut"), 0x5E17C000ull + static_cast<uint64_t>(q));
    }
    if (h && ImGui::IsMouseClicked(1)) {
      Model nm = e.m;
      if (removeCut(nm, q)) {
        commit(a, t, e, nm, i18n::N_("Remove a cut"));
        e.selSlice = std::min(e.selSlice, static_cast<int>(nm.slices.size()) - 1);
      }
    }
    const ImU32 col = h || ac ? P.paper : theme::withAlpha(P.paper, 0.65f);
    dl->AddLine(ImVec2(x, g0.y - 4), ImVec2(x, p1.y - 2), col, h || ac ? 2.0f : 1.0f);
    dl->AddTriangleFilled(ImVec2(x - 4.5f, g0.y - 6), ImVec2(x + 4.5f, g0.y - 6), ImVec2(x, g0.y), col);
    ImGui::PopID();
    if (q >= n) break;
  }
  // slot letters on wide slices
  for (int q = 0; q < n; ++q) {
    const auto& s = m.slices[static_cast<size_t>(q)];
    const float x0 = X(static_cast<double>(s.start)), x1 = X(static_cast<double>(s.end));
    const int slot = sliceSlot(m, q);
    if (slot < 0 || x1 - x0 < 16) continue;
    const float fs = x1 - x0 >= 30 ? 15.0f : 12.0f;
    dl->AddText(a.fontBold, fs, ImVec2(x0 + 5, g1.y - fs - 3), slotColor(slot), slotName(slot));
  }
  // playhead
  if (lv.srcFrame >= ra && lv.srcFrame <= rb) {
    const float x = X(lv.srcFrame);
    dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), P.playhead, 2.0f);
  }
  // caption: region
  {
    char b[96];
    const int bars = static_cast<int>(std::lround((beatAt(m.grid, rb) - beatAt(m.grid, ra)) / bpb));
    std::snprintf(b, sizeof b, "%d %s  ·  %d %s  ·  %s", bars, bars == 1 ? i18n::tr("bar") : i18n::tr("bars"), n,
                  n == 1 ? i18n::tr("slice") : i18n::tr("slices"), fmtTime((rb - ra) / m.srcRate).c_str());
    const float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, b).x;
    dl->AddRectFilled(ImVec2(p1.x - 16 - tw, p0.y + 2), ImVec2(p1.x - 4, p0.y + 16), theme::withAlpha(P.well, 0.92f), theme::kRadiusSm);
    textR(dl, a.fontMono, theme::kFontLabel, ImVec2(p1.x - 10, p0.y + 3), P.textDim, b);
  }
}

}  // namespace gl::packs::seltmann::sui
