// Gridline — "mini" pack: editors. Every Mini plugin uses the same compact card
// (one layout system, one width): a title strip (wordmark hook + family accent
// + suite tag), an optional display well that explains the effect, a 5-column
// knob grid and an option row. Controls come from the shared pui kit, so the
// app-wide control design applies to the whole suite.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginEditorsCommon.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../core/I18n.h"
#include "../../engine/Dsp.h"
#include "../PacksUi.h"
#include "mini.h"
#include "mini_curves.h"

namespace gl::packs::mini {

namespace {

using theme::pal;
using i18n::N_;
using cplx = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

// ---- suite identity ------------------------------------------------------------
// Accent of each family (0xRRGGBB): colour marks the family, never the plugin.
uint32_t familyRole(Family f) {
  switch (f) {
    case Family::Dynamics: return 0xF2A541;  // amber
    case Family::Tone: return 0x8BC34A;      // lime
    case Family::Drive: return 0xFF6B6B;     // coral
    case Family::Pitch: return 0xF06292;     // rose
    case Family::Motion: return 0x4DD0E1;    // cyan
    case Family::Time: return 0x9575CD;      // violet
    default: return 0x90A4AE;                // slate (utility)
  }
}
const char* familyName(Family f) {
  switch (f) {
    case Family::Dynamics: return N_("dynamics");
    case Family::Tone: return N_("tone");
    case Family::Drive: return N_("drive");
    case Family::Pitch: return N_("pitch");
    case Family::Motion: return N_("motion");
    case Family::Time: return N_("time");
    default: return N_("utility");
  }
}

// Suite mark: a small rounded square with a filled quarter (the "mini" cell of
// the Gridline grid). Hook for the J-130 suite logo: replace this one drawing.
void suiteMark(ImDrawList* dl, ImVec2 p, float s, ImU32 fg, ImU32 accent) {
  dl->AddRect(p, ImVec2(p.x + s, p.y + s), fg, 2.5f, 0, 1.2f);
  const float h = s * 0.5f;
  dl->AddRectFilled(ImVec2(p.x + h, p.y + h), ImVec2(p.x + s - 1.5f, p.y + s - 1.5f), accent, 1.5f);
  dl->AddLine(ImVec2(p.x + h, p.y + 2), ImVec2(p.x + h, p.y + s - 2), theme::withAlpha(fg, 0.5f));
  dl->AddLine(ImVec2(p.x + 2, p.y + h), ImVec2(p.x + s - 2, p.y + h), theme::withAlpha(fg, 0.5f));
}

// Title strip: the plugin's wordmark (J-130 hook: pui::wordmark draws one when the
// brand table has it), otherwise the lower-case name after the family accent bar.
void titleStrip(App& a, const Entry& e, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 30.f;
  const ImU32 accent = theme::litRole(familyRole(e.family));
  if (!pui::wordmark(a, e.type, 18.f)) {
    std::string word = e.name;
    for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    dl->AddRectFilled(ImVec2(p.x, p.y + 7), ImVec2(p.x + 3, p.y + h - 7), accent, 1.f);
    ImFont* f = a.fontBold ? a.fontBold : ImGui::GetFont();
    dl->AddText(f, 18.f, ImVec2(p.x + 10, p.y + (h - 18.f) * 0.5f), P.text, word.c_str());
    ImGui::Dummy(ImVec2(width, h));
  } else {
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  }
  // Right side: family + suite mark.
  const char* fam = i18n::tr(familyName(e.family));
  ImGui::PushFont(a.fontMono, 10.f);
  const float tw = ImGui::CalcTextSize(fam).x;
  ImGui::PopFont();
  const float mx = p.x + width - 16.f;
  ed::monoText(dl, ImVec2(mx - 8.f - tw, p.y + (h - 10.f) * 0.5f), P.textFaint, fam, 10.f);
  suiteMark(dl, ImVec2(mx, p.y + (h - 14.f) * 0.5f), 14.f, P.textDim, accent);
  dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + width, p.y + h - 1), P.line);
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6));
}

// ---- displays --------------------------------------------------------------------
float val(const PluginState& s, int k) { return ed::val(s, k); }
int ival(const PluginState& s, int k) { return ed::ival(s, k); }

struct Well {
  ImDrawList* dl;
  ImVec2 p0, p1;
  float w() const { return p1.x - p0.x; }
  float h() const { return p1.y - p0.y; }
};

// Response curve (dB over log frequency) with the live output spectrum beneath it.
template <typename F>
void responseCurve(App& a, const PluginTarget& t, const Well& w, double range, ImU32 col, F&& magDb) {
  const auto& P = pal();
  pui::drawFreqGrid(w.dl, w.p0, w.p1, range, false);
  const int N = 96;
  float spec[N];
  if (const ScopeTap* tap = t.tap(a)) {
    pui::spectrum(tap, t.key(), N, spec, a.engine.sampleRate());
    for (int k = 0; k + 1 < N; ++k) {
      auto Y = [&](float db) { return w.p1.y - std::clamp((db + 90.f) / 96.f, 0.f, 1.f) * w.h(); };
      w.dl->AddLine(ImVec2(w.p0.x + w.w() * (k + 0.5f) / N, Y(spec[k])), ImVec2(w.p0.x + w.w() * (k + 1.5f) / N, Y(spec[k + 1])),
                    theme::withAlpha(P.textFaint, 0.5f));
    }
  }
  ImVec2 pts[128];
  for (int k = 0; k < 128; ++k) {
    const float x = w.p0.x + w.w() * k / 127.f;
    const double db = std::clamp(magDb(pui::xToFreq(x, w.p0.x, w.w())), -range, range);
    pts[k] = ImVec2(x, w.p0.y + w.h() * 0.5f - static_cast<float>(db / range) * w.h() * 0.5f);
  }
  w.dl->AddPolyline(pts, 128, theme::withAlpha(col, 0.25f), 0, 5.f);
  w.dl->AddPolyline(pts, 128, col, 0, 2.f);
}

double bqDb(dsp::Biquad::Type ty, double hz, double q, double gain, double f, double sr) {
  dsp::Biquad b;
  b.set(ty, sr, hz, q, gain);
  return 20.0 * std::log10(std::max(1e-9, b.magnitude(f, sr)));
}

// Transfer curve y = f(x) over x, y in [-1, 1].
template <typename F>
void transfer(const Well& w, ImU32 col, F&& f) {
  const auto& P = pal();
  pui::drawGrid(w.dl, w.p0, w.p1, 4, 4, true);
  const float cx = (w.p0.x + w.p1.x) * 0.5f, cy = (w.p0.y + w.p1.y) * 0.5f, sx = w.w() * 0.5f - 6, sy = w.h() * 0.5f - 6;
  w.dl->AddLine(ImVec2(cx - sy, cy + sy), ImVec2(cx + sy, cy - sy), theme::withAlpha(P.lineStrong, 0.5f));
  ImVec2 pts[161];
  for (int k = 0; k <= 160; ++k) {
    const double x = -1.0 + 2.0 * k / 160.0;
    const double y = std::clamp(f(x), -1.2, 1.2);
    pts[k] = ImVec2(cx + static_cast<float>(x) * sx, cy - static_cast<float>(y) * sy);
  }
  w.dl->AddPolyline(pts, 161, theme::withAlpha(col, 0.25f), 0, 5.f);
  w.dl->AddPolyline(pts, 161, col, 0, 2.f);
}

// Dynamics: in/out curve (dB) on the left, gain meter on the right.
template <typename F>
void dynamics(App& a, const PluginTarget& t, const Well& w, ImU32 col, double lo, F&& outDb, bool reduction = true) {
  const auto& P = pal();
  float gr = 0, in = -120;
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
      gr = rt->meter.load();
      in = rt->input.load();
    }
  const float meterW = 12.f;
  const ImVec2 q1(w.p1.x - meterW - 14.f, w.p1.y);
  const float side = q1.x - w.p0.x;
  const ImVec2 c0(w.p0.x + 6, w.p0.y + 6), c1(q1.x - 6, w.p1.y - 6);
  auto X = [&](double db) { return c0.x + static_cast<float>((db - lo) / -lo) * (c1.x - c0.x); };
  auto Y = [&](double db) { return c1.y - static_cast<float>((std::clamp(db, lo, 0.0) - lo) / -lo) * (c1.y - c0.y); };
  for (double db = lo; db <= 0.01; db += 12) {
    w.dl->AddLine(ImVec2(X(db), c0.y), ImVec2(X(db), c1.y), P.gridStep);
    w.dl->AddLine(ImVec2(c0.x, Y(db)), ImVec2(c1.x, Y(db)), P.gridStep);
  }
  w.dl->AddLine(ImVec2(X(lo), Y(lo)), ImVec2(X(0), Y(0)), theme::withAlpha(P.lineStrong, 0.5f));
  ImVec2 pts[97];
  for (int k = 0; k <= 96; ++k) {
    const double db = lo + (-lo) * k / 96.0;
    pts[k] = ImVec2(X(db), Y(outDb(db)));
  }
  w.dl->AddPolyline(pts, 97, theme::withAlpha(col, 0.25f), 0, 5.f);
  w.dl->AddPolyline(pts, 97, col, 0, 2.f);
  if (in > lo) w.dl->AddCircleFilled(ImVec2(X(std::min<double>(in, 0)), Y(outDb(std::min<double>(in, 0)))), 4.f, P.paper);
  ed::monoText(w.dl, ImVec2(w.p0.x + side + 2, w.p0.y + 4), P.textFaint, reduction ? "GR" : "dB", 9.f);
  pui::levelMeter(w.dl, ImVec2(w.p1.x - meterW - 6, w.p0.y + 18), ImVec2(w.p1.x - 6, w.p1.y - 6), reduction ? std::min(0.f, gr) : gr,
                  reduction, 24.f);
}

// Delay taps on a time line: (time 0..1 of the view, level 0..1, lane 0/1).
void taps(const Well& w, ImU32 col, const std::vector<std::array<double, 3>>& v) {
  const auto& P = pal();
  pui::drawGrid(w.dl, w.p0, w.p1, 8, 2, false);
  const float base = w.p1.y - 6, top = w.p0.y + 8;
  w.dl->AddLine(ImVec2(w.p0.x + 6, base), ImVec2(w.p0.x + 6, top), P.paper, 2.f);
  for (const auto& tp : v) {
    if (tp[0] > 1.0 || tp[1] < 0.01) continue;
    const float x = w.p0.x + 6 + static_cast<float>(tp[0]) * (w.w() - 12);
    const float y = base - static_cast<float>(tp[1]) * (base - top);
    const ImU32 c = tp[2] > 0.5 ? theme::withAlpha(col, 0.6f) : col;
    w.dl->AddLine(ImVec2(x, base), ImVec2(x, y), c, 2.f);
    w.dl->AddCircleFilled(ImVec2(x, y), 2.5f, c);
  }
}

// Big value readout (utility and pitch plugins).
void readout(App& a, const PluginState& s, const PluginInfo& info, const Well& w, int param, ImU32 col, const char* sub = nullptr) {
  const auto& P = pal();
  const std::string v = info.params[static_cast<size_t>(param)].format(val(s, param));
  ImFont* f = a.fontLarge ? a.fontLarge : ImGui::GetFont();
  const float size = 30.f;
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, v.c_str());
  w.dl->AddText(f, size, ImVec2((w.p0.x + w.p1.x - ts.x) * 0.5f, w.p0.y + (w.h() - ts.y) * 0.5f - (sub ? 6 : 0)), col, v.c_str());
  if (sub) {
    ImGui::PushFont(a.fontMono, 10.f);
    const float sw = ImGui::CalcTextSize(sub).x;
    ImGui::PopFont();
    ed::monoText(w.dl, ImVec2((w.p0.x + w.p1.x - sw) * 0.5f, w.p1.y - 18), P.textFaint, sub, 10.f);
  }
}

// Ladder response (linear model of the Rungs DSP).
double rungsDb(const PluginState& s, double f, double sr) {
  const double fc = std::clamp(static_cast<double>(val(s, rungs::Cutoff)), 20.0, sr * 0.45);
  const double k = 3.9 * std::clamp(static_cast<double>(val(s, rungs::Resonance)), 0.0, 1.0);
  const double g = std::tan(kPi * fc / sr);
  const cplx z1 = std::polar(1.0, -2.0 * kPi * f / sr);
  const cplx h1 = g * (1.0 + z1) / ((1.0 + g) + (g - 1.0) * z1);
  const cplx h2 = h1 * h1, h4 = h2 * h2, den = 1.0 + k * z1 * h4, gain = 1.0 + 0.5 * k;
  cplx h;
  switch (ival(s, rungs::Mode)) {
    case 1: h = h2 / den * gain; break;
    case 2: h = 2.0 * (h2 - h4) / den * gain; break;
    case 3: h = (1.0 - 4.0 * h1 + 6.0 * h2 - 4.0 * h2 * h1 + h4) / den * gain; break;
    default: h = h4 / den * gain; break;
  }
  return 20.0 * std::log10(std::max(1e-9, std::abs(h)));
}

// State-variable response (linear part of Scorch).
double svfDb(int type, double fc, double q, double f) {
  const cplx s(0.0, f / fc);
  const cplx den = s * s + s / q + 1.0;
  cplx h = type == 0 ? 1.0 / den : type == 1 ? s / den : type == 2 ? s * s / den : (s * s + 1.0) / den;
  return 20.0 * std::log10(std::max(1e-9, std::abs(h)));
}

void drawDisplay(App& a, const PluginTarget& t, const PluginState& s, const PluginInfo& info, const Entry& e, const Well& w) {
  const std::string ty = e.type;
  const ImU32 col = theme::litRole(familyRole(e.family));
  const auto& P = pal();
  const double sr = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : 48000.0;
  if (ty == "gl.minitrim") {
    readout(a, s, info, w, trim::Gain, col);
  } else if (ty == "gl.minibreadth") {
    // Stereo field: the mid and side extents as a lozenge.
    const float cx = (w.p0.x + w.p1.x) * 0.5f, cy = (w.p0.y + w.p1.y) * 0.5f, r = w.h() * 0.42f;
    pui::drawGrid(w.dl, w.p0, w.p1, 4, 2, true);
    const float wd = std::clamp(val(s, breadth::Width), 0.f, 2.f) * 0.5f, md = std::clamp(val(s, breadth::Mid), 0.f, 1.f);
    const float pan = val(s, breadth::Pan);
    const ImVec2 q[4] = {ImVec2(cx + pan * r, cy - r * md), ImVec2(cx + pan * r + r * 1.6f * wd, cy), ImVec2(cx + pan * r, cy + r * md),
                         ImVec2(cx + pan * r - r * 1.6f * wd, cy)};
    w.dl->AddConvexPolyFilled(q, 4, theme::withAlpha(col, 0.25f));
    w.dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 2.f);
    ed::monoText(w.dl, ImVec2(w.p0.x + 8, w.p0.y + 6), P.textFaint, "L", 10.f);
    ed::monoText(w.dl, ImVec2(w.p1.x - 14, w.p0.y + 6), P.textFaint, "R", 10.f);
  } else if (ty == "gl.miniskew") {
    readout(a, s, info, w, skew::Delay, col, ival(s, skew::Side) == 1 ? i18n::tr("right side late") : i18n::tr("left side late"));
  } else if (ty == "gl.miniweave") {
    // 2 x 2 matrix: rows are outputs, columns inputs; cell fill = gain, colour = sign.
    const char* rows[2] = {"L", "R"};
    const int idx[2][2] = {{weave::LL, weave::RL}, {weave::LR, weave::RR}};
    const float cw = 60, ch = (w.h() - 24) / 2, x0 = (w.p0.x + w.p1.x) * 0.5f - cw, y0 = w.p0.y + 18;
    for (int c = 0; c < 2; ++c) ed::monoText(w.dl, ImVec2(x0 + c * cw + cw * 0.5f - 12, w.p0.y + 4), P.textFaint, c ? "R IN" : "L IN", 9.f);
    for (int r = 0; r < 2; ++r) {
      ed::monoText(w.dl, ImVec2(x0 - 34, y0 + r * ch + ch * 0.5f - 5), P.textFaint, rows[r], 10.f);
      for (int c = 0; c < 2; ++c) {
        const float v = val(s, idx[r][c]);
        const ImVec2 a0(x0 + c * cw + 3, y0 + r * ch + 3), a1(x0 + (c + 1) * cw - 3, y0 + (r + 1) * ch - 3);
        w.dl->AddRect(a0, a1, P.line, 3.f);
        const ImU32 fc = v >= 0 ? col : P.danger;
        const float k = std::min(1.f, std::fabs(v));
        const float mx = (a0.x + a1.x) * 0.5f, my = (a0.y + a1.y) * 0.5f;
        w.dl->AddRectFilled(ImVec2(mx - (a1.x - a0.x) * 0.5f * k, my - (a1.y - a0.y) * 0.5f * k),
                            ImVec2(mx + (a1.x - a0.x) * 0.5f * k, my + (a1.y - a0.y) * 0.5f * k), theme::withAlpha(fc, 0.7f), 2.f);
      }
    }
  } else if (ty == "gl.minibands") {
    const double f1 = val(s, bands::LowSplit), f2 = std::max(f1 * 1.5, static_cast<double>(val(s, bands::HighSplit)));
    const double gl = dsp::dbToGain(val(s, bands::Low)), gm = dsp::dbToGain(val(s, bands::Mid)), gh = dsp::dbToGain(val(s, bands::High));
    responseCurve(a, t, w, 18, col, [&](double f) {
      auto lp = [&](double fc) { double m = dsp::dbToGain(bqDb(dsp::Biquad::LowPass, fc, 0.70710678, 0, f, sr)); return m * m; };
      auto hp = [&](double fc) { double m = dsp::dbToGain(bqDb(dsp::Biquad::HighPass, fc, 0.70710678, 0, f, sr)); return m * m; };
      return dsp::gainToDb(gl * lp(f1) + gm * hp(f1) * lp(f2) + gh * hp(f1) * hp(f2));
    });
  } else if (ty == "gl.minisieve") {
    static const dsp::Biquad::Type map[7] = {dsp::Biquad::LowPass, dsp::Biquad::HighPass, dsp::Biquad::BandPass, dsp::Biquad::Notch,
                                             dsp::Biquad::Peak, dsp::Biquad::LowShelf, dsp::Biquad::HighShelf};
    const int type = std::clamp(ival(s, sieve::Type), 0, 6);
    const int stages = type <= 3 ? std::clamp(ival(s, sieve::Slope), 0, 3) + 1 : 1;
    responseCurve(a, t, w, 30, col, [&](double f) {
      double db = 0;
      for (int k = 0; k < stages; ++k)
        db += bqDb(map[type], val(s, sieve::Cutoff), k == 0 ? val(s, sieve::Q) : 0.70710678, val(s, sieve::Gain), f, sr);
      return db;
    });
  } else if (ty == "gl.minirungs") {
    responseCurve(a, t, w, 30, col, [&](double f) { return rungsDb(s, f, sr); });
  } else if (ty == "gl.miniscorch") {
    responseCurve(a, t, w, 30, col, [&](double f) {
      return svfDb(ival(s, scorch::Type), val(s, scorch::Cutoff), std::max(0.5f, val(s, scorch::Q)), f);
    });
  } else if (ty == "gl.minivowel") {
    // Open / front pad: drag the dot.
    const auto& Pp = P;
    pui::drawGrid(w.dl, w.p0, w.p1, 4, 4, false);
    ImGui::SetCursorScreenPos(w.p0);
    ImGui::InvisibleButton("##vowelpad", ImVec2(w.w(), w.h()));
    if (ImGui::IsItemActive()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      pui::setParamValue(a, t, vowel::X, std::clamp((m.x - w.p0.x) / w.w(), 0.f, 1.f), true);
      pui::setParamValue(a, t, vowel::Y, std::clamp(1.f - (m.y - w.p0.y) / w.h(), 0.f, 1.f), true);
    }
    const char* marks[5][3] = {{"oo", "0.1", "0.1"}, {"ee", "0.1", "0.95"}, {"ah", "0.9", "0.4"}, {"eh", "0.5", "0.75"}, {"oh", "0.45", "0.2"}};
    for (auto& m : marks) {
      const float x = w.p0.x + std::stof(m[1]) * w.w(), y = w.p1.y - std::stof(m[2]) * w.h();
      ed::monoText(w.dl, ImVec2(std::min(x, w.p1.x - 16), std::max(y - 12, w.p0.y + 2)), Pp.textFaint, m[0], 10.f);
    }
    const float x = w.p0.x + val(s, vowel::X) * w.w(), y = w.p1.y - val(s, vowel::Y) * w.h();
    w.dl->AddCircleFilled(ImVec2(x, y), 9.f, theme::withAlpha(col, 0.3f));
    w.dl->AddCircleFilled(ImVec2(x, y), 5.f, col);
    ed::monoText(w.dl, ImVec2(w.p0.x + 6, w.p1.y - 14), Pp.textFaint, i18n::tr("open"), 9.f);
    ed::monoText(w.dl, ImVec2(w.p0.x + 6, w.p0.y + 4), Pp.textFaint, i18n::tr("front"), 9.f);
  } else if (ty == "gl.minitines") {
    const double f0 = std::max(1.f, val(s, tines::Freq)), fb = val(s, tines::Feedback), mix = val(s, tines::Mix);
    const double pol = ival(s, tines::Polarity) == 1 ? -1.0 : 1.0;
    responseCurve(a, t, w, 24, col, [&](double f) {
      const cplx z = std::polar(1.0, -2.0 * kPi * f / f0);
      const cplx wet = (1.0 - std::fabs(fb)) / (1.0 - fb * z);
      return 20.0 * std::log10(std::max(1e-9, std::abs((1.0 - mix) + mix * pol * wet)));
    });
  } else if (ty == "gl.minichime") {
    pui::drawFreqGrid(w.dl, w.p0, w.p1, 24, false);
    const double f0 = dsp::noteToHz(val(s, chime::Note)), bright = val(s, chime::Bright);
    const int set = ival(s, chime::Partials);
    for (int k = 0; k < 12; ++k) {
      const double r = set == 1 ? 2.0 * k + 1.0 : set == 2 ? std::pow(k + 1.0, 1.42) : k + 1.0;
      const double f = f0 * r;
      if (f > 20000) break;
      const float x = pui::freqToX(f, w.p0.x, w.w());
      const float amp = static_cast<float>(std::pow(0.15 + 0.85 * bright, k) / std::sqrt(k + 1.0));
      w.dl->AddLine(ImVec2(x, w.p1.y - 4), ImVec2(x, w.p1.y - 4 - amp * (w.h() - 12)), col, 2.f);
    }
  } else if (ty == "gl.minipress") {
    const double th = val(s, press::Threshold), ra = val(s, press::Ratio), kn = val(s, press::Knee), mk = val(s, press::Makeup);
    dynamics(a, t, w, col, -60, [&](double in) { return in + curve::pressGain(in, th, ra, kn) + mk; });
  } else if (ty == "gl.miniceiling") {
    const double in = val(s, ceiling::Input), c = val(s, ceiling::Ceiling);
    dynamics(a, t, w, col, -36, [&](double x) { return std::min(x + in, c); });
  } else if (ty == "gl.minishear") {
    const double d = dsp::dbToGain(val(s, shear::Drive)), c = dsp::dbToGain(val(s, shear::Ceiling)), so = val(s, shear::Soft);
    const double o = dsp::dbToGain(val(s, shear::Output));
    transfer(w, col, [&](double x) { return curve::shear(x * d, c, so) * o; });
  } else if (ty == "gl.minipack") {
    const double th = val(s, pack::Threshold), am = val(s, pack::Amount);
    dynamics(a, t, w, col, -60, [&](double in) {
      const double fade = std::clamp((in - (th - 60.0)) / 20.0, 0.0, 1.0);
      return in + std::clamp((th - in) * am, -40.0, 24.0) * fade;
    }, false);
  } else if (ty == "gl.minispan") {
    const double lt = val(s, span::LowThresh), lr = val(s, span::LowRatio), ht = val(s, span::HighThresh), hr = val(s, span::HighRatio);
    const double out = val(s, span::Output);
    dynamics(a, t, w, col, -72, [&](double in) {
      double g = 0;
      if (in < lt) g += std::clamp((lt - in) * lr, -60.0, 24.0);
      if (in > ht) g -= (in - ht) * (1.0 - 1.0 / std::max(1.0, hr));
      return in + g + out;
    }, false);
  } else if (ty == "gl.minilatch") {
    const double th = val(s, latch::Threshold), fl = val(s, latch::Floor);
    const bool flip = ed::bval(s, latch::Flip);
    dynamics(a, t, w, col, -84, [&](double in) { return (in > th) != flip ? in : in + fl; });
  } else if (ty == "gl.minipunch") {
    // Shaped envelope of a hit: attack emphasis and sustain change drawn on a decay.
    pui::drawGrid(w.dl, w.p0, w.p1, 8, 2, false);
    const double at = val(s, punch::Attack), su = val(s, punch::Sustain), sp = val(s, punch::Speed);
    ImVec2 dry[97], wet[97];
    for (int k = 0; k <= 96; ++k) {
      const double tt = k / 96.0;
      const double env = std::min(1.0, tt * 60.0) * std::exp(-tt * 5.0);
      const double tr = std::exp(-tt * 40.0 / sp), tail = 1.0 - std::exp(-tt * 12.0 / sp);
      const double g = std::pow(10.0, (at * 12.0 * tr + su * 12.0 * tail) / 20.0);
      const float x = w.p0.x + 6 + static_cast<float>(tt) * (w.w() - 12);
      dry[k] = ImVec2(x, w.p1.y - 6 - static_cast<float>(env * 0.45) * (w.h() - 12));
      wet[k] = ImVec2(x, w.p1.y - 6 - static_cast<float>(std::min(2.2, env * g) * 0.45) * (w.h() - 12));
    }
    w.dl->AddPolyline(dry, 97, P.textFaint, 0, 1.5f);
    w.dl->AddPolyline(wet, 97, col, 0, 2.f);
  } else if (ty == "gl.miniburn") {
    const double d = dsp::dbToGain(val(s, burn::Drive)), b = 0.5 * val(s, burn::Bias), o = dsp::dbToGain(val(s, burn::Output));
    const int type = ival(s, burn::Type);
    transfer(w, col, [&](double x) { return (curve::burn(type, x * d + b) - curve::burn(type, b)) * o; });
  } else if (ty == "gl.minibend") {
    const int sh = ival(s, bend::Shape), ed2 = ival(s, bend::Edge);
    const double am = val(s, bend::Amount), d = dsp::dbToGain(val(s, bend::Drive));
    transfer(w, col, [&](double x) { return curve::bend(sh, am, ed2, x * d); });
  } else if (ty == "gl.minicrumble") {
    const double steps = std::pow(2.0, std::clamp(val(s, crumble::Bits), 1.f, 16.f) - 1.0);
    transfer(w, col, [&](double x) { return std::round(x * steps) / steps; });
  } else if (ty == "gl.miniwarp" || ty == "gl.miniclang") {
    // One cycle of a sine through the effect (Warp) or the carrier (Clang).
    pui::drawGrid(w.dl, w.p0, w.p1, 4, 2, true);
    ImVec2 pts[129];
    const float cy = (w.p0.y + w.p1.y) * 0.5f, ay = w.h() * 0.4f;
    for (int k = 0; k <= 128; ++k) {
      const double ph = k / 128.0;
      double y;
      if (ty == "gl.miniwarp") {
        const double m = val(s, warp::Drive) * (0.8 * std::sin(2 * kPi * ph) + val(s, warp::Bias)) * 0.25;
        y = std::sin(2 * kPi * (ph + m));
      } else {
        const int sh = ival(s, clang::Shape);
        const double c = sh == 1 ? (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph) : sh == 2 ? (ph < 0.5 ? 1.0 : -1.0) : sh == 3 ? 2 * ph - 1 : std::sin(2 * kPi * ph);
        const double b = val(s, clang::Bias);
        y = (1 - b) * c + b;
      }
      pts[k] = ImVec2(w.p0.x + 6 + static_cast<float>(ph) * (w.w() - 12), cy - static_cast<float>(y) * ay);
    }
    w.dl->AddPolyline(pts, 129, theme::withAlpha(col, 0.25f), 0, 5.f);
    w.dl->AddPolyline(pts, 129, col, 0, 2.f);
    if (ty == "gl.miniclang" && ival(s, clang::Source) == 1)
      ed::monoText(w.dl, ImVec2(w.p0.x + 8, w.p0.y + 6), P.textFaint, i18n::tr("carrier: sidechain input"), 10.f);
  } else if (ty == "gl.minislide") {
    readout(a, s, info, w, slide::Shift, col, i18n::tr("every partial moves by the same Hz"));
  } else if (ty == "gl.miniinterval") {
    readout(a, s, info, w, interval::Pitch, col);
  } else if (ty == "gl.minidouble" || ty == "gl.minicrowd" || ty == "gl.minijet") {
    // Voices as delay traces over two LFO cycles, panned by height.
    pui::drawGrid(w.dl, w.p0, w.p1, 8, 2, false);
    int voices = ty == "gl.minidouble" ? ival(s, dbl::Voices) + 2 : ty == "gl.minicrowd" ? static_cast<int>(std::lround(val(s, crowd::Voices))) : 2;
    voices = std::clamp(voices, 1, 12);
    for (int v = 0; v < voices; ++v) {
      ImVec2 pts[97];
      for (int k = 0; k <= 96; ++k) {
        const double tt = k / 96.0;
        double ph = 2.0 * tt + static_cast<double>(v) / voices;
        if (ty == "gl.minicrowd") ph = tt * (0.5 + 1.5 * std::fmod(v * 0.618034, 1.0)) + v * 0.381966;
        if (ty == "gl.minijet") ph = 2.0 * tt + v * 0.5 * val(s, jet::Stereo);
        const double depth = ty == "gl.minidouble" ? 0.8 : ty == "gl.minicrowd" ? val(s, crowd::Detune) : val(s, jet::Depth);
        const float lane = (v + 0.5f) / voices;
        const float y = w.p0.y + 6 + lane * (w.h() - 12) + static_cast<float>(std::sin(2 * kPi * ph) * depth) * (w.h() - 12) / (2.f * voices);
        pts[k] = ImVec2(w.p0.x + 6 + static_cast<float>(tt) * (w.w() - 12), y);
      }
      w.dl->AddPolyline(pts, 97, theme::withAlpha(col, 0.5f + 0.5f / voices), 0, 1.6f);
    }
  } else if (ty == "gl.miniorbit") {
    static const int kStages[5] = {2, 4, 6, 8, 12};
    const int st = kStages[std::clamp(ival(s, orbit::Stages), 0, 4)];
    const double fc = val(s, orbit::Centre), mix = val(s, orbit::Mix), fb = val(s, orbit::Feedback);
    responseCurve(a, t, w, 24, col, [&](double f) {
      const double tn = std::tan(kPi * std::min(fc, sr * 0.45) / sr), ac = (tn - 1) / (tn + 1);
      const cplx z1 = std::polar(1.0, -2.0 * kPi * f / sr);
      const cplx ap = std::pow((ac + z1) / (1.0 + ac * z1), st);
      const cplx wet = ap / (1.0 - fb * ap);
      return 20.0 * std::log10(std::max(1e-9, std::abs((1.0 - mix) + mix * wet)));
    });
  } else if (ty == "gl.miniecho") {
    const double tempo = a.doc.song().tempo > 0 ? a.doc.song().tempo : 120.0;
    const double ms = ed::bval(s, echo::Sync) ? kNoteBeats[std::clamp(ival(s, echo::Steps), 0, kNoteValues - 1)] * 60000.0 / tempo
                                              : val(s, echo::Time);
    const double view = std::max(1000.0, ms * 6.5), fb = val(s, echo::Feedback);
    const bool pp = ed::bval(s, echo::PingPong);
    std::vector<std::array<double, 3>> v;
    double lvl = 1.0;
    for (int k = 1; k < 24 && lvl > 0.01; ++k, lvl *= fb) v.push_back({k * ms / view, lvl, pp ? static_cast<double>(k % 2 == 0) : 0.0});
    taps(w, col, v);
  } else if (ty == "gl.minivolley") {
    const double tl = val(s, volley::Time), tr = tl * val(s, volley::Ratio), fb = val(s, volley::Feedback), cr = val(s, volley::Cross);
    const double view = std::max(1000.0, std::max(tl, tr) * 5.5);
    std::vector<std::array<double, 3>> v;
    for (int k = 1; k < 12; ++k) {
      v.push_back({k * tl / view, std::pow(fb, k - 1) * (k == 1 ? 1.0 : 1.0 - 0.5 * cr), 0.0});
      v.push_back({k * tr / view, std::pow(fb, k - 1) * (k == 1 ? 1.0 : 1.0 - 0.5 * cr), 1.0});
    }
    taps(w, col, v);
  } else if (ty == "gl.minivault") {
    pui::drawGrid(w.dl, w.p0, w.p1, 8, 2, false);
    const double decay = val(s, vault::Decay), pre = val(s, vault::PreDelay) * 0.001, view = std::max(1.0, std::min(8.0, decay * 1.3 + pre));
    ImVec2 pts[129];
    for (int k = 0; k <= 128; ++k) {
      const double tt = view * k / 128.0;
      const double env = tt < pre ? 0.0 : std::pow(10.0, -3.0 * (tt - pre) / decay);
      pts[k] = ImVec2(w.p0.x + 6 + (w.w() - 12) * k / 128.f, w.p1.y - 6 - static_cast<float>(env) * (w.h() - 16));
    }
    w.dl->AddPolyline(pts, 129, theme::withAlpha(col, 0.25f), 0, 5.f);
    w.dl->AddPolyline(pts, 129, col, 0, 2.f);
    const double early = val(s, vault::Early);
    for (int j = 0; j < 6; ++j) {
      const double tt = pre + (7 + j * 8) * 0.001 * (0.5 + val(s, vault::Size));
      const float x = w.p0.x + 6 + static_cast<float>(tt / view) * (w.w() - 12);
      w.dl->AddLine(ImVec2(x, w.p1.y - 6), ImVec2(x, w.p1.y - 6 - static_cast<float>(early * 0.5 / (1 + j)) * (w.h() - 16)), P.textDim, 1.5f);
    }
  } else if (ty == "gl.minirewind") {
    // Segments as forward ramps (input) and the reversed playback under them.
    pui::drawGrid(w.dl, w.p0, w.p1, 4, 2, false);
    const float mid = (w.p0.y + w.p1.y) * 0.5f, segW = (w.w() - 12) / 4;
    for (int k = 0; k < 4; ++k) {
      const float x0 = w.p0.x + 6 + k * segW;
      w.dl->AddLine(ImVec2(x0, mid - 6), ImVec2(x0 + segW - 4, w.p0.y + 8), P.textFaint, 1.5f);
      if (k > 0) w.dl->AddLine(ImVec2(x0, mid + 8), ImVec2(x0 + segW - 4, w.p1.y - 6), col, 2.f);
    }
    ed::monoText(w.dl, ImVec2(w.p0.x + 8, w.p0.y + 4), P.textFaint, i18n::tr("in"), 9.f);
    ed::monoText(w.dl, ImVec2(w.p0.x + 8, w.p1.y - 14), P.textFaint, i18n::tr("out"), 9.f);
  } else if (ty == "gl.minihalt") {
    pui::drawGrid(w.dl, w.p0, w.p1, 8, 2, false);
    const double cv = val(s, halt::Curve), ts = val(s, halt::StopTime), tu = val(s, halt::StartTime);
    const double tot = ts + tu + 400.0;
    ImVec2 pts[130];
    for (int k = 0; k <= 129; ++k) {
      const double tt = tot * k / 129.0;
      double sp;
      if (tt < 100) sp = 1;
      else if (tt < 100 + ts) sp = std::pow(1.0 - (tt - 100) / ts, cv);
      else if (tt < 300 + ts) sp = 0;
      else if (tt < 300 + ts + tu) sp = std::pow((tt - 300 - ts) / tu, cv);
      else sp = 1;
      pts[k] = ImVec2(w.p0.x + 6 + (w.w() - 12) * k / 129.f, w.p1.y - 6 - static_cast<float>(sp) * (w.h() - 16));
    }
    w.dl->AddPolyline(pts, 130, col, 0, 2.f);
    ed::monoText(w.dl, ImVec2(w.p0.x + 8, w.p0.y + 4), P.textFaint, i18n::tr("speed"), 9.f);
  } else if (ty == "gl.minipulse") {
    // 16 clickable steps.
    const float gap = 3, sw = (w.w() - 12 - gap * 15) / 16, h = w.h() - 16;
    for (int k = 0; k < pulse::kSteps; ++k) {
      const ImVec2 b0(w.p0.x + 6 + k * (sw + gap), w.p0.y + 8), b1(b0.x + sw, b0.y + h);
      ImGui::SetCursorScreenPos(b0);
      ImGui::PushID(k);
      const bool on = ed::bval(s, pulse::Step0 + k);
      if (ImGui::InvisibleButton("##step", ImVec2(sw, h))) pui::setParamValue(a, t, pulse::Step0 + k, on ? 0.0 : 1.0);
      ImGui::PopID();
      const bool hov = ImGui::IsItemHovered();
      w.dl->AddRectFilled(b0, b1, on ? col : (k / 4) % 2 ? P.stepOffAlt : P.stepOff, 3.f);
      if (hov) w.dl->AddRect(b0, b1, P.text, 3.f);
    }
  }
}

// ---- layout --------------------------------------------------------------------
constexpr size_t kColumns = 5;  // knob grid columns (the suite's one rhythm)

enum class Disp { None, Short, Tall };

struct Layout {
  const char* type;
  Disp disp;
  std::vector<int> knobs;    // continuous parameters, grid order
  std::vector<int> options;  // choices and toggles, option row order
};

const std::vector<Layout>& layouts() {
  static const std::vector<Layout> v = {
      {"gl.minitrim", Disp::Short, {trim::Gain}, {trim::FlipL, trim::FlipR}},
      {"gl.minibreadth", Disp::Short, {breadth::Width, breadth::Mid, breadth::Pan, breadth::MonoBelow}, {}},
      {"gl.miniskew", Disp::Short, {skew::Delay, skew::Balance}, {skew::Side}},
      {"gl.miniweave", Disp::Short, {weave::LL, weave::RL, weave::LR, weave::RR}, {}},
      {"gl.minibands", Disp::Tall, {bands::Low, bands::Mid, bands::High, bands::LowSplit, bands::HighSplit}, {}},
      {"gl.minisieve", Disp::Tall, {sieve::Cutoff, sieve::Q, sieve::Gain}, {sieve::Type, sieve::Slope}},
      {"gl.minirungs", Disp::Tall, {rungs::Cutoff, rungs::Resonance, rungs::Drive, rungs::Mix}, {rungs::Mode}},
      {"gl.miniscorch", Disp::Tall, {scorch::Cutoff, scorch::Q, scorch::Drive, scorch::Mix}, {scorch::Type, scorch::Flavour}},
      {"gl.minivowel", Disp::Tall, {vowel::X, vowel::Y, vowel::Sharpness, vowel::Shift, vowel::Mix}, {}},
      {"gl.minitines", Disp::Tall, {tines::Freq, tines::Feedback, tines::Damp, tines::Stereo, tines::Mix}, {tines::Polarity}},
      {"gl.minichime", Disp::Short, {chime::Note, chime::Decay, chime::Bright, chime::Mix}, {chime::Partials}},
      {"gl.minipress", Disp::Tall, {press::Threshold, press::Ratio, press::Attack, press::Release, press::Knee, press::Makeup},
       {press::Detect, press::Sidechain}},
      {"gl.miniceiling", Disp::Tall, {ceiling::Input, ceiling::Ceiling, ceiling::Release}, {}},
      {"gl.minishear", Disp::Tall, {shear::Drive, shear::Ceiling, shear::Soft, shear::Output}, {}},
      {"gl.minipack", Disp::Tall, {pack::Threshold, pack::Amount, pack::Attack, pack::Release, pack::Link}, {pack::Detect}},
      {"gl.minispan", Disp::Tall, {span::LowThresh, span::LowRatio, span::HighThresh, span::HighRatio, span::Speed, span::Output}, {}},
      {"gl.minilatch", Disp::Tall, {latch::Threshold, latch::Hysteresis, latch::Attack, latch::Hold, latch::Release, latch::Floor},
       {latch::Flip, latch::Sidechain}},
      {"gl.minipunch", Disp::Short, {punch::Attack, punch::Sustain, punch::Speed, punch::Output}, {punch::Clip}},
      {"gl.miniburn", Disp::Tall, {burn::Drive, burn::Bias, burn::Tone, burn::Output, burn::Mix}, {burn::Type}},
      {"gl.minibend", Disp::Tall, {bend::Amount, bend::Drive, bend::Mix}, {bend::Shape, bend::Edge}},
      {"gl.miniwarp", Disp::Short, {warp::Drive, warp::Tone, warp::Bias, warp::Stereo, warp::Mix}, {}},
      {"gl.minicrumble", Disp::Short, {crumble::Rate, crumble::Bits, crumble::Jitter, crumble::Smooth, crumble::Dither, crumble::Mix}, {}},
      {"gl.miniclang", Disp::Short, {clang::Freq, clang::Bias, clang::Spread, clang::Mix}, {clang::Shape, clang::Source}},
      {"gl.minislide", Disp::Short, {slide::Shift, slide::Stereo, slide::Feedback, slide::Mix}, {}},
      {"gl.miniinterval", Disp::Short, {interval::Pitch, interval::Fine, interval::Grain, interval::Jitter, interval::Mix}, {}},
      {"gl.minidouble", Disp::Short, {dbl::Delay, dbl::Depth, dbl::Rate, dbl::Spread, dbl::Mix}, {dbl::Voices}},
      {"gl.minicrowd", Disp::Short, {crowd::Voices, crowd::Detune, crowd::Spread, crowd::Mix}, {crowd::Motion}},
      {"gl.minijet", Disp::Short, {jet::Delay, jet::Depth, jet::Rate, jet::Feedback, jet::Stereo, jet::Mix}, {}},
      {"gl.miniorbit", Disp::Tall, {orbit::Centre, orbit::Depth, orbit::Rate, orbit::Feedback, orbit::Stereo, orbit::Mix}, {orbit::Stages}},
      {"gl.miniecho", Disp::Short, {echo::Time, echo::Feedback, echo::Tone, echo::Duck, echo::Mix}, {echo::Steps, echo::Sync, echo::PingPong}},
      {"gl.minivolley", Disp::Short, {volley::Time, volley::Ratio, volley::Feedback, volley::Cross, volley::Tone, volley::Mix}, {}},
      {"gl.minivault", Disp::Short, {vault::Size, vault::Decay, vault::Damp, vault::PreDelay, vault::Width, vault::Early, vault::Mix}, {}},
      {"gl.minirewind", Disp::Short, {rewind::Length, rewind::Fade, rewind::Mix}, {rewind::Steps, rewind::Sync}},
      {"gl.minihalt", Disp::Short, {halt::StopTime, halt::StartTime, halt::Curve}, {halt::Run}},
      {"gl.minipulse", Disp::Short, {pulse::Attack, pulse::Release, pulse::Floor, pulse::Swing, pulse::Mix}, {pulse::Rate}},
  };
  return v;
}

const Layout* layoutFor(const std::string& type) {
  for (const auto& l : layouts())
    if (type == l.type) return &l;
  return nullptr;
}

void drawMini(App& a, const PluginTarget& t, const PluginInfo& info, const Entry& e, const Layout& lay, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState s = *sp;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  titleStrip(a, e, width);
  // Display well.
  if (lay.disp != Disp::None) {
    const float h = lay.disp == Disp::Tall ? 120.f : 84.f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + h);
    pui::displayFrame(dl, p0, p1);
    dl->PushClipRect(p0, p1, true);
    drawDisplay(a, t, s, info, e, Well{dl, p0, p1});
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 8));
  }
  // Knob grid: kColumns equal columns.
  const float cell = std::floor(width / static_cast<float>(kColumns)), rowH = pui::knobCellHeight(pui::KnobMedium) + 6.f;
  const ImVec2 g0 = ImGui::GetCursorScreenPos();
  for (size_t k = 0; k < lay.knobs.size(); ++k) {
    const ImVec2 pos(g0.x + cell * static_cast<float>(k % kColumns), g0.y + rowH * static_cast<float>(k / kColumns));
    pui::knobAt(a, t, lay.knobs[k], pos, nullptr, pui::KnobMedium, cell);
  }
  const size_t rows = (lay.knobs.size() + kColumns - 1) / kColumns;
  ImGui::SetCursorScreenPos(ImVec2(g0.x, g0.y + rowH * static_cast<float>(rows)));
  ImGui::Dummy(ImVec2(width, 2));
  // Option rows: choices as segmented selectors (two rows when long), then the toggles as pills on one row.
  for (int k : lay.options) {
    const ParamDesc& d = info.params[static_cast<size_t>(k)];
    if (d.kind != ParamKind::Choice) continue;
    ImGui::PushID(k);
    pui::caption(d.name.c_str());
    const int n = static_cast<int>(d.choices.size());
    pui::segmented(a, t, k, width, nullptr, n <= 7 ? 0 : (n + 1) / 2);
    ImGui::PopID();
  }
  bool first = true;
  for (int k : lay.options) {
    const ParamDesc& d = info.params[static_cast<size_t>(k)];
    if (d.kind != ParamKind::Toggle) continue;
    ImGui::PushID(k);
    if (!first) ImGui::SameLine(0, 6);
    pui::toggleButton(a, t, k, d.name.c_str());
    first = false;
    ImGui::PopID();
  }
  ImGui::Dummy(ImVec2(width, 4));
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginInfo* info = t.info(a.doc.song());
  if (!info) return false;
  const Entry* e = findEntry(info->type);
  const Layout* lay = e ? layoutFor(info->type) : nullptr;
  if (!lay) return false;
  drawMini(a, t, *info, *e, *lay, width);
  return true;
}

}  // namespace gl::packs::mini
