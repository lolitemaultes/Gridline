// Gridline: time stretching and pitch shifting of recorded audio (J-87, J-153).
//
// One engine for every pitch / time change Gridline makes to recorded audio:
// playlist audio clips (rendered and live), tempo-synced clips, the sampler's
// time modes, the beat slicer's processed slices, the audio editor, the
// Pitch Shifter's Hi-fi engine, Seltmann's warping and Platter's key lock.
//
// There are two ways to change the speed of a sound:
//   Resample   pitch and speed together, like a turntable: band-limited
//              windowed-sinc resampling (no aliasing when pitching up);
//   Stretch    pitch and time apart: Signalsmith Stretch (MIT, third_party/
//              signalsmith-stretch), one continuous stream per clip or voice.
//              The engine only stretches time; the pitch comes from resampling
//              its output with the same band-limited kernel, so notes are in
//              tune and pitch changes glide smoothly from frame to frame.
// Offline renders and realtime voices use the same settings (a 210 ms block,
// a 30 ms hop) at every sample rate; live effects (prepare(..., live)) use a
// 120 ms block for half the latency. On every path a guard holds the output
// under the source's level +6 dB (25 ms RMS), which removes the pre-echo a
// spectral stretch leaves in the quiet ahead of a hit.
//
// J-153: the stretch is plain and continuous. It never restarts at hits,
// blocks, tempo steps or pitch changes, only where playback really jumps (a
// new note, a seek, a loop); every start is primed so its first output is
// already settled. The modes earlier versions stored (Auto, Mono, Transient,
// Formant, Voice, Beats) all play through the one stretch.
//
// J-157: Rubber Band (Rubber Band Library R3, GPL-2.0-or-later, third_party/
// rubberband) is a second way to keep the pitch, chosen per clip or sampler
// channel: offline renders run it in its offline mode (study, then process,
// with a key frame map for tempo-mapped clips), realtime voices in its
// realtime mode (fed fixed chunks, started with pre-roll, its start delay
// dropped), so both line up with the source like the stretch does. It does
// its own transient handling and its own pitch shifting; Gridline's guard
// and resampler are not used on it.
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

// Parameter values are stored in projects (SmpStretchMode): every value but
// Resample and RubberBand is the stretch. The values are kept so old projects
// and FL imports load and export unchanged (J-157 appended RubberBand).
enum class Mode : int { Resample = 0, Auto, Mono, Transient, Formant, Voice, Beats, RubberBand, Count };
// The value pickers write for the stretch.
inline constexpr Mode kStretch = Mode::Auto;
inline bool keepsPitch(Mode m) { return m != Mode::Resample; }
// The engine a mode keeps the pitch with: Gridline's stretch (Signalsmith) or Rubber Band.
enum class Engine { Stretch, RubberBand };
inline Engine engineOf(Mode m) { return m == Mode::RubberBand ? Engine::RubberBand : Engine::Stretch; }
// The three choices pickers show, in order: Stretch, Rubber Band, Resample.
inline constexpr Mode kChoices[3] = {kStretch, Mode::RubberBand, Mode::Resample};
// The choice a stored mode shows as (every older stretch mode is Stretch).
inline Mode choiceOf(Mode m) { return m == Mode::Resample || m == Mode::RubberBand ? m : kStretch; }
// English names, "Resample", "Stretch" or "Rubber Band", and a one-line hint
// for each choice (the UI wraps them in i18n::tr).
const char* modeName(Mode m);
const char* modeHint(Mode m);
// SmpStretchMode parameter value → mode (negative: Resample; past the last mode: Stretch).
Mode modeOf(double paramValue);

// FL Studio's stored stretch method (channel parameters 215, offset 108):
// 0 resample, -1 stretch, -2 stretch pro, 1 e3 generic, 2 e3 mono, 3 slice
// stretch, 4 slice map, 5 auto, 6..9 e2 generic / transient / mono / speech.
// Import keeps the closest stored mode, export the closest FL method.
Mode modeFromFl(int code);
int flCodeOf(Mode m);

struct Settings {
  Mode mode = kStretch;  // Resample, RubberBand or (any other) the stretch
  double pitch = 1.0;    // frequency ratio
};

// ---- offline (UI / worker threads) ----

// Stretches `n` frames of `nch` channels into exactly `outLen` frames: output
// frame 0 is input frame 0, the last output frame the last input frame (no
// latency, no pre-roll). Resample mode ignores s.pitch (the length sets it).
void process(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sampleRate, const Settings& s);

// Variable time map: srcAt(j) is the input position (frames, fractional) heard
// at output frame j; it must not decrease.
void processMapped(const float* const* in, int nch, int64_t n, float* const* out, int64_t outLen, double sampleRate,
                   const Settings& s, const std::function<double(int64_t)>& srcAt);

// Onset (attack start) frames of a mono signal (analysis: Seltmann's slices).
std::vector<int64_t> detectOnsets(const float* x, int64_t n, double sampleRate, double sensitivity);

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
  Mode mode = kStretch;             // Resample, RubberBand or (any other) the stretch
  double pitch = 1;                 // frequency ratio (stretch)
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
// the realtime voice until ready); otherwise rendered before returning.
RenderedClipPtr renderClip(const ClipRequest& r, bool async);
// J-196: the render renderClip would return if one is cached (finished or on
// its way), else null; never starts one (an edit in progress: the realtime
// stretch plays until the value settles).
RenderedClipPtr findRenderedClip(const ClipRequest& r);
// Blocks until every queued clip render is done (tests, exports).
void waitForPendingRenders();
// J-103 app quit: drops queued renders and joins the render thread before main() returns
// (later requests render synchronously). Idempotent.
void shutdownClipRenderer();
// Frees the cached renders nothing uses any more (memory pressure, tests).
void trimClipCache();
size_t clipCacheBytes();

// ---- realtime voice (sampler notes, live clips, effects) ----

// A streaming pitch / time stretcher: one continuous engine stream from
// start() on. prepare() allocates (UI thread); start(), render() and reset()
// are realtime safe (no allocation, no locks). The output does not depend on
// how render() calls split it. J-157: prepare(..., Engine::RubberBand) runs
// Rubber Band's realtime engine instead (about 6 MB per stereo voice at
// 44.1 kHz, twice that at 96 kHz). J-170: startLater() and prime() spread
// its start-up over the blocks before a stream plays (identical output).
class Voice {
 public:
  // Pulls the next `frames` input frames into dst[0], dst[1] (zeros past the
  // end of the sound; both are written, also for one channel).
  using Reader = void (*)(void* ctx, float* const* dst, int frames);

  Voice();
  ~Voice();
  Voice(const Voice&) = delete;
  Voice& operator=(const Voice&) = delete;
  // maxPitch / maxSpeed bound the buffers (frequency ratio, input frames per output frame).
  // live: the shorter block of live effects (about half the latency).
  void prepare(int channels, double sampleRate, double maxPitch = 8.0, double maxSpeed = 8.0, bool live = false,
               Engine engine = Engine::Stretch);
  bool prepared() const { return prepared_; }
  Engine engine() const;
  // Begins a stream (a note, a seek, a loop): speed = input frames per output
  // frame, pitch = frequency ratio. The reader's first `preroll` frames come
  // before the start point (audio that leads into it): the engine runs
  // through them so its first output is already settled, and the first
  // rendered frame is the start point (no latency).
  void start(Reader rd, void* ctx, double speed, double pitch, int preroll = 0);
  // J-170: begins a stream like start() but leaves its start-up work to
  // prime() and render(). Rubber Band works through its whole start delay
  // (half its longest window: 9 engine steps at 44.1 kHz, 17 at 96 kHz)
  // before a stream's first frame, which made a note-on cost many blocks'
  // worth of CPU at once; priming does that work in pieces over the blocks
  // before the stream plays. The engine sees the same calls with the same
  // input in the same order as after start(), so the output is identical.
  // The Stretch engine starts at once (its start-up is cheap).
  void startLater(Reader rd, void* ctx, double speed, double pitch, int preroll = 0);
  // Does start-up work of the stream begun by startLater(), about `budget`
  // input frames (at least one engine call while any is left). Returns true
  // once the stream's first frame is ready (render() then only plays on).
  // speed / pitch: what render() will be given first.
  bool prime(Reader rd, void* ctx, double speed, double pitch, int budget);
  // Input frames of start-up work still to do (an estimate; 0: ready).
  int primeLeft(double speed) const;
  // Frames the engine has been given since the stream began (Rubber Band:
  // its start pad included). Tests: where the start-up work was done.
  int64_t fedFrames() const;
  // n output frames (any n). Speed and pitch may change between calls; a
  // pitch change glides across the frames (no stepped pitch).
  void render(float* const* out, int n, double speed, double pitch, Reader rd, void* ctx);
  // Input frames the voice reads ahead of what it has played (at these settings).
  int lookahead(double speed, double pitch) const;
  // Pre-roll frames worth giving start() (one engine block at this rate).
  int prerollFor(double speed) const;
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool prepared_ = false;
};

// J-170 (tests, diagnostics): frames of sound given to Rubber Band's realtime
// engines so far, by every voice (not the silence voices feed themselves:
// start pads, the warm-up in prepare()).
int64_t rubberBandFramesFed();

}  // namespace gl::stretch
