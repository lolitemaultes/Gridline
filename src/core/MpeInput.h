// Gridline — K-99: what an MPE controller does to each held note, for recording.
//
// MPE (lower zone): MIDI channel 1 is the master channel, channels 2..16 carry one note each,
// and a member channel's pitch bend, channel pressure and CC 74 move that note alone. A note
// starts with the values its channel had just before it. The tracker turns that stream into
// per-note changes (the note's key, which dimension, the value) for NoteRecorder::expression.
// Values: pitch in semitones off the key, pressure 0..1, timbre 0..1 (0.5 = centre).
#pragma once
#include <cstdint>
#include "Types.h"

namespace gl {

class MpeInput {
 public:
  struct Change {
    int midiChannel = 0;  // 0..15
    int key = 60;
    ExprDim dim = ExprDim::Pressure;
    float value = 0;
  };
  float bendRange = 48.0f;  // semitones of a full member-channel bend
  // Feeds one message; returns how many changes it made (written to `out`, up to `max`). A note
  // on reports the note's starting values that are not neutral; a note off reports nothing.
  // Master channel (channel 1) messages change nothing here.
  int feed(uint8_t status, uint8_t d1, uint8_t d2, Change* out, int max);
  // The key held on a member channel (-1: none).
  int held(int channel) const { return channel >= 0 && channel < 16 ? key_[channel] : -1; }
  void reset();

 private:
  int key_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  float bend_[16] = {}, pressure_[16] = {};
  float timbre_[16] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
};

}  // namespace gl
