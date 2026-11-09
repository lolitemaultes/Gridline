// Gridline tests - K-147: magnetic knob detents (core/KnobMagnet.h) and the plugin window
// fit rules (core/EditorZoom.h).
//  * a drag that ends inside a magnet's capture zone lands exactly on the magnet;
//  * outside the zones the value follows the drag, so values between magnets stay reachable;
//  * the curve is continuous and never goes backwards (no jump, no flicker at a zone edge);
//  * crowded magnets share the room between them; unshape() inverts shape();
//  * the wheel steps from magnet to magnet;
//  * the fit size: a window never larger than the work area, never smaller than the floor.
#include <cmath>
#include <vector>
#include "Check.h"
#include "core/EditorZoom.h"
#include "core/KnobMagnet.h"

using namespace gl;

int main() {
  // Tracer's synced Time knob: ticks 1..768, magnets at the divisions.
  const double ticks[] = {12, 24, 32, 48, 64, 72, 96, 128, 144, 192, 256, 288, 384, 576, 768};
  std::vector<double> at;
  for (double t : ticks) at.push_back((t - 1) / 767.0);
  const double zone = magnet::kZonePx / 170.0;  // the skin wheel's drag travel at 100 %

  // 1. inside the zone (2 px of drag from 1/4) the value sits on 1/4
  const double q = (192 - 1) / 767.0;
  expect(magnet::shape(q + 2 / 170.0, at, zone) == q, "2 px past 1/4 holds on 1/4");
  expect(magnet::shape(q - 2 / 170.0, at, zone) == q, "2 px before 1/4 holds on 1/4");
  // 2. halfway between 1/4 and 1/2T the value follows the drag (free in between)
  const double mid = ((192 + 256) * 0.5 - 1) / 767.0;
  expect(std::fabs(magnet::shape(mid, at, zone) - mid) < 1e-12, "between two divisions the value is free");
  // every tick value between 200 and 250 is reachable with a drag
  bool all = true;
  for (int v = 200; v <= 250; ++v) {
    const double n = (v - 1) / 767.0;
    const double s = magnet::unshape(n, at, zone);
    if (std::fabs(magnet::shape(s, at, zone) - n) > 1e-6) all = false;
  }
  expect(all, "every value between 1/4 and 1/2T can be reached");
  // 3. continuous and monotone over the whole travel, in 0.01 px steps
  bool mono = true, cont = true;
  double prev = magnet::shape(0, at, zone);
  const int steps = 17000;
  for (int i = 1; i <= steps; ++i) {
    const double v = magnet::shape(static_cast<double>(i) / steps, at, zone);
    if (v < prev - 1e-12) mono = false;
    if (v - prev > 3.0 / steps + 1e-9) cont = false;  // the release eases off at most 2.5x the drag (no jump)
    prev = v;
  }
  expect(mono, "the shaped value never goes backwards (no flicker in and out of a zone)");
  expect(cont, "the shaped value has no jumps");
  // a pointer wobbling 0.5 px about a zone edge moves the value by less than 0.5 px of travel
  const double edge = q + zone;
  const double wob = std::fabs(magnet::shape(edge + 0.5 / 170, at, zone) - magnet::shape(edge - 0.5 / 170, at, zone));
  expect(wob <= 1.5 / 170 && wob > 0, "a wobble at the zone edge stays a small, smooth move");
  // 4. crowded magnets (1/64, 1/32, 1/16T are 1.5 px of travel apart at the low end): each still lands
  const double m24 = (24 - 1) / 767.0, m32 = (32 - 1) / 767.0;
  expect(magnet::shape(m24 + (m32 - m24) * 0.1, at, zone) == m24, "1/32 holds next to a close neighbour");
  expect(std::fabs(magnet::shape((m24 + m32) * 0.5, at, zone) - (m24 + m32) * 0.5) < 1e-12, "the midpoint of two close magnets is free");
  // 5. unshape inverts shape; on a magnet it gives the magnet itself
  expect(magnet::unshape(q, at, zone) == q, "unshape of a magnet is the magnet");
  expect(std::fabs(magnet::shape(magnet::unshape(0.3, at, zone), at, zone) - 0.3) < 1e-9, "unshape inverts shape");
  // 6. the wheel steps division to division (from between two, to the next one)
  expect(magnet::step(q, at, 1) == (256 - 1) / 767.0, "wheel up from 1/4 goes to 1/2T");
  expect(magnet::step(q, at, -1) == (144 - 1) / 767.0, "wheel down from 1/4 goes to 1/8 dotted");
  expect(magnet::step(mid, at, 1) == (256 - 1) / 767.0, "wheel up from between goes to the next division");
  expect(magnet::step(1.0, at, 1) == 1.0, "wheel up from the last division stays");
  // no magnets: the straight mapping
  expect(magnet::shape(0.37, {}, zone) == 0.37, "no magnets: free knob");
  // 7. the ease meets the straight line with the same slope at the zone edge
  const double e1 = magnet::ease(1.0), e0 = magnet::ease(0.999);
  expect(std::fabs(e1 - 1) < 1e-12 && std::fabs((e1 - e0) / 0.001 - 1) < 0.01, "the zone edge is seamless");

  // ---- plugin window fit (EditorZoom.h)
  // a window that fits at the size asked for keeps it
  expect(edzoom::fitZoom(0.85f, 800, 300, 1400, 800) == 0.85f, "fits: the size asked for");
  // too wide for the area: shrinks to fit, past 100 % and the asked size, down to the floor
  const float z = edzoom::fitZoom(1.0f, 1200, 300, 900, 800);
  expect(z <= 0.75f && 1200 * z <= 900 + 0.5f, "too wide: the window shrinks to fit the area");
  expect(edzoom::fitZoom(1.0f, 4000, 300, 900, 800) == edzoom::kFitMin, "never under the floor");
  expect(edzoom::globalSize(0) == edzoom::kGlobalDefault, "no setting: the default size");
  return finish();
}
