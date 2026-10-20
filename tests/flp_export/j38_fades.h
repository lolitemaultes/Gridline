// Gridline — FLP export tests (J-38): audio clip fades and automatic crossfades
// round trip through FL's clip record (@36 fade in ms, @44 fade out ms).
#pragma once
#include <cmath>
#include "core.h"
#include "core/Fades.h"

inline void testJ38Fades() {
  using namespace gl;
  using namespace gl::flp;
  Song s = coretest::featureSong();
  ObjectId kick = kNoId;
  for (const auto& ch : s.channels)
    if (ch.plugin.type == "gl.sampler") kick = ch.id;
  auto audio = [&](int track, Tick start, Tick len) {
    Clip c;
    c.id = s.allocateId(), c.kind = ClipKind::Audio, c.source = kick, c.track = track, c.start = start, c.length = len;
    s.playlist.push_back(c);
    return c.id;
  };
  // Two clips crossfading automatically over 96 ticks, one with a hand-set fade-in.
  const ObjectId A = audio(5, 0, 384), B = audio(5, 288, 384), C = audio(6, 0, 192);
  s.clip(C)->fadeIn = 0.1, s.clip(C)->fadeInManual = true;
  TempoMap tm;
  tm.build(s);
  const double ov = tm.secondsAt(384) - tm.secondsAt(288);
  Song b;
  ExportReport rep;
  const bool ok = kick != kNoId && coretest::exportImport(s, b, rep);
  expect(ok, "J-38 fades: song with audio clip fades exports and imports back");
  if (!ok) return;
  const Clip *ba = b.clip(A), *bb = b.clip(B), *bc = b.clip(C);
  if (!ba || !bb || !bc) {  // ids may be renumbered: match by track and start
    ba = bb = bc = nullptr;
    for (const auto& c : b.playlist)
      if (c.kind == ClipKind::Audio) (c.track == 6 ? bc : c.start == 0 ? ba : bb) = &c;
  }
  expect(ba && bb && bc, "J-38 fades: the audio clips come back");
  if (!ba || !bb || !bc) return;
  TempoMap tb;
  tb.build(b);
  const ClipFades fa = resolveFades(b, tb, *ba), fb = resolveFades(b, tb, *bb);
  expect(b.autoCrossfades && std::fabs(fa.out.len - ov) < 1e-4 && std::fabs(fb.in.len - ov) < 1e-4 && fa.out.automatic && fb.in.automatic,
         "J-38 fades: automatic crossfades are written as FL fade lengths and come back automatic (event 44 on)");
  expect(ba->fadeOut == 0 && bb->fadeIn == 0 && !ba->fadeOutManual && !bb->fadeInManual,
         "J-43 fades: a stored FL crossfade equal to the overlap is not kept on the clip (it follows the clips)");
  expect(std::fabs(bc->fadeIn - 0.1) < 1e-4 && bc->fadeInManual && bc->fadeOut == 0, "J-38 fades: a hand-set fade round-trips (manual flag @56 bit 0)");
  expect(fb.in.crossfade && fb.in.shape == FadeShape::Smooth,
         "J-38 fades: the re-imported overlap still plays as a smooth crossfade of the same length");
}
