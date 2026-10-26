// Gridline — J-128 layout uniformity check. Pure geometry (no ImGui), shared
// by the editor audit (--layout-audit, LayoutAudit.h), the panel audit
// (--align-audit) and the tests (tests/test_align_check.cpp).
//
// Input: what an editor or panel drew, in logical px: frames (panels, wells,
// sections, windows), controls (knobs, sliders, buttons, ...) and text runs
// (read back from the draw lists: layout box, baseline, font size, clipped).
// Output: findings, one per problem:
//   row       knobs meant to share a row whose centres differ
//   col       knobs meant to share a column (grid) whose centres differ
//   gap       a row of equal knobs with uneven gaps
//   ctl-row   other controls of one height in a row whose centres differ
//   label     a label / value not centred on the knob it belongs to
//   baseline  text of one size side by side whose baselines differ
//   overlap   controls overlapping each other / text overlapping text
//   clip      text cut by its clip rectangle, a control outside its window
// Every distance is checked at the interface scales 100 / 150 / 200 %: the
// layout is the same in logical px and rendered at the framebuffer density,
// so `d` logical px are d * scale physical px; a finding is anything >= 1
// physical px at some scale (|d| >= 0.5 logical).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

namespace gl::align {

struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  float cx() const { return (x0 + x1) * 0.5f; }
  float cy() const { return (y0 + y1) * 0.5f; }
  float area() const { return std::max(0.0f, w()) * std::max(0.0f, h()); }
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  bool holds(const Box& o, float tol = 0.5f) const { return o.x0 >= x0 - tol && o.y0 >= y0 - tol && o.x1 <= x1 + tol && o.y1 <= y1 + tol; }
};
inline float overlapLen(float a0, float a1, float b0, float b1) { return std::min(a1, b1) - std::max(a0, b0); }

enum Kind : uint8_t { kOther = 0, kKnob, kSlider, kButton, kSelector, kField, kPad };

struct Ctl {
  Box r;
  Box clip;          // clip rectangle it was drawn in (w() <= 0: unknown)
  uint8_t kind = kOther;
  std::string what;  // for the report
};
struct Text {
  Box r;              // layout box: pen start .. end, line top .. line top + size
  Box ink;            // union of the glyph quads
  float baseline = 0;
  float size = 0;
  bool clipped = false;  // a glyph cut by the clip rectangle (or dropped by it)
  std::string str;
};
struct Frame {
  Box r;
  std::string what;
};
struct Scene {
  std::vector<Frame> frames;
  std::vector<Ctl> ctls;
  std::vector<Text> texts;
  std::vector<Box> captions;  // section captions / headers (their text is no knob's label)
};
struct Options {
  bool gaps = true;       // uneven gaps in knob rows
  bool columns = true;    // knob grids
  bool ctlRows = true;    // rows of other controls
  bool overlaps = true;     // text over text, control over control
  bool ctlOverlaps = true;  // off for app panels: their hit areas overlap on purpose (grips over strips)
  bool clipping = true;     // text cut off, control outside its window
  bool ctlClip = true;
  float tol = 0.5f;       // logical px: >= 1 physical px at 200 %
};
struct Finding {
  std::string rule;
  std::string detail;
  float off = 0;  // logical px
  Box a, b;       // the two things compared (b may be empty)
};

// Physical px at 100 / 150 / 200 % for a logical distance, as "1.0/1.5/2.0".
inline std::string physical(float d) {
  char b[48];
  std::snprintf(b, sizeof b, "%.1f/%.1f/%.1f px", static_cast<double>(std::fabs(d)), static_cast<double>(std::fabs(d) * 1.5f),
                static_cast<double>(std::fabs(d) * 2.0f));
  return b;
}

namespace detail {
inline bool isDial(const Ctl& c) {
  if (c.kind == kKnob) return true;
  if (c.kind != kOther) return false;
  const float w = c.r.w(), h = c.r.h();
  return w >= 10 && w <= 160 && std::fabs(w - h) <= 0.1f * std::max(w, h);
}
// Innermost frame holding the box's centre that is clearly larger than it (-1: none).
inline int frameOf(const std::vector<Frame>& fr, const Box& r) {
  int best = -1;
  for (size_t i = 0; i < fr.size(); ++i) {
    const Box& f = fr[i].r;
    if (!f.contains(r.cx(), r.cy()) || f.area() <= r.area() * 1.2f) continue;
    if (best < 0 || f.area() < fr[static_cast<size_t>(best)].r.area()) best = static_cast<int>(i);
  }
  return best;
}
struct Dsu {
  std::vector<int> p;
  explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) { return p[static_cast<size_t>(x)] == x ? x : p[static_cast<size_t>(x)] = find(p[static_cast<size_t>(x)]); }
  void join(int a, int b) { p[static_cast<size_t>(find(a))] = find(b); }
};
inline float median(std::vector<float> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}
// Two boxes meant to sit in one row: they overlap vertically by half the
// smaller height, and the smaller one's centre lies in the middle half of the
// larger (a small knob stacked beside a big one is not in its row).
inline bool sameRow(const Box& a, const Box& b) {
  if (overlapLen(a.x0, a.x1, b.x0, b.x1) > 0.5f) return false;  // stacked, not side by side
  const float ov = overlapLen(a.y0, a.y1, b.y0, b.y1);
  const float mh = std::min(a.h(), b.h());
  if (ov < 0.5f * mh) return false;
  const Box& big = a.h() >= b.h() ? a : b;
  const Box& small = a.h() >= b.h() ? b : a;
  return small.cy() >= big.y0 + big.h() * 0.25f && small.cy() <= big.y1 - big.h() * 0.25f;
}
inline std::string fmtBox(const Box& r) {
  char b[64];
  std::snprintf(b, sizeof b, "%.1f,%.1f %.1fx%.1f", static_cast<double>(r.x0), static_cast<double>(r.y0), static_cast<double>(r.w()),
                static_cast<double>(r.h()));
  return b;
}
inline std::string quote(const std::string& s) { return "'" + (s.size() > 24 ? s.substr(0, 24) + "..." : s) + "'"; }
}  // namespace detail

inline std::vector<Finding> check(const Scene& sc, const Options& o = {}) {
  using namespace detail;
  std::vector<Finding> out;
  auto add = [&](const char* rule, std::string det, float off, const Box& a, const Box& b) {
    out.push_back(Finding{rule, std::move(det), off, a, b});
  };
  const float tol = o.tol;

  // ---- knobs: rows, gaps, columns, labels
  std::vector<int> dials;
  for (size_t i = 0; i < sc.ctls.size(); ++i)
    if (isDial(sc.ctls[i])) dials.push_back(static_cast<int>(i));
  std::vector<int> dframe(sc.ctls.size(), -1);
  for (int i : dials) dframe[static_cast<size_t>(i)] = frameOf(sc.frames, sc.ctls[static_cast<size_t>(i)].r);
  {
    Dsu rows(sc.ctls.size());
    for (size_t a = 0; a < dials.size(); ++a)
      for (size_t b = a + 1; b < dials.size(); ++b) {
        const int i = dials[a], j = dials[b];
        if (dframe[static_cast<size_t>(i)] != dframe[static_cast<size_t>(j)]) continue;
        if (sameRow(sc.ctls[static_cast<size_t>(i)].r, sc.ctls[static_cast<size_t>(j)].r)) rows.join(i, j);
      }
    std::vector<std::vector<int>> groups(sc.ctls.size());
    for (int i : dials) groups[static_cast<size_t>(rows.find(i))].push_back(i);
    for (auto& g : groups) {
      if (g.size() < 2) continue;
      std::vector<float> cys;
      for (int i : g) cys.push_back(sc.ctls[static_cast<size_t>(i)].r.cy());
      const float med = median(cys);
      for (int i : g) {
        const Ctl& c = sc.ctls[static_cast<size_t>(i)];
        const float d = c.r.cy() - med;
        if (std::fabs(d) >= tol)
          add("row", c.what + " centre " + (d > 0 ? "below" : "above") + " its row by " + physical(d) + " (" + std::to_string(g.size()) + " knobs)",
              d, c.r, Box{});
      }
      if (!o.gaps) continue;
      // Gaps between equal knobs, left to right.
      std::vector<int> eq;
      const float w0 = median([&] {
        std::vector<float> ws;
        for (int i : g) ws.push_back(sc.ctls[static_cast<size_t>(i)].r.w());
        return ws;
      }());
      for (int i : g)
        if (std::fabs(sc.ctls[static_cast<size_t>(i)].r.w() - w0) < 1.0f) eq.push_back(i);
      if (eq.size() < 3) continue;
      std::sort(eq.begin(), eq.end(), [&](int a, int b) { return sc.ctls[static_cast<size_t>(a)].r.x0 < sc.ctls[static_cast<size_t>(b)].r.x0; });
      std::vector<float> gaps;
      for (size_t k = 1; k < eq.size(); ++k) gaps.push_back(sc.ctls[static_cast<size_t>(eq[k])].r.x0 - sc.ctls[static_cast<size_t>(eq[k - 1])].r.x1);
      const float gm = median(gaps);
      float worst = 0;
      size_t wk = 0;
      for (size_t k = 0; k < gaps.size(); ++k) {
        const float d = gaps[k] - gm;
        if (std::fabs(d) < tol || std::fabs(d) >= 0.12f * w0) continue;  // larger: deliberate grouping
        if (std::fabs(d) > std::fabs(worst)) worst = d, wk = k;
      }
      if (worst != 0)
        add("gap", "uneven gap in a row of " + std::to_string(eq.size()) + " knobs: " + physical(worst) + " off the usual " +
                       std::to_string(static_cast<int>(std::lround(gm))) + " px",
            worst, sc.ctls[static_cast<size_t>(eq[wk])].r, sc.ctls[static_cast<size_t>(eq[wk + 1])].r);
    }
  }
  if (o.columns) {
    for (size_t a = 0; a < dials.size(); ++a)
      for (size_t b = a + 1; b < dials.size(); ++b) {
        const Ctl& A = sc.ctls[static_cast<size_t>(dials[a])];
        const Ctl& B = sc.ctls[static_cast<size_t>(dials[b])];
        if (dframe[static_cast<size_t>(dials[a])] != dframe[static_cast<size_t>(dials[b])]) continue;
        if (std::fabs(A.r.w() - B.r.w()) >= 1.0f) continue;
        if (overlapLen(A.r.y0, A.r.y1, B.r.y0, B.r.y1) > 0) continue;  // same row
        const float d = B.r.cx() - A.r.cx();
        if (std::fabs(d) >= tol && std::fabs(d) < 0.25f * A.r.w())
          add("col", A.what + " / " + B.what + " stacked in a column, centres apart by " + physical(d), d, A.r, B.r);
      }
  }
  // Labels and values attached to a knob: centred on it.
  auto inControl = [&](const Text& x) {
    for (const Ctl& c : sc.ctls)
      if (!isDial(c) && c.r.contains(x.r.cx(), x.r.cy())) return true;
    for (const Box& b : sc.captions)
      if (b.contains(x.r.cx(), x.r.cy())) return true;
    return false;
  };
  std::vector<int> owner(sc.texts.size(), -1);
  for (int i : dials) {
    const Box& k = sc.ctls[static_cast<size_t>(i)].r;
    int below = -1, above = -1;
    for (size_t t = 0; t < sc.texts.size(); ++t) {
      const Text& x = sc.texts[t];
      if (x.r.cx() < k.x0 || x.r.cx() > k.x1) continue;
      if (x.size > std::max(14.0f, 0.4f * k.h())) continue;  // a title, not a label
      if (inControl(x)) continue;                             // a button's own text
      if (x.r.y0 >= k.y1 - 2 && x.r.y0 - k.y1 < 1.2f * x.size) {
        if (below < 0 || x.r.y0 < sc.texts[static_cast<size_t>(below)].r.y0) below = static_cast<int>(t);
      } else if (x.r.y1 <= k.y0 + 2 && k.y0 - x.r.y1 < 1.0f * x.size && std::fabs(x.r.cx() - k.cx()) < 0.3f * k.w()) {
        // (a caption above that is clearly off the knob's centre heads a section, not the knob)
        if (above < 0 || x.r.y1 > sc.texts[static_cast<size_t>(above)].r.y1) above = static_cast<int>(t);
      }
    }
    // The text under the first one (label under the value) belongs to the knob too.
    std::vector<int> mine;
    if (above >= 0) mine.push_back(above);
    for (int t = below; t >= 0 && mine.size() < 4;) {
      mine.push_back(t);
      const Text& x = sc.texts[static_cast<size_t>(t)];
      int next = -1;
      for (size_t u = 0; u < sc.texts.size(); ++u) {
        const Text& y = sc.texts[u];
        if (static_cast<int>(u) == t || y.r.cx() < k.x0 || y.r.cx() > k.x1 || y.r.w() > 1.5f * k.w() || inControl(y)) continue;
        if (y.r.y0 >= x.r.y1 - 1 && y.r.y0 - x.r.y1 < 0.6f * y.size && (next < 0 || y.r.y0 < sc.texts[static_cast<size_t>(next)].r.y0))
          next = static_cast<int>(u);
      }
      t = next;
    }
    for (int t : mine) {
      const Text& x = sc.texts[static_cast<size_t>(t)];
      if (owner[static_cast<size_t>(t)] >= 0) continue;
      owner[static_cast<size_t>(t)] = i;
      // Text snaps to whole logical px (Dear ImGui), so half a pixel is the best a
      // centred label can do when its width and the knob differ in parity.
      const float d = x.r.cx() - k.cx();
      if (std::fabs(d) > tol + 0.01f)
        add("label", quote(x.str) + " not centred on its knob (" + sc.ctls[static_cast<size_t>(i)].what + "): " + physical(d) + (d > 0 ? " right" : " left"),
            d, x.r, k);
    }
  }

  // ---- other controls in a row
  if (o.ctlRows) {
    std::vector<int> cs;
    for (size_t i = 0; i < sc.ctls.size(); ++i)
      if (!isDial(sc.ctls[i]) && sc.ctls[i].r.w() > 2 && sc.ctls[i].r.h() > 2 && sc.ctls[i].kind != kPad) cs.push_back(static_cast<int>(i));
    std::vector<int> cf(sc.ctls.size(), -1);
    for (int i : cs) cf[static_cast<size_t>(i)] = frameOf(sc.frames, sc.ctls[static_cast<size_t>(i)].r);
    Dsu rows(sc.ctls.size());
    for (size_t a = 0; a < cs.size(); ++a)
      for (size_t b = a + 1; b < cs.size(); ++b) {
        const Box& A = sc.ctls[static_cast<size_t>(cs[a])].r;
        const Box& B = sc.ctls[static_cast<size_t>(cs[b])].r;
        if (cf[static_cast<size_t>(cs[a])] != cf[static_cast<size_t>(cs[b])]) continue;
        if (std::fabs(A.h() - B.h()) > 0.15f * std::max(A.h(), B.h())) continue;
        if (A.h() > 60 || B.h() > 60) continue;  // tall things (pads, lists) are not row items
        if (sameRow(A, B)) rows.join(cs[a], cs[b]);
      }
    std::vector<std::vector<int>> groups(sc.ctls.size());
    for (int i : cs) groups[static_cast<size_t>(rows.find(i))].push_back(i);
    for (auto& g : groups) {
      if (g.size() < 2) continue;
      std::vector<float> cys;
      for (int i : g) cys.push_back(sc.ctls[static_cast<size_t>(i)].r.cy());
      const float med = median(cys);
      for (int i : g) {
        const Ctl& c = sc.ctls[static_cast<size_t>(i)];
        const float d = c.r.cy() - med;
        if (std::fabs(d) >= tol)
          add("ctl-row", c.what + " centre " + (d > 0 ? "below" : "above") + " its row by " + physical(d) + " (" + std::to_string(g.size()) + " controls)",
              d, c.r, Box{});
      }
    }
  }

  // ---- text baselines (same size, side by side, same frame)
  {
    const size_t n = sc.texts.size();
    std::vector<int> tf(n, -1);
    for (size_t i = 0; i < n; ++i) tf[i] = frameOf(sc.frames, sc.texts[i].r);
    // A text inside a list control (several rows tall) follows the list's own
    // pitch: it is not in a row with the items of another list beside it.
    std::vector<int> list(n, -1);
    for (size_t i = 0; i < n; ++i) {
      const Text& x = sc.texts[i];
      for (size_t k = 0; k < sc.ctls.size(); ++k) {
        const Box& b = sc.ctls[k].r;
        if (isDial(sc.ctls[k]) || b.h() < 3.5f * x.size || !b.contains(x.r.cx(), x.r.cy())) continue;
        if (list[i] < 0 || b.area() < sc.ctls[static_cast<size_t>(list[i])].r.area()) list[i] = static_cast<int>(k);
      }
    }
    Dsu rows(n);
    for (size_t a = 0; a < n; ++a)
      for (size_t b = a + 1; b < n; ++b) {
        const Text& A = sc.texts[a];
        const Text& B = sc.texts[b];
        if (tf[a] != tf[b] || std::fabs(A.size - B.size) > 0.6f) continue;
        if (list[a] != list[b] && (list[a] >= 0 || list[b] >= 0)) continue;
        if (overlapLen(A.r.x0, A.r.x1, B.r.x0, B.r.x1) > 0) continue;
        if (overlapLen(A.r.y0, A.r.y1, B.r.y0, B.r.y1) < 0.5f * A.size) continue;
        rows.join(static_cast<int>(a), static_cast<int>(b));
      }
    std::vector<std::vector<int>> groups(n);
    for (size_t i = 0; i < n; ++i) groups[static_cast<size_t>(rows.find(static_cast<int>(i)))].push_back(static_cast<int>(i));
    for (auto& g : groups) {
      if (g.size() < 2) continue;
      std::vector<float> bl;
      for (int i : g) bl.push_back(sc.texts[static_cast<size_t>(i)].baseline);
      const float med = median(bl);
      for (int i : g) {
        const Text& x = sc.texts[static_cast<size_t>(i)];
        const float d = x.baseline - med;
        if (std::fabs(d) >= tol)
          add("baseline", quote(x.str) + " baseline " + (d > 0 ? "below" : "above") + " its row by " + physical(d) + " (" + std::to_string(g.size()) +
                              " texts)",
              d, x.r, Box{});
      }
    }
  }

  // ---- overlaps
  if (o.overlaps) {
    for (size_t a = 0; a < sc.ctls.size() && o.ctlOverlaps; ++a)
      for (size_t b = a + 1; b < sc.ctls.size(); ++b) {
        const Box& A = sc.ctls[a].r;
        const Box& B = sc.ctls[b].r;
        const float ow = overlapLen(A.x0, A.x1, B.x0, B.x1), oh = overlapLen(A.y0, A.y1, B.y0, B.y1);
        if (ow < 1.0f || oh < 1.0f) continue;
        if (A.holds(B) || B.holds(A)) continue;  // a control inside another (a handle on a pad)
        add("overlap", sc.ctls[a].what + " overlaps " + sc.ctls[b].what + " by " + std::to_string(static_cast<int>(std::lround(std::min(ow, oh)))) + " px",
            std::min(ow, oh), A, B);
      }
    for (size_t a = 0; a < sc.texts.size(); ++a)
      for (size_t b = a + 1; b < sc.texts.size(); ++b) {
        const Box& A = sc.texts[a].ink;
        const Box& B = sc.texts[b].ink;
        const float ow = overlapLen(A.x0, A.x1, B.x0, B.x1), oh = overlapLen(A.y0, A.y1, B.y0, B.y1);
        if (ow < 0.5f || oh < 0.5f) continue;
        add("overlap", "text " + quote(sc.texts[a].str) + " overlaps text " + quote(sc.texts[b].str), std::min(ow, oh), A, B);
      }
  }
  // ---- clipping
  if (o.clipping) {
    for (const Text& x : sc.texts)
      if (x.clipped) add("clip", "text " + quote(x.str) + " is cut off", 0, x.ink, Box{});
    for (const Ctl& c : sc.ctls) {
      if (!o.ctlClip || c.clip.w() <= 0) continue;
      const float d = std::max({c.clip.x0 - c.r.x0, c.clip.y0 - c.r.y0, c.r.x1 - c.clip.x1, c.r.y1 - c.clip.y1});
      if (d >= 1.0f) add("clip", c.what + " sticks out of its window by " + physical(d), d, c.r, c.clip);
    }
  }
  return out;
}

inline std::string describe(const Finding& f) { return f.rule + "\t" + f.detail + "\t" + detail::fmtBox(f.a); }

// ---- regression gate (ctest): findings counted per (scope, rule), scope = an
// editor's type or a panel's window. A count above the baseline fails unless
// the allowlist names the scope (rule or "*") with a justification. Files are
// tab separated, '#' starts a comment:
//   baseline   scope <TAB> rule <TAB> count        (known findings; may only go down)
//   allowlist  scope <TAB> rule|* <TAB> why         (free-form layouts: pads, envelopes, keyboards)
// Returns one message per regression (empty: pass). An allowlist line without
// a justification is itself an error.
namespace detail {
inline std::vector<std::vector<std::string>> tsvRows(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p);
    p = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> cols;
    size_t q = 0;
    for (;;) {
      const size_t t = line.find('\t', q);
      cols.push_back(line.substr(q, t == std::string::npos ? std::string::npos : t - q));
      if (t == std::string::npos) break;
      q = t + 1;
    }
    rows.push_back(std::move(cols));
  }
  return rows;
}
}  // namespace detail

struct Count {
  std::string scope, rule;
  int n = 0;
};
// The per-(scope, rule) counts of findings, in first-seen order.
inline void tally(std::vector<Count>& counts, const std::string& scope, const std::vector<Finding>& fs) {
  for (const Finding& f : fs) {
    bool hit = false;
    for (Count& c : counts)
      if (c.scope == scope && c.rule == f.rule) ++c.n, hit = true;
    if (!hit) counts.push_back(Count{scope, f.rule, 1});
  }
}
inline std::string countsTsv(const std::vector<Count>& counts) {
  std::string s = "# scope\trule\tcount\n";
  for (const Count& c : counts) s += c.scope + "\t" + c.rule + "\t" + std::to_string(c.n) + "\n";
  return s;
}
inline std::vector<std::string> gate(const std::vector<Count>& counts, const std::string& baseline, const std::string& allowlist) {
  std::vector<std::string> bad;
  std::vector<Count> base;
  for (const auto& r : detail::tsvRows(baseline))
    if (r.size() >= 3) base.push_back(Count{r[0], r[1], std::atoi(r[2].c_str())});
  struct Allow {
    std::string scope, rule;
  };
  std::vector<Allow> allow;
  for (const auto& r : detail::tsvRows(allowlist)) {
    if (r.size() < 3 || r[2].find_first_not_of(' ') == std::string::npos) {
      bad.push_back("allowlist entry without a justification: " + (r.empty() ? std::string() : r[0]));
      continue;
    }
    allow.push_back(Allow{r[0], r[1]});
  }
  for (const Count& c : counts) {
    if (c.n <= 0) continue;
    bool allowed = false;
    for (const Allow& a : allow)
      if (a.scope == c.scope && (a.rule == "*" || a.rule == c.rule)) allowed = true;
    if (allowed) continue;
    int known = 0;
    for (const Count& b : base)
      if (b.scope == c.scope && b.rule == c.rule) known = b.n;
    if (c.n > known) bad.push_back(c.scope + ": " + std::to_string(c.n) + " " + c.rule + " findings, " + std::to_string(known) + " known");
  }
  return bad;
}

}  // namespace gl::align
