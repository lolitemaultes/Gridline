// Gridline - J-219: small PDF writer (see PdfWriter.h).
#include "PdfWriter.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>

namespace gl::pdf {

namespace {

// Advance widths (1/1000 em) of the standard Helvetica fonts for 32..126.
const short kHelv[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
                         556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
                         722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
                         278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
                         556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
const short kHelvB[95] = {278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
                          556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778,
                          722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333,
                          278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
                          611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

std::string num(double v) {
  if (std::fabs(v) < 1e-9) v = 0;
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  std::string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? "0" : s;
}

std::string escape(const std::string& bytes) {
  std::string o;
  for (char c : bytes) {
    if (c == '(' || c == ')' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

}  // namespace

std::string toWinAnsi(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 6 && i + 1 < s.size()) cp = ((c & 31u) << 6) | (static_cast<unsigned char>(s[i + 1]) & 63u), len = 2;
    else if ((c >> 4) == 14 && i + 2 < s.size())
      cp = ((c & 15u) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 63u) << 6) | (static_cast<unsigned char>(s[i + 2]) & 63u), len = 3;
    else if ((c >> 3) == 30) cp = '?', len = 4;
    else cp = '?';
    i += static_cast<size_t>(len);
    if (cp < 0x20) cp = ' ';
    if (cp == 0x2022) out += static_cast<char>(0x95);  // bullet
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2026) out += "...";
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp < 0x7F || (cp >= 0xA0 && cp <= 0xFF)) out += static_cast<char>(cp);
    else out += '?';
  }
  return out;
}

Doc::Doc(double width, double height) : w_(width), h_(height) {}

void Doc::newPage() {
  pages_.emplace_back();
  page_ = static_cast<int>(pages_.size()) - 1;
}

void Doc::selectPage(int index) {
  if (index >= 0 && index < pageCount()) page_ = index;
}

std::string& Doc::cur() {
  if (pages_.empty()) newPage();
  return pages_[static_cast<size_t>(page_)];
}

void Doc::fillColor(Color c) { cur() += num(c.r) + " " + num(c.g) + " " + num(c.b) + " rg\n"; }
void Doc::strokeColor(Color c) { cur() += num(c.r) + " " + num(c.g) + " " + num(c.b) + " RG\n"; }
void Doc::lineWidth(double w) { cur() += num(w) + " w\n"; }

void Doc::rect(double x, double y, double w, double h, bool fill, bool stroke) {
  cur() += num(x) + " " + num(fy(y + h)) + " " + num(w) + " " + num(h) + " re " + (fill && stroke ? "B" : fill ? "f" : "S") + "\n";
}

void Doc::roundRect(double x, double y, double w, double h, double r, bool fill, bool stroke) {
  r = std::min(r, std::min(w, h) / 2);
  const double k = 0.5523 * r;
  moveTo(x + r, y);
  lineTo(x + w - r, y);
  curveTo(x + w - r + k, y, x + w, y + r - k, x + w, y + r);
  lineTo(x + w, y + h - r);
  curveTo(x + w, y + h - r + k, x + w - r + k, y + h, x + w - r, y + h);
  lineTo(x + r, y + h);
  curveTo(x + r - k, y + h, x, y + h - r + k, x, y + h - r);
  lineTo(x, y + r);
  curveTo(x, y + r - k, x + r - k, y, x + r, y);
  cur() += fill && stroke ? "h B\n" : fill ? "h f\n" : "h S\n";
}

void Doc::line(double x1, double y1, double x2, double y2) {
  cur() += num(x1) + " " + num(fy(y1)) + " m " + num(x2) + " " + num(fy(y2)) + " l S\n";
}

void Doc::circle(double cx, double cy, double r, bool fill, bool stroke) {
  const double k = 0.5523 * r;
  moveTo(cx, cy - r);
  curveTo(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
  curveTo(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
  curveTo(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
  curveTo(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
  cur() += fill && stroke ? "h B\n" : fill ? "h f\n" : "h S\n";
}

void Doc::ringSegment(double cx, double cy, double r0, double r1, double a0, double a1) {
  if (a1 <= a0) return;
  auto pt = [&](double r, double a, double& x, double& y) {
    x = cx + r * std::sin(a);
    y = cy - r * std::cos(a);
  };
  // Arcs in pieces of at most 90 degrees (cubic approximation).
  const int n = std::max(1, static_cast<int>(std::ceil((a1 - a0) / (3.14159265358979 / 2))));
  const double step = (a1 - a0) / n;
  auto arc = [&](double r, double from, double to, bool first) {
    const int m = n;
    const double st = (to - from) / m;
    for (int i = 0; i < m; ++i) {
      const double s0 = from + st * i, s1 = s0 + st;
      const double k = 4.0 / 3.0 * std::tan((s1 - s0) / 4.0) * r;
      double x0, y0, x3, y3;
      pt(r, s0, x0, y0);
      pt(r, s1, x3, y3);
      if (first && i == 0) moveTo(x0, y0);
      // tangent directions (clockwise in screen space, y down)
      const double x1 = x0 + k * std::cos(s0), y1 = y0 + k * std::sin(s0);
      const double x2 = x3 - k * std::cos(s1), y2 = y3 - k * std::sin(s1);
      curveTo(x1, y1, x2, y2, x3, y3);
    }
  };
  (void)step;
  arc(r1, a0, a1, true);
  double x, y;
  pt(r0, a1, x, y);
  lineTo(x, y);
  arc(r0, a1, a0, false);
  closePath();
  fillPath();
}

void Doc::moveTo(double x, double y) {
  cur() += num(x) + " " + num(fy(y)) + " m\n";
  pathStartX_ = lastX_ = x, pathStartY_ = lastY_ = y;
}
void Doc::lineTo(double x, double y) {
  cur() += num(x) + " " + num(fy(y)) + " l\n";
  lastX_ = x, lastY_ = y;
}
void Doc::curveTo(double x1, double y1, double x2, double y2, double x3, double y3) {
  cur() += num(x1) + " " + num(fy(y1)) + " " + num(x2) + " " + num(fy(y2)) + " " + num(x3) + " " + num(fy(y3)) + " c\n";
  lastX_ = x3, lastY_ = y3;
}
void Doc::quadTo(double cx, double cy, double x, double y) {
  curveTo(lastX_ + 2.0 / 3.0 * (cx - lastX_), lastY_ + 2.0 / 3.0 * (cy - lastY_), x + 2.0 / 3.0 * (cx - x), y + 2.0 / 3.0 * (cy - y), x, y);
}
void Doc::closePath() {
  cur() += "h\n";
  lastX_ = pathStartX_, lastY_ = pathStartY_;
}
void Doc::fillPath(bool evenOdd) { cur() += evenOdd ? "f*\n" : "f\n"; }

double Doc::textWidth(const std::string& utf8, double size, bool bold) {
  const std::string b = toWinAnsi(utf8);
  double w = 0;
  for (char ch : b) {
    const auto c = static_cast<unsigned char>(ch);
    w += c >= 32 && c <= 126 ? (bold ? kHelvB : kHelv)[c - 32] : 556;
  }
  return w * size / 1000.0;
}

void Doc::text(double x, double y, const std::string& utf8, double size, bool bold, Align a) {
  const double w = textWidth(utf8, size, bold);
  if (a == Align::Center) x -= w / 2;
  if (a == Align::Right) x -= w;
  cur() += "BT /" + std::string(bold ? "F2 " : "F1 ") + num(size) + " Tf " + num(x) + " " + num(fy(y)) + " Td (" + escape(toWinAnsi(utf8)) +
           ") Tj ET\n";
}

std::vector<std::string> Doc::wrap(const std::string& utf8, double size, bool bold, double width) {
  std::vector<std::string> lines;
  std::stringstream ss(utf8);
  std::string word, line;
  while (ss >> word) {
    const std::string next = line.empty() ? word : line + " " + word;
    if (!line.empty() && textWidth(next, size, bold) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = next;
    }
    // A word wider than a line (a long file name): broken, after a "-", "_" or "." when there is one.
    while (textWidth(line, size, bold) > width && line.size() > 1) {
      size_t cut = 0, soft = 0;
      for (size_t i = 1; i <= line.size(); ++i) {
        if (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) continue;  // whole UTF-8 characters
        if (textWidth(line.substr(0, i), size, bold) > width) break;
        cut = i;
        const char c = line[i - 1];
        if (c == '-' || c == '_' || c == '.' || c == ' ' || c == '/') soft = i;
      }
      if (cut == 0) cut = 1;
      if (soft > cut / 2) cut = soft;
      lines.push_back(line.substr(0, cut));
      line = line.substr(cut);
      while (!line.empty() && line.front() == ' ') line.erase(line.begin());
    }
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

double Doc::fitSize(const std::string& utf8, double size, bool bold, double width, double minSize) {
  const double w = textWidth(utf8, size, bold);
  if (w <= width || w <= 0) return size;
  return std::max(minSize, size * width / w);
}

std::string Doc::fit(const std::string& utf8, double size, bool bold, double width) {
  if (textWidth(utf8, size, bold) <= width) return utf8;
  std::string s = utf8;
  while (!s.empty() && textWidth(s + "...", size, bold) > width) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // whole UTF-8 characters
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
  }
  return s + "...";
}

std::string Doc::finish() const {
  std::vector<std::string> obj;  // object bodies, 1-based ids
  const int nPages = std::max(1, static_cast<int>(pages_.size()));
  // 1 catalog, 2 pages, 3 F1, 4 F2, 5 info, then per page: page, contents
  obj.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  std::string kids;
  for (int i = 0; i < nPages; ++i) kids += std::to_string(6 + 2 * i) + " 0 R ";
  obj.push_back("<< /Type /Pages /Count " + std::to_string(nPages) + " /Kids [" + kids + "] >>");
  obj.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  obj.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
  obj.push_back("<< /Producer (Gridline) /Creator (Gridline Track Receipt) /Title (" + escape(toWinAnsi(title_)) + ") >>");
  for (int i = 0; i < nPages; ++i) {
    obj.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(w_) + " " + num(h_) +
                  "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " + std::to_string(7 + 2 * i) + " 0 R >>");
    const std::string body = i < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(i)] : std::string();
    obj.push_back("<< /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "endstream");
  }
  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offs;
  for (size_t i = 0; i < obj.size(); ++i) {
    offs.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + obj[i] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(obj.size() + 1) + "\n0000000000 65535 f \n";
  for (size_t o : offs) {
    char b[24];
    std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
    out += b;
  }
  out += "trailer\n<< /Size " + std::to_string(obj.size() + 1) + " /Root 1 0 R /Info 5 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return out;
}

}  // namespace gl::pdf
