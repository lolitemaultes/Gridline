// Gridline - J-219: a small PDF 1.4 writer (vector drawing and text with the
// standard Helvetica fonts, WinAnsi encoding). Written for the Track Receipt
// results document; no external library. Coordinates are points from the
// top-left corner of the page (y grows down); the writer flips them.
#pragma once
#include <string>
#include <vector>

namespace gl::pdf {

struct Color {
  double r = 0, g = 0, b = 0;
};
inline Color rgb(unsigned hex) {
  return Color{((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0};
}

enum class Align { Left, Center, Right };

class Doc {
 public:
  explicit Doc(double width = 595.28, double height = 841.89);  // A4
  double width() const { return w_; }
  double height() const { return h_; }
  // Starts a new page (the first call makes page 1).
  void newPage();
  int pageCount() const { return static_cast<int>(pages_.size()); }
  // Draw on an earlier page (headers / footers after the page count is known).
  void selectPage(int index);

  void fillColor(Color c);
  void strokeColor(Color c);
  void lineWidth(double w);
  void rect(double x, double y, double w, double h, bool fill = true, bool stroke = false);
  void roundRect(double x, double y, double w, double h, double r, bool fill = true, bool stroke = false);
  void line(double x1, double y1, double x2, double y2);
  void circle(double cx, double cy, double r, bool fill = true, bool stroke = false);
  // A ring segment from angle a0 to a1 (radians, 0 = 12 o'clock, clockwise).
  void ringSegment(double cx, double cy, double r0, double r1, double a0, double a1);
  // Path building (filled with fill(), even-odd or non-zero).
  void moveTo(double x, double y);
  void lineTo(double x, double y);
  void curveTo(double x1, double y1, double x2, double y2, double x3, double y3);
  void quadTo(double cx, double cy, double x, double y);  // as a cubic
  void closePath();
  void fillPath(bool evenOdd = false);

  // Text: y is the baseline. Bold selects Helvetica-Bold.
  void text(double x, double y, const std::string& utf8, double size, bool bold = false, Align a = Align::Left);
  static double textWidth(const std::string& utf8, double size, bool bold = false);
  // Words of `utf8` broken into lines no wider than `width`.
  static std::vector<std::string> wrap(const std::string& utf8, double size, bool bold, double width);
  // The size (down to `minSize`) at which `utf8` fits `width`.
  static double fitSize(const std::string& utf8, double size, bool bold, double width, double minSize);
  // Shortens with "..." to fit `width`.
  static std::string fit(const std::string& utf8, double size, bool bold, double width);

  void setTitle(const std::string& t) { title_ = t; }
  // The finished file.
  std::string finish() const;

 private:
  std::string& cur();
  double fy(double y) const { return h_ - y; }
  double w_, h_;
  std::vector<std::string> pages_;
  int page_ = -1;
  double pathStartX_ = 0, pathStartY_ = 0, lastX_ = 0, lastY_ = 0;
  std::string title_;
};

// UTF-8 to WinAnsi bytes (Latin-1 range kept, a few typographic marks mapped, others '?').
std::string toWinAnsi(const std::string& utf8);

}  // namespace gl::pdf
