// Gridline — "synths" pack: Matrix FM drawn data (text form, defaults, curve
// evaluation). Used by the runtime, the editor and the FLP importer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "matrixfm_data.h"

namespace gl::packs::synths::mfm {

Curve defaultEnvelope() {
  Curve c;
  c.pts = {{0.f, 0.f, 0.f}, {0.004f, 1.f, 0.f}, {0.4f, 0.7f, 0.4f}, {0.25f, 0.f, 0.4f}};
  c.sustain = 2;
  c.decay = 1;
  return c;
}

Curve defaultLfo() {
  Curve c;
  // Sine-like cycle: centre → top → centre → bottom → centre.
  c.pts = {{0.f, 0.5f, 0.f}, {0.25f, 1.f, -0.5f}, {0.25f, 0.5f, 0.5f}, {0.25f, 0.f, -0.5f}, {0.25f, 0.5f, 0.5f}};
  return c;
}

PatchData::PatchData() {
  for (auto& l : level) l.fill(0.f);
  for (auto& p : phase) p.fill(0.f);
  for (auto& l : level) l[0] = 1.f;
  for (auto& u : arts)
    for (auto& a : u) {
      a.env = defaultEnvelope();
      a.lfo = defaultLfo();
    }
}

double tensionShape(double t, double tension) {
  t = std::clamp(t, 0.0, 1.0);
  tension = std::clamp(tension, -1.0, 1.0);
  if (std::fabs(tension) < 1e-6) return t;
  // Positive tension: slow start, fast finish; negative: the reverse.
  const double e = std::pow(2.0, 4.0 * tension);
  return std::pow(t, e);
}

double curveLength(const Curve& c) {
  double x = 0;
  for (size_t i = 1; i < c.pts.size(); ++i) x += std::max(0.f, c.pts[i].dt);
  return x;
}

double curveAt(const Curve& c, double x) {
  if (c.pts.empty()) return 0.0;
  double x0 = 0, y0 = c.pts[0].y;
  if (x <= 0) return y0;
  for (size_t i = 1; i < c.pts.size(); ++i) {
    const Pt& p = c.pts[i];
    const double x1 = x0 + std::max(0.f, p.dt);
    if (x < x1) {
      const double t = p.dt > 0 ? (x - x0) / p.dt : 1.0;
      return y0 + (p.y - y0) * tensionShape(t, p.tension);
    }
    x0 = x1;
    y0 = p.y;
  }
  return y0;
}

namespace {

bool sameCurve(const Curve& a, const Curve& b) {
  if (a.pts.size() != b.pts.size() || a.sustain != b.sustain || a.loopStart != b.loopStart || a.loopEnd != b.loopEnd || a.decay != b.decay ||
      a.tempo != b.tempo)
    return false;
  for (size_t i = 0; i < a.pts.size(); ++i)
    if (std::fabs(a.pts[i].dt - b.pts[i].dt) > 1e-7f || std::fabs(a.pts[i].y - b.pts[i].y) > 1e-7f ||
        std::fabs(a.pts[i].tension - b.pts[i].tension) > 1e-7f)
      return false;
  return true;
}

void writePts(std::ostringstream& o, const Curve& c) {
  char buf[96];
  bool first = true;
  for (const Pt& p : c.pts) {
    std::snprintf(buf, sizeof buf, "%s%.6g,%.6g,%.4g", first ? "" : ";", static_cast<double>(p.dt), static_cast<double>(p.y),
                  static_cast<double>(p.tension));
    o << buf;
    first = false;
  }
}

bool readPts(const std::string& s, Curve& c) {
  c.pts.clear();
  size_t i = 0;
  while (i < s.size()) {
    size_t e = s.find(';', i);
    if (e == std::string::npos) e = s.size();
    const std::string item = s.substr(i, e - i);
    Pt p;
    char* end = nullptr;
    p.dt = std::strtof(item.c_str(), &end);
    if (end && *end == ',') p.y = std::strtof(end + 1, &end);
    if (end && *end == ',') p.tension = std::strtof(end + 1, &end);
    if (!std::isfinite(p.dt) || !std::isfinite(p.y) || !std::isfinite(p.tension)) return false;
    p.dt = std::clamp(p.dt, 0.f, 1000.f);
    p.y = std::clamp(p.y, -1.f, 2.f);
    p.tension = std::clamp(p.tension, -1.f, 1.f);
    if (c.pts.size() < 4096) c.pts.push_back(p);  // as many as the FL import keeps (J-39)
    i = e + 1;
  }
  return !c.pts.empty();
}

bool validUnitArt(int u, int a) { return u >= 0 && u < kUnits && a >= 0 && a < unitArts(u); }

}  // namespace

bool parsePatch(const std::string& text, PatchData& out) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("mfm", 0) != 0) return false;
  while (std::getline(in, line)) {
    if (line.size() < 2) continue;
    std::istringstream ls(line);
    char kind = 0;
    ls >> kind;
    if (kind == 'h') {
      int k = -1;
      ls >> k;
      if (k < 0 || k >= kOps) continue;
      out.level[static_cast<size_t>(k)].fill(0.f);
      out.phase[static_cast<size_t>(k)].fill(0.f);
      std::string item;
      while (ls >> item) {
        int h = -1;
        float lv = 0, ph = 0;
        if (std::sscanf(item.c_str(), "%d:%f:%f", &h, &lv, &ph) >= 2 && h >= 0 && h < kHarmonics && std::isfinite(lv) && std::isfinite(ph)) {
          out.level[static_cast<size_t>(k)][static_cast<size_t>(h)] = std::clamp(lv, 0.f, 1.f);
          out.phase[static_cast<size_t>(k)][static_cast<size_t>(h)] = ph - std::floor(ph);
        }
      }
    } else if (kind == 'e' || kind == 'l') {
      int u = -1, a = -1, sus = -1, ls0 = -1, le = -1;
      std::string pts;
      if (kind == 'e') ls >> u >> a >> sus >> ls0 >> le >> pts;
      else ls >> u >> a >> pts;
      if (!validUnitArt(u, a)) continue;
      Curve c;
      if (!readPts(pts, c)) continue;
      const int n = static_cast<int>(c.pts.size());
      c.sustain = sus >= 0 && sus < n ? sus : -1;
      c.loopStart = ls0 >= 0 && ls0 < n ? ls0 : -1;
      c.loopEnd = le >= 0 && le < n && c.loopStart >= 0 && le > c.loopStart ? le : -1;
      if (c.loopEnd < 0) c.loopStart = -1;
      ArtData& ad = out.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
      // Optional key=value extras (older data has none).
      std::string kv;
      while (ls >> kv) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        const float v = std::strtof(kv.c_str() + eq + 1, nullptr);
        if (!std::isfinite(v)) continue;
        if (kind == 'e' && key == "dec") c.decay = static_cast<int>(v) >= 0 && static_cast<int>(v) < n ? static_cast<int>(v) : -1;
        else if (kind == 'e' && key == "tempo") c.tempo = v != 0;
        else if (kind == 'l' && key == "ph") ad.lfoPhase = v - std::floor(v);
        else if (kind == 'l' && key == "tn") ad.lfoTension = std::clamp(v, -1.f, 1.f);
        else if (kind == 'l' && key == "pw") ad.lfoPw = std::clamp(v, -1.f, 1.f);
      }
      (kind == 'e' ? ad.env : ad.lfo) = c;
    } else if (kind == 'm') {
      int u = -1, a = -1, m = -1;
      std::string pts;
      ls >> u >> a >> m >> pts;
      if (!validUnitArt(u, a) || m < 0 || m >= kMaps) continue;
      Curve c;
      if (readPts(pts, c)) out.arts[static_cast<size_t>(u)][static_cast<size_t>(a)].maps[static_cast<size_t>(m)] = c;
    } else if (kind == 'w') {
      int j = -1;
      std::string pts;
      ls >> j >> pts;
      if (j < 0 || j >= kFilters) continue;
      Curve c;
      if (readPts(pts, c)) out.ws[static_cast<size_t>(j)] = c;
    }
  }
  return true;
}

std::string serializePatch(const PatchData& d) {
  std::ostringstream o;
  o << "mfm 1\n";
  const PatchData def;
  char buf[64];
  for (int k = 0; k < kOps; ++k) {
    if (d.level[static_cast<size_t>(k)] == def.level[0] && d.phase[static_cast<size_t>(k)] == def.phase[0]) continue;
    o << "h " << k;
    for (int h = 0; h < kHarmonics; ++h) {
      const float lv = d.level[static_cast<size_t>(k)][static_cast<size_t>(h)], ph = d.phase[static_cast<size_t>(k)][static_cast<size_t>(h)];
      if (lv == 0.f && ph == 0.f) continue;
      std::snprintf(buf, sizeof buf, " %d:%.6g:%.5g", h, static_cast<double>(lv), static_cast<double>(ph));
      o << buf;
    }
    o << "\n";
  }
  const Curve env0 = defaultEnvelope(), lfo0 = defaultLfo();
  for (int u = 0; u < kUnits; ++u)
    for (int a = 0; a < unitArts(u); ++a) {
      const ArtData& ad = d.arts[static_cast<size_t>(u)][static_cast<size_t>(a)];
      if (!sameCurve(ad.env, env0) && !ad.env.empty()) {
        o << "e " << u << ' ' << a << ' ' << ad.env.sustain << ' ' << ad.env.loopStart << ' ' << ad.env.loopEnd << ' ';
        writePts(o, ad.env);
        if (ad.env.decay >= 0) o << " dec=" << ad.env.decay;
        if (ad.env.tempo) o << " tempo=1";
        o << "\n";
      }
      const bool lfoExtras = ad.lfoPhase != 0.f || ad.lfoTension != 0.f || ad.lfoPw != 0.f;
      if ((!sameCurve(ad.lfo, lfo0) || lfoExtras) && !ad.lfo.empty()) {
        o << "l " << u << ' ' << a << ' ';
        writePts(o, ad.lfo);
        if (ad.lfoPhase != 0.f) std::snprintf(buf, sizeof buf, " ph=%.6g", static_cast<double>(ad.lfoPhase)), o << buf;
        if (ad.lfoTension != 0.f) std::snprintf(buf, sizeof buf, " tn=%.4g", static_cast<double>(ad.lfoTension)), o << buf;
        if (ad.lfoPw != 0.f) std::snprintf(buf, sizeof buf, " pw=%.4g", static_cast<double>(ad.lfoPw)), o << buf;
        o << "\n";
      }
      for (int m = 0; m < kMaps; ++m) {
        const Curve& c = ad.maps[static_cast<size_t>(m)];
        if (c.empty()) continue;
        o << "m " << u << ' ' << a << ' ' << m << ' ';
        writePts(o, c);
        o << "\n";
      }
    }
  for (int j = 0; j < kFilters; ++j) {
    if (d.ws[static_cast<size_t>(j)].empty()) continue;
    o << "w " << j << ' ';
    writePts(o, d.ws[static_cast<size_t>(j)]);
    o << "\n";
  }
  return o.str();
}

}  // namespace gl::packs::synths::mfm
