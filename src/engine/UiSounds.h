// Gridline — interface sounds (J-48, themes J-133): short WAVs for significant
// moments (startup, save, export finished, errors, ...), mixed into the device
// output after the master. They never enter the song mixer, so they are not
// recorded, not rendered and not metered.
//
// Sound themes (J-133): assets/sounds/<theme>/<event>.wav, one folder per
// theme (graphite, prism, tine, signal, classic). Setting "sound.theme";
// default graphite. A missing theme or file falls back to graphite, then to
// classic, then to an older flat layout (assets/sounds/<event>.wav), so an
// event never goes silent by accident.
//
// Threads: loading, trigger and policy run on the UI thread (or a loader
// thread for loadTheme); mix() runs on the audio thread and never allocates,
// locks or does I/O. A loaded set of clips (a Bank) is handed to the audio
// thread through an atomic pointer and adopted at the start of the next
// block; sounds still playing from the previous bank finish first, then the
// audio thread hands that bank back and collect() frees it on the UI thread.
// So a theme can be switched while audio runs, with no restart and no gap.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
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

// Settings keys: "sounds.enabled", "sounds.volume", "sounds.<eventId>", "sound.theme".
std::string settingKey(Event e);
inline constexpr const char* kThemeKey = "sound.theme";

// ---- themes
inline constexpr int kThemeCount = 5;
inline constexpr const char* kDefaultTheme = "graphite";
inline constexpr const char* kFallbackTheme = "classic";
const char* themeId(int i);           // "graphite", "prism", "tine", "signal", "classic"
const char* themeLabel(int i);        // "Graphite", ...
const char* themeDescription(int i);  // one line for Settings
int themeIndex(const std::string& id);  // -1 when unknown
// The theme to use for a settings value: itself when known, else the default.
std::string resolveTheme(const std::string& id);
// The order themes are tried in for each event: the theme, graphite, classic.
std::vector<std::string> fallbackChain(const std::string& id);

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

// Decodes a WAV/FLAC/... file and converts it to `rate` with the windowed-sinc
// resampler (engine/Resample.h). Returns false (clip untouched) on failure.
bool loadClip(const std::string& path, double rate, Clip& out, std::string* err = nullptr);

// A full set of clips, one per event (empty = that event has no sound).
struct Bank {
  std::array<Clip, kEventCount> clips;
  std::string theme;                         // the theme that was asked for
  double rate = 0;                           // the device rate the clips were converted to
  std::array<std::string, kEventCount> from;  // the theme each clip came from ("" = none)
};

// Loads every event of `theme` from <soundsDir>/<theme>/<event>.wav, filling
// a missing file from the next theme in fallbackChain() and finally from
// <soundsDir>/<event>.wav. Any thread (no shared state). Never null.
std::unique_ptr<Bank> loadTheme(const std::string& soundsDir, const std::string& theme, double rate);
// Loads one event of a theme (with the same fallback), for previews.
bool loadThemeClip(const std::string& soundsDir, const std::string& theme, Event e, double rate, Clip& out);

class Player {
 public:
  static constexpr int kVoices = 4;
  static constexpr int kQueue = 16;
  static constexpr int kRetiring = 4;  // banks the audio thread can hold while their sounds finish
  static constexpr int kRing = 16;     // banks handed back to the UI thread

  Player() = default;
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  ~Player();  // the audio device must be closed (or no longer calling mix)

  // UI thread: make `bank` the theme's sounds. Takes effect at the audio
  // thread's next block; sounds already playing finish from the old bank.
  void install(std::unique_ptr<Bank> bank);
  // UI thread: decode <dir>/<eventId>.wav for every event at `rate` and install
  // them (older flat layout; tests). Missing files leave that event silent.
  int loadAll(const std::string& dir, double rate);
  // UI thread: replace one clip (installs a copy of the current bank with it).
  void setClip(Event e, Clip clip);
  // UI thread: the bank last installed (what triggers will play).
  bool hasClip(Event e) const { return bank_ && !bank_->clips[static_cast<size_t>(e)].lr.empty(); }
  const Clip& clip(Event e) const;
  const Bank* bank() const { return bank_; }

  // UI thread: queue a sound (no policy here; see shouldPlay / App::uiSound).
  // False when the queue is full or the event has no clip.
  bool trigger(Event e, float gain = 1.0f);
  // UI thread: play one clip that is not part of the theme (Settings preview
  // of another theme). Same hand-over as install().
  bool preview(Clip clip, float gain = 1.0f);
  void setVolume(float v) { volume_.store(v < 0 ? 0 : v > 1 ? 1 : v, std::memory_order_relaxed); }
  float volume() const { return volume_.load(std::memory_order_relaxed); }

  // UI thread: free banks the audio thread has finished with. Cheap; call
  // once per frame. Returns how many were freed.
  int collect();

  // Audio thread: add the active sounds into an interleaved stereo buffer.
  void mix(float* interleaved, int frames) noexcept;
  int activeVoices() const noexcept;  // audio thread (or tests)

 private:
  struct Voice {
    const Clip* clip = nullptr;
    const Bank* owner = nullptr;
    size_t pos = 0;
    float gain = 0;
    int fade = 0;  // > 0: being stolen, samples of fade-out left
    const Clip* pending = nullptr;  // starts in this slot once the fade-out ends
    const Bank* pendingOwner = nullptr;
    float pendingGain = 0;
  };
  struct Request {
    int event = 0;  // kEventCount = the preview bank's clip
    float gain = 0;
  };
  // One hand-over lane: the UI thread publishes `pending`, the audio thread adopts it.
  struct Lane {
    std::atomic<Bank*> pending{nullptr};
    Bank* active = nullptr;  // audio thread only
  };
  void submit(Lane& lane, std::unique_ptr<Bank> bank);
  void adopt(Lane& lane) noexcept;
  void retire() noexcept;

  std::vector<std::unique_ptr<Bank>> owned_;  // UI thread: every bank not yet freed
  const Bank* bank_ = nullptr;                // UI thread: latest theme bank
  Lane theme_, preview_;
  std::array<Bank*, kRetiring> retiring_{};    // audio thread
  std::array<Bank*, kRing> ring_{};            // audio -> UI
  std::atomic<unsigned> ringHead_{0}, ringTail_{0};
  std::array<Voice, kVoices> voices_{};
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
