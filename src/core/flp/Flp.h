// Gridline — FL Studio project (.flp) import: shared reader and import context.
//
// The importer is written from the documented container layout
// (analysis/reference/format-flp.md) and public format notes; no Image-Line
// code is used. Each area lives in its own file and fills the Gridline song:
//   FlpProject.cpp      project info, tempo, time signature, master pitch
//   FlpMixer.cpp        inserts, names, colours, levels, routing, slot mix
//   FlpChannels.cpp     channel rack: names, colours, routing, sampler settings
//   FlpPatterns.cpp     patterns, notes (+ per-note properties)
//   FlpArrangement.cpp  playlist clips, tracks, markers, audio / automation clips
//   FlpPlugins.cpp      plugin states → Gridline instruments / effects
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../Params.h"
#include "../Song.h"

namespace gl::flp {

struct Event {
  uint8_t id = 0;
  uint32_t offset = 0;           // byte offset in the file (diagnostics)
  const uint8_t* data = nullptr;  // payload (points into File::bytes)
  uint32_t size = 0;

  // Integer value of BYTE / WORD / DWORD events (little endian).
  uint32_t value() const;
  int32_t ivalue() const { return static_cast<int32_t>(value()); }
  // Text payload: UTF-16LE (FL 11.5+) or ASCII, trailing NUL removed. UTF-8 out.
  std::string text() const;
  // Little-endian reads inside the payload (0 when out of range).
  uint8_t u8(size_t at) const { return at < size ? data[at] : 0; }
  uint16_t u16(size_t at) const;
  uint32_t u32(size_t at) const;
  int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }
  float f32(size_t at) const;
  double f64(size_t at) const;
};

struct File {
  std::vector<uint8_t> bytes;
  int format = 0, channelCount = 0, ppq = 96;
  std::string version;  // "25.2.3.5164"
  std::vector<Event> events;
};

// Reads and tokenises an .flp (or .fst) file. false + err on failure.
bool readFile(const std::string& path, File& out, std::string* err);
bool readBytes(std::vector<uint8_t> bytes, File& out, std::string* err);

// The event stream split by owner. FL writes patterns, channels, the
// arrangement (playlist, tracks, markers) and the mixer as runs of events;
// later runs can add to an earlier owner (e.g. pattern names after channels).
struct Sections {
  std::vector<Event> project;                           // global events
  std::map<int, std::vector<Event>> patterns;           // FL pattern index (1-based) → events
  std::map<int, std::vector<Event>> channels;           // FL channel (rack) index → events
  std::map<int, std::vector<Event>> arrangements;       // arrangement index → events (233, 148/205, 238/239…)
  bool legacyArrangement = false;  // saved before FL 12.9: one playlist, no Arrangement.New events
  struct Slot {
    std::vector<Event> events;  // plugin events (201 name, 212 wrapper, 213 state…) — empty slot: none
  };
  struct Insert {
    std::vector<Event> events;  // insert events (204 name, 149 colour, 236 flags, 235 routing, 147 output…)
    std::array<Slot, 10> slots;
  };
  std::vector<Insert> inserts;     // 0 = master, 1… = inserts, in file order
  std::vector<Event> mixerParams;  // 225 records (volume, pan, slot mix, route levels…)
};
Sections split(const File& f);

// Finds the first event with `id` in a list (nullptr if none).
const Event* find(const std::vector<Event>& evs, uint8_t id);

struct Report {
  std::vector<std::string> warnings;    // things that could not be imported exactly
  std::map<std::string, int> counts;    // "channels", "notes", "pattern clips", …
  std::map<std::string, int> unmapped;  // plugin internal name → instances without a Gridline equivalent
  void warn(const std::string& w) { warnings.push_back(w); }
};

// Result of decoding one FL plugin instance (a channel's generator or a mixer
// slot's effect) into a Gridline plugin.
struct PluginMapping {
  std::string flName;      // FL internal name, e.g. "Fruity Reeverb 2"
  std::string glType;      // Gridline type, e.g. "gl.reverb"; empty = no equivalent
  PluginState state;       // decoded parameters (+ data such as sample paths)
  bool exact = false;      // true when every used setting maps 1:1
  // FL parameter index (as used by automation clips / remote control) →
  // Gridline parameter index (-1 = not mapped), and FL normalized value
  // (0..1) → Gridline parameter value. Both optional.
  std::function<int(int flParam)> param;
  std::function<double(int flParam, double flNorm)> value;
  // Optional: a recorded control value (pattern events store the plugin's
  // own integer value, not 0..1) → FL normalized value 0..1, NaN if unknown.
  std::function<double(int flParam, int32_t raw)> rawNorm;
};

// A control's stored integer range (controls.md Min..Max, tag = FL parameter).
struct RawRange {
  int fl = 0;
  double lo = 0, hi = 1;
};
// Sets m.rawNorm: linear position of a raw value in its control's range
// (clamped to 0..1; NaN for parameters without a range).
void setRawRanges(PluginMapping& m, std::vector<RawRange> ranges);

// Decoder for one FL plugin: gets the plugin's events (201 name, 212 wrapper,
// 213 state, …) and fills the mapping. Return false to leave it unmapped.
using PluginDecoder = std::function<bool(struct Context&, const std::vector<Event>& events, PluginMapping& out)>;
// FL internal name → decoder. Filled by registerEffectDecoders() (FlpEffects.cpp)
// and registerGeneratorDecoders() (FlpGenerators.cpp).
std::map<std::string, PluginDecoder>& decoders();
// Decoder for an FL plugin name; falls back to a case-insensitive match (FL
// presets spell some names differently, e.g. "Fruity delay 2"). nullptr if none.
const PluginDecoder* findDecoder(const std::string& flName);
void registerEffectDecoders(std::map<std::string, PluginDecoder>& d);
void registerGeneratorDecoders(std::map<std::string, PluginDecoder>& d);
// Second wave (corpus-wide coverage): FlpEffects2.cpp / FlpGenerators2.cpp.
void registerEffectDecoders2(std::map<std::string, PluginDecoder>& d);
void registerGeneratorDecoders2(std::map<std::string, PluginDecoder>& d);
// Third wave (presets coverage: note effects, leftovers): FlpDecoders3.cpp.
void registerDecoders3(std::map<std::string, PluginDecoder>& d);
// Fourth wave (J-18 coverage sweep: the remaining stock plugins): FlpDec4.cpp.
void registerDecoders4(std::map<std::string, PluginDecoder>& d);
// Helpers for decoders: the raw state payload (event 213) and the wrapper (212).
const Event* pluginState(const std::vector<Event>& events);

// Shared state while importing one file.
struct Context {
  const File& file;
  Sections sec;
  Song& song;
  Report& report;
  std::string factoryData;  // folder that %FLStudioFactoryData% expands to
  std::string projectDir;   // folder of the .flp (relative sample paths)
  std::map<int, ObjectId> channelIds;  // FL channel index → Gridline channel
  std::map<int, ObjectId> patternIds;  // FL pattern index → Gridline pattern
  std::map<int, ObjectId> insertIds;   // FL insert index (0 = master) → Gridline mixer track
  std::map<int, int> channelTypes;     // FL channel index → FL channel type (0 sampler, 2 native, 3 layer, 4 audio clip, 5 automation)
  std::map<int, PluginMapping> channelPlugins;                  // FL channel index → decoded generator
  std::map<std::pair<int, int>, PluginMapping> slotPlugins;     // (FL insert, slot) → decoded effect
  std::map<int, ObjectId> automationIds;      // FL automation channel → AutomationClip
  std::map<int, uint32_t> automationTargets;  // FL automation channel → its first linked target (227)
  // FL origin capture (J-34, FlpOrigin.cpp): the playlist record each clip
  // came from, and each arrangement's Gridline index (FL arrangement → index).
  std::map<ObjectId, std::string> clipRecords;
  std::map<int, int> arrangementIndex;
  Context(const File& f, Song& s, Report& r) : file(f), song(s), report(r) {}
  // FL tick → Gridline tick (the song keeps the FL PPQ, so this is identity
  // unless the PPQ had to change).
  Tick tick(int64_t flTicks) const { return static_cast<Tick>(flTicks); }
  // Expands %FLStudioFactoryData% and relative paths; normalises separators.
  std::string resolvePath(const std::string& flPath) const;
};

// Area importers (called in this order by importFlp). Plugins are decoded
// before the arrangement so automation clips can target plugin parameters.
void importProject(Context& c);
void importMixer(Context& c);
void importChannels(Context& c);
void importPatterns(Context& c);
void importPlugins(Context& c);
void importArrangement(Context& c);

// A resolved FL parameter target (automation clips, pattern events): the
// Gridline parameter, the normalized range an FL value 0..1 maps to, and an
// optional conversion (FL value 0..1 → Gridline normalized value).
struct FlTarget {
  ParamId id;
  float min = 0, max = 1;
  std::function<double(double)> convert;
  std::string what;  // description for the report
  double toNormalized(double fl) const { return convert ? convert(fl) : min + fl * (max - min); }
};
// FL target id (event 227 / pattern events; see FlpArrangement.cpp) → parameter.
// Valid after importArrangement (needs the decoded plugins and audio channels).
bool resolveFlTarget(Context& c, uint32_t target, FlTarget& out);
// MIDI remote-control link (event 226, 20 bytes): u32 MIDI source
// ((port + 1) << 22 | channel << 16 | control: 0..127 CC, 253 channel
// aftertouch, 254 key aftertouch, 255 pitch bend, 256 + n note n; port 0 =
// any), i32 pickup state (runtime), u32 target (the 227 target id; a channel
// of 0xFFF / mixer base 0x3FC0 = "the focused channel / track"), u32 flags
// (4 MIDI source, 8 internal controller, 2 smoothing, bits 8..15 note mode:
// default, hold, latch, increment, stay), i32 smoothing time.
struct FlRemoteLink {
  int port = -1, channel = 0, control = 0;
  uint32_t target = 0, flags = 0;
  int noteMode = 0, smoothing = 0;
  bool midi() const { return (flags & 4) != 0; }
  bool focused() const { return (target < 0x10000000u && (target >> 16) == 0xFFF) || (target >> 22) == 0xFF; }
};
bool decodeRemoteLink(const Event& e, FlRemoteLink& out);
// The link as a Gridline ControllerLink (owner / index from `t`); false when
// the MIDI source has no Gridline equivalent.
bool remoteLinkToController(const FlRemoteLink& r, const FlTarget& t, ControllerLink& out);
// Pattern events (event 223) and notes on automation channels → pattern
// automation (FlpPatternEvents.cpp). After importArrangement.
void importPatternEvents(Context& c);
// J-43: after everything else: FL stores the crossfades it derives in the clip
// records; where Gridline derives the same length from the overlap the stored
// copy is dropped (the fade then follows the clips like in FL), elsewhere it
// stays stored so playback matches FL (FlpArrangement.cpp).
void settleImportedFades(Context& c);
// Automation channel points (event 234) → Gridline points: ticks at `ppq`
// from the first point, FL curve modes and tension sign converted
// (FlpAutomation.cpp). Reads both point layouts: 16-byte f32 records (header
// format 1, FL 4–8 presets) and 24-byte f64 records (format 2+).
bool decodeAutomationPoints(const Event& e, int ppq, std::vector<AutoPoint>& out);
// Plain text of an RTF document (project comments, event 198).
std::string rtfToText(const std::string& rtf);

// After importPlugins: channel settings that depend on the decoded instrument
// (FlpChannels.cpp; e.g. the channel volume envelope of native synths).
void finishChannels(Context& c);

// A channel's instrument page (events 218 ×5 and the filter in 219), decoded
// to times in seconds and normalized amounts. Envelope order in the file:
// pan, volume, mod X (cutoff), mod Y (resonance), pitch.
struct ChannelIns {
  enum Target { Pan = 0, Volume, ModX, ModY, Pitch };
  struct Env {
    bool enabled = false, tempoSync = false;
    double predelay = 0, attack = 0, hold = 0, decay = 0, sustain = 1, release = 0;  // seconds / 0..1
    double amount = 0;                                                             // -1..1
    double lfoDelay = 0, lfoAmount = 0, lfoRate = 1;                               // s, -1..1, Hz
    int lfoShape = 0;                                                              // 0 sine, 1 triangle, 2 pulse
  };
  Env env[5];
  int cutoff = 256, resonance = 0, filterType = 0;  // filter Mod X / Mod Y 0..256, FL filter type 0..7
  bool present = false;
};
ChannelIns decodeChannelIns(const std::vector<Event>& channelEvents);
// Seconds of an FL channel envelope time knob (100..65536), as the importer reads it.
double flEnvTime(int32_t v);
// Maps the instrument page onto a "gl.sampler" state (filter, volume and
// cutoff envelopes, LFO).
void applyInsToSampler(const ChannelIns& ins, PluginState& sampler);

// Default location of FL Studio's factory data (samples used by demo
// projects), or "" when not found. Checked: $GRIDLINE_FL_DATA, the macOS
// application bundle, common Wine prefixes.
std::string defaultFactoryData();

}  // namespace gl::flp

namespace gl {

// Imports an FL Studio project into a new Gridline song.
bool importFlp(const std::string& path, Song& out, flp::Report& report, std::string* err,
               const std::string& factoryData = "");
// The same from the file's bytes (`projectDir`: folder relative sample paths
// resolve against). keepOrigin = false skips Song::flOrigin (the FL
// exporter's own decoding of single objects, FlpExport*.cpp).
bool importFlpBytes(std::vector<uint8_t> bytes, Song& out, flp::Report& report, std::string* err, const std::string& factoryData = "",
                    const std::string& projectDir = "", bool keepOrigin = true);

}  // namespace gl
