// Gridline — "edison" pack: the waveform / spectrogram editor — time ruler with
// markers, regions and loop points, stereo lanes drawn from the peak pyramid,
// selection with snapping, scrubbing, spectral brush, overview scroller and
// the bottom bar with view options and level / pitch readouts. J-207: every
// size from edison_style.h (one row of equal-height controls in the bar).
#include "../../app/GlEpoch.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "edison_style.h"
#include "edison_ui.h"
#include "../../app/SmoothScrollUi.h"
#include "../../app/LiveWaveUi.h"
#include "../../engine/Dsp.h"

namespace gl::packs::edison {

namespace {

struct Geo {
  ImVec2 p0, p1;  // lanes
  double start, len;
  float W() const { return p1.x - p0.x; }
  double frameAt(float x) const { return start + (x - p0.x) / std::max(1.f, W()) * len; }
  float xOf(double f) const { return p0.x + static_cast<float>((f - start) / len) * W(); }
};

constexpr double kMinHz = 30.0;

double hzAtY(const Session& s, float y, float y0, float y1) {
  const double ny = s.rate() * 0.5;
  const double t = std::clamp((y1 - y) / std::max(1.f, y1 - y0), 0.f, 1.f);
  return kMinHz * std::pow(ny / kMinHz, t);
}
float yOfHz(const Session& s, double hz, float y0, float y1) {
  const double ny = s.rate() * 0.5;
  const double t = std::log(std::max(kMinHz, hz) / kMinHz) / std::log(ny / kMinHz);
  return y1 - static_cast<float>(t) * (y1 - y0);
}

int64_t snapFrame(App& a, const Session& s, const Geo& g, double f) {
  int64_t p = std::clamp<int64_t>(static_cast<int64_t>(std::llround(f)), 0, s.frames());
  if (ImGui::GetIO().KeyShift && ImGui::GetIO().KeyAlt) return p;
  switch (s.snap) {
    case 1: return nearestZeroCrossing(s.cur().a(), p, static_cast<int64_t>(s.rate() * 0.004));
    case 2: {
      int64_t best = p;
      float bd = 8;
      for (const auto& m : s.cur().markers)
        if (std::fabs(g.xOf(static_cast<double>(m.pos)) - g.xOf(f)) < bd) bd = std::fabs(g.xOf(static_cast<double>(m.pos)) - g.xOf(f)), best = m.pos;
      return best;
    }
    case 3: {
      const double beat = s.rate() * 60.0 / std::max(10.0, a.doc.song().tempo);
      return std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::round(f / beat) * beat)), 0, s.frames());
    }
    default: return p;
  }
}

// Spectrogram colour map: dark slate → green → amber → white.
ImU32 heat(uint8_t v) {
  const float t = std::clamp((v / 255.f - 0.3f) / 0.7f, 0.f, 1.f);  // -84 dB and below = background
  const float stops[5][4] = {{0.f, 0x1C, 0x21, 0x24}, {0.45f, 0x23, 0x4D, 0x3A}, {0.7f, 0x8F, 0xD3, 0xA0}, {0.88f, 0xE8, 0xC4, 0x6A}, {1.f, 0xFF, 0xFF, 0xFF}};
  for (int k = 1; k < 5; ++k)
    if (t <= stops[k][0]) {
      float u = (t - stops[k - 1][0]) / (stops[k][0] - stops[k - 1][0]);
      auto lerp = [&](int c) { return static_cast<int>(stops[k - 1][c] + (stops[k][c] - stops[k - 1][c]) * u); };
      return IM_COL32(lerp(1), lerp(2), lerp(3), 255);
    }
  return IM_COL32(255, 255, 255, 255);
}

void updateSpectrogram(Session& s) {
  if (s.tex && s.texEpoch != glepoch::current()) {  // K-47: a driver reset took it
    s.tex = 0;
    s.texDirty = true;
  }
  const uint64_t serial = s.hist.serial();
  if (s.specJob.valid() && s.specJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    s.spec = s.specJob.get();
    s.texDirty = true;
  }
  if (s.specSerial != serial && !s.specJob.valid()) {
    s.specSerial = serial;
    AudioPtr a = s.cur().audio;
    s.specJob = std::async(std::launch::async, [a]() { return a ? computeSpectrogram(*a, 2048, 4096) : Spectrogram{}; });
  }
  if (!s.texDirty || s.spec.cols == 0) return;
  s.texDirty = false;
  // Rows on a log-frequency axis (bottom = 30 Hz, top = Nyquist).
  const int H = 256, W = s.spec.cols;
  std::vector<uint32_t> px(static_cast<size_t>(W) * H);
  const double ny = s.spec.rate * 0.5;
  for (int r = 0; r < H; ++r) {
    const double hz = kMinHz * std::pow(ny / kMinHz, static_cast<double>(H - 1 - r) / (H - 1));
    const int bin = std::clamp(static_cast<int>(hz / ny * s.spec.bins), 0, s.spec.bins - 1);
    for (int c = 0; c < W; ++c) px[static_cast<size_t>(r) * W + c] = heat(s.spec.at(c, bin));
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  if (!s.tex) {
    GLuint t = 0;
    glGenTextures(1, &t);
    s.tex = t;
    s.texEpoch = glepoch::current();
  }
  glBindTexture(GL_TEXTURE_2D, s.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  s.texW = W;
  s.texH = H;
}

void updateLevels(Session& s) {
  if (s.levelsJob.valid() && s.levelsJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto r = s.levelsJob.get();
    s.levels = r.first;
    s.pitch = r.second;
  }
  const Range r = target(s);
  const uint64_t key = s.hist.serial() * 1000003ull ^ static_cast<uint64_t>(r.a) * 31ull ^ static_cast<uint64_t>(r.b);
  if (key == s.levelsKey || s.dragging || s.levelsJob.valid() || s.frames() == 0) return;
  s.levelsKey = key;
  AudioPtr a = s.cur().audio;
  s.levelsJob = std::async(std::launch::async, [a, r]() {
    Levels L = measure(*a, r);
    Range pr{r.a, std::min(r.b, r.a + static_cast<int64_t>(a->rate * 2))};
    return std::make_pair(L, detectPitch(*a, pr));
  });
}

void drawLanes(App& a, Session& s, ImDrawList* dl, const Geo& g, float scale) {
  const Audio& au = s.cur().a();
  const int lanes = au.channels == 1 ? 1 : 2;
  const float laneH = (g.p1.y - g.p0.y) / lanes;
  const int W = static_cast<int>(g.W());
  const Range sel = s.sel;
  const float selX0 = sel.empty() ? 1e9f : g.xOf(static_cast<double>(sel.a)), selX1 = sel.empty() ? -1e9f : g.xOf(static_cast<double>(sel.b));
  for (int ln = 0; ln < lanes; ++ln) {
    const float y0 = g.p0.y + ln * laneH, y1 = y0 + laneH, mid = (y0 + y1) * 0.5f, half = laneH * 0.46f * s.vZoom;
    dl->AddLine(ImVec2(g.p0.x, mid), ImVec2(g.p1.x, mid), C(col::WaveGridHi), 1.f);
    for (float q : {0.5f, -0.5f}) dl->AddLine(ImVec2(g.p0.x, mid - q * half), ImVec2(g.p1.x, mid - q * half), C(col::WaveGrid), 1.f);
    if (ln > 0) dl->AddLine(ImVec2(g.p0.x, y0), ImVec2(g.p1.x, y0), C(col::BtnLo), 1.f);
    if (au.frames() == 0) continue;
    const double fpp = g.len / std::max(1, W);
    if (fpp >= 1.0) {
      for (int x = 0; x < W; ++x) {
        const double f0 = g.start + x * fpp, f1 = f0 + fpp;
        if (f1 < 0 || f0 >= static_cast<double>(au.frames())) continue;
        float mn, mx;
        s.peaks.query(au, ln, static_cast<int64_t>(f0), static_cast<int64_t>(std::ceil(f1)), mn, mx);
        const float px = g.p0.x + x + 0.5f;
        const ImU32 c = px >= selX0 && px <= selX1 ? C(col::WaveSel) : C(col::Wave);
        float ya = mid - std::clamp(mx, -1.f, 1.f) * half, yb = mid - std::clamp(mn, -1.f, 1.f) * half;
        if (yb - ya < 1.f) ya -= 0.5f, yb += 0.5f;
        dl->AddLine(ImVec2(px, std::max(y0, ya)), ImVec2(px, std::min(y1, yb)), c, 1.f);
      }
    } else {
      const int64_t i0 = std::max<int64_t>(0, static_cast<int64_t>(g.start) - 1);
      const int64_t i1 = std::min<int64_t>(au.frames(), static_cast<int64_t>(g.start + g.len) + 2);
      std::vector<ImVec2> pts;
      pts.reserve(static_cast<size_t>(i1 - i0));
      const float* x = au.ch(ln);
      for (int64_t i = i0; i < i1; ++i) pts.push_back(ImVec2(g.xOf(static_cast<double>(i)), std::clamp(mid - x[i] * half, y0, y1)));
      if (pts.size() > 1) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), C(col::Wave), 0, std::max(1.f, scale * 0.8f));
      if (fpp < 0.2)
        for (auto& p : pts) dl->AddCircleFilled(p, std::max(1.5f, scale * 1.4f), C(col::WaveSel));
    }
  }
  (void)a;
}

// K-145 dev checks: GRIDLINE_EDISON_LIVETRACE=1 prints the live view every frame (frames
// recorded, drawn head, span, the head's x in the lanes, their width) and the normal view
// once a take has become the file.
void devLiveTrace(const Session& s, const Geo& g, bool live, float pad) {
  static const bool on = std::getenv("GRIDLINE_EDISON_LIVETRACE") != nullptr;
  if (!on) return;
  if (live) {
    const float headX = g.xOf(s.fit.shown) - g.p0.x;
    // gap: px between the head and the edge less the pad (0 once the take is fitted); fit: past the edge
    std::printf("loupe: live frames %zu shown %.0f span %.0f base %.0f head %.2f width %.2f pad %.2f gap %.3f fit %d rate %.0f\n", s.live.frames(),
                s.fit.shown, g.len, s.fit.base, static_cast<double>(headX), static_cast<double>(g.W()), static_cast<double>(pad),
                static_cast<double>(g.W() - pad - headX), g.len > s.fit.base ? 1 : 0, s.live.rate());
  } else {
    static double lastStart = -1, lastLen = -1;
    static int64_t lastFrames = -1;
    if (g.start == lastStart && g.len == lastLen && s.frames() == lastFrames) return;
    lastStart = g.start, lastLen = g.len, lastFrames = s.frames();
    std::printf("loupe: view start %.0f len %.0f frames %lld spectral %d\n", g.start, g.len, static_cast<long long>(s.frames()), s.spectral ? 1 : 0);
  }
  std::fflush(stdout);
}

// K-145: the take being recorded, one lane per channel, in the lanes' own colours; the
// head (the drawn end) in the record colour.
void drawLiveLanes(Session& s, ImDrawList* dl, const Geo& g, float scale) {
  const float laneH = (g.p1.y - g.p0.y) / 2;
  LiveWaveView v;
  v.start = g.start;
  v.span = g.len;
  v.end = s.fit.shown;
  v.pxScale = ImGui::GetIO().DisplayFramebufferScale.x;
  for (int ln = 0; ln < 2; ++ln) {
    const float y0 = g.p0.y + ln * laneH, y1 = y0 + laneH, mid = (y0 + y1) * 0.5f, half = laneH * 0.46f * s.vZoom;
    dl->AddLine(ImVec2(g.p0.x, mid), ImVec2(g.p1.x, mid), C(col::WaveGridHi), 1.f);
    for (float q : {0.5f, -0.5f}) dl->AddLine(ImVec2(g.p0.x, mid - q * half), ImVec2(g.p1.x, mid - q * half), C(col::WaveGrid), 1.f);
    if (ln > 0) dl->AddLine(ImVec2(g.p0.x, y0), ImVec2(g.p1.x, y0), C(col::BtnLo), 1.f);
    livewaveui::Style st;
    st.wave = C(col::Wave);
    st.channel = ln;
    st.gain = s.vZoom;
    st.head = C(col::Rec);
    st.headWidth = std::max(1.5f, scale * 2.0f);
    livewaveui::drawLane(dl, s.live, ImVec2(g.p0.x, y0), ImVec2(g.p1.x, y1), v, st);
  }
}

void drawSpectral(Session& s, ImDrawList* dl, const Geo& g, ImFont* fm, float fs) {
  updateSpectrogram(s);
  if (s.tex && s.spec.cols > 0) {
    const double total = static_cast<double>(s.spec.cols) * static_cast<double>(s.spec.hop);
    const double u0 = (g.start + s.spec.hop * 0.5) / total, u1 = (g.start + g.len + s.spec.hop * 0.5) / total;
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(s.tex)), g.p0, g.p1, ImVec2(static_cast<float>(u0), 0), ImVec2(static_cast<float>(u1), 1));
  } else if (s.frames() > 0) {
    sty::textAt(dl, fm, fs, (g.p0.x + g.p1.x) * 0.5f, (g.p0.y + g.p1.y) * 0.5f, C(col::TextDim), i18n::tr("Computing the spectrogram..."), 1);
  }
  // Frequency labels (a tick and the value on its line).
  for (double hz : {100.0, 1000.0, 10000.0}) {
    if (hz >= s.rate() * 0.5) continue;
    const float y = std::round(yOfHz(s, hz, g.p0.y, g.p1.y)) + 0.5f;
    dl->AddLine(ImVec2(g.p0.x, y), ImVec2(g.p0.x + 8, y), C(col::Text, 0.6f));
    char b[16];
    std::snprintf(b, sizeof b, hz >= 1000 ? "%.0fk" : "%.0f", hz >= 1000 ? hz / 1000 : hz);
    sty::textAt(dl, fm, fs, g.p0.x + 11, y, C(col::Text, 0.7f), b, 0);
  }
  // Brush strokes.
  for (const auto& q : s.brush) {
    ImVec2 a(g.xOf(static_cast<double>(q.f0)), yOfHz(s, q.hz1, g.p0.y, g.p1.y)), b(g.xOf(static_cast<double>(q.f1)), yOfHz(s, q.hz0, g.p0.y, g.p1.y));
    dl->AddRectFilled(a, b, C(col::Brush, 0.38f));
    dl->AddRect(a, b, C(col::Brush, 0.8f));
  }
}

}  // namespace

void drawWaveArea(App& a, Session& s, ImVec2 p0, ImVec2 p1, const sty::Metrics& M) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* fm = a.fontMono ? a.fontMono : ImGui::GetFont();
  ImFont* fr = a.fontRegular ? a.fontRegular : ImGui::GetFont();
  ImFont* fb = a.fontBold ? a.fontBold : ImGui::GetFont();
  auto P = [&M](float v) { return M.px(v); };
  const float rulerH = P(sty::kRulerH), scrollH = P(sty::kScrollH), barH = P(sty::kBarH), sideW = P(sty::kSideW);
  const float fs = M.font(11), fsCap = M.font(sty::kFontCap), fsVal = M.font(sty::kFontValue);
  // Columns: the lanes, ruler and overview end where the auto-scroll strip begins.
  const float sideX = p1.x - sideW;
  const float barY = p1.y - barH;
  const ImVec2 r0 = p0, r1(sideX, p0.y + rulerH);
  Geo g{ImVec2(p0.x, r1.y), ImVec2(sideX, barY - scrollH), 0, 0};
  const ImVec2 sc0(p0.x, g.p1.y), sc1(sideX, barY);
  const ImVec2 b0(p0.x, barY), b1(p1.x, p1.y);

  if (s.peaksSerial != s.hist.serial()) {
    s.peaks.build(s.cur().a());
    s.peaksSerial = s.hist.serial();
  }
  if (s.viewLen <= 0) zoomAll(s);
  const double n = static_cast<double>(s.frames());
  // K-145: while a take records, the lanes show it from its start: at the normal scale while
  // it fits, then the whole take, fitted to the width every frame (a few px after the head).
  const bool live = s.fit.active;
  // Auto-scroll with the playhead.
  const double pos = live ? -1.0 : playPos(a, s);
  if (autoScroll(a) && pos >= 0 && !s.dragging && (pos < s.viewStart || pos > s.viewStart + s.viewLen * 0.98))
    s.viewStart = std::clamp(pos - s.viewLen * 0.05, 0.0, std::max(0.0, n - s.viewLen));
  g.start = live ? 0.0 : s.viewStart;
  g.len = live ? s.fit.span(g.W(), P(8)) : std::max(1.0, s.viewLen);
  const double vr = viewRate(s);
  devLiveTrace(s, g, live, P(8));
  s.devLx0 = g.p0.x, s.devLx1 = g.p1.x, s.devLy0 = g.p0.y, s.devLy1 = g.p1.y, s.devRy0 = r0.y, s.devRy1 = r1.y;
  s.devViewStart = g.start, s.devViewLen = g.len;

  // ---- backgrounds
  dl->AddRectFilled(r0, r1, C(col::RulerBg));
  dl->AddRectFilled(g.p0, g.p1, C(col::WaveBg));
  dl->AddLine(ImVec2(r0.x, r1.y - 0.5f), ImVec2(r1.x, r1.y - 0.5f), C(col::BtnLo), 1.f);
  // Selection. Its edges can be dragged: the one under the pointer (or being dragged) lights up.
  const ImVec2 mouseNow = ImGui::GetIO().MousePos;
  int hoverEdge = 0;
  if (!s.sel.empty() && !live && !s.dragging && !s.scrubbing && !s.scrubMode && !(s.spectral && s.brushMode) && !ImGui::GetIO().KeyAlt &&
      ImGui::IsMouseHoveringRect(g.p0, g.p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    hoverEdge = transport::edgeAt(mouseNow.x, g.xOf(static_cast<double>(s.sel.a)), g.xOf(static_cast<double>(s.sel.b)), P(5));
  if (!s.sel.empty() && !live) {
    const float e0 = g.xOf(static_cast<double>(s.sel.a)), e1 = g.xOf(static_cast<double>(s.sel.b));
    float x0 = std::max(g.p0.x, e0), x1 = std::min(g.p1.x, e1);
    if (x1 > x0) {
      dl->AddRectFilled(ImVec2(x0, g.p0.y), ImVec2(x1, g.p1.y), C(col::SelBg));
      dl->AddRectFilled(ImVec2(x0, r0.y), ImVec2(x1, r1.y - 1), C(col::SelBg, 0.8f));
      for (int e = 1; e <= 2; ++e) {
        const float x = std::round(e == 1 ? e0 : e1);
        if (x < g.p0.x - 1 || x > g.p1.x + 1) continue;
        const bool hot = hoverEdge == e || s.edgeDrag == e;
        dl->AddLine(ImVec2(x + 0.5f, g.p0.y), ImVec2(x + 0.5f, g.p1.y), C(col::SelEdge, hot ? 1.f : 0.55f), hot ? std::max(2.f, P(2)) : 1.f);
        // A small grip on the edge, at the top of the lanes: the edge can be dragged.
        const float gw = std::max(2.f, P(3)), gh = P(14);
        dl->AddRectFilled(ImVec2(x + 0.5f - gw * 0.5f, g.p0.y), ImVec2(x + 0.5f + gw * 0.5f, g.p0.y + gh), C(col::SelEdge, hot ? 1.f : 0.8f), 1.f);
      }
    }
  }
  // Time grid + labels (none while empty: there is no time to show).
  if (s.frames() > 0 || live) {
    const double secPerPx = g.len / vr / std::max(1.f, g.W());
    const float minStep = P(80);
    double step;
    if (s.barsTime) {
      const double beat = 60.0 / std::max(10.0, a.doc.song().tempo);
      step = beat;
      while (step / secPerPx < minStep) step *= 2;
    } else {
      static const double kSteps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
      step = 600;
      for (double st : kSteps)
        if (st / secPerPx >= minStep) {
          step = st;
          break;
        }
    }
    const double t0 = g.start / vr, t1 = (g.start + g.len) / vr;
    dl->PushClipRect(r0, g.p1, true);
    const float tickCy = r0.y + (rulerH - P(4)) * 0.5f;  // labels centred in the ruler above the region band
    for (double t = std::floor(t0 / step) * step; t <= t1; t += step) {
      const float x = std::round(g.xOf(t * vr)) + 0.5f;
      dl->AddLine(ImVec2(x, r1.y - P(6)), ImVec2(x, r1.y), C(col::RulerText, 0.7f));
      dl->AddLine(ImVec2(x, g.p0.y), ImVec2(x, g.p1.y), C(col::WaveGrid), 1.f);
      std::string lbl = timeText(a, s, static_cast<int64_t>(std::llround(t * vr)), step < 0.1);
      sty::textAt(dl, fm, fs, x + P(4), tickCy, C(col::RulerText), lbl.c_str(), 0);
    }
    dl->PopClipRect();
  }

  // ---- lanes
  dl->PushClipRect(g.p0, g.p1, true);
  if (live) {
    drawLiveLanes(s, dl, g, M.s);  // (the spectrogram comes back with the finished take)
  } else if (s.spectral) {
    drawSpectral(s, dl, g, fm, fs);
    if (!s.sel.empty())
      dl->AddRectFilled(ImVec2(g.xOf(static_cast<double>(s.sel.a)), g.p0.y), ImVec2(g.xOf(static_cast<double>(s.sel.b)), g.p1.y), C(col::SelEdge, 0.18f));
  } else {
    drawLanes(a, s, dl, g, M.s);
  }
  if (s.frames() == 0 && !s.job.active() && !live) {
    // Empty state: what Loupe is waiting for, centred in the lanes.
    const char* head = s.slot ? i18n::tr("No audio yet") : i18n::tr("This sample is empty");
    const char* sub = s.slot ? i18n::tr("Record from the mixer input (R), load a file from the File menu, or drop one here")
                             : i18n::tr("Load a file from the File menu, or drop one here");
    const float cy = std::round((g.p0.y + g.p1.y) * 0.5f), cx = std::round((g.p0.x + g.p1.x) * 0.5f);
    ui::drawIcon(dl, ui::Icon::Wave, ImVec2(cx, cy - P(34)), P(28), C(col::TextDim, 0.55f));
    sty::textAt(dl, fb, M.font(15), cx, cy, C(col::Text, 0.85f), head, 1);
    sty::textAt(dl, fr, M.font(12.5f), cx, cy + P(24), C(col::TextDim), sub, 1);
  }
  // Regions (band in the ruler + tint), loop, markers, cursor, playhead.
  const Content& c = s.cur();
  dl->PopClipRect();
  dl->PushClipRect(r0, g.p1, true);
  static const Content kNone;
  const Content& shownC = live ? kNone : c;  // the old file's marks go while a take replaces it
  for (const auto& rg : shownC.regions) {
    float x0 = g.xOf(static_cast<double>(rg.start)), x1 = g.xOf(static_cast<double>(rg.end));
    dl->AddRectFilled(ImVec2(x0, r1.y - P(4)), ImVec2(x1, r1.y), C(col::Region, 0.9f));
    dl->AddRectFilled(ImVec2(x0, g.p0.y), ImVec2(x1, g.p1.y), C(col::Region, 0.07f));
    sty::textAt(dl, fm, fs, x0 + P(4), g.p1.y - P(4) - fs * 0.5f, C(col::Region), rg.name.c_str(), 0);
  }
  if (shownC.hasLoop()) {
    float x0 = g.xOf(static_cast<double>(c.loopStart)), x1 = g.xOf(static_cast<double>(c.loopEnd));
    dl->AddRectFilled(ImVec2(x0, r0.y), ImVec2(x1, r0.y + P(3)), C(col::Loop));
    for (float x : {x0, x1}) dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Loop, 0.8f), 1.f);
  }
  int hoverMarker = -1;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  for (size_t k = 0; k < shownC.markers.size(); ++k) {
    const auto& m = c.markers[k];
    const float x = std::round(g.xOf(static_cast<double>(m.pos))) + 0.5f;
    if (x < g.p0.x - 50 || x > g.p1.x) continue;
    const bool hot = std::fabs(mouse.x - x) < 5 && mouse.y >= r0.y && mouse.y < r1.y;
    if (hot) hoverMarker = static_cast<int>(k);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Marker, hot || s.dragMarker == static_cast<int>(k) ? 1.f : 0.75f), 1.f);
    const float fh = std::round(rulerH * 0.55f);
    dl->AddTriangleFilled(ImVec2(x, r0.y + 1), ImVec2(x + fh * 0.9f, r0.y + 1 + fh * 0.5f), ImVec2(x, r0.y + 1 + fh), C(col::Marker));
    sty::textAt(dl, fm, fs, x + P(4), g.p0.y + P(4) + fs * 0.5f, C(col::Marker), m.name.c_str(), 0);
  }
  if (s.cursor >= g.start && s.cursor <= g.start + g.len && s.frames() > 0 && !live) {
    const float x = std::round(g.xOf(static_cast<double>(s.cursor))) + 0.5f;
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Cursor, 0.75f), 1.f);
  }
  if (pos >= 0) {
    const float x = g.xOf(pos);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Playhead), std::max(1.5f, P(2)));
  }
  dl->PopClipRect();

  // ---- interaction: ruler (none while a take records: it edits the file it replaces)
  ImGui::BeginDisabled(live);
  ImGui::SetCursorScreenPos(r0);
  ImGui::InvisibleButton("ruler", ImVec2(std::max(1.f, r1.x - r0.x), std::max(1.f, r1.y - r0.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool busy = s.job.active();
  static float rulerPressX = 0;  // a drag along the ruler scrubs (a click sets the play position)
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hoverMarker >= 0) {
      s.dragMarker = hoverMarker;
    } else {
      rulerPressX = mouse.x;
      s.cursor = snapFrame(a, s, g, g.frameAt(mouse.x));
      s.sel = {};
      if (isPlaying(a, s)) play(a, s);  // playing: it jumps there
    }
  }
  if (s.dragMarker < 0 && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && s.frames() > 1) {
    const double f = std::clamp(g.frameAt(mouse.x), 0.0, n);
    if (!s.scrubbing && std::fabs(mouse.x - rulerPressX) > P(3)) scrubBegin(a, s, f);
    else if (s.scrubbing) scrubMove(a, s, f);
  }
  if (ImGui::IsItemDeactivated() && s.scrubbing) scrubEnd(a, s);
  if (hoverMarker < 0 && ImGui::IsItemHovered() && s.frames() > 0)
    a.setHint(i18n::tr("Ruler: click to set the play position (playing: jump there), drag to scrub"));
  if (s.dragMarker >= 0 && ImGui::IsItemActive() && static_cast<size_t>(s.dragMarker) < c.markers.size()) {
    // J-67: previewed where it will land (the snapped frame), gliding between snap points.
    const double to = static_cast<double>(snapFrame(a, s, g, g.frameAt(std::clamp(mouse.x, g.p0.x, g.p1.x))));
    float x = std::clamp(g.xOf(scrollui::glide(0x45444D4Bull, to, s.snap != 0, 0.5)), g.p0.x, g.p1.x);
    dl->AddLine(ImVec2(x, r0.y), ImVec2(x, g.p1.y), C(col::Marker), 2.f);
  }
  if (s.dragMarker >= 0 && ImGui::IsItemDeactivated()) {
    if (static_cast<size_t>(s.dragMarker) < c.markers.size()) {
      int64_t to = snapFrame(a, s, g, g.frameAt(std::clamp(mouse.x, g.p0.x, g.p1.x)));
      if (to != c.markers[static_cast<size_t>(s.dragMarker)].pos) applyNow(a, s, "Move marker", moveMarker(c, static_cast<size_t>(s.dragMarker), to));
    }
    s.dragMarker = -1;
  }
  if (hoverMarker >= 0 && ImGui::IsItemHovered()) {
    a.setHint("Marker: drag to move, double-click to rename, right-click for options");
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      s.renameMarker = hoverMarker;
      s.renameRegion = -1;
      std::snprintf(s.renameBuf, sizeof s.renameBuf, "%s", c.markers[static_cast<size_t>(hoverMarker)].name.c_str());
      openTool(s, Tool::Rename);
    }
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hoverMarker >= 0) {
    s.renameMarker = hoverMarker;
    ImGui::OpenPopup("markermenu");
  }
  if (ImGui::BeginPopup("markermenu")) {
    if (s.renameMarker >= 0 && static_cast<size_t>(s.renameMarker) < c.markers.size()) {
      ImGui::TextDisabled("%s", c.markers[static_cast<size_t>(s.renameMarker)].name.c_str());
      if (ImGui::MenuItem(i18n::tr("Rename..."))) {
        std::snprintf(s.renameBuf, sizeof s.renameBuf, "%s", c.markers[static_cast<size_t>(s.renameMarker)].name.c_str());
        s.renameRegion = -1;
        openTool(s, Tool::Rename);
      }
      if (ImGui::MenuItem(i18n::tr("Snap to zero crossing"))) {
        const auto& m = c.markers[static_cast<size_t>(s.renameMarker)];
        applyNow(a, s, "Snap marker", moveMarker(c, static_cast<size_t>(s.renameMarker), nearestZeroCrossing(c.a(), m.pos, static_cast<int64_t>(s.rate() * 0.01))));
      }
      if (ImGui::MenuItem(i18n::tr("Select to next marker"))) {
        const int64_t from = c.markers[static_cast<size_t>(s.renameMarker)].pos;
        const int64_t to = static_cast<size_t>(s.renameMarker) + 1 < c.markers.size() ? c.markers[static_cast<size_t>(s.renameMarker) + 1].pos : s.frames();
        s.sel = {from, to};
      }
      if (ImGui::MenuItem(i18n::tr("Remove"))) applyNow(a, s, "Remove marker", removeMarker(c, static_cast<size_t>(s.renameMarker)));
    }
    ImGui::EndPopup();
  }

  // ---- interaction: lanes
  ImGui::SetCursorScreenPos(g.p0);
  ImGui::InvisibleButton("lanes", ImVec2(std::max(1.f, g.W()), std::max(1.f, g.p1.y - g.p0.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  // J-97: an audio file dragged from the browser (or another application) onto the
  // waveform loads into Loupe, like "Load audio file..." (one step of Loupe's undo).
  // Loupe's own drag out never lands back on its own waveform (that only made an undo step).
  if (!s.job.active() && s.dragOut.empty() && ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
      const std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      const std::string name = std::filesystem::path(path).filename().string();
      if (pl->Delivery) {
        runEdit(a, s, "Load " + name, [path](const Content& c) {
          Content n;
          return loadContent(path, n) ? n : c;
        });
      } else {
        char buf[320];
        std::snprintf(buf, sizeof buf, i18n::tr("Drop to load %s into Loupe"), name.c_str());
        ImGui::SetTooltip("%s", buf);
        a.setHint(buf);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
    }
    ImGui::EndDragDropTarget();
  }
  const bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  const double fm0 = std::clamp(g.frameAt(mouse.x), 0.0, n);
  if (hoverEdge != 0 && hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    a.setHint(i18n::tr("Selection edge: drag to move it (Shift-click elsewhere also extends the selection)"));
  }
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && s.frames() > 0) {
    s.edgeDrag = 0;
    if (s.spectral && s.brushMode) {
      s.dragging = true;
    } else if (s.scrubMode || io.KeyAlt) {
      scrubBegin(a, s, fm0);
    } else if (hoverEdge != 0 && !io.KeyShift) {
      // Grab a selection edge: the other edge stays where it is.
      s.edgeDrag = hoverEdge;
      s.anchor = hoverEdge == 1 ? s.sel.b : s.sel.a;
      s.dragging = true;
    } else {
      const int64_t f = snapFrame(a, s, g, fm0);
      if (io.KeyShift && !s.sel.empty()) {
        s.anchor = std::llabs(f - s.sel.a) > std::llabs(f - s.sel.b) ? s.sel.a : s.sel.b;
        s.sel = {std::min(s.anchor, f), std::max(s.anchor, f)};
      } else if (io.KeyShift && s.sel.empty()) {  // from the cursor to here
        s.anchor = s.cursor;
        s.sel = {std::min(s.anchor, f), std::max(s.anchor, f)};
      } else {
        s.anchor = f;
        s.cursor = f;
        s.sel = {};
        if (isPlaying(a, s)) play(a, s);  // playing: the playback jumps to the click
      }
      s.dragging = true;
    }
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (s.scrubbing) {
      scrubMove(a, s, fm0);
    } else if (s.dragging && s.spectral && s.brushMode) {
      // Paint: one cell per ~1/3 octave × 20 ms around the mouse.
      const double hz = hzAtY(s, mouse.y, g.p0.y, g.p1.y);
      const int64_t w = static_cast<int64_t>(std::max(s.rate() * 0.02, g.len / g.W() * 6));
      SpectralRect q{static_cast<int64_t>(fm0) - w / 2, static_cast<int64_t>(fm0) + w / 2, hz / 1.12, hz * 1.12};
      if (s.brush.empty()) {
        s.brush.push_back(q);
      } else {  // J-99: stamp along the path from the last cell so fast strokes leave no gaps
        const SpectralRect b = s.brush.back();
        const double df = static_cast<double>(q.f0 - b.f0), dlog = std::log(q.hz0 / b.hz0);
        const int steps = std::min(512, static_cast<int>(std::max(std::fabs(df) / std::max<double>(1.0, w / 3.0), std::fabs(dlog) / 0.04)));
        for (int k = 1; k <= steps; ++k) {
          const double u = static_cast<double>(k) / steps;
          const int64_t f0 = b.f0 + static_cast<int64_t>(std::llround(df * u));
          const double h0 = b.hz0 * std::exp(dlog * u);
          s.brush.push_back(SpectralRect{f0, f0 + w, h0, h0 * (q.hz1 / q.hz0)});
        }
      }
    } else if (s.dragging) {
      const int64_t f = snapFrame(a, s, g, fm0);
      s.sel = {std::min(s.anchor, f), std::max(s.anchor, f)};
      // Drag past the edges scrolls.
      if (mouse.x > g.p1.x) s.viewStart = std::min(std::max(0.0, n - s.viewLen), s.viewStart + s.viewLen * 0.02);
      if (mouse.x < g.p0.x) s.viewStart = std::max(0.0, s.viewStart - s.viewLen * 0.02);
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (s.scrubbing) scrubEnd(a, s);
    // A click (a new selection narrower than 3 px) only sets the play position.
    const bool tiny = s.sel.len() < 2 || (s.edgeDrag == 0 && !io.KeyShift && g.xOf(static_cast<double>(s.sel.b)) - g.xOf(static_cast<double>(s.sel.a)) < P(3));
    if (s.dragging && !(s.spectral && s.brushMode) && tiny) s.sel = {};
    s.dragging = false;
    s.edgeDrag = 0;
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !(s.spectral && s.brushMode)) {
    // Double-click: the region under the mouse, else the span between markers.
    const int64_t f = static_cast<int64_t>(fm0);
    Range r{0, s.frames()};
    bool found = false;
    for (const auto& rg : c.regions)
      if (f >= rg.start && f < rg.end) r = {rg.start, rg.end}, found = true;
    if (!found)
      for (const auto& m : c.markers) {
        if (m.pos <= f) r.a = std::max(r.a, m.pos);
        if (m.pos > f) r.b = std::min(r.b, m.pos);
      }
    s.sel = r;
  }
  double wheelFrames = 0;  // J-67: horizontal scroll input this frame (frames), eased by the smooth scroller
  bool wheelNotch = false;
  if (hovered) {
    if (io.MouseWheel != 0) {
      if (io.KeyCtrl || io.KeySuper)
        s.vZoom = std::clamp(s.vZoom * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.25f, 16.f);
      else if (io.KeyShift)
        wheelFrames -= io.MouseWheel * s.viewLen * 0.1, wheelNotch = smoothscroll::isNotch(io.MouseWheel);
      else
        zoomBy(s, io.MouseWheel > 0 ? 0.8 : 1.25, fm0);
    }
    if (io.MouseWheelH != 0) wheelFrames += io.MouseWheelH * s.viewLen * 0.1, wheelNotch = smoothscroll::isNotch(io.MouseWheelH);
    char b[256];
    if (s.spectral)
      std::snprintf(b, sizeof b, "%s  %.0f Hz%s", timeText(a, s, static_cast<int64_t>(fm0)).c_str(), hzAtY(s, mouse.y, g.p0.y, g.p1.y),
                    s.brushMode ? "  - brush: paint the area to edit, Tools > Effects > Apply spectral brush" : "");
    else if (s.scrubMode)
      std::snprintf(b, sizeof b, i18n::tr("%s  - scrub: drag to hear the audio at the speed you move; the play position stays where you let go"),
                    timeText(a, s, static_cast<int64_t>(fm0)).c_str());
    else
      std::snprintf(b, sizeof b, i18n::tr("%s  - click sets the play position, drag selects, Shift-click extends, Alt scrubs, wheel zooms, right-click for edits"),
                    timeText(a, s, static_cast<int64_t>(fm0)).c_str());
    if (hoverEdge == 0) a.setHint(b);
  }
  if (s.viewLen > 0)  // J-67: wheel scrolling glides; zoom, follow and drags are adopted as they are
    scrollui::stepUnits(s.viewAx, s.viewStart, wheelFrames, wheelNotch, 0.0, std::max(0.0, n - s.viewLen), s.viewLen / std::max(1.0f, g.W()), false);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("lanemenu");
  if (ImGui::BeginPopup("lanemenu")) {
    const bool has = s.frames() > 0, sel = !s.sel.empty();
    const Range tr = target(s);
    if (ImGui::MenuItem(i18n::tr("Cut"), "Ctrl+X", false, sel && !busy)) {
      clipboard() = extract(c.a(), s.sel);
      Range r = s.sel;
      runEdit(a, s, "Cut", [r](const Content& cc) { return deleteRange(cc, r); });
    }
    if (ImGui::MenuItem(i18n::tr("Copy"), "Ctrl+C", false, has)) clipboard() = extract(c.a(), tr);
    if (ImGui::MenuItem(i18n::tr("Paste"), "Ctrl+V", false, clipboard().frames() > 0 && !busy)) {
      Audio clip = clipboard();
      Range sl = s.sel;
      int64_t at = s.cursor;
      runEdit(a, s, "Paste", [clip, sl, at](const Content& cc) { return sl.empty() ? insertAudio(cc, at, clip) : replaceRange(cc, sl, clip); });
    }
    if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, sel && !busy)) {
      Range r = s.sel;
      runEdit(a, s, "Delete", [r](const Content& cc) { return deleteRange(cc, r); });
    }
    if (ImGui::MenuItem(i18n::tr("Trim to selection"), "Ctrl+T", false, sel && !busy)) {
      Range r = s.sel;
      runEdit(a, s, "Trim to selection", [r](const Content& cc) { return cropTo(cc, r); });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Fade in"), nullptr, false, has && !busy)) runEdit(a, s, "Fade in", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& cc) { return fadeIn(cc, tr, k); });
    if (ImGui::MenuItem(i18n::tr("Fade out"), nullptr, false, has && !busy)) runEdit(a, s, "Fade out", [tr, k = static_cast<Curve>(s.fadeCurve)](const Content& cc) { return fadeOut(cc, tr, k); });
    if (ImGui::MenuItem(i18n::tr("Normalize"), nullptr, false, has && !busy)) runEdit(a, s, "Normalize", [tr](const Content& cc) { return normalize(cc, tr, 0); });
    if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, false, has && !busy)) runEdit(a, s, "Reverse", [tr](const Content& cc) { return reverse(cc, tr); });
    ImGui::Separator();
    if (ImGui::MenuItem(sel ? i18n::tr("Add region") : i18n::tr("Add marker"), "M", false, has)) addMarkerAtCursor(a, s);
    if (ImGui::MenuItem(i18n::tr("Set loop from selection"), nullptr, false, sel)) applyNow(a, s, "Set loop", setLoop(c, s.sel));
    if (ImGui::MenuItem(i18n::tr("Export selection..."), nullptr, false, has)) exportSelection(a, s);
    if (s.spectral && !s.brush.empty()) {
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Apply spectral brush..."))) openTool(s, Tool::Spectral);
      if (ImGui::MenuItem(i18n::tr("Clear brush"))) s.brush.clear();
    }
    ImGui::EndPopup();
  }

  ImGui::EndDisabled();

  // ---- overview scroller
  dl->AddRectFilled(sc0, sc1, C(col::RulerBg));
  dl->AddLine(ImVec2(sc0.x, sc0.y + 0.5f), ImVec2(sc1.x, sc0.y + 0.5f), C(col::BtnLo), 1.f);
  if (live) {  // the whole take is in view
    const float in = std::max(2.0f, P(3));
    const float x1 = sc0.x + static_cast<float>(std::min(1.0, s.fit.shown / std::max(1.0, g.len))) * (sc1.x - sc0.x);
    dl->AddRectFilled(ImVec2(sc0.x, sc0.y + in), ImVec2(std::max(sc0.x + P(6), x1), sc1.y - in + 1), C(col::BtnHi), (sc1.y - sc0.y) * 0.5f - in);
  } else if (n > 0) {
    const float x0 = sc0.x + static_cast<float>(s.viewStart / n) * (sc1.x - sc0.x);
    const float x1 = sc0.x + static_cast<float>(std::min(1.0, (s.viewStart + s.viewLen) / n)) * (sc1.x - sc0.x);
    const float in = std::max(2.0f, P(3));
    dl->AddRectFilled(ImVec2(x0, sc0.y + in), ImVec2(std::max(x0 + P(6), x1), sc1.y - in + 1), C(col::BtnHi), (sc1.y - sc0.y) * 0.5f - in);
    if (pos >= 0) {
      const float px = sc0.x + static_cast<float>(pos / n) * (sc1.x - sc0.x);
      dl->AddLine(ImVec2(px, sc0.y), ImVec2(px, sc1.y), C(col::Playhead), 1.f);
    }
  }
  ImGui::SetCursorScreenPos(sc0);
  ImGui::InvisibleButton("overview", ImVec2(std::max(1.f, sc1.x - sc0.x), std::max(1.f, sc1.y - sc0.y)));
  if (ImGui::IsItemActive() && n > 0 && !live) {
    const double f = (mouse.x - sc0.x) / std::max(1.f, sc1.x - sc0.x) * n;
    s.viewStart = std::clamp(f - s.viewLen * 0.5, 0.0, std::max(0.0, n - s.viewLen));
  }
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Overview: drag to scroll"));

  // ---- auto-scroll strip (right of the ruler, lanes and overview)
  {
    sty::Btn b;
    b.lit = autoScroll(a);
    b.mark = false;
    b.flat = true;  // a quiet strip: the glyph says whether it follows
    dl->AddRectFilled(ImVec2(sideX, p0.y), ImVec2(p1.x, barY), C(col::RulerBg));
    b.hint = "Auto-scroll: follow the playback position";
    if (sty::iconButton("autoscroll", ImVec2(sideX, p0.y), ImVec2(p1.x, barY), ui::Icon::ScrollTo, P(sty::kBarIcon), b))
      setAutoScroll(a, !autoScroll(a));
  }
  dl->AddLine(ImVec2(sideX - 0.5f, p0.y), ImVec2(sideX - 0.5f, barY), C(col::BtnLo), 1.f);

  // ---- bottom bar: view | snap, time | zoom | readouts (one row of equal-height controls)
  dl->AddRectFilled(b0, b1, C(col::Panel));
  dl->AddLine(ImVec2(b0.x, b0.y + 0.5f), ImVec2(b1.x, b0.y + 0.5f), C(col::BtnLo), 1.f);
  const float ctl = P(sty::kBarCtl), cg = P(sty::kCellGap), gg = P(sty::kGroupGap), sg = P(sty::kSectionGap), ico = P(sty::kBarIcon);
  const float by = b0.y + std::round((barH - ctl) * 0.5f);
  float x = b0.x + std::round((barH - ctl) * 0.5f);
  // K-15: a narrow dock leaves out what its menus also offer: the time format (View), then
  // the snap field (Snap) and the brush buttons (View), instead of clipping the row.
  const float barW = b1.x - b0.x;
  const bool narrow = barW < P(760), tiny = barW < P(520);
  {
    static const char* kViews[] = {"WAVE", "SPECTRUM"};
    static const char* kViewHints[] = {"Waveform view (W)", "Spectrogram view (W)"};
    const char* labels[] = {i18n::tr(kViews[0]), i18n::tr(kViews[1])};
    const float w = P(tiny ? 120 : 160);
    const int v = sty::segmented("view", ImVec2(x, by), ImVec2(x + w, by + ctl), labels, 2, s.spectral ? 1 : 0, fm, fsVal, kViewHints);
    if (v >= 0) s.spectral = v == 1;
    sty::note("bar.view", "view", ImVec2(x, by), ImVec2(x + w, by + ctl));
    x += w + (tiny ? sg : gg);
  }
  if (!tiny) {
    sty::Btn b;
    b.lit = s.spectral && s.brushMode;
    b.enabled = s.spectral;
    b.hint = "Spectral brush: paint time / frequency areas, then apply a gain to them (B)";
    if (sty::iconButton("v_brush", ImVec2(x, by), ImVec2(x + ctl, by + ctl), ui::Icon::Paint, ico, b)) s.brushMode = !s.brushMode;
    sty::note("bar.brush", "brush", ImVec2(x, by), ImVec2(x + ctl, by + ctl));
    x += ctl + cg;
    b = sty::Btn{};
    b.enabled = s.spectral && !s.brush.empty();
    b.hint = "Apply a gain to the painted areas...";
    if (sty::iconButton("v_apply", ImVec2(x, by), ImVec2(x + ctl, by + ctl), ui::Icon::Check, ico, b)) openTool(s, Tool::Spectral);
    sty::note("bar.brush", "apply", ImVec2(x, by), ImVec2(x + ctl, by + ctl));
    x += ctl + sg;
  }
  if (!tiny) {
    static const char* kSnap[] = {"No snap", "Zero crossings", "Markers", "Beats"};
    const float w = P(144);
    if (sty::dropField("##snapf", ImVec2(x, by), ImVec2(x + w, by + ctl), i18n::tr(kSnap[s.snap]), fm, fsVal, true,
                       "Snap: off / zero crossings / markers / beats (Shift+Alt while dragging: no snap)", ui::Icon::Magnet))
      ImGui::OpenPopup("##snap");
    sty::note("bar.snap", "snap", ImVec2(x, by), ImVec2(x + w, by + ctl));
    ImGui::SetNextWindowPos(ImVec2(x, by + ctl + 2));
    if (ImGui::BeginPopup("##snap")) {
      for (int k = 0; k < 4; ++k)
        if (ImGui::MenuItem(i18n::tr(kSnap[k]), nullptr, s.snap == k)) s.snap = k;
      ImGui::EndPopup();
    }
    x += w + (narrow ? sg : gg);
  }
  if (!narrow) {
    const float tw = P(112);
    if (sty::dropField("##timefmtf", ImVec2(x, by), ImVec2(x + tw, by + ctl), s.barsTime ? i18n::tr("Bar:beat") : i18n::tr("Min:sec"), fm, fsVal, true,
                       "Time display: minutes and seconds, or bars and beats at the song tempo"))
      ImGui::OpenPopup("##timefmt");
    sty::note("bar.snap", "time", ImVec2(x, by), ImVec2(x + tw, by + ctl));
    ImGui::SetNextWindowPos(ImVec2(x, by + ctl + 2));
    if (ImGui::BeginPopup("##timefmt")) {
      if (ImGui::MenuItem(i18n::tr("Min:sec"), nullptr, !s.barsTime)) s.barsTime = false;
      if (ImGui::MenuItem(i18n::tr("Bar:beat"), nullptr, s.barsTime)) s.barsTime = true;
      ImGui::EndPopup();
    }
    x += tw + sg;
  }
  {
    auto zb = [&](const char* id, ui::Icon icon, const char* hint, bool en) {
      sty::Btn b;
      b.hint = hint;
      b.enabled = en;
      const bool c2 = sty::iconButton(id, ImVec2(x, by), ImVec2(x + ctl, by + ctl), icon, ico, b);
      sty::note("bar.zoom", id, ImVec2(x, by), ImVec2(x + ctl, by + ctl));
      x += ctl + cg;
      return c2;
    };
    const bool has = s.frames() > 0 && !live;
    if (zb("z_out", ui::Icon::Minus, "Zoom out (-)", has)) zoomBy(s, 2.0, s.viewStart + s.viewLen * 0.5);
    if (zb("z_in", ui::Icon::Plus, "Zoom in (+)", has)) zoomBy(s, 0.5, s.viewStart + s.viewLen * 0.5);
    if (zb("z_sel", ui::Icon::Zoom, "Zoom to the selection", has && !s.sel.empty())) zoomTo(s, s.sel);
    if (zb("z_all", ui::Icon::StretchMode, "Show everything (0)", has)) zoomAll(s);
    x += sg - cg;
  }
  // Readouts: one recessed field to the right edge; caption / value pairs on even stops.
  updateLevels(s);
  {
    const ImVec2 f0(x, by), f1(b1.x - std::round((barH - ctl) * 0.5f), by + ctl);
    if (f1.x - f0.x > P(60)) {
      sty::note("bar.readout", "readout", f0, f1);
      if (live) {  // K-145: the take so far, its length and peak
        float lo = 0, hi = 0, l1 = 0, h1 = 0;
        const int top = s.live.levels() - 1;
        if (top >= 0 && s.live.buckets(top) > 0) {
          s.live.bucket(0, top, 0, lo, hi);
          s.live.bucket(1, top, 0, l1, h1);
        }
        const double pk = std::max(std::max(std::fabs(lo), std::fabs(hi)), std::max(std::fabs(l1), std::fabs(h1)));
        char b[96];
        const std::string len = timeText(a, s, static_cast<int64_t>(s.live.frames()));
        if (pk > 0)
          std::snprintf(b, sizeof b, "%s   %s %.1f dB", len.c_str(), i18n::tr("PEAK"), dsp::gainToDb(pk));
        else
          std::snprintf(b, sizeof b, "%s   %s -inf", len.c_str(), i18n::tr("PEAK"));
        sty::field(dl, f0, f1, i18n::tr("TAKE"), b, C(col::Rec), fm, fsVal, fsCap, 2);
      } else if (!s.status.empty() && ImGui::GetTime() < s.statusUntil) {
        sty::field(dl, f0, f1, nullptr, s.status.c_str(), C(col::LcdText), fm, fsVal, fsCap, 2);
      } else if (s.frames() == 0) {
        sty::field(dl, f0, f1, nullptr, "", C(col::LcdText), fm, fsVal, fsCap, 2);
      } else {
        skin::insetField(dl, f0, f1, C(col::Lcd), sty::kRadius);
        auto db = [](double v) {
          char t[16];
          if (v < -150)
            std::snprintf(t, sizeof t, "-inf");
          else
            std::snprintf(t, sizeof t, "%.1f", v);
          return std::string(t);
        };
        const std::string pitch =
            s.pitch.hz > 0 && s.pitch.confidence > 0.5
                ? noteName(s.pitch.note) + (s.pitch.cents >= 0 ? " +" : " ") + std::to_string(static_cast<int>(std::lround(s.pitch.cents))) + "c"
                : "-";
        const std::pair<const char*, std::string> kv[] = {{s.sel.empty() ? "FILE" : "SEL", ""},
                                                          {"PEAK", db(s.levels.peakDb)},
                                                          {"RMS", db(s.levels.rmsDb)},
                                                          {"LUFS", db(s.levels.lufs)},
                                                          {"TP", db(s.levels.truePeakDb)},
                                                          {"PITCH", pitch}};
        // Widths of the pairs; the ones that do not fit drop from the right.
        const float in = std::round(std::max(6.0f, ctl * 0.3f)), sp = P(6);
        float need[6] = {}, total = 0;
        int count = 0;
        for (int k = 0; k < 6; ++k) {
          need[k] = sty::textW(fm, fsCap, i18n::tr(kv[k].first)) + (kv[k].second.empty() ? 0 : sp + sty::textW(fm, fsVal, kv[k].second.c_str()));
          if (total + need[k] + (count ? P(12) : 0) > f1.x - f0.x - 2 * in) break;
          total += need[k] + (count ? P(12) : 0);
          ++count;
        }
        const float slack = count > 1 ? (f1.x - f0.x - 2 * in - total) / static_cast<float>(count - 1) : 0;
        const float cy = (f0.y + f1.y) * 0.5f;
        float tx = f0.x + in;
        dl->PushClipRect(f0, f1, true);
        for (int k = 0; k < count; ++k) {
          sty::textAt(dl, fm, fsCap, tx, cy, k == 0 ? C(col::Text, 0.8f) : C(col::TextDim), i18n::tr(kv[k].first), 0);
          if (!kv[k].second.empty())
            sty::textAt(dl, fm, fsVal, tx + sty::textW(fm, fsCap, i18n::tr(kv[k].first)) + sp, cy, C(col::LcdText), kv[k].second.c_str(), 0);
          tx += need[k] + P(12) + slack;
        }
        dl->PopClipRect();
        if (ImGui::IsMouseHoveringRect(f0, f1))
          a.setHint(i18n::tr("Levels of the selection (or the whole file): peak, RMS, integrated loudness, true peak, detected pitch"));
      }
    }
  }
  // Frame around the whole editor.
  dl->AddRect(ImVec2(p0.x - 0.5f, p0.y - 0.5f), ImVec2(p1.x + 0.5f, p1.y + 0.5f), C(col::BtnLo), sty::kRadius, 0, 1.f);
}

}  // namespace gl::packs::edison
