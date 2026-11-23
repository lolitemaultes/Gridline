// Gridline — "edison" pack: Loupe's repair tools (edison_repair_dsp.cpp): deverb (late room
// reverb taken out of a recording), declip (clipped peaks rebuilt) and the drum loop stretch
// (a new tempo with the hits kept sharp and on the grid). Pure functions on Content like the
// other edit operations; each takes an optional Progress the worker reports through and the
// dialog's Stop button cancels (a cancelled run returns the content unchanged).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include "edison.h"

namespace gl::packs::edison {

// Progress of a running edit (0..1), and a request to stop it. Shared by the worker and the UI.
struct Progress {
  std::atomic<float> done{0.0f};
  std::atomic<bool> cancel{false};
  void set(double f) { done.store(static_cast<float>(f), std::memory_order_relaxed); }
  bool cancelled() const { return cancel.load(std::memory_order_relaxed); }
};
inline bool stopped(Progress* p) { return p && p->cancelled(); }
inline void report(Progress* p, double f) {
  if (p) p->set(f);
}

// ---- deverb ------------------------------------------------------------------------------
// The room's decay, measured from the free decays in the audio (where a sound stops and the
// room rings on): reverb time (seconds to -60 dB) overall and per octave band.
struct DecayEstimate {
  static constexpr int kBands = 6;  // octave bands centred on 177, 354, 707, 1414, 2828, 5657 Hz
  double t60 = 0;                   // overall (0: no free decays found)
  std::array<double, kBands> band{};  // per band (0: that band had none)
  int decays = 0;                   // free decays measured
  bool valid() const { return t60 > 0; }
};
DecayEstimate estimateDecay(const Audio& a, Range r, Progress* p = nullptr);
// The band reverb time at a frequency (interpolated on a log axis; the overall value where a
// band has none).
double decayAt(const DecayEstimate& e, double hz);

struct DeverbSettings {
  double amount = 0.6;     // 0..1: how much of the estimated reverb comes out
  double t60 = 0.8;        // the room's reverb time (seconds) when there is no estimate
  bool useEstimate = true; // per band times from `estimate` (else t60 everywhere)
  DecayEstimate estimate;
  bool keepEarly = true;   // the first 50 ms of reflections stay (only the late tail comes out)
};
// Late reverb reduction: a statistical model of the room's late reverberation (what was heard
// the early-reflection time ago, decayed at the room's rate per band) is subtracted from the
// power spectrum, with gains smoothed over time and frequency so the result has no musical noise.
// The two channels share one gain (the stereo image stays). A selection fades in and out of
// the processed audio over 10 ms at its edges.
Content deverb(const Content& c, Range r, const DeverbSettings& s, Progress* p = nullptr);

// ---- declip ------------------------------------------------------------------------------
struct ClipScan {
  double peakPos = 0, peakNeg = 0;  // the range's highest and lowest sample
  double level = 0;                 // the clip level used (linear, positive side)
  std::vector<Range> regions;       // clipped regions (both channels together), sorted
  int64_t samples = 0;              // clipped samples, all channels
};
// clipDb: the level counted as clipped, in dB below the range's peak; <= -100 (or `autoLevel`)
// finds it: runs of two or more equal samples at the peak.
ClipScan findClipping(const Audio& a, Range r, bool autoLevel, double clipDb);

struct DeclipSettings {
  bool autoLevel = true;
  double clipDb = -0.1;      // manual clip level below the peak
  bool fitGain = true;       // lower the gain so the rebuilt peaks fit under the ceiling
  double ceilingDb = -0.3;   // dBFS
};
// Clipped runs are rebuilt by autoregressive interpolation (the samples that best continue the
// audio around the gap, least squares, re-estimated a few times), kept consistent with the
// clipping: a rebuilt sample never comes out below the clip level it was cut at.
Content declip(const Content& c, Range r, const DeclipSettings& s, Progress* p = nullptr, int* fixedRegions = nullptr);

// ---- denoise -------------------------------------------------------------------------------
struct DenoiseSettings {
  double reductionDb = 15;  // the deepest cut (the noise is lowered by up to this much)
  double sensitivity = 0.5; // 0..1: how much of the sound counts as noise (the profile scaled -6..+6 dB)
  double smoothing = 0.5;   // 0..1: steadier gains (fewer artefacts) against a quicker response
  int character = 0;        // 0 broadband (hiss, room noise), 1 tonal (hum, whine: finer frequency resolution)
  bool adaptive = false;    // follow the noise as it changes (no profile needed)
  NoiseProfile profile;     // learned from a part with only noise (acquireNoise)
};
// Noise reduction: the clean sound's spectrum is estimated with the log-spectral amplitude
// estimator (decision-directed a priori SNR), from a learned profile or a noise floor tracked by
// minimum statistics. Two resolutions run side by side: a long frame for the body of the
// sound, a short one around transients (no pre-echo, attacks kept).
Content denoiseHq(const Content& c, Range r, const DenoiseSettings& s, Progress* p = nullptr);

// ---- declick -------------------------------------------------------------------------------
struct DeclickSettings {
  double sensitivity = 0.5;  // 0..1: higher finds smaller clicks (crackle)
  double maxClickMs = 2.0;   // longer disturbances are left alone (they are sound, not clicks)
};
// Clicks: samples the local autoregressive model cannot predict from either side (both the
// forward and the backward prediction errors far above their robust level), sorted.
std::vector<Range> findClicks(const Audio& a, Range r, const DeclickSettings& s, Progress* p = nullptr);
// Each click is replaced by the samples that best continue the audio around it (least squares
// with the local model).
Content declickHq(const Content& c, Range r, const DeclickSettings& s, Progress* p = nullptr, int* fixed = nullptr);

// ---- gate ----------------------------------------------------------------------------------
struct GateSettings {
  double thresholdDb = -45;  // opens above this
  double hysteresisDb = 6;   // closes this far below the threshold (no chatter)
  double attackMs = 1, holdMs = 30, releaseMs = 80;
  double floorDb = -90;      // the level when closed (-90: silence)
};
// Peak-envelope gate with look-ahead of the attack time; the gain moves in decibels (natural
// releases).
Content gateHq(const Content& c, Range r, const GateSettings& s, Progress* p = nullptr);

// ---- spectral brush ------------------------------------------------------------------------
// The painted areas' gain with soft edges (15 ms in time, a sixth of an octave in frequency)
// and fine frequency resolution; both channels together.
Content spectralGainSoft(const Content& c, const std::vector<SpectralRect>& rects, double gainDb, Progress* p = nullptr);

// ---- drum loop stretch --------------------------------------------------------------------
struct LoopTempo {
  double bpm = 0;            // detected tempo (0: none)
  double bars = 0;           // the loop's length in bars at that tempo
  std::vector<int64_t> hits; // onsets (frames from the range start)
};
LoopTempo analyseLoop(const Audio& a, Range r, double sensitivity, int beatsPerBar);

struct DrumStretchSettings {
  double sourceBpm = 120;  // the loop's tempo now
  double targetBpm = 120;  // the new tempo
  double bars = 1;         // the loop's length in bars at the target tempo (its length at the source tempo keeps it)
  int beatsPerBar = 4;
  double sensitivity = 0.5;
  int tails = 0;           // 0: the sound between hits is stretched; 1: each slice plays as is and fades
  double attackMs = 30;    // each hit's attack copied unchanged
};
// The new length (frames) for a range of `frames` at these settings.
int64_t drumStretchLength(const DrumStretchSettings& s, double rate);
// The loop at the new tempo: every hit lands at its place on the new grid (start of the range
// plus its old offset scaled to the new length, to the sample), the result is exactly
// drumStretchLength long, and its end runs on into its start (no click at the loop point).
Audio drumStretchAudio(const Audio& loop, const std::vector<int64_t>& hits, int64_t newLen, const DrumStretchSettings& s, Progress* p = nullptr);
Content drumStretch(const Content& c, Range r, const DrumStretchSettings& s, Progress* p = nullptr);

}  // namespace gl::packs::edison
