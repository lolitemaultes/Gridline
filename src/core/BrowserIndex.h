// Gridline — browser content model: file kinds, smart search, cheap tempo /
// key detection, a background content indexer with an on-disk cache
// (<config>/browser-index.json), user tags, preset trees and the drag & drop
// target table. Pure logic (no UI); the browser window, plugin picker and drop
// targets in app/ use it.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "PluginInfo.h"
#include "Presets.h"

namespace gl::browse {

// ---------------------------------------------------------------- file kinds
enum class Kind { Other, Audio, Midi, Project, Flp, Fst, Preset, Chain, Shape, Score, Folder };

// By extension (and, for .json, by name / location: "*.gridline.json" is a
// project, files under a "chains" / "automation" folder of the preset library
// are chains / shapes, other .json files there are plugin presets).
Kind kindOf(const std::string& path);
const char* kindName(Kind k);  // "audio", "midi", "project", …
// Kind named in a search filter ("audio", "sample", "wav", "midi", "preset",
// "fst", …); false when unknown.
bool kindFromName(const std::string& name, Kind& out);
// Files the browser lists (everything but Kind::Other / Folder).
inline bool listable(Kind k) { return k != Kind::Other && k != Kind::Folder; }

// ------------------------------------------------------------- file metadata
struct FileMeta {
  std::string path;
  Kind kind = Kind::Other;
  uint64_t size = 0;
  int64_t mtime = 0;      // seconds since the epoch
  bool analysed = false;  // audio: decoded once (length, rate, peaks)
  double seconds = 0;
  double rate = 0;
  int channels = 0;
  double bpm = 0;         // 0 = unknown
  int key = -1;           // pitch class 0..11 (C = 0), -1 = unknown
  bool minor = false;
  std::string peaks;      // waveform overview, see encodePeaks
  std::vector<std::string> tags;  // user tags (lower case)
};

// Tempo from a file name: "120bpm", "120 BPM", "loop_128_", "bpm95"; 0 if none.
double bpmFromName(const std::string& name);
// Key from a file name: "Am", "C#min", "F major", "Bb_minor", "key of D"… Tokens
// are delimited by non-letters, so "Amen" or "Cmaj7chord" are not keys.
bool keyFromName(const std::string& name, int& pitchClass, bool& minor);
std::string keyName(int pitchClass, bool minor);  // "A minor", "C# major"
// Parses a key filter ("Am", "c#", "F#m", "Ebmaj", "a minor"); minor = -1
// when the mode is not given.
bool parseKey(const std::string& text, int& pitchClass, int& minor);
// Tempo of a loop from its length: 1, 2, 4, 8 or 16 bars of 4/4 at a tempo in
// [70, 180] BPM that is a whole or half number. 0 when nothing fits (one-shots).
double bpmFromLength(double seconds);
// Tempo stored in a WAV file's "acid" chunk (loop metadata written by many
// sample editors), folded by octaves into [70, 180]; 0 when there is none.
// Reads chunk headers only.
double wavAcidTempo(const std::string& path);
// Playback speed ratio for tempo-synced preview: songBpm / sampleBpm, folded
// by octaves into [1/sqrt2, sqrt2] (a 70 BPM loop previews against 140 at 1x).
double syncRatio(double sampleBpm, double songBpm);
// Automatic tags from a path's file name and its two parent folders
// (kick, snare, hat, clap, bass, vocal, fx, loop, perc, pad, lead, …).
std::vector<std::string> autoTags(const std::string& path);

// Waveform overview: per-bucket peak (0..1) quantised into one character each
// ('0'..'9', 'a'..'z' = 36 levels). decodePeaks returns values 0..1.
std::string encodePeaks(const std::vector<float>& peaks);
std::vector<float> decodePeaks(const std::string& s);
// Peaks of interleaved-free channels (max |l|,|r| per bucket).
std::vector<float> computePeaks(const std::vector<float>& left, const std::vector<float>& right, int buckets);

// ---------------------------------------------------------------- smart search
// "kick tag:808 type:audio bpm:120-130 key:Am len:<2"
//   words     every word must appear in the name, folder or tags
//   type:K    file kind (or plugin / instrument / effect for plugin lists); repeatable (OR)
//   tag:T     user or automatic tag; repeatable (AND)
//   bpm:N     N±1, bpm:A-B range, bpm:>N / bpm:<N
//   key:K     pitch class (+ mode when given)
//   len:<S / len:>S / len:A-B   length in seconds (audio)
struct Query {
  std::vector<std::string> words;
  std::set<Kind> kinds;
  std::vector<std::string> pluginKinds;  // "plugin", "instrument", "effect", "notefx"
  std::vector<std::string> tags;
  double bpmLo = 0, bpmHi = 0;  // 0/0 = any
  int key = -1, minor = -1;
  double lenLo = 0, lenHi = 0;  // 0/0 = any
  bool fileFilters() const { return !kinds.empty() || !tags.empty() || bpmHi > 0 || key >= 0 || lenHi > 0; }
  bool empty() const { return words.empty() && pluginKinds.empty() && !fileFilters(); }
};
Query parseQuery(const std::string& text);
// A file against the query (name/folders/tags/metadata). Files without
// detected tempo/key fail bpm:/key: filters.
bool matches(const FileMeta& m, const Query& q);
// A plugin (built-in or hosted) against the query: words in name / category,
// type:plugin|instrument|effect|notefx, tag:<category word>. File-only
// filters (bpm, key, len, file types) exclude plugins.
bool matchesPlugin(const PluginInfo& p, const Query& q, bool favourite = false);
// Lower-case copy.
std::string lower(std::string s);

// ---------------------------------------------------------------- preset tree
// Presets of one plugin type (PresetEntry::folder "a/b") as a folder tree.
struct PresetNode {
  std::string name;                // folder name ("" for the root)
  std::vector<PresetNode> folders;  // sorted by name
  std::vector<size_t> presets;     // indices into the entry list, sorted by name
  size_t count() const;            // presets in this node and below
};
PresetNode buildPresetTree(const std::vector<PresetEntry>& entries);
// Plugin types that have a folder in the preset library (excludes chains /
// automation), sorted.
std::vector<std::string> libraryTypes(const std::string& presetRoot);

// ---------------------------------------------------------------- drag & drop
enum class DropTarget {
  Rack,          // channel rack (empty area or a channel row)
  Playlist,      // playlist lanes
  PianoRoll,     // piano roll grid
  MixerTrack,    // a mixer strip
  MixerSlot,     // an effect slot of a strip
  PluginWindow,  // a plugin window (header / body)
  Workspace,     // anywhere else (file dropped onto the app window)
};
enum class DropAction {
  None,
  NewSamplerChannel,   // audio → new sampler channel with the sample
  AudioClip,           // audio → sampler channel + audio clip at the drop position
  LoadSample,          // audio → the target plugin's sample (sample-based plugins)
  ConvolverIR,         // audio → convolver with the file as impulse response
  ApplyPreset,         // plugin preset → the open plugin (same type)
  NewChannelFromPreset,  // generator preset → new channel
  InsertEffectPreset,  // effect preset → mixer slot / first free slot
  ApplyChain,          // effect chain → the mixer track
  ImportMidiChannels,  // MIDI file → new pattern with one channel per track
  ImportMidiNotes,     // MIDI file → notes of the open pattern / channel
  OpenProject,         // Gridline / FL Studio / zipped project → open (J-52)
};
// What a dragged item is. FL presets (.fst) are decoded first and described
// by what they contain (plugin preset or chain).
struct DropItem {
  Kind kind = Kind::Other;
  std::string pluginType;  // Preset: its plugin type
  PluginKind pluginKind = PluginKind::Generator;
};
// `targetType` = the plugin type shown by a PluginWindow target / of the
// MixerSlot's current effect; `targetUsesSample` = that plugin loads samples.
DropAction dropAction(const DropItem& item, DropTarget target, const std::string& targetType = "", bool targetUsesSample = false);
const char* dropActionLabel(DropAction a);  // "New sampler channel", …

// ---------------------------------------------------------------- the indexer
// Background content index. Roots are walked on a worker thread; audio files
// are decoded once for length / rate / peaks (tempo from the name or the loop
// length, key from the name). Results are cached by path + size + mtime in
// the index file; user tags live there too. Refresh re-walks the roots and
// only analyses new or changed files. All members are thread-safe.
class Index {
 public:
  Index() = default;
  ~Index();
  Index(const Index&) = delete;
  Index& operator=(const Index&) = delete;

  // Cache file (loaded at once, saved after each pass and on tag changes).
  void setCacheFile(const std::string& file);
  // Roots to walk (recursively; hidden entries skipped). `analyse` = decode audio.
  void setRoots(const std::vector<std::pair<std::string, bool>>& roots);
  void start();           // starts / restarts a pass on the worker thread
  void stop();            // cancels and joins the worker
  void waitIdle();        // blocks until the current pass ends (tests)
  bool busy() const { return busy_.load(); }
  size_t done() const { return done_.load(); }
  size_t pending() const { return pending_.load(); }
  uint64_t generation() const { return gen_.load(); }  // bumps when entries change

  bool lookup(const std::string& path, FileMeta& out) const;
  // Entries matching the query, sorted by name, at most `limit`.
  std::vector<FileMeta> search(const Query& q, size_t limit = 400) const;
  size_t size() const;
  // Adds / replaces one entry (also used for files outside the roots, e.g. on preview).
  void put(const FileMeta& m);
  // Analyses one file now on the calling thread (and stores it). False if not audio.
  bool analyseNow(const std::string& path, FileMeta* out = nullptr);

  // User tags (lower case, unique). Saved immediately.
  void setTags(const std::string& path, std::vector<std::string> tags);
  std::vector<std::string> tagsOf(const std::string& path) const;
  std::vector<std::string> allTags() const;  // user tags in use, sorted

  bool save() const;
  bool load();

  // Fills metadata of one file (stat + optional decode). Exposed for tests.
  static FileMeta describe(const std::string& path, bool analyse);

 private:
  void run(std::vector<std::pair<std::string, bool>> roots);
  mutable std::mutex mu_;
  std::map<std::string, FileMeta> entries_;
  std::map<std::string, std::vector<std::string>> tags_;
  std::vector<std::pair<std::string, bool>> roots_;
  std::string cacheFile_;
  std::thread worker_;
  std::atomic<bool> cancel_{false}, busy_{false};
  std::atomic<size_t> done_{0}, pending_{0};
  std::atomic<uint64_t> gen_{0};
};

}  // namespace gl::browse
