// Gridline — "plasma" pack: the wavetable editor (replaces the page and the
// lower panel). Frame strip, a drawable waveform (freehand / line / smooth
// brushes, snap grid), editable harmonics, formulas (x = -1..1 across the
// cycle, y = 0..1 across the table, in = the current sample) and table
// processing (normalize, DC, reverse, invert, morph, resize, smooth).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../../core/AudioBuffer.h"
#include "../../core/Expr.h"
#include "../../core/I18n.h"
#include "../../core/WavWriter.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"
#include "../../app/Widgets.h"

namespace gl::packs::plasma::ui {

namespace {

// layout (the Serum 2 editor's arrangement): harmonics band on top, brushes
// on the left, waveform in the centre, table / process / grid on the right,
// frame strip and the position / clipboard / formula bar at the bottom.
constexpr float kLeftW = 150, kRightX = 1030;
constexpr float kHarmY = kPageY, kHarmH = 140, kMagH = 100;
constexpr float kWaveY = kPageY + kHarmH, kWaveH = 304;
constexpr float kFramesY = kWaveY + kWaveH, kFramesH = 66;
constexpr float kBarY = kFramesY + kFramesH;
constexpr int kHarm = 256;
constexpr int kMaxFrames = 256;
const double kTau = 6.283185307179586;

using Frames = std::vector<std::vector<float>>;

// The oscillator's table as editable frames (factory / file tables are copied).
Frames currentFrames(const Ctx& c, int o) {
  const OscData& d = c.m.osc[static_cast<size_t>(o)];
  if (d.table == "custom" && !d.frames.empty()) return d.frames;
  std::shared_ptr<const Table> t;
  if (d.table.rfind("file:", 0) == 0) t = loadTableFile(d.table.substr(5));
  if (!t) t = factoryTable(std::max(1, vali(c, oscP(o, O_Table))) - 1);
  Frames f;
  if (t)
    for (int i = 0; i < t->frames; ++i) f.emplace_back(t->frame(i), t->frame(i) + kFrameSize);
  if (f.empty()) {
    f.emplace_back(static_cast<size_t>(kFrameSize));
    for (int i = 0; i < kFrameSize; ++i) f[0][static_cast<size_t>(i)] = static_cast<float>(std::sin(kTau * i / kFrameSize));
  }
  return f;
}

// Stores frames as the oscillator's custom table (one undo step).
void storeFrames(Ctx& c, int o, Frames f, const char* label) {
  if (f.empty()) return;
  if (f.size() > kMaxFrames) f.resize(kMaxFrames);
  std::string name = c.m.osc[static_cast<size_t>(o)].tableName;
  if (name.empty() && c.m.osc[static_cast<size_t>(o)].table.empty()) {
    const auto& n = factoryTableNames();
    const int t = std::max(1, vali(c, oscP(o, O_Table))) - 1;
    if (t < static_cast<int>(n.size())) name = n[static_cast<size_t>(t)];
  }
  const int pk = oscP(o, O_Table);
  editState(c.a, c.t, label, [&](PluginState& st, Model& m) {
    auto& d = m.osc[static_cast<size_t>(o)];
    d.table = "custom";
    d.tableName = name.empty() ? "Custom" : name;
    d.frames = std::move(f);
    st.params[static_cast<size_t>(pk)] = 0;
  });
}

// Selects a factory table (no copy: the oscillator plays it by index, one undo step).
void selectFactory(Ctx& c, int o, int index) {
  const int pk = oscP(o, O_Table);
  editState(c.a, c.t, "Load table", [&](PluginState& st, Model& m) {
    auto& d = m.osc[static_cast<size_t>(o)];
    d.table.clear();
    d.tableName.clear();
    d.frames.clear();
    st.params[static_cast<size_t>(pk)] = static_cast<float>(index + 1);
  });
}

// cos / sin of 2*pi*k/kFrameSize (frames are always kFrameSize long)
const std::vector<float>& cosTab() {
  static const std::vector<float> t = [] {
    std::vector<float> v(static_cast<size_t>(kFrameSize));
    for (int i = 0; i < kFrameSize; ++i) v[static_cast<size_t>(i)] = static_cast<float>(std::cos(kTau * i / kFrameSize));
    return v;
  }();
  return t;
}
inline float cosAt(size_t k) { return cosTab()[k % static_cast<size_t>(kFrameSize)]; }
inline float sinAt(size_t k) { return cosTab()[(k + static_cast<size_t>(kFrameSize) * 3 / 4) % static_cast<size_t>(kFrameSize)]; }

struct Harm {
  float a[kHarm + 1] = {}, b[kHarm + 1] = {};
  float mag(int h) const { return std::hypot(a[h], b[h]); }
};
Harm analyse(const std::vector<float>& x, int nh = kHarm) {
  Harm r;
  const size_t n = x.size();
  for (int h = 1; h <= std::min(nh, kHarm); ++h) {
    double sa = 0, sb = 0;
    for (size_t i = 0; i < n; ++i) {
      const size_t k = static_cast<size_t>(h) * i;
      sa += static_cast<double>(x[i] * cosAt(k));
      sb += static_cast<double>(x[i] * sinAt(k));
    }
    r.a[h] = static_cast<float>(2 * sa / static_cast<double>(n));
    r.b[h] = static_cast<float>(2 * sb / static_cast<double>(n));
  }
  return r;
}
// Sets harmonic h's magnitude (keeps the phase; a silent harmonic becomes a sine).
void setHarm(std::vector<float>& x, const Harm& H, int h, float m) {
  const float m0 = H.mag(h);
  float na, nb;
  if (m0 > 1e-6f) na = H.a[h] * m / m0, nb = H.b[h] * m / m0;
  else na = 0, nb = m;
  const float da = na - H.a[h], db = nb - H.b[h];
  const size_t n = x.size();
  for (size_t i = 0; i < n; ++i) {
    const size_t k = static_cast<size_t>(h) * i;
    x[i] += da * cosAt(k) + db * sinAt(k);
  }
}

// Sets harmonic h's phase (keeps the magnitude).
void setPhase(std::vector<float>& x, const Harm& H, int h, float ph) {
  const float m = H.mag(h);
  const float na = m * std::sin(ph), nb = m * std::cos(ph);
  const float da = na - H.a[h], db = nb - H.b[h];
  for (size_t i = 0; i < x.size(); ++i) {
    const size_t k = static_cast<size_t>(h) * i;
    x[i] += da * cosAt(k) + db * sinAt(k);
  }
}
float phaseOf(const Harm& H, int h) {
  float p = std::atan2(H.a[h], H.b[h]);
  if (p < 0) p += 6.2831853f;
  return p;
}

void normalize(std::vector<float>& x) {
  float p = 0;
  for (float v : x) p = std::max(p, std::fabs(v));
  if (p > 1e-6f)
    for (float& v : x) v /= p;
}
void removeDc(std::vector<float>& x) {
  double s = 0;
  for (float v : x) s += v;
  const float m = static_cast<float>(s / static_cast<double>(std::max<size_t>(1, x.size())));
  for (float& v : x) v -= m;
}
// Keeps harmonics 1..keep (low-pass through the analysis).
void bandLimit(std::vector<float>& x, int keep) {
  const Harm H = analyse(x, keep);
  std::vector<float> y(x.size(), 0.f);
  for (int h = 1; h <= std::min(keep, kHarm); ++h)
    for (size_t i = 0; i < x.size(); ++i) {
      const double ph = kTau * h * static_cast<double>(i) / static_cast<double>(x.size());
      y[i] += static_cast<float>(H.a[h] * std::cos(ph) + H.b[h] * std::sin(ph));
    }
  x = std::move(y);
}
Frames resizeTable(const Frames& f, int n) {
  Frames r(static_cast<size_t>(n), std::vector<float>(static_cast<size_t>(kFrameSize)));
  for (int i = 0; i < n; ++i) {
    const double pos = n > 1 ? static_cast<double>(i) * static_cast<double>(f.size() - 1) / (n - 1) : 0.0;
    const size_t a = static_cast<size_t>(pos), b = std::min(f.size() - 1, a + 1);
    const float t = static_cast<float>(pos - static_cast<double>(a));
    for (size_t s = 0; s < static_cast<size_t>(kFrameSize); ++s) r[static_cast<size_t>(i)][s] = f[a][s] + (f[b][s] - f[a][s]) * t;
  }
  return r;
}
// Fills the frames between the first and the last: crossfade or spectral (magnitudes and phases).
Frames morphTable(const Frames& f, int n, bool spectral) {
  if (f.empty()) return f;
  const auto& A = f.front();
  const auto& B = f.back();
  Frames r(static_cast<size_t>(n), std::vector<float>(static_cast<size_t>(kFrameSize), 0.f));
  Harm ha, hb;
  if (spectral) ha = analyse(A, 128), hb = analyse(B, 128);
  for (int i = 0; i < n; ++i) {
    const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.f;
    auto& o = r[static_cast<size_t>(i)];
    if (!spectral) {
      for (size_t s = 0; s < o.size(); ++s) o[s] = A[s] + (B[s] - A[s]) * t;
      continue;
    }
    for (int h = 1; h <= 128; ++h) {
      const float m = ha.mag(h) + (hb.mag(h) - ha.mag(h)) * t;
      const float pa = std::atan2(ha.b[h], ha.a[h]), pb = std::atan2(hb.b[h], hb.a[h]);
      float dp = pb - pa;
      while (dp > 3.14159265f) dp -= 6.2831853f;
      while (dp < -3.14159265f) dp += 6.2831853f;
      const float p = pa + dp * t;
      if (m < 1e-6f) continue;
      for (size_t s = 0; s < o.size(); ++s) o[s] += m * static_cast<float>(std::cos(kTau * h * static_cast<double>(s) / kFrameSize - p));
    }
  }
  return r;
}

bool applyFormula(Frames& f, const std::string& text, bool all, int frame, std::string& err) {
  Expr e;
  if (!e.compile(text, {"x", "y", "in"}, &err)) return false;
  uint32_t rng = 0x9e3779b9u;
  for (size_t i = 0; i < f.size(); ++i) {
    if (!all && static_cast<int>(i) != frame) continue;
    double v[3];
    v[1] = f.size() > 1 ? static_cast<double>(i) / static_cast<double>(f.size() - 1) : 0.0;
    for (size_t s = 0; s < f[i].size(); ++s) {
      v[0] = -1.0 + 2.0 * static_cast<double>(s) / static_cast<double>(f[i].size());
      v[2] = f[i][s];
      const double y = e.eval(v, &rng);
      f[i][s] = std::isfinite(y) ? static_cast<float>(std::clamp(y, -4.0, 4.0)) : 0.f;
    }
  }
  return true;
}

// Thumbnail / waveform polyline.
void wave(const Ctx& c, const std::vector<float>& x, float x0, float y0, float w, float h, ImU32 col, float thick, bool fill) {
  const int n = std::max(8, static_cast<int>(c.s(w)));
  std::vector<ImVec2> pts;
  pts.reserve(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) {
    const size_t s = std::min(x.size() - 1, static_cast<size_t>(static_cast<double>(i) / n * static_cast<double>(x.size())));
    pts.push_back(c.p(x0 + w * i / n, y0 + h * 0.5f - std::clamp(x[s], -1.2f, 1.2f) * h * 0.46f));
  }
  if (fill) {
    const float mid = c.p(0, y0 + h * 0.5f).y;
    const ImDrawListFlags fl = c.dl->Flags;
    c.dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // no fringe dots between the slices
    for (int i = 0; i < n; ++i)
      c.dl->AddQuadFilled(ImVec2(pts[static_cast<size_t>(i)].x, mid), pts[static_cast<size_t>(i)], pts[static_cast<size_t>(i) + 1],
                          ImVec2(pts[static_cast<size_t>(i) + 1].x, mid), theme::withAlpha(col, 0.12f));
    c.dl->Flags = fl;
  }
  c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, thick);
}

// ---- brushes: a shape drawn into each grid cell the pointer passes (amplitude
// from the pointer height), plus freehand and line.
enum Brush : int { BrFree = 0, BrLine, BrFlat, BrSine, BrRampUp, BrRampDown, BrSUp, BrSDown, BrArch, BrValley, BrSteps, BrNoise, BrCount };
const char* const kBrushName[BrCount] = {"Freehand", "Line", "Flat", "Sine", "Ramp up", "Ramp down", "Curve up", "Curve down", "Arch", "Valley", "Steps", "Noise"};
float shapeAt(int b, float t, uint32_t& rng) {
  const float pi = 3.14159265f;
  switch (b) {
    case BrSine: return std::sin(pi * t);
    case BrRampUp: return 2 * t - 1;
    case BrRampDown: return 1 - 2 * t;
    case BrSUp: return -std::cos(pi * t);
    case BrSDown: return std::cos(pi * t);
    case BrArch: return 1 - 2 * (2 * t - 1) * (2 * t - 1);
    case BrValley: return 2 * (2 * t - 1) * (2 * t - 1) - 1;
    case BrSteps: return std::floor(std::min(t, 0.999f) * 4) / 1.5f - 1;
    case BrNoise: rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) / 8388608.f - 1;
    default: return 1;
  }
}
void brushIcon(const Ctx& c, int b, float x, float y, float w, float h, ImU32 col) {
  std::vector<ImVec2> pts;
  uint32_t rng = 7;
  if (b == BrFree) {
    for (int i = 0; i <= 24; ++i) {
      const float t = i / 24.f;
      pts.push_back(c.p(x + w * t, y + h * 0.5f - h * 0.35f * (std::sin(t * 9.f) * 0.6f + std::sin(t * 23.f) * 0.3f)));
    }
  } else if (b == BrLine) {
    pts = {c.p(x, y + h * 0.85f), c.p(x + w, y + h * 0.15f)};
    c.dl->AddCircleFilled(pts[0], c.s(2), col);
    c.dl->AddCircleFilled(pts[1], c.s(2), col);
  } else {
    const int n = b == BrSteps || b == BrNoise ? 16 : 24;
    for (int i = 0; i <= n; ++i) {
      const float t = static_cast<float>(i) / n;
      const float v = shapeAt(b, b == BrNoise ? t : t, rng) * (b == BrFlat ? 0 : 1);
      pts.push_back(c.p(x + w * t, y + h * 0.5f - v * h * 0.4f));
    }
    if (b == BrSteps) {  // rebuild as clean steps
      pts.clear();
      for (int k = 0; k < 4; ++k) {
        const float v = shapeAt(b, (k + 0.5f) / 4, rng);
        pts.push_back(c.p(x + w * k / 4, y + h * 0.5f - v * h * 0.4f));
        pts.push_back(c.p(x + w * (k + 1) / 4, y + h * 0.5f - v * h * 0.4f));
      }
    }
  }
  c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, std::max(1.2f, c.s(1.5f)));
}

// Spectral centroid (brightness) of a frame, for sorting.
float brightness(const std::vector<float>& x) {
  const Harm H = analyse(x, 64);
  double num = 0, den = 1e-9;
  for (int h = 1; h <= 64; ++h) num += h * static_cast<double>(H.mag(h)), den += H.mag(h);
  return static_cast<float>(num / den);
}
float rms(const std::vector<float>& x) {
  double s = 0;
  for (float v : x) s += static_cast<double>(v) * v;
  return static_cast<float>(std::sqrt(s / static_cast<double>(std::max<size_t>(1, x.size()))));
}

// Formula examples (own set) for the formula drop-down.
const char* const kFormulas[][2] = {
    {"Sine", "sin(pi*x)"},
    {"Saw", "-x"},
    {"Square", "sign(sin(pi*x))"},
    {"Triangle", "1-2*abs(x)"},
    {"Saw to square across the table", "lerp(-x, sign(sin(pi*x)), y)"},
    {"Harmonic sweep", "sin(pi*x*(1+round(y*15)))"},
    {"Pulse width sweep", "x < (y*1.8-0.9) ? 1 : -1"},
    {"Sync sweep", "sin(pi*(x+1)*(1+y*7))"},
    {"Fold the current frame", "fold(in*(1+y*4)*0.5+0.5)*2-1"},
    {"Soft clip the current frame", "tanh(in*(1+y*6))"},
    {"Bit reduce", "quant(in, 2+round((1-y)*30))"},
};

// Clipboard (one frame), shared by the editors.
std::vector<float>& clipFrame() {
  static std::vector<float> f;
  return f;
}

// Harmonic analysis of the shown frame, cached by content.
const Harm& harmOf(const std::vector<float>& x) {
  static Harm h;
  static size_t key = 0;
  size_t k = x.size();
  for (size_t i = 0; i < x.size(); i += 7) {
    uint32_t bits;
    std::memcpy(&bits, &x[i], 4);
    k = k * 1000003u ^ bits;
  }
  if (k != key) h = analyse(x), key = k;
  return h;
}

}  // namespace

void wtEditor(Ctx& c) {
  Ui& u = c.u;
  const int o = std::clamp(u.wtOsc, 0, kOscs - 1);
  panelRect(c, 0, kPageY, kW, kStripY - kPageY, c.P.frame);
  // frames: tools (left), harmonics + zoom (top), waveform, table tools (right), frames strip, bottom bar
  frameBox(c, kLeftW, kPageY, kW - kLeftW, kFramesY - kPageY);
  OscData& d = c.m.osc[static_cast<size_t>(o)];
  // A factory or file table is shown from a copy; the first edit stores it as the
  // oscillator's custom table (opening the editor alone changes nothing).
  const bool custom = d.table == "custom" && !d.frames.empty();
  static Frames view;
  static std::string viewKey;
  if (!custom) {
    const std::string key = std::to_string(o) + "|" + d.table + "|" + std::to_string(vali(c, oscP(o, O_Table)));
    if (key != viewKey || view.empty()) view = currentFrames(c, o), viewKey = key;
  }
  Frames& F = custom ? d.frames : view;
  std::string shownName = d.tableName;
  if (!custom && d.table.empty()) {
    const auto& fn = factoryTableNames();
    const int ti = std::max(1, vali(c, oscP(o, O_Table))) - 1;
    if (ti < static_cast<int>(fn.size())) shownName = fn[static_cast<size_t>(ti)];
  }
  const int nf = static_cast<int>(F.size());
  u.wtFrame = std::clamp(u.wtFrame, 0, nf - 1);
  auto& cur = F[static_cast<size_t>(u.wtFrame)];
  static bool pending = false;  // edited in place: committed when the gesture ends
  const char* did = nullptr;    // a table operation (stored at the end of the frame)
  Frames next;

  // ================================================================ left: oscillator + brushes
  frameBox(c, 0, kPageY, kLeftW, kFramesY - kPageY, c.P.panelLo);
  textC(c, kLeftW * 0.5f, kPageY + 8, "OSC", c.P.label, ty::kCap, true);
  for (int i = 0; i < kOscs; ++i) {
    const char* nm[] = {"A", "B", "C"};
    ImGui::PushID(i + 9000);
    if (button(c, "osc", 12, kPageY + 24 + i * 23, kLeftW - 24, 20, nm[i], i == o, 9)) u.wtOsc = i, u.wtFrame = 0;
    ImGui::PopID();
  }
  if (button(c, "close", 12, kPageY + 100, kLeftW - 24, 24, "CLOSE EDITOR", false, 8)) u.wtEdit = false;
  if (ImGui::IsItemHovered()) c.a.setHint("Back to the oscillator page (the table stays edited)");
  {
    const float by = kWaveY + 8;
    textC(c, kLeftW * 0.5f, by, "BRUSH", c.P.label, ty::kCap, true);
    for (int b = 0; b < BrCount; ++b) {
      const float bx = 14 + (b % 2) * 62, yy = by + 18 + (b / 2) * 34;
      ImGui::PushID(b + 9200);
      ImGui::SetCursorScreenPos(c.p(bx, yy));
      if (ImGui::InvisibleButton("##b", c.F.sz(58, 30))) u.wtBrush = b;
      const bool on = u.wtBrush == b, hv = ImGui::IsItemHovered();
      panelRect(c, bx, yy, 58, 30, on ? c.P.headOn : (hv ? c.P.panelHi : c.P.panel), 3);
      if (on) c.dl->AddRect(c.p(bx, yy), c.p(bx + 58, yy + 30), theme::withAlpha(c.P.accent, 0.45f), c.s(3));
      brushIcon(c, b, bx + 12, yy + 6, 34, 18, on ? c.P.accent : c.P.textDim);
      if (hv) c.a.setHint(std::string("Brush: ") + kBrushName[b] + (b >= BrFlat ? " (fills each grid cell the pointer passes)" : ""));
      ImGui::PopID();
    }
  }

  // ================================================================ top: harmonics (magnitude + phase)
  {
    const float x0 = kLeftW + 4, x1 = kW - 82, w = x1 - x0;
    displayWellPublic(c, x0, kHarmY + 2, w, kMagH);
    displayWellPublic(c, x0, kHarmY + kMagH + 6, w, kHarmH - kMagH - 10);
    const int nh = std::clamp(32 << std::clamp(u.wtZoom, 0, 3), 32, kHarm);
    const Harm& H = harmOf(cur);
    float peak = 1e-6f;
    for (int i = 1; i <= nh; ++i) peak = std::max(peak, H.mag(i));
    const float bw = w / nh;
    const float my0 = kHarmY + 4, mh = kMagH - 4, py0 = kHarmY + kMagH + 8, ph = kHarmH - kMagH - 14;
    for (int i = 1; i <= nh; ++i) {
      const float bx = x0 + (i - 1) * bw;
      const float v = std::sqrt(std::min(1.f, H.mag(i) / std::max(1.f, peak)));
      if (v > 0.003f)
        skin::gradientRect(c.dl, c.p(bx + 0.5f, my0 + mh - v * mh), c.p(bx + bw - 0.5f, my0 + mh), skin::lighten(c.P.accent, 0.15f), c.P.accentDim, 0);
      // J-202: a phase reads as a short mark at its height (no bars: they read as noise)
      if (H.mag(i) > 1e-4f) {
        const float pv = phaseOf(H, i) / 6.2831853f;
        const float yy = py0 + ph - pv * ph;
        c.dl->AddRectFilled(c.p(bx + 1, yy - 1), c.p(bx + bw - 1, yy + 1), theme::withAlpha(c.P.mod, 0.85f), c.s(0.5f));
      }
    }
    // magnitudes: drag sets, right-drag clears
    ImGui::SetCursorScreenPos(c.p(x0, my0));
    ImGui::InvisibleButton("##mag", c.F.sz(w, mh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int hi = std::clamp(1 + static_cast<int>((m.x - c.p(x0, 0).x) / c.s(bw)), 1, nh);
    if (ImGui::IsItemActive()) {
      const float hv = std::clamp((c.p(0, my0 + mh).y - m.y) / c.s(mh), 0.f, 1.f);
      setHarm(cur, H, hi, ImGui::IsMouseDown(1) ? 0.f : hv * hv * std::max(1.f, peak));
      pending = true;
    }
    if (ImGui::IsItemHovered()) {
      char b[48];
      std::snprintf(b, sizeof b, "#%d  %.1f dB", hi, 20.0 * std::log10(std::max(1e-6, static_cast<double>(H.mag(hi)))));
      MonoText mono;
      textR(c, x1 - 6, my0 + 3, b, c.P.textDim, ty::kCap, false);
      c.a.setHint("Harmonics: drag to set levels (phases kept) \xC2\xB7 right-drag: remove");
    }
    ImGui::SetCursorScreenPos(c.p(x0, py0));
    ImGui::InvisibleButton("##phs", c.F.sz(w, ph));
    if (ImGui::IsItemActive()) {
      const int pi = std::clamp(1 + static_cast<int>((m.x - c.p(x0, 0).x) / c.s(bw)), 1, nh);
      const float pv = std::clamp((c.p(0, py0 + ph).y - m.y) / c.s(ph), 0.f, 1.f);
      if (H.mag(pi) > 1e-4f) setPhase(cur, H, pi, pv * 6.2831853f), pending = true;
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Phases: drag to set the phase of each harmonic");
    // zoom
    const float zx = x1 + 6;
    textC(c, zx + 35, kHarmY + 10, "ZOOM", c.P.label, ty::kCap, true);
    int z = u.wtZoom;
    const std::string zt = std::to_string(32 << z);
    if (stepperFree(c, "zoom", z, 0, 3, zx + 4, kHarmY + 24, 62, 20, zt.c_str())) u.wtZoom = z;
    if (ImGui::IsItemHovered()) c.a.setHint("Harmonics shown");
    textC(c, zx + 35, kHarmY + 56, "HARMONICS", c.P.label, ty::kCap, true);
    textC(c, zx + 35, py0 + ph * 0.5f - 5, "PHASE", c.P.label, ty::kCap, true);
  }

  // ================================================================ centre: waveform
  {
    const float x0 = kLeftW + 4, w = kRightX - x0 - 2, y0 = kWaveY, h = kWaveH - 2;
    displayWellPublic(c, x0, y0, w, h);
    const int gx = std::max(1, u.wtGrid), gy = std::max(1, u.wtGridY);
    for (int i = 1; i < gx; ++i) vline(c, x0 + w * i / gx, y0, y0 + h, c.P.grid);
    for (int i = 1; i < gy; ++i) hline(c, x0, x0 + w, y0 + h * i / gy, c.P.grid);
    hline(c, x0, x0 + w, y0 + h * 0.5f, c.P.gridStrong);
    if (u.wtFrame > 0) wave(c, F[static_cast<size_t>(u.wtFrame - 1)], x0, y0, w, h, theme::withAlpha(c.P.textFaint, 0.45f), std::max(1.f, c.s(1)), false);
    wave(c, cur, x0, y0, w, h, c.P.accent, std::max(1.5f, c.s(2)), true);
    ImGui::SetCursorScreenPos(c.p(x0, y0));
    ImGui::InvisibleButton("##wave", c.F.sz(w, h));
    const ImVec2 m = ImGui::GetIO().MousePos, p0 = c.p(x0, y0);
    const float fx = std::clamp((m.x - p0.x) / c.s(w), 0.f, 0.99999f);
    float fy = std::clamp(((p0.y + c.s(h) * 0.5f) - m.y) / (c.s(h) * 0.46f), -1.f, 1.f);
    if (u.wtGridY > 1) fy = std::round(fy * u.wtGridY * 0.5f) / (u.wtGridY * 0.5f);
    static float lastX = -1, lastY = 0, lineX = 0, lineY = 0;
    static uint32_t rng = 1;
    const size_t n = cur.size();
    auto fill = [&](float xa, float ya, float xb, float yb) {
      if (xa > xb) std::swap(xa, xb), std::swap(ya, yb);
      const size_t a = static_cast<size_t>(xa * static_cast<float>(n)), b = std::min(n, static_cast<size_t>(xb * static_cast<float>(n)) + 1);
      for (size_t s = a; s < b; ++s) {
        const float t = xb > xa ? (static_cast<float>(s) / static_cast<float>(n) - xa) / (xb - xa) : 0.f;
        cur[s] = ya + (yb - ya) * std::clamp(t, 0.f, 1.f);
      }
    };
    const int cells = u.wtGrid > 1 ? u.wtGrid : 8;
    auto cellFill = [&](int cell, float amp) {
      const size_t a = n * static_cast<size_t>(cell) / static_cast<size_t>(cells), b = n * static_cast<size_t>(cell + 1) / static_cast<size_t>(cells);
      for (size_t s = a; s < b; ++s) cur[s] = amp * shapeAt(u.wtBrush, static_cast<float>(s - a) / static_cast<float>(b - a), rng);
    };
    if (ImGui::IsItemActivated()) lastX = fx, lastY = fy, lineX = fx, lineY = fy;
    if (ImGui::IsItemActive()) {
      if (u.wtBrush == BrFree) {
        if (u.wtGrid > 1) {
          const float c0 = std::floor(fx * u.wtGrid) / u.wtGrid;
          fill(c0, fy, c0 + 1.f / u.wtGrid - 1e-6f, fy);
        } else {
          fill(lastX, lastY, fx, fy);
        }
        lastX = fx, lastY = fy;
        pending = true;
      } else if (u.wtBrush == BrLine) {
        c.dl->AddLine(c.p(x0 + w * lineX, y0 + h * 0.5f - lineY * h * 0.46f), c.p(x0 + w * fx, y0 + h * 0.5f - fy * h * 0.46f), c.P.mod, c.s(1.5f));
      } else {
        // every cell between the last and the current pointer position
        const int ca = static_cast<int>(std::min(lastX, fx) * cells), cb = static_cast<int>(std::max(lastX, fx) * cells);
        for (int k = ca; k <= cb && k < cells; ++k) cellFill(k, fy);
        lastX = fx;
        pending = true;
      }
    }
    if (ImGui::IsItemDeactivated() && u.wtBrush == BrLine) fill(lineX, lineY, fx, fy), pending = true;
    if (ImGui::IsItemHovered()) {
      char b[48];
      std::snprintf(b, sizeof b, "%.0f%%   %+.2f", static_cast<double>(fx) * 100.0, static_cast<double>(fy));
      textR(c, x0 + w - 8, y0 + 6, b, c.P.textDim, 8, true);
      c.a.setHint("Draw with the brush \xC2\xB7 \xE2\x86\x90 \xE2\x86\x92 change frame");
    }
  }

  // ================================================================ right: table, import / export, process, grid
  {
    const float x = kRightX + 10, w = kW - kRightX - 20;
    frameBox(c, kRightX, kWaveY - 2, kW - kRightX, kWaveH + 2, c.P.panelLo);
    float y = kWaveY + 8;
    const auto& names = factoryTableNames();
    int step = 0;
    int pick = -1;
    dropField(c, "tbl", x, y, w, 20, shownName.c_str(), [&] {
      if (custom) {
        char nb[64];
        std::snprintf(nb, sizeof nb, "%s", d.tableName.c_str());
        ImGui::SetNextItemWidth(c.s(160));
        if (ImGui::InputText(i18n::tr("Name"), nb, sizeof nb, ImGuiInputTextFlags_EnterReturnsTrue)) d.tableName = nb, commit(c, "Rename table");
        ImGui::Separator();
      }
      for (int i = 0; i < static_cast<int>(names.size()); ++i)
        if (ImGui::MenuItem(names[static_cast<size_t>(i)].c_str(), nullptr, !custom && names[static_cast<size_t>(i)] == shownName)) pick = i;
    }, true, &step);
    if (step) {
      int idx = 0;
      for (int i = 0; i < static_cast<int>(names.size()); ++i)
        if (names[static_cast<size_t>(i)] == shownName) idx = i;
      pick = (idx + step + static_cast<int>(names.size())) % static_cast<int>(names.size());
    }
    if (pick >= 0) {
      selectFactory(c, o, pick);
      return;
    }
    y += 30;
    if (button(c, "imp", x, y, w * 0.5f - 2, 24, "IMPORT", false, 8))
      openFile(c, i18n::tr("Load wavetable"), {".wav"}, [o](App& a, const PluginTarget& t, const std::string& p) {
        auto tb = loadTableFile(p);
        if (!tb) {
          a.notify("Could not read the wavetable", true);
          return;
        }
        editState(a, t, "Load wavetable", [&](PluginState& st, Model& m) {
          auto& od = m.osc[static_cast<size_t>(o)];
          od.table = "custom";
          od.tableName = fs_stem(p);
          od.frames.clear();
          for (int i = 0; i < std::min(tb->frames, kMaxFrames); ++i) od.frames.emplace_back(tb->frame(i), tb->frame(i) + kFrameSize);
          st.params[static_cast<size_t>(oscP(o, O_Table))] = 0;
        });
      });
    if (ImGui::IsItemHovered()) c.a.setHint("Load a WAV as frames (2048 samples each, or the cycle length the file states)");
    if (button(c, "exp", x + w * 0.5f + 2, y, w * 0.5f - 2, 24, "EXPORT", false, 8)) {
      App& a = c.a;
      Frames copy = F;
      a.fileDialog = {};
      a.fileDialog.open = true;
      a.fileDialog.save = true;
      a.fileDialog.title = i18n::tr("Export wavetable");
      const char* hm = std::getenv("HOME");
      a.fileDialog.dir = hm ? hm : ".";
      a.fileDialog.name = (d.tableName.empty() ? std::string("table") : d.tableName) + ".wav";
      a.fileDialog.exts = {".wav"};
      a.fileDialog.onOk = [&a, copy](const std::string& p) {
        AudioBuffer b(1, copy.size() * static_cast<size_t>(kFrameSize));
        for (size_t i = 0; i < copy.size(); ++i) std::copy(copy[i].begin(), copy[i].end(), b.plane(0) + i * static_cast<size_t>(kFrameSize));
        a.notify(writeWav(p, b, 44100, 24) ? "Exported the wavetable" : "Could not write the file", false);
      };
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Save the table as a WAV of 2048-sample frames");
    y += 36;
    textC(c, x + w * 0.5f, y, "PROCESS", c.P.label, ty::kCap, true);
    y += 16;
    if (button(c, "single", x, y, w, 22, "SINGLE", false, 8)) ImGui::OpenPopup("##wtsingle");
    iconArrow(c.dl, c.p(x + w - 12, y + 11), c.s(7), 3, c.P.textDim);
    if (ImGui::BeginPopup("##wtsingle")) {
      next = F;
      auto& fr = next[static_cast<size_t>(u.wtFrame)];
      if (ImGui::MenuItem(i18n::tr("Normalize"))) normalize(fr), did = "Normalize";
      if (ImGui::MenuItem(i18n::tr("Remove DC offset"))) removeDc(fr), did = "Remove DC";
      if (ImGui::MenuItem(i18n::tr("Reverse"))) std::reverse(fr.begin(), fr.end()), did = "Reverse";
      if (ImGui::MenuItem(i18n::tr("Invert"))) {
        for (float& v : fr) v = -v;
        did = "Invert";
      }
      if (ImGui::MenuItem(i18n::tr("Mirror (second half reversed)"))) {
        for (size_t i = 0; i < fr.size() / 2; ++i) fr[fr.size() - 1 - i] = -fr[i];
        did = "Mirror";
      }
      if (ImGui::BeginMenu(i18n::tr("Smooth (keep harmonics)"))) {
        for (int k : {4, 8, 16, 32, 64, 128})
          if (ImGui::MenuItem(("1.." + std::to_string(k)).c_str())) bandLimit(fr, k), did = "Smooth";
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(i18n::tr("Clear (sine)"))) {
        for (size_t i = 0; i < fr.size(); ++i) fr[i] = static_cast<float>(std::sin(kTau * static_cast<double>(i) / static_cast<double>(fr.size())));
        did = "Clear frame";
      }
      ImGui::EndPopup();
    }
    y += 26;
    if (button(c, "all", x, y, w, 22, "ALL", false, 8)) ImGui::OpenPopup("##wtall");
    iconArrow(c.dl, c.p(x + w - 12, y + 11), c.s(7), 3, c.P.textDim);
    if (ImGui::BeginPopup("##wtall")) {
      next = F;
      if (ImGui::MenuItem(i18n::tr("Normalize (each frame)"))) {
        for (auto& fr : next) normalize(fr);
        did = "Normalize all";
      }
      if (ImGui::MenuItem(i18n::tr("Normalize (whole table)"))) {
        float p = 0;
        for (auto& fr : next)
          for (float v : fr) p = std::max(p, std::fabs(v));
        if (p > 1e-6f)
          for (auto& fr : next)
            for (float& v : fr) v /= p;
        did = "Normalize table";
      }
      if (ImGui::MenuItem(i18n::tr("Remove DC offset"))) {
        for (auto& fr : next) removeDc(fr);
        did = "Remove DC";
      }
      if (ImGui::MenuItem(i18n::tr("Invert"))) {
        for (auto& fr : next)
          for (float& v : fr) v = -v;
        did = "Invert all";
      }
      if (ImGui::MenuItem(i18n::tr("Reverse frame order"))) std::reverse(next.begin(), next.end()), did = "Reverse order";
      if (ImGui::BeginMenu(i18n::tr("Resize"))) {
        for (int k : {1, 2, 4, 8, 16, 32, 64, 128, 256})
          if (ImGui::MenuItem(std::to_string(k).c_str(), nullptr, nf == k)) next = resizeTable(F, k), did = "Resize table";
        ImGui::EndMenu();
      }
      ImGui::EndPopup();
    }
    y += 26;
    if (button(c, "morph", x, y, w, 22, "MORPH", false, 8)) ImGui::OpenPopup("##wtmorph");
    iconArrow(c.dl, c.p(x + w - 12, y + 11), c.s(7), 3, c.P.textDim);
    if (ImGui::IsItemHovered()) c.a.setHint("Fill the table from the first to the last frame");
    if (ImGui::BeginPopup("##wtmorph")) {
      for (int sp = 0; sp < 2; ++sp)
        if (ImGui::BeginMenu(sp ? i18n::tr("Spectral") : i18n::tr("Crossfade"))) {
          for (int k : {8, 16, 32, 64, 128, 256})
            if (ImGui::MenuItem((std::to_string(k) + " frames").c_str())) next = morphTable(F, k, sp == 1), did = "Morph";
          ImGui::EndMenu();
        }
      ImGui::EndPopup();
    }
    y += 36;
    textC(c, x + w * 0.5f, y, "GRID", c.P.label, ty::kCap, true);
    y += 14;
    static const int gridSteps[] = {0, 2, 4, 8, 16, 32, 64};
    auto gridBox = [&](const char* id, int& val, float bx, const char* icon) {
      int gi = 0;
      for (int i = 0; i < 7; ++i)
        if (gridSteps[i] == val) gi = i;
      textC(c, bx + w * 0.25f, y, icon, c.P.label, ty::kCap, true);
      if (stepperFree(c, id, gi, 0, 6, bx, y + 12, w * 0.5f - 4, 20, gi ? std::to_string(gridSteps[gi]).c_str() : "Off")) val = gridSteps[gi];
    };
    gridBox("gx", u.wtGrid, x, "X");
    gridBox("gy", u.wtGridY, x + w * 0.5f + 2, "Y");
    y += 38;
    // shift the frame by one grid step
    const char* tips[] = {"Shift left", "Shift right", "Shift up", "Shift down"};
    const int dirs[] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
      const float bx = x + i * (w / 4);
      ImGui::PushID(i + 9300);
      if (button(c, "sh", bx, y, w / 4 - 3, 20, "", false, 8)) {
        next = F;
        auto& fr = next[static_cast<size_t>(u.wtFrame)];
        const int gxs = std::max(2, u.wtGrid > 1 ? u.wtGrid : 16);
        const float dy = 2.f / std::max(2, u.wtGridY > 1 ? u.wtGridY : 16);
        const long sh = static_cast<long>(fr.size()) / gxs;
        if (i == 0) std::rotate(fr.begin(), fr.begin() + sh, fr.end());
        if (i == 1) std::rotate(fr.begin(), fr.end() - sh, fr.end());
        if (i == 2)
          for (float& v : fr) v = std::min(2.f, v + dy);
        if (i == 3)
          for (float& v : fr) v = std::max(-2.f, v - dy);
        did = tips[i];
      }
      if (ImGui::IsItemHovered()) c.a.setHint(std::string(tips[i]) + " (one grid step)");
      iconArrow(c.dl, c.p(bx + (w / 4 - 3) * 0.5f, y + 10), c.s(8), dirs[i], c.P.textDim);
      ImGui::PopID();
    }
  }

  // ================================================================ frames strip
  {
    const float x0 = 0, x1 = kW, w = 50;
    frameBox(c, x0, kFramesY, x1 - x0, kFramesH, c.P.panelLo);
    const int vis = static_cast<int>((x1 - x0 - 50) / w);
    int& sc = u.wtScroll;
    if (u.wtFrame < sc) sc = u.wtFrame;
    if (u.wtFrame >= sc + vis) sc = u.wtFrame - vis + 1;
    sc = std::clamp(sc, 0, std::max(0, nf - vis));
    ImGui::SetCursorScreenPos(c.p(x0, kFramesY + 2));
    ImGui::InvisibleButton("##strip", c.F.sz(std::min(x1 - x0, 4 + (std::min(nf, vis)) * w), kFramesH - 4));
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) sc = std::clamp(sc - static_cast<int>(ImGui::GetIO().MouseWheel * 4), 0, std::max(0, nf - vis));
    if (ImGui::IsItemActive()) {
      const float mx = (ImGui::GetIO().MousePos.x - c.p(x0 + 6, 0).x) / c.s(w);
      u.wtFrame = std::clamp(sc + static_cast<int>(mx), 0, nf - 1);
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Frames: click or drag to select \xC2\xB7 wheel: scroll");
    for (int i = sc; i < std::min(nf, sc + vis); ++i) {
      const float tx = x0 + 6 + (i - sc) * w;
      const bool sel = i == u.wtFrame;
      panelRect(c, tx, kFramesY + 4, w - 3, kFramesH - 22, sel ? c.P.headOn : c.P.well, 2);
      if (sel) c.dl->AddRect(c.p(tx, kFramesY + 4), c.p(tx + w - 3, kFramesY + kFramesH - 18), theme::withAlpha(c.P.accent, 0.6f), c.s(2));
      wave(c, F[static_cast<size_t>(i)], tx + 2, kFramesY + 6, w - 7, kFramesH - 26, sel ? c.P.accent : c.P.textDim, std::max(1.f, c.s(1.2f)), sel);
      MonoText mono;
      textC(c, tx + (w - 3) * 0.5f, kFramesY + kFramesH - 14, std::to_string(i + 1).c_str(), sel ? c.P.text : c.P.textDim, ty::kCap, false);
    }
    if (nf > vis) {  // J-202: a scroll bar shows there are more frames (wheel scrolls)
      const float tx0 = x0 + 6, tw = vis * w - 3, ty = kFramesY + kFramesH - 4;
      c.dl->AddRectFilled(c.p(tx0, ty), c.p(tx0 + tw, ty + 2), c.P.grid, c.s(1));
      const float th = tw * vis / nf, tp = tx0 + (tw - th) * sc / std::max(1, nf - vis);
      c.dl->AddRectFilled(c.p(tp, ty), c.p(tp + th, ty + 2), c.P.lineHi, c.s(1));
    }
    const float px = x0 + 6 + std::min(nf - sc, vis) * w;
    if (nf < kMaxFrames && sc + vis >= nf && px + 40 < x1) {
      if (button(c, "addf", px + 4, kFramesY + 12, 34, 30, GL_ICON_LABEL("plus", "+"), false, 13)) {
        next = F;
        next.push_back(F.back());
        did = "Add frame";
        u.wtFrame = nf;
      }
      if (ImGui::IsItemHovered()) c.a.setHint("Add a frame (a copy of the last)");
    }
  }

  // ================================================================ bottom bar: position, clipboard, formula
  {
    const float y = kBarY + 8;
    frameBox(c, 0, kBarY, kW, kStripY - kBarY);
    int pos = u.wtFrame + 1;
    if (stepperFree(c, "pos", pos, 1, nf, 14, y, 48, 22)) u.wtFrame = pos - 1;
    char t[16];
    std::snprintf(t, sizeof t, "/ %d", nf);
    text(c, 68, y + 6, t, c.P.text, 9.5f, true);
    if (button(c, "minus", 104, y, 22, 22, GL_ICON_LABEL("minus", "\xE2\x80\x93"), false, 11) && nf > 1) {
      next = F;
      next.erase(next.begin() + u.wtFrame);
      did = "Remove frame";
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Remove this frame");
    if (button(c, "plus", 128, y, 22, 22, GL_ICON_LABEL("plus", "+"), false, 11) && nf < kMaxFrames) {
      next = F;
      next.insert(next.begin() + u.wtFrame + 1, cur);
      did = "Duplicate frame";
    }
    if (ImGui::IsItemHovered()) c.a.setHint("Duplicate this frame");
    textC(c, 82, y + 30, "WT POS", c.P.textDim, 8, true);
    vline(c, 166, y - 2, y + 40, c.P.line);
    struct B {
      const char* id;
      const char* label;
    };
    const B bs[] = {{"copy", "COPY"}, {"paste", "PASTE"}, {"remove", "REMOVE\xE2\x80\xA6"}, {"sort", "SORT\xE2\x80\xA6"}};
    for (int i = 0; i < 4; ++i) {
      const float bx = 182 + i * 80;
      if (button(c, bs[i].id, bx, y, 62, 22, "", false, 8)) {
        if (i == 0) clipFrame() = cur;
        if (i == 1 && clipFrame().size() == cur.size()) next = F, next[static_cast<size_t>(u.wtFrame)] = clipFrame(), did = "Paste frame";
        if (i == 2) ImGui::OpenPopup("##wtremove");
        if (i == 3) ImGui::OpenPopup("##wtsort");
      }
      textC(c, bx + 31, y + 30, bs[i].label, c.P.textDim, 8, true);
      // little glyphs: two sheets / arrow-in / cross / swap
      const ImVec2 q = c.p(bx + 31, y + 11);
      const float s = c.s(5);
      const ImU32 gc = c.P.textDim;
      if (i == 0) ::gl::ui::drawIconFit(c.dl, ::gl::ui::Icon::Clone, ImVec2(q.x - s - 0.5f, q.y - s - 0.5f), ImVec2(q.x + s + 0.5f, q.y + s * 0.8f + 0.5f), gc);
      if (i == 1) c.dl->AddRect(ImVec2(q.x - s * 0.2f, q.y - s), ImVec2(q.x + s, q.y + s), gc), iconArrow(c.dl, ImVec2(q.x - s * 0.7f, q.y), s * 1.4f, 1, gc);
      if (i == 2) c.dl->AddRect(ImVec2(q.x - s, q.y - s * 0.8f), ImVec2(q.x + s * 0.4f, q.y + s * 0.8f), gc), iconClose(c.dl, ImVec2(q.x + s * 0.6f, q.y + s * 0.5f), s * 0.9f, gc);
      if (i == 3) c.dl->AddRect(ImVec2(q.x - s * 1.4f, q.y - s * 0.5f), ImVec2(q.x - s * 0.4f, q.y + s * 0.5f), gc), c.dl->AddRect(ImVec2(q.x + s * 0.4f, q.y - s * 0.5f), ImVec2(q.x + s * 1.4f, q.y + s * 0.5f), gc);
    }
    if (ImGui::BeginPopup("##wtremove")) {
      next = F;
      if (ImGui::MenuItem(i18n::tr("Remove this frame"), nullptr, false, nf > 1)) next.erase(next.begin() + u.wtFrame), did = "Remove frame";
      if (ImGui::MenuItem(i18n::tr("Remove the frames after this one"), nullptr, false, u.wtFrame + 1 < nf))
        next.resize(static_cast<size_t>(u.wtFrame) + 1), did = "Remove frames";
      if (ImGui::MenuItem(i18n::tr("Remove the frames before this one"), nullptr, false, u.wtFrame > 0))
        next.erase(next.begin(), next.begin() + u.wtFrame), did = "Remove frames", u.wtFrame = 0;
      if (ImGui::MenuItem(i18n::tr("Keep every second frame"), nullptr, false, nf > 2)) {
        Frames k;
        for (int i = 0; i < nf; i += 2) k.push_back(F[static_cast<size_t>(i)]);
        next = k, did = "Remove frames";
      }
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##wtsort")) {
      next = F;
      if (ImGui::MenuItem(i18n::tr("By brightness"))) {
        std::stable_sort(next.begin(), next.end(), [](const std::vector<float>& p, const std::vector<float>& q) { return brightness(p) < brightness(q); });
        did = "Sort frames";
      }
      if (ImGui::MenuItem(i18n::tr("By level"))) {
        std::stable_sort(next.begin(), next.end(), [](const std::vector<float>& p, const std::vector<float>& q) { return rms(p) < rms(q); });
        did = "Sort frames";
      }
      if (ImGui::MenuItem(i18n::tr("Reverse order"))) std::reverse(next.begin(), next.end()), did = "Reverse order";
      ImGui::EndPopup();
    }
    // formula
    vline(c, 506, y - 2, y + 40, c.P.line);
    const float fx = 520, fw = kW - fx - 12;
    char fb[256];
    std::snprintf(fb, sizeof fb, "%s", u.wtFormula.c_str());
    const bool enter = inputField(c, "wtf", fx, y, fw - 26, 22, fb, sizeof fb, "(enter formula)", ImGuiInputTextFlags_EnterReturnsTrue);
    u.wtFormula = fb;
    if (ImGui::IsItemHovered())
      c.a.setHint("Formula (Enter: apply to every frame): x = -1..1 across the cycle, y = 0..1 across the table, in = the current sample");
    if (button(c, "fmenu", fx + fw - 24, y, 24, 22, "", false, 8)) ImGui::OpenPopup("##wtfmenu");
    iconArrow(c.dl, c.p(fx + fw - 12, y + 11), c.s(8), 3, c.P.textDim);
    std::string run;
    bool all = true;
    if (ImGui::BeginPopup("##wtfmenu")) {
      for (const auto& f : kFormulas)
        if (ImGui::MenuItem(f[0], f[1])) u.wtFormula = f[1], run = f[1];
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Apply to this frame only"), nullptr, false, !u.wtFormula.empty())) run = u.wtFormula, all = false;
      ImGui::EndPopup();
    }
    if (enter && !u.wtFormula.empty()) run = u.wtFormula;
    if (!run.empty()) {
      next = F;
      std::string err;
      if (applyFormula(next, run, all, u.wtFrame, err)) did = "Formula", u.wtFormulaErr.clear();
      else u.wtFormulaErr = err, next.clear();
    }
    textC(c, fx + fw * 0.5f, y + 30, u.wtFormulaErr.empty() ? "FORMULA" : u.wtFormulaErr.c_str(), u.wtFormulaErr.empty() ? c.P.textDim : c.P.danger, 8,
          true);
  }

  // ================================================================ commit
  if (did && !next.empty()) {
    u.wtFrame = std::clamp(u.wtFrame, 0, static_cast<int>(next.size()) - 1);
    storeFrames(c, o, std::move(next), did);
    pending = false;
    return;
  }
  if (pending && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1)) {
    pending = false;
    if (custom) commit(c, "Draw wavetable");
    else storeFrames(c, o, F, "Draw wavetable");  // the first edit makes the custom table
  }
  if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) u.wtFrame = std::max(0, u.wtFrame - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) u.wtFrame = std::min(nf - 1, u.wtFrame + 1);
  }
}

}  // namespace gl::packs::plasma::ui
