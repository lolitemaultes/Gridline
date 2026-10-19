// Gridline — "models" pack, J-22 group "metal": drawn data of Alloy, Spectra
// Morph and Spline Scratcher as compact text in PluginState::data (UI thread),
// plus the spline evaluation the scratcher's runtime uses on prepared copies.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "metal.h"

namespace gl::packs::models::metal {

namespace {

constexpr size_t kMaxCurvePts = 16384;

void writeCurve(std::ostringstream& o, const char* key, int idx, const hm::Curve& c) {
  if (c.pts.empty() && c.sustain < 0 && c.loopStart < 0 && c.decay < 0 && !c.enabled) return;
  o << key << ' ' << idx << ' ' << (c.enabled ? 1 : 0) << (c.tempo ? 1 : 0) << (c.global ? 1 : 0) << ' ' << c.sustain << ' '
    << c.loopStart << ' ' << c.decay << ' ';
  if (c.pts.empty()) o << '-';
  char buf[96];
  for (size_t i = 0; i < c.pts.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s%.7g,%.7g,%.4g,%d", i ? ";" : "", static_cast<double>(c.pts[i].dt), static_cast<double>(c.pts[i].y),
                  static_cast<double>(c.pts[i].tension), static_cast<int>(c.pts[i].kind));
    o << buf;
  }
  o << '\n';
}

bool readCurve(std::istringstream& in, hm::Curve& c) {
  std::string flags, pts;
  int sus = -1, loop = -1, dec = -1;
  if (!(in >> flags >> sus >> loop >> dec >> pts) || flags.size() != 3) return false;
  c.enabled = flags[0] == '1', c.tempo = flags[1] == '1', c.global = flags[2] == '1';
  c.pts.clear();
  if (pts != "-") {
    size_t at = 0;
    while (at <= pts.size()) {
      size_t semi = pts.find(';', at);
      if (semi == std::string::npos) semi = pts.size();
      hm::Pt p;
      int kind = 0;
      if (std::sscanf(pts.c_str() + at, "%f,%f,%f,%d", &p.dt, &p.y, &p.tension, &kind) < 2) return false;
      if (!std::isfinite(p.dt) || !std::isfinite(p.y) || !std::isfinite(p.tension)) return false;
      p.dt = std::max(0.0f, p.dt);
      p.y = std::clamp(p.y, -4.0f, 4.0f);
      p.tension = std::clamp(p.tension, -1.0f, 1.0f);
      p.kind = static_cast<uint8_t>(std::clamp(kind, 0, 12));
      c.pts.push_back(p);
      if (c.pts.size() > kMaxCurvePts) return false;
      at = semi + 1;
    }
  }
  const int n = static_cast<int>(c.pts.size());
  c.sustain = sus >= 0 && sus < n ? sus : -1;
  c.loopStart = loop >= 0 && loop < n ? loop : -1;
  c.decay = dec >= 0 && dec < n ? dec : -1;
  return true;
}

std::string escapeLine(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '\\') o += "\\\\";
    else if (ch == '\n') o += "\\n";
    else if (ch == '\r') o += "\\r";
    else o += ch;
  }
  return o;
}
std::string unescapeLine(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      o += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}

}  // namespace

// ------------------------------------------------------------------ Alloy
bool alloyArtBipolar(int art) {
  // Offsets (0.5 neutral): filter frequency / resonance, pitch, fullness,
  // rotation, EQ shifting; the others are factors (1 neutral) — as the shipped
  // patches' untouched maps show.
  switch (art) {
    case AaCutoff: case AaReso: case AaPitch: case AaFullness: case AaRotation: case AaEqShift: return true;
    default: return false;
  }
}
float alloyEqNeutral(int eq) { return eq == AeForced ? 0.0f : 0.5f; }

AlloyData::AlloyData() {
  for (int a = 0; a < AaCount; ++a) {
    AlloyArtData& A = arts[static_cast<size_t>(a)];
    A.bipolar = alloyArtBipolar(a);
    if (a < kAlloyEnvArts) {
      A.env.pts = {{0, A.bipolar ? 0.5f : 0.0f, 0, 0}, {0.01f, A.bipolar ? 0.5f : 1.0f, 0, 0}, {0.5f, A.bipolar ? 0.5f : 1.0f, 0, 0},
                   {0.3f, A.bipolar ? 0.5f : 0.0f, 0, 0}};
      A.env.sustain = 2;
      A.env.enabled = a == AaVolume;
      A.lfo.pts = {{0, 0.5f, 0, 0}, {0.25f, 1, 0, 0}, {0.5f, 0, 0, 0}, {0.25f, 0.5f, 0, 0}};
    }
  }
}

std::string serializeAlloy(const AlloyData& d) {
  std::ostringstream o;
  o << "alloy1\n";
  char buf[200];
  for (int a = 0; a < AaCount; ++a) {
    const AlloyArtData& A = d.arts[static_cast<size_t>(a)];
    std::snprintf(buf, sizeof buf, "art %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", a, A.bipolar ? 1 : 0,
                  static_cast<double>(A.att), static_cast<double>(A.dec), static_cast<double>(A.sus), static_cast<double>(A.rel),
                  static_cast<double>(A.lfoSpeed), static_cast<double>(A.lfoTension), static_cast<double>(A.lfoSkew),
                  static_cast<double>(A.lfoPw), static_cast<double>(A.lfoPhase));
    o << buf;
    writeCurve(o, "env", a, A.env);
    writeCurve(o, "lfo", a, A.lfo);
    for (int m = 0; m < AmCount; ++m) {
      std::snprintf(buf, sizeof buf, "map%d", m);
      writeCurve(o, buf, a, A.maps[static_cast<size_t>(m)]);
    }
  }
  for (int e = 0; e < AeCount; ++e) writeCurve(o, "eq", e, d.eq[static_cast<size_t>(e)]);
  if (!d.info.empty()) o << "info " << escapeLine(d.info) << '\n';
  return o.str();
}

bool parseAlloy(const std::string& text, AlloyData& out) {
  out = AlloyData{};
  if (text.rfind("alloy1", 0) != 0) return false;
  std::istringstream lines(text);
  std::string line;
  std::getline(lines, line);
  // Curves present in the text replace the defaults; absent ones are empty.
  for (auto& A : out.arts) A.env = hm::Curve{}, A.lfo = hm::Curve{};
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    if (line.rfind("info ", 0) == 0) {
      out.info = unescapeLine(line.substr(5));
      continue;
    }
    std::istringstream in(line);
    std::string key;
    int idx = -1;
    if (!(in >> key >> idx)) return false;
    if (key == "art") {
      if (idx < 0 || idx >= AaCount) return false;
      AlloyArtData& A = out.arts[static_cast<size_t>(idx)];
      int bip = 0;
      if (!(in >> bip >> A.att >> A.dec >> A.sus >> A.rel >> A.lfoSpeed >> A.lfoTension >> A.lfoSkew >> A.lfoPw >> A.lfoPhase)) return false;
      A.bipolar = bip != 0;
      for (float* f : {&A.att, &A.dec, &A.rel}) *f = std::isfinite(*f) ? std::clamp(*f, 0.0f, 8.0f) : 1.0f;
      A.sus = std::isfinite(A.sus) ? std::clamp(A.sus, -1.0f, 1.0f) : 0.0f;
      A.lfoSpeed = std::isfinite(A.lfoSpeed) ? std::clamp(A.lfoSpeed, 0.0f, 200.0f) : 1.0f;
      for (float* f : {&A.lfoTension, &A.lfoSkew, &A.lfoPw}) *f = std::isfinite(*f) ? std::clamp(*f, -1.0f, 1.0f) : 0.0f;
      A.lfoPhase = std::isfinite(A.lfoPhase) ? A.lfoPhase - std::floor(A.lfoPhase) : 0.0f;
    } else if (key == "env" || key == "lfo" || key.rfind("map", 0) == 0) {
      if (idx < 0 || idx >= AaCount) return false;
      AlloyArtData& A = out.arts[static_cast<size_t>(idx)];
      hm::Curve* c = nullptr;
      if (key == "env") c = &A.env;
      else if (key == "lfo") c = &A.lfo;
      else {
        const int m = std::atoi(key.c_str() + 3);
        if (m < 0 || m >= AmCount) return false;
        c = &A.maps[static_cast<size_t>(m)];
      }
      if (!readCurve(in, *c)) return false;
    } else if (key == "eq") {
      if (idx < 0 || idx >= AeCount || !readCurve(in, out.eq[static_cast<size_t>(idx)])) return false;
    }
  }
  return true;
}

// ----------------------------------------------------------- Spectra Morph
// "smorph1", "name <text>", then per corner program p and zone:
// "zone p key gain loopStart loopEnd", followed by its frames
// "f level hold <hex>" (amplitudes as 16-bit log codes: 0 = silent,
// 1..65535 = -120..0 dB).
std::string serializeMorph(const SmData& d) {
  std::ostringstream o;
  o << "smorph1\n";
  if (!d.name.empty()) o << "name " << escapeLine(d.name) << '\n';
  char buf[96];
  for (int p = 0; p < kSmPrograms; ++p)
    for (const SmZone& z : d.prog[static_cast<size_t>(p)].zones) {
      std::snprintf(buf, sizeof buf, "zone %d %d %.6g %d %d\n", p, z.key, static_cast<double>(z.gain), z.loopStart, z.loopEnd);
      o << buf;
      for (const SmFrame& f : z.frames) {
        std::snprintf(buf, sizeof buf, "f %.6g %d ", static_cast<double>(f.level), f.hold);
        o << buf;
        if (f.amp.empty()) o << '-';
        for (float a : f.amp) {
          int code = 0;
          if (a > 1e-6f) code = std::clamp(static_cast<int>(std::lround((20.0 * std::log10(std::min(1.0f, a)) + 120.0) / 120.0 * 65534.0)) + 1, 1, 65535);
          std::snprintf(buf, sizeof buf, "%04x", code);
          o << buf;
        }
        o << '\n';
      }
    }
  return o.str();
}

bool parseMorph(const std::string& text, SmData& out) {
  out = SmData{};
  if (text.rfind("smorph1", 0) != 0) return false;
  std::istringstream lines(text);
  std::string line;
  std::getline(lines, line);
  SmZone* cur = nullptr;
  size_t frames = 0;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    if (line.rfind("name ", 0) == 0) {
      out.name = unescapeLine(line.substr(5));
      continue;
    }
    std::istringstream in(line);
    std::string key;
    in >> key;
    if (key == "zone") {
      int p = -1;
      SmZone z;
      if (!(in >> p >> z.key >> z.gain >> z.loopStart >> z.loopEnd) || p < 0 || p >= kSmPrograms || !std::isfinite(z.gain)) return false;
      z.key = std::clamp(z.key, 0, 127);
      z.gain = std::clamp(z.gain, 0.0f, 16.0f);
      auto& zs = out.prog[static_cast<size_t>(p)].zones;
      if (zs.size() >= 64) return false;
      zs.push_back(z);
      cur = &zs.back();
    } else if (key == "f") {
      if (!cur || ++frames > 200000) return false;
      SmFrame f;
      std::string hex;
      if (!(in >> f.level >> f.hold >> hex) || !std::isfinite(f.level)) return false;
      f.level = std::clamp(f.level, 0.0f, 16.0f);
      f.hold = std::clamp(f.hold, 0, 1 << 20);
      if (hex != "-") {
        if (hex.size() % 4 || hex.size() / 4 > static_cast<size_t>(kSmMaxPartials)) return false;
        const size_t n = hex.size() / 4;
        f.amp.resize(n);
        for (size_t i = 0; i < n; ++i) {
          const int code = static_cast<int>(std::strtol(hex.substr(i * 4, 4).c_str(), nullptr, 16));
          f.amp[i] = code <= 0 ? 0.0f : static_cast<float>(std::pow(10.0, ((code - 1) / 65534.0 * 120.0 - 120.0) / 20.0));
        }
      }
      cur->frames.push_back(std::move(f));
    }
  }
  return true;
}

// ------------------------------------------------------- Spline Scratcher
SsPatch ssDefaultPatch() {
  SsPatch p;
  p.pos = {{0, 0, 0.5f, SsLinear, false}, {1, 1, 0.5f, SsLinear, false}};
  p.vol = {{0, 1, 0.5f, SsLinear, false}, {1, 1, 0.5f, SsLinear, false}};
  p.mutes = {{true, 0.0f}};
  return p;
}

namespace {
void writePoints(std::ostringstream& o, const std::vector<SsPoint>& v) {
  char buf[96];
  if (v.empty()) o << '-';
  for (size_t i = 0; i < v.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s%.6g,%.6g,%.4g,%d,%d", i ? ";" : "", static_cast<double>(v[i].x), static_cast<double>(v[i].y),
                  static_cast<double>(v[i].tension), static_cast<int>(v[i].curve), v[i].locked ? 1 : 0);
    o << buf;
  }
}
bool readPoints(const std::string& s, std::vector<SsPoint>& v) {
  v.clear();
  if (s == "-") return true;
  size_t at = 0;
  while (at <= s.size()) {
    size_t semi = s.find(';', at);
    if (semi == std::string::npos) semi = s.size();
    SsPoint p;
    int curve = SsLinear, locked = 0;
    if (std::sscanf(s.c_str() + at, "%f,%f,%f,%d,%d", &p.x, &p.y, &p.tension, &curve, &locked) < 2) return false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.tension)) return false;
    p.x = std::clamp(p.x, 0.0f, 1.0f), p.y = std::clamp(p.y, 0.0f, 1.0f), p.tension = std::clamp(p.tension, 0.0f, 1.0f);
    p.curve = static_cast<uint8_t>(std::clamp(curve, 0, SsCurveCount - 1));
    p.locked = locked != 0;
    v.push_back(p);
    if (v.size() > 4096) return false;
    at = semi + 1;
  }
  std::stable_sort(v.begin(), v.end(), [](const SsPoint& a, const SsPoint& b) { return a.x < b.x; });
  return true;
}
}  // namespace

std::string serializeScratchSpl(const SsData& d) {
  std::ostringstream o;
  // First line: the sample path (the channel-sample convention), then the patches.
  o << d.path << '\n' << "sscratch1 " << d.current << '\n';
  char buf[160];
  for (int k = 0; k < kSsKeys; ++k) {
    const SsPatch& p = d.keys[static_cast<size_t>(k)];
    if (!p.used) continue;
    std::snprintf(buf, sizeof buf, "key %d %.6g %.6g %.6g %.6g %.6g %.6g ", k, static_cast<double>(p.beats), static_cast<double>(p.speed),
                  static_cast<double>(p.attack), static_cast<double>(p.release), static_cast<double>(p.start), static_cast<double>(p.end));
    o << buf;
    if (p.mutes.empty()) o << '-';
    for (size_t i = 0; i < p.mutes.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%d,%.6g", i ? ";" : "", p.mutes[i].first ? 1 : 0, static_cast<double>(p.mutes[i].second));
      o << buf;
    }
    o << ' ';
    writePoints(o, p.pos);
    o << ' ';
    writePoints(o, p.vol);
    o << ' ' << escapeLine(p.name.empty() ? std::string("-") : p.name) << '\n';
  }
  return o.str();
}

bool parseScratchSpl(const std::string& text, SsData& out) {
  out = SsData{};
  for (auto& k : out.keys) k = ssDefaultPatch();
  std::istringstream lines(text);
  std::string line;
  bool first = true;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("sscratch1", 0) == 0) {
      out.current = std::clamp(std::atoi(line.c_str() + 9), 0, kSsKeys - 1);
      first = false;
      continue;
    }
    if (first) {  // a bare sample path (a dropped file) keeps the default patches
      first = false;
      if (line.rfind("key ", 0) != 0) {
        out.path = line;
        continue;
      }
    }
    std::istringstream in(line);
    std::string key, mutes, pos, vol, name;
    int k = -1;
    SsPatch p;
    if (!(in >> key >> k) || key != "key") continue;
    if (k < 0 || k >= kSsKeys) return false;
    if (!(in >> p.beats >> p.speed >> p.attack >> p.release >> p.start >> p.end >> mutes >> pos >> vol)) return false;
    std::getline(in, name);
    if (!name.empty() && name[0] == ' ') name.erase(0, 1);
    p.name = name == "-" ? std::string() : unescapeLine(name);
    for (float* f : {&p.beats, &p.speed, &p.attack, &p.release, &p.start, &p.end})
      if (!std::isfinite(*f)) return false;
    p.beats = std::clamp(p.beats, 0.0625f, 64.0f);
    p.start = std::clamp(p.start, 0.0f, 1.0f), p.end = std::clamp(p.end, 0.0f, 1.0f);
    p.speed = std::clamp(p.speed, 0.0f, 16.0f), p.attack = std::clamp(p.attack, 0.0f, 16.0f), p.release = std::clamp(p.release, 0.0f, 16.0f);
    if (mutes != "-") {
      size_t at = 0;
      while (at <= mutes.size()) {
        size_t semi = mutes.find(';', at);
        if (semi == std::string::npos) semi = mutes.size();
        int on = 1;
        float x = 0;
        if (std::sscanf(mutes.c_str() + at, "%d,%f", &on, &x) < 2 || !std::isfinite(x)) return false;
        p.mutes.push_back({on != 0, std::clamp(x, 0.0f, 1.0f)});
        if (p.mutes.size() > 4096) return false;
        at = semi + 1;
      }
    }
    if (!readPoints(pos, p.pos) || !readPoints(vol, p.vol)) return false;
    if (p.pos.empty()) p.pos = ssDefaultPatch().pos;
    if (p.vol.empty()) p.vol = ssDefaultPatch().vol;
    p.used = true;
    out.keys[static_cast<size_t>(k)] = std::move(p);
  }
  return true;
}

float ssEval(const std::vector<SsPoint>& pts, float x) {
  if (pts.empty()) return 0;
  if (x <= pts.front().x) return pts.front().y;
  if (x >= pts.back().x) return pts.back().y;
  size_t i = 1;
  while (i < pts.size() && pts[i].x < x) ++i;
  const SsPoint& a = pts[i - 1];
  const SsPoint& b = pts[i];
  const float w = b.x - a.x;
  float t = w > 1e-9f ? (x - a.x) / w : 1.0f;
  constexpr float kHalfPi = 1.57079632679f;
  switch (a.curve) {
    case SsHalfCosine: t = 0.5f - 0.5f * std::cos(t * 2 * kHalfPi); break;
    case SsQuarterSine: t = std::sin(t * kHalfPi); break;
    case SsQuarterCosine: t = 1.0f - std::cos(t * kHalfPi); break;
    case SsTension: {
      // tension 0.5 = straight; towards 0 / 1 bends the segment (power curve).
      const float e = std::pow(4.0f, (a.tension - 0.5f) * 2.0f);
      t = std::pow(t, e);
      break;
    }
    case SsBezier: t = t * t * (3 - 2 * t); break;
    default: break;
  }
  return a.y + (b.y - a.y) * t;
}

bool ssSoundOn(const SsPatch& p, float x) {
  bool on = true;
  for (const auto& [state, at] : p.mutes)
    if (at <= x) on = state;
  return on;
}

}  // namespace gl::packs::models::metal
