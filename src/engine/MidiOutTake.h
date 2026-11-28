// Gridline — K-438: recording an instrument's MIDI output into a pattern while the transport plays
// (an installed arpeggiator or sequencer plugin, a built-in instrument's own sequencer). The audio
// thread copies the channel's output into the engine's capture ring (AudioEngine::captureMidiOf);
// the UI thread takes it from there every frame without allocating, pairs the note-ons and offs,
// and when recording ends writes the notes into a new pattern in one undo step.
#pragma once
#include <array>
#include <string>
#include <vector>
#include "../core/Document.h"
#include "AudioEngine.h"

namespace gl {

class MidiOutTake {
 public:
  // Starts recording what `channel` sends. `loop` > 0: pattern mode, the ticks loop over [0, loop)
  // (every loop pass records into the same pattern; a note held over the end is cut there). Else
  // song mode: the notes are placed from `origin` (a song tick, the pattern's start).
  void begin(AudioEngine& e, ObjectId channel, ObjectId likePattern, Tick loop, Tick origin = 0);
  bool active() const { return active_; }
  ObjectId channel() const { return channel_; }
  // UI frame: takes what the engine captured since (no allocation).
  void poll(AudioEngine& e);
  // Stops recording and writes the notes as `target`'s notes in a new pattern (one undo step).
  // The new pattern, or kNoId: nothing was played.
  ObjectId end(Document& d, AudioEngine& e, ObjectId target);
  // Stops without writing anything.
  void cancel(AudioEngine& e);
  int notes() const { return static_cast<int>(notes_.size()); }
  int held() const;
  static constexpr size_t kMaxNotes = 16384;
  static constexpr Tick kSameTicks = 2;  // loop passes: notes this close are the same note

 private:
  void message(const AudioEngine::CapturedMidi& m);
  void close(int slot, double at, uint32_t pass);
  Tick place(double tick) const;

  struct Open {
    double start = 0;
    uint32_t pass = 0;
    int velocity = 0;
    bool on = false;
  };
  bool active_ = false;
  ObjectId channel_ = kNoId, like_ = kNoId;
  Tick loop_ = 0, origin_ = 0;
  double last_ = 0;
  uint32_t lastPass_ = 0;
  std::array<AudioEngine::CapturedMidi, 1024> buf_{};
  std::array<Open, 16 * 128> open_{};
  std::vector<Note> notes_;  // reserved at begin (kMaxNotes)
};

}  // namespace gl
