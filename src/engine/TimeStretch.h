// Gridline — high-quality time stretching and pitch shifting (J-87).
//
// One place for every pitch / time change Gridline makes to recorded audio:
// playlist audio clips, the sampler's time modes, the beat slicer's processed
// slices, the audio editor and the Pitch Shifter's "Hi-fi" engine.
//
// The spectral engine is Signalsmith Stretch (MIT, third_party/signalsmith-
// stretch): pitch and time independently, phase-coherent across channels,
// optional formant preservation. Gridline adds its own modes on top:
//   Resample   pitch and speed together, band-limited windowed-sinc
//              resampling (no aliasing when pitching up);
//   Auto       general purpose: full-size spectral blocks; strong onsets
//              (drum hits) are kept sharp by the transient layer;
//   Mono       monophonic material, vocals, bass: shorter blocks (tighter
//              timing, less smearing of note starts);
//   Transient  drum loops, percussive material: short blocks and every
//              detected onset re-attacked from the original audio;
//   Formant    pitch shift that keeps the spectral envelope (voices keep
//              their character instead of sounding chipmunk / giant);
//   Voice      (J-140) time-domain pitch-synchronous engine for single
//              voices and speech: no phasiness, formants kept;
//   Beats      (J-140) time-domain WSOLA engine for drums and loops, every
//              onset restarts it: hits are copied, never re-synthesised.
// Offline renders (clips, slices, the editor) use higher overlap than the
// realtime voices (sampler notes, the Pitch Shifter).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Sample.h"
#include "pitch/Kernel.h"

namespace gl {
class TempoMap;
}

namespace gl::stretch {

// Parameter values are stored in projects: new modes are appended (J-140: Voice, Beats).
enum class Mode : int { Resample = 0, Auto, Mono, Transient, Formant, Voice, Beats, Count };
// English names (the UI wraps them in i18n::tr).
const char* modeName(Mode m);
const char* modeHint(Mode m);
// SmpStretchMode parameter value → mode (clamped).
Mode modeOf(double paramValue);
inline bool keepsPitch(Mode m) { return m != Mode::Resample; }
// Modes whose engines restart at the sound's onsets, and their detection sensitivity.
inline bool usesOnsets(Mode m) { return m == Mode::Auto || m == Mode::Transient || m == Mode::Beats; }
inline double onsetSensitivity(Mode m) { return m == Mode::Transient ? 0.75 : m == Mode::Beats ? 0.6 : 0.25; }

// FL Studio's stored stretch method (channel parameters 215, offset 108):
// 0 resample, -1 stretch, -2 stretch pro, 1 e3 generic, 2 e3 mono, 3 slice
// stretch, 4 slice map, 5 auto, 6..9 e2 generic / transient / mono / speech.
// Import picks the closest Gridline mode, export the closest FL method.
Mode modeFromFl(int code);
int flCodeOf(Mode m);

struct Settings {
  Mode mode = Mode::Auto;
  double pitch = 1.0;    // frequency ratio
  double formant = 0.0;  // extra formant shift, semitones (Formant mode keeps formants, then moves them by this)
};

// ---- offline (UI / worker threads) ----

// Stretches `n` frames of `nch` channels into exactly `outLen` frames: output
// frame 0 is input frame 0, the last output frame the last input frame (no
// latency, no pre-roll). Resample mode ignores s.pitch (the length sets it).
void process(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sampleRate, const Settings& s);

// Variable time map: srcAt(j) is the input position (frames, fractional) heard
// at output frame j; it must not decrease. Evaluated once per ~10 ms of output.
void processMapped(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sampleRate,
                   const Settings& s, const std::function<double(int64_t)>& srcAt);

// Onset (attack start) frames of a mono signal, for the transient layer and tests.
std::vector<int64_t> detectOnsets(const float* x, int64_t n, double sampleRate, double sensitivity);
// Onsets of a sample (channel mix), cached per sample and sensitivity (UI thread).
std::shared_ptr<const std::vector<int64_t>> onsetsOf(const SamplePtr& s, double sensitivity);
// J-145: the harmonic part of a sample (pitch/Hpss.h, at the onsets of
// onsetsOf(s, sensitivity)), for the realtime hybrid stretch (Voice::start
// with harmonic); the rest of the sample is its percussive part. Cached per
// sample and sensitivity (UI thread). Null for an empty sample
// or one longer than kHarmonicMaxSeconds (the split is not worth the wait:
// such samples stretch on one continuous engine).
constexpr double kHarmonicMaxSeconds = 120.0;
SamplePtr harmonicOf(const SamplePtr& s, double sensitivity);

// ---- band-limited resampling (engine/pitch/Kernel.h) ----

// Windowed-sinc interpolation (Kaiser window) with the cutoff following the
// read rate, so pitching up never aliases. Realtime: 24 zero crossings per
// side (8 above rate 6); offline: 48. One implementation: pitch::.
using SincTable = pitch::SincTable;
using Quality = pitch::Quality;
inline const SincTable& sincTable(Quality q) { return pitch::table(q); }  // built once; call on a non-realtime thread first

// One frame read at fractional position `pos` of x[0, n) (zero outside) for a
// read rate `rate` (input frames per output frame; > 1 lowers the cutoff).
// Realtime safe once sincTable(q) exists.
inline float readSinc(const float* x, int64_t n, double pos, double rate, const SincTable& t) { return pitch::read(x, n, pos, rate, t); }
inline void readSinc2(const float* l, const float* r, int64_t n, double pos, double rate, const SincTable& t, float& outL, float& outR) {
  pitch::read2(l, r, n, pos, rate, t, outL, outR);
}

// Offline resampling of a whole channel: out[j] = x(start + j · rate).
inline void resample(const float* x, int64_t n, double start, double rate, float* out, int64_t outLen, Quality q = Quality::Offline) {
  pitch::resample(x, n, start, rate, out, outLen, q);
}

// ---- rendered playlist clips ----

// The audio a playlist clip plays, rendered once (offline quality) at the
// engine sample rate: frame j is what the clip plays j frames after its start.
// Built on a worker thread for the app (ready flips once, then the data never
// changes), synchronously for offline renders.
struct RenderedClip {
  std::vector<float> L, R;  // sized when created, written by the renderer
  SamplePtr source;         // kept alive: the cache key names it by address
  std::atomic<int64_t> readyFrames{0};  // frames [0, readyFrames) are final (renders publish as they go)
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  int64_t frames() const { return static_cast<int64_t>(L.size()); }
};
using RenderedClipPtr = std::shared_ptr<const RenderedClip>;

struct ClipRequest {
  SamplePtr sample;
  double srcStart = 0, srcEnd = 0;  // source window (seconds)
  bool reverse = false;
  Mode mode = Mode::Auto;
  double pitch = 1;                 // frequency ratio (stretch modes)
  double rate = 44100;              // output (engine) sample rate
  int64_t frames = 0;               // output frames
  // Time map: constant `speed` (source seconds per output second), or
  // tempo-synced through `map` (source seconds per song tick, from the clip's
  // start tick / second).
  double speed = 1;
  const TempoMap* map = nullptr;
  double secPerTick = 0, startTick = 0, startSec = 0;
  std::string mapKey;               // identifies the tempo map (cache key)
};
// Returns the rendered clip (cached by content), or null when nothing needs
// rendering. async: queued on the background renderer (playback falls back to
// the realtime path until ready); otherwise rendered before returning.
RenderedClipPtr renderClip(const ClipRequest& r, bool async);
// Blocks until every queued clip render is done (tests, exports).
void waitForPendingRenders();
// J-103 app quit: drops queued renders and joins the render thread before main() returns
// (later requests render synchronously). Idempotent.
void shutdownClipRenderer();
// Frees the cached renders nothing uses any more (memory pressure, tests).
void trimClipCache();
size_t clipCacheBytes();

// ---- realtime voice (sampler notes, effects) ----

// A streaming pitch / time stretcher. The spectral engine only stretches time;
// the pitch comes from a band-limited resampler on its output, so notes are
// in tune to a fraction of a cent. prepare() allocates (UI thread); start(),
// render() and reset() are realtime safe (no allocation, no locks).
class Voice {
 public:
  // Pulls the next `frames` input frames into dst[0], dst[1] (zeros past the
  // end of the sound; both are written, also for one channel). dst has four
  // channels: a sound started with `harmonic` also writes its harmonic part
  // (harmonicOf) into dst[2], dst[3]. Returns the index in dst of the first
  // onset (a hit) among the frames, −1 if none; reading always goes on
  // (J-145: no reader stops at onsets any more). `resume` is always false
  // (kept for the signature).
  using Reader = int (*)(void* ctx, float* const* dst, int frames, bool resume);

  Voice();
  ~Voice();
  Voice(const Voice&) = delete;
  Voice& operator=(const Voice&) = delete;
  // maxPitch / maxSpeed bound the buffers (frequency ratio, input frames per output frame).
  // light: a spectral mode runs on the time-domain engine (WSOLA) instead, a
  // fraction of the cost, for voices past a spectral budget (J-140).
  void prepare(int channels, double sampleRate, Mode mode, double maxPitch = 8.0, double maxSpeed = 8.0, bool light = false);
  bool prepared() const { return prepared_; }
  Mode mode() const { return mode_; }
  // Begins a sound: the first rendered frame is the reader's first frame
  // (no latency). speed = input frames per output frame, pitch = frequency
  // ratio. attack: the first milliseconds come straight from the input.
  // harmonic (J-145, the onset modes Auto / Transient / Beats): the reader
  // supplies the harmonic part and reports onsets; the voice then runs the
  // hybrid stretch (the harmonic part through one continuous engine, the
  // rest restarted at each hit, see process()). Without it the onset modes
  // run one continuous engine.
  void start(Reader rd, void* ctx, double speed, double pitch, bool attack = true, bool harmonic = false);
  // n output frames (any n). Speed and pitch may change between calls; a
  // pitch change ramps across the call (no stepped pitch).
  void render(float* const* out, int n, double speed, double pitch, double formantSemis, Reader rd, void* ctx);
  // Input frames the voice reads ahead of what it has played (at these settings).
  int lookahead(double speed, double pitch) const;
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Mode mode_ = Mode::Auto;
  bool prepared_ = false;
};

}  // namespace gl::stretch
