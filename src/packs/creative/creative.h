// Gridline — "creative" pack: parameter indices and the envelope data model
// shared by the catalogue, the DSP, the editors, the FL import and the tests.
//
//   gl.octafilter   Octa Filter   eight filter units (12 filter types, 1-3 stages,
//                                 per-unit waveshaper, in / out / to-next routing),
//                                 each unit's pan, volume, cutoff, resonance, low,
//                                 band, high and shaper mix articulated by 10 pattern
//                                 envelopes, an LFO and X / Y / follower maps.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../../engine/MEnv.h"

namespace gl::packs::creative {

// ------------------------------------------------------------- Octa Filter parameters
inline constexpr int kUnits = 8, kTargets = 8, kPatterns = 10;
enum Target { TPan, TVol, TCut, TRes, TLow, TBand, THigh, TWsMix, TargetCount };
inline bool additiveTarget(int t) { return t == TPan || t == TCut || t == TRes; }

enum OfGlobal {
  OfMaster,      // master level 0..1.25 (1 = unity, volume curve)
  OfMasterLfo,   // master LFO level -1..1
  OfIefLevel,    // follower input level 0..1.25
  OfIefAttack,   // follower attack (s)
  OfIefRelease,  // follower release (s)
  OfPattern,     // pattern number 1..10
  OfModX, OfModY,  // 0..1
  OfSmooth, OfCenter, OfSyncPatterns, OfHqEnvelopes, OfOversample, OfSolo, OfIefUse1, OfHqShaper,
  OfGlobalCount
};
enum OfUnitField {
  UEnv,      // envelope / LFO / follower amount on the cutoff, 0..2 (1 = 128)
  UCut,      // cutoff 0..1
  URes,      // resonance / bandwidth 0..1
  ULow,      // low (state variable) / shelf-peak gain / ladder drive 0..1
  UBand, UHigh,  // 0..1
  UWsPre,    // shaper pre amp 0.5..1.5
  UWsMix,    // shaper mix 0..1
  UPan,      // out pan -1..1
  UOut,      // out volume 0..1.25 (volume curve)
  UIn,       // in volume -1.25..1.25 (negative inverts)
  UNext,     // to next unit -1.25..1.25
  UOrder,    // 0 off, 1 single, 2 dual, 3 triple, 4 alt dual, 5 alt triple
  UType,     // filter type 0..11
  UAlt, UEnabled, UWsOn, UWsBipolar,
  UFields
};
enum LfoField { LSpeed, LTension, LSkew, LPulse, LPhase, LfoFields };
inline constexpr int unitParam(int u, int f) { return OfGlobalCount + u * UFields + f; }
inline constexpr int LfoBase = OfGlobalCount + kUnits * UFields;
inline constexpr int lfoParam(int u, int t, int f) { return LfoBase + (u * kTargets + t) * LfoFields + f; }
inline constexpr int OfCount = LfoBase + kUnits * kTargets * LfoFields;
// J-42: engine model (appended): 0 legacy, 1 reference (octafilter_refeng.h). In the
// reference LPhase is the raw 32-bit phase offset / 2^32 and pattern / LFO lines
// are present only when enabled. Older states load as legacy.
inline constexpr int OfModel = OfCount;
inline constexpr int OfTotal = OfCount + 1;

enum FilterType { FtSvf, FtLp2, FtHp2, FtLpRbj, FtBp, FtNotch, FtHpRbj, FtAllPass, FtLowShelf, FtPeak, FtHighShelf, FtLadder, FilterTypeCount };
inline bool filterUsesGain(int type) { return type == FtLowShelf || type == FtPeak || type == FtHighShelf || type == FtLadder; }

// Reference volume curve: 0.1 (11^x - 1); 1 at x = 1.
inline double volCurve(double x) {
  if (x == 1.0) return 1.0;
  return x <= 0 ? 0.0 : 0.1 * (std::pow(11.0, x) - 1.0);
}
inline double signedVol(double x) { return x < 0 ? -volCurve(-x) : volCurve(x); }

// ------------------------------------------------------------- envelope data
// Envelope kinds per (unit, target): patterns 0..9, LFO shape, X / Y / follower maps; plus one shaper curve per unit.
enum EnvKind { KPattern0 = 0, KLfo = kPatterns, KMapX, KMapY, KMapF, EnvKinds };
inline constexpr int kEnvPts = 192;  // J-42: FL's longest shipped pattern / shaper curve is 183 points
using OEnv = menv::Env<kEnvPts>;

struct OctaBank {
  static constexpr int kPool = 192;
  OEnv pool[kPool];
  int used = 0;
  int16_t index[kUnits][kTargets][EnvKinds];
  int16_t ws[kUnits];
  int8_t loopS[kUnits][kTargets][kPatterns], loopE[kUnits][kTargets][kPatterns];
  OctaBank() { clear(); }
  void clear() {
    used = 0;
    std::fill(&index[0][0][0], &index[0][0][0] + kUnits * kTargets * EnvKinds, int16_t{-1});
    std::fill(ws, ws + kUnits, int16_t{-1});
    std::fill(&loopS[0][0][0], &loopS[0][0][0] + kUnits * kTargets * kPatterns, int8_t{-1});
    std::fill(&loopE[0][0][0], &loopE[0][0][0] + kUnits * kTargets * kPatterns, int8_t{-1});
  }
  const OEnv* env(int u, int t, int k) const {
    int i = index[u][t][k];
    return i >= 0 ? &pool[i] : nullptr;
  }
  const OEnv* wsCurve(int u) const { return ws[u] >= 0 ? &pool[ws[u]] : nullptr; }
  OEnv* alloc() { return used < kPool ? &pool[used++] : nullptr; }
};

// Default shaper curve (identity bend): (0,0) → (1, 0.75) with a fast start.
inline void defaultWs(OEnv& e) {
  e.clear();
  e.add(0, 0.f);
  e.add(1, 0.75f, 0.75f, menv::SingleCurve);
  e.prepare();
}

// Text format (PluginState::data), first line "OF1":
//   E u t k ls le|x,y,tension,mode ...   envelope k of unit u, target t (x in beats for patterns, 0..1 otherwise;
//                                       ls/le = loop start / end point, -1 none)
//   W u|x,y,tension,mode ...             shaper curve of unit u (x 0..1 input, y 0..1 output)
//   I text                               info text ("\n" escaped as "\\n")
namespace detail {
inline const char* readPoints(const char* p, OEnv* e) {
  char* end = nullptr;
  if (e) e->clear();
  while (*p && *p != '\n') {
    double x = std::strtod(p, &end);
    if (end == p) break;
    p = end;
    double v[3] = {0, 0, 0};
    for (int k = 0; k < 3 && *p == ','; ++k) {
      ++p;
      v[k] = std::strtod(p, &end);
      p = end;
    }
    if (e)
      e->add(std::clamp(x, 0.0, 1024.0), static_cast<float>(std::clamp(v[0], 0.0, 1.0)), static_cast<float>(std::clamp(v[1], -1.0, 1.0)),
             static_cast<uint8_t>(std::clamp(static_cast<int>(v[2]), 0, menv::ModeCount - 1)));
    while (*p == ' ') ++p;
  }
  if (e) e->prepare();
  return p;
}
}  // namespace detail

// Parses data into the bank (no allocation).
inline void parse(const char* data, OctaBank& b) {
  b.clear();
  if (!data || std::strncmp(data, "OF1", 3) != 0) return;
  const char* p = data + 3;
  char* end = nullptr;
  while (*p) {
    while (*p == '\n' || *p == '\r' || *p == ' ') ++p;
    if (!*p) break;
    char c = *p++;
    if (c == 'E') {
      long v[5];
      for (long& x : v) {
        x = std::strtol(p, &end, 10);
        p = end;
      }
      while (*p && *p != '|' && *p != '\n') ++p;
      if (*p == '|') ++p;
      bool ok = v[0] >= 0 && v[0] < kUnits && v[1] >= 0 && v[1] < kTargets && v[2] >= 0 && v[2] < EnvKinds;
      OEnv* e = ok ? b.alloc() : nullptr;
      p = detail::readPoints(p, e);
      if (e && e->n > 0) {
        b.index[v[0]][v[1]][v[2]] = static_cast<int16_t>(e - b.pool);
        if (v[2] < kPatterns) {
          b.loopS[v[0]][v[1]][v[2]] = static_cast<int8_t>(std::clamp(v[3], -1L, static_cast<long>(e->n - 1)));
          b.loopE[v[0]][v[1]][v[2]] = static_cast<int8_t>(std::clamp(v[4], -1L, static_cast<long>(e->n - 1)));
        }
      } else if (e) {
        --b.used;
      }
    } else if (c == 'W') {
      long u = std::strtol(p, &end, 10);
      p = end;
      while (*p && *p != '|' && *p != '\n') ++p;
      if (*p == '|') ++p;
      OEnv* e = u >= 0 && u < kUnits ? b.alloc() : nullptr;
      p = detail::readPoints(p, e);
      if (e && e->n > 0) b.ws[u] = static_cast<int16_t>(e - b.pool);
      else if (e) --b.used;
    }
    while (*p && *p != '\n') ++p;
  }
}

// Serialisation helpers (UI thread).
inline std::string pointsText(const OEnv& e) {
  std::string out;
  char buf[96];
  for (int i = 0; i < e.n; ++i) {
    std::snprintf(buf, sizeof buf, "%s%.9g,%.7g,%.5g,%d", i ? " " : "", e.x[i], static_cast<double>(e.y[i]), static_cast<double>(e.t[i]),
                  static_cast<int>(e.mode[i]));
    out += buf;
  }
  return out;
}
inline std::string envLine(int u, int t, int k, const OEnv& e, int ls = -1, int le = -1) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "E %d %d %d %d %d|", u, t, k, ls, le);
  return buf + pointsText(e);
}
inline std::string wsLine(int u, const OEnv& e) { return "W " + std::to_string(u) + "|" + pointsText(e); }

// Replaces the line whose key matches (key = text before '|', e.g. "E 0 2 3" or "W 1"); empty line removes it.
inline std::string replaceLine(const std::string& data, const std::string& key, const std::string& line) {
  std::string out = "OF1\n";
  size_t st = data.rfind("OF1", 0) == 0 ? 4 : 0;
  while (st < data.size()) {
    size_t e = data.find('\n', st);
    std::string l = data.substr(st, e == std::string::npos ? std::string::npos : e - st);
    bool match = l.compare(0, key.size(), key) == 0 && (l.size() == key.size() || l[key.size()] == ' ' || l[key.size()] == '|');
    if (!l.empty() && !match) out += l + "\n";
    if (e == std::string::npos) break;
    st = e + 1;
  }
  if (!line.empty()) out += line + "\n";
  return out;
}
inline std::string envKey(int u, int t, int k) {
  return "E " + std::to_string(u) + " " + std::to_string(t) + " " + std::to_string(k);
}
inline std::string wsKey(int u) { return "W " + std::to_string(u); }

// Info text stored in the data ("I " line).
inline std::string infoText(const std::string& data) {
  size_t p = data.find("\nI ");
  if (p == std::string::npos) return {};
  size_t e = data.find('\n', p + 3);
  std::string s = data.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3), out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') out += '\n', ++i;
    else out += s[i];
  }
  return out;
}
inline std::string withInfo(const std::string& data, const std::string& text) {
  std::string esc;
  for (char c : text) {
    if (c == '\n') esc += "\\n";
    else if (c != '\r') esc += c;
  }
  return replaceLine(data, "I", esc.empty() ? std::string() : "I " + esc);
}

}  // namespace gl::packs::creative
