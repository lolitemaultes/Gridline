// Gridline — recording from MIDI / the typing keyboard (J-17): notes into
// patterns (overdub / replace, quantize on input, loop passes into takes),
// controller moves into pattern automation or automation clips, FL-style step
// recording, and the typing-keyboard layout (octave, velocity, chord mode).
// All edits go through Document, so they are undoable.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Document.h"

namespace gl {

enum class AutoRecordTarget : uint8_t { Auto = 0, PatternEvents, AutomationClips, Count };
const char* autoRecordTargetName(AutoRecordTarget t);

struct RecordOptions {
  bool overdub = true;          // false = replace: a channel's first note in a pass clears its notes there
  bool quantize = true;         // note starts snap to `grid`
  bool quantizeLength = false;  // note lengths snap too (at least one grid)
  Tick grid = 24;               // ticks (the piano roll snap)
  bool takes = false;           // loop recording: every new loop pass records into a new pattern (a take)
  bool notes = true;            // record notes
  bool automation = true;       // record linked controller moves
  AutoRecordTarget autoTarget = AutoRecordTarget::Auto;  // Auto: pattern events in pattern mode, clips in song mode
};

// Nearest grid line (grid <= 1: unchanged).
Tick quantizeTick(Tick t, Tick grid);

class NoteRecorder {
 public:
  // Pattern mode: positions are pattern ticks looping over [0, loopLength).
  void beginPattern(Document& doc, ObjectId pattern, const RecordOptions& o, Tick loopLength);
  // Song mode: positions are song ticks. Notes go into `pattern` through the
  // clip that covers the position (a clip is placed when none does).
  void beginSong(Document& doc, ObjectId pattern, const RecordOptions& o);
  bool active() const { return active_; }
  bool songMode() const { return song_; }
  ObjectId pattern() const { return pattern_; }  // current take
  int pass() const { return pass_; }
  int takes() const { return static_cast<int>(takeIds_.size()); }
  int notesRecorded() const { return recorded_; }

  // Transport position update (every UI frame / before each event):
  // detects loop wraps (new pass, new take) and commits recorded automation.
  void position(Document& doc, double tick);
  void noteOn(Document& doc, ObjectId channel, int pitch, int velocity, double tick);
  void noteOff(Document& doc, ObjectId channel, int pitch, double tick);
  // A linked parameter moved (normalized value) at `tick`.
  void controller(ParamId target, double normalized, double tick);
  // Closes held notes at `tick` and commits everything.
  void end(Document& doc, double tick);
  const RecordOptions& options() const { return opt_; }

 private:
  struct Held {
    ObjectId channel;
    int velocity;
    double start;
    int pass;
    ObjectId pattern;
    Tick songOrigin;  // song mode: clip origin for the note's pattern ticks
  };
  void commitNote(Document& doc, const Held& h, int pitch, double endTick);
  void commitAutomation(Document& doc);
  void newPass(Document& doc);
  // Song mode: (pattern tick origin) for a song tick, placing a clip if needed
  // (PL-08: on the instrument track of `channel` when it has one).
  Tick songOrigin(Document& doc, double tick, ObjectId channel = kNoId);

  bool active_ = false, song_ = false;
  RecordOptions opt_;
  ObjectId base_ = kNoId, pattern_ = kNoId;
  Pattern baseContent_;  // the pattern as it was when recording started (takes start from it)
  Tick loop_ = 0;
  double last_ = -1;
  int pass_ = 0, recorded_ = 0;
  std::vector<ObjectId> takeIds_;
  std::map<std::pair<ObjectId, int>, Held> held_;  // (channel, pitch)
  std::set<ObjectId> cleared_;                      // replace mode: channels cleared this pass
  std::map<uint64_t, std::vector<std::pair<double, double>>> auto_;  // ParamId → (tick, value)
};

// FL-style step recording: keys pressed together form a chord that is placed
// at the step cursor when the last key is released; the cursor then moves on
// one step. Works while the transport is stopped.
class StepRecorder {
 public:
  Tick cursor = 0;
  Tick step = 24;        // grid (note length and advance)
  Tick lengthOverride = 0;  // > 0: note length instead of one step
  void press(int pitch, int velocity);
  // Returns true when the chord was written (all keys released).
  bool release(Document& doc, ObjectId pattern, ObjectId channel, int pitch);
  void rest() { cursor += step; }  // skip a step
  // Moves back one step and removes the notes starting there.
  void backspace(Document& doc, ObjectId pattern, ObjectId channel);
  int held() const { return static_cast<int>(down_.size()); }

 private:
  std::set<int> down_;
  std::map<int, int> chord_;  // pitch → velocity, collected while keys are down
};

// Computer keyboard as a piano (FL-like layout): lower row Z.. from the base
// octave, upper row Q.. one octave higher; octave and velocity keys; chord mode.
enum class ChordType : uint8_t { Major = 0, Minor, Seventh, Major7, Minor7, Sus4, Power, Octave, Count };
const char* chordTypeName(ChordType c);
std::vector<int> chordIntervals(ChordType c);

class TypingKeyboard {
 public:
  int octave = 4;      // lower row starts at baseNote() = 12 * octave
  int velocity = 100;  // 1..127
  bool chordMode = false;
  ChordType chord = ChordType::Major;

  int baseNote() const { return 12 * octave; }  // octave 4 → 48 (C4 in FL naming, 60 = C5)
  // Key codes are lower-case ASCII ('z', ',', '[', …). Returns the notes the
  // key starts (empty when it is not a note key or already held). Octave,
  // velocity and chord keys change the state and return nothing.
  std::vector<int> keyDown(int key);
  // Notes the key's release ends.
  std::vector<int> keyUp(int key);
  std::vector<int> releaseAll();
  // Semitone offset of a note key from baseNote(), -1 if not a note key.
  static int keyOffset(int key);
  // True for octave / velocity / chord keys (so the caller can swallow them).
  static bool isControlKey(int key);
  bool anyHeld() const { return !held_.empty(); }

 private:
  std::map<int, std::vector<int>> held_;
};

}  // namespace gl
