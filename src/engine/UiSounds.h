// Gridline — interface sounds (J-48): short WAVs for significant moments
// (startup, save, export finished, errors, ...), mixed into the device output
// after the master. They never enter the song mixer, so they are not recorded,
// not rendered and not metered.
//
// Threads: load / trigger / policy run on the UI thread; mix() runs on the audio
// thread and never allocates, locks or does I/O. Triggers cross over through a
// fixed single-producer / single-consumer ring. Clips are replaced only through
// load() while the audio device is closed or paused (main.cpp loads them right
// after opening the device, before unpausing it).
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace gl {
class Settings;

namespace uisound {

enum class Event : int { Startup, Open, Save, Export, Import, TaskDone, Warning, Error, RecordStart, RecordStop, Count };
inline constexpr int kEventCount = static_cast<int>(Event::Count);

const char* eventId(Event e);     // file stem and settings suffix: "startup", "save", ...
const char* eventLabel(Event e);  // Settings label: "Startup", "Project saved", ...
bool defaultOn(Event e);          // per-event default (warning / record off)

// Settings keys: "sounds.enabled", "sounds.volume", "sounds.<eventId>".
std::string settingKey(Event e);

// What the app knows when an event fires.
struct Context {
  bool enabled = true;             // "Interface sounds" master switch (and not disabled by flag / env)
  bool eventEnabled = true;        // per-event switch
  bool transportPlaying = false;   // song or pattern playing
  bool recording = false;          // note or audio recording running
  bool audioInputRecording = false;  // an audio input is being recorded (a microphone could hear us)
};

// Muting rules. Nothing plays while the transport plays or records (the music
// has the floor), except Error (a failure must always be noticed) and
// RecordStart / RecordStop (they mark the transport change itself); those two
// are skipped while an audio input records, so they never leak into a take.
bool shouldPlay(Event e, const Context& c);

// One decoded clip at the device rate, interleaved stereo.
struct Clip {
  std::vector<float> lr;
  size_t frames() const { return lr.size() / 2; }
};

// Decodes a WAV/FLAC/... file and resamples it (linear, sufficient for these
// band-limited sounds) to `rate`. Returns false (clip untouched) on failure.
bool loadClip(const std::string& path, double rate, Clip& out, std::string* err = nullptr);

class Player {
 public:
  static constexpr int kVoices = 4;
  static constexpr int kQueue = 16;

  // UI thread, device closed or paused: decode <dir>/<eventId>.wav for every
  // event at `rate`. Missing files leave that event silent. Returns the count loaded.
  int loadAll(const std::string& dir, double rate);
  // UI thread, device closed or paused: one clip (tests, custom picks).
  void setClip(Event e, Clip clip);
  bool hasClip(Event e) const { return !clips_[static_cast<size_t>(e)].lr.empty(); }
  const Clip& clip(Event e) const { return clips_[static_cast<size_t>(e)]; }

  // UI thread: queue a sound (no policy here; see shouldPlay / App::uiSound).
  // False when the queue is full or the event has no clip.
  bool trigger(Event e, float gain = 1.0f);
  void setVolume(float v) { volume_.store(v < 0 ? 0 : v > 1 ? 1 : v, std::memory_order_relaxed); }
  float volume() const { return volume_.load(std::memory_order_relaxed); }

  // Audio thread: add the active sounds into an interleaved stereo buffer.
  void mix(float* interleaved, int frames) noexcept;
  int activeVoices() const noexcept;  // audio thread (or tests)

 private:
  struct Voice {
    const Clip* clip = nullptr;
    size_t pos = 0;
    float gain = 0;
    int fade = 0;  // > 0: being stolen, samples of fade-out left
    const Clip* pending = nullptr;  // starts in this slot once the fade-out ends
    float pendingGain = 0;
  };
  std::array<Clip, kEventCount> clips_;
  std::array<Voice, kVoices> voices_{};
  struct Request {
    int event = 0;
    float gain = 0;
  };
  std::array<Request, kQueue> queue_{};
  std::atomic<unsigned> head_{0}, tail_{0};  // head: written by the UI thread, tail: by the audio thread
  std::atomic<float> volume_{0.7f};
};

// Per-event cool-down so bursts (save all, repeated errors) do not stack.
class Gate {
 public:
  explicit Gate(double cooldownSeconds = 0.25) : cooldown_(cooldownSeconds) { last_.fill(-1e9); }
  bool allow(Event e, double now);

 private:
  double cooldown_;
  std::array<double, kEventCount> last_{};
};

}  // namespace uisound
}  // namespace gl
