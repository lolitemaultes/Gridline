// Gridline — FLP import, J-22 (PoiZone, Sawer, BooBass) onto the reference-
// architecture engines of the "va" group (src/packs/synths/va.h).
//
// States (worked out from FL's 386 PoiZone / 421 Sawer shipped presets, the
// demo projects and render tests; every one walks to its exact end):
//   PoiZone  "SSLF", u32 size (= length − 8), "4reV" or "3reV",
//            81 × f32 control values in tag order (knobs, switches and
//            selectors 0..1; the digit wheels 75..80 raw: value − minimum),
//            74 × f32 MIDI CC link per tag 0..73 (−1 = none), u32 0,
//            char[64] preset name                               → 700 bytes
//   Sawer    "SSLF", u32 size, "1rwS", 96 × f32 control values in tag order
//            (wheels / switches 0..1; combos 74..77 and the matrix
//            destinations / sources 84..95 raw), 96 × u8 MIDI CC link per
//            tag (0xFF = none), char[128] preset name           → 620 bytes
//   BooBass  u32 version (1), i32 bass, mid, treble 0..65535   → 16 bytes
// Parameter index = control tag; values convert linearly from the control's
// normalized position (raw indices stay raw), so the mapping is 1:1 and FL
// automation indices are kept. CC links go to PluginState::data
// ("cc <tag> <controller>" lines) and drive the parameters from MIDI in the
// engine; the preset name is kept as "name <text>". Selector positions are
// truncated, switches read as on at any non-zero value and the stepped
// pitches (PoiZone osc B pitch / transpose, Sawer transpose) as whole
// semitones — the way the plugins' own value readouts read them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../packs/synths/va.h"
#include "FlpGen2.h"
#include "FlpJ22.h"

namespace gl::flp::j22 {

namespace {

using gen2::Blob;
namespace va = gl::packs::synths::va;

// N: normalized control (switches on at ≥ 0.5); Z: normalized, switches /
// two-way selectors on at any non-zero value (PoiZone's reading); R: raw index.
enum Kind : uint8_t { N, Z, R };

// Sets parameter `p` from the reference value `v` (normalized or raw).
// Returns false when the value is not finite.
bool put(PluginMapping& m, const PluginInfo& info, int p, float v, Kind k) {
  if (p < 0 || static_cast<size_t>(p) >= info.params.size() || static_cast<size_t>(p) >= m.state.params.size()) return true;
  // Raw combo / digit indices are read as integers (truncated) by the
  // reference; a NaN there is an invalid index, i.e. "none" (index 0).
  if (k == R && !std::isfinite(v)) v = 0;
  if (!std::isfinite(v)) return false;
  const ParamDesc& d = info.params[static_cast<size_t>(p)];
  double out;
  if (k == R)
    out = std::trunc(v);
  else if (d.kind == ParamKind::Toggle || (d.kind == ParamKind::Choice && d.choices.size() == 2 && k == Z))
    out = k == Z ? (v != 0.0f ? 1.0 : 0.0) : (v >= 0.5f ? 1.0 : 0.0);
  else if (d.kind == ParamKind::Choice)  // selectors: truncated position (with a small guard)
    out = std::trunc(std::clamp(static_cast<double>(v), 0.0, 1.0) * static_cast<double>(d.choices.size() - 1) + 1e-3);
  else
    out = d.min + std::clamp(static_cast<double>(v), 0.0, 1.0) * (d.max - d.min);
  m.state.params[static_cast<size_t>(p)] = static_cast<float>(d.clamp(out));
  return true;
}

std::string latin(const Blob& b, size_t at, size_t maxLen) {
  std::string s;
  for (size_t i = 0; i < maxLen && b.has(at + i, 1); ++i) {
    const uint8_t c = b.u8(at + i);
    if (!c) break;
    if (c < 0x80)
      s += static_cast<char>(c);
    else
      s += static_cast<char>(0xC0 | (c >> 6)), s += static_cast<char>(0x80 | (c & 0x3F));
  }
  return s;
}

// Automation links: every tag → the same parameter (descriptor conversion);
// recorded control values: knobs 0..256, digit wheels / combos raw.
void linkAll(PluginMapping& m, int count, const Kind* kinds, const std::vector<int>& semis = {}, bool towardZero = false) {
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  const PluginInfo* info = findPlugin(m.glType);
  for (int t = 0; t < count; ++t) {
    if (std::find(semis.begin(), semis.end(), t) != semis.end())
      links.push_back({t, t, [towardZero](double n) {
                         const double x = std::clamp(n, 0.0, 1.0) * 48.0;
                         return towardZero ? std::trunc(x - 24.0) : std::floor(x) - 24.0;
                       }});
    else
      links.push_back({t, t, {}});
    if (!info || static_cast<size_t>(t) >= info->params.size()) continue;
    const ParamDesc& d = info->params[static_cast<size_t>(t)];
    if (kinds[t] == R)
      ranges.push_back({t, d.min, d.max});
    else if (d.kind == ParamKind::Continuous)
      ranges.push_back({t, 0, 256});
  }
  gen2::linkParams(m, std::move(links), std::move(ranges));
}

// ------------------------------------------------------------------ PoiZone
constexpr int kPzTags = 81, kPzCc = 74, kPzSize = 700;

bool decodePoiZone(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Blob b = gen2::stateBlob(evs);
  gen2::begin(m, "gl.twinva");
  const PluginInfo* info = findPlugin("gl.twinva");
  if (!info) return false;
  Kind kinds[kPzTags];
  for (int t = 0; t < kPzTags; ++t) kinds[t] = t >= va::TwUnison ? R : Z;
  linkAll(m, kPzTags, kinds, {va::TwBPitch, va::TwTranspose});
  m.exact = false;
  if (b.size() < 12 || b.find("SSLF") != 0 || b.u8(9) != 'r' || b.u8(10) != 'e' || b.u8(11) != 'V') return true;  // defaults
  bool ok = b.u32(4) + 8 == b.size() && b.size() == kPzSize;
  const int n = static_cast<int>(std::min<size_t>(kPzTags, (b.size() - 12) / 4));
  for (int t = 0; t < n; ++t)
    if (t != va::TwModWheel) ok = put(m, *info, t, b.f32(12 + 4 * static_cast<size_t>(t)), kinds[t]) && ok;
  // Osc B pitch / transpose step in whole semitones (truncated).
  for (int t : {va::TwBPitch, va::TwTranspose})
    if (n > t && std::isfinite(b.f32(12 + 4 * static_cast<size_t>(t))))
      gen2::set(m, t, std::floor(std::clamp(b.f32(12 + 4 * static_cast<size_t>(t)), 0.0f, 1.0f) * 48.0) - 24.0);
  // Tag 74 is the reference's unused control (always 0): Gridline's mod wheel starts at 0.
  ok = ok && b.f32(12 + 4 * va::TwModWheel) == 0.0f;
  std::string data = "va 1\n";
  if (b.size() >= kPzSize) {
    for (int t = 0; t < kPzCc; ++t) {
      const float cc = b.f32(336 + 4 * static_cast<size_t>(t));
      if (cc >= 0 && cc < 128) data += "cc " + std::to_string(t) + " " + std::to_string(static_cast<int>(cc)) + "\n";
    }
    const std::string name = latin(b, 636, 64);
    if (!name.empty()) data += "name " + name + "\n";
  }
  m.state.data = data;
  m.exact = ok;
  return true;
}

// -------------------------------------------------------------------- Sawer
constexpr int kSwTags = 96, kSwSize = 620;

bool decodeSawer(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Blob b = gen2::stateBlob(evs);
  gen2::begin(m, "gl.sawswarm");
  const PluginInfo* info = findPlugin("gl.sawswarm");
  if (!info) return false;
  Kind kinds[kSwTags];
  for (int t = 0; t < kSwTags; ++t)
    kinds[t] = (t >= va::SwArpSync && t <= va::SwBendRange) || t >= va::SwModDest1 ? R : Z;
  linkAll(m, kSwTags, kinds, {va::SwTranspose}, true);
  m.exact = false;
  if (b.size() < 12 || b.find("SSLF") != 0 || b.find("1rwS") != 8) return true;
  bool ok = b.u32(4) + 8 == b.size() && b.size() == kSwSize;
  const int n = static_cast<int>(std::min<size_t>(kSwTags, (b.size() - 12) / 4));
  for (int t = 0; t < n; ++t)
    if (t != va::SwModWheel) ok = put(m, *info, t, b.f32(12 + 4 * static_cast<size_t>(t)), kinds[t]) && ok;
  // Transpose: whole semitones, truncated towards zero.
  if (n > va::SwTranspose && std::isfinite(b.f32(12 + 4 * va::SwTranspose)))
    gen2::set(m, va::SwTranspose, std::trunc(std::clamp(b.f32(12 + 4 * va::SwTranspose), 0.0f, 1.0f) * 48.0 - 24.0));
  ok = ok && b.f32(12 + 4 * va::SwModWheel) == 0.0f;
  std::string data = "va 1\n";
  if (b.size() >= kSwSize) {
    for (int t = 0; t < kSwTags; ++t) {
      const uint8_t cc = b.u8(396 + static_cast<size_t>(t));
      if (cc < 128) data += "cc " + std::to_string(t) + " " + std::to_string(cc) + "\n";
    }
    const std::string name = latin(b, 492, 128);
    if (!name.empty()) data += "name " + name + "\n";
  }
  m.state.data = data;
  m.exact = ok;
  return true;
}

// ------------------------------------------------------------------ BooBass
bool decodeBooBass(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  const Blob b = gen2::stateBlob(evs);
  gen2::begin(m, "gl.fingerbass");
  gen2::linkParams(m, {{0, va::BbBass, {}}, {1, va::BbMid, {}}, {2, va::BbTreble, {}}}, {{0, 0, 65535}, {1, 0, 65535}, {2, 0, 65535}});
  m.exact = false;
  if (b.size() < 16) return true;
  for (int k = 0; k < 3; ++k) gen2::set(m, k, std::clamp(b.i32(4 + 4 * static_cast<size_t>(k)), 0, 65535) / 65535.0);
  m.exact = b.size() == 16 && b.u32(0) == 1;
  return true;
}

}  // namespace

void registerVa(std::map<std::string, PluginDecoder>& d) {
  set(d, "PoiZone", decodePoiZone);
  set(d, "Sawer", decodeSawer);
  set(d, "BooBass", decodeBooBass);
}

}  // namespace gl::flp::j22
