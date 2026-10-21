// Gridline — "utility" pack: visual plugin editors (Scope, Spectrogram,
// Level Meter, Clock, Notes) and the analysis helpers they share.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include "../../core/I18n.h"
#include "../../core/Params.h"
#include "../../engine/Dsp.h"
#include "utility_candy.h"
#include "utility_widgets.h"

namespace gl::packs::utility {

using theme::pal;

// ------------------------------------------------------------------ helpers

UtilityFx* liveFx(App& a, const PluginTarget& t) {
  if (t.isChannel()) return nullptr;
  auto rt = a.builder.effectRuntime(t.owner, t.slot);
  return rt && rt->fx ? dynamic_cast<UtilityFx*>(rt->fx.get()) : nullptr;  // owned by the builder's registry
}

void SpectrumAnalyzer::analyse(const StereoRing* ring, int fftSize, bool window, int channel, uint64_t end, std::vector<float>& outDb) {
  if (fftSize != n_ || window != hann_ || win_.empty()) {
    n_ = fftSize;
    hann_ = window;
    fft_.init(n_);
    re_.assign(static_cast<size_t>(n_), 0.f);
    im_.assign(static_cast<size_t>(n_), 0.f);
    l_.assign(static_cast<size_t>(n_), 0.f);
    r_.assign(static_cast<size_t>(n_), 0.f);
    win_.resize(static_cast<size_t>(n_));
    double sum = 0;
    for (int i = 0; i < n_; ++i) {
      float w = window ? static_cast<float>(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / n_)) : 1.0f;
      win_[static_cast<size_t>(i)] = w;
      sum += w;
    }
    norm_ = static_cast<float>(2.0 / sum);
  }
  outDb.assign(static_cast<size_t>(n_ / 2 + 1), -200.f);
  if (!ring) return;
  bool ok = end ? ring->readAt(end, l_.data(), r_.data(), static_cast<size_t>(n_)) : (ring->readLatest(l_.data(), r_.data(), static_cast<size_t>(n_)), true);
  if (!ok) return;
  for (int i = 0; i < n_; ++i) {
    size_t k = static_cast<size_t>(i);
    float x = channel == 1 ? l_[k] : channel == 2 ? r_[k] : 0.5f * (l_[k] + r_[k]);
    re_[k] = x * win_[k];
    im_[k] = 0;
  }
  fft_.transform(re_.data(), im_.data(), false);
  for (int k = 0; k <= n_ / 2; ++k) {
    size_t i = static_cast<size_t>(k);
    float mag = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]) * norm_;
    outDb[i] = 20.0f * std::log10(std::max(mag, 1e-10f));
  }
}

float bandDb(const std::vector<float>& bins, double f0, double f1, double sr) {
  if (bins.size() < 2) return -200.f;
  double binHz = sr / (2.0 * static_cast<double>(bins.size() - 1));
  double b0 = f0 / binHz, b1 = f1 / binHz;
  size_t i0 = static_cast<size_t>(std::ceil(b0)), i1 = static_cast<size_t>(std::floor(b1));
  if (i1 >= bins.size()) i1 = bins.size() - 1;
  if (i0 <= i1) {
    float m = -200.f;
    for (size_t i = i0; i <= i1; ++i) m = std::max(m, bins[i]);
    return m;
  }
  double c = std::clamp((b0 + b1) * 0.5, 0.0, static_cast<double>(bins.size() - 1));
  size_t i = static_cast<size_t>(c);
  size_t j = std::min(i + 1, bins.size() - 1);
  double t = c - static_cast<double>(i);
  return static_cast<float>(bins[i] * (1 - t) + bins[j] * t);
}

double axisToHz(double u, double scale, double nyquist) {
  u = std::clamp(u, 0.0, 1.0);
  double lin = 20.0 + u * (nyquist - 20.0), lg = 20.0 * std::pow(nyquist / 20.0, u);
  return std::exp((1 - scale) * std::log(lin) + scale * std::log(lg));
}

double hzToAxis(double hz, double scale, double nyquist) {
  double lo = 0, hi = 1;
  for (int i = 0; i < 40; ++i) {
    double m = (lo + hi) * 0.5;
    (axisToHz(m, scale, nyquist) < hz ? lo : hi) = m;
  }
  return (lo + hi) * 0.5;
}

ImU32 heat(float v) {
  const auto& P = pal();
  v = std::clamp(v, 0.f, 1.f);
  if (v > 0.88f) return theme::mix(P.paper, P.accent, (v - 0.88f) / 0.12f);
  return theme::mix(P.surface950, P.paper, std::pow(v / 0.88f, 2.2f));
}

void Sonogram::update(const StereoRing* ring, double sr, int fftSize, int hop, double rangeDb, double gainDb, double tiltDbOct, double scale,
                      int columns, int rowCount) {
  if (columns != cols || rowCount != rows) {
    cols = std::max(1, columns);
    rows = std::max(1, rowCount);
    img.assign(static_cast<size_t>(cols * rows), 0.f);
    head = 0;
    lastPos = 0;
  }
  if (!ring) return;
  uint64_t w = ring->written();
  if (lastPos == 0 || w < lastPos || w - lastPos > static_cast<uint64_t>(hop) * 64) lastPos = w > static_cast<uint64_t>(hop) ? w - static_cast<uint64_t>(hop) : 0;
  int added = 0;
  while (lastPos + static_cast<uint64_t>(hop) <= w && added < 32) {
    lastPos += static_cast<uint64_t>(hop);
    ++added;
    if (lastPos < static_cast<uint64_t>(fftSize)) continue;
    an.analyse(ring, fftSize, true, 0, lastPos, bins);
    float* col = &img[static_cast<size_t>(head * rows)];
    double nyq = sr * 0.5;
    for (int r = 0; r < rows; ++r) {
      double u0 = static_cast<double>(r) / rows, u1 = static_cast<double>(r + 1) / rows;
      double f0 = axisToHz(u0, scale, std::min(nyq, 20000.0)), f1 = axisToHz(u1, scale, std::min(nyq, 20000.0));
      double db = bandDb(bins, f0, f1, sr) + gainDb + tiltDbOct * std::log2(std::max(20.0, (f0 + f1) * 0.5) / 1000.0);
      col[r] = static_cast<float>(std::clamp((db + rangeDb) / rangeDb, 0.0, 1.0));
    }
    head = (head + 1) % cols;
  }
}

void Sonogram::draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1) const {
  if (cols <= 0 || rows <= 0) return;
  float cw = (p1.x - p0.x) / cols, rh = (p1.y - p0.y) / rows;
  for (int c = 0; c < cols; ++c) {
    int src = (head + c) % cols;  // oldest on the left
    const float* col = &img[static_cast<size_t>(src * rows)];
    float x0 = p0.x + c * cw, x1 = x0 + cw + 0.5f;
    int r = 0;
    while (r < rows) {
      int q = static_cast<int>(col[r] * 40.0f);
      int e = r + 1;
      while (e < rows && static_cast<int>(col[e] * 40.0f) == q) ++e;
      if (q > 0) dl->AddRectFilled(ImVec2(x0, p1.y - e * rh), ImVec2(x1, p1.y - r * rh + 0.5f), heat(static_cast<float>(q) / 40.0f));
      r = e;
    }
  }
}

void panelBg(ImDrawList* dl, ImVec2 p0, ImVec2 p1) { dl->AddRectFilled(p0, p1, pal().surface950, 6); }

void textAt(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text) {
  ImGui::PushFont(font, size);
  dl->AddText(pos, col, text);
  ImGui::PopFont();
}

void textCentred(ImDrawList* dl, ImFont* font, float size, ImVec2 c, ImU32 col, const char* text) {
  ImGui::PushFont(font, size);
  ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
  ImGui::PopFont();
}

std::string fmtDb(float db, const char* suffix) {
  char buf[32];
  if (db <= -100.f)
    std::snprintf(buf, sizeof buf, "-inf%s", suffix);
  else
    std::snprintf(buf, sizeof buf, "%+.1f%s", db, suffix);
  return buf;
}

namespace {

float toDb(float lin) { return lin > 1e-7f ? 20.0f * std::log10(lin) : -140.f; }

// Meter needle state: fall rate, peak hold, clip latch.
struct Ballistic {
  float disp = -140, hold = -140, holdLeft = 0;
  bool clip = false;
  void feed(float db, float dt, float holdSec, float fall) {
    disp = std::max(db, disp - fall * dt);
    if (db >= hold) {
      hold = db;
      holdLeft = holdSec;
    } else if ((holdLeft -= dt) < 0) {
      hold = std::max(disp, hold - fall * dt);
    }
    if (db > 0.0f) clip = true;
  }
};

// Vertical stereo-capable bar with a dB scale on the left.
void meterBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float db, float rmsDb, float holdDb, bool clip, float lo, float hi) {
  const auto& P = pal();
  auto y = [&](float d) { return p1.y - (std::clamp(d, lo, hi) - lo) / (hi - lo) * (p1.y - p0.y); };
  dl->AddRectFilled(p0, p1, P.surface800, 3);
  float yv = y(db);
  float y0 = y(0.f), y6 = y(-6.f);
  // Segments coloured by zone: under -6, -6..0, over 0.
  if (db > lo) {
    dl->AddRectFilled(ImVec2(p0.x, std::max(yv, y6)), p1, P.meterLow, 3);
    if (yv < y6) dl->AddRectFilled(ImVec2(p0.x, std::max(yv, y0)), ImVec2(p1.x, y6), P.meterMid);
    if (yv < y0) dl->AddRectFilled(ImVec2(p0.x, yv), ImVec2(p1.x, y0), P.danger);
  }
  if (rmsDb > lo) {
    float yr = y(rmsDb);
    dl->AddRectFilled(ImVec2(p0.x + (p1.x - p0.x) * 0.3f, yr), ImVec2(p1.x - (p1.x - p0.x) * 0.3f, p1.y), theme::withAlpha(P.paper, 0.55f), 2);
  }
  if (holdDb > lo) dl->AddLine(ImVec2(p0.x, y(holdDb)), ImVec2(p1.x, y(holdDb)), holdDb > 0 ? P.danger : P.paper, 2.0f);
  (void)clip;
}

void dbScale(App& a, ImDrawList* dl, float x, float yTop, float yBot, float lo, float hi, float step, bool right) {
  const auto& P = pal();
  for (float d = hi; d >= lo - 0.01f; d -= step) {
    float yy = yBot - (d - lo) / (hi - lo) * (yBot - yTop);
    char buf[12];
    std::snprintf(buf, sizeof buf, d > 0 ? "+%.0f" : "%.0f", d);
    ImGui::PushFont(a.fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(right ? x : x - ts.x, yy - ts.y * 0.5f), std::fabs(d) < 0.01f ? P.text : P.textFaint, buf);
    ImGui::PopFont();
  }
}

// ==================================================================== scope

struct ScopeUi {
  std::vector<float> wl, wr, vl, vr;
  std::vector<float> spec, peak, bins;
  SpectrumAnalyzer an;
  Sonogram sono;
  Ballistic bl[2];
  float tpMax = 0;
  std::deque<float> history;  // momentary loudness, 10 per second
  double histClock = 0;
};
std::map<uint64_t, ScopeUi> scopeUis;

void drawWave(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, double windowSec, double gainDb, ScopeUi& ui,
              bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  size_t n = static_cast<size_t>(std::clamp(windowSec * sr, 64.0, 16384.0));
  if (!frozen || ui.wl.size() != n * 2) {
    ui.wl.assign(n * 2, 0.f);
    ui.wr.assign(n * 2, 0.f);
    if (ring) ring->readLatest(ui.wl.data(), ui.wr.data(), n * 2);
  }
  // Trigger: last rising zero crossing of the mid signal in the older half.
  size_t start = n;
  for (size_t i = n - 1; i > 0; --i) {
    float m0 = ui.wl[i - 1] + ui.wr[i - 1], m1 = ui.wl[i] + ui.wr[i];
    if (m0 < 0 && m1 >= 0) {
      start = i;
      break;
    }
  }
  float g = static_cast<float>(dsp::dbToGain(gainDb));
  float w = p1.x - p0.x, lane = (p1.y - p0.y) * 0.5f;
  const float* ch[2] = {ui.wl.data() + start, ui.wr.data() + start};
  const char* names[2] = {"L", "R"};
  for (int c = 0; c < 2; ++c) {
    float cy = p0.y + lane * (c + 0.5f);
    dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), P.gridBeat);
    int cols = static_cast<int>(w);
    for (int x = 0; x < cols; ++x) {
      size_t a0 = static_cast<size_t>(static_cast<double>(x) / cols * n), a1 = std::max(a0 + 1, static_cast<size_t>(static_cast<double>(x + 1) / cols * n));
      float mn = 1e9f, mx = -1e9f;
      for (size_t i = a0; i < a1 && i < n; ++i) mn = std::min(mn, ch[c][i]), mx = std::max(mx, ch[c][i]);
      if (mn > mx) continue;
      float ya = cy - std::clamp(mx * g, -1.f, 1.f) * lane * 0.9f, yb = cy - std::clamp(mn * g, -1.f, 1.f) * lane * 0.9f;
      dl->AddLine(ImVec2(p0.x + x + 0.5f, ya), ImVec2(p0.x + x + 0.5f, std::max(yb, ya + 1.0f)), c == 0 ? P.paper : theme::withAlpha(P.paper, 0.75f));
    }
    textAt(dl, a.fontMono, 10.5f, ImVec2(p0.x + 6, p0.y + lane * c + 4), P.textFaint, names[c]);
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.0f ms", windowSec * 1000);
  textAt(dl, a.fontMono, 10.0f, ImVec2(p1.x - 46, p0.y + 4), P.textFaint, buf);
}

void drawSpectrum(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, int fftSize, double rangeDb,
                  double tilt, double gainDb, ScopeUi& ui, bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  float w = p1.x - p0.x, h = p1.y - p0.y;
  const double top = 6.0;
  auto yOf = [&](double db) { return p0.y + static_cast<float>((top - db) / (rangeDb + top)) * h; };
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    float x = pui::freqToX(f, p0.x, w);
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), f == 100.0 || f == 1000.0 || f == 10000.0 ? P.gridBar : P.gridStep);
    char buf[48];
    std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
    textAt(dl, a.fontMono, 10.0f, ImVec2(x + 3, p1.y - 14), P.textFaint, buf);
  }
  for (double d = 0; d >= -rangeDb; d -= 12) {
    float y = yOf(d);
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), d == 0 ? P.lineStrong : P.gridStep);
    if (y > p1.y - 30) continue;
    char buf[12];
    std::snprintf(buf, sizeof buf, "%.0f", d);
    textAt(dl, a.fontMono, 10.0f, ImVec2(p0.x + 4, y + 1), P.textFaint, buf);
  }
  const int N = std::max(16, static_cast<int>(w / 2));
  if (static_cast<int>(ui.spec.size()) != N) {
    ui.spec.assign(static_cast<size_t>(N), -200.f);
    ui.peak.assign(static_cast<size_t>(N), -200.f);
  }
  float dt = ImGui::GetIO().DeltaTime;
  if (!frozen) {
    ui.an.analyse(ring, fftSize, true, 0, 0, ui.bins);
    for (int k = 0; k < N; ++k) {
      double f0 = 20.0 * std::pow(1000.0, static_cast<double>(k) / N), f1 = 20.0 * std::pow(1000.0, static_cast<double>(k + 1) / N);
      float db = bandDb(ui.bins, f0, f1, sr) + static_cast<float>(gainDb + tilt * std::log2(std::sqrt(f0 * f1) / 1000.0));
      float& s = ui.spec[static_cast<size_t>(k)];
      s = db > s ? s + (db - s) * 0.7f : std::max(db, s - 40.0f * dt);
      float& pk = ui.peak[static_cast<size_t>(k)];
      pk = std::max(s, pk - 8.0f * dt);
    }
  }
  std::vector<ImVec2> line(static_cast<size_t>(N)), peaks(static_cast<size_t>(N));
  for (int k = 0; k < N; ++k) {
    float x = p0.x + w * (k + 0.5f) / N;
    line[static_cast<size_t>(k)] = ImVec2(x, std::clamp(yOf(ui.spec[static_cast<size_t>(k)]), p0.y, p1.y));
    peaks[static_cast<size_t>(k)] = ImVec2(x, std::clamp(yOf(ui.peak[static_cast<size_t>(k)]), p0.y, p1.y));
  }
  for (int k = 0; k + 1 < N; ++k) {
    ImVec2 q[4] = {line[static_cast<size_t>(k)], line[static_cast<size_t>(k + 1)], ImVec2(line[static_cast<size_t>(k + 1)].x, p1.y),
                   ImVec2(line[static_cast<size_t>(k)].x, p1.y)};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.10f));
  }
  dl->AddPolyline(peaks.data(), N, theme::withAlpha(P.accent, 0.6f), 0, 1.0f);
  dl->AddPolyline(line.data(), N, P.paper, 0, 1.5f);
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsMouseHoveringRect(p0, p1)) {
    double hz = pui::xToFreq(io.MousePos.x, p0.x, w);
    float db = static_cast<float>(top - (io.MousePos.y - p0.y) / h * (rangeDb + top));
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = static_cast<int>(std::lround(69 + 12 * std::log2(hz / 440.0)));
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.0f Hz (%s%d) · %.1f dB", hz, names[((note % 12) + 12) % 12], note / 12 - 1, db);
    a.setHint(buf);
  }
}

void drawVector(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double gainDb, int style, float corr, ScopeUi& ui,
                bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  const size_t n = 2048;
  if (!frozen || ui.vl.size() != n) {
    ui.vl.assign(n, 0.f);
    ui.vr.assign(n, 0.f);
    if (ring) ring->readLatest(ui.vl.data(), ui.vr.data(), n);
  }
  float side = std::min(p1.x - p0.x, p1.y - p0.y - 26) - 12;
  ImVec2 c((p0.x + p1.x) * 0.5f, p0.y + 6 + side * 0.5f);
  float rad = side * 0.5f;
  dl->AddCircle(c, rad, P.gridBeat, 64);
  dl->AddCircle(c, rad * 0.5f, P.gridStep, 48);
  float d = rad * 0.7071f;
  dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), P.gridStep);
  dl->AddLine(ImVec2(c.x + d, c.y - d), ImVec2(c.x - d, c.y + d), P.gridStep);
  dl->AddLine(ImVec2(c.x, c.y - rad), ImVec2(c.x, c.y + rad), P.gridStep);
  dl->AddLine(ImVec2(c.x - rad, c.y), ImVec2(c.x + rad, c.y), P.gridStep);
  textAt(dl, a.fontMono, 10.0f, ImVec2(c.x - d - 12, c.y - d - 12), P.textFaint, "L");
  textAt(dl, a.fontMono, 10.0f, ImVec2(c.x + d + 4, c.y - d - 12), P.textFaint, "R");
  textAt(dl, a.fontMono, 10.0f, ImVec2(c.x + 3, c.y - rad + 1), P.textFaint, "M");
  float g = static_cast<float>(dsp::dbToGain(gainDb));
  dl->PushClipRect(ImVec2(c.x - rad, c.y - rad), ImVec2(c.x + rad, c.y + rad), true);
  std::vector<ImVec2> pts(n);
  for (size_t i = 0; i < n; ++i) {
    float x = (ui.vr[i] - ui.vl[i]) * 0.5f * g, y = (ui.vl[i] + ui.vr[i]) * 0.5f * g;
    pts[i] = ImVec2(c.x + x * rad, c.y - y * rad);
  }
  if (style == 0)
    dl->AddPolyline(pts.data(), static_cast<int>(n), theme::withAlpha(P.paper, 0.45f), 0, 1.0f);
  else
    for (const auto& q : pts) dl->AddRectFilled(ImVec2(q.x - 0.8f, q.y - 0.8f), ImVec2(q.x + 0.8f, q.y + 0.8f), theme::withAlpha(P.paper, 0.6f));
  dl->PopClipRect();
  // Correlation strip.
  float by = p1.y - 16, bx0 = p0.x + 26, bx1 = p1.x - 26;
  dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 6), P.surface800, 3);
  float cx = (bx0 + bx1) * 0.5f;
  dl->AddLine(ImVec2(cx, by - 2), ImVec2(cx, by + 8), P.gridBar);
  float mx = cx + std::clamp(corr, -1.f, 1.f) * (bx1 - bx0) * 0.5f;
  dl->AddRectFilled(ImVec2(std::min(cx, mx), by), ImVec2(std::max(cx, mx), by + 6), corr < 0 ? P.danger : P.text, 3);
  textAt(dl, a.fontMono, 10.0f, ImVec2(p0.x + 6, by - 4), P.textFaint, "-1");
  textAt(dl, a.fontMono, 10.0f, ImVec2(p1.x - 20, by - 4), P.textFaint, "+1");
}

void drawLoudness(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, LevelState* lv, int meterMode, float holdSec, float fall, ScopeUi& ui,
                  bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  float dt = ImGui::GetIO().DeltaTime;
  float pk[2] = {0, 0}, rm[2] = {0, 0}, vu[2] = {0, 0};
  if (lv && !frozen) {
    pk[0] = lv->peakL.exchange(0);
    pk[1] = lv->peakR.exchange(0);
    rm[0] = lv->rmsL.load();
    rm[1] = lv->rmsR.load();
    vu[0] = lv->vuL.load();
    vu[1] = lv->vuR.load();
    for (int c = 0; c < 2; ++c) {
      float v = meterMode == 1 ? rm[c] : meterMode == 2 ? vu[c] : pk[c];
      ui.bl[c].feed(toDb(v), dt, holdSec, fall);
    }
    ui.histClock += dt;
    while (ui.histClock >= 0.1) {
      ui.histClock -= 0.1;
      ui.history.push_back(lv->momentary.load());
      while (ui.history.size() > 300) ui.history.pop_front();
    }
  }
  // Bars.
  const float lo = -60, hi = 6;
  float bt = p0.y + 22, bb = p1.y - 12;
  float bx = p0.x + 34, bw = (p1.y - p0.y) > 220 ? 22.f : 14.f;
  textAt(dl, a.fontMono, 10.0f, ImVec2(bx + bw * 2 + 10, bb - 12), P.textFaint, meterMode == 1 ? "RMS" : meterMode == 2 ? "VU" : "PEAK");
  dbScale(a, dl, p0.x + 28, bt, bb, lo, hi, 6, false);
  for (int c = 0; c < 2; ++c) {
    ImVec2 q0(bx + c * (bw + 6), bt), q1(bx + c * (bw + 6) + bw, bb);
    meterBar(dl, q0, q1, ui.bl[c].disp, meterMode == 3 ? toDb(lv ? (c ? lv->rmsR.load() : lv->rmsL.load()) : 0) : -200, ui.bl[c].hold, ui.bl[c].clip, lo, hi);
    dl->AddCircleFilled(ImVec2((q0.x + q1.x) * 0.5f, p0.y + 12), 4, ui.bl[c].clip ? P.danger : P.surface700);
  }
  // Readouts.
  float rx = bx + bw * 2 + 50, ry = p0.y + 8;
  auto readout = [&](const char* label, const std::string& value, float size, ImU32 col) {
    textAt(dl, a.fontMono, 10.0f, ImVec2(rx, ry), P.textFaint, label);
    textAt(dl, a.fontMonoBold, size, ImVec2(rx, ry + 12), col, value.c_str());
    ry += size + 18;
  };
  auto lu = [](float v) { return v <= kNoLoudness + 1 ? std::string("--.-") : fmtDb(v); };
  float M = lv ? lv->momentary.load() : kNoLoudness, S = lv ? lv->shortTerm.load() : kNoLoudness, I = lv ? lv->integrated.load() : kNoLoudness;
  float colW = std::max(110.f, (p1.x - rx) * 0.5f);
  float ry0 = ry;
  readout("MOMENTARY LUFS", lu(M), 24, P.paper);
  readout("SHORT-TERM LUFS", lu(S), 20, P.text);
  readout("INTEGRATED LUFS", lu(I), 20, P.text);
  float rowEnd = ry;
  rx += colW;
  ry = ry0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f LU", lv ? lv->range.load() : 0.f);
  readout("RANGE", buf, 16, P.text);
  float tp = lv ? lv->maxTruePeak.load() : 0.f;
  readout("TRUE PEAK MAX", fmtDb(toDb(tp), " dBTP"), 16, tp > 1.0f ? P.danger : P.text);
  readout("CORRELATION", [&] {
    char b[16];
    std::snprintf(b, sizeof b, "%+.2f", lv ? lv->correlation.load() : 0.f);
    return std::string(b);
  }(), 16, P.text);
  // Momentary history (last 30 s).
  float hx0 = bx + bw * 2 + 50, hy0 = std::max(rowEnd, ry) + 4, hx1 = p1.x - 8, hy1 = p1.y - 8;
  if (hy1 - hy0 > 24) {
    dl->AddRectFilled(ImVec2(hx0, hy0), ImVec2(hx1, hy1), P.surface900, 4);
    auto hy = [&](float l) { return hy1 - (std::clamp(l, -48.f, 0.f) + 48.f) / 48.f * (hy1 - hy0); };
    for (float ref : {-23.f, -14.f}) {
      dl->AddLine(ImVec2(hx0, hy(ref)), ImVec2(hx1, hy(ref)), P.gridStep);
      std::snprintf(buf, sizeof buf, "%.0f", ref);
      textAt(dl, a.fontMono, 9.5f, ImVec2(hx1 - 22, hy(ref) - 12), P.textFaint, buf);
    }
    std::vector<ImVec2> pts;
    size_t k = 0;
    for (float l : ui.history) {
      float x = hx1 - (static_cast<float>(ui.history.size() - k) / 300.f) * (hx1 - hx0);
      ++k;
      if (l <= kNoLoudness + 1) continue;
      pts.push_back(ImVec2(x, hy(l)));
    }
    if (pts.size() > 1) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), P.paper, 0, 1.5f);
  }
}

}  // namespace

// --------------------------------------------------------- reference skins
namespace {

// Visual-tool family (input / sheet panels, per-sheet wheel colours).
const skin::Skin& scopeSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x363C40, 0x3B4144, 0x50A0FF, 0x42474A, 0xC1C7CB);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0x9FA8AD);
    s.handle = skin::hex(0x6196D6);
    s.select = skin::hex(0xFFBC40);
    s.valueBg = skin::hex(0x262B2F);
    s.valueText = skin::hex(0xC1C7CB);
    s.graphBg = skin::hex(0x14181A);
    return s;
  }();
  return k;
}

// Small labelled text in a form row (vertically centred on cy).
void rowLabel(ImDrawList* dl, const skin::Form& F, float x, float cy, const skin::Skin& k, const char* s) {
  float fs = F.font(8.0f);
  skin::text(dl, ImVec2(F.p(x, 0).x, F.p(0, cy).y - fs * 0.62f), k.text, s, fs);
}

}  // namespace

// Reference panel 592 × 315: INPUT panel (gain slider, hold pad) on the left,
// the visual where the reference keeps its appearance / colour panels, the
// per-view settings sheet underneath and the view tabs at the bottom.
void editorScopeRef(App& a, const PluginTarget& t, float width);

void editorScope(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  if (val(st, ScEngine) >= 0.5f) {
    editorScopeRef(a, t, width);
    return;
  }
  const skin::Skin& K = scopeSkin();
  constexpr float kW = 592, kH = 315;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  UtilityFx* fx = liveFx(a, t);
  const StereoRing* ring = fx ? fx->ring() : nullptr;
  LevelState* lv = fx ? fx->levels() : nullptr;
  ScopeUi& ui = scopeUis[t.key()];
  const double sr = a.engine.sampleRate();
  int mode = std::clamp(static_cast<int>(val(st, ScMode) + 0.5f), 0, SvCount - 1);
  bool frozen = val(st, ScFreeze) >= 0.5f;
  const float fs = F.font(8.0f);

  // ---- INPUT panel ----
  soberPanel(dl, F, 7, 7, 116, 204, K, "INPUT");
  skin::vslider(a, t, ScGain, F.p(14, 24), F.sz(34, 112), K, K.fill, true);
  rowLabel(dl, F, 15, 145, K, "GAIN");
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f dB", val(st, ScGain));
    skin::lcd(dl, F.p(56, 24), F.p(116, 40), K, buf, fs);
  }
  if (skin::button("##reset", F.p(56, 92), F.sz(60, 36), K, "RESET", false, fs) && lv) {
    lv->resetRequest.store(true);
    ui.bl[0] = ui.bl[1] = Ballistic{};
    ui.history.clear();
  }
  if (ImGui::IsItemHovered()) a.setHint("Restart the integrated loudness, range and peak memory");
  {
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, ScEngine, F.p(56, 139), F.sz(60, 14), K, eng, 2, true);
  }
  skin::toggle(a, t, ScFreeze, F.p(13, 156), F.sz(104, 48), K, frozen ? "FROZEN" : "HOLD", F.font(10));

  // ---- visual ----
  ImVec2 p = F.p(131, 7), e = F.p(585, 211);
  ImVec2 sz(e.x - p.x, e.y - p.y);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##display", sz);
  double gain = val(st, ScGain), win = val(st, ScWindow, 0.04), range = val(st, ScRange, 96), tilt = val(st, ScTilt, 3);
  int fftSize = 1024 << std::clamp(static_cast<int>(val(st, ScRes, 2) + 0.5f), 0, 3);
  int meterMode = static_cast<int>(val(st, ScMeterMode, 3) + 0.5f);
  float hold = val(st, ScHold, 1.5f), fall = val(st, ScDecay, 18);
  int vecMode = static_cast<int>(val(st, ScVecMode) + 0.5f);
  float corr = lv ? lv->correlation.load() : 0.f;
  auto sonogram = [&](ImVec2 q0, ImVec2 q1) {
    panelBg(dl, q0, q1);
    if (!frozen)
      ui.sono.update(ring, sr, std::min(fftSize, 4096), 512, range, gain, tilt, 1.0, std::max(32, static_cast<int>((q1.x - q0.x) / 3)),
                     std::max(24, static_cast<int>((q1.y - q0.y) / 3)));
    ui.sono.draw(dl, q0, q1);
    for (double f : {100.0, 1000.0, 10000.0}) {
      float y = q1.y - static_cast<float>(hzToAxis(f, 1.0, std::min(sr * 0.5, 20000.0))) * (q1.y - q0.y);
      char buf[12];
      std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
      dl->AddRectFilled(ImVec2(q0.x, y - 8), ImVec2(q0.x + 36, y + 7), theme::withAlpha(P.surface950, 0.75f), 3);
      dl->AddLine(ImVec2(q0.x, y), ImVec2(q0.x + 6, y), P.text);
      textAt(dl, a.fontMono, 10.0f, ImVec2(q0.x + 9, y - 7), P.text, buf);
    }
  };
  switch (mode) {
    case SvOsc: drawWave(a, dl, p, e, ring, sr, win, gain, ui, frozen); break;
    case SvSpectrum: drawSpectrum(a, dl, p, e, ring, sr, fftSize, range, tilt, gain, ui, frozen); break;
    case SvSonogram: sonogram(p, e); break;
    case SvVector: drawVector(a, dl, p, e, ring, gain, vecMode, corr, ui, frozen); break;
    case SvMeters: drawLoudness(a, dl, p, e, lv, meterMode, hold, fall, ui, frozen); break;
    default: {
      float hw = (sz.x - 6) * 0.5f, hh = (sz.y - 6) * 0.5f;
      float vw = std::min(hh + 20, hw * 0.45f);
      drawWave(a, dl, p, ImVec2(p.x + hw, p.y + hh), ring, sr, win, gain, ui, frozen);
      drawSpectrum(a, dl, ImVec2(p.x + hw + 6, p.y), ImVec2(e.x, p.y + hh), ring, sr, fftSize, range, tilt, gain, ui, frozen);
      drawVector(a, dl, ImVec2(p.x, p.y + hh + 6), ImVec2(p.x + vw, e.y), ring, gain, vecMode, corr, ui, frozen);
      drawLoudness(a, dl, ImVec2(p.x + vw + 6, p.y + hh + 6), e, lv, meterMode, hold, fall, ui, frozen);
      break;
    }
  }
  if (!ring) textCentred(dl, a.fontRegular, 13, ImVec2((p.x + e.x) * 0.5f, (p.y + e.y) * 0.5f), P.textDim, "Waiting for audio…");
  if (frozen) textAt(dl, a.fontMonoBold, 10.5f, ImVec2(e.x - 58, e.y - 16), K.select, "FROZEN");

  // ---- settings sheet for the current view ----
  static const char* sheetNames[SvCount] = {"OSCILLOSCOPE", "SPECTRUM", "SONOGRAM", "VECTORSCOPE", "METER", "GRID"};
  constexpr float sx = 7, sy = 219;
  soberPanel(dl, F, sx, sy, 578, 57, K, sheetNames[mode]);
  const ImU32 blue = skin::hex(0x50A0FF), orange = skin::hex(0xFFA600), lime = skin::hex(0xBCFF40), red = skin::hex(0xFF4060);
  auto wheelAt = [&](int param, float x, ImU32 fill) {
    skin::wheel(a, t, param, F.p(sx + x, sy + 12), 40 * F.s, K, skin::WheelBigLine, fill);
  };
  auto label = [&](float x, const char* s) { rowLabel(dl, F, sx + x, sy + 32, K, s); };
  auto menu = [&](int param, float x, float w) { skin::menuBox(a, t, param, F.p(sx + x, sy + 21), F.sz(w, 22), K, fs); };
  auto valueAt = [&](int param, float x) {
    auto d = describeParam(a.doc.song(), t.pid(param));
    if (!d) return;
    std::string v = d->format(val(st, param));
    skin::valueStrip(dl, F.p(sx + x, sy + 25), F.p(sx + x + 52, sy + 39), K, v.c_str(), fs);
  };
  if (mode == SvOsc) {
    label(10, "WINDOW");
    wheelAt(ScWindow, 58, blue);
    valueAt(ScWindow, 102);
    label(215, "VECTOR");
    menu(ScVecMode, 262, 101);
  } else if (mode == SvSpectrum || mode == SvSonogram || mode == SvGrid) {
    label(10, "MAX RES");
    menu(ScRes, 70, 80);
    label(162, "SLOPE");
    wheelAt(ScTilt, 205, orange);
    valueAt(ScTilt, 249);
    label(310, "DB RANGE");
    wheelAt(ScRange, 375, orange);
    valueAt(ScRange, 419);
    if (mode == SvGrid) {
      label(478, "WIN");
      wheelAt(ScWindow, 512, blue);
    }
  } else if (mode == SvVector) {
    label(10, "MODE");
    menu(ScVecMode, 52, 101);
  } else if (mode == SvMeters) {
    label(10, "MODE");
    menu(ScMeterMode, 52, 101);
    label(170, "HOLD TIME");
    wheelAt(ScHold, 245, lime);
    valueAt(ScHold, 289);
    label(366, "DECAY");
    wheelAt(ScDecay, 415, lime);
    valueAt(ScDecay, 459);
  }
  (void)red;

  // ---- view tabs ----
  {
    static const char* tabs[SvCount] = {"WAVE", "SPECTRUM", "SONO", "VECTOR", "METER", "GRID"};
    skin::switchSel(a, t, ScMode, F.p(6, 283), F.sz(450, 26), K, tabs, SvCount, true);
  }
  F.end(kW, kH);
}

// ============================================================== spectrogram

namespace {
struct SpectroUi {
  SpectrumAnalyzer an[2];
  std::vector<float> bins[2], bars[2], peaks[2], peakAge[2];
  Sonogram sono;
};
std::map<uint64_t, SpectroUi> spectroUis;

const skin::Skin& spectroSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2D3453, 0x272832, 0x3F484D, 0xFFA830, 0x3F484D, 0x9FA5A9);
    s.lcdBg = skin::hex(0x1C1F2E);
    s.lcdText = skin::hex(0xFFC060);
    s.graphBg = skin::hex(0x000000);
    s.pillBg = skin::hex(0x272832);
    s.select = skin::hex(0xFFA830);
    return s;
  }();
  return k;
}
}  // namespace

// Reference panel 285 × 159: the spectrum view on the left, stereo / peaks /
// windowing switches, the mode selector and the AMP / SCALE wheels on the right.
void editorSpectro(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = spectroSkin();
  constexpr float kW = 285, kH = 159;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  UtilityFx* fx = liveFx(a, t);
  const StereoRing* ring = fx ? fx->ring() : nullptr;
  SpectroUi& ui = spectroUis[t.key()];
  const double sr = a.engine.sampleRate(), nyq = std::min(sr * 0.5, 22000.0);
  int mode = static_cast<int>(val(st, SgMode) + 0.5f);
  bool stereo = val(st, SgStereo) >= 0.5f, peaksOn = val(st, SgPeaks, 1) >= 0.5f, windowOn = val(st, SgWindow, 1) >= 0.5f;
  double amp = val(st, SgAmp), scale = val(st, SgScale, 0.8f);
  int speed = static_cast<int>(val(st, SgSpeed, 1) + 0.5f);
  const float fallRate[3] = {12.f, 30.f, 70.f};
  const double rangeDb = 90;

  ImVec2 p = F.p(6, 6), e = F.p(182, 153);
  ImVec2 sz(e.x - p.x, e.y - p.y);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##spectro", sz);
  skin::graphWell(dl, p, e, K);
  dl->PushClipRect(p, e, true);
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
    if (f > nyq) continue;
    float u = static_cast<float>(hzToAxis(f, scale, nyq));
    if (mode == SpSonogram) {
      float y = e.y - u * sz.y;
      dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + 6, y), P.textFaint);
      char buf[12];
      std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
      textAt(dl, a.fontMono, 10.0f, ImVec2(p.x + 8, y - 7), P.textFaint, buf);
    } else {
      float x = p.x + u * sz.x;
      dl->AddLine(ImVec2(x, p.y), ImVec2(x, e.y), skin::hex(0x1E2230));
    }
  }
  float dt = ImGui::GetIO().DeltaTime;
  const ImU32 barLo = skin::hex(0x8A5A20), barHi = skin::hex(0xFFD080);
  if (mode == SpSonogram) {
    int hop = speed == 0 ? 1024 : speed == 1 ? 512 : 256;
    ui.sono.update(ring, sr, 2048, hop, rangeDb, amp, 0, scale, std::max(32, static_cast<int>(sz.x / 3)), std::max(24, static_cast<int>(sz.y / 3)));
    ui.sono.draw(dl, p, e);
  } else {
    const int bands = mode == SpBars ? 64 : std::max(32, static_cast<int>(sz.x / 2));
    int chans = stereo ? 2 : 1;
    for (int c = 0; c < chans; ++c) {
      ui.an[c].analyse(ring, 4096, windowOn, stereo ? c + 1 : 0, 0, ui.bins[c]);
      auto& bars = ui.bars[c];
      auto& pk = ui.peaks[c];
      auto& age = ui.peakAge[c];
      if (static_cast<int>(bars.size()) != bands) {
        bars.assign(static_cast<size_t>(bands), -200.f);
        pk.assign(static_cast<size_t>(bands), -200.f);
        age.assign(static_cast<size_t>(bands), 0.f);
      }
      for (int k = 0; k < bands; ++k) {
        double f0 = axisToHz(static_cast<double>(k) / bands, scale, nyq), f1 = axisToHz(static_cast<double>(k + 1) / bands, scale, nyq);
        float db = bandDb(ui.bins[c], f0, f1, sr) + static_cast<float>(amp + 1.5 * std::log2(std::sqrt(f0 * f1) / 1000.0));
        float& b = bars[static_cast<size_t>(k)];
        b = db > b ? db : std::max(db, b - fallRate[speed] * dt);
        if (b >= pk[static_cast<size_t>(k)]) {
          pk[static_cast<size_t>(k)] = b;
          age[static_cast<size_t>(k)] = 0;
        } else if ((age[static_cast<size_t>(k)] += dt) > 0.7f) {
          pk[static_cast<size_t>(k)] -= 20.f * dt;
        }
      }
      float base = stereo ? (p.y + e.y) * 0.5f : e.y, span = stereo ? sz.y * 0.5f - 4 : sz.y - 4;
      float dir = stereo && c == 1 ? 1.f : -1.f;
      auto h = [&](float db) { return std::clamp((db + static_cast<float>(rangeDb)) / static_cast<float>(rangeDb), 0.f, 1.f) * span; };
      if (mode == SpBars) {
        float bw = sz.x / bands;
        for (int k = 0; k < bands; ++k) {
          float x0 = p.x + k * bw + 1, x1 = p.x + (k + 1) * bw - 1;
          float hh = h(bars[static_cast<size_t>(k)]);
          if (hh > 0.5f)
            dl->AddRectFilled(ImVec2(x0, std::min(base, base + dir * hh)), ImVec2(x1, std::max(base, base + dir * hh)), theme::mix(barLo, barHi, hh / span),
                              1.0f);
          if (peaksOn) {
            float yp = base + dir * h(pk[static_cast<size_t>(k)]);
            dl->AddRectFilled(ImVec2(x0, yp - 1), ImVec2(x1, yp + 1), skin::hex(0xFFFFFF));
          }
        }
      } else {
        std::vector<ImVec2> pts(static_cast<size_t>(bands)), pp(static_cast<size_t>(bands));
        for (int k = 0; k < bands; ++k) {
          float x = p.x + sz.x * (k + 0.5f) / bands;
          pts[static_cast<size_t>(k)] = ImVec2(x, base + dir * h(bars[static_cast<size_t>(k)]));
          pp[static_cast<size_t>(k)] = ImVec2(x, base + dir * h(pk[static_cast<size_t>(k)]));
        }
        for (int k = 0; k + 1 < bands; ++k) {
          ImVec2 q[4] = {pts[static_cast<size_t>(k)], pts[static_cast<size_t>(k + 1)], ImVec2(pts[static_cast<size_t>(k + 1)].x, base),
                         ImVec2(pts[static_cast<size_t>(k)].x, base)};
          if (dir > 0) std::swap(q[0], q[3]), std::swap(q[1], q[2]);
          dl->AddConvexPolyFilled(q, 4, theme::withAlpha(K.fill, 0.18f));
        }
        if (peaksOn) dl->AddPolyline(pp.data(), bands, theme::withAlpha(skin::hex(0xFFFFFF), 0.6f), 0, 1.0f);
        dl->AddPolyline(pts.data(), bands, K.fill, 0, 1.5f);
      }
      if (stereo) textAt(dl, a.fontMono, 10.5f, ImVec2(e.x - 16, c ? e.y - 30 : p.y + 6), P.textFaint, c ? "R" : "L");
    }
    if (stereo) dl->AddLine(ImVec2(p.x, (p.y + e.y) * 0.5f), ImVec2(e.x, (p.y + e.y) * 0.5f), skin::hex(0x3A3F55));
  }
  if (mode != SpSonogram)
    for (double f : {100.0, 1000.0, 10000.0}) {
      if (f > nyq) continue;
      float x = p.x + static_cast<float>(hzToAxis(f, scale, nyq)) * sz.x;
      char buf[12];
      std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
      textAt(dl, a.fontMono, 10.0f, ImVec2(x + 3, e.y - 14), P.textDim, buf);
    }
  dl->PopClipRect();
  if (!ring) textCentred(dl, a.fontRegular, 13, ImVec2((p.x + e.x) * 0.5f, (p.y + e.y) * 0.5f), P.textDim, "Waiting for audio…");
  if (ImGui::IsItemHovered()) {
    ImGuiIO& io = ImGui::GetIO();
    double u = mode == SpSonogram ? (e.y - io.MousePos.y) / sz.y : (io.MousePos.x - p.x) / sz.x;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.0f Hz", axisToHz(u, scale, nyq));
    a.setHint(buf);
  }

  // ---- right column ----
  const float fs = F.font(7.5f);
  {
    static const char* modes[3] = {"BAR", "LINE", "SONO"};
    skin::switchSel(a, t, SgMode, F.p(190, 5), F.sz(91, 16), K, modes, 3, true);
  }
  skin::check(a, t, SgStereo, F.p(191, 26), F.sz(88, 11), K, "STEREO");
  skin::check(a, t, SgPeaks, F.p(191, 39), F.sz(88, 11), K, "SHOW PEAKS");
  skin::check(a, t, SgWindow, F.p(191, 52), F.sz(88, 11), K, "WINDOWING");
  {
    static const char* sp[3] = {"SLOW", "NORM", "FAST"};
    skin::switchSel(a, t, SgSpeed, F.p(190, 66), F.sz(91, 14), K, sp, 3, true);
  }
  skin::wheel(a, t, SgAmp, F.p(196, 84), 34 * F.s, K, skin::WheelBigLine);
  skin::pill(dl, F.p(234, 92), F.p(281, 108), K, "AMP", fs);
  skin::wheel(a, t, SgScale, F.p(196, 121), 34 * F.s, K, skin::WheelBigLine);
  skin::pill(dl, F.p(234, 129), F.p(281, 145), K, "SCALE", fs);
  F.end(kW, kH);
}

// ============================================================== level meter

namespace {
struct MeterUi {
  Ballistic pk[2], rms[2];
  float tpHold = 0;
};
std::map<uint64_t, MeterUi> meterUis;

const skin::Skin& meterSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x44545C, 0x3A4950, 0x3B4144, 0xBCFF40, 0x42474A, 0xC1C7CB);
    s.lcdBg = skin::hex(0x26313A);
    s.lcdText = skin::hex(0xD8FF90);
    s.pillBg = skin::hex(0x8998A0);
    s.pillText = skin::hex(0x36444D);
    return s;
  }();
  return k;
}
}  // namespace

// Reference panel 100 × 270 (dB labels, ruler, stereo volumeter); Gridline
// adds the read-out and meter settings column to the right of it.
void editorMeter(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = meterSkin();
  constexpr float kW = 360, kH = 270;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  UtilityFx* fx = liveFx(a, t);
  LevelState* lv = fx ? fx->levels() : nullptr;
  MeterUi& ui = meterUis[t.key()];
  int mode = static_cast<int>(val(st, LmMode, 2) + 0.5f);
  int scale = static_cast<int>(val(st, LmRange) + 0.5f);
  float lo = scale == 0 ? -36.f : scale == 1 ? -60.f : -96.f, hi = 6.f;
  float hold = val(st, LmHold, 1.5f), fall = val(st, LmDecay, 20);
  bool tp = val(st, LmTruePeak, 1) >= 0.5f;
  float dt = ImGui::GetIO().DeltaTime;
  if (lv) {
    float pl = tp ? lv->truePeakL.exchange(0) : lv->peakL.exchange(0), pr = tp ? lv->truePeakR.exchange(0) : lv->peakR.exchange(0);
    if (tp) {
      lv->peakL.store(0);
      lv->peakR.store(0);
    }
    ui.pk[0].feed(toDb(pl), dt, hold, fall);
    ui.pk[1].feed(toDb(pr), dt, hold, fall);
    ui.rms[0].feed(toDb(lv->rmsL.load()), dt, hold, fall);
    ui.rms[1].feed(toDb(lv->rmsR.load()), dt, hold, fall);
  }
  const float fs = F.font(7.5f);
  // ---- reference meter column ----
  const float top = 8, bot = 263;
  auto yOf = [&](float d) { return top + (hi - std::clamp(d, lo, hi)) / (hi - lo) * (bot - top); };
  const float step = scale == 2 ? 12.f : 6.f;
  for (float d = hi; d >= lo - 0.01f; d -= step) {
    char buf[12];
    if (d > 0.01f) std::snprintf(buf, sizeof buf, "+ %.0f", d);
    else if (d < -0.01f) std::snprintf(buf, sizeof buf, "- %.0f", -d);
    else std::snprintf(buf, sizeof buf, "0");
    float y = yOf(d);
    skin::pill(dl, F.p(0, y - 4.5f), F.p(26, y + 4.5f), K, buf, fs);
    dl->AddLine(F.p(28, y), F.p(38, y), theme::withAlpha(skin::hex(0xFFFFFF), 0.7f), 1.0f);
  }
  ImVec2 m0 = F.p(40, 0), m1 = F.p(100, 270);
  ImGui::SetCursorScreenPos(m0);
  ImGui::InvisibleButton("##meter", ImVec2(m1.x - m0.x, m1.y - m0.y));
  bool clicked = ImGui::IsItemClicked();
  dl->AddRectFilled(m0, m1, skin::darken(K.panel, 0.35f));
  for (int c = 0; c < 2; ++c) {
    float x0 = c == 0 ? 43 : 72;
    ImVec2 q0 = F.p(x0, top), q1 = F.p(x0 + 26, bot);
    float pdb = mode == 1 ? ui.rms[c].disp : ui.pk[c].disp;
    float rdb = mode == 2 ? ui.rms[c].disp : -200.f;
    float hdb = mode == 1 ? ui.rms[c].hold : ui.pk[c].hold;
    meterBar(dl, q0, q1, pdb, rdb, hdb, ui.pk[c].clip, lo, hi);
    bool clip = ui.pk[c].clip;
    dl->AddRectFilled(F.p(x0, 1), F.p(x0 + 26, 6), clip ? P.danger : skin::darken(K.panel, 0.55f), 1);
  }
  if (clicked) {
    ui.pk[0].clip = ui.pk[1].clip = false;
    ui.pk[0].hold = ui.pk[1].hold = ui.rms[0].hold = ui.rms[1].hold = -140;
    if (lv) lv->resetRequest.store(true);
  }
  if (ImGui::IsItemHovered()) a.setHint("Level meter · click to clear the clip lights and held peaks");

  // ---- read-out ----
  soberPanel(dl, F, 106, 5, 249, 128, K, "READOUT");
  auto row = [&](float y, const char* label, const std::string& v, ImU32 col) {
    skin::text(dl, F.p(114, y - 1), K.textDim, label, fs);
    skin::lcd(dl, F.p(114, y + 10), F.p(347, y + 27), K, "", fs);
    ImGui::PushFont(a.fontMonoBold, F.font(10.5f));
    ImVec2 ts = ImGui::CalcTextSize(v.c_str());
    ImVec2 c = F.p(230.5f, y + 18.5f);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), col, v.c_str());
    ImGui::PopFont();
  };
  float maxPk = lv ? lv->maxPeak.load() : 0.f, maxTp = lv ? lv->maxTruePeak.load() : 0.f;
  row(20, "PEAK HOLD  L / R", fmtDb(ui.pk[0].hold) + "  " + fmtDb(ui.pk[1].hold), K.lcdText);
  row(47, "RMS  L / R", fmtDb(ui.rms[0].disp) + "  " + fmtDb(ui.rms[1].disp), K.lcdText);
  row(74, "MAX SAMPLE PEAK", fmtDb(toDb(maxPk), " dB"), maxPk > 1.f ? P.danger : K.lcdText);
  row(101, "MAX TRUE PEAK", fmtDb(toDb(maxTp), " dBTP"), maxTp > 1.f ? P.danger : K.lcdText);

  // ---- settings ----
  soberPanel(dl, F, 106, 138, 249, 127, K, "METER");
  {
    static const char* md[3] = {"PEAK", "RMS", "PK+RMS"};
    static const char* rg[3] = {"36 DB", "60 DB", "96 DB"};
    skin::switchSel(a, t, LmMode, F.p(114, 152), F.sz(233, 18), K, md, 3, true);
    skin::switchSel(a, t, LmRange, F.p(114, 174), F.sz(233, 18), K, rg, 3, true);
  }
  skin::labeledWheel(a, t, LmHold, F, 120, 199, 40, K, "HOLD");
  skin::labeledWheel(a, t, LmDecay, F, 176, 199, 40, K, "FALL");
  skin::check(a, t, LmTruePeak, F.p(236, 212), F.sz(108, 14), K, "TRUE PEAK");
  F.end(kW, kH);
}

// ==================================================================== clock

// Reference panel 200 × 150: BAR / BEAT / TICK digits over MIN / SEC / 1/100
// digits, each in a recessed digit box; Gridline adds the display / source
// selectors underneath.
void editorClock(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  static const skin::Skin K = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x536566, 0x464E51, 0x8FD3D6, 0x3F484D, 0xB9BFC3);
    s.lcdBg = skin::hex(0x262B2F);
    return s;
  }();
  UtilityFx* fx = liveFx(a, t);
  const TransportState* ts = fx ? fx->transport() : nullptr;
  int disp = std::clamp(static_cast<int>(val(st, CkDisplay) + 0.5f), 0, 3);
  bool sincePlay = val(st, CkSource) >= 0.5f;
  const auto& prog = a.engine.program();
  double tick = ts ? ts->tick.load() : a.engine.position();
  double tempo = ts ? ts->tempo.load() : (prog ? prog->tempo : 140.0);
  int ppq = ts ? ts->ppq.load() : (prog ? prog->ppq : 96);
  double sr = ts ? ts->sampleRate.load() : a.engine.sampleRate();
  bool playing = ts ? ts->playing.load() : a.engine.playing();
  int64_t barTicks = prog ? prog->barTicks : ppq * 4, beatTicks = prog ? prog->beatTicks : ppq;
  double seconds = sincePlay ? (ts ? static_cast<double>(ts->playedSamples.load()) / sr : 0.0) : tick / std::max(1, ppq) * 60.0 / std::max(1.0, tempo);
  int64_t samples = sincePlay ? (ts ? ts->playedSamples.load() : 0) : static_cast<int64_t>(seconds * sr);
  double bt = sincePlay ? seconds * tempo / 60.0 * ppq : tick;
  int64_t it = static_cast<int64_t>(std::floor(bt + 1e-6));
  int64_t bar = it / std::max<int64_t>(1, barTicks) + 1, beat = (it % std::max<int64_t>(1, barTicks)) / std::max<int64_t>(1, beatTicks) + 1,
          tk = it % std::max<int64_t>(1, beatTicks);

  constexpr float kW = 200, kPanel = 72;
  const int panels = disp == CdBoth ? 2 : 1;
  const float kH = panels * kPanel + 42;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const ImU32 digitCol = skin::hex(0xE3ECEC), labelCol = skin::hex(0x8D989E);
  const float lf = F.font(8.0f);
  ImGui::SetCursorScreenPos(F.o);
  ImGui::InvisibleButton("##clock", F.sz(kW, panels * kPanel));
  if (ImGui::IsItemHovered()) a.setHint("Clock · follows the transport");
  // One digit panel: captions at form x, digit fields right-aligned at rights[], separators.
  auto digitPanel = [&](float y, const char* const* caps, const float* capX, const std::string* vals, const float* rights, int n, const char* const* seps,
                        float digitPx) {
    for (int k = 0; k < n; ++k) skin::text(dl, F.p(capX[k], y + 4), labelCol, caps[k], lf);
    ImVec2 b0 = F.p(5, y + 20), b1 = F.p(195, y + 66);
    dl->AddRectFilled(b0, b1, K.well, 3 * F.s);
    dl->AddRect(b0, b1, skin::darken(K.well, 0.4f), 3 * F.s, 0, 1.0f);
    float fsz = std::min(F.u(digitPx), 120.0f);
    ImGui::PushFont(app().fontMonoBold, fsz);
    float cy = (b0.y + b1.y) * 0.5f;
    for (int k = 0; k < n; ++k) {
      ImVec2 tsz = ImGui::CalcTextSize(vals[k].c_str());
      dl->AddText(ImVec2(std::round(F.p(rights[k], 0).x - tsz.x), std::round(cy - tsz.y * 0.5f)), digitCol, vals[k].c_str());
      if (seps && k + 1 < n) {
        ImVec2 ss = ImGui::CalcTextSize(seps[k]);
        float sx = F.p(rights[k] + 3, 0).x;
        dl->AddText(ImVec2(std::round(sx), std::round(cy - ss.y * 0.5f)), theme::withAlpha(digitCol, 0.55f), seps[k]);
      }
    }
    ImGui::PopFont();
  };
  float y = 0;
  char b[3][24];
  if (disp == CdBoth || disp == CdBars) {
    std::snprintf(b[0], 24, "%lld", static_cast<long long>(bar));
    std::snprintf(b[1], 24, "%02lld", static_cast<long long>(beat));
    std::snprintf(b[2], 24, "%03lld", static_cast<long long>(tk));
    static const char* L[] = {"BAR", "BEAT", "TICK"};
    static const float X[] = {14, 107, 170};
    static const float R[] = {94, 140, 190};
    static const char* S[] = {":", ":"};
    std::string v[3] = {b[0], b[1], b[2]};
    digitPanel(y, L, X, v, R, 3, S, 24);
    y += kPanel;
  }
  if (disp == CdBoth || disp == CdTime) {
    int64_t cs = static_cast<int64_t>(seconds * 100.0);
    std::snprintf(b[0], 24, "%lld", static_cast<long long>(cs / 6000));
    std::snprintf(b[1], 24, "%02lld", static_cast<long long>((cs / 100) % 60));
    std::snprintf(b[2], 24, "%02lld", static_cast<long long>(cs % 100));
    static const char* L[] = {"MIN", "SEC", "1/100"};
    static const float X[] = {13, 107, 165};
    static const float R[] = {94, 140, 190};
    static const char* S[] = {":", "."};
    std::string v[3] = {b[0], b[1], b[2]};
    digitPanel(y, L, X, v, R, 3, S, 24);
    y += kPanel;
  }
  if (disp == CdSamples) {
    std::snprintf(b[0], 24, "%lld", static_cast<long long>(samples));
    char cap[48];
    std::snprintf(cap, sizeof cap, "SAMPLES @ %.0f HZ", sr);
    static const float X[] = {14};
    static const float R[] = {190};
    const char* L[] = {cap};
    std::string v[1] = {b[0]};
    digitPanel(y, L, X, v, R, 1, nullptr, 18);
    y += kPanel;
  }
  // Play LED + tempo, then the selectors.
  skin::ledDot(dl, F.p(190, 5.5f), 2.5f * F.s, playing, K, skin::hex(0x8FE07A));
  {
    char info[96];
    std::snprintf(info, sizeof info, "%.2f BPM · %s", tempo, sincePlay ? "SINCE PLAY" : "SONG POSITION");
    skin::text(dl, F.p(8, y + 1), labelCol, info, F.font(6.5f));
  }
  {
    static const char* dn[4] = {"BOTH", "BARS", "TIME", "SMPL"};
    static const char* sn[2] = {"SONG", "PLAY"};
    skin::switchSel(a, t, CkDisplay, F.p(5, y + 13), F.sz(118, 22), K, dn, 4, true);
    skin::switchSel(a, t, CkSource, F.p(128, y + 13), F.sz(67, 22), K, sn, 2, true);
  }
  F.end(kW, kH);
}

// ==================================================================== notes

namespace {
struct NotesUi {
  bool preview = false;
  std::string buf;
  int bufPage = -1;
  std::string bufData;
  uint64_t session = 0;
  bool active = false;  // text box focused last frame
  bool init = false;
};
std::map<uint64_t, NotesUi> notesUis;

int resizeCb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(d->UserData);
    s->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = s->data();
  }
  return 0;
}

// Renders a page with light formatting: "# " headings, "- " bullets, "[ ]"/"[x]" tasks, "---" rules.
// Returns the index of a task line that was clicked (to toggle), or -1.
int renderPage(App& a, const std::string& text, float width) {
  const auto& P = pal();
  int clicked = -1, lineNo = 0;
  size_t pos = 0;
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (line.rfind("## ", 0) == 0) {
      ImGui::PushFont(a.fontBold, 14.0f);
      ImGui::TextWrapped("%s", line.c_str() + 3);
      ImGui::PopFont();
    } else if (line.rfind("# ", 0) == 0) {
      ImGui::PushFont(a.fontBold, 18.0f);
      ImGui::TextWrapped("%s", line.c_str() + 2);
      ImGui::PopFont();
    } else if (line.rfind("[ ] ", 0) == 0 || line.rfind("[x] ", 0) == 0 || line.rfind("[X] ", 0) == 0) {
      bool done = line[1] != ' ';
      ImGui::PushID(lineNo);
      ImVec2 q = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("task", ImVec2(14, 14))) clicked = lineNo;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRect(q, ImVec2(q.x + 14, q.y + 14), done ? P.accent : P.textDim, 3, 0, 1.5f);
      if (done) dl->AddRectFilled(ImVec2(q.x + 3, q.y + 3), ImVec2(q.x + 11, q.y + 11), P.accent, 2);
      ImGui::SameLine(0, 8);
      ImGui::TextColored(theme::vec(done ? P.textFaint : P.text), "%s", line.c_str() + 4);
      ImGui::PopID();
    } else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      ImGui::Bullet();
      ImGui::TextWrapped("%s", line.c_str() + 2);
    } else if (line == "---") {
      ImGui::Separator();
    } else if (line.empty()) {
      ImGui::Dummy(ImVec2(0, 6));
    } else {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ++lineNo;
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  ImGui::PopTextWrapPos();
  return clicked;
}
}  // namespace


// Reference panel 297 × 211: a light paper page filling the panel, the page
// number bar along the bottom and the edit switch at its right end.
void editorNotes(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  static const skin::Skin K = [] {
    skin::Skin s = skin::Skin::fl(0xCCD1D5, 0xBFC5CA, 0xA9B0B5, 0x4C7FB8, 0xB4BBC0, 0x2D2C28);
    s.panelHi = skin::hex(0xCCD1D5);
    s.panelLo = skin::hex(0xB8BEC3);
    s.lcdBg = skin::hex(0x2D2C28);
    s.lcdText = skin::hex(0xCCD1D5);
    s.select = skin::hex(0x4C7FB8);
    return s;
  }();
  constexpr float kW = 297, kH = 211;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  NotesUi& ui = notesUis[t.key()];
  std::vector<std::string> pages = decodeNotes(st.data);
  int page = std::clamp(static_cast<int>(val(st, NtPage, 1) + 0.5f), 1, kNotePages) - 1;
  if (!ui.init) {
    ui.init = true;
    ui.preview = !pages[static_cast<size_t>(page)].empty();  // open on reading, like a notebook
  }
  auto setPage = [&](int pg) { a.doc.setParam(t.pid(NtPage), std::clamp(pg, 0, kNotePages - 1) + 1, false); };
  if (ui.bufPage != page || (ui.bufData != st.data && !ui.active)) {
    ui.buf = pages[static_cast<size_t>(page)];
    ui.bufPage = page;
    ui.bufData = st.data;
  }

  // ---- page ----
  const ImU32 paper = skin::hex(0xCCD1D5), ink = skin::hex(0x2D2C28);
  ImVec2 t0 = F.p(0, 0), t1 = F.p(kW, 191);
  ImGui::SetCursorScreenPos(t0);
  ImVec2 tsz(t1.x - t0.x, t1.y - t0.y);
  if (ui.preview) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(paper));
    ImGui::BeginChild("##preview", tsz, ImGuiChildFlags_AlwaysUseWindowPadding);
    if (ui.buf.empty()) ImGui::TextColored(theme::vec(K.textDim), "%s", i18n::tr("This page is empty."));
    int toggled = renderPage(a, ui.buf, tsz.x - 20);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (toggled >= 0) {
      size_t pos = 0;
      for (int l = 0; l < toggled; ++l) pos = ui.buf.find('\n', pos) + 1;
      if (pos + 1 < ui.buf.size()) ui.buf[pos + 1] = ui.buf[pos + 1] == ' ' ? 'x' : ' ';
      pages[static_cast<size_t>(page)] = ui.buf;
      PluginState ns = st;
      ns.data = encodeNotes(pages);
      ui.bufData = ns.data;
      setPluginState(a, t, ns, "tick note task");
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(paper));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(ink));
    ImGui::PushFont(a.fontMono, 13.0f);
    bool changed = ImGui::InputTextMultiline("##page", ui.buf.data(), ui.buf.capacity() + 1, tsz, ImGuiInputTextFlags_CallbackResize, resizeCb, &ui.buf);
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    ui.active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) ui.session = 0x4E4F544500000000ull ^ (t.key() << 8) ^ static_cast<uint64_t>(ImGui::GetFrameCount());
    if (changed) {
      pages[static_cast<size_t>(page)] = ui.buf;
      PluginState ns = st;
      ns.data = encodeNotes(pages);
      ui.bufData = ns.data;
      setPluginState(a, t, ns, "edit notes", ui.session);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Notes are saved with the project · Preview renders # headings, - bullets, [ ] tasks");
  }

  // ---- page number bar ----
  const float fs = F.font(7.5f);
  dl->AddRectFilled(F.p(0, 192), F.p(kW, kH), skin::hex(0xB8BEC3));
  dl->AddLine(F.p(0, 192), F.p(kW, 192), skin::hex(0x9AA1A6), 1.0f);
  if (skin::button("##prev", F.p(2, 194), F.sz(14, 15), K, "<", false, fs)) setPage(page - 1);
  if (ImGui::IsItemHovered()) a.setHint("Previous page");
  {
    char buf[16];
    std::snprintf(buf, sizeof buf, "PAGE %d", page + 1);
    skin::lcd(dl, F.p(18, 194), F.p(62, 209), K, buf, fs);
  }
  if (skin::button("##next", F.p(64, 194), F.sz(14, 15), K, ">", false, fs)) setPage(page + 1);
  if (ImGui::IsItemHovered()) a.setHint("Next page");
  {
    ImVec2 q = F.p(82, 195), q1 = F.p(272, 208);
    ImGui::SetCursorScreenPos(q);
    ImGui::InvisibleButton("##strip", ImVec2(q1.x - q.x, q1.y - q.y));
    float cw = (q1.x - q.x) / kNotePages;
    for (int k = 0; k < kNotePages; ++k) {
      ImVec2 c0(q.x + k * cw, q.y + 1), c1(q.x + (k + 1) * cw - 1, q1.y - 1);
      ImU32 col = k == page ? K.select : !pages[static_cast<size_t>(k)].empty() ? ink : skin::hex(0xA3AAAF);
      dl->AddRectFilled(c0, c1, col, 1);
    }
    if (ImGui::IsItemHovered()) {
      int k = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - q.x) / cw), 0, kNotePages - 1);
      int used = 0;
      for (const auto& pg : pages) used += !pg.empty();
      char buf[96];
      std::snprintf(buf, sizeof buf, "Page %d%s · click to open · %d of %d pages used", k + 1, pages[static_cast<size_t>(k)].empty() ? " (empty)" : "", used,
                    kNotePages);
      a.setHint(buf);
      if (ImGui::IsItemClicked()) setPage(k);
    }
  }
  if (skin::button("##mode", F.p(275, 193), F.sz(21, 17), K, ui.preview ? "EDIT" : "VIEW", false, fs)) ui.preview = !ui.preview;
  if (ImGui::IsItemHovered()) a.setHint(ui.preview ? "Edit this page" : "Show the page formatted (# heading, - bullet, [ ] task, ---)");
  F.end(kW, kH);
}

// ======================================================= scope: reference engine
// Display behaviour of the reference visual (ScEngine = 1): refresh-gated
// oscilloscope with span / split / taper / smooth zoom, reassigned spectrum on
// a linear↔log axis, typed meters with hold and fall, styled vectorscope with
// trails, auto view, fade on silence. Colours stay Gridline's.
namespace {

struct VecFrame {
  double t = 0;
  std::vector<float> l, r;
};
struct ScopeRefUi {
  std::vector<float> wl, wr;
  uint64_t waveAt = 0;
  CandySpectrum spec;
  std::vector<float> sl, sr, mono, cols;
  uint64_t specAt = 0;
  CandyMeter meter;
  std::vector<float> ml, mr;
  uint64_t meterAt = 0;
  std::deque<VecFrame> vec;
  uint64_t vecAt = 0;
  std::vector<float> probeL, probeR;
  double lastLoud = -1e9;
};
std::map<uint64_t, ScopeRefUi> scopeRefUis;

// True once `ms` of new audio has arrived since `at` (then moves `at`).
bool due(const StereoRing* ring, uint64_t& at, double ms, double sr) {
  if (!ring) return false;
  uint64_t w = ring->written();
  uint64_t step = static_cast<uint64_t>(std::max(10.0, ms * sr / 1000.0));
  if (w < at) at = 0;
  if (w - at < step) return false;
  at = w;
  return true;
}

std::string scopeTitle(const std::string& data) {
  if (data.compare(0, 8, "GLSCOPE1") != 0) return {};
  size_t p = data.find("\ntitle=");
  if (p == std::string::npos) return {};
  p += 7;
  size_t e = data.find('\n', p);
  return data.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

float taperAt(double x) {
  const double edge = 0.1;
  double w = std::min({1.0, x / edge, (1.0 - x) / edge});
  w = std::max(0.0, w);
  double s = std::sin(1.5707963267948966 * w);
  return static_cast<float>(s * s);
}

void refWave(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, const PluginState& st, ScopeRefUi& ui, bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  const size_t n = static_cast<size_t>(std::clamp(val(st, ScWindow, 0.02f) * sr, 16.0, static_cast<double>(StereoRing::kSize / 2)));
  if (ui.wl.size() != n) {
    ui.wl.assign(n, 0.f);
    ui.wr.assign(n, 0.f);
    ui.waveAt = 0;
  }
  if (!frozen && due(ring, ui.waveAt, val(st, ScWaveRefresh, 10), sr)) ring->readLatest(ui.wl.data(), ui.wr.data(), n);
  const float g = static_cast<float>(dsp::dbToGain(val(st, ScGain)));
  const bool split = val(st, ScSplit) >= 0.5f, taper = val(st, ScTaper, 1) >= 0.5f, smooth = val(st, ScSmoothZoom, 1) >= 0.5f;
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const float gap = split ? h * std::clamp(val(st, ScSplitGap), 0.f, 100.f) / 100.f * 0.5f : 0.f;
  const float laneH = split ? (h - gap) * 0.5f : h;
  dl->PushClipRect(p0, p1, true);
  for (int c = 0; c < 2; ++c) {
    float top = split && c == 1 ? p1.y - laneH : p0.y;
    float cy = top + laneH * 0.5f, amp = laneH * 0.45f;
    if (c == 0 || split) dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), P.gridBeat);
    const float* x = c == 0 ? ui.wl.data() : ui.wr.data();
    ImU32 col = c == 0 ? P.paper : theme::withAlpha(P.paper, split ? 0.85f : 0.5f);
    const int cols = std::max(1, static_cast<int>(w));
    auto sampleY = [&](double v, double pos) { return cy - std::clamp(static_cast<float>(v) * g * (taper ? taperAt(pos) : 1.f), -1.1f, 1.1f) * amp; };
    if (static_cast<int>(n) >= cols) {
      for (int px = 0; px < cols; ++px) {
        size_t a0 = static_cast<size_t>(static_cast<double>(px) / cols * n), a1 = std::max(a0 + 1, static_cast<size_t>(static_cast<double>(px + 1) / cols * n));
        float mn = 1e9f, mx = -1e9f;
        for (size_t i = a0; i < a1 && i < n; ++i) mn = std::min(mn, x[i]), mx = std::max(mx, x[i]);
        double pos = (px + 0.5) / cols;
        float ya = sampleY(mx, pos), yb = sampleY(mn, pos);
        dl->AddLine(ImVec2(p0.x + px + 0.5f, ya), ImVec2(p0.x + px + 0.5f, std::max(yb, ya + 1.0f)), col);
      }
    } else {
      // Zoomed in: 2x cubic interpolation (smooth zoom) or held samples.
      std::vector<ImVec2> pts;
      pts.reserve(n * 2 + 2);
      auto xs = [&](double i) { return p0.x + static_cast<float>(i / static_cast<double>(n - 1)) * w; };
      for (size_t i = 0; i < n; ++i) {
        double pos = static_cast<double>(i) / static_cast<double>(n - 1);
        pts.push_back(ImVec2(xs(static_cast<double>(i)), sampleY(x[i], pos)));
        if (i + 1 >= n) break;
        if (smooth) {
          float y0 = x[i > 0 ? i - 1 : 0], y1 = x[i], y2 = x[i + 1], y3 = x[std::min(i + 2, n - 1)];
          float mid = 0.5625f * (y1 + y2) - 0.0625f * (y0 + y3);
          pts.push_back(ImVec2(xs(i + 0.5), sampleY(mid, (i + 0.5) / static_cast<double>(n - 1))));
        } else {
          pts.push_back(ImVec2(xs(static_cast<double>(i + 1)), sampleY(x[i], pos)));
        }
      }
      dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, 1.3f);
    }
  }
  dl->PopClipRect();
}

void refSpectrum(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, const PluginState& st, ScopeRefUi& ui,
                 bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const double curve = val(st, ScFreqCurve), range = val(st, ScRange, 96), nyq = sr * 0.5;
  int idx = std::clamp(static_cast<int>(val(st, ScSpecBands, 5) + 0.5f), 0, 8);
  if (curve < 1e-3)  // linear axis: no more bands than 1.5 x the display width
    while (idx > 0 && scopeBands(idx) > 1.5 * w) --idx;
  const int fft = scopeFftSize(idx);
  ui.spec.init(fft);
  const int ncol = std::max(16, static_cast<int>(w / 2));
  if (!frozen && due(ring, ui.specAt, val(st, ScSpecRefresh, 15), sr)) {
    ui.sl.resize(static_cast<size_t>(fft));
    ui.sr.resize(static_cast<size_t>(fft));
    ui.mono.resize(static_cast<size_t>(fft));
    ring->readLatest(ui.sl.data(), ui.sr.data(), static_cast<size_t>(fft));
    for (size_t i = 0; i < ui.mono.size(); ++i) ui.mono[i] = 0.5f * (ui.sl[i] + ui.sr[i]);
    ui.spec.analyse(ui.mono.data(), sr, val(st, ScSharpen, 1) >= 0.5f);
    ui.spec.columns(ncol, curve, val(st, ScPink, 1) >= 0.5f, val(st, ScGain), sr, ui.cols);
  }
  auto xOf = [&](double hz) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 32; ++i) {
      double m = (lo + hi) * 0.5;
      (candyAxisHz(m, curve, nyq) < hz ? lo : hi) = m;
    }
    return p0.x + static_cast<float>((lo + hi) * 0.5) * w;
  };
  auto yOf = [&](double db) { return p0.y + static_cast<float>(-db / range) * h; };
  float lastLabel = -1e9f;
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
    if (f >= nyq) continue;
    float x = xOf(f);
    bool major = f == 100.0 || f == 1000.0 || f == 10000.0;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), major ? P.gridBar : P.gridStep);
    if ((!major && curve < 0.5) || x - lastLabel < 30) continue;
    lastLabel = x;
    char buf[12];
    std::snprintf(buf, sizeof buf, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
    textAt(dl, a.fontMono, 10.0f, ImVec2(x + 3, p1.y - 14), P.textFaint, buf);
  }
  for (double d = -12; d > -range; d -= 12) {
    float y = yOf(d);
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), P.gridStep);
    char buf[12];
    std::snprintf(buf, sizeof buf, "%.0f", d);
    if (y < p1.y - 28) textAt(dl, a.fontMono, 10.0f, ImVec2(p0.x + 4, y + 1), P.textFaint, buf);
  }
  if (static_cast<int>(ui.cols.size()) == ncol) {
    std::vector<ImVec2> line;
    line.reserve(ui.cols.size());
    for (int k = 0; k < ncol; ++k) {
      float v = ui.cols[static_cast<size_t>(k)];
      if (v <= -199.f) continue;
      line.push_back(ImVec2(p0.x + w * (k + 0.5f) / ncol, std::clamp(yOf(v), p0.y, p1.y)));
    }
    for (size_t k = 0; k + 1 < line.size(); ++k) {
      ImVec2 q[4] = {line[k], line[k + 1], ImVec2(line[k + 1].x, p1.y), ImVec2(line[k].x, p1.y)};
      dl->AddConvexPolyFilled(q, 4, theme::withAlpha(P.text, 0.12f));
    }
    if (line.size() > 1) dl->AddPolyline(line.data(), static_cast<int>(line.size()), P.paper, 0, 1.4f);
  }
  if (ImGui::IsMouseHoveringRect(p0, p1)) {
    ImGuiIO& io = ImGui::GetIO();
    double hz = candyAxisHz((io.MousePos.x - p0.x) / w, curve, nyq);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.0f Hz · %.1f dB · FFT %d (%d bands)", hz, -(io.MousePos.y - p0.y) / h * range, fft, fft / 2);
    a.setHint(buf);
  }
}

void refMeter(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, const PluginState& st, ScopeRefUi& ui,
              bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  CandyMeter::Config c;
  c.type = std::clamp(static_cast<int>(val(st, ScMeterType) + 0.5f), 0, SmtCount - 1);
  c.sr = sr;
  c.curve = val(st, ScMeterCurve, 0.5f);
  c.holdMs = val(st, ScHold, 2.5f) * 1000.0;
  c.holdInfinite = val(st, ScHoldInf) >= 0.5f;
  c.fallMs = val(st, ScFallTime, 750);
  ui.meter.configure(c);
  if (!frozen && ring) {
    uint64_t wpos = ring->written();
    if (ui.meterAt == 0 || wpos < ui.meterAt || wpos - ui.meterAt > StereoRing::kSize / 2) ui.meterAt = wpos - std::min<uint64_t>(wpos, 960);
    const float g = static_cast<float>(dsp::dbToGain(val(st, ScGain)));
    while (ui.meterAt < wpos) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(wpos - ui.meterAt, 4096));
      ui.ml.resize(n);
      ui.mr.resize(n);
      if (!ring->readAt(ui.meterAt + n, ui.ml.data(), ui.mr.data(), n)) break;
      for (size_t i = 0; i < n; ++i) ui.ml[i] *= g, ui.mr[i] *= g;
      ui.meter.feed(ui.ml.data(), ui.mr.data(), n);
      ui.meterAt += n;
    }
  }
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const float bt = p0.y + 14, bb = p1.y - 14;
  const float bw = std::min(46.f, w * 0.12f), bx = p0.x + 64;
  auto yAmp = [&](double amp) { return bb - static_cast<float>(candyMeterPos(amp, c.curve)) * (bb - bt); };
  for (double db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0}) {
    float y = yAmp(std::pow(10.0, db / 20.0));
    char buf[12];
    std::snprintf(buf, sizeof buf, db > 0 ? "+%.0f" : "%.0f", db);
    textAt(dl, a.fontMono, 10.0f, ImVec2(p0.x + 30, y - 6), db == 0 ? P.text : P.textFaint, buf);
    dl->AddLine(ImVec2(bx - 4, y), ImVec2(bx + 2 * bw + 10, y), theme::withAlpha(P.gridStep, 0.8f));
  }
  const float y6 = yAmp(0.5011872), y0 = yAmp(1.0);
  for (int ch = 0; ch < 2; ++ch) {
    float x0 = bx + ch * (bw + 6), x1 = x0 + bw;
    dl->AddRectFilled(ImVec2(x0, bt), ImVec2(x1, bb), P.surface800, 3);
    float y = bb - static_cast<float>(ui.meter.pos(ch)) * (bb - bt);
    if (y < bb) {
      dl->AddRectFilled(ImVec2(x0, std::max(y, y6)), ImVec2(x1, bb), P.meterLow, 3);
      if (y < y6) dl->AddRectFilled(ImVec2(x0, std::max(y, y0)), ImVec2(x1, y6), P.meterMid);
      if (y < y0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y0), P.danger);
    }
    float hy = bb - static_cast<float>(ui.meter.hold(ch)) * (bb - bt);
    if (hy < bb - 1) dl->AddLine(ImVec2(x0, hy), ImVec2(x1, hy), hy < y0 ? P.danger : P.paper, 2.0f);
  }
  static const char* names[SmtCount] = {"PEAK", "PPM", "RMS", "VU", "LEQ(A)", "LOUDNESS"};
  float rx = bx + 2 * bw + 28;
  if (rx + 120 < p1.x) {
    textAt(dl, a.fontMono, 10.0f, ImVec2(rx, p0.y + 14), P.textFaint, names[c.type]);
    for (int ch = 0; ch < 2; ++ch) {
      double lv = ui.meter.level(ch);
      std::string v = lv > 1e-7 ? fmtDb(static_cast<float>(20 * std::log10(lv)), c.type == SmtLoudness ? " LUFS" : " dB") : std::string("-inf");
      textAt(dl, a.fontMonoBold, 18.0f, ImVec2(rx, p0.y + 30 + ch * 28), P.paper, (std::string(ch ? "R " : "L ") + v).c_str());
    }
  }
  (void)h;
}

void refVector(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const StereoRing* ring, double sr, const PluginState& st, ScopeRefUi& ui, bool frozen) {
  const auto& P = pal();
  panelBg(dl, p0, p1);
  const double now = ImGui::GetTime();
  const size_t nb = static_cast<size_t>(std::max(10.0, val(st, ScVecRefresh, 10) * sr / 1000.0));
  if (!frozen && due(ring, ui.vecAt, val(st, ScVecRefresh, 10), sr)) {
    VecFrame f;
    f.t = now;
    f.l.assign(nb, 0.f);
    f.r.assign(nb, 0.f);
    ring->readLatest(f.l.data(), f.r.data(), nb);
    ui.vec.push_back(std::move(f));
  }
  const double trail = 8.1567;  // 1/s: per-sample decay exp(-8.1567/sr)
  while (!ui.vec.empty() && (ui.vec.size() > 64 || (!frozen && now - ui.vec.front().t > 0.6))) ui.vec.pop_front();
  float side = std::min(p1.x - p0.x, p1.y - p0.y) - 12;
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float rad = side * 0.5f, d = rad * 0.7071f;
  dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), P.gridStep);
  dl->AddLine(ImVec2(c.x + d, c.y - d), ImVec2(c.x - d, c.y + d), P.gridStep);
  dl->AddLine(ImVec2(c.x, c.y - rad), ImVec2(c.x, c.y + rad), P.gridBeat);
  dl->AddLine(ImVec2(c.x - rad, c.y), ImVec2(c.x + rad, c.y), P.gridStep);
  const float g = static_cast<float>(dsp::dbToGain(val(st, ScGain)));
  const int style = std::clamp(static_cast<int>(val(st, ScVecStyle, 2) + 0.5f), 0, 3);
  const float width = std::clamp(val(st, ScVecWidth, 3.5f), 1.f, 6.f);
  const float base = val(st, ScVecAdd, 1) >= 0.5f ? 0.4f : 0.9f;
  dl->PushClipRect(p0, p1, true);
  std::vector<ImVec2> pts;
  for (const VecFrame& f : ui.vec) {
    float al = static_cast<float>(std::exp(-(frozen ? 0.0 : now - f.t) * trail)) * base;
    if (al < 0.01f) continue;
    pts.resize(f.l.size());
    for (size_t i = 0; i < f.l.size(); ++i) pts[i] = ImVec2(c.x + (f.r[i] - f.l[i]) * 0.5f * g * rad, c.y - (f.l[i] + f.r[i]) * 0.5f * g * rad);
    int np = static_cast<int>(pts.size());
    switch (style) {
      case SvsThin: dl->AddPolyline(pts.data(), np, theme::withAlpha(P.paper, al), 0, 1.0f); break;
      case SvsRibbon: dl->AddPolyline(pts.data(), np, theme::withAlpha(P.paper, al * 0.6f), 0, width); break;
      case SvsTube:
        dl->AddPolyline(pts.data(), np, theme::withAlpha(P.text, al * 0.45f), 0, width);
        dl->AddPolyline(pts.data(), np, theme::withAlpha(P.paper, al), 0, std::max(1.0f, width - 1.5f));
        break;
      default:
        dl->AddPolyline(pts.data(), np, theme::withAlpha(P.paper, al * 0.15f), 0, width * 2.5f);
        dl->AddPolyline(pts.data(), np, theme::withAlpha(P.paper, al), 0, 1.0f);
        break;
    }
  }
  dl->PopClipRect();
}

}  // namespace

// Reference panel 592 × 315: INPUT panel (gain slider, fade, hold pad), the
// visual where the reference keeps its appearance / colour panels, the view
// page with its settings, the view tabs and the auto-view switch.
void editorScopeRef(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = scopeSkin();
  constexpr float kW = 592, kH = 315;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const auto& P = pal();
  UtilityFx* fx = liveFx(a, t);
  const StereoRing* ring = fx ? fx->ring() : nullptr;
  LevelState* lv = fx ? fx->levels() : nullptr;
  ScopeUi& legacy = scopeUis[t.key()];
  ScopeRefUi& ui = scopeRefUis[t.key()];
  const double sr = a.engine.sampleRate();
  const int sel = std::clamp(static_cast<int>(val(st, ScMode) + 0.5f), 0, SvCount - 1);
  const bool frozen = val(st, ScFreeze) >= 0.5f;
  const float fs = F.font(8.0f);
  const double now = ImGui::GetTime();

  // ---- INPUT panel ----
  soberPanel(dl, F, 7, 7, 116, 204, K, "INPUT");
  skin::vslider(a, t, ScGain, F.p(14, 24), F.sz(34, 112), K, K.fill, true);
  rowLabel(dl, F, 15, 145, K, "GAIN");
  {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%+.1f dB", val(st, ScGain));
    skin::lcd(dl, F.p(56, 24), F.p(116, 40), K, buf, fs);
  }
  if (skin::button("##reset", F.p(56, 45), F.sz(60, 17), K, "RESET", false, fs)) {
    if (lv) lv->resetRequest.store(true);
    ui.meter.reset();
    legacy.history.clear();
  }
  if (ImGui::IsItemHovered()) a.setHint("Clear the meter hold and the loudness memory");
  skin::check(a, t, ScFade, F.p(56, 68), F.sz(60, 14), K, "FADE");
  skin::wheel(a, t, ScFadeDelay, F.p(72, 86), 34 * F.s, K, skin::WheelBigLine, K.fill);
  {
    char buf[24];
    if (val(st, ScFade) >= 0.5f) std::snprintf(buf, sizeof buf, "%.0f ms", val(st, ScFadeDelay, 5000));
    else std::snprintf(buf, sizeof buf, "NO FADE");
    skin::valueStrip(dl, F.p(56, 123), F.p(116, 136), K, buf, fs);
  }
  {
    static const char* eng[2] = {"OLD", "REF"};
    skin::switchSel(a, t, ScEngine, F.p(56, 139), F.sz(60, 14), K, eng, 2, true);
  }
  skin::toggle(a, t, ScFreeze, F.p(13, 156), F.sz(104, 48), K, frozen ? "FROZEN" : "HOLD", F.font(10));

  // ---- visual ----
  ImVec2 p = F.p(131, 7), e = F.p(585, 211);
  ImVec2 sz(e.x - p.x, e.y - p.y);
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##display", sz);
  int view = sel;
  if (val(st, ScAutoView) >= 0.5f && sz.y > 1) {
    float aspect = sz.x / sz.y;  // tall → meter, square → vectorscope, wide → the chosen view
    if (aspect < 0.75f) view = SvMeters;
    else if (aspect < 1.3f) view = SvVector;
  }
  switch (view) {
    case SvOsc: refWave(dl, p, e, ring, sr, st, ui, frozen); break;
    case SvSpectrum: refSpectrum(a, dl, p, e, ring, sr, st, ui, frozen); break;
    case SvMeters: refMeter(a, dl, p, e, ring, sr, st, ui, frozen); break;
    case SvVector: refVector(dl, p, e, ring, sr, st, ui, frozen); break;
    case SvSonogram: {
      panelBg(dl, p, e);
      int fftSize = 1024 << std::clamp(static_cast<int>(val(st, ScRes, 2) + 0.5f), 0, 3);
      if (!frozen)
        legacy.sono.update(ring, sr, std::min(fftSize, 4096), 512, val(st, ScRange, 96), val(st, ScGain), val(st, ScTilt, 3), 1.0,
                           std::max(32, static_cast<int>(sz.x / 3)), std::max(24, static_cast<int>(sz.y / 3)));
      legacy.sono.draw(dl, p, e);
      break;
    }
    default: {
      float hw = (sz.x - 6) * 0.5f, hh = (sz.y - 6) * 0.5f;
      refWave(dl, p, ImVec2(p.x + hw, p.y + hh), ring, sr, st, ui, frozen);
      refSpectrum(a, dl, ImVec2(p.x + hw + 6, p.y), ImVec2(e.x, p.y + hh), ring, sr, st, ui, frozen);
      refVector(dl, ImVec2(p.x, p.y + hh + 6), ImVec2(p.x + hw, e.y), ring, sr, st, ui, frozen);
      refMeter(a, dl, ImVec2(p.x + hw + 6, p.y + hh + 6), e, ring, sr, st, ui, frozen);
      break;
    }
  }
  // Fade on silence.
  if (ring && !frozen) {
    const size_t n = 1024;
    ui.probeL.resize(n);
    ui.probeR.resize(n);
    ring->readLatest(ui.probeL.data(), ui.probeR.data(), n);
    float pk = 0;
    for (size_t i = 0; i < n; ++i) pk = std::max({pk, std::fabs(ui.probeL[i]), std::fabs(ui.probeR[i])});
    if (pk > 1e-4f || ui.lastLoud < -1e8) ui.lastLoud = now;
  }
  if (val(st, ScFade) >= 0.5f) {
    double quiet = now - ui.lastLoud - val(st, ScFadeDelay, 5000) / 1000.0;
    if (quiet > 0) dl->AddRectFilled(p, e, theme::withAlpha(P.surface950, static_cast<float>(std::min(1.0, quiet / 0.5))), 6);
  }
  {
    std::string title = scopeTitle(st.data);
    if (!title.empty()) textAt(dl, a.fontMonoBold, 10.5f, ImVec2(p.x + 8, p.y + 5), P.textDim, title.c_str());
  }
  if (!ring) textCentred(dl, a.fontRegular, 13, ImVec2((p.x + e.x) * 0.5f, (p.y + e.y) * 0.5f), P.textDim, "Waiting for audio…");
  if (frozen) textAt(dl, a.fontMonoBold, 10.5f, ImVec2(e.x - 58, e.y - 16), K.select, "FROZEN");

  // ---- view page ----
  static const char* sheetNames[SvCount] = {"OSCILLOSCOPE", "SPECTRUM", "SONOGRAM", "VECTORSCOPE", "METER", "GRID"};
  constexpr float sy = 219;
  soberPanel(dl, F, 7, sy, 578, 57, K, sheetNames[sel]);
  const ImU32 blue = skin::hex(0x50A0FF), orange = skin::hex(0xFFA600), lime = skin::hex(0xBCFF40);
  // Wheel at form x (40 units) with its caption and value in the column to its left.
  auto knob = [&](int param, float x, ImU32 fill, const char* cap) {
    skin::wheel(a, t, param, F.p(x, 227), 40 * F.s, K, skin::WheelBigLine, fill);
    skin::text(dl, F.p(x - 48, 233), K.textDim, cap, fs);
    if (auto d = describeParam(a.doc.song(), t.pid(param))) skin::text(dl, F.p(x - 48, 246), K.text, d->format(val(st, param)).c_str(), fs);
  };
  auto menu = [&](int param, float x, float w, const char* cap) {
    skin::text(dl, F.p(x, 225), K.textDim, cap, fs);
    skin::menuBox(a, t, param, F.p(x, 236), F.sz(w, 23), K, fs);
  };
  auto check = [&](int param, float x, float y, float w, const char* cap) { skin::check(a, t, param, F.p(x, y), F.sz(w, 18), K, cap); };
  if (sel == SvOsc) {
    knob(ScWaveRefresh, 57, blue, "REFRESH");
    knob(ScWindow, 160, blue, "SPAN");
    check(ScSplit, 214, 229, 44, "SPLIT");
    knob(ScSplitGap, 310, blue, "");
    check(ScTaper, 453, 225, 120, "TAPER EDGES");
    check(ScSmoothZoom, 453, 247, 120, "SMOOTH ZOOM");
  } else if (sel == SvSpectrum) {
    knob(ScSpecRefresh, 57, orange, "REFRESH");
    menu(ScSpecBands, 157, 89, "BANDS");
    knob(ScFreqCurve, 293, orange, "LOG AXIS");
    knob(ScRange, 400, orange, "RANGE");
    check(ScPink, 453, 225, 120, "PINK WEIGHT");
    check(ScSharpen, 453, 247, 120, "SHARPEN PEAKS");
  } else if (sel == SvMeters) {
    knob(ScMeterCurve, 57, lime, "CURVE");
    knob(ScHold, 168, lime, "HOLD");
    check(ScHoldInf, 212, 252, 42, "INF");
    menu(ScMeterType, 258, 101, "TYPE");
    knob(ScFallTime, 444, lime, "FALL");
  } else if (sel == SvVector) {
    knob(ScVecRefresh, 57, blue, "REFRESH");
    knob(ScVecWidth, 168, blue, "WIDTH");
    menu(ScVecStyle, 258, 101, "STYLE");
    check(ScVecAdd, 372, 238, 70, "ADD");
  } else {
    menu(ScRes, 60, 80, "FFT");
    knob(ScTilt, 200, orange, "SLOPE");
    knob(ScRange, 310, orange, "RANGE");
    if (sel == SvGrid) knob(ScWindow, 420, blue, "SPAN");
  }

  // ---- view tabs, auto view ----
  {
    static const char* tabs[SvCount] = {"WAVE", "SPECTRUM", "SONO", "VECTOR", "METER", "GRID"};
    skin::switchSel(a, t, ScMode, F.p(6, 283), F.sz(440, 26), K, tabs, SvCount, true);
  }
  skin::check(a, t, ScAutoView, F.p(454, 285), F.sz(130, 23), K, "AUTO VIEW");
  if (ImGui::IsItemHovered()) a.setHint("Auto view: tall displays show the meter, square ones the vectorscope");
  F.end(kW, kH);
}

}  // namespace gl::packs::utility
