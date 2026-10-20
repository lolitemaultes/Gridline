// Gridline — FLP export tests (J-43): FL's clip fade record (@36 in ms, @40 in
// tension, @44 out ms, @48 out tension, @56 flags: manual, declicker, curve)
// and the per-project automatic crossfades switch (event 44), written and
// read back exactly.
#pragma once
#include <cmath>
#include <cstring>
#include "core.h"
#include "core/Fades.h"
#include "core/flp/Flp.h"

namespace j43test {

// The 80-byte playlist records of the exported file's audio clips (item < 0x5000).
inline std::vector<std::vector<uint8_t>> audioRecords(const gl::flp::Bytes& bytes, int* event44 = nullptr) {
  std::vector<std::vector<uint8_t>> out;
  gl::flp::File f;
  std::string err;
  if (!gl::flp::readBytes(bytes, f, &err)) return out;
  bool head = true;
  for (const auto& e : f.events) {
    if (e.id == 64 || e.id == 99) head = false;
    if (e.id == 44 && head && event44) *event44 = static_cast<int>(e.value());
    if (e.id != 233 || e.size % 80) continue;
    for (uint32_t o = 0; o + 80 <= e.size; o += 80)
      if (e.u16(o + 6) < 0x5000) out.emplace_back(e.data + o, e.data + o + 80);
  }
  return out;
}
inline float f32At(const std::vector<uint8_t>& r, size_t at) {
  float v;
  std::memcpy(&v, r.data() + at, 4);
  return v;
}
inline uint32_t u32At(const std::vector<uint8_t>& r, size_t at) {
  return static_cast<uint32_t>(r[at]) | static_cast<uint32_t>(r[at + 1]) << 8 | static_cast<uint32_t>(r[at + 2]) << 16 |
         static_cast<uint32_t>(r[at + 3]) << 24;
}

}  // namespace j43test

inline void testJ43Fades() {
  using namespace gl;
  using namespace gl::flp;
  // ---- FL's tension law, values worked out by hand ----
  expect(std::fabs(fadeTension(0.5, 3.0f) - (1.0 - 1.0 / 3.0)) < 1e-12 && std::fabs(fadeTension(0.5, -3.0f) - 1.0 / 3.0) < 1e-12,
         "J-43 law: t = 3 lifts the midpoint to 2/3, t = -3 drops it to 1/3");
  expect(fadeTension(0.25, 0.004f) == 0.25, "J-43 law: FL rounds the tension to 0.01 (0.004 plays plain)");
  expect(std::fabs(fadeGain(0.5, FadeShape::Smooth, 0) - 0.5) < 1e-12 && std::fabs(fadeGain(1.0 / 3, FadeShape::Smooth, 0) - 0.25) < 1e-12,
         "J-43 law: the smooth curve is a raised cosine");
  expect(fadeProgressIn(0, 4) == 0.2 && fadeProgressIn(3, 4) == 0.8 && fadeProgressOut(4, 4) == 0.8 && fadeSamples(0.0001, 44100) == 4,
         "J-43 law: an n-sample fade steps through k / (n + 1)");
  bool ui = true;
  for (float u : {-1.0f, -0.5f, -0.01f, 0.0f, 0.2f, 0.75f, 1.0f}) ui = ui && std::fabs(fadeTensionToUi(fadeTensionFromUi(u)) - u) < 1e-5f;
  expect(ui && std::fabs(fadeTensionFromUi(1.0f) - 635.0f) < 1e-2f, "J-43 law: the handle maps onto FL's tension scale and back");

  // ---- export: every fade field lands in FL's record, and comes back ----
  Song s = coretest::featureSong();
  ObjectId kick = kNoId;
  for (const auto& ch : s.channels)
    if (ch.plugin.type == "gl.sampler") kick = ch.id;
  Clip c;
  c.id = s.allocateId(), c.kind = ClipKind::Audio, c.source = kick, c.track = 7, c.start = 0, c.length = 768;
  c.fadeIn = 0.2, c.fadeInManual = true, c.fadeInTension = -3.5f, c.fadeInShape = 1, c.fadeInDeclick = 2;
  c.fadeOut = 0.3, c.fadeOutManual = true, c.fadeOutTension = 12.25f, c.fadeOutShape = 0, c.fadeOutDeclick = 1;
  s.playlist.push_back(c);
  Clip z = c;  // a hand-set zero fade in on an overlapping clip: no crossfade there
  z.id = s.allocateId(), z.start = 700, z.fadeIn = 0, z.fadeInManual = true, z.fadeInTension = 0, z.fadeInShape = 0, z.fadeInDeclick = 0;
  z.fadeOut = 0, z.fadeOutManual = false, z.fadeOutTension = 0, z.fadeOutDeclick = 0;
  s.playlist.push_back(z);
  s.autoCrossfades = false;
  Song b;
  ExportReport rep;
  Bytes bytes;
  const bool ok = kick != kNoId && coretest::exportImport(s, b, rep, &bytes);
  expect(ok, "J-43 fades: song exports and imports back");
  if (!ok) return;
  int e44 = -1;
  const auto recs = j43test::audioRecords(bytes, &e44);
  const std::vector<uint8_t>* rc = nullptr;
  for (const auto& r : recs)
    if (j43test::u32At(r, 0) == 0 && j43test::u32At(r, 8) == 768) rc = &r;
  expect(e44 == 0, "J-43 fades: automatic crossfades off is written as event 44 = 0");
  expect(rc != nullptr, "J-43 fades: the clip's 80-byte record is written");
  if (rc) {
    const auto& r = *rc;
    expect(std::fabs(j43test::f32At(r, 36) - 200.0f) < 1e-3f && std::fabs(j43test::f32At(r, 44) - 300.0f) < 1e-3f,
           "J-43 fades: lengths at @36 / @44 in ms");
    expect(j43test::f32At(r, 40) == -3.5f && j43test::f32At(r, 48) == 12.25f, "J-43 fades: tensions at @40 (in) / @48 (out)");
    const uint32_t want = 1u | 2u | (2u << 2) | (1u << 8) | (0u << 12) | (1u << 14);
    expect(j43test::u32At(r, 56) == want, "J-43 fades: flags @56 (manual bits 0/1, declicker bits 2-4 / 14-16, curve bits 8-9 / 12-13)");
  }
  const Clip* bc = nullptr;
  const Clip* bz = nullptr;
  for (const auto& x : b.playlist)
    if (x.kind == ClipKind::Audio && x.track == 7) (x.start == 0 ? bc : bz) = &x;
  expect(!b.autoCrossfades, "J-43 fades: event 44 = 0 imports as automatic crossfades off");
  expect(bc && std::fabs(bc->fadeIn - 0.2) < 1e-6 && std::fabs(bc->fadeOut - 0.3) < 1e-6 && bc->fadeInManual && bc->fadeOutManual &&
             bc->fadeInTension == -3.5f && bc->fadeOutTension == 12.25f && bc->fadeInShape == 1 && bc->fadeOutShape == 0 &&
             bc->fadeInDeclick == 2 && bc->fadeOutDeclick == 1,
         "J-43 fades: lengths, tensions, curves, manual flags and declicker modes round-trip");
  expect(bz && bz->fadeIn == 0 && bz->fadeInManual && !bz->fadeOutManual, "J-43 fades: a hand-set zero fade keeps its manual flag");
  // Off: the overlap is not crossfaded, nothing derived is written.
  TempoMap tb;
  tb.build(b);
  if (bz) expect(resolveFades(b, tb, *bz).in.len == 0, "J-43 fades: crossfades off -> no fade over the overlap");
}
