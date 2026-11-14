// Gridline — float to int conversions that used to overflow (UBSan sweep): the clamped helper the editors
// use for mouse cells, and the icon rasteriser on a near-flat edge. Run under -fsanitize=undefined these
// fail on the old code; in a plain build they check the values.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "Check.h"
#include "core/SafeCast.h"
#include "app/icons/NanoSvg.h"

namespace {

void testClamp() {
  // An editor with no mouse sees -FLT_MAX: the cell is the first, not an overflowed int.
  expect(gl::toIntClamped(-FLT_MAX / 7.5, 0, 95) == 0, "clamp: no mouse is the first cell");
  expect(gl::toIntClamped(static_cast<double>(FLT_MAX), 0, 11) == 11, "clamp: far right is the last cell");
  expect(gl::toIntClamped(-std::numeric_limits<double>::infinity(), 0, 11) == 0, "clamp: minus infinity");
  expect(gl::toIntClamped(std::numeric_limits<double>::infinity(), 0, 11) == 11, "clamp: plus infinity");
  expect(gl::toIntClamped(std::nan(""), 3, 11) == 3, "clamp: NaN gives the low end");
  expect(gl::toIntClamped(4.9, 0, 11) == 4, "clamp: in range truncates like a cast");
  expect(gl::toIntClamped(-0.5, 0, 11) == 0, "clamp: just left of the strip");
  expect(gl::toIntClamped(std::floor(-0.5), -3, 11) == -1, "clamp: a negative low end");
}

void testFlatEdge() {
  // An almost horizontal edge: its slope in 22.10 fixed point is far past INT_MAX.
  std::string svg =
      "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32' viewBox='0 0 32 32'>"
      "<path d='M2 4 L30 4.0000001 L30 28 L2 28 Z' fill='#fff'/>"
      "<path d='M0 10 L1e7 10.5 L0 11 Z' fill='#fff'/></svg>";
  std::vector<char> text(svg.begin(), svg.end());
  text.push_back('\0');
  NSVGimage* img = nsvgParse(text.data(), "px", 96.0f);
  expect(img != nullptr, "flat edge: parses");
  if (!img) return;
  NSVGrasterizer* r = nsvgCreateRasterizer();
  std::vector<unsigned char> px(32 * 32 * 4, 0);
  nsvgRasterize(r, img, 0, 0, 1.0f, px.data(), 32, 32, 32 * 4);
  expect(px[(16 * 32 + 16) * 4 + 3] > 0, "flat edge: the shape is drawn");
  nsvgDeleteRasterizer(r);
  nsvgDelete(img);
}

}  // namespace

int main() {
  testClamp();
  testFlatEdge();
  return finish();
}
