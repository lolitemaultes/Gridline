// Gridline — recording from MIDI / the typing keyboard (J-17): notes into
// patterns (overdub / replace, quantize on input, loop passes into takes),
// controller moves into pattern automation or automation clips, FL-style step
// recording, and the typing-keyboard layout (octave, velocity, chord mode).
// All edits go through Document, so they are undoable.
#pragma once
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Document.h"

namespace gl {

enum class AutoRecordTarget : uint8_t { Auto = 0, PatternEvents, AutomationClips, Count };
const char* autoRecordTargetName(AutoRecordTarget t);

struct RecordOptions {
  bool overdub = true;          // false = replace: what a pass plays over (from its first note on) is cleared
  bool quantize = true;         // note starts snap to `grid`
  bool quantizeLength = false;  // note lengths snap too (at least one grid)
  Tick grid = 24;               // ticks (the piano roll snap)
  bool takes = false;           // loop recording: every new loop pass records into a new pattern (a take)
  bool notes = true;            // record notes
  bool automation = true;       // record linked controller moves
  // K-123 (4.1): the rest of the input filter (each person's own, kept in their settings): pitch bend (an
  // MPE note's bend, linked bend moves), pressure (aftertouch: an MPE note's, linked pressure) and
  // program changes (linked). Notes and controllers are `notes` and `automation`.
  bool bend = true;
  bool pressure = true;
  bool program = true;
  AutoRecordTarget autoTarget = AutoRecordTarget::Auto;  // Auto: pattern events in pattern mode, clips in song mode
};

class Settings;
// K-226: the record options, the count-in (bars) and the punch switch, kept between runs (settings
// "record.midi.*", "record.countInBars", "record.punch"). `grid` is not kept (the piano roll snap).
void loadRecordOptions(const Settings& s, RecordOptions& o, int& countInBars, bool& punch);
void saveRecordOptions(Settings& s, const RecordOptions& o, int countInBars, bool punch);

// K-123 (4.1): what kind of MIDI message a status byte (and first data byte) is, for the input filter.
enum class MidiRecordKind : uint8_t { Note = 0, Controller, PitchBend, Pressure, Program, Other };
MidiRecordKind midiRecordKind(uint8_t status, uint8_t data1);
// Whether a recording takes this message in. The sustain pedal (CC 64) records with the notes.
bool recordFilterTakes(const RecordOptions& o, uint8_t status, uint8_t data1);
// The filter as one line ("notes 1 controllers 1 bend 1 pressure 1 program 1"), for the dev output.
std::string recordFilterText(const RecordOptions& o);

// K-123 (4.5): channels armed for recording notes (`armed`, in rack order). What live notes play: the
// selected channel when it is armed or nothing is armed, else the first armed one; and whether notes
// played on `channel` record (with channels armed: only theirs).
ObjectId armedLiveChannel(const std::vector<ObjectId>& armed, ObjectId selected);
bool armedRecords(const std::vector<ObjectId>& armed, ObjectId channel);

// Nearest grid line (grid <= 1: unchanged).
Tick quantizeTick(Tick t, Tick grid);

// K-226: where a recording runs, besides the options. Ticks are what NoteRecorder is fed: pattern
// ticks in pattern mode, song ticks in song mode.
struct RecordSetup {
  Tick start = 0;                   // where recording started (the transport position when it began)
  Tick pickup = 0;                  // notes played up to this much before `start` (the count-in's last beat) are kept at the start
  Tick punchIn = 0, punchOut = 0;   // song mode: only notes inside [in, out) record (out <= in: no punch)
  Tick loopStart = 0, loopEnd = 0;  // song mode: the range the transport loops over (end <= start: none known)
  // K-112B (R05-02): song mode: the track the take goes on (the Tracks layout's selected track; its
  // earlier regions there stay, the take plays over them). -1: the note's instrument track, else a free one.
  int track = -1;
  // K-112B (R14-04): song mode, merging: a take recorded over regions already on its track makes a take
  // folder with them (one take group, the take a lane of its own playing over them where it was
  // recorded), instead of stacking on them. Off: the take stacks, as before.
  bool takeFolder = false;
  bool takeLanes = true;  // with takeFolder: the track shows its takes as lanes
};

// Records notes (MIDI, the typing keyboard) and controller moves while the transport plays. Every
// edit of one recording goes into one undo step (also when it spans many passes); the notes appear
// as they are played.
class NoteRecorder {
 public:
  // Pattern mode: positions are pattern ticks looping over [0, loopLength). A note held over the
  // loop end is cut there: the pattern never grows during a take.
  void beginPattern(Document& doc, ObjectId pattern, const RecordOptions& o, Tick loopLength, const RecordSetup& setup = {});
  // Song mode: positions are song ticks. The take is a new pattern with one clip, placed with the
  // first note at the bar where recording started (on the instrument track of the note's channel
  // when it has one); the clip grows as recording goes on. `pattern` gives the take its bar layout.
  // Loop passes record into the same take, or a new one each (options().takes), stacked below.
  void beginSong(Document& doc, ObjectId pattern, const RecordOptions& o, const RecordSetup& setup = {});
  bool active() const { return active_; }
  bool songMode() const { return song_; }
  // The pattern recorded into now: pattern mode, the current take; song mode, the current take's
  // pattern (kNoId until the first note).
  ObjectId pattern() const { return pattern_; }
  ObjectId takeClip() const { return clip_; }  // song mode: the current take's clip
  // From now on records into `pattern` (pattern mode: an instrument's pass moved to a new
  // pattern, J-159). The notes held so far end in the pattern they started in.
  void retarget(ObjectId pattern) {
    if (active_ && !song_) pattern_ = pattern;
  }
  // Pattern mode: the loop length follows the pattern when it changes during a take.
  void setLoopLength(Tick len) {
    if (active_ && !song_ && len > 0) loop_ = len;
  }
  // Song mode: the range the transport loops over now (end <= start: none).
  void setSongLoop(Tick start, Tick end) {
    setup_.loopStart = start;
    setup_.loopEnd = end;
  }
  int pass() const { return pass_; }
  int takes() const { return static_cast<int>(takeIds_.size()); }
  int notesRecorded() const { return recorded_; }

  // Transport position update (every UI frame / before each event):
  // detects loop wraps (new pass, new take) and commits recorded automation.
  void position(Document& doc, double tick);
  void noteOn(Document& doc, ObjectId channel, int pitch, int velocity, double tick);
  void noteOff(Document& doc, ObjectId channel, int pitch, double tick);
  // K-99: per-note expression of the held note (`channel`, `pitch`) at `tick` (an MPE
  // controller's member channel): pitch in semitones off the key, pressure 0..1, timbre 0..1
  // (0.5 = centre). Kept with the note and written into its lanes (bend, pressure, timbre) when
  // it ends, thinned to what the engine plays. Nothing held there: ignored.
  void expression(ObjectId channel, int pitch, ExprDim dim, float value, double tick);
  // A linked parameter moved (normalized value) at `tick`.
  void controller(ParamId target, double normalized, double tick);
  // The sustain pedal (CC 64) of `channel` went down or up at `tick`: recorded with the notes
  // (as the channel's Sustain automation), so playback has the pedal as it was played.
  void pedal(ObjectId channel, bool down, double tick);
  // Closes held notes at `tick` and commits everything.
  void end(Document& doc, double tick);
  const RecordOptions& options() const { return opt_; }
  // Runs `fn`'s edits as part of this recording's undo step (a linked control moved meanwhile).
  void edit(Document& doc, const std::function<void()>& fn);
  // K-112B (R11-03): recorders taking several tracks at once share one undo step: the history
  // step this one's edits went into (0: none yet), and the step to go on with (another's).
  uint64_t step() const { return step_; }
  void joinStep(uint64_t serial) { step_ = serial; }

 private:
  struct Held {
    ObjectId channel;
    int velocity;
    double start;
    int pass;
    ObjectId pattern;
    Tick origin;  // song mode: the song tick of the pattern's tick 0 (the take's clip)
    double played;  // where the key went down (a pickup's start is moved to the take's start)
  };
  struct Mine {  // a note this pass recorded (replace mode keeps it)
    ObjectId pattern, channel;
    Tick start;
    int pitch;
  };
  struct Replace {  // replace mode: what this pass of a channel clears, [from, to) in its own ticks
    double from = 0, to = 0;
  };
  void commitNote(Document& doc, const Held& h, int pitch, double endTick);
  void commitAutomation(Document& doc);
  void newPass(Document& doc);
  void foldTakes(Document& doc);  // R14-04
  void cutHeld(Document& doc, double at);  // commits every held note, ending at `at`
  void clearReplaced(Document& doc, double upTo);
  void growTake(Document& doc, double tick);
  // Song mode: the current take, made when needed (for a note of `channel` at `tick`).
  bool ensureTake(Document& doc, double tick, ObjectId channel);
  double fold(double tick) const;  // pattern mode: into [0, loop)

  bool active_ = false, song_ = false, started_ = false;
  RecordOptions opt_;
  RecordSetup setup_;
  ObjectId base_ = kNoId, pattern_ = kNoId, clip_ = kNoId;
  Pattern baseContent_;  // the pattern as it was when recording started (takes start from it)
  Tick loop_ = 0;
  Tick origin_ = 0;      // song mode: the take's clip start
  int track_ = -1;       // song mode: the take's track
  int takePass_ = -1;    // song mode: the pass the current take was made in
  double last_ = -1e18;
  double wrapFrom_ = 0, wrapTo_ = 0;  // song mode: the last wrap (from the loop end to its start)
  int pass_ = 0, recorded_ = 0;
  uint64_t step_ = 0;  // the history step of this recording (0: none yet)
  std::vector<ObjectId> takeIds_;
  std::vector<ObjectId> takeClips_;
  std::map<std::pair<ObjectId, int>, Held> held_;  // (channel, pitch)
  // K-99: the held notes' expression as played: per dimension, (ticks after the key went down, value).
  std::map<std::pair<ObjectId, int>, std::array<std::vector<std::pair<double, float>>, 3>> heldExpr_;
  std::map<ObjectId, Replace> replace_;            // replace mode: by channel, this pass
  std::vector<Mine> mine_;                         // replace mode: the notes this pass recorded
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
  // GitHub #71: the arrow keys. Right with keys held makes the chord one step longer (it is
  // written on release, as one undo step, and the cursor moves past it); with none held it skips
  // a step. Left with keys held takes a step back off that length (never under the first step);
  // with none held the cursor moves back one step (nothing deleted).
  void right();
  void left();
  // Moves back one step and removes the notes starting there.
  void backspace(Document& doc, ObjectId pattern, ObjectId channel);
  int held() const { return static_cast<int>(down_.size()); }
  int extraSteps() const { return extra_; }  // steps Right added to the held chord
  // The length the held chord gets on release (one step when nothing is held).
  Tick pendingLength() const { return (lengthOverride > 0 ? lengthOverride : std::max<Tick>(1, step)) + extra_ * std::max<Tick>(1, step); }

 private:
  std::set<int> down_;
  std::map<int, int> chord_;  // pitch → velocity, collected while keys are down
  int extra_ = 0;             // GitHub #71: steps added while the keys are held
};

// K-112c: Capture. The notes played while nothing records are kept (the latest few hundred), so
// what was just played can still become notes afterwards: commit() writes the last phrase (the
// notes after the last pause longer than `gapSeconds`) of the channel played last into `pattern`,
// from its start, timed at `bpm`, note starts rounded to `grid` ticks (0: as played), added to the
// notes there as one undo step ("capture"). Returns how many notes were written (0: nothing kept)
// and forgets what was kept.
// K-123 (4.2): Recover what I just played. Notes kept for two minutes; a note played while the
// transport played (not recording) also keeps where it was heard (`CaptureTime`), so recover() puts
// the phrase back where it was played, as a recording would have: pattern mode, into the pattern that
// played; song mode, into a new take clip at the bar it was played in (on the channel's instrument
// track, else a free one). A phrase played while stopped goes in as commit() puts it.
struct CaptureTime {
  enum Mode : uint8_t { Stopped = 0, Pattern, Song };
  Mode mode = Stopped;
  double tick = -1;         // the tick heard (pattern ticks in pattern mode, song ticks in song mode)
  ObjectId pattern = kNoId;  // pattern mode: the pattern playing
};
class NoteCapture {
 public:
  static constexpr size_t kMax = 2048;
  static constexpr double kKeepSeconds = 120.0;
  void noteOn(ObjectId channel, int pitch, int velocity, double seconds, const CaptureTime& at = {});
  void noteOff(ObjectId channel, int pitch, double seconds, double tick = -1);
  void clear() { played_.clear(); }
  int size() const { return static_cast<int>(played_.size()); }
  ObjectId lastChannel() const { return played_.empty() ? kNoId : played_.back().channel; }
  int commit(Document& doc, ObjectId pattern, double bpm, Tick grid, double now, double gapSeconds = 4.0);
  struct Recovered {
    int notes = 0;
    bool timed = false;        // put where it was played
    ObjectId pattern = kNoId;  // where the notes went
    ObjectId clip = kNoId;     // song mode: the new take's clip
  };
  // `pattern`: the pattern a phrase played while stopped goes into (and, in song mode, the bar layout
  // of the new take); `o`: the record options (quantize, grid). One undo step; forgets what was kept.
  Recovered recover(Document& doc, ObjectId pattern, double bpm, const RecordOptions& o, double now, double gapSeconds = 4.0);

 private:
  struct Played {
    ObjectId channel;
    int pitch, velocity;
    double on, off;  // seconds; off < 0 while held
    CaptureTime at;  // where the note started (K-123)
    double offTick = -1;
  };
  // The last phrase of the channel played last (indexes into played_, in order).
  std::vector<size_t> lastPhrase(double now, double gapSeconds) const;
  std::vector<Played> played_;
};

// Computer keyboard as a piano (FL-like layout): lower row Z.. from the base
// octave, upper row Q.. one octave higher; octave and velocity keys; chord mode.
enum class ChordType : uint8_t { Major = 0, Minor, Seventh, Major7, Minor7, Sus4, Power, Octave, Count };
const char* chordTypeName(ChordType c);
std::vector<int> chordIntervals(ChordType c);

// K-113: the layout the typing keyboard names its keys after. Keys always play by where they sit;
// the layout only says what each place is called. Standard (US QWERTY) is the default, also for
// settings saved before there was a choice; Computer asks the operating system's current layout.
enum class TypingLayout : uint8_t { Standard = 0, Qwertz, Azerty, Dvorak, Colemak, Computer, Count };
const char* typingLayoutName(TypingLayout l);           // "Standard (QWERTY)", ..., "Follow the computer"
const char* typingLayoutId(TypingLayout l);             // the settings value: "standard", "qwertz", ..., "computer"
TypingLayout typingLayoutFromId(const std::string& id);  // anything unknown or empty: Standard

class TypingKeyboard {
 public:
  int octave = 4;      // lower row starts at baseNote() = 12 * octave
  int velocity = 100;  // 1..127
  bool chordMode = false;
  ChordType chord = ChordType::Major;
  // N48-08 (checkpoint 49): the Clips layout's note keys, as the reference's computer keyboard: the
  // A row plays from the base C (W E T Y U O P the black keys), Z and X move the octave, C and V
  // the velocity; M and the other keys play nothing.
  bool aRow = false;

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
  // The key code of a key by where it sits, whatever the layout types there: `position` is its USB
  // usage (SDL's scan codes use the same numbers, named after US QWERTY). 0 = not a typing key.
  static int keyAtPosition(int position);
  // The key at `position` in the mode in use (the A row's A, F and K count only with aRow).
  int keyAt(int position) const;
  // Where a key code sits, 0 if it has no place.
  static int positionOfKey(int key);
  // What to call a key on this keyboard: `typed`, the character its position types on the layout
  // in use (a Unicode code point), in capitals; the US character when it has no Latin-1 name.
  static std::string keyLabel(int key, int typed);
  // K-113: what `layout` types at `position` (a Unicode code point), 0 when it has nothing there or the
  // layout is Computer (the caller asks the system).
  static int layoutChar(TypingLayout layout, int position);
  bool anyHeld() const { return !held_.empty(); }
  std::vector<int> heldNotes() const;

  // K-175: physical key events, by where the key sits (`position`, its USB usage = SDL scan code).
  // Only a fresh press acts: a repeat, or a second press of a key already down (a copy of the
  // event, a held key reported again when the focus comes back), does nothing, so the octave,
  // velocity and chord keys move one step per press. press() returns the notes started,
  // release() the notes ended (a release of a key that is not down ends nothing).
  std::vector<int> press(int position, bool repeat = false);
  std::vector<int> release(int position);
  bool isDown(int position) const { return down_.count(position) != 0; }
  // The keyboard focus came to one of Gridline's windows: keys reported down before settle() are
  // keys the system reports as still held, not presses. They count as down and do nothing.
  void focusGained() { settling_ = true; }
  void settle() { settling_ = false; }
  bool settling() const { return settling_; }
  // Gridline lost the keyboard (or the typing keyboard was switched off): every key counts as up
  // and every note held ends.
  std::vector<int> releaseAllKeys();
  // The times the octave changed by a key (for the long-session checks).
  int octaveSteps() const { return octaveSteps_; }

 private:
  std::map<int, std::vector<int>> held_;
  std::set<int> down_;  // positions down
  bool settling_ = false;
  int octaveSteps_ = 0;
};

}  // namespace gl
