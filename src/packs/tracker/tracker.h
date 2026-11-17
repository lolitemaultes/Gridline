// Gridline - "tracker" pack (K-76, K-120): the Tracker Sampler, the instrument of imported modules
// that also plays as an ordinary sampler.
//   gl.tracker  Tracker Sampler: a module instrument (samples, key map, envelopes, auto vibrato,
//               fadeout) whose notes carry their tracker channel's cells (core/TrackerStream.h).
// Parameters: core/TrackerData.h (gl::tracker::Param). Instrument data: PluginState::data.
//
// K-120 additions, all in the instrument text (old states read the same):
//   * "cut <slot> <start> <end>" lines: a sample plays frames start..end of its file (end 0 = to
//     the file's end). Loop points stay in file frames.
//   * built-in sounds (PSound): a waveform Gridline makes itself, played while the instrument has
//     no sample file of its own. An instrument text without an instrument part takes the sound's
//     envelopes, vibrato and fadeout too.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/TrackerData.h"
#include "../../engine/Generators.h"

namespace gl {
class App;
}

namespace gl::packs::trackerpack {

namespace trk = gl::tracker;

// Sample start / end inside each slot's file (frames; end 0 = the file's end).
struct Cuts {
  std::array<int32_t, trk::kMaxSamples> start{}, end{};
  bool any() const {
    for (size_t k = 0; k < start.size(); ++k)
      if (start[k] != 0 || end[k] != 0) return true;
    return false;
  }
};

// K-129: the instrument's MIDI settings ("midi <on> <channel> <program> <bend> <mute>" line):
// with `on`, the notes it plays also go out to the MIDI output on `channel` (0..15) after a
// program change to `program` (0..127); `bend` is its pitch wheel range in semitones (0..36);
// `mute` silences its own sound (the MIDI notes still go out).
struct MidiSettings {
  bool on = false;
  int channel = 0, program = 0, bend = 2;
  bool mute = false;
  bool isDefault() const { return !on && channel == 0 && program == 0 && bend == 2 && !mute; }
};

// The instrument as the editor edits it: its text, the sample cuts, whether the state had an
// instrument part at all. K-129: the MIDI settings and the samples played at 8 bits
// ("bits <slot> 8" lines: the sample's values rounded to 256 steps, as an 8-bit sample holds them).
struct Edit {
  trk::InstrumentText text;
  Cuts cuts;
  bool body = false;
  MidiSettings midi;
  std::array<bool, trk::kMaxSamples> eightBit{};
};
// Reads a state's data (a missing instrument part gives `fallback`'s definition: the built-in
// sound's, or the default one for new samples).
Edit readEdit(const std::string& data, int sound);
std::string writeEdit(const Edit& e);

// ---- built-in sounds ----
inline constexpr int kSoundCount = 5;  // 0 = none
const char* soundName(int sound);
struct BuiltinSound {
  std::vector<float> frames;
  trk::SampleDef sample;   // length, loop, tuning: C-4 plays middle C
  trk::InstrumentDef def;  // its envelopes, vibrato, fadeout (sample 0 on every key)
};
// The sound (null for 0 / out of range). Made once, kept for the program's life.
const BuiltinSound* builtinSound(int sound);
// The definition new instruments start from: a volume envelope that holds while the key is down
// and fades out briefly after it is let go.
trk::InstrumentDef defaultDefinition();

// ---- edits (UI thread) ----
// Tuning that plays a file of `rate` Hz at its own speed on C-4.
void tuneForRate(trk::SampleDef& s, double rate);
// The key (tracker note 1..96) that plays the sample at its file's own rate.
int rootNote(const trk::SampleDef& s, double rate);
// Retunes the sample so `note` plays it at its file's rate (keeps the fine tune).
void setRootNote(trk::SampleDef& s, double rate, int note);
// Adds a sample file in the first free slot: -1 when all slots are taken. `frames` and `rate` are
// the file's. The first sample of an instrument takes every key.
int addSample(Edit& e, const std::string& path, int32_t frames, double rate, const std::string& name);
// K-129: audio files become samples (a drop, the "Add file" button): each in the next free slot,
// loaded through the sample cache, named after the file; files that do not load are skipped and
// it stops when every slot is taken (`full`). The first sample of an instrument also trades the
// built-in sound's envelope for a plain one. Returns the first new slot, -1 when none was added.
int addSampleFiles(Edit& e, const std::vector<std::string>& paths, int* added = nullptr, bool* full = nullptr);
// Removes a slot (its keys go to the nearest sample left).
void removeSample(Edit& e, int slot);
// Swaps two slots (the key map follows).
void swapSamples(Edit& e, int a, int b);
// Keys lo..hi (0..95) play `slot`.
void mapKeys(Edit& e, int lo, int hi, int slot);
// Drops file entries no slot uses and renumbers the rest.
void compactFiles(Edit& e);
// Envelope edits keeping sustain and loop indices on their points.
int insertEnvPoint(trk::Envelope& env, int x, int y);  // index, -1 when full
void deleteEnvPoint(trk::Envelope& env, int index);
// K-129: a copy of slot `from` in the first free slot (its file, settings, cuts; no keys): the
// new slot, -1 when none is free.
int duplicateSample(Edit& e, int from);
// K-106 (the classic "Copy smp."): `src`'s slot `sslot` into `dst`'s slot `dslot` (file, settings,
// name, cuts, bit depth; the key map stays, or every key plays it in an instrument with no sample).
// `dslot`, or -1 when the source slot is empty.
int copySampleInto(Edit& dst, int dslot, const Edit& src, int sslot);
// K-106 (the classic "Xchg smp."): two slots trade their samples, the key map stays.
void exchangeSampleData(Edit& e, int a, int b);
// K-106: the file into slot `slot` (the Files screen's load into the selected sample): its keys,
// the other samples and the instrument stay; an instrument with no sample takes it as addSample
// does. The slot, -1 when nothing was loaded.
int replaceSample(Edit& e, int slot, const std::string& path, int32_t frames, double rate, const std::string& name);
// K-106: the Clean up box's sample parts (the classic Trim): samples no key plays go (the rest move
// up in order, the key map following), looped samples end at their loop's end (the sample's end
// point; the file stays), samples play at 8 bits. Counts of each.
struct SampleTrim {
  int removed = 0, trimmed = 0, eightBit = 0;
  int total() const { return removed + trimmed + eightBit; }
};
SampleTrim trimSamples(Edit& e, bool unused, bool afterLoop, bool eightBits);
// The same on a Tracker Sampler's state: false (and nothing changed) for other instruments or when
// nothing would change.
bool trimSampleState(PluginState& st, bool unused, bool afterLoop, bool eightBits, SampleTrim* counts = nullptr);
// The bytes of sample data the instrument plays (each sample's played frames, channels, 2 bytes a
// value or 1 at 8 bits), for the Clean up box's size estimate.
long long sampleBytes(const PluginState& st);
// R11-07: a Tracker Sampler whose own sound is silent (its notes go to MIDI with Mute on).
bool midiMuted(const PluginState& st);
// A sample value as an 8-bit sample holds it (256 steps).
inline float eightBitValue(float v) {
  const float q = std::floor(v * 128.0f + 0.5f);
  return (q < -128.0f ? -128.0f : q > 127.0f ? 127.0f : q) * (1.0f / 128.0f);
}

// ---- K-129: envelope slots (six for each envelope, kept for the user, not the project) ----
// A volume slot also holds the fadeout and the auto vibrato, as the tracker's did.
inline constexpr int kEnvSlots = 6;
struct EnvSlot {
  trk::Envelope env;
  uint16_t fadeout = 0;
  uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
};
struct EnvSlots {
  std::array<EnvSlot, kEnvSlots> vol, pan;
};
// Gridline's own starting shapes.
EnvSlots factoryEnvSlots();
// Reads the slots from `path` (missing or malformed entries keep the factory shapes) / writes them.
EnvSlots loadEnvSlots(const std::string& path);
bool saveEnvSlots(const std::string& path, const EnvSlots& s);
// Puts a slot into the instrument (pan = the panning envelope) / takes one from it.
void applyEnvSlot(trk::InstrumentDef& d, const EnvSlot& s, bool pan);
EnvSlot takeEnvSlot(const trk::InstrumentDef& d, bool pan);

struct InstrumentAux;

// ---- K-129: sample edits (UI thread) ----
// A slot's audio as the edits work on it, with the points that move with it (file frames).
struct Wave {
  double rate = 44100;
  bool stereo = false;
  std::vector<float> L, R;  // R is empty for mono audio
  int32_t loopStart = 0, loopLength = 0;
  uint8_t loop = trk::kLoopOff;
  int32_t cutA = 0, cutB = 0;  // sample start / end (end 0 = the end)
  int relNote = 0;              // the sample's transpose (resampling keeps the pitch with it)
  int32_t frames() const { return static_cast<int32_t>(L.size()); }
};
struct Span {
  int32_t a = 0, b = 0;  // frames [a, b)
  bool empty() const { return b <= a; }
};
// The slot's wave from its file and settings (false when the file is not there).
bool waveOf(const Edit& e, int slot, const InstrumentAux& inst, Wave& out);
// The edits. Each returns false when it changes nothing (an empty range, nothing copied, ...).
// Ranges are clamped to the wave; an empty range means the whole wave where that makes sense.
Span clampSpan(const Wave& w, Span r);
bool waveDelete(Wave& w, Span r);                         // cut / delete: the range goes
Wave waveCopy(const Wave& w, Span r);                     // the range as a clip
bool wavePaste(Wave& w, Span r, const Wave& clip);        // over the range, or inserted at r.a when empty
bool waveCrop(Wave& w, Span r);                           // only the range stays
bool waveSilence(Wave& w, Span r);                        // the range becomes silence
bool waveReverse(Wave& w, Span r);
bool waveRamp(Wave& w, Span r, double startGain, double endGain);  // volume from start to end (fades: 0 / 1)
bool waveNormalise(Wave& w, Span r);                      // loudest point to full scale
double waveMaxScale(const Wave& w, Span r);               // the gain normalising would apply
bool waveResample(Wave& w, int semitones);                // longer / shorter, transpose follows: same pitch
bool waveCrossfadeLoop(Wave& w, int32_t fadeFrames);      // smooths the loop seam (equal power)
bool waveFixDc(Wave& w, Span r);
bool waveMix(Wave& w, const Wave& other, double balance);  // other mixed in from the start (0 = none, 1 = only it)
bool waveEcho(Wave& w, int count, int32_t distance, double feedback, bool extend);
bool waveMinimise(Wave& w);                               // drops what follows the loop
// Writes the wave as a new file in `dir` (named after `name` and its content) and points the slot
// at it with the wave's points. Returns the path ("" when it could not be written).
std::string commitWave(Edit& e, int slot, const Wave& w, const std::string& dir, const std::string& name);

// ---- K-106: the Tracker layout's header tools (its scope box) on a channel's Tracker Sampler ----
// The classic sample editor's extension (range tools) and instrument editor's extension (MIDI)
// work on the current instrument. Here they act on the sample its editor has selected (the first
// one when the editor never opened) and on the range dragged out on its waveform there, with the
// editor's own edits (one undo step each, the edited audio written as a new file).
struct HeaderView {
  bool tracker = false;        // the channel plays a Tracker Sampler
  int slot = -1, samples = 0;  // the selected slot (-1: no sample) and how many there are
  std::string name;
  int32_t frames = 0;
  double rate = 44100;
  Span range;                  // empty: no range (the tools take the whole sample)
  bool looped = false;
  int32_t clipFrames = 0;      // the sample clipboard (0: empty)
  MidiSettings midi;
};
HeaderView headerView(App& a, ObjectId channel);
enum class HeaderEdit { Reverse, Normalise, FadeIn, FadeOut, FixDc, Silence, Crop, Delete, Cut, Copy, Paste, TrimLoop, SelectAll, ClearRange, EmptyClipboard };
bool headerEdit(App& a, ObjectId channel, HeaderEdit what);
// Another sample of the instrument (the next present slot in direction `d`).
void headerStepSample(App& a, ObjectId channel, int d);
// The edits that ask for values first ("volume", "resample", "echo", "mix", "xfade"): opens one in
// the current window; headerPopups draws them there (each frame the tools show).
void headerOpenPopup(const char* which);
void headerPopups(App& a, ObjectId channel);
// The MIDI settings (`field` merges the steps of one control into one undo step).
void headerSetMidi(App& a, ObjectId channel, const MidiSettings& m, const char* label, int field);
// K-106: the Files screen's sample load: the file into the selected sample slot (an instrument with
// no sample takes it as its first), one undo step. The slot, or -1 (not a Tracker Sampler, or the
// file has no audio).
int headerLoadSample(App& a, ObjectId channel, const std::string& path);
// K-106: the header band's sample list: each present sample's slot and name (the name it was given
// or loaded as, never an edit's file name), the selected slot, and whether a built-in sound plays
// while there is none. `tracker` false: not a Tracker Sampler.
struct HeaderSamples {
  bool tracker = false;
  int sel = -1, sound = 0;
  std::vector<std::pair<int, std::string>> list;
};
HeaderSamples headerSamples(App& a, ObjectId channel);
// cp16 (CPU): the editor's sample selection for the channel, as a key that changes when it does (no
// parsing): the header keeps its sample list until this, the channel or the song changes.
uint64_t headerSampleSelKey(ObjectId channel);
void headerSelectSample(App& a, ObjectId channel, int slot);
// K-106 (SX-13, SX-15): the source instrument's slot `srcSlot` over the current instrument's
// selected sample (one undo step), and two of the current instrument's samples trading places
// (the selected one and `srcSlot`). False, with the reason on the status line, when nothing changed.
bool headerCopySample(App& a, ObjectId channel, ObjectId srcChannel, int srcSlot);
bool headerExchangeSamples(App& a, ObjectId channel, int srcSlot);
// The name of a channel's sample slot ("" when the slot is empty or it is not a Tracker Sampler).
std::string headerSampleName(App& a, ObjectId channel, int slot);

// K-129: the MIDI pitch of key `key` (0 = C0 .. 95 = B7) of the editor's key strip.
inline int stripKeyPitch(int key) { return key + 1 + trk::kPitchOffset; }

// Notes the editor holds (a pressed key on the strip, a sample preview), UI thread. The editor
// marks each one seen on every frame it is still on screen, visible and focused; a sweep at the
// start of the next frame releases any that were not, so a closed editor, a lost focus or a
// strip scrolled away never leaves a note sounding.
struct HeldNote {
  uint64_t editor = 0;
  uint32_t channel = 0;  // ObjectId
  int pitch = -1;
  uint32_t serial = 0;   // the preview's audition, 0 for a plain note
  int seen = 0;          // the last frame the editor showed it
  bool held() const { return pitch >= 0; }
};
class NoteGuard {
 public:
  enum Slot { kStrip = 0, kPreview = 1, kSlots = 2 };
  void hold(Slot s, uint64_t editor, uint32_t channel, int pitch, uint32_t serial, int frame) {
    n_[s] = HeldNote{editor, channel, pitch, serial, frame};
  }
  void seen(Slot s, int frame) {
    if (n_[s].held()) n_[s].seen = frame;
  }
  const HeldNote& get(Slot s) const { return n_[s]; }
  void drop(Slot s) { n_[s] = HeldNote{}; }
  // Releases (off(note)) every note its editor did not show on the frame before `frame`.
  template <class F>
  void sweep(int frame, F&& off) {
    for (HeldNote& h : n_)
      if (h.held() && h.seen < frame - 1) {
        const HeldNote copy = h;
        h = HeldNote{};
        off(copy);
      }
  }
  // Releases every note of one editor, or of all editors (editor 0).
  template <class F>
  void releaseAll(uint64_t editor, F&& off) {
    for (HeldNote& h : n_)
      if (h.held() && (editor == 0 || h.editor == editor)) {
        const HeldNote copy = h;
        h = HeldNote{};
        off(copy);
      }
  }

 private:
  std::array<HeldNote, kSlots> n_{};
};

// The instrument as the audio thread plays it: its definition and its samples as floats.
struct InstrumentAux : AuxData {
  trk::InstrumentText text;
  std::array<SamplePtr, trk::kMaxSamples> files{};  // keeps the sample data alive
  std::array<const float*, trk::kMaxSamples> data{};   // frames of each slot (null = none)
  std::array<const float*, trk::kMaxSamples> dataR{};  // the right channel of stereo files (null = mono)
  int builtin = 0;  // the built-in sound playing in slot 0 (0 = none)
  MidiSettings midi;                                   // K-129
  std::array<bool, trk::kMaxSamples> eightBit{};       // K-129
  std::array<std::vector<float>, trk::kMaxSamples> ownL, ownR;  // K-129: 8-bit copies (data points here)
};

// Builds (or finds, when the same data was built before) the playable instrument. UI thread.
std::shared_ptr<const InstrumentAux> instrumentFor(const PluginState& state);

// ---- K-129: hearing one sample (the editor's click to preview) ----
// The editor asks for slot `slot` of `inst` at `pitch` (MIDI), then sends that pitch as a note to
// the channel: the instrument plays that sample once at its own rate instead of the key map's.
// Returns the request's serial. UI thread.
uint32_t requestAudition(const InstrumentAux* inst, int slot, int pitch);
// Whether the audition `serial` still sounds (false once it ended or was replaced).
bool auditionSounding(uint32_t serial);
// Whether a voice plays it (not only asked for).
bool auditionPlaying(uint32_t serial);
// Drops the request when no note has taken it yet (the note went elsewhere).
void cancelAudition(uint32_t serial);
// The MIDI pitch that plays slot `slot` at its file's own rate.
int auditionPitch(const Edit& e, int slot, double rate);

}  // namespace gl::packs::trackerpack
