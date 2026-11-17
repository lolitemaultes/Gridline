// Gridline - "tracker" pack, K-106 cp17 (K-316 plan section 2): the Tracker Sampler's editing
// session, shared by its two front ends: the plugin editor (tracker_ui.cpp, every layout) and the
// Tracker layout's own screens (the header tools now, the Sample and Instrument editors from cp18).
// No drawing here.
//
// * Memory: per instrument (PluginTarget::key), what the editing remembers: the selected sample,
//   the range dragged on the waveform, the waveform view, the values the valued edits last used.
//   One memory, two views: the plugin window and the Tracker screens show the same selection.
// * The sample clipboard.
// * Snapshot: the instrument's parsed text and playable instrument, cached per document revision
//   (no parsing while nothing changes).
// * Every edit as a named operation. Each re-reads the CURRENT state, changes it, and commits once
//   through setPluginState with the label and merge key the plugin editor always used (so a drag
//   folds into one undo step the same way in either front end, and undo reads the same). A remote
//   edit arriving mid-drag is never overwritten by a copy read earlier in the gesture.
//   The pure part of each edit (apply*: Edit in, Edit out, no App) is what test_tracker_ops checks.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "tracker.h"

namespace gl {
class App;
struct PluginTarget;
}  // namespace gl

namespace gl::packs::trackerpack {

// ---- the editing memory ----
struct PopupValues {
  int rampStart = 100, rampEnd = 100;  // volume (percent)
  int resTones = 0;                    // resample (semitones; R12-06: 0, so an Apply by mistake changes nothing)
  int echoCount = 4, echoMs = 120, echoFeedback = 50;
  bool echoExtend = true;
  int mixSlot = -1, mixBalance = 50;
  int fadeMs = 20;                     // loop crossfade (ms)
};
struct Memory {
  int sel = 0;
  int wantSlot = -1;                   // R12-04: an empty slot picked in the header list: the next load from Files goes there
  double viewStart = 0, viewLen = 0;   // waveform view, file frames (0 = the whole file)
  int viewSlot = -1;
  int32_t selA = -1, selB = -1;        // the range dragged out on the waveform (file frames; -1 = none)
  int selSlot = -1;
  PopupValues pv;
  int playNote = 49;                   // C-4: the Tracker screens' play note (cp18)
};
Memory& memory(uint64_t targetKey);
const Memory* findMemory(uint64_t targetKey);
// The range on slot k (empty: none).
Span rangeOf(const Memory& m, int k);

// The sample clipboard of every Tracker Sampler editor.
Wave& clipboard();

// ---- pure edits (no App; test_tracker_ops) ----
inline uint64_t mergeKeyFor(uint64_t targetKey, int field) { return (targetKey * 2654435761ull) ^ (0x7A1C0000ull + static_cast<uint64_t>(field)); }
int soundOf(const PluginState& st);
bool hasSample(const Edit& e, int k);
int sampleCountOf(const Edit& e);
// The name an edited file of slot k is written under (the sample's name, else its file's stem).
std::string editName(const Edit& e, int k);

enum class SampleField { Volume, Pan, Fine, RelNote };
// Volume 0..64, Pan -128..127 (0 the middle), Fine -128..127, RelNote -48..71; clamped as the knobs do.
void applySampleField(Edit& e, int k, SampleField f, long v);
const char* sampleFieldLabel(SampleField f);
int sampleFieldMerge(SampleField f);  // 1..4 (the root is 5)
// Loop type; switched on with no loop, the loop takes the played part s0..s1.
void applyLoopType(Edit& e, int k, uint8_t loop, int32_t s0, int32_t s1);
// A waveform marker dragged to frame f (0 sample start, 1 end, 2 loop start, 3 loop end), clamped
// as the plugin editor's flags are (the loop inside the played part, start before end).
void applyMarker(Edit& e, int k, int marker, int32_t f, int32_t frames);
const char* markerLabel(int marker);
// Envelopes (pan: the panning one). Flag toggle: switched on with no points, a flat line to start from.
void applyEnvFlag(Edit& e, bool pan, uint8_t flag);
// A point added at (x, y): an empty envelope also gets its first point at 0 and turns on. The index, -1 when full.
int applyEnvAdd(Edit& e, bool pan, int x, int y);
// Point i to (x, y), x kept between its neighbours (the first point stays at 0). False: no change.
bool applyEnvMove(Edit& e, bool pan, int i, int x, int y);
bool applyEnvDelete(Edit& e, bool pan, int i);  // never the first point, never below two
void applyEnvSustain(Edit& e, bool pan, int i);
void applyEnvLoopStart(Edit& e, bool pan, int i);
void applyEnvLoopEnd(Edit& e, bool pan, int i);
inline const char* envLabel(bool pan) { return pan ? "panning envelope" : "volume envelope"; }
enum class VibField { Sweep, Depth, Rate };
void applyFadeout(Edit& e, long v);                // 0..4095
void applyVibrato(Edit& e, VibField f, long v);    // sweep 0..255, depth 0..15, rate 0..63
const char* vibLabel(VibField f);
int vibMerge(VibField f);                          // 31..33
// The MIDI settings; `syncMute`: the instrument's own mute follows (on && mute), as the MIDI box and the plugin editor's Mute do.
void applyMidi(Edit& e, const MidiSettings& m, bool syncMute);
// K-106 cp19 (the Instrument editor's Add): where a point added after point `i` goes: halfway to
// the next point (in ticks and level), or 16 ticks past the last one at its level; an empty
// envelope: tick 16 at full volume (the middle for panning). False: no room (full, or no tick free
// between point i and the next).
bool envPointAfter(const trk::Envelope& env, bool pan, int i, int* x, int* y);
// K-106 cp19 (the key map's "go to the nearest other sample"): every key playing `slot` plays the
// sample of the nearest key that plays another sample (else the first other sample). False when
// no key plays `slot` or the instrument has no other sample.
bool applyKeysAway(Edit& e, int slot);

}  // namespace gl::packs::trackerpack

// ---- the operations (UI thread; tracker_session.cpp, in the app) ----
namespace gl::packs::trackerpack {

uint64_t mergeKey(const PluginTarget& t, int field);
// The edit written back as one undo step (nothing when the text did not change). True: committed.
bool commitEdit(App& a, const PluginTarget& t, const Edit& e, const char* label, uint64_t merge = 0);

// The instrument as last read: cached by (target, document revision), so nothing is parsed while
// nothing changes. `ok` false when the target is not a Tracker Sampler.
struct Snapshot {
  bool ok = false;
  const PluginState* st = nullptr;
  std::shared_ptr<const InstrumentAux> inst;
  Edit e;
  int sound = 0;
};
const Snapshot& snapshot(App& a, const PluginTarget& t);
// cp17 CPU counters: snapshots built and served since the start ("tracker editors perf").
void snapshotCounts(long long* built, long long* served);

// Every op: the current state read, the edit applied, one commit. `mirror` (optional) receives the
// edited instrument so a front end drawing on in the same frame shows it. False: nothing changed.
bool opSampleField(App& a, const PluginTarget& t, int k, SampleField f, long v, Edit* mirror = nullptr);
bool opSampleRoot(App& a, const PluginTarget& t, int k, double rate, int note, Edit* mirror = nullptr);
bool opSampleLoopType(App& a, const PluginTarget& t, int k, uint8_t loop, int32_t s0, int32_t s1, Edit* mirror = nullptr);
bool opSampleBits(App& a, const PluginTarget& t, int k, bool eight, Edit* mirror = nullptr);
bool opSampleMarker(App& a, const PluginTarget& t, int k, int marker, int32_t f, int32_t frames, Edit* mirror = nullptr);
bool opRenameSample(App& a, const PluginTarget& t, int k, const std::string& name, Edit* mirror = nullptr);
bool opSwapSamples(App& a, const PluginTarget& t, int k, int to, Edit* mirror = nullptr);
int opDuplicateSample(App& a, const PluginTarget& t, int k, Edit* mirror = nullptr);  // the new slot, -1: none
bool opRemoveSample(App& a, const PluginTarget& t, int k, Edit* mirror = nullptr);
bool opRenameInstrument(App& a, const PluginTarget& t, const std::string& name, Edit* mirror = nullptr);
bool opInsFadeout(App& a, const PluginTarget& t, long v, Edit* mirror = nullptr);
bool opInsVibrato(App& a, const PluginTarget& t, VibField f, long v, Edit* mirror = nullptr);
bool opInsVibShape(App& a, const PluginTarget& t, int shape, Edit* mirror = nullptr);
bool opEnvFlag(App& a, const PluginTarget& t, bool pan, uint8_t flag, Edit* mirror = nullptr);
int opEnvPointAdd(App& a, const PluginTarget& t, bool pan, int x, int y, Edit* mirror = nullptr);
bool opEnvPointMove(App& a, const PluginTarget& t, bool pan, int i, int x, int y, Edit* mirror = nullptr);
bool opEnvPointDelete(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror = nullptr);
bool opEnvSustain(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror = nullptr);
bool opEnvLoopStart(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror = nullptr);
bool opEnvLoopEnd(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror = nullptr);
bool opEnvApplySlot(App& a, const PluginTarget& t, bool pan, const EnvSlot& slot, Edit* mirror = nullptr);
bool opMapKeys(App& a, const PluginTarget& t, int lo, int hi, int slot, Edit* mirror = nullptr);
// K-106 cp19: the keys of `slot` to the nearest other sample (applyKeysAway), labelled "map keys" as the drag.
bool opMapKeysAway(App& a, const PluginTarget& t, int slot, Edit* mirror = nullptr);
// K-106 cp19 (moved from tracker_ui.cpp, unchanged): the six envelope slots of each envelope, kept
// with the user's settings ("tracker-envelopes.txt" in the config folder, never the project), read
// once. Both front ends use the same slots.
std::string envSlotsPath(App& a);
EnvSlots& envSlots(App& a);
// The MIDI settings; `merge` 0: its own step, else the merge field (40.. as the knobs).
bool opSetMidi(App& a, const PluginTarget& t, const MidiSettings& m, bool syncMute, const char* label, int mergeField, Edit* mirror = nullptr);
// A sample edit's result kept: the wave written as a new file of slot k ("Edited audio"), the text
// pointing at it, one commit. False (with a notice) when the file could not be written.
bool opWaveCommit(App& a, const PluginTarget& t, int k, const Wave& w, const char* label, Edit* mirror = nullptr);

}  // namespace gl::packs::trackerpack
