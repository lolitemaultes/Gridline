// Gridline - how the Stage shapes a mixer track's sound (docs/stage-view.md). Every part runs in the
// track's strip after the strip EQ, and every part is skipped at its neutral value, so a project that
// never touches the Stage renders bit for bit as before:
//   - distance (MixerTrack::depth, 0 front .. 1 back): level r^-k with r = 1 + 3 d and k from the
//     room's depth strength (0.5 / 0.75 / 1.0: -6 / -9 / -12 dB at the back); an air shelf whose corner
//     moves from 6 kHz down to 3.5 kHz while its cut grows to -9 dB; the image narrows to 65 %;
//   - focus (MixerTrack::focus, -1 soft .. +1 present): a broad presence bell at 2.8 kHz (+-3 dB) and
//     the room send 6 dB drier (present) or wetter (soft);
//   - the pan law (Song::panLaw): Balance (the old law), Constant power, or Stage placement, where each
//     input channel is placed on its own (left at pan - half width, right at pan + half width), so a
//     wide track moved to the side keeps both its channels;
//   - the room send (Send::stageRoom): tapped before the distance gain (the room sound does not fall
//     with distance while the dry sound does), the front row 6 dB quieter in it, a pre-delay that
//     shrinks with distance (20 ms at the front, 2 ms at the back) and three early reflections from
//     the walls of a 12 x 8 m stage, added to the room track after its reverb.
// Realtime-safe: fixed-size state, no allocation, no locks.
#pragma once
#include <algorithm>
#include <cmath>
#include "Dsp.h"

namespace gl::depth {

// ---- distance ----
constexpr double kShelfDb = -9.0;      // the air shelf's cut at the back
constexpr double kShelfHzFront = 6000.0, kShelfHzBack = 3500.0;
constexpr double kShelfQ = 0.7;        // the strip EQ's default shelf slope (an FL export bakes the shelf there)
constexpr double kNarrow = 0.35;       // side gain lost at the back
// The exponent of the distance gain for a depth strength 0 .. 1 (gentle .. natural .. strong).
inline double strengthK(double strength) { return 0.5 + 0.5 * std::clamp(strength, 0.0, 1.0); }
// Linear gain at distance `d`: exactly 1 at 0.
inline double gain(double d, double k = 0.75) {
  return d > 0 ? std::pow(1.0 + 3.0 * std::clamp(d, 0.0, 1.0), -k) : 1.0;
}
// The side gain factor at `d` (multiplies the strip's width 1 + separation).
inline double narrow(double d) { return d > 0 ? 1.0 - kNarrow * std::clamp(d, 0.0, 1.0) : 1.0; }
// The air shelf's gain (dB) and corner (Hz) at `d`.
inline double shelfDb(double d) { return kShelfDb * std::clamp(d, 0.0, 1.0); }
inline double shelfHz(double d) { return kShelfHzFront * std::pow(kShelfHzBack / kShelfHzFront, std::clamp(d, 0.0, 1.0)); }

// ---- focus ----
constexpr double kFocusHz = 2800.0, kFocusQ = 0.6, kFocusDb = 3.0, kFocusRoomDb = -6.0;
inline double focusDb(double f) { return kFocusDb * std::clamp(f, -1.0, 1.0); }
// The room send factor of a focus: present is drier, soft is wetter.
inline double focusRoom(double f) { return f != 0 ? dsp::dbToGain(kFocusRoomDb * std::clamp(f, -1.0, 1.0)) : 1.0; }

// ---- the room send ----
// The share of the room send a track at distance `d` gets (the front row 6 dB less), on top of
// undoing its distance gain (the post-fader tap is taken "before" the distance gain).
inline double front(double d, bool frontQuiet) { return frontQuiet ? 0.5 + 0.5 * std::clamp(d, 0.0, 1.0) : 1.0; }
inline double roomScale(double d, double focus, double k, bool frontQuiet) {
  return front(d, frontQuiet) / gain(d, k) * focusRoom(focus);
}
// Pre-delay of the room send (seconds): near sources wait longer for their room sound.
inline double preDelaySec(double d) { return 0.002 + 0.018 * (1.0 - std::clamp(d, 0.0, 1.0)); }

// ---- the pan law ----
// Strip gains: diagonal (gl: left in -> left out, gr: right in -> right out) and, for Stage placement,
// the cross terms (xlr: left in -> right out, xrl: right in -> left out). `g` is the fader and distance
// gain. `law` 0 Balance, 1 Constant power, 2 Stage placement. `sep` is the stereo separation (-1 .. 1).
struct PanGains {
  double gl = 1, gr = 1, xlr = 0, xrl = 0;
  bool cross = false;  // Stage placement: the cross terms are in use
};
// The half width of a track's image in pan units (Stage placement): 0 mono .. 1 as recorded.
inline double halfWidth(double sep) { return std::clamp(1.0 + std::min(sep, 0.0), 0.0, 1.0); }
inline PanGains panGains(int law, double g, double pan, double sep) {
  PanGains o;
  pan = std::clamp(pan, -1.0, 1.0);
  if (law == 1) {  // constant power: centre unity, a hard pan +3 dB on its side
    const double t = (pan + 1.0) * dsp::kPi * 0.25;
    o.gl = g * std::sqrt(2.0) * std::cos(t);
    o.gr = g * std::sqrt(2.0) * std::sin(t);
    return o;
  }
  if (law == 2) {
    const double s = halfWidth(sep);
    if (pan == 0 && s >= 1.0) {  // the centre, as recorded: exactly the input
      o.gl = o.gr = g;
      return o;
    }
    const double pl = std::clamp(pan - s, -1.0, 1.0), pr = std::clamp(pan + s, -1.0, 1.0);
    const double tl = (pl + 1.0) * dsp::kPi * 0.25, tr = (pr + 1.0) * dsp::kPi * 0.25;
    // Two placed channels: unity for a stereo image at the centre (s = 1), the constant power law
    // for a mono one (s = 0), a smooth blend between.
    const double c = g * std::cos((1.0 - s) * dsp::kPi * 0.25);
    o.gl = c * std::cos(tl);
    o.xlr = c * std::sin(tl);
    o.xrl = c * std::cos(tr);
    o.gr = c * std::sin(tr);
    o.cross = true;
    return o;
  }
  double l = 1, r = 1;  // Balance (dsp::panGains)
  dsp::panGains(pan, l, r);
  o.gl = g * l;
  o.gr = g * r;
  return o;
}

// ---- per-strip filters: the air shelf and the focus bell ----
struct ShelfState {
  dsp::Biquad bq[2], fb[2];
  float key = -1.0f, fkey = 0.0f;  // the distance / focus the coefficients are for
  double sr = 0;
  bool on = false, fon = false;

  // Filters L / R in place. A new value only changes the coefficients (the memory stays), so
  // automation glides without clicks.
  void process(float d, float focus, double sampleRate, float* L, float* R, int n) {
    if (sampleRate != sr) key = -1.0f, fkey = 0.0f, sr = sampleRate;
    if (d > 0) {
      if (!on) bq[0].clear(), bq[1].clear();
      on = true;
      if (d != key) {
        dsp::Biquad c;
        c.set(dsp::Biquad::HighShelf, sampleRate, shelfHz(d), kShelfQ, shelfDb(d));
        for (auto& b : bq) b.b0 = c.b0, b.b1 = c.b1, b.b2 = c.b2, b.a1 = c.a1, b.a2 = c.a2;
        key = d;
      }
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(bq[0].process(L[i]));
        R[i] = static_cast<float>(bq[1].process(R[i]));
      }
    } else {
      on = false;
    }
    if (focus != 0) {
      if (!fon) fb[0].clear(), fb[1].clear();
      fon = true;
      if (focus != fkey) {
        dsp::Biquad c;
        c.set(dsp::Biquad::Peak, sampleRate, kFocusHz, kFocusQ, focusDb(focus));
        for (auto& b : fb) b.b0 = c.b0, b.b1 = c.b1, b.b2 = c.b2, b.a1 = c.a1, b.a2 = c.a2;
        fkey = focus;
      }
      for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(fb[0].process(L[i]));
        R[i] = static_cast<float>(fb[1].process(R[i]));
      }
    } else {
      fon = false;
    }
  }
  // Both back at neutral: nothing runs (the memory is cleared when a part starts again).
  void off() { on = fon = false; }
};

// ---- the room tap of one track: pre-delay and early reflections ----
// The geometry (the room type's, core/StageRooms.h; the studio's by default): the listener at (0, 0),
// side walls at x = +-halfW, the back wall at y = depth; a track at pan p and distance d stands at
// x = 0.75 halfW p, y = 1.5 + 0.75 depth d. `wall` is how much of a reflection a wall gives back.
struct EarlyTap {
  double delaySec = 0, gain = 0, toL = 0, toR = 0;
};
constexpr double kEarlyMaxSec = 0.075;  // the ring holds this much at 192 kHz
inline void earlyTaps(double pan, double d, EarlyTap out[3], double halfW = 6.0, double depth = 8.0, double wall = 0.5) {
  const double x = 0.75 * halfW * std::clamp(pan, -1.0, 1.0), y = 1.5 + 0.75 * depth * std::clamp(d, 0.0, 1.0);
  const double direct = std::hypot(x, y);
  const double paths[3] = {std::hypot(x + 2.0 * halfW, y), std::hypot(2.0 * halfW - x, y), std::hypot(x, 2.0 * depth - y)};  // left, right, back wall
  const double sides[3][2] = {{0.85, 0.25}, {0.25, 0.85}, {0.55, 0.55}};
  for (int k = 0; k < 3; ++k) {
    out[k].delaySec = std::min(kEarlyMaxSec, (paths[k] - direct) / 343.0);
    out[k].gain = wall * direct / paths[k];  // -6 dB per doubling of the path, part lost to the wall
    out[k].toL = sides[k][0];
    out[k].toR = sides[k][1];
  }
}

constexpr int kRing = 32768;  // 20 ms pre-delay and 75 ms reflections after a block at 192 kHz
constexpr int kTapBlock = 1024;  // the engine's largest block (kEngineMaxBlock)
struct RoomTap {
  float ringL[kRing] = {}, ringR[kRing] = {};
  int w = 0;
  float outL[kTapBlock] = {}, outR[kTapBlock] = {};

  // Writes this block of the track's (post-fader) output and reads it back `delay` samples later into
  // outL / outR. `in` null: silence.
  void run(const float* inL, const float* inR, int n, int delay) {
    delay = std::clamp(delay, 0, kRing - 1);
    for (int i = 0; i < n; ++i) {
      ringL[w] = inL ? inL[i] : 0.0f;
      ringR[w] = inR ? inR[i] : 0.0f;
      const int r = (w - delay + kRing) & (kRing - 1);
      outL[i] = ringL[r];
      outR[i] = ringR[r];
      w = (w + 1) & (kRing - 1);
    }
  }
  // Adds the reflections of the block just written (`n` frames) into eL / eR, scaled by `level`.
  void early(const EarlyTap taps[3], double sampleRate, float level, float* eL, float* eR, int n) const {
    for (int k = 0; k < 3; ++k) {
      const int dly = std::clamp(static_cast<int>(std::lround(taps[k].delaySec * sampleRate)), 1, kRing - kTapBlock - 1);
      const float gl = static_cast<float>(taps[k].gain * taps[k].toL) * level, gr = static_cast<float>(taps[k].gain * taps[k].toR) * level;
      int r = (w - n - dly + 2 * kRing) & (kRing - 1);
      for (int i = 0; i < n; ++i) {
        const float m = 0.5f * (ringL[r] + ringR[r]);
        eL[i] += m * gl;
        eR[i] += m * gr;
        r = (r + 1) & (kRing - 1);
      }
    }
  }
};

// The room track's reflections input: what its senders' taps add after its effects.
struct RoomEarly {
  float L[kTapBlock] = {}, R[kTapBlock] = {};
  bool any = false;
};

}  // namespace gl::depth
