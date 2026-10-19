// Gridline — FLP import: third-wave decoders (plugins whose shipped presets
// had no decoder yet: VFX note effects, Kepler Exo, Transmitter, …).
#include <cmath>
#include <memory>
#include "FlpDec3.h"

namespace gl::flp {

namespace dec3 {

void linkAll(PluginMapping& m, std::vector<Link> links, std::vector<RawRange> ranges) {
  gen2::linkParams(m, std::move(links));
  auto table = std::make_shared<std::map<int, RawRange>>();
  for (const auto& r : ranges) (*table)[r.fl] = r;
  m.rawNorm = [table](int fl, int32_t raw) {
    auto it = table->find(fl);
    if (it == table->end() || it->second.hi == it->second.lo) return std::nan("");
    double n = (raw - it->second.lo) / (it->second.hi - it->second.lo);
    return n < 0 ? 0.0 : (n > 1 ? 1.0 : n);
  };
}

void applyRows(PluginMapping& m, const std::vector<Row>& rows, const std::vector<double>& flByTag) {
  std::vector<Link> links;
  std::vector<RawRange> ranges;
  for (const Row& r : rows) {
    if (r.fl >= 0 && static_cast<size_t>(r.fl) < flByTag.size() && r.gl >= 0) {
      double v = flByTag[static_cast<size_t>(r.fl)];
      gen2::set(m, r.gl, r.conv ? r.conv(v) : v);
    }
    if (r.gl < 0) continue;
    auto conv = r.conv;
    double lo = r.lo, hi = r.hi;
    links.push_back({r.fl, r.gl, [conv, lo, hi](double n) {
                       double v = lo + n * (hi - lo);
                       return conv ? conv(v) : v;
                     }});
    ranges.push_back({r.fl, lo, hi});
  }
  linkAll(m, std::move(links), std::move(ranges));
}

}  // namespace dec3

void registerDecoders3(std::map<std::string, PluginDecoder>& d) {
  dec3::registerVfx(d);
  dec3::registerKepler(d);
  dec3::registerFx(d);
  dec3::registerInst(d);
}

}  // namespace gl::flp
