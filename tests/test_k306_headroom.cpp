// Gridline — K-306 (Tracker tester R6-10): a song made from the New project's starter kit
// exports without clipping. The kit's five instruments hit together at their default
// levels (as in the tester's song: every voice on row 0, the master at 80 %). The kit's
// channel volumes leave headroom (hits together peak near -2 dBFS) and the master's Limiter
// (-1 dB ceiling) is the safety net for rare overs.
#include <cmath>
#include <cstdio>
#include <string>
#include "Check.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "core/Templates.h"
#include "engine/Render.h"

using namespace gl;

namespace {

// The tester's pattern: kick and snare on every beat, clap on 2 and 4, eighth hats, a synth line.
void writeSong(Song& s) {
  Pattern& p = s.patterns[0];
  p.bars = 2;
  const Tick q = s.ppq, e = s.ppq / 2;
  auto add = [&](const std::string& name, Tick start, Tick len, int pitch) {
    Note n;
    n.start = start, n.length = len, n.pitch = pitch, n.velocity = 100;
    for (const auto& c : s.channels)
      if (c.name == name) p.notes[c.id].push_back(n);
  };
  for (int b = 0; b < 8; ++b) {
    add("Kick", b * q, q, 60);
    add("Snare", b * q, q, 60);
    if (b % 2 == 1) add("Clap", b * q, q, 60);
    add("Hat", b * q, e, 60), add("Hat", b * q + e, e, 60);
    add("Synth", b * q, e, 60 + (b % 4) * 4), add("Synth", b * q + e, e, 67);
  }
}

struct Peak {
  double peak = 0;
  long over = 0;  // samples at or past full scale
};
Peak measure(const Song& s) {
  RenderOptions opt;
  opt.patternMode = true;
  opt.pattern = s.patterns[0].id;
  opt.tail = s.ppq * 2;
  const AudioBuffer b = renderSong(s, opt);
  Peak r;
  for (size_t c = 0; c < b.channels; ++c)
    for (size_t i = 0; i < b.frames; ++i) {
      const double v = std::fabs(static_cast<double>(b.at(c, i)));
      r.peak = std::max(r.peak, v);
      if (v >= 1.0) ++r.over;
    }
  return r;
}

}  // namespace

int main() {
  Song kit = addStarterKit(makeEmptySong());
  expect(kit.channels.size() == 5, "starter kit: five instruments");
  const MixerTrack* master = nullptr;
  for (const auto& m : kit.mixer)
    if (m.id == kit.masterId()) master = &m;
  expect(master && master->slots.back().plugin.type == "gl.limiter", "starter kit: the master ends with the Limiter");
  writeSong(kit);
  const Peak with = measure(kit);
  std::printf("# starter kit render: peak %.4f, %ld samples at full scale\n", with.peak, with.over);
  expect(with.over == 0 && with.peak <= std::pow(10.0, -0.9 / 20.0), "starter kit song: no sample reaches full scale (peak under -0.9 dB)");
  // Gain staging: before the Limiter the kit's hits together peak around -3..-1 dBFS, so the
  // Limiter (ceiling -1 dB) takes off 1 dB or less and the kit is not squashed.
  Song bare = kit;
  for (auto& m : bare.mixer)
    if (m.id == bare.masterId()) m.slots.back() = EffectSlot{};
  const Peak without = measure(bare);
  const double preDb = 20.0 * std::log10(without.peak), grDb = std::max(0.0, preDb - (-1.0));
  std::printf("# without the Limiter: peak %.4f (%.2f dBFS), %ld samples at full scale; the Limiter takes off up to %.2f dB\n", without.peak, preDb,
              without.over, grDb);
  expect(preDb >= -4.0 && preDb <= 0.0, "starter kit: hits together peak between -4 and 0 dBFS before the Limiter");
  expect(grDb <= 1.0, "starter kit: the Limiter takes off 1 dB or less");
  // Each voice still speaks: none is buried under the others (solo peaks within 12 dB of the loudest).
  double loud = 0, quiet = 1e9;
  for (const auto& c : kit.channels) {
    Song solo = bare;
    for (auto& o : solo.channels) o.muted = o.id != c.id;
    const double pk = measure(solo).peak;
    std::printf("#   %s alone: %.2f dBFS\n", c.name.c_str(), 20.0 * std::log10(pk));
    loud = std::max(loud, pk), quiet = std::min(quiet, pk);
  }
  expect(quiet > 0 && 20.0 * std::log10(loud / quiet) <= 12.0, "starter kit: every voice within 12 dB of the loudest");
  return finish();
}
