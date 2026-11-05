// Gridline - J-219: the Track Receipt results document (PDF). Gridline's own
// look: a dark banner with the Gridline mark and wordmark (vectors), white
// pages, slate text, one warm accent for the parts of the originals used.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include "PdfWriter.h"
#include "TrackReceipt.h"

namespace gl::receipt {

namespace {

using pdf::Align;
using pdf::Color;
using pdf::rgb;

// The wordmark "gridline" in Inter Bold (SIL Open Font License, assets/fonts/Inter-LICENSE.txt),
// as outlines in the units of the Gridline lockup (viewBox 520 x 120).
// ops: 0 move (x y), 1 line (x y), 2 quadratic (cx cy x y).
const float kWordmark[] = {
    0, 179.74, 114.45, 2, 169.07, 114.45, 162.57, 110.54, 2, 156.07, 106.62, 154.20, 100.53, 1, 167.30, 96.88, 2,
    168.36, 99.26, 171.21, 101.34, 2, 174.10, 103.38, 179.64, 103.38, 2, 185.43, 103.38, 188.78, 100.73, 2, 192.13,
    98.04, 192.13, 92.41, 1, 192.13, 81.95, 1, 190.96, 81.95, 2, 189.39, 85.40, 185.73, 88.29, 2, 182.12, 91.19,
    175.22, 91.19, 2, 168.82, 91.19, 163.59, 88.19, 2, 158.36, 85.20, 155.21, 79.05, 2, 152.11, 72.91, 152.11, 63.51,
    2, 152.11, 53.91, 155.26, 47.46, 2, 158.46, 41.02, 163.69, 37.77, 2, 168.97, 34.52, 175.27, 34.52, 2, 180.04,
    34.52, 183.29, 36.14, 2, 186.54, 37.77, 188.52, 40.15, 2, 190.55, 42.54, 191.57, 44.77, 1, 192.23, 44.77, 1,
    192.23, 35.23, 1, 207.21, 35.23, 1, 207.21, 91.54, 2, 207.21, 99.21, 203.66, 104.34, 2, 200.15, 109.42, 193.91,
    111.96, 2, 187.71, 114.45, 179.74, 114.45, 0, 179.99, 79.61, 2, 185.83, 79.61, 188.98, 75.34, 2, 192.18, 71.03,
    192.18, 63.41, 2, 192.18, 55.84, 189.03, 51.27, 2, 185.88, 46.70, 179.99, 46.70, 2, 174.00, 46.70, 170.85, 51.43,
    2, 167.75, 56.10, 167.75, 63.41, 2, 167.75, 70.88, 170.90, 75.24, 2, 174.05, 79.61, 179.99, 79.61, 0, 217.21,
    92.00, 1, 217.21, 35.23, 1, 231.94, 35.23, 1, 231.94, 45.13, 1, 232.55, 45.13, 2, 234.12, 39.90, 237.78, 37.21, 2,
    241.48, 34.46, 246.26, 34.46, 2, 248.85, 34.46, 251.18, 34.97, 1, 251.18, 48.58, 2, 250.17, 48.28, 248.19, 48.07,
    2, 246.26, 47.82, 244.58, 47.82, 2, 239.30, 47.82, 235.85, 51.12, 2, 232.45, 54.42, 232.45, 59.70, 1, 232.45,
    92.00, 1, 217.21, 92.00, 0, 256.56, 92.00, 1, 256.56, 35.23, 1, 271.80, 35.23, 1, 271.80, 92.00, 1, 256.56, 92.00,
    0, 264.18, 27.81, 2, 260.73, 27.81, 258.29, 25.53, 2, 255.85, 23.24, 255.85, 19.99, 2, 255.85, 16.74, 258.29,
    14.46, 2, 260.73, 12.17, 264.18, 12.17, 2, 267.63, 12.17, 270.07, 14.46, 2, 272.56, 16.74, 272.56, 19.99, 2,
    272.56, 23.24, 270.07, 25.53, 2, 267.63, 27.81, 264.18, 27.81, 0, 302.52, 92.96, 2, 295.96, 92.96, 290.73, 89.61,
    2, 285.50, 86.21, 282.41, 79.71, 2, 279.36, 73.16, 279.36, 63.71, 2, 279.36, 54.02, 282.51, 47.52, 2, 285.66,
    41.02, 290.89, 37.77, 2, 296.12, 34.52, 302.41, 34.52, 2, 307.24, 34.52, 310.44, 36.14, 2, 313.64, 37.77, 315.57,
    40.15, 2, 317.55, 42.54, 318.56, 44.77, 1, 319.07, 44.77, 1, 319.07, 16.34, 1, 334.30, 16.34, 1, 334.30, 92.00, 1,
    319.32, 92.00, 1, 319.32, 82.91, 1, 318.56, 82.91, 2, 317.50, 85.20, 315.52, 87.53, 2, 313.54, 89.82, 310.34,
    91.39, 2, 307.14, 92.96, 302.52, 92.96, 0, 307.19, 80.73, 2, 312.98, 80.73, 316.18, 76.05, 2, 319.38, 71.38,
    319.38, 63.66, 2, 319.38, 55.95, 316.18, 51.32, 2, 313.03, 46.70, 307.19, 46.70, 2, 301.20, 46.70, 298.10, 51.48,
    2, 295.00, 56.20, 295.00, 63.66, 2, 295.00, 71.18, 298.10, 75.95, 2, 301.25, 80.73, 307.19, 80.73, 0, 359.54,
    16.34, 1, 359.54, 92.00, 1, 344.30, 92.00, 1, 344.30, 16.34, 1, 359.54, 16.34, 0, 369.49, 92.00, 1, 369.49, 35.23,
    1, 384.72, 35.23, 1, 384.72, 92.00, 1, 369.49, 92.00, 0, 377.11, 27.81, 2, 373.65, 27.81, 371.21, 25.53, 2, 368.78,
    23.24, 368.78, 19.99, 2, 368.78, 16.74, 371.21, 14.46, 2, 373.65, 12.17, 377.11, 12.17, 2, 380.56, 12.17, 383.00,
    14.46, 2, 385.48, 16.74, 385.48, 19.99, 2, 385.48, 23.24, 383.00, 25.53, 2, 380.56, 27.81, 377.11, 27.81, 0,
    409.91, 59.14, 1, 409.91, 92.00, 1, 394.67, 92.00, 1, 394.67, 35.23, 1, 409.04, 35.23, 1, 409.25, 46.96, 2, 411.63,
    41.02, 416.00, 37.77, 2, 420.37, 34.52, 426.92, 34.52, 2, 435.75, 34.52, 441.09, 40.20, 2, 446.42, 45.89, 446.42,
    55.89, 1, 446.42, 92.00, 1, 431.18, 92.00, 1, 431.18, 58.54, 2, 431.18, 53.25, 428.44, 50.26, 2, 425.75, 47.26,
    420.93, 47.26, 2, 416.05, 47.26, 412.95, 50.41, 2, 409.91, 53.51, 409.91, 59.14, 0, 482.06, 93.12, 2, 473.38,
    93.12, 467.08, 89.56, 2, 460.79, 86.01, 457.38, 79.46, 2, 454.03, 72.91, 454.03, 63.92, 2, 454.03, 55.13, 457.38,
    48.53, 2, 460.79, 41.93, 466.88, 38.22, 2, 473.02, 34.52, 481.30, 34.52, 2, 488.71, 34.52, 494.76, 37.66, 2,
    500.80, 40.81, 504.36, 47.21, 2, 507.96, 53.61, 507.96, 63.31, 1, 507.96, 67.57, 1, 469.11, 67.57, 2, 469.37,
    74.53, 472.97, 78.09, 2, 476.58, 81.64, 482.32, 81.64, 2, 486.33, 81.64, 489.17, 79.96, 2, 492.07, 78.24, 493.29,
    74.94, 1, 507.10, 77.53, 2, 505.02, 84.59, 498.46, 88.85, 2, 491.96, 93.12, 482.06, 93.12, 0, 469.21, 58.03, 1,
    493.29, 58.03, 2, 492.73, 52.49, 489.73, 49.24, 2, 486.73, 45.99, 481.45, 45.99, 2, 475.97, 45.99, 472.82, 49.39,
    2, 469.67, 52.75, 469.21, 58.03,
};

// Palette (Gridline: Ink / Paper and slate greys; one accent for the used parts).
const Color kInk = rgb(0x15191C), kText = rgb(0x2B3236), kDim = rgb(0x5D676C), kFaint = rgb(0x8A9398);
const Color kLine = rgb(0xE2E6E9), kPanel = rgb(0xF5F7F8), kBanner = rgb(0x262C30), kPaper = rgb(0xFFFFFF);
const Color kAccent = rgb(0xFF9E3D), kAccentSoft = rgb(0xFFE7CF), kDanger = rgb(0xD9443F), kBar = rgb(0xE9EDEF);
const Color kYours = rgb(0x434C51);
const Color kSongs[6] = {rgb(0xFF9E3D), rgb(0xE07A2E), rgb(0xF4C27A), rgb(0xB85F24), rgb(0xFFCB96), rgb(0x8F4A1C)};

constexpr double kM = 48;  // page margin

std::string today() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
  return b;
}

std::string pct(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g%%", std::round(v * 100.0) / 100.0);
  return b;
}

// The Gridline mark (rounded square with the grid cut out) at x, y, size s (points).
void drawMark(pdf::Doc& d, double x, double y, double s, Color c) {
  const double k = s / 120.0;
  d.fillColor(c);
  const double r = 28 * k, q = 0.5523 * r;
  auto P = [&](double px, double py) { return std::make_pair(x + px * k, y + py * k); };
  // outer rounded square
  d.moveTo(x + r, y);
  d.lineTo(x + s - r, y);
  d.curveTo(x + s - r + q, y, x + s, y + r - q, x + s, y + r);
  d.lineTo(x + s, y + s - r);
  d.curveTo(x + s, y + s - r + q, x + s - r + q, y + s, x + s - r, y + s);
  d.lineTo(x + r, y + s);
  d.curveTo(x + r - q, y + s, x, y + s - r + q, x, y + s - r);
  d.lineTo(x, y + r);
  d.curveTo(x, y + r - q, x + r - q, y, x + r, y);
  d.closePath();
  const double cut[8][4] = {{35, 24, 10, 72}, {75, 24, 10, 72}, {24, 35, 11, 10}, {45, 35, 30, 10},
                            {85, 35, 11, 10}, {24, 75, 11, 10}, {45, 75, 30, 10}, {85, 75, 11, 10}};
  for (const auto& c4 : cut) {
    auto a = P(c4[0], c4[1]);
    d.moveTo(a.first, a.second);
    d.lineTo(a.first + c4[2] * k, a.second);
    d.lineTo(a.first + c4[2] * k, a.second + c4[3] * k);
    d.lineTo(a.first, a.second + c4[3] * k);
    d.closePath();
  }
  d.fillPath(true);
}

// The lockup (mark + wordmark) with its top-left at x, y and height h.
void drawLockup(pdf::Doc& d, double x, double y, double h, Color c) {
  drawMark(d, x, y, h, c);
  const double k = h / 120.0;
  d.fillColor(c);
  const size_t n = sizeof(kWordmark) / sizeof(kWordmark[0]);
  for (size_t i = 0; i < n;) {
    const int op = static_cast<int>(kWordmark[i]);
    if (op == 0) {
      d.moveTo(x + kWordmark[i + 1] * k, y + kWordmark[i + 2] * k);
      i += 3;
    } else if (op == 1) {
      d.lineTo(x + kWordmark[i + 1] * k, y + kWordmark[i + 2] * k);
      i += 3;
    } else {
      d.quadTo(x + kWordmark[i + 1] * k, y + kWordmark[i + 2] * k, x + kWordmark[i + 3] * k, y + kWordmark[i + 4] * k);
      i += 5;
    }
  }
  d.fillPath(false);
}

// Wrapped text: lines of `t` no wider than `width`, first baseline at top + size,
// `lh` apart. Draws when `d` is given; returns the height used (lines x lh).
double wrapped(pdf::Doc* d, double x, double top, const std::string& t, double size, bool bold, double width, double lh, Color c,
               Align a = Align::Left) {
  const auto lines = pdf::Doc::wrap(t, size, bold, width);
  if (d) {
    d->fillColor(c);
    for (size_t i = 0; i < lines.size(); ++i) d->text(x, top + size + lh * static_cast<double>(i), lines[i], size, bold, a);
  }
  return lh * static_cast<double>(lines.size());
}

constexpr double kTop = 84;  // content top below the page header

struct Page {
  pdf::Doc d;
  double y = 0;
  std::string project;
  bool endless = false;  // measuring: never breaks
  double bottom() const { return endless ? 1e9 : d.height() - 56; }
  double contentW() const { return d.width() - 2 * kM; }
  double usable() const { return d.height() - 56 - kTop; }
  bool atTop() const { return y <= kTop + 0.5; }

  void firstPage(const Report& r) {
    d.newPage();
    // Banner
    d.fillColor(kBanner);
    d.rect(0, 0, d.width(), 92);
    drawLockup(d, kM, 30, 32, kPaper);
    d.fillColor(rgb(0xC9CFD3));
    d.text(d.width() - kM, 44, "TRACK RECEIPT", 9, true, Align::Right);
    d.fillColor(rgb(0x8A9398));
    d.text(d.width() - kM, 58, today(), 9, false, Align::Right);
    y = 112;
    y += wrapped(&d, kM, y, r.project.empty() ? "Untitled" : r.project, 26, true, contentW(), 30, kInk);
    y += 2;
    y += wrapped(&d, kM, y, "The parts of original recordings this project uses, measured by playing it through.", 10.5, false, contentW(), 14, kDim);
    y += 18;
  }
  void pageHeader() {
    d.newPage();
    drawMark(d, kM, 30, 14, kInk);
    d.fillColor(kDim);
    d.text(kM + 22, 41, "Track Receipt", 9, true);
    const double size = pdf::Doc::fitSize(project, 9, false, 300, 6);
    const auto lines = pdf::Doc::wrap(project, size, false, 300);
    for (size_t i = 0; i < lines.size(); ++i)
      d.text(d.width() - kM, 41 - (size + 1) * static_cast<double>(lines.size() - 1 - i), lines[i], size, false, Align::Right);
    d.strokeColor(kLine);
    d.lineWidth(0.6);
    d.line(kM, 54, d.width() - kM, 54);
    y = kTop;
  }
  void need(double h) {
    if (y + h > bottom() && !atTop()) pageHeader();
  }
  // A section heading: the same space above every one (none at the top of a page),
  // kept on the page with at least `keep` points of what follows.
  void heading(const std::string& t, double keep = 60) {
    need(26 + 24 + keep);
    if (!atTop()) y += 26;
    d.fillColor(kInk);
    d.text(kM, y + 13, t, 15, true);
    y += 24;
  }
  void para(const std::string& t, double size = 10, Color c = kText, double width = 0, double x = kM) {
    if (width <= 0) width = contentW() - (x - kM);
    for (const auto& line : pdf::Doc::wrap(t, size, false, width)) {
      need(size * 1.5);
      d.fillColor(c);
      d.text(x, y + size, line, size);
      y += size * 1.45;
    }
  }
  void footers() {
    const int n = d.pageCount();
    for (int i = 0; i < n; ++i) {
      d.selectPage(i);
      d.fillColor(kFaint);
      d.text(kM, d.height() - 28, "A helper, not legal advice. Check the figures against your license on Tracklib.", 7.5);
      d.text(d.width() - kM, d.height() - 28, "Page " + std::to_string(i + 1) + " of " + std::to_string(n), 7.5, false, Align::Right);
    }
  }
};

std::string tierText(const SongRow& s) {
  if (s.seconds <= 0) return "Not played";
  if (s.over) return "Over 60 s";
  return "Up to " + std::to_string(s.tier) + " s";
}

using Runs = std::vector<std::pair<std::string, Color>>;
// Items packed into lines (", " between them), each in its own colour.
std::vector<Runs> packRuns(const Runs& items, double size, double width) {
  std::vector<Runs> lines;
  double used = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string text = items[i].first + (i + 1 < items.size() ? "," : "");
    const double w = pdf::Doc::textWidth(text, size, false);
    const double sp = pdf::Doc::textWidth(" ", size, false);
    if (lines.empty() || (used > 0 && used + sp + w > width)) {
      lines.push_back({});
      used = 0;
    }
    lines.back().push_back({(used > 0 ? " " : "") + text, items[i].second});
    used += (used > 0 ? sp : 0) + w;
  }
  return lines;
}

double tileHeight(const std::vector<Runs>& sub) { return sub.empty() ? 62 : std::max(62.0, 54 + 9.5 * static_cast<double>(sub.size()) + 8); }

void statTile(Page& w, double x, double y, double tw, double th, const std::string& label, const std::string& value, Color vc,
              const std::vector<Runs>& sub = {}) {
  w.d.fillColor(kPanel);
  w.d.roundRect(x, y, tw, th, 6);
  w.d.fillColor(kDim);
  w.d.text(x + 12, y + 20, label, 8.5, true);
  w.d.fillColor(vc);
  const double size = pdf::Doc::fitSize(value, 17, true, tw - 24, 9);
  w.d.text(x + 12, y + 44, value, size, true);
  double ly = y + 58;
  for (const Runs& line : sub) {
    double lx = x + 12;
    for (const auto& [t, c] : line) {
      w.d.fillColor(c);
      w.d.text(lx, ly, t, 7.5);
      lx += pdf::Doc::textWidth(t, 7.5, false);
    }
    ly += 9.5;
  }
}

void timelineBar(Page& w, const Timeline& t, double x, double y, double bw) {
  pdf::Doc& d = w.d;
  const double len = std::max(t.length, 0.001);
  d.fillColor(kBar);
  d.roundRect(x, y, bw, 10, 5);
  d.fillColor(kAccent);
  for (const Range& r : t.used) {
    const double a = x + bw * std::clamp(r.a / len, 0.0, 1.0), b = x + bw * std::clamp(r.b / len, 0.0, 1.0);
    d.rect(a, y, std::max(1.2, b - a), 10);
  }
  d.fillColor(kFaint);
  d.text(x, y + 21, "0:00", 7.5);
  d.text(x + bw, y + 21, timeText(t.length), 7.5, false, Align::Right);
}

// One song. On an endless page it measures; on a real page it breaks between
// rows only when the card is taller than a page (the caller keeps it whole otherwise).
void songCard(Page& w, const SongRow& s, int index) {
  pdf::Doc& d = w.d;
  const double x = kM, cw = w.contentW();
  const double lw = cw - 14 - 160;  // header text column (the seconds and the tier sit at the right)
  double y = w.y;
  // header
  double hy = y;
  hy += wrapped(&d, x + 14, hy, s.title, 13, true, lw, 16, kInk);
  hy += 1;
  hy += wrapped(&d, x + 14, hy, s.artist.empty() ? "Artist unknown" : s.artist, 9.5, false, lw, 12, kDim);
  if (!s.sourceName.empty()) hy += 1 + wrapped(&d, x + 14, hy + 1, s.sourceName, 7.5, false, lw, 9.5, kFaint);
  if (s.checkName) hy += 2 + wrapped(&d, x + 14, hy + 2, "Check artist and title: guessed from the file name.", 7.5, true, lw, 9.5, kDanger);
  const double hh = std::max(40.0, hy - y + 4);
  d.fillColor(kSongs[index % 6]);
  d.rect(x, y, 3, hh);
  d.fillColor(s.over ? kDanger : kInk);
  d.text(x + cw, y + 15, secondsText(s.seconds) + " s", 17, true, Align::Right);
  const std::string chip = tierText(s);
  const double chw = pdf::Doc::textWidth(chip, 8, true) + 16;
  d.fillColor(s.over ? kDanger : s.seconds <= 0 ? kBar : kAccentSoft);
  d.roundRect(x + cw - chw, y + 21, chw, 15, 7.5);
  d.fillColor(s.over ? kPaper : kText);
  d.text(x + cw - chw / 2, y + 31.5, chip, 8, true, Align::Center);
  w.y = y + hh + 12;
  for (const Timeline& t : s.timelines) {
    w.need(44);
    y = w.y;
    std::string label = t.part > 0 ? "Part " + std::to_string(t.part) : t.part < 0 ? "Derived files, part of the original unknown" : "Original";
    d.fillColor(kDim);
    d.text(x + 14, y + 8, label + "  " + secondsText(t.seconds) + " s used", 8.5, true);
    timelineBar(w, t, x + 14, y + 14, cw - 14);
    w.y = y + 44;
    for (const RangeFrom& rf : t.parts) {
      std::string from;
      for (const auto& f : rf.from) from += (from.empty() ? "" : "; ") + f;
      if (rf.estimated) from += " (not measured: whole file)";
      const double fh = wrapped(nullptr, 0, 0, from, 8, false, cw - 170, 10, kFaint);
      const double rh = std::max(15.0, fh + 5);
      w.need(rh);
      y = w.y;
      d.fillColor(kAccent);
      d.circle(x + 18, y + 4.5, 2.4);
      d.fillColor(kText);
      d.text(x + 26, y + 8, timeText(rf.r.a) + " - " + timeText(rf.r.b), 9, true);
      d.fillColor(kDim);
      d.text(x + 128, y + 8, secondsText(rf.r.b - rf.r.a) + " s", 9);
      wrapped(&d, x + 170, y, from, 8, false, cw - 170, 10, rf.estimated ? kDanger : kFaint);
      w.y += rh;
    }
    w.y += 4;
  }
  // stems + fields
  std::string stems;
  for (const auto& st : s.stems) stems += (stems.empty() ? "" : ", ") + st;
  const std::string usedFrom = "Used from: " + (stems.empty() ? std::string("-") : stems);
  w.need(wrapped(nullptr, 0, 0, usedFrom, 8.5, false, cw - 14, 11, kDim) + 30);
  w.y += wrapped(&d, x + 14, w.y - 0.5, usedFrom, 8.5, false, cw - 14, 11, kDim) + 5;
  y = w.y;
  const char* labels[3] = {"Category", "Share", "Fee"};
  const std::string vals[3] = {s.fields.category, s.share >= 0 ? pct(s.share) : "", s.fields.fee};
  const double fw = (cw - 14) / 3.0;
  double fh = 11;
  for (int i = 0; i < 3; ++i) {
    const double fx = x + 14 + fw * i;
    d.fillColor(kFaint);
    d.text(fx, y + 8, labels[i], 7.5, true);
    const double vh = wrapped(&d, fx + 50, y - 1, vals[i], 9, true, fw - 60, 11, kText);
    fh = std::max(fh, vh);
    d.strokeColor(kLine);
    d.lineWidth(0.6);
    d.line(fx + 48, y + fh, fx + fw - 10, y + fh);
  }
  w.y = y + fh + 9;
  for (const auto& warn : s.warnings) {
    const double wh = wrapped(nullptr, 0, 0, warn, 8.5, false, cw - 14, 12, kDanger);
    w.need(wh);
    wrapped(&d, x + 14, w.y - 1, warn, 8.5, false, cw - 14, 12, kDanger);
    w.y += wh;
  }
  w.y += 14;
  d.strokeColor(kLine);
  d.lineWidth(0.6);
  d.line(x, w.y - 8, x + cw, w.y - 8);
}

void donut(Page& w, double cx, double cy, double r, const std::vector<std::pair<std::string, double>>& parts, double yours,
           const std::string& title, bool filled) {
  pdf::Doc& d = w.d;
  const double tau = 6.283185307179586;
  if (!filled) {
    d.fillColor(kBar);
    d.ringSegment(cx, cy, r * 0.62, r, 0, tau - 1e-6);
  } else {
    double a = 0;
    if (yours > 0) {
      d.fillColor(kYours);
      d.ringSegment(cx, cy, r * 0.62, r, a, a + tau * yours / 100.0);
      a += tau * yours / 100.0;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
      if (parts[i].second <= 0) continue;
      d.fillColor(kSongs[i % 6]);
      d.ringSegment(cx, cy, r * 0.62, r, a, a + tau * parts[i].second / 100.0);
      a += tau * parts[i].second / 100.0;
    }
  }
  d.fillColor(kInk);
  d.text(cx, cy + 2, filled ? pct(yours) : "?", 15, true, Align::Center);
  d.fillColor(kDim);
  d.text(cx, cy + 14, "yours", 8, false, Align::Center);
  d.fillColor(kInk);
  d.text(cx, cy - r - 12, title, 10, true, Align::Center);
}

// A table: column starts (fractions of the width), header, rows of cells; every cell wraps.
void table(Page& w, const std::vector<double>& cols, const std::vector<std::string>& hdr, const std::vector<std::vector<std::string>>& rows,
           double size, double lh, const std::vector<Color>& rowColor = {}, const std::vector<std::vector<Color>>& cellColor = {},
           int boldCol = -1, int rightCol = -1) {
  const double cw = w.contentW();
  auto colW = [&](size_t i) { return cw * ((i + 1 < cols.size() ? cols[i + 1] : 1.0) - cols[i]) - 8; };
  auto header = [&]() {
    for (size_t i = 0; i < hdr.size(); ++i) {
      w.d.fillColor(kFaint);
      const double hx = static_cast<int>(i) == rightCol ? kM + cw : kM + cw * cols[i];
      w.d.text(hx, w.y + 10, hdr[i], 7.5, true, static_cast<int>(i) == rightCol ? Align::Right : Align::Left);
    }
    w.y += 15;
  };
  w.need(15 + lh + 10);
  header();
  for (size_t k = 0; k < rows.size(); ++k) {
    double rh = lh;
    for (size_t i = 0; i < rows[k].size() && i < cols.size(); ++i)
      rh = std::max(rh, wrapped(nullptr, 0, 0, rows[k][i], size, static_cast<int>(i) == boldCol, colW(i), lh, kText));
    rh += lh * 0.9;
    if (w.y + rh > w.bottom()) {
      w.pageHeader();
      header();
    }
    w.d.strokeColor(kLine);
    w.d.lineWidth(0.6);
    w.d.line(kM, w.y, kM + cw, w.y);
    for (size_t i = 0; i < rows[k].size() && i < cols.size(); ++i) {
      Color c = k < rowColor.size() ? rowColor[k] : kText;
      if (k < cellColor.size() && i < cellColor[k].size()) c = cellColor[k][i];
      if (static_cast<int>(i) == rightCol)
        wrapped(&w.d, kM + cw, w.y + lh * 0.45 - 1, rows[k][i], size, false, colW(i), lh, c, Align::Right);
      else
        wrapped(&w.d, kM + cw * cols[i], w.y + lh * 0.45 - 1, rows[k][i], size, static_cast<int>(i) == boldCol, colW(i), lh, c);
    }
    w.y += rh;
  }
  w.d.strokeColor(kLine);
  w.d.line(kM, w.y, kM + cw, w.y);
}

}  // namespace

std::string resultsPdf(const Report& r) {
  Page w;
  w.project = r.project.empty() ? "Untitled" : r.project;
  w.d.setTitle(w.project + " - Track Receipt");
  w.firstPage(r);
  // ---- summary tiles
  int songs = 0, over = 0;
  double secs = 0;
  std::map<int, int> tiers;
  for (const SongRow& s : r.songs) {
    if (s.seconds <= 0) continue;
    ++songs;
    secs += s.seconds;
    if (s.over) ++over;
    else ++tiers[s.tier];
  }
  Runs tierItems;
  for (const auto& [t, n] : tiers) tierItems.push_back({std::to_string(n) + " up to " + std::to_string(t) + " s", kDim});
  if (over) tierItems.push_back({std::to_string(over) + " over 60 s", kDanger});
  const double tw = (w.contentW() - 3 * 10) / 4.0;
  const auto sub = packRuns(tierItems, 7.5, tw - 24);
  const double th = tileHeight(sub);
  statTile(w, kM, w.y, tw, th, "ORIGINAL SONGS", std::to_string(songs), kInk);
  statTile(w, kM + (tw + 10), w.y, tw, th, "SECONDS USED", secondsText(secs) + " s", kInk);
  statTile(w, kM + 2 * (tw + 10), w.y, tw, th, "LICENSES", std::to_string(songs), kInk, sub);
  statTile(w, kM + 3 * (tw + 10), w.y, tw, th, "SAMPLE SHARES", r.sharesComplete ? pct(r.totalShare) : "To fill in", r.sharesComplete ? kInk : kFaint);
  w.y += th + 16;
  w.para("One license per original song covers all of its stems. The time that counts is the time of the original used: loops and "
         "repeats count once, chopped parts add up, and the same part taken from several stems counts once. A license covers up to "
         "2, 15 or 60 seconds of one song; more than 60 seconds cannot be licensed.",
         9.5, kDim);
  for (const auto& warn : r.warnings) w.para(warn, 9.5, kDanger);
  // ---- songs (each card in one piece; a card taller than a page breaks between its rows)
  w.heading("Songs sampled", 90);
  if (r.songs.empty()) w.para("No ticked files. Tick the files that are licensed samples in Track Receipt.", 10, kDim);
  int idx = 0;
  for (const SongRow& s : r.songs) {
    Page m;
    m.endless = true;
    m.d.newPage();
    m.y = 0;
    songCard(m, s, idx);
    const double h = m.y;
    if (h <= w.usable()) w.need(h);
    else w.need(120);
    songCard(w, s, idx++);
  }

  // ---- splits
  std::vector<std::pair<std::string, double>> parts;
  const bool filled = r.sharesComplete;
  for (const SongRow& s : r.songs)
    if (s.seconds > 0) parts.push_back({s.title, std::max(0.0, s.share)});
  const double yours = std::max(0.0, 100.0 - r.totalShare);
  const double lx = kM + 200, lright = w.d.width() - kM, nameW = lright - lx - 14 - 56;
  double legendH = 15;
  for (const auto& p : parts) legendH += std::max(15.0, wrapped(nullptr, 0, 0, p.first, 8.5, false, nameW, 11, kText) + 4);
  w.heading("Splits when you release", std::max(170.0, legendH) + 40);
  w.para("The share each license sets applies to both the master and the publishing of your song, so one chart shows both. With several "
         "samples the shares add up: the rest is yours. Choose each song's category in Track Receipt and these figures follow.",
         9.5, kDim);
  w.need(std::max(170.0, legendH + 30));
  const double top = w.y + 4;
  const double cy = top + 84;
  donut(w, kM + 80, cy, 62, parts, yours, "Master and publishing", filled);
  {
    double ly = top + 30;
    w.d.fillColor(kYours);
    w.d.rect(lx, ly - 7, 8, 8);
    w.d.fillColor(kText);
    w.d.text(lx + 14, ly, "You (your writers and label)", 8.5);
    w.d.text(lright, ly, filled ? pct(yours) : "-", 8.5, true, Align::Right);
    ly += 15;
    for (size_t i = 0; i < parts.size(); ++i) {
      w.d.fillColor(kSongs[i % 6]);
      w.d.rect(lx, ly - 7, 8, 8);
      const double nh = wrapped(&w.d, lx + 14, ly - 8.5, parts[i].first, 8.5, false, nameW, 11, kText);
      w.d.fillColor(kText);
      w.d.text(lright, ly, parts[i].second > 0 ? pct(parts[i].second) : "-", 8.5, true, Align::Right);
      ly += std::max(15.0, nh + 4);
    }
    w.y = std::max(cy + 76, ly);
  }
  // PRO registration
  w.heading("Registering the song with your PRO", 40);
  std::string ex = "Your writers share " + (filled ? pct(yours) : std::string("100% minus the sample shares")) + " of the publishing";
  if (filled && !parts.empty()) {
    ex += "; ";
    for (size_t i = 0; i < parts.size(); ++i) ex += (i ? ", " : "") + pct(parts[i].second) + " goes to " + parts[i].first;
  }
  ex += ". Tracklib registers each sample on its side (for a 10% sample: 90 / 10), listing only your own writers and publishers.";
  w.para(ex, 9.5);
  // Rights holders (fill in)
  w.heading("Rights holders", 60);
  w.para("Who owns each original (from your license). Share this with your distributor and co-writers.", 9, kDim);
  w.y += 4;
  {
    std::vector<std::vector<std::string>> rows;
    for (const SongRow& s : r.songs)
      if (s.seconds > 0) rows.push_back({s.title, s.fields.holders, s.share >= 0 ? pct(s.share) : "", s.fields.category});
    table(w, {0, 0.34, 0.66, 0.84}, {"Original song", "Rights holders", "Share", "Category"}, rows, 9, 11, {}, {}, 0);
  }
  // Your writers
  w.heading("Your writers and publishers", 60);
  {
    std::vector<std::vector<std::string>> rows;
    for (const auto& wr : r.saved.writers) rows.push_back({wr.name, wr.role, wr.pro, wr.ipi, wr.share.empty() ? "" : wr.share + "%"});
    while (rows.size() < 3) rows.push_back({"", "", "", "", ""});
    table(w, {0, 0.32, 0.52, 0.70, 0.86}, {"Name", "Role", "PRO", "IPI", "Share of yours"}, rows, 9, 11);
  }

  // ---- how to
  w.heading("Registering your license", 80);
  const Release& rel = r.saved.release;
  const char* steps[] = {
      "Open each original song on Tracklib and start a sample license for it. One license covers every stem of that song.",
      "Choose the length tier shown on this receipt for that song (up to 2, 15 or 60 seconds). Use the parts listed if you are asked "
      "which sections you used.",
      "Enter your song's details: title, performer or artist, a reference copy (an MP3 of at most 10 MB), the ISRC, the release date "
      "and your distributor, and the licensee (you or your company).",
      "Check the category, fee and revenue share the license shows. In Track Receipt choose the category and type the fee: the share is calculated from the category and length, and a share you type replaces it.",
      "Release only after every license is in place. If you sell or lease the beat, each buyer needs a license of their own.",
  };
  int k = 1;
  for (const char* s : steps) {
    w.need(wrapped(nullptr, 0, 0, s, 9.5, false, w.contentW() - 28, 13.8, kText) + 6);
    w.d.fillColor(kAccentSoft);
    w.d.circle(kM + 9, w.y + 8, 9);
    w.d.fillColor(kInk);
    w.d.text(kM + 9, w.y + 11.5, std::to_string(k++), 9, true, Align::Center);
    w.para(s, 9.5, kText, w.contentW() - 28, kM + 28);
    w.y += 6;
  }
  w.heading("Your release", 60);
  {
    const std::pair<const char*, std::string> rows[] = {{"Title", rel.title},       {"Performer", rel.performer}, {"Licensee", rel.licensee},
                                                        {"ISRC", rel.isrc},         {"Release date", rel.date},   {"Distributor", rel.distributor},
                                                        {"Reference copy", "MP3, at most 10 MB"}};
    for (const auto& [label, v] : rows) {
      const double vh = std::max(12.0, wrapped(nullptr, 0, 0, v, 9.5, false, w.contentW() - 110, 12, kText));
      w.need(vh + 8);
      w.d.fillColor(kFaint);
      w.d.text(kM, w.y + 12, label, 8, true);
      wrapped(&w.d, kM + 110, w.y + 2.5, v, 9.5, false, w.contentW() - 110, 12, kText);
      w.d.strokeColor(kLine);
      w.d.line(kM + 110, w.y + vh + 4, kM + w.contentW(), w.y + vh + 4);
      w.y += vh + 8;
    }
  }
  w.heading("After release", 50);
  w.para("Put the splits on the master: the sample shares go to the original rights holders, the rest to you. Register the song "
         "with your PRO with your writers' share and each original's share (see the splits above).",
         9.5);
  w.y += 4;
  std::string report = "Report your master revenue to Tracklib at least every 6 months. Publishing is paid through the PRO registration.";
  const auto dates = reportDates(rel.date, 6);
  if (!dates.empty()) {
    std::string ds;
    for (const auto& dt : dates) ds += (ds.empty() ? "" : ", ") + dt;
    report += " Report by: " + ds + ".";
  } else {
    report += " Type the release date in Track Receipt to see your report dates.";
  }
  w.para(report, 9.5);

  // ---- every use
  w.heading("Every use", 70);
  w.para("Each place a file plays, the part of the file it reads and how often it starts. File names are given in full, to find each "
         "original again.",
         9, kDim);
  w.y += 4;
  {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<Color>> colors;
    for (const FileRow& f : r.files) {
      const Color nameC = f.ticked ? kText : kFaint;
      for (int pass = 0; pass < 2; ++pass) {
        const auto& list = pass == 0 ? f.uses : f.muted;
        for (const Use& u : list) {
          std::string where = u.where;
          if (u.bar0 > 0) where += u.bar1 > u.bar0 ? ", bars " + std::to_string(static_cast<int>(u.bar0)) + "-" + std::to_string(static_cast<int>(u.bar1)) : ", bar " + std::to_string(static_cast<int>(u.bar0));
          std::string part = u.muted ? "not counted: " + (u.note.empty() ? std::string("muted") : u.note)
                             : u.estimated && !f.manual ? "not measured: whole file"
                                                        : rangesText(unite(u.ranges));
          rows.push_back({f.name, where, part, u.plays > 0 ? std::to_string(u.plays) : "-"});
          colors.push_back({nameC, nameC, u.muted ? kFaint : u.estimated ? kDanger : kText, kText});
        }
      }
      if (f.uses.empty() && f.muted.empty()) {
        rows.push_back({f.name, "never plays", "", "-"});
        colors.push_back({kFaint, kFaint, kFaint, kFaint});
      }
    }
    table(w, {0, 0.36, 0.64, 0.92}, {"File", "Where", "Part of the file", "Starts"}, rows, 7, 8.5, {}, colors, -1, 3);
  }
  w.footers();
  return w.d.finish();
}

}  // namespace gl::receipt
