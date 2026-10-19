// Gridline — Resynth (gl.harmor): drawn data (text), curve evaluation and the
// image builders (audio resynthesis, pictures). UI-thread code except curveAt
// / shapeAt*, which the runtime uses on prepared copies.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "harmor.h"
#include "models.h"

namespace gl::packs::models::hm {

namespace {

constexpr double kPi = 3.14159265358979323846;

Curve curveOf(std::initializer_list<Pt> pts, int sus = -1) {
  Curve c;
  c.pts = pts;
  c.sustain = sus;
  return c;
}

bool sameCurve(const Curve& a, const Curve& b) {
  if (a.pts.size() != b.pts.size() || a.sustain != b.sustain || a.loopStart != b.loopStart || a.decay != b.decay ||
      a.enabled != b.enabled || a.tempo != b.tempo || a.global != b.global)
    return false;
  for (size_t i = 0; i < a.pts.size(); ++i)
    if (std::fabs(a.pts[i].dt - b.pts[i].dt) > 1e-6f || std::fabs(a.pts[i].y - b.pts[i].y) > 1e-6f ||
        std::fabs(a.pts[i].tension - b.pts[i].tension) > 1e-6f || a.pts[i].kind != b.pts[i].kind)
      return false;
  return true;
}

void writePts(std::ostringstream& o, const Curve& c) {
  o << ' ';
  if (c.pts.empty()) o << '-';
  char buf[80];
  for (size_t i = 0; i < c.pts.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s%.6g,%.6g,%.4g,%d", i ? ";" : "", static_cast<double>(c.pts[i].dt), static_cast<double>(c.pts[i].y),
                  static_cast<double>(c.pts[i].tension), static_cast<int>(c.pts[i].kind));
    o << buf;
  }
}

bool readPts(const std::string& s, Curve& c) {
  c.pts.clear();
  if (s == "-") return true;
  std::istringstream in(s);
  std::string tok;
  while (std::getline(in, tok, ';')) {
    Pt p;
    int kind = 0;
    if (std::sscanf(tok.c_str(), "%f,%f,%f,%d", &p.dt, &p.y, &p.tension, &kind) < 2) return false;
    if (!std::isfinite(p.dt) || !std::isfinite(p.y) || !std::isfinite(p.tension)) return false;
    p.dt = std::max(0.0f, p.dt);
    p.y = std::clamp(p.y, -4.0f, 4.0f);
    p.tension = std::clamp(p.tension, -1.0f, 1.0f);
    p.kind = static_cast<uint8_t>(std::clamp(kind, 0, 12));
    c.pts.push_back(p);
    if (c.pts.size() > 4096) return false;
  }
  return true;
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const uint8_t* p, size_t n) {
  std::string s;
  s.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = static_cast<uint32_t>(p[i]) << 16;
    if (i + 1 < n) v |= static_cast<uint32_t>(p[i + 1]) << 8;
    if (i + 2 < n) v |= p[i + 2];
    s += kB64[(v >> 18) & 63];
    s += kB64[(v >> 12) & 63];
    s += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    s += i + 2 < n ? kB64[v & 63] : '=';
  }
  return s;
}

std::vector<uint8_t> unb64(const std::string& s) {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int bits = 0;
  for (char ch : s) {
    const char* f = std::strchr(kB64, ch);
    if (!f || ch == '\0') continue;
    acc = (acc << 6) | static_cast<uint32_t>(f - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace

bool artBipolarDefault(int art) {
  switch (art) {
    case ArPan: case ArFxMix: case ArF1Freq: case ArF2Freq: case ArF1ResOfs: case ArF2ResOfs: case ArFMix: case ArPhOfs:
    case ArPrism: case ArPitch: case ArImgTime: case ArImgFormant: case ArGSusOfs: case ArGLfoPhase: case ArUniPhase: return true;
    default: return false;
  }
}

float neutralShape(int shape) {
  switch (shape) {
    case ShLocalEq: case ShTimbre1: case ShTimbre2: case ShGlobalEq: return 0.25f;
    case ShPhase: case ShRandom: case ShClip: case ShPrism: case ShUniPitch: case ShFormant: case ShRes1: case ShRes2: case ShHarmonizer:
      return 0.5f;
    default: return 1.0f;
  }
}

PatchData::PatchData() {
  for (auto& p : part) {
    for (int a = 0; a < kArts; ++a) {
      ArtData& d = p.arts[static_cast<size_t>(a)];
      d.bipolar = artBipolarDefault(a);
      const float n = d.bipolar ? 0.5f : 1.0f;
      d.env = curveOf({{0, n, 0, 0}});
      d.lfo = curveOf({{0, n, 0, 0}, {0.25f, 1, -0.5f, 0}, {0.25f, n, 0.5f, 0}, {0.25f, d.bipolar ? 0.0f : 0.5f, -0.5f, 0}, {0.25f, n, 0.5f, 0}});
    }
    // Volume: a short attack, full sustain, quick release.
    ArtData& vol = p.arts[ArVol];
    vol.env = curveOf({{0, 0, 0, 0}, {0.004f, 1, 0, 0}, {0.1f, 1, 0, 0}, {0.25f, 0, 0.3f, 0}}, 2);
    vol.env.enabled = true;
  }
}

double curveLength(const Curve& c) {
  double x = 0;
  for (size_t i = 1; i < c.pts.size(); ++i) x += std::max(0.f, c.pts[i].dt);
  return x;
}

namespace {
// Segment shape t 0..1 → 0..1 for kind / tension.
double seg(double t, double tension, int kind, double y0, double y1) {
  t = std::clamp(t, 0.0, 1.0);
  const double e = std::pow(2.0, 4.0 * std::clamp(tension, -1.0, 1.0));
  double u;
  switch (kind) {
    case 1: case 8: case 12: {  // double curve (S)
      u = t < 0.5 ? 0.5 * std::pow(2 * t, e) : 1 - 0.5 * std::pow(2 - 2 * t, e);
      break;
    }
    case 2: u = 0; break;                                                                         // hold
    case 3: u = std::floor(t * (2 + 8 * std::fabs(tension))) / (2 + 8 * std::fabs(tension)); break;  // stairs
    case 4: {
      const double n = 2 + 8 * std::fabs(tension), f = t * n, i = std::floor(f), r = f - i;
      u = (i + (0.5 - 0.5 * std::cos(kPi * r))) / n;
      break;
    }
    case 5: u = std::fmod(t * (2 + 16 * std::fabs(tension)), 1.0) < 0.5 ? 1 : 0; break;  // pulse
    case 6: {  // wave: oscillates around the line
      const double n = 1 + std::floor(16 * std::fabs(tension));
      const double line = y0 + (y1 - y0) * t;
      const double amp = std::fabs(y1 - y0) * 0.5;
      return line + amp * std::sin(2 * kPi * n * t);
    }
    case 9: u = std::sin(0.5 * kPi * std::pow(t, e)); break;  // half sine
    case 10: u = 0.5 - 0.5 * std::cos(kPi * t); break;        // smooth
    default: u = std::pow(t, e); break;                       // single curve
  }
  return y0 + (y1 - y0) * u;
}
}  // namespace

double curveAt(const Curve& c, double x) {
  if (c.pts.empty()) return 0.0;
  double x0 = 0, y0 = c.pts[0].y;
  if (x <= 0) return y0;
  for (size_t i = 1; i < c.pts.size(); ++i) {
    const Pt& p = c.pts[i];
    const double x1 = x0 + std::max(0.f, p.dt);
    if (x < x1) return seg(p.dt > 0 ? (x - x0) / p.dt : 1.0, p.tension, p.kind, y0, p.y);
    x0 = x1;
    y0 = p.y;
  }
  return y0;
}

double shapeAtHarmonic(const Curve& c, int k, int count, float neutral) {
  if (c.pts.empty()) return neutral;
  const double len = std::max(1e-9, curveLength(c));
  const double x = count > 1 ? (k - 1.0) / (count - 1.0) : 0.0;
  // "Wave" segments on the harmonic axis alternate per harmonic (odd on the
  // line, even at its floor) — the classic way to draw odd-only spectra.
  double xa = 0;
  for (size_t i = 1; i < c.pts.size(); ++i) {
    const double xb = xa + c.pts[i].dt / len;
    if (x * 1.0 <= xb + 1e-12) {
      if (c.pts[i].kind == 6) {
        const double t = c.pts[i].dt > 0 ? (x - xa) / (c.pts[i].dt / len) : 1.0;
        const double line = c.pts[i - 1].y + (c.pts[i].y - c.pts[i - 1].y) * std::clamp(t, 0.0, 1.0);
        return (k % 2 == 1) ? line : 0.0;
      }
      break;
    }
    xa = xb;
  }
  return curveAt(c, x * len);
}

double shapeAtHz(const Curve& c, double hz, float neutral) {
  if (c.pts.empty()) return neutral;
  const double x = std::clamp(std::log(std::max(hz, 20.0) / 20.0) / std::log(1000.0), 0.0, 1.0);
  return curveAt(c, x * std::max(1e-9, curveLength(c)));
}

// ------------------------------------------------------------------ text
std::string serializePatch(const PatchData& d) {
  const PatchData def;
  std::ostringstream o;
  o << "hmr 1\n";
  char buf[256];
  for (int p = 0; p < kParts; ++p) {
    const PartData& P = d.part[static_cast<size_t>(p)];
    const PartData& D = def.part[static_cast<size_t>(p)];
    for (int a = 0; a < kArts; ++a) {
      const ArtData& A = P.arts[static_cast<size_t>(a)];
      const ArtData& B = D.arts[static_cast<size_t>(a)];
      if (A.bipolar != B.bipolar || A.att != B.att || A.dec != B.dec || A.sus != B.sus || A.rel != B.rel || A.lfoSpeed != B.lfoSpeed ||
          A.lfoSkew != B.lfoSkew || A.lfoPw != B.lfoPw || A.lfoPhase != B.lfoPhase || A.smooth != B.smooth) {
        std::snprintf(buf, sizeof buf, "a %d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", p, a, A.bipolar ? 1 : 0,
                      static_cast<double>(A.att), static_cast<double>(A.dec), static_cast<double>(A.sus), static_cast<double>(A.rel),
                      static_cast<double>(A.lfoSpeed), static_cast<double>(A.lfoSkew), static_cast<double>(A.lfoPw),
                      static_cast<double>(A.lfoPhase), static_cast<double>(A.smooth));
        o << buf;
      }
      if (!sameCurve(A.env, B.env)) {
        o << "e " << p << ' ' << a << ' ' << (A.env.enabled ? 1 : 0) << ' ' << (A.env.tempo ? 1 : 0) << ' ' << A.env.sustain << ' '
          << A.env.loopStart << ' ' << A.env.decay;
        writePts(o, A.env);
        o << '\n';
      }
      if (!sameCurve(A.lfo, B.lfo)) {
        o << "l " << p << ' ' << a << ' ' << (A.lfo.enabled ? 1 : 0) << ' ' << (A.lfo.tempo ? 1 : 0) << ' ' << (A.lfo.global ? 1 : 0);
        writePts(o, A.lfo);
        o << '\n';
      }
      for (int m = 0; m < kMaps; ++m)
        if (!A.maps[static_cast<size_t>(m)].empty()) {
          o << "m " << p << ' ' << a << ' ' << m;
          writePts(o, A.maps[static_cast<size_t>(m)]);
          o << '\n';
        }
    }
    for (int s = 0; s < kShapes; ++s)
      if (!P.shapes[static_cast<size_t>(s)].empty()) {
        o << "s " << p << ' ' << s;
        writePts(o, P.shapes[static_cast<size_t>(s)]);
        o << '\n';
      }
    const ImageData& im = P.image;
    if (!im.gain.empty()) {
      std::snprintf(buf, sizeof buf, "i %d %d %d %d %.6g ", p, im.cols, im.rows, im.loop, static_cast<double>(im.colSec));
      o << buf << b64(im.gain.data(), im.gain.size()) << ' ';
      if (im.freq.empty()) o << '-';
      else o << b64(reinterpret_cast<const uint8_t*>(im.freq.data()), im.freq.size());
      o << '\n';
    }
    if (!im.source.empty()) o << "x " << p << ' ' << im.source << '\n';
  }
  const std::string s = o.str();
  return s == "hmr 1\n" ? "" : s;
}

bool parsePatch(const std::string& text, PatchData& out) {
  out = PatchData{};
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line.rfind("hmr ", 0) != 0) return false;
  while (std::getline(in, line)) {
    if (line.size() < 3) continue;
    std::istringstream ls(line);
    char tag = 0;
    int p = -1;
    ls >> tag >> p;
    if (p < 0 || p >= kParts) continue;
    PartData& P = out.part[static_cast<size_t>(p)];
    if (tag == 'x') {
      std::string rest;
      std::getline(ls, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      P.image.source = rest;
      continue;
    }
    if (tag == 'i') {
      ImageData im;
      std::string g, f;
      if (!(ls >> im.cols >> im.rows >> im.loop >> im.colSec >> g >> f)) continue;
      if (im.cols <= 0 || im.rows <= 0 || im.cols > 20000 || im.rows > 1024 || !std::isfinite(im.colSec)) continue;
      im.gain = unb64(g);
      if (im.gain.size() != static_cast<size_t>(im.cols) * static_cast<size_t>(im.rows)) continue;
      if (f != "-") {
        auto fv = unb64(f);
        if (fv.size() == im.gain.size()) im.freq.assign(fv.begin(), fv.end());
      }
      im.colSec = std::clamp(im.colSec, 0.0005f, 10.0f);
      im.loop = std::clamp(im.loop, 0, 2);
      im.source = P.image.source;
      P.image = std::move(im);
      continue;
    }
    if (tag == 's') {
      int s = -1;
      std::string pts;
      if (!(ls >> s >> pts) || s < 0 || s >= kShapes) continue;
      Curve c;
      if (readPts(pts, c)) P.shapes[static_cast<size_t>(s)] = c;
      continue;
    }
    int a = -1;
    if (!(ls >> a) || a < 0 || a >= kArts) continue;
    ArtData& A = P.arts[static_cast<size_t>(a)];
    if (tag == 'a') {
      int bip = 0;
      float v[9];
      if (!(ls >> bip >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7] >> v[8])) continue;
      bool ok = true;
      for (float x : v) ok = ok && std::isfinite(x);
      if (!ok) continue;
      A.bipolar = bip != 0;
      A.att = std::clamp(v[0], 0.0f, 4.0f), A.dec = std::clamp(v[1], 0.0f, 4.0f), A.sus = std::clamp(v[2], -1.0f, 1.0f);
      A.rel = std::clamp(v[3], 0.0f, 4.0f), A.lfoSpeed = std::clamp(v[4], 0.0f, 100.0f), A.lfoSkew = std::clamp(v[5], -1.0f, 1.0f);
      A.lfoPw = std::clamp(v[6], -1.0f, 1.0f), A.lfoPhase = v[7] - std::floor(v[7]), A.smooth = std::clamp(v[8], 0.0f, 10.0f);
    } else if (tag == 'e') {
      int en = 0, tempo = 0, sus = -1, ls0 = -1, dec = -1;
      std::string pts;
      if (!(ls >> en >> tempo >> sus >> ls0 >> dec >> pts)) continue;
      Curve c;
      if (!readPts(pts, c)) continue;
      const int n = static_cast<int>(c.pts.size());
      c.enabled = en != 0, c.tempo = tempo != 0;
      c.sustain = sus >= 0 && sus < n ? sus : -1;
      c.loopStart = ls0 >= 0 && ls0 < n ? ls0 : -1;
      c.decay = dec >= 0 && dec < n ? dec : -1;
      A.env = c;
    } else if (tag == 'l') {
      int en = 0, tempo = 0, glob = 0;
      std::string pts;
      if (!(ls >> en >> tempo >> glob >> pts)) continue;
      Curve c;
      if (!readPts(pts, c)) continue;
      c.enabled = en != 0, c.tempo = tempo != 0, c.global = glob != 0;
      A.lfo = c;
    } else if (tag == 'm') {
      int m = -1;
      std::string pts;
      if (!(ls >> m >> pts) || m < 0 || m >= kMaps) continue;
      Curve c;
      if (readPts(pts, c)) A.maps[static_cast<size_t>(m)] = c;
    }
  }
  return true;
}

// ------------------------------------------------------------------ images
bool analyzeAudio(const std::vector<float>& x, double sr, int rows, double hopSec, ImageData& out, double* f0Out) {
  out = ImageData{};
  if (x.size() < 2048 || sr <= 0) return false;
  rows = std::clamp(rows, 1, kHarm);
  // Pitch: normalised autocorrelation over a loud stretch (60..1500 Hz).
  size_t at = 0;
  double best = 0;
  for (size_t i = 0; i + 4096 < x.size(); i += 2048) {
    double e = 0;
    for (size_t j = 0; j < 4096; ++j) e += x[i + j] * x[i + j];
    if (e > best) best = e, at = i;
  }
  const size_t win = std::min<size_t>(4096, x.size() - at);
  const int lagMin = static_cast<int>(sr / 1500), lagMax = std::min(static_cast<int>(sr / 60), static_cast<int>(win / 2));
  double bestR = -1;
  int lag = lagMax;
  std::vector<double> r(static_cast<size_t>(lagMax + 2), 0.0);
  for (int L = lagMin; L <= lagMax; ++L) {
    double s = 0, e1 = 0, e2 = 0;
    for (size_t j = 0; j + static_cast<size_t>(L) < win; ++j) {
      s += x[at + j] * x[at + j + static_cast<size_t>(L)];
      e1 += x[at + j] * x[at + j];
      e2 += x[at + j + static_cast<size_t>(L)] * x[at + j + static_cast<size_t>(L)];
    }
    r[static_cast<size_t>(L)] = s / std::sqrt(std::max(1e-12, e1 * e2));
  }
  // The shortest-lag peak within 10 % of the strongest one (avoids both
  // octave-down errors and locking onto a strong upper harmonic).
  double top = -1;
  for (int L = lagMin + 1; L < lagMax; ++L) top = std::max(top, r[static_cast<size_t>(L)]);
  for (int L = lagMin + 1; L < lagMax; ++L) {
    const double v = r[static_cast<size_t>(L)];
    if (v > r[static_cast<size_t>(L - 1)] && v >= r[static_cast<size_t>(L + 1)] && v >= 0.9 * top) {
      lag = L;
      bestR = v;
      break;
    }
  }
  if (bestR < 0)
    for (int L = lagMin; L <= lagMax; ++L)
      if (r[static_cast<size_t>(L)] > bestR) bestR = r[static_cast<size_t>(L)], lag = L;
  const double f0 = sr / std::max(1, lag);
  if (f0Out) *f0Out = f0;
  // Harmonic tracking: Hann window of ~4 periods, Goertzel at k·f0.
  const size_t W = std::clamp<size_t>(static_cast<size_t>(4.0 * sr / f0), 256, 8192);
  const size_t hop = std::max<size_t>(16, static_cast<size_t>(hopSec * sr));
  const int cols = static_cast<int>(std::min<size_t>(20000, x.size() / hop + 1));
  std::vector<float> mag(static_cast<size_t>(cols) * static_cast<size_t>(rows), 0.0f);
  std::vector<double> w(W);
  for (size_t i = 0; i < W; ++i) w[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / (W - 1));
  float peak = 1e-9f;
  for (int c = 0; c < cols; ++c) {
    const long start = static_cast<long>(c * hop) - static_cast<long>(W / 2);
    for (int k = 1; k <= rows; ++k) {
      const double f = f0 * k;
      if (f >= sr * 0.48) break;
      const double cw = 2 * std::cos(2 * kPi * f / sr);
      double s1 = 0, s2 = 0;
      for (size_t i = 0; i < W; ++i) {
        const long j = start + static_cast<long>(i);
        const double v = j >= 0 && static_cast<size_t>(j) < x.size() ? x[static_cast<size_t>(j)] * w[i] : 0.0;
        const double s0 = v + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      const float m = static_cast<float>(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - cw * s1 * s2)) / (W * 0.25));
      mag[static_cast<size_t>(c) * static_cast<size_t>(rows) + static_cast<size_t>(k - 1)] = m;
      peak = std::max(peak, m);
    }
  }
  out.cols = cols;
  out.rows = rows;
  out.loop = 0;
  out.colSec = static_cast<float>(hop / sr);
  out.gain.resize(mag.size());
  for (size_t i = 0; i < mag.size(); ++i) out.gain[i] = static_cast<uint8_t>(std::clamp(std::lround(255.0 * mag[i] / peak), 0L, 255L));
  return true;
}

bool imageFromPicture(const std::string& path, int rows, ImageData& out) {
  out = ImageData{};
  std::string err;
  ImagePtr im = loadImageFile(path, &err);
  if (!im || im->w <= 0 || im->h <= 0) return false;
  rows = std::clamp(rows, 1, kHarm);
  out.cols = im->w;
  out.rows = rows;
  out.loop = 1;
  out.colSec = 0.02f;
  out.gain.assign(static_cast<size_t>(out.cols) * static_cast<size_t>(rows), 0);
  for (int c = 0; c < im->w; ++c)
    for (int k = 0; k < rows; ++k) {
      // Bottom row = fundamental; the picture's rows spread over the partials.
      const int y = std::clamp(im->h - 1 - k * im->h / rows, 0, im->h - 1);
      const float v = (im->at(im->r, c, y) + im->at(im->g, c, y) + im->at(im->b, c, y)) / 3.0f;
      out.gain[static_cast<size_t>(c) * static_cast<size_t>(rows) + static_cast<size_t>(k)] =
          static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
    }
  out.source = path;
  return true;
}

}  // namespace gl::packs::models::hm
