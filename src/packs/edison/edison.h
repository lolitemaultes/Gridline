// Gridline — "edison" pack: audio editor / recorder (gl.edison), modelled on
// the workflow of FL Studio's Edison (function and layout only; all code,
// names and wording are Gridline's own).
//
// The pack has three layers:
//   * an offline editing model (Audio, Content, History) with pure edit
//     operations, analysis and file I/O — UI-free, used by the tests;
//   * the mixer-slot effect (EdisonFx): passes its input through, captures it
//     into a lock-free ring for recording, and plays the edited audio;
//   * the editor (edison_*_ui.cpp): waveform / spectral views, tools, menus,
//     recording, and a standalone window opened from any sample or audio clip.
//
// Audio lives in a sample file referenced by path + content hash (rendered to
// "<project>/Audio/Edison/…"); PluginState::data keeps a small JSON state.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"
#include "../../engine/Sample.h"

namespace gl::packs::edison {

inline constexpr const char* kType = "gl.edison";

// ------------------------------------------------------------------ parameters
enum Param {
  EdRecMode,     // when to start recording (RecMode)
  EdRecMax,      // maximum recording length (recMaxSeconds)
  EdThreshold,   // input trigger level, dBFS
  EdPreRoll,     // seconds kept from before the trigger
  EdMuteInput,   // the slot outputs no input signal
  EdAppend,      // recordings are added after the existing audio
  EdLoop,        // playback loops (selection, loop points or the whole file)
  EdOutGain,     // playback level, dB
  EdFollowHost,  // playback starts / stops with the song transport
  EdCount
};
enum RecMode { RecNow = 0, RecOnInput = 1, RecInput = 2, RecOnPlay = 3 };
// EdRecMax choice → seconds (0 = unlimited).
double recMaxSeconds(int choice);

// ------------------------------------------------------------------ editing model
struct Audio {
  double rate = 44100.0;
  int channels = 2;          // 1 or 2; mono audio keeps R identical to L
  std::vector<float> L, R;   // always equal length
  int64_t frames() const { return static_cast<int64_t>(L.size()); }
  double seconds() const { return rate > 0 ? static_cast<double>(frames()) / rate : 0.0; }
  void resize(int64_t n);
  float* ch(int c) { return c == 0 ? L.data() : R.data(); }
  const float* ch(int c) const { return c == 0 ? L.data() : R.data(); }
};
using AudioPtr = std::shared_ptr<const Audio>;
Audio makeAudio(double rate, int channels, int64_t frames);

struct Range {
  int64_t a = 0, b = 0;  // frames [a, b)
  int64_t len() const { return b - a; }
  bool empty() const { return b <= a; }
};
struct Marker {
  int64_t pos = 0;
  std::string name;
};
struct Region {
  int64_t start = 0, end = 0;
  std::string name;
};

// One version of the edited material: audio + markers, regions, loop points.
struct Content {
  AudioPtr audio;
  std::vector<Marker> markers;  // sorted by position
  std::vector<Region> regions;  // sorted by start
  int64_t loopStart = -1, loopEnd = -1;  // -1 = no loop points
  int64_t frames() const { return audio ? audio->frames() : 0; }
  double rate() const { return audio ? audio->rate : 44100.0; }
  const Audio& a() const;
  bool hasLoop() const { return loopStart >= 0 && loopEnd > loopStart; }
};
Content makeContent(Audio a);
// Range clamped to the content ([0, frames]).
Range clampRange(const Content& c, Range r);
// The selection or, when it is empty, the whole file.
Range rangeOrAll(const Content& c, Range r);

// Undo history of an editor (snapshots; audio buffers are shared between
// snapshots, so an edit that keeps the audio costs nothing).
class History {
 public:
  History();
  explicit History(Content c);
  const Content& cur() const { return cur_; }
  void reset(Content c);
  void push(const std::string& label, Content next);
  bool undo();
  bool redo();
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().first; }
  std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().first; }
  // Labels of the undo steps, oldest first.
  std::vector<std::string> labels() const;
  uint64_t serial() const { return serial_; }  // changes with every content change
  void setMaxLevels(size_t n) { max_ = n < 1 ? 1 : n; }

 private:
  Content cur_;
  std::vector<std::pair<std::string, Content>> undo_, redo_;
  size_t max_ = 64;
  uint64_t serial_ = 1;
};

// ---- edit operations (pure: return the new version) --------------------------
Audio extract(const Audio& a, Range r);
Content deleteRange(const Content& c, Range r);
Content insertAudio(const Content& c, int64_t at, const Audio& clip);  // clip converted to the content's rate/channels
Content replaceRange(const Content& c, Range r, const Audio& clip);   // paste over a selection
Content insertSilence(const Content& c, int64_t at, int64_t frames);
Content silenceRange(const Content& c, Range r);
Content cropTo(const Content& c, Range r);                         // keep only the range (trim)
Content trimSilence(const Content& c, double thresholdDb, Range* kept = nullptr);  // trim side noise

enum class Curve { Linear = 0, Fast, Slow, SCurve, EqualPower, Count };
const char* curveName(Curve c);
// Gain of a fade-in curve at x (0..1); a fade-out uses 1 - x.
double curveGain(Curve c, double x);
// Fades: the first sample of a fade-in is silent and its last one at full level
// (and the reverse for fade-outs).
Content fadeIn(const Content& c, Range r, Curve k = Curve::Linear);
Content fadeOut(const Content& c, Range r, Curve k = Curve::Linear);
Content crossfadeLoop(const Content& c, Range loop, int64_t fadeFrames);  // smooths the loop seam
Content gain(const Content& c, Range r, double db);
Content normalize(const Content& c, Range r, double targetDb = 0.0);
Content reverse(const Content& c, Range r);
Content invert(const Content& c, Range r);
Content removeDc(const Content& c, Range r);
Content swapChannels(const Content& c);
Content toMono(const Content& c);
Content toStereo(const Content& c);
Content applyEnvelope(const Content& c, Range r, const std::vector<std::pair<double, double>>& points);  // (x 0..1, gain)

// Sample-rate conversion (Kaiser-windowed sinc). quality 0 fast, 1 normal, 2 best.
Audio resampleAudio(const Audio& a, double newRate, int quality = 1);
Content resample(const Content& c, double newRate, int quality = 1);

// J-87: Elastic / Transient / Mono / Formant run on engine/TimeStretch (its
// Auto, Transient, Mono / vocal and Formant keep modes); Tape is varispeed.
enum class Stretch { Elastic = 0, Transient, Tape, Mono, Formant, Count };
const char* stretchName(Stretch s);
// Time factor (2 = twice as long) and pitch shift in semitones. Tape couples them
// (the time factor is ignored: pitch follows speed). quality 0..2 (FFT size).
Audio stretchAudio(const Audio& a, double timeFactor, double semitones, Stretch mode, int quality = 1);
Content stretch(const Content& c, Range r, double timeFactor, double semitones, Stretch mode, int quality = 1);

Content gate(const Content& c, Range r, double thresholdDb, double attackMs, double holdMs, double releaseMs, double floorDb = -90);
Content declick(const Content& c, Range r, double sensitivity, int* fixed = nullptr);

struct NoiseProfile {
  int fft = 2048;
  double rate = 0;
  std::vector<float> mag;  // mean magnitude per bin (fft / 2 + 1)
  bool valid() const { return !mag.empty(); }
};
NoiseProfile acquireNoise(const Audio& a, Range r);
// Spectral noise reduction: reduction (dB, max attenuation) and sensitivity 0..1.
Content denoise(const Content& c, Range r, const NoiseProfile& p, double reductionDb = 24, double sensitivity = 0.5);

struct EqPoint {
  double hz = 1000, db = 0;
};
// Drawn EQ curve (points by frequency, interpolated on a log axis).
double eqCurveDb(const std::vector<EqPoint>& pts, double hz);
Content equalize(const Content& c, Range r, const std::vector<EqPoint>& pts);

struct ReverbSettings {
  double decay = 1.6;       // seconds to -60 dB
  double predelayMs = 12;
  double damping = 0.5;     // high frequencies decay faster (0..1)
  double width = 1.0;       // stereo decorrelation 0..1
  double wet = 0.3, dry = 1.0;
  uint32_t seed = 1;
};
Audio makeReverbIr(double rate, const ReverbSettings& s);
// FFT convolution (block overlap-add); the output is x.len + ir.len - 1 long.
Audio convolveAudio(const Audio& x, const Audio& ir);
// Mixes dry + wet convolution of the range back in place; the tail rings into
// the following audio (and lengthens the file when it runs past the end).
Content convolve(const Content& c, Range r, const Audio& ir, double wet, double dry);
Content reverb(const Content& c, Range r, const ReverbSettings& s);
// "Blur": diffuses the sound over `ms` (dense decaying noise kernel), mix 0..1.
Content blur(const Content& c, Range r, double ms, double mix);

// Spectral editing: attenuates (gainDb < 0) or boosts the time/frequency cells
// inside any of the rectangles.
struct SpectralRect {
  int64_t f0 = 0, f1 = 0;  // frames
  double hz0 = 0, hz1 = 0;
};
Content spectralGain(const Content& c, const std::vector<SpectralRect>& rects, double gainDb);

// ---- markers, regions, loop -------------------------------------------------------
Content addMarker(const Content& c, int64_t pos, const std::string& name);
Content removeMarker(const Content& c, size_t index);
Content renameMarker(const Content& c, size_t index, const std::string& name);
Content moveMarker(const Content& c, size_t index, int64_t pos);
Content addRegion(const Content& c, Range r, const std::string& name);
Content removeRegion(const Content& c, size_t index);
Content renameRegion(const Content& c, size_t index, const std::string& name);
Content setLoop(const Content& c, Range r);  // empty range clears
Content clearMarkers(const Content& c);
// Replaces the markers with "Slice n" markers at the onsets (sorted, deduplicated).
Content setSliceMarkers(const Content& c, const std::vector<int64_t>& onsets);
// Snaps each marker to the nearest zero crossing within `radius` frames.
Content snapMarkersToZero(const Content& c, int64_t radius);

// ---- analysis ------------------------------------------------------------------------
struct Levels {
  double peakDb = -180, rmsDb = -180;  // sample peak / RMS (dBFS), all channels
  double lufs = -180;                  // integrated loudness (BS.1770 gated)
  double shortTermMax = -180;          // loudest 3 s window (LUFS)
  double truePeakDb = -180;            // 4x oversampled peak (dBTP)
  double dc = 0;                       // mean value
};
Levels measure(const Audio& a, Range r);
double integratedLufs(const Audio& a, Range r);
double truePeakDb(const Audio& a, Range r);

struct PitchInfo {
  double hz = 0;
  double confidence = 0;  // 0..1
  int note = -1;          // nearest MIDI note
  double cents = 0;       // offset from that note
};
PitchInfo detectPitch(const Audio& a, Range r);
std::string noteName(int midi);
// Shifts the range so its detected pitch lands on `note` (-1 = nearest note).
Content tuneToNote(const Content& c, Range r, int note, PitchInfo* found = nullptr);

// Onset (beat / transient) positions, refined to the attack start. sensitivity 0..1.
std::vector<int64_t> detectOnsets(const Audio& a, Range r, double sensitivity = 0.5);
double estimateTempo(const std::vector<int64_t>& onsets, double rate);  // BPM (0 = unknown)
int64_t nearestZeroCrossing(const Audio& a, int64_t pos, int64_t radius);

// Min/max peak pyramid for fast waveform drawing.
class PeakCache {
 public:
  void build(const Audio& a);
  void clear() { levels_.clear(); frames_ = 0; }
  bool built() const { return frames_ > 0; }
  int64_t frames() const { return frames_; }
  // Min / max of channel c over frames [f0, f1).
  void query(const Audio& a, int c, int64_t f0, int64_t f1, float& mn, float& mx) const;
  static constexpr int kBase = 32, kFactor = 4;

 private:
  struct Level {
    int64_t block = 0;
    std::vector<float> mn[2], mx[2];
  };
  std::vector<Level> levels_;
  int64_t frames_ = 0;
};

// Short-time spectrum for display: cols × bins magnitudes (0..255 ↔ -120..0 dB).
struct Spectrogram {
  int cols = 0, bins = 0, fft = 0;
  int64_t hop = 0;
  double rate = 44100;
  std::vector<uint8_t> db;
  uint8_t at(int col, int bin) const { return db[static_cast<size_t>(col) * static_cast<size_t>(bins) + static_cast<size_t>(bin)]; }
};
Spectrogram computeSpectrogram(const Audio& a, int fftSize, int maxCols);

// ---- files --------------------------------------------------------------------------
enum class FileFormat { Wav = 0, Flac };
// WAV: 16 / 24-bit PCM or 32-bit float, with cue / label chunks for markers and
// regions and a sampler chunk for loop points.
bool writeWavFile(const std::string& path, const Content& c, int bits, std::string* err = nullptr);
// FLAC: 16 or 24-bit, fixed predictors + Rice coding (lossless).
bool writeFlacFile(const std::string& path, const Audio& a, int bits, std::string* err = nullptr);
bool exportFile(const std::string& path, const Content& c, Range r, FileFormat f, int bits, std::string* err = nullptr);
// Loads any supported sample file; WAV markers / regions / loop points are read back.
bool loadContent(const std::string& path, Content& out, std::string* err = nullptr);
// Parses a RIFF/WAVE image in memory (PCM 8/16/24/32, float 32/64).
bool parseWav(const uint8_t* p, size_t n, Content& out, std::string* err = nullptr);
uint64_t audioHash(const Audio& a);
std::string hashHex(uint64_t h);
// "<dir>/<clean name>-<hash>.wav"
std::string renderPath(const std::string& dir, const std::string& name, uint64_t hash);

// ---- plugin state (PluginState::data) ---------------------------------------------
struct State {
  std::string path;   // audio file ("" = empty editor)
  std::string hash;   // content hash of the file's audio (hex)
  std::string name;   // display name
  std::vector<Marker> markers;
  std::vector<Region> regions;
  int64_t loopStart = -1, loopEnd = -1;
  // FL Studio import: undecoded parts kept so the state round-trips.
  int flVersion = 0;
  std::string flExtra;  // hex
};
std::string encodeState(const State& s);
bool decodeState(const std::string& data, State& s);  // a bare path is accepted too

// ---- FL Studio Edison state --------------------------------------------------------
// J-39: the version word is either a plain number (4, 6, 11) or 3 | number << 16
// (12, 13); from 13 the audio size before the RIFF image is a u64.
inline int flEdisonVersion(int32_t word) { return word > 0xFFFF ? static_cast<int>(static_cast<uint32_t>(word) >> 16) : word; }
inline bool flEdisonSize64(int32_t word) { return flEdisonVersion(word) >= 13; }
struct FlEdison {
  int version = 0;                // the version word as stored
  std::vector<int32_t> head;      // ints between the version and the RIFF size
  std::vector<int32_t> settings;  // trailing ints (record mode, …, max time, …)
  bool hasAudio = false;
  Content content;                // embedded audio + its markers / regions / loop
  std::vector<uint8_t> extra;     // editor records between the audio and the settings
  int recMode = 1;                // 0 now, 1 on input, 2 input, 3 on play
  int recMaxChoice = 0;           // FL list index (0 forever, 2 1 min, 3 3 min, …)
  bool followHost = false;
};
bool decodeFlState(const uint8_t* p, size_t n, FlEdison& out);
// FL state → gl.edison parameters + data. Embedded audio is written to
// `audioDir` (named by content hash). `note` receives a short report line.
bool mapFlState(const uint8_t* p, size_t n, const std::string& audioDir, PluginState& out, std::string* note = nullptr);

// ---- recording -----------------------------------------------------------------------
// Turns a stream of input blocks into a take: triggers (now / on input / input
// gated / on play), pre-roll from before the trigger, maximum length.
class Recorder {
 public:
  struct Config {
    int mode = RecOnInput;
    double rate = 44100;
    double thresholdDb = -40;
    double preRollSec = 0.5;
    double maxSec = 0;       // 0 = unlimited
    double gateHoldSec = 0.5;  // RecInput: silence that pauses the take
  };
  void arm(const Config& c);
  void disarm();  // drops any take in progress
  void stop();    // ends the take in progress (it becomes ready)
  bool armed() const { return armed_; }
  bool recording() const { return recording_; }
  bool takeReady() const { return ready_; }
  int64_t recordedFrames() const { return static_cast<int64_t>(L_.size()); }
  // Feeds input frames (playing: transport state per frame, may be null).
  void feed(const float* L, const float* R, const uint8_t* playing, size_t n);
  // Moves the finished take out (empty when none).
  Audio take(int channels = 2);
  const Config& config() const { return cfg_; }

 private:
  void start(size_t preFrames);
  void push(float l, float r);
  Config cfg_;
  bool armed_ = false, recording_ = false, ready_ = false, paused_ = false;
  bool wasPlaying_ = false;
  double thr_ = 0.01;
  int64_t quiet_ = 0;
  std::vector<float> histL_, histR_;  // pre-roll ring
  size_t histPos_ = 0, histFill_ = 0;
  std::vector<float> L_, R_;
};

// Lock-free capture of the slot input (audio thread writes, UI reads).
class CaptureRing {
 public:
  static constexpr size_t kSize = size_t{1} << 19;  // ~11.9 s at 44.1 kHz
  CaptureRing();
  void write(const float* L, const float* R, int n, bool playing);
  size_t written() const { return pos_.load(std::memory_order_acquire); }
  // Copies up to maxN frames starting at absolute frame `from`. Returns frames
  // copied; `from` is advanced past frames that were already overwritten.
  size_t read(size_t& from, size_t maxN, float* L, float* R, uint8_t* playing) const;

 private:
  std::vector<float> L_, R_;
  std::vector<uint8_t> play_;
  std::atomic<size_t> pos_{0};
};

// The mixer-slot runtime. The editor reaches it through the slot's
// EffectRuntime (dynamic_cast) to start playback and drain the capture ring.
class EdisonFx : public Effect {
 public:
  EdisonFx();
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override;
  void processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override;
  void reset() override;
  // J-110: the user's Stop ends the editor's playback in the slot (the
  // editor's Play button reads isPlaying(), so it shows stopped).
  void stopSound() override;
  // J-112: the editor's Play sounding in the slot is plugin playback.
  bool ownPlayback() const override { return playing_.load(std::memory_order_relaxed); }
  float meter() const override { return 0.0f; }

  // ---- UI thread -------------------------------------------------------------
  void play(int64_t from, int64_t to, bool loop);  // to <= from: to the end
  void stop();
  void scrub(bool on, double targetFrame);
  bool isPlaying() const { return playing_.load(); }
  double position() const { return pos_.load(); }  // playback frame
  float inputPeak() { return inPeak_.exchange(0.f); }
  float outputPeak() { return outPeak_.exchange(0.f); }
  const CaptureRing& capture() const { return ring_; }
  // Frames of audio the playback source holds (0 = none).
  int64_t sourceFrames() const { return srcFrames_.load(); }

 private:
  void run(const PluginState& state, const Sample* src, const ProcessContext& ctx, float* L, float* R, int n);
  CaptureRing ring_;
  std::atomic<bool> playing_{false}, loop_{false}, scrubbing_{false}, scrubJump_{false}, startReq_{false}, stopReq_{false};
  std::atomic<int64_t> from_{0}, to_{0};
  std::atomic<double> pos_{0}, scrubTarget_{0};
  std::atomic<float> inPeak_{0}, outPeak_{0};
  std::atomic<int64_t> srcFrames_{0};
  bool hostWasPlaying_ = false;  // audio thread
  double rpos_ = 0, scrubVel_ = 0;  // audio thread
  bool rplaying_ = false;
};

// ---- sample registry for the effect (path → decoded sample) ----------------------------
// Edits publish their result here so the program build does not re-read the
// file it just wrote. Holds the most recent entries only.
void publishSample(const std::string& path, SamplePtr s);
SamplePtr sampleFor(const std::string& path);
SamplePtr toSample(const Audio& a, const std::string& path);
Audio fromSample(const Sample& s);

}  // namespace gl::packs::edison
