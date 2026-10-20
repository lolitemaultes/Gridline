// Gridline — FLP import, J-22 (Drumpad, Drumaxx, BassDrum, Fruit Kick).
//   Drumpad / DrumPad → Membrane Pad ("gl.membranepad")
//   Drumaxx           → Membrane Kit ("gl.membranekit")
//   BassDrum          → Kick Sculptor ("gl.kicksculpt")
//   Fruit Kick        → Sweep Kick ("gl.sweepkick")
// Every Gridline parameter index equals the FL control tag, so values and
// automation map 1:1. State layouts (worked out from FL's 961 + 131 + 52 shipped
// presets and every demo-project / template / render-test channel; each state
// walks to its last byte):
//
// Drumpad: "SSLF", u32 chunk size, "3PMD", preset name [64], drum name [64],
//   38 × f32 drum model (tags 0..37: knobs 0..1, velocity-route sources as raw
//   indices), then (newer saves, chunk size 288) f32 output level and, after the
//   chunk, u32 0. Older saves (chunk 284) stop after the model.
// Drumaxx: "SSLF", u32 size, "3SMD" / "4SMD", kit name [256], 16 channels × 44 f32
//   (volume, pan, mute, solo, key, the 38-value drum model, spare; the model's
//   repeat selector is the raw index 0..3 in "3SMD", index / 3 in "4SMD"), 16 f32 master
//   (tags 704..719: -, level, swing, EQ on, 8 EQ bands, limiter on, gain,
//   release, -), 16 patterns × {u8 length − 1 (0x7F = unused), u8 trigger key
//   (0x7F = none), 16 channels × 64 step cells (bit 7 on, bits 0..6 velocity)},
//   16 × {channel name [64], second name [64]}, 32 link bytes.
// BassDrum: "SSLF", u32 size, "1kiK" (older) / "2kiK", preset name [256], noise sample path
//   [1024] (inside the plugin's "Noise Samples" folder), 23 × f32 (tags 0..22).
// Fruit Kick: i32 version (0 / 1), 6 × i32 control values (tags 0..5: max freq
//   cents −2400..3600, min freq cents −1200..1200, freq decay 0..256, amp decay
//   0..256, click 0..64, distortion 0..128), i32 seed; version 1 adds a copy of
//   the channel's five envelope / LFO records (5 × 68 bytes), its levels record
//   (24 bytes) and a word; envelopes are switched off in FL's content.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../packs/models/drums.h"
#include "FlpGen.h"
#include "FlpGen2.h"
#include "FlpJ22.h"

namespace gl::flp::j22 {

namespace {

namespace dr = gl::packs::models::dr;
using gen2::Blob;

bool magic(const Blob& b, size_t at, const char* s) {
  for (size_t k = 0; k < 4; ++k)
    if (b.u8(at + k) != static_cast<uint8_t>(s[k])) return false;
  return true;
}

// A fixed-size, NUL-terminated name field (garbage after the NUL is ignored).
std::string field(const Blob& b, size_t at, size_t len) {
  std::string s;
  for (size_t k = 0; k < len && b.u8(at + k); ++k) s += static_cast<char>(b.u8(at + k));
  return s;
}

bool finite01(float v) { return std::isfinite(v) && v >= -0.001f && v <= 1.001f; }

// Reads the 38 model values. The repeat selector (tag 1) has four positions:
// the pad stores it normalized (index / 3), the kit as the raw index (a few
// older kit saves normalized). `strict`: every knob must be within 0..1 (unused
// kit channels hold uninitialised memory; there values are clamped and
// `junk` is set instead of failing).
bool readModel(const Blob& b, size_t at, PluginMapping& m, int base, bool rawSelector, bool strict, bool& junk) {
  for (int k = 0; k < dr::MdCount; ++k) {
    float v = b.f32(at + 4 * static_cast<size_t>(k));
    double out;
    if (k >= dr::MdVelSrc1 && k <= dr::MdVelSrc4) {
      if (!std::isfinite(v) || v < 0 || v >= static_cast<float>(dr::velDestCount()) || v != std::floor(v)) return false;
      out = v;
    } else if (k == dr::MdRetrig) {
      if (!std::isfinite(v) || v < 0 || v > 3.001f) return false;
      out = rawSelector && v == std::floor(v) ? v : std::round(std::clamp(v, 0.0f, 1.0f) * 3.0f);
    } else if (!finite01(v)) {
      if (strict) return false;
      junk = true;
      out = std::isfinite(v) ? std::clamp(static_cast<double>(v), 0.0, 1.0) : 0.0;
    } else {
      out = std::clamp(static_cast<double>(v), 0.0, 1.0);
    }
    gen2::set(m, base + k, out);
  }
  return true;
}

// Identity automation links for `n` parameters starting at FL / Gridline index `first`.
void identityLinks(std::vector<gen2::Link>& links, std::vector<RawRange>& ranges, int first, int n, double rawHi = 256) {
  for (int k = first; k < first + n; ++k) {
    links.push_back({k, k, {}});
    ranges.push_back({k, 0, rawHi});
  }
}

// ------------------------------------------------------------------ Drumpad
bool decodeDrumpad(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = gen2::stateBlob(events);
  constexpr size_t kModel = 0x8C, kOld = kModel + dr::MdCount * 4;  // 292
  if (!(b.size() == kOld || b.size() == kOld + 8) || !magic(b, 0, "SSLF") || !magic(b, 8, "3PMD")) return false;
  const uint32_t chunk = b.u32(4);
  if (chunk + 8 != (b.size() == kOld ? kOld : kOld + 4)) return false;
  gen2::begin(m, "gl.membranepad");
  bool junk = false;
  if (!readModel(b, kModel, m, dr::PdModel, false, true, junk)) return false;
  if (b.size() > kOld) {
    float out = b.f32(kOld);
    if (!finite01(out) || b.u32(kOld + 4) != 0) return false;
    gen2::set(m, dr::PdOutput, out);
  } else {
    gen2::set(m, dr::PdOutput, 1.0);
  }
  m.state.data = field(b, 12, 64);  // preset name (shown in the editor)
  m.exact = true;
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  identityLinks(links, ranges, dr::MdMalletRetrig, 2, 1);
  identityLinks(links, ranges, dr::MdMalletAmp, dr::MdVelSrc1 - dr::MdMalletAmp);
  identityLinks(links, ranges, dr::MdVelAmt1, 4);
  gen2::linkParams(m, std::move(links), std::move(ranges));
  return true;
}

// ------------------------------------------------------------------ Drumaxx
constexpr size_t kDxName = 12, kDxPads = kDxName + 256, kDxMaster = kDxPads + dr::kKitPads * dr::kKitStride * 4,
                 kDxPatterns = kDxMaster + 16 * 4, kDxPatSize = 2 + dr::kKitPads * dr::kKitSteps,
                 kDxNames = kDxPatterns + dr::kKitPatterns * kDxPatSize, kDxLinks = kDxNames + dr::kKitPads * 128,
                 kDxEnd = kDxLinks + 32;  // 21644

bool decodeDrumaxx(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = gen2::stateBlob(events);
  if (b.size() != kDxEnd || !magic(b, 0, "SSLF") || !(magic(b, 8, "3SMD") || magic(b, 8, "4SMD")) || b.u32(4) + 8 != kDxEnd) return false;
  const bool rawSelector = magic(b, 8, "3SMD");  // version 4 stores the repeat selector normalized
  gen2::begin(m, "gl.membranekit");
  bool junkUsed = false;
  for (int p = 0; p < dr::kKitPads; ++p) {
    const size_t at = kDxPads + static_cast<size_t>(p) * dr::kKitStride * 4;
    const std::string name = field(b, kDxNames + static_cast<size_t>(p) * 128, 64);
    const bool used = !(name.empty() || name == "...");
    for (int f : {dr::KpVolume, dr::KpPan, dr::KpMute, dr::KpSolo, dr::KpSpare}) {
      float v = b.f32(at + 4 * static_cast<size_t>(f));
      if (!finite01(v)) return false;
      gen2::set(m, dr::kitPad(p, f), v);
    }
    float key = b.f32(at + 4 * dr::KpKey);
    if (!std::isfinite(key) || key < 0 || key > 127 || key != std::floor(key)) return false;
    gen2::set(m, dr::kitPad(p, dr::KpKey), key);
    bool junk = false;
    if (!readModel(b, at + 4 * dr::KpModel, m, dr::kitPad(p, dr::KpModel), rawSelector, false, junk)) return false;
    junkUsed = junkUsed || (junk && used);
  }
  for (int k = 0; k < 16; ++k) {
    float v = b.f32(kDxMaster + 4 * static_cast<size_t>(k));
    if (!finite01(v)) return false;
    gen2::set(m, dr::kKitMaster + k, v);
  }
  dr::KitData d;
  d.kitName = field(b, kDxName, 256);
  for (int k = 0; k < dr::kKitPatterns; ++k) {
    const size_t at = kDxPatterns + static_cast<size_t>(k) * kDxPatSize;
    const int len = b.u8(at), key = b.u8(at + 1);
    dr::KitPattern& P = d.pattern[k];
    P.length = len >= dr::kKitSteps ? 0 : len + 1;
    P.key = key >= 0x7F ? -1 : key;
    for (int ch = 0; ch < dr::kKitPads; ++ch)
      for (int s = 0; s < dr::kKitSteps; ++s) P.cell[ch][s] = b.u8(at + 2 + static_cast<size_t>(ch) * dr::kKitSteps + static_cast<size_t>(s));
  }
  for (int p = 0; p < dr::kKitPads; ++p) {
    d.padName[p] = field(b, kDxNames + static_cast<size_t>(p) * 128, 64);
    if (!field(b, kDxNames + static_cast<size_t>(p) * 128 + 64, 64).empty()) d.padName[p] += "|" + field(b, kDxNames + static_cast<size_t>(p) * 128 + 64, 64);
  }
  for (int k = 0; k < 32; ++k) d.link[k] = b.u8(kDxLinks + static_cast<size_t>(k));
  m.state.data = dr::encodeKitData(d);
  // J-33: the kit's voices still use Gridline's modal membrane (the reference
  // drum engine is confirmed for Drumpad only, see drums_ref.h), so not exact.
  (void)junkUsed;
  m.exact = false;
  gen::note(c, "Drumaxx", "drum voices use Gridline's modal membrane model");
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  for (int p = 0; p < dr::kKitPads; ++p) {
    const int o = dr::kitPad(p, 0);
    identityLinks(links, ranges, o + dr::KpVolume, 4, 256);
    links.push_back({o + dr::KpKey, o + dr::KpKey, [](double n) { return std::round(n * 127.0); }});
    identityLinks(links, ranges, o + dr::KpModel, 2, 1);
    identityLinks(links, ranges, o + dr::KpModel + dr::MdMalletAmp, dr::MdVelSrc1 - dr::MdMalletAmp);
    identityLinks(links, ranges, o + dr::KpModel + dr::MdVelAmt1, 4);
  }
  identityLinks(links, ranges, dr::KmLevel, dr::KmReserved2 - dr::KmLevel);
  gen2::linkParams(m, std::move(links), std::move(ranges));
  return true;
}

// ------------------------------------------------------------------ BassDrum
bool decodeBassDrum(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = gen2::stateBlob(events);
  constexpr size_t kPath = 12 + 256, kVals = kPath + 1024, kEnd = kVals + 23 * 4;  // 1384
  if (b.size() != kEnd || !magic(b, 0, "SSLF") || !(magic(b, 8, "2kiK") || magic(b, 8, "1kiK")) || b.u32(4) + 8 != kEnd) return false;
  gen2::begin(m, "gl.kicksculpt");
  for (int k = 0; k < 23; ++k) {
    float v = b.f32(kVals + 4 * static_cast<size_t>(k));
    if (!finite01(v)) return false;
    gen2::set(m, k, v);
  }
  const std::string path = field(b, kPath, 1024);
  const int noise = dr::kickNoiseIndex(path);
  gen2::set(m, dr::KsNoiseSample, std::max(0, noise));
  m.state.data = field(b, 12, 256);  // preset name
  m.exact = noise >= 0 || path.empty();
  std::vector<gen2::Link> links;
  std::vector<RawRange> ranges;
  identityLinks(links, ranges, 0, 23);
  gen2::linkParams(m, std::move(links), std::move(ranges));
  return true;
}

// ---------------------------------------------------------------- Fruit Kick
bool decodeFruitKick(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  (void)c;
  Blob b = gen2::stateBlob(events);
  const int32_t version = b.i32(0);
  constexpr size_t kV0 = 32, kV1 = 32 + 5 * 68 + 24 + 4;  // 400
  if (!((version == 0 && b.size() == kV0) || (version == 1 && b.size() == kV1))) return false;
  static const int lo[6] = {-2400, -1200, 0, 0, 0, 0}, hi[6] = {3600, 1200, 256, 256, 64, 128};
  gen2::begin(m, "gl.sweepkick");
  for (int k = 0; k < 6; ++k) {
    const int32_t v = b.i32(4 + 4 * static_cast<size_t>(k));
    if (v < lo[k] || v > hi[k]) return false;
    gen2::set(m, k, v);
  }
  gen2::set(m, dr::SkSeed, static_cast<double>(b.u32(28) & 0xFFFFFF));
  bool neutral = true;
  if (version == 1) {
    // Five channel envelope / LFO records (17 dwords: flags, on, predelay, A,
    // H, D, S, R, amount, LFO predelay / attack / amount / speed / shape,
    // tensions) from offset 32, then the levels record (24 bytes) and a word.
    for (int k = 0; k < 5; ++k) {
      const size_t at = 32 + 68 * static_cast<size_t>(k);
      if (b.i32(at + 4) > 0 || b.i32(at + 44) != 0) neutral = false;  // envelope on / LFO amount
    }
  }
  m.exact = neutral;
  std::vector<gen2::Link> links;
  for (int k = 0; k < 6; ++k) links.push_back({k, k, [k](double n) { return lo[k] + n * (hi[k] - lo[k]); }});
  gen2::linkParams(m, std::move(links), {{0, -2400, 3600}, {1, -1200, 1200}, {2, 0, 256}, {3, 0, 256}, {4, 0, 64}, {5, 0, 128}});
  return true;
}

}  // namespace

void registerDrums(std::map<std::string, PluginDecoder>& d) {
  set(d, "Drumpad", decodeDrumpad);
  set(d, "DrumPad", decodeDrumpad);
  set(d, "Drumaxx", decodeDrumaxx);
  set(d, "BassDrum", decodeBassDrum);
  set(d, "Bassdrum", decodeBassDrum);
  set(d, "Fruit Kick", decodeFruitKick);
  set(d, "Fruit kick", decodeFruitKick);
}

}  // namespace gl::flp::j22
