// Gridline — "plasma" pack: the table editor (an overlay over the stage and
// the dock). Frame list, a drawable waveform (freehand / line / smooth
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

// layout: a tool bar across the top (layer, drawing tools, close); the frame
// list down the left side; the waveform, then the partials under it, then the
// frame / clipboard / formula line; table, files, processing and grid in the
// right column.
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

// Saves frames as a WAV of 2048-sample frames (a save dialog).
void exportFrames(Ctx& c, const Frames& F, const std::string& name) {
  App& a = c.a;
  Frames copy = F;
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.save = true;
  a.fileDialog.title = i18n::tr("Export wavetable");
  const char* hm = std::getenv("HOME");
  a.fileDialog.dir = hm ? hm : ".";
  a.fileDialog.name = (name.empty() ? std::string("table") : name) + ".wav";
  a.fileDialog.exts = {".wav"};
  a.fileDialog.onOk = [&a, copy](const std::string& p) {
    AudioBuffer b(1, copy.size() * static_cast<size_t>(kFrameSize));
    for (size_t i = 0; i < copy.size(); ++i) std::copy(copy[i].begin(), copy[i].end(), b.plane(0) + i * static_cast<size_t>(kFrameSize));
    a.notify(writeWav(p, b, 44100, 24) ? "Exported the wavetable" : "Could not write the file", false);
  };
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
  const Pal& P = c.P;
  const int o = std::clamp(u.wtOsc, 0, kOscs - 1);
  panelRect(c, 0, kTopH, kW, kPlayY - kTopH, P.frame);
  OscData& d = c.m.osc[static_cast<size_t>(o)];
  // A factory or file table is shown from a copy; the first edit stores it as the
  // oscillator's custom table (opening the editor alone changes nothing).
  const bool custom = d.table == "custom" && !d.frames.empty();
  Frames& view = u.wtView;
  if (!custom && !u.wtPending) {  // an undo or redo brings the stored table back into the copy
    const std::string key = std::to_string(o) + "|" + d.table + "|" + std::to_string(vali(c, oscP(o, O_Table))) + "|" +
                            std::to_string(c.a.doc.history().stateSerial());
    if (key != u.wtViewKey || view.empty()) view = currentFrames(c, o), u.wtViewKey = key;
  }
  Frames& F = custom ? d.frames : view;
  std::string shownName = d.tableName;
  if (!custom && d.table.empty()) {
    const auto& fn = factoryTableNames();
    const int ti = std::max(1, vali(c, oscP(o, O_Table))) - 1;
    if (ti < static_cast<int>(fn.size())) shownName = fn[static_cast<size_t>(ti)];
  }
  if (shownName.empty()) shownName = "Custom";
  const int nf = static_cast<int>(F.size());
  u.wtFrame = std::clamp(u.wtFrame, 0, nf - 1);
  auto& cur = F[static_cast<size_t>(u.wtFrame)];
  bool& pending = u.wtPending;  // edited in place: committed when the gesture ends
  const char* did = nullptr;    // a table operation (stored at the end of the frame)
  Frames next;
  const ImU32 col = partCol(c, o);
  KnobColour kc(col);

  // ================================================================ head: the layer, its table, the three layers
  {
    char title[16], sub[96];
    std::snprintf(title, sizeof title, "Layer %d", o + 1);
    std::snprintf(sub, sizeof sub, "%s, %d frame%s", shownName.c_str(), nf, nf == 1 ? "" : "s");
    u.back = VLayer;
    advHead(c, title, sub, col, -1);
    const float w = M(194), x = kAdvIX1 - w, y = kAdvHeadY - M(14);
    const int hit = segPick(c, "layers", x, y, w, M(28), {"Layer 1", "Layer 2", "Layer 3"}, o, false, T(12));
    if (hit >= 0 && hit != o) u.wtOsc = hit, u.wtFrame = 0;
    textR(c, x - M(14), kAdvHeadY - T(11.5f) * 0.68f, "Esc or Back to return", P.textFaint, T(11.5f), false);
  }
  const float lx = kAdvIX0, lr = M(1012), lw = lr - lx, rx = M(1030), rw = kAdvIX1 - rx;
  const float ty = kAdvBodyY, th = M(28);
  // ================================================================ tool row: the view, the frame, the clipboard
  {
    const int hit = segPick(c, "view", lx, ty, M(152), th, {"Edit frame", "3D view"}, u.wt3dEdit ? 1 : 0, false, T(12));
    if (hit >= 0) u.wt3dEdit = hit == 1;
    float x = lx + M(152) + M(16);
    text(c, x, ty + th * 0.5f - T(12) * 0.68f, "Frame", P.textFaint, T(12), false);
    x += textW(c, "Frame", T(12), false) + M(10);
    {
      char b[24];
      std::snprintf(b, sizeof b, "%d", u.wtFrame + 1);
      char of[16];
      std::snprintf(of, sizeof of, "of %d", nf);
      MonoText mono;
      const float fw = textW(c, b, T(12), false) + textW(c, of, T(11), false) + M(28);
      c.dl->AddRectFilled(c.p(x, ty), c.p(x + fw, ty + th), P.field, c.s(M(4)));
      c.dl->AddRect(c.p(x, ty), c.p(x + fw, ty + th), P.line, c.s(M(4)));
      text(c, x + M(10), ty + th * 0.5f - T(12) * 0.68f, b, P.text, T(12), false);
      text(c, x + M(18) + textW(c, b, T(12), false), ty + th * 0.5f - T(11) * 0.68f, of, P.label, T(11), false);
      ImGui::SetCursorScreenPos(c.p(x, ty));
      ImGui::InvisibleButton("##framedrag", c.F.sz(fw, th));
      if (ImGui::IsItemHovered()) c.a.setHint("Drag up or down to pick a frame (arrow keys too)"), ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      static float acc = 0;
      if (ImGui::IsItemActive()) {
        acc -= ImGui::GetIO().MouseDelta.y / 6.f;
        const int st = static_cast<int>(acc);
        if (st) u.wtFrame = std::clamp(u.wtFrame + st, 0, nf - 1), acc -= st;
      } else {
        acc = 0;
      }
      x += fw + M(10);
    }
    for (int i = 0; i < 2; ++i) {
      ImGui::PushID(i + 9400);
      if (btnLook(c, "step", x, ty, M(30), th, "", false, false, T(12))) u.wtFrame = std::clamp(u.wtFrame + (i ? 1 : -1), 0, nf - 1);
      if (ImGui::IsItemHovered()) c.a.setHint(i ? "The next frame" : "The frame before");
      const ImVec2 m = c.p(x + M(15), ty + th * 0.5f);
      const float a = c.s(M(3)), sg = i ? -1.f : 1.f;
      c.dl->AddLine(ImVec2(m.x + a * 0.5f * sg, m.y - a), ImVec2(m.x - a * 0.5f * sg, m.y), P.label, std::max(1.f, c.s(M(1.3f))));
      c.dl->AddLine(ImVec2(m.x - a * 0.5f * sg, m.y), ImVec2(m.x + a * 0.5f * sg, m.y + a), P.label, std::max(1.f, c.s(M(1.3f))));
      x += M(30) + M(10);
      ImGui::PopID();
    }
    // the right end: Copy, Paste, Add frame, more
    float r = lr;
    {
      r -= th;
      if (btnLook(c, "framemore", r, ty, th, th, "...", false, false, T(12))) ImGui::OpenPopup("##wtframes");
      if (ImGui::IsItemHovered()) c.a.setHint("Remove frames, put them in order, duplicate this one");
      if (ImGui::BeginPopup("##wtframes")) {
        next = F;
        if (ImGui::MenuItem(i18n::tr("Duplicate this frame"), nullptr, false, nf < kMaxFrames))
          next.insert(next.begin() + u.wtFrame + 1, cur), did = "Duplicate frame";
        ImGui::Separator();
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
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Order by brightness"))) {
          std::stable_sort(next.begin(), next.end(), [](const std::vector<float>& p, const std::vector<float>& q) { return brightness(p) < brightness(q); });
          did = "Sort frames";
        }
        if (ImGui::MenuItem(i18n::tr("Order by level"))) {
          std::stable_sort(next.begin(), next.end(), [](const std::vector<float>& p, const std::vector<float>& q) { return rms(p) < rms(q); });
          did = "Sort frames";
        }
        if (ImGui::MenuItem(i18n::tr("Reverse the order"))) std::reverse(next.begin(), next.end()), did = "Reverse order";
        ImGui::EndPopup();
      }
      r -= M(10);
    }
    struct B {
      const char* id;
      const char* label;
      const char* hint;
    };
    const B bs[3] = {{"add", "Add frame", "Add a frame after this one (a copy of it)"}, {"paste", "Paste", "Paste the copied frame over this one"}, {"copy", "Copy", "Copy this frame"}};
    for (int i = 0; i < 3; ++i) {
      const float w = textW(c, bs[i].label, T(12), false) + M(26);
      r -= w;
      if (btnLook(c, bs[i].id, r, ty, w, th, bs[i].label, false, false, T(12))) {
        if (i == 2) clipFrame() = cur;
        if (i == 1 && clipFrame().size() == cur.size()) next = F, next[static_cast<size_t>(u.wtFrame)] = clipFrame(), did = "Paste frame";
        if (i == 0 && nf < kMaxFrames) next = F, next.insert(next.begin() + u.wtFrame + 1, cur), did = "Add frame", u.wtFrame += 1;
      }
      if (ImGui::IsItemHovered()) c.a.setHint(bs[i].hint);
      r -= M(10);
    }
  }
  // ================================================================ the frame (or the whole table in depth)
  const float wy = ty + th + M(15), wb = kTopH + M(572), wh = wb - wy;
  {
    wellLook(c, lx, wy, lw, wh);
    const int gx = std::max(1, u.wtGrid), gy = std::max(1, u.wtGridY);
    const float x0 = lx + M(10), w = lw - M(20), y0 = wy + M(30), h = wh - M(44);
    {
      char b[96];
      std::snprintf(b, sizeof b, "Frame %d, drawing with %s%s%s", u.wtFrame + 1, kBrushName[u.wtBrush], u.wtGrid > 1 ? ", snap 1/" : "",
                    u.wtGrid > 1 ? std::to_string(u.wtGrid).c_str() : "");
      MonoText mono;
      capsText(c, lx + M(12), wy + M(10), u.wt3dEdit ? "The whole table: drag across it to move through the frames" : b, P.textFaint, T(10), M(0.7f));
    }
    if (u.wt3dEdit) {
      tableDepthAt(c, o, x0, y0, w, h);
      ImGui::SetCursorScreenPos(c.p(x0, y0));
      ImGui::InvisibleButton("##depth", c.F.sz(w, h));
      if (ImGui::IsItemActive()) u.wtFrame = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - c.p(x0, 0).x) / c.s(w) * nf), 0, nf - 1);
      if (ImGui::IsItemHovered()) c.a.setHint("Drag across to pick a frame, then Edit frame to draw it");
    } else {
      for (int i = 1; i < gx; ++i) vline(c, x0 + w * i / gx, y0, y0 + h, theme::withAlpha(P.text, 0.05f));
      for (int i = 1; i < gy; ++i) hline(c, x0, x0 + w, y0 + h * i / gy, theme::withAlpha(P.text, 0.05f));
      if (gx <= 1) gridLook(c, x0, y0, w, h, 8, 4, false);
      hline(c, x0, x0 + w, y0 + h * 0.5f, theme::withAlpha(P.text, 0.1f));
      if (u.wtFrame > 0) wave(c, F[static_cast<size_t>(u.wtFrame - 1)], x0, y0, w, h, theme::withAlpha(P.textFaint, 0.4f), std::max(1.f, c.s(1)), false);
      {  // this frame: a soft fill, the line with a low glow
        const int n = std::max(8, static_cast<int>(c.s(w) / 2));
        std::vector<ImVec2> pts;
        for (int i = 0; i <= n; ++i) {
          const size_t s = std::min(cur.size() - 1, static_cast<size_t>(static_cast<double>(i) / n * static_cast<double>(cur.size())));
          pts.push_back(c.p(x0 + w * i / n, y0 + h * 0.5f - std::clamp(cur[s], -1.2f, 1.2f) * h * 0.46f));
        }
        fillUnder(c, pts, c.p(0, y0 + h * 0.5f).y, col, 0.22f, 0.04f, c.p(0, y0).y);
        glowLine(c, pts, col, M(2));
      }
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
          c.dl->AddLine(c.p(x0 + w * lineX, y0 + h * 0.5f - lineY * h * 0.46f), c.p(x0 + w * fx, y0 + h * 0.5f - fy * h * 0.46f), P.mod, c.s(1.5f));
        } else {
          const int ca = static_cast<int>(std::min(lastX, fx) * cells), cb = static_cast<int>(std::max(lastX, fx) * cells);
          for (int k = ca; k <= cb && k < cells; ++k) cellFill(k, fy);
          lastX = fx;
          pending = true;
        }
      }
      if (ImGui::IsItemDeactivated() && u.wtBrush == BrLine) fill(lineX, lineY, fx, fy), pending = true;
      if (ImGui::IsItemHovered()) {
        const float vx = x0 + w * fx;
        for (float yy = y0; yy < y0 + h; yy += M(6)) c.dl->AddLine(c.p(vx, yy), c.p(vx, std::min(y0 + h, yy + M(3))), theme::withAlpha(col, 0.6f));
        char b[24];
        std::snprintf(b, sizeof b, "%+.2f", static_cast<double>(fy));
        MonoText mono;
        text(c, vx + M(8), y0 + h * 0.5f - fy * h * 0.46f - M(18), b, P.text, T(11), false);
        c.a.setHint("Draw on the frame \xC2\xB7 \xE2\x86\x90 \xE2\x86\x92 change frame");
      }
      // the whole table, small, at the top right (playing)
      {
        const float iw = std::min(M(230), w * 0.3f), ih = M(140), ix = lx + lw - iw - M(14), iy = wy + M(12);
        c.dl->AddRectFilled(c.p(ix, iy), c.p(ix + iw, iy + ih), P.well, c.s(M(4)));
        c.dl->AddRect(c.p(ix, iy), c.p(ix + iw, iy + ih), P.lineHi, c.s(M(4)));
        {
          MonoText mono;
          capsText(c, ix + M(8), iy + M(8), c.hasLive && c.live.voices > 0 ? "Whole table, playing" : "Whole table", P.textFaint, T(9.5f), M(0.6f));
        }
        tableDepthAt(c, o, ix + M(8), iy + M(22), iw - M(16), ih - M(28));
      }
    }
  }
  // ================================================================ harmonics: drag a bar (levels, or phases)
  {
    const float hy = kTopH + M(588);
    const int nh = std::clamp(32 << std::clamp(u.wtZoom, 0, 3), 32, kHarm);
    char note[24];
    std::snprintf(note, sizeof note, "%d shown", nh);
    float r = sectionHead(c, lx, hy, lw, "Harmonics", u.wtPhases ? "drag a mark to change one partial's phase" : "drag a bar to change one partial", "");
    {  // how many, and levels or phases
      const float w = M(150);
      r -= w;
      MonoText mono;
      const int hit = segPick(c, "hz", r, hy, w, M(22), {"32", "64", "128", "256"}, std::clamp(u.wtZoom, 0, 3), true, T(10));
      if (hit >= 0) u.wtZoom = hit;
      r -= M(8);
    }
    {
      const float w = M(130);
      r -= w;
      const int hit = segPick(c, "lp", r, hy, w, M(22), {"Levels", "Phases"}, u.wtPhases ? 1 : 0, false, T(10.5f));
      if (hit >= 0) u.wtPhases = hit == 1;
    }
    const float by = hy + M(26), bh = M(86), x0 = lx, w = lw;
    wellLook(c, x0, by, w, bh);
    const Harm& H = harmOf(cur);
    float peak = 1e-6f;
    for (int i = 1; i <= nh; ++i) peak = std::max(peak, H.mag(i));
    const float bw = w / nh, my0 = by + M(14), mh = bh - M(18);
    {
      MonoText mono;
      for (int i = 1; i <= nh; i += nh / 8) text(c, x0 + (i - 1) * bw + M(4), by + M(3), std::to_string(i).c_str(), P.textFaint, T(9), false);
    }
    for (int i = 1; i <= nh; ++i) {
      const float bx = x0 + (i - 1) * bw;
      if (u.wtPhases) {
        if (H.mag(i) > 1e-4f) {
          const float pv = phaseOf(H, i) / 6.2831853f, yy = my0 + mh - pv * mh;
          c.dl->AddRectFilled(c.p(bx + bw * 0.2f, yy - M(1)), c.p(bx + bw * 0.8f, yy + M(1)), theme::withAlpha(P.mod, 0.9f), c.s(M(0.5f)));
        }
      } else {
        const float v = std::sqrt(std::min(1.f, H.mag(i) / std::max(1.f, peak)));
        const float top = my0 + mh - std::max(v * mh, M(1.5f));
        c.dl->AddRectFilled(c.p(bx + bw * 0.18f, top), c.p(bx + bw * 0.82f, my0 + mh), v > 0.003f ? col : theme::withAlpha(col, 0.25f), c.s(M(1)));
      }
    }
    ImGui::SetCursorScreenPos(c.p(x0, my0));
    ImGui::InvisibleButton("##harm", c.F.sz(w, mh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int hi = std::clamp(1 + static_cast<int>((m.x - c.p(x0, 0).x) / c.s(bw)), 1, nh);
    if (ImGui::IsItemActive()) {
      const float hv = std::clamp((c.p(0, my0 + mh).y - m.y) / c.s(mh), 0.f, 1.f);
      if (u.wtPhases) {
        if (H.mag(hi) > 1e-4f) setPhase(cur, H, hi, hv * 6.2831853f), pending = true;
      } else {
        setHarm(cur, H, hi, ImGui::IsMouseDown(1) ? 0.f : hv * hv * std::max(1.f, peak));
        pending = true;
      }
    }
    if (ImGui::IsItemHovered()) {
      char b[48];
      std::snprintf(b, sizeof b, "#%d  %.1f dB", hi, 20.0 * std::log10(std::max(1e-6, static_cast<double>(H.mag(hi)))));
      MonoText mono;
      textR(c, x0 + w - M(8), by + M(3), b, P.label, T(10), false);
      c.a.setHint(u.wtPhases ? "Phases: drag to set the phase of each partial" : "Partials: drag to set levels (phases kept) \xC2\xB7 right-drag: remove");
    }
  }
  // ================================================================ every frame in a row: click or drag to pick one
  {
    const float sy = kTopH + M(716), sh = M(50), x0 = lx, w = lw;
    wellLook(c, x0, sy, w, sh);
    const float fw = w / std::max(1, nf);
    const int step = std::max(1, static_cast<int>(std::ceil(nf / std::max(1.f, c.s(w) / 6.f))));
    for (int i = 0; i < nf; i += step) {
      const bool sel = u.wtFrame >= i && u.wtFrame < i + step;
      wave(c, F[static_cast<size_t>(i)], x0 + i * fw + M(1), sy + M(4), fw * step - M(2), sh - M(8), sel ? col : theme::withAlpha(col, 0.55f), std::max(1.f, c.s(M(1))), false);
    }
    const float sx = x0 + u.wtFrame * fw;
    c.dl->AddRect(c.p(sx, sy + M(1)), c.p(sx + std::max(fw, M(10)), sy + sh - M(1)), col, c.s(M(2)), 0, std::max(1.f, c.s(M(1.3f))));
    const float lp = c.hasLive && c.live.voices > 0 && c.live.oscOn[o] ? c.live.oscPos[o] : -1.f;
    if (lp >= 0) {  // the frame playing now
      const float px = x0 + w * std::clamp(lp, 0.f, 1.f);
      c.dl->AddRectFilled(c.p(px - M(7), sy + sh + M(2)), c.p(px + M(7), sy + sh + M(4)), P.fam[FamLfo], c.s(M(1)));
    }
    ImGui::SetCursorScreenPos(c.p(x0, sy));
    ImGui::InvisibleButton("##strip", c.F.sz(w, sh));
    if (ImGui::IsItemActive()) u.wtFrame = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - c.p(x0, 0).x) / c.s(fw)), 0, nf - 1);
    if (ImGui::IsItemHovered()) c.a.setHint("Every frame: click or drag to pick one");
  }
  // ================================================================ right: table, draw, change frames, voice
  auto card = [&](float y, float h, const char* title, const char* note) {
    c.dl->AddRectFilled(c.p(rx, y), c.p(rx + rw, y + h), barBg(c), c.s(M(6)));
    c.dl->AddRect(c.p(rx, y), c.p(rx + rw, y + h), P.line, c.s(M(6)));
    text(c, rx + M(14), y + M(22) - T(13) * 0.68f, title, P.text, T(13), true);
    if (note && *note) textR(c, rx + rw - M(14), y + M(22) - T(11) * 0.68f, note, P.textFaint, T(11), false);
    return y + M(38);
  };
  const float ix = rx + M(14), iw = rw - M(28);
  float cy = kTopH + M(88);
  {  // Table
    char note[48];
    std::snprintf(note, sizeof note, "%d frames, %d points", nf, static_cast<int>(cur.size()));
    float y = card(cy, M(122), "Table", note);
    const float aw = M(30), fw = iw - aw * 2 - M(16);
    ImGui::SetCursorScreenPos(c.p(ix, y));
    if (ImGui::InvisibleButton("##tblpick", c.F.sz(fw, M(30)))) ImGui::OpenPopup("##tblmenu");
    const bool hv = ImGui::IsItemHovered();
    if (hv) c.a.setHint(shownName + ": click for every table");
    c.dl->AddRectFilled(c.p(ix, y), c.p(ix + fw, y + M(30)), P.field, c.s(M(4)));
    c.dl->AddRect(c.p(ix, y), c.p(ix + fw, y + M(30)), hv ? P.lineHi : P.line, c.s(M(4)));
    {
      std::string s = shownName;
      while (s.size() > 3 && textW(c, s.c_str(), T(12), false) > fw - M(36)) s.pop_back();
      text(c, ix + M(10), y + M(15) - T(12) * 0.68f, s.c_str(), P.text, T(12), false);
      const ImVec2 m = c.p(ix + fw - M(14), y + M(15));
      const float a = c.s(M(3.5f));
      c.dl->AddLine(ImVec2(m.x - a, m.y - a * 0.5f), ImVec2(m.x, m.y + a * 0.5f), P.label, std::max(1.f, c.s(M(1.2f))));
      c.dl->AddLine(ImVec2(m.x, m.y + a * 0.5f), ImVec2(m.x + a, m.y - a * 0.5f), P.label, std::max(1.f, c.s(M(1.2f))));
    }
    if (ImGui::BeginPopup("##tblmenu")) {
      if (custom) {
        char nb[64];
        std::snprintf(nb, sizeof nb, "%s", d.tableName.c_str());
        ImGui::SetNextItemWidth(c.s(160));
        if (ImGui::InputText(i18n::tr("Name"), nb, sizeof nb, ImGuiInputTextFlags_EnterReturnsTrue)) d.tableName = nb, commit(c, "Rename table");
        ImGui::Separator();
      }
      tableMenuPublic(c, o);
      ImGui::EndPopup();
    }
    for (int i = 0; i < 2; ++i) {  // the table before / after in the catalog
      ImGui::PushID(i + 9500);
      const float bx = ix + fw + M(8) + i * (aw + M(8));
      if (btnLook(c, "tstep", bx, y, aw, M(30), "", false, false, T(12))) {
        const TableCatalog& cat = tableCatalogUi(c);
        const int n = static_cast<int>(cat.entries.size());
        int at = -1;
        for (int e = 0; e < n; ++e) {
          const TableEntry& te = cat.entries[static_cast<size_t>(e)];
          if (te.factory > 0 ? (d.table.empty() && vali(c, oscP(o, O_Table)) == te.factory) : d.table == te.ref) at = e;
        }
        if (n > 0) {
          const int nx = at < 0 ? 0 : (at + (i ? 1 : -1) + n) % n;
          chooseTableEntry(c, o, cat.entries[static_cast<size_t>(nx)]);
          ImGui::PopID();
          return;
        }
      }
      if (ImGui::IsItemHovered()) c.a.setHint(i ? "The next table" : "The table before");
      const ImVec2 m = c.p(bx + aw * 0.5f, y + M(15));
      const float a = c.s(M(3)), sg = i ? -1.f : 1.f;
      c.dl->AddLine(ImVec2(m.x + a * 0.5f * sg, m.y - a), ImVec2(m.x - a * 0.5f * sg, m.y), P.label, std::max(1.f, c.s(M(1.3f))));
      c.dl->AddLine(ImVec2(m.x - a * 0.5f * sg, m.y), ImVec2(m.x + a * 0.5f * sg, m.y + a), P.label, std::max(1.f, c.s(M(1.3f))));
      ImGui::PopID();
    }
    y += M(30) + M(10);
    const float bw = (iw - M(16)) / 3;
    if (btnLook(c, "load", ix, y, bw, M(30), "Load", false, false, T(12)))
      openFile(c, i18n::tr("Load wavetable"), {".wav"}, [o](App& a, const PluginTarget& t, const std::string& p) {
        editState(a, t, "Load wavetable", [&](PluginState& st, Model& m) {
          auto& od = m.osc[static_cast<size_t>(o)];
          od.table = "file:" + p;
          od.tableName = fs_stem(p);
          od.frames.clear();
          st.params[static_cast<size_t>(oscP(o, O_Table))] = 0;
        });
      });
    if (ImGui::IsItemHovered()) c.a.setHint("Load: play a WAV from where it is (the sound keeps a link to the file)");
    if (btnLook(c, "imp", ix + bw + M(8), y, bw, M(30), "Import", false, false, T(12)))
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
    if (ImGui::IsItemHovered()) c.a.setHint("Import: copy a WAV's frames into the sound to edit them (2048 samples each, or the cycle length the file states)");
    if (btnLook(c, "exp", ix + (bw + M(8)) * 2, y, bw, M(30), "Export", false, false, T(12))) exportFrames(c, F, d.tableName);
    if (ImGui::IsItemHovered()) c.a.setHint("Save the table as a WAV of 2048-sample frames");
    cy += M(122) + M(10);
  }
  {  // Draw: every brush, the snap grid
    const float h = M(178);
    float y = card(cy, h, "Draw", "on the frame");
    const int per = 6;
    const float bw = (iw - M(6) * (per - 1)) / per, bh = M(34);
    for (int b = 0; b < BrCount; ++b) {
      const float bx = ix + (b % per) * (bw + M(6)), by = y + (b / per) * (bh + M(6));
      ImGui::PushID(b + 9200);
      const bool on = u.wtBrush == b;
      if (btnLook(c, "br", bx, by, bw, bh, "", false, on, T(10))) u.wtBrush = b;
      if (ImGui::IsItemHovered()) c.a.setHint(std::string("Draw: ") + kBrushName[b] + (b >= BrFlat ? " (fills each grid cell the pointer passes)" : ""));
      brushIcon(c, b, bx + bw * 0.2f, by + M(8), bw * 0.6f, bh - M(16), on ? col : P.label);
      ImGui::PopID();
    }
    y += (bh + M(6)) * 2;
    static const int snaps[4] = {0, 8, 16, 32};
    auto snapRow = [&](const char* label, const char* id, int& v, float yy) {
      text(c, ix, yy + M(11) - T(11.5f) * 0.68f, label, P.label, T(11.5f), false);
      int cur2 = 0;
      for (int i = 0; i < 4; ++i)
        if (snaps[i] == v) cur2 = i;
      MonoText mono;
      const int hit = segPick(c, id, ix + iw - M(164), yy, M(164), M(22), {"Off", "1/8", "1/16", "1/32"}, cur2, true, T(10));
      if (hit >= 0) v = snaps[hit];
    };
    snapRow("Snap across", "snapx", u.wtGrid, y);
    snapRow("Snap up and down", "snapy", u.wtGridY, y + M(26));
    cy += h + M(10);
  }
  {  // Change frames: this one, every one, fill between; the rest folded
    const float h = M(170);
    float y = card(cy, h, "Change frames", "");
    struct Row {
      const char* name;
      const char* note;
      const char* pop;
    };
    const Row rows[3] = {{"Change this frame", "this frame", "##wtsingle"}, {"Change every frame", "every frame", "##wtall"}, {"Blend frames between", "", "##wtmorph"}};
    char between[24];
    std::snprintf(between, sizeof between, "1 to %d", nf);
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i + 9600);
      const float ry = y + i * M(32);
      ImGui::SetCursorScreenPos(c.p(ix, ry));
      if (ImGui::InvisibleButton("##row", c.F.sz(iw, M(28)))) ImGui::OpenPopup(rows[i].pop);
      const bool hv = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(ix, ry), c.p(ix + iw, ry + M(28)), hv ? P.panelHi : P.panel, c.s(M(4)));
      c.dl->AddRect(c.p(ix, ry), c.p(ix + iw, ry + M(28)), P.line, c.s(M(4)));
      text(c, ix + M(12), ry + M(14) - T(12) * 0.68f, rows[i].name, P.text, T(12), false);
      textR(c, ix + iw - M(12), ry + M(14) - T(10.5f) * 0.68f, i == 2 ? between : rows[i].note, P.textFaint, T(10.5f), false);
      ImGui::PopID();
    }
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
    if (ImGui::BeginPopup("##wtmorph")) {
      for (int sp = 0; sp < 2; ++sp)
        if (ImGui::BeginMenu(sp ? i18n::tr("Spectral") : i18n::tr("Crossfade"))) {
          for (int k : {8, 16, 32, 64, 128, 256})
            if (ImGui::MenuItem((std::to_string(k) + " frames").c_str())) next = morphTable(F, k, sp == 1), did = "Morph";
          ImGui::EndMenu();
        }
      ImGui::EndPopup();
    }
    // the folded tools: a formula, shifting the frame by a grid step
    const float fy = y + M(3 * 32) + M(2);
    hline(c, ix, ix + iw, fy, P.line);
    ImGui::SetCursorScreenPos(c.p(ix, fy + M(4)));
    if (ImGui::InvisibleButton("##tools", c.F.sz(iw, M(22)))) ImGui::OpenPopup("##wttools");
    const bool hv = ImGui::IsItemHovered();
    {
      const ImVec2 m = c.p(ix + M(4), fy + M(15));
      const float a = c.s(M(2.6f));
      c.dl->AddTriangleFilled(ImVec2(m.x - a * 0.5f, m.y - a), ImVec2(m.x - a * 0.5f, m.y + a), ImVec2(m.x + a * 0.7f, m.y), P.label);
    }
    text(c, ix + M(14), fy + M(15) - T(12) * 0.68f, "Formula and shifting tools", hv ? P.text : P.label, T(12), false);
    textR(c, ix + iw, fy + M(15) - T(11) * 0.68f, "6 more", P.textFaint, T(11), false);
    if (ImGui::BeginPopup("##wttools")) {
      ImGui::TextDisabled("%s", i18n::tr("Formula: x = -1..1 across the cycle, y = 0..1 across the table, in = the current sample"));
      char fb[256];
      std::snprintf(fb, sizeof fb, "%s", u.wtFormula.c_str());
      ImGui::SetNextItemWidth(c.s(320));
      const bool enter = ImGui::InputTextWithHint("##wtf", i18n::tr("Formula, e.g. sin(x * 3.14)"), fb, sizeof fb, ImGuiInputTextFlags_EnterReturnsTrue);
      u.wtFormula = fb;
      std::string run;
      bool all = true;
      if (enter && !u.wtFormula.empty()) run = u.wtFormula;
      if (ImGui::Button(i18n::tr("Every frame")) && !u.wtFormula.empty()) run = u.wtFormula;
      ImGui::SameLine();
      if (ImGui::Button(i18n::tr("This frame")) && !u.wtFormula.empty()) run = u.wtFormula, all = false;
      if (ImGui::BeginMenu(i18n::tr("Examples"))) {
        for (const auto& f : kFormulas)
          if (ImGui::MenuItem(f[0], f[1])) u.wtFormula = f[1], run = f[1];
        ImGui::EndMenu();
      }
      if (!u.wtFormulaErr.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.danger), "%s", u.wtFormulaErr.c_str());
      if (!run.empty()) {
        next = F;
        std::string err;
        if (applyFormula(next, run, all, u.wtFrame, err)) did = "Formula", u.wtFormulaErr.clear();
        else u.wtFormulaErr = err, next.clear();
      }
      ImGui::Separator();
      ImGui::TextDisabled("%s", i18n::tr("Shift this frame by one grid step"));
      const char* tips[] = {"Shift left", "Shift right", "Shift up", "Shift down"};
      for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(i18n::tr(tips[i]))) {
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
      }
      ImGui::EndPopup();
    }
    cy += h + M(10);
  }
  {  // Voice: how the layer plays the table
    const float h = kPlayY - M(18) - cy;
    float y = card(cy, h, "Voice", "how the layer plays the table");
    const int ks[4] = {oscP(o, O_Unison), oscP(o, O_UniDetune), oscP(o, O_UniWidth), oscP(o, O_Warp1)};
    const char* ls[4] = {"Unison", "Detune", "Spread", "Warp"};
    const float dx = iw / 4;
    for (int i = 0; i < 4; ++i) knob(c, ks[i], ix + dx * (i + 0.5f), y + M(14), M(15), ls[i], KnobValue | (i == 2 ? KnobBipolar : 0));
    y += M(64);
    text(c, ix, y + M(14) - T(12) * 0.68f, "Warp style", P.label, T(12), false);
    {
      reach(oscP(o, O_Warp1Mode));
      const std::string wm = ptext(c, oscP(o, O_Warp1Mode));
      const float fx = ix + iw * 0.45f, fw = iw * 0.55f;
      ImGui::SetCursorScreenPos(c.p(fx, y));
      if (ImGui::InvisibleButton("##warp", c.F.sz(fw, M(28)))) ImGui::OpenPopup("##warpmenu");
      const bool hv = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(fx, y), c.p(fx + fw, y + M(28)), P.field, c.s(M(4)));
      c.dl->AddRect(c.p(fx, y), c.p(fx + fw, y + M(28)), hv ? P.lineHi : P.line, c.s(M(4)));
      std::string s = wm;
      while (s.size() > 3 && textW(c, s.c_str(), T(12), false) > fw - M(30)) s.pop_back();
      text(c, fx + M(10), y + M(14) - T(12) * 0.68f, s.c_str(), P.text, T(12), false);
      if (ImGui::BeginPopup("##warpmenu")) {
        warpMenuItems(c, o, 0);
        ImGui::EndPopup();
      }
    }
    // the rest of the layer, folded
    const float fy = cy + h - M(10) - M(22);
    hline(c, ix, ix + iw, fy - M(4), P.line);
    ImGui::SetCursorScreenPos(c.p(ix, fy));
    if (ImGui::InvisibleButton("##voicemore", c.F.sz(iw, M(22)))) ImGui::OpenPopup("##voicemorepop");
    const bool hv = ImGui::IsItemHovered();
    {
      const ImVec2 m = c.p(ix + M(4), fy + M(11));
      const float a = c.s(M(2.6f));
      c.dl->AddTriangleFilled(ImVec2(m.x - a * 0.5f, m.y - a), ImVec2(m.x - a * 0.5f, m.y + a), ImVec2(m.x + a * 0.7f, m.y), P.label);
    }
    const std::vector<int> more = {oscP(o, O_Phase),     oscP(o, O_RandPhase), oscP(o, O_PhaseMemory), oscP(o, O_Smooth),     oscP(o, O_Octave),
                                   oscP(o, O_Semi),      oscP(o, O_Fine),      oscP(o, O_PitchMode),   oscP(o, O_UniStack),   oscP(o, O_UniBlend),
                                   oscP(o, O_UniSpan),   oscP(o, O_UniRange),  oscP(o, O_UniRotate),   oscP(o, O_UniRandStart), oscP(o, O_UniWtPos),
                                   oscP(o, O_UniWarp1),  oscP(o, O_UniWarp2),  oscP(o, O_DetuneMode),  oscP(o, O_Warp1Var),   oscP(o, O_Warp2Mode),
                                   oscP(o, O_Warp2),     oscP(o, O_Warp2Var)};
    char note[16];
    std::snprintf(note, sizeof note, "%d more", static_cast<int>(more.size()));
    text(c, ix + M(14), fy + M(11) - T(12) * 0.68f, "Phase, pitch and stack options", hv ? P.text : P.label, T(12), false);
    textR(c, ix + iw, fy + M(11) - T(11) * 0.68f, note, P.textFaint, T(11), false);
    for (int k : more) reach(k);
    if (ImGui::BeginPopup("##voicemorepop")) {
      paramRows(c, more);
      ImGui::EndPopup();
    }
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

void exportTablePublic(Ctx& c, int o) { exportFrames(c, currentFrames(c, o), c.m.osc[static_cast<size_t>(o)].tableName); }

}  // namespace gl::packs::plasma::ui
