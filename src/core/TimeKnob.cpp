// Gridline - K-230: the delay time knobs (see TimeKnob.h).
#include "TimeKnob.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include "KnobMagnet.h"
#include "Params.h"
#include "Song.h"
#include "../packs/mini/mini.h"
#include "../packs/spacefx/spacefx.h"
#include "PluginParams.h"

namespace gl::timeknob {

std::vector<double> noteTicks(double lo, double hi) { return magnet::syncDivisions(lo, hi, kPpq); }

std::vector<double> roundMs(double lo, double hi) {
  std::vector<double> out;
  for (double dec = 1; dec <= 10000; dec *= 10)
    for (double m : {1.0, 2.0, 5.0}) {
      const double v = m * dec;
      if (v >= lo - 1e-9 && v <= hi + 1e-9) out.push_back(v);
    }
  return out;
}

namespace {

Spec ticksSpec(double lo, double hi) {
  Spec s;
  s.unit = Unit::Ticks;
  s.magnets = noteTicks(lo, hi);
  s.offset = kPpq / 16;  // a 1/64 note: the bottom sliver stays small
  s.quantum = 1;
  return s;
}
Spec stepsSpec(double lo, double hi) {
  Spec s;
  s.unit = Unit::Steps;
  for (double t : noteTicks(lo * kPpq / 4, hi * kPpq / 4)) s.magnets.push_back(t / (kPpq / 4));
  return s;
}
Spec msSpec(double lo, double hi, bool linear) {
  Spec s;
  s.unit = Unit::Ms;
  s.magnets = roundMs(std::max(lo, 1.0), hi);
  if (linear) s.offset = 5, s.quantum = 1;
  return s;
}
Spec secondsSpec(double lo, double hi) {
  Spec s;
  s.unit = Unit::Seconds;
  for (double m : roundMs(lo * 1000, hi * 1000)) s.magnets.push_back(m / 1000);
  return s;
}

Spec beatsSpec(double lo, double hi) {
  Spec s;
  s.unit = Unit::Beats;
  for (double t : noteTicks(lo * kPpq, hi * kPpq)) s.magnets.push_back(t / kPpq);
  return s;
}
// A rate: one cycle per note value at the tempo (a 1/4 at 120 BPM is 2 Hz).
Spec hzSpec(double lo, double hi, double tempo) {
  Spec s;
  s.unit = Unit::Hz;
  if (!(tempo > 0)) tempo = 120;
  for (double t : noteTicks(1, kPpq * 32)) {
    const double hz = tempo / 60.0 * kPpq / t;
    if (hz >= lo - 1e-9 && hz <= hi + 1e-9) s.magnets.push_back(hz);
  }
  std::sort(s.magnets.begin(), s.magnets.end());
  return s;
}

std::string lower(std::string t) {
  for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return t;
}
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

}  // namespace

std::optional<Spec> classify(const ParamDesc& d, double tempo) {
  if (d.kind != ParamKind::Continuous || !(d.max > d.min)) return std::nullopt;
  const std::string u = lower(d.unit), n = lower(d.name);
  std::optional<Spec> s;
  if (u == "ms") {
    // A wide linear range travels by the even log curve (whole ms when dragged coarsely); a narrow or
    // logarithmic one keeps its own travel and only gets the round values.
    const bool linear = d.scale == ParamScale::Linear && d.max - d.min >= 200 && d.min >= 0;
    s = msSpec(d.min, d.max, linear);
  } else if (u == "s" || u == "sec" || u == "seconds") {
    s = secondsSpec(d.min, d.max);
  } else if (u == "beats" || u == "beat") {
    s = beatsSpec(d.min, d.max);
  } else if (u == "steps" || u == "step") {
    s = stepsSpec(d.min, d.max);
  } else if (u == "ticks") {
    s = ticksSpec(d.min, d.max);
    s->offset = 0, s->quantum = 0;  // (its own travel)
  } else if (u == "hz" && d.max <= 50 && !has(n, "cut") && !has(n, "filter") && !has(n, "pitch") && !has(n, "tune") && !has(n, "shift") &&
             !has(n, "below") && !has(n, "above") && !has(n, "freq") && !has(n, "band") && !has(n, "split")) {  // (not a band's frequency)
    s = hzSpec(d.min, d.max, tempo);  // a slow rate (LFO, tremolo, wobble, auto pan): an audio frequency goes higher
  }
  if (!s || s->magnets.size() < 2) return std::nullopt;  // nothing to aim for in this range
  return s;
}

std::optional<Spec> forParam(const std::string& type, int param, double tempo) {
  if (type == "gl.delay") {
    if (param == pp::DlyTicks) return ticksSpec(1, 768);
    if (param == pp::DlyTimeMs) return msSpec(1, 3000, false);
  } else if (type == "gl.multidelay") {
    using namespace packs::spacefx;
    if (param >= MdGlobalCount && param < MdCount) {
      const int f = (param - MdGlobalCount) % MdFieldCount;
      if (f == MdSteps) return stepsSpec(0.002, 64);
      if (f == MdTimeMs) return msSpec(0.02, 5000, false);
    }
  } else if (type == "gl.banddelay") {
    if (param == packs::spacefx::BdMaxTime) return secondsSpec(0.05, 2);
  } else if (type == "gl.vphaser") {
    if (param == packs::spacefx::VpDelayTicks) return ticksSpec(1, 768);
    if (param == packs::spacefx::VpDelayTime) return msSpec(0, 1500, true);
  } else if (type == "gl.miniecho") {
    if (param == packs::mini::echo::Time) return msSpec(1, 2000, false);
  } else if (type == "gl.minivolley") {
    if (param == packs::mini::volley::Time) return msSpec(10, 2000, false);
  } else if (type == "gl.minirewind") {
    if (param == packs::mini::rewind::Length) return msSpec(30, 2000, false);
  }
  // K-240: every other time control of a built-in plugin, from its descriptor.
  if (isHostedType(type)) return std::nullopt;
  const PluginInfo* info = findPlugin(type);
  if (!info || param < 0 || static_cast<size_t>(param) >= info->params.size()) return std::nullopt;
  return classify(info->params[static_cast<size_t>(param)], tempo);
}

std::optional<Spec> forParam(const Song& song, ParamId pid) {
  if (const Channel* c = song.channel(pid.owner)) {
    if (pid.index >= kChannelPluginBase && pid.index < kNoteFxBase) return forParam(c->plugin.type, static_cast<int>(pid.index - kChannelPluginBase), song.tempo);
    if (pid.index >= kNoteFxBase) {  // K-240: a note processor's time controls (an arpeggiator's gate, a strum)
      const uint32_t fx = (pid.index - kNoteFxBase) / kNoteFxStride, k = (pid.index - kNoteFxBase) % kNoteFxStride;
      if (fx < c->noteFx.size()) return forParam(c->noteFx[fx].type, static_cast<int>(k), song.tempo);
    }
    return std::nullopt;
  }
  if (const MixerTrack* m = song.mixerTrack(pid.owner)) {
    if (pid.index < kSlotBase) return std::nullopt;
    const uint32_t slot = (pid.index - kSlotBase) / kSlotStride, k = (pid.index - kSlotBase) % kSlotStride;
    if (slot >= m->slots.size()) return std::nullopt;
    return forParam(m->slots[slot].plugin.type, static_cast<int>(k), song.tempo);
  }
  return std::nullopt;
}

double toPos(const Spec& s, const ParamDesc& d, double v) {
  if (!(s.offset > 0) || !(d.max > d.min)) return d.toNormalized(v);
  v = std::clamp(v, d.min, d.max);
  return std::clamp(std::log((v - d.min + s.offset) / s.offset) / std::log((d.max - d.min + s.offset) / s.offset), 0.0, 1.0);
}

double toValue(const Spec& s, const ParamDesc& d, double pos, bool quantise) {
  pos = std::clamp(pos, 0.0, 1.0);
  double v = !(s.offset > 0) || !(d.max > d.min) ? d.fromNormalized(pos)
                                                   : d.min - s.offset + s.offset * std::pow((d.max - d.min + s.offset) / s.offset, pos);
  if (quantise && s.quantum > 0) v = std::round(v / s.quantum) * s.quantum;
  // a magnet stays exact (no rounding error from the log and back)
  for (double m : s.magnets)
    if (std::fabs(v - m) < 1e-7 * std::max(1.0, std::fabs(m))) v = m;
  return std::clamp(v, std::min(d.min, d.max), std::max(d.min, d.max));
}

std::string noteName(double ticks, bool compact) {
  const double whole = kPpq * 4;
  char b[32];
  if (std::fabs(ticks - std::round(ticks)) > 1e-6) return {};
  const long t = std::lround(ticks);
  if (t <= 0) return {};
  if (t % static_cast<long>(whole) == 0) {
    const long bars = t / static_cast<long>(whole);
    std::snprintf(b, sizeof b, bars == 1 ? "%ld bar" : "%ld bars", bars);
    return b;
  }
  for (int den : {2, 4, 8, 16, 32, 64}) {
    const double straight = whole / den;
    if (std::fabs(ticks - straight) < 1e-6) return "1/" + std::to_string(den);
    if (den <= 16 && std::fabs(ticks - straight * 1.5) < 1e-6) return "1/" + std::to_string(den) + (compact ? " D" : " dotted");
    if (den <= 16 && std::fabs(ticks - straight * 2.0 / 3.0) < 1e-6) return "1/" + std::to_string(den) + (compact ? " T" : " triplet");
  }
  return {};
}

namespace {
std::string msText(double ms) {
  char b[32];
  if (ms >= 1000) std::snprintf(b, sizeof b, "%.2f s", ms / 1000);
  else if (ms >= 10) std::snprintf(b, sizeof b, "%.0f ms", ms);
  else std::snprintf(b, sizeof b, "%.1f ms", ms);
  return b;
}
}  // namespace

std::string readout(const Spec& s, double v, double tempo, bool compact) {
  if (s.unit == Unit::Ms) return msText(v);
  if (s.unit == Unit::Seconds) return msText(v * 1000);
  if (s.unit == Unit::Hz) {  // K-240: "1/8 (4.00 Hz)" on a note value, else "2.37 Hz"
    char b[48];
    std::snprintf(b, sizeof b, v >= 10 ? "%.1f Hz" : "%.2f Hz", v);
    for (double m : s.magnets)
      if (std::fabs(v - m) < 1e-6 * std::max(1.0, m)) {
        const double t = (tempo > 0 ? tempo : 120.0) / 60.0 * kPpq / v;
        const std::string nn = noteName(std::round(t * 1e6) / 1e6, compact);
        if (!nn.empty()) return compact ? nn : nn + " (" + b + ")";
      }
    return b;
  }
  const double ticks = s.unit == Unit::Steps ? v * kPpq / 4 : s.unit == Unit::Beats ? v * kPpq : v;
  std::string name = noteName(ticks, compact);
  if (name.empty()) {
    char b[32];
    const double beats = ticks / kPpq;
    std::snprintf(b, sizeof b, compact ? "%.2f bt" : "%.2f beat", beats);
    name = b;
    if (!compact && std::fabs(beats - 1.0) >= 0.005) name += "s";
  }
  if (compact || !(tempo > 0)) return name;
  return name + " (" + msText(ticks / kPpq * 60000.0 / tempo) + ")";
}

}  // namespace gl::timeknob
