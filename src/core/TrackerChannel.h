// Gridline - tracker modules (K-76): the channel logic of a FastTracker 2 style replayer.
//
// One tracker channel's behaviour, tick by tick: reading a pattern cell (note, instrument, volume
// column, effect), the continuous effects of the following ticks, key off, the envelopes, fadeout
// and auto vibrato. The importer runs it over a whole module to learn when every voice starts and
// ends and what state it starts in; the Tracker Sampler instrument (packs/tracker) runs the same
// code on each note's own cells, so the two never disagree. No allocation, no I/O: the instrument
// calls it on the audio thread.
//
// Behaviour follows FastTracker 2 as documented by the ft2-clone project (BSD-3), including the
// quirks modules depend on. The Amiga period table is ft2-clone data (TrackerTables.cpp).
#pragma once
#include <array>
#include <cstdint>

namespace gl::tracker {

inline constexpr int kPeriodCount = 1936;  // 10 octaves x 12 notes x 16 finetune steps + 16
extern const uint16_t kAmigaPeriods[kPeriodCount];
inline constexpr int kNoteOff = 97;
inline constexpr int kMaxSamples = 16;
inline constexpr int kEnvPoints = 12;

// Period of table index i (note index * 16 + finetune / 8 + 16).
inline uint16_t periodAt(int i, bool linear) {
  if (i < 0) i = 0;
  if (i >= kPeriodCount) i = kPeriodCount - 1;
  return linear ? static_cast<uint16_t>((kPeriodCount - i) * 4) : kAmigaPeriods[i];
}
// Playback rate in Hz of a period, the way FT2 computes it (its fixed point steps included).
double periodHz(uint16_t period, bool linear);

enum EnvFlag : uint8_t { kEnvOn = 1, kEnvSustain = 2, kEnvLoop = 4 };
struct Envelope {
  std::array<int16_t, kEnvPoints> x{}, y{};  // ticks, value (volume 0..64, pan 0..63)
  uint8_t count = 0, sustain = 0, loopStart = 0, loopEnd = 0, flags = 0;
  bool on() const { return (flags & kEnvOn) != 0; }
};

enum LoopType : uint8_t { kLoopOff = 0, kLoopForward = 1, kLoopPingPong = 2 };
struct SampleDef {
  int32_t length = 0, loopStart = 0, loopLength = 0;  // frames
  uint8_t volume = 64;   // 0..64
  int8_t finetune = 0;   // -128..127
  uint8_t loop = kLoopOff;
  uint8_t pan = 128;     // 0..255
  int8_t relNote = 0;    // -48..71
  bool present() const { return length > 0; }
};

struct InstrumentDef {
  std::array<uint8_t, 96> keymap{};  // note -> sample 0..15
  Envelope vol, pan;
  uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
  uint16_t fadeout = 0;
  bool mute = false;
  std::array<SampleDef, kMaxSamples> samples{};
};
// The placeholder for empty / missing instruments (plays nothing, has no envelopes).
const InstrumentDef& emptyInstrument();

struct Cell {
  uint8_t note = 0, instr = 0, vol = 0, efx = 0, param = 0;
  bool empty() const { return note == 0 && instr == 0 && vol == 0 && efx == 0 && param == 0; }
  bool operator==(const Cell& o) const { return note == o.note && instr == o.instr && vol == o.vol && efx == o.efx && param == o.param; }
};

// What changed for the mixer on the last tick.
enum Status : uint8_t { kUpdVol = 1, kUpdPeriod = 2, kTrigger = 4, kUpdPan = 8, kQuickRamp = 16 };

// Everything a channel remembers between ticks (plain data: it is stored in imported notes).
struct State {
  uint8_t keyOff = 0, semitonePorta = 0, status = 0;
  int8_t relNote = 0, finetune = 0;
  uint8_t smpNum = 0, instrNum = 0, efxData = 0, efx = 0, sampleOffset = 0, tremorParam = 0, tremorPos = 0;
  uint8_t globVolSlideSpeed = 0, panSlideSpeed = 0, vibTremCtrl = 0, portaDir = 0;
  uint8_t vibPos = 0, tremPos = 0, vibSpeed = 0, vibDepth = 0, tremSpeed = 0, tremDepth = 0;
  uint8_t volSlideSpeed = 0, fVolUp = 0, fVolDown = 0;
  uint8_t fPitchUp = 0, fPitchDown = 0, efPitchUp = 0, efPitchDown = 0;
  uint8_t pitchUp = 0, pitchDown = 0, retrigSpeed = 0, retrigCounter = 0, retrigVol = 0;
  uint8_t volColumn = 0, noteNum = 0, panEnvPos = 0, autoVibPos = 0, volEnvPos = 0, realVol = 0, oldVol = 0, outVol = 0;
  uint8_t oldPan = 128, outPan = 128, finalPan = 128, pad0 = 0;
  int16_t volEnvDelta = 0, volEnvValue = 0, panEnvDelta = 0, panEnvValue = 0;
  uint16_t outPeriod = 0, realPeriod = 0, finalPeriod = 0, copyOfInstrAndNote = 0, portaTarget = 0, portaSpeed = 0;
  uint16_t volEnvTick = 0, panEnvTick = 0, autoVibAmp = 0, autoVibSweep = 0;
  uint16_t fadeoutVol = 0, fadeoutSpeed = 0;
  int32_t smpStartPos = 0;
  float finalVol = 0;  // 0..1 (global volume included)
};
static_assert(sizeof(State) == 88, "State is stored in imported notes: keep its size");

// Song-level requests a channel made on a tick (the importer applies them; an instrument ignores
// them). -1 = none.
struct SongFx {
  int speed = -1, bpm = -1, jump = -1, breakRow = -1, loop = -1, delay = -1, globalVol = -1;
  int globalSlide = -1;  // Hxy parameter (memory resolved), applied to the song's global volume
  bool any() const {
    return speed >= 0 || bpm >= 0 || jump >= 0 || breakRow >= 0 || loop >= 0 || delay >= 0 || globalVol >= 0 || globalSlide >= 0;
  }
};

// One channel: its state plus the instrument and sample it plays (not part of the stored state).
struct Channel {
  State s;
  const InstrumentDef* ins = nullptr;  // null = the placeholder
  // Instrument numbers 1..128 -> definitions (importer). When `fixed` is set every number
  // resolves to it (an instrument playing its own notes).
  const InstrumentDef* fixed = nullptr;
  const InstrumentDef* const* bank = nullptr;
  int bankSize = 0;
  bool linear = true;
  int globalVol = 64;  // 0..64 (the instrument keeps 64: the song's global volume is automation)

  const InstrumentDef& instrument() const { return ins ? *ins : emptyInstrument(); }
  const SampleDef& sample() const { return instrument().samples[s.smpNum & 15]; }
  void reset();  // a fresh channel (FT2's state after stop)

  // Tick 0 of a row: read the cell. `speed` = ticks per row.
  void row(const Cell& c, int speed, SongFx& fx);
  // A tick after tick 0 (`tick` counts down from speed - 1 to 1; repeated passes of a delayed
  // row start again at speed).
  void tick(int speed, int tickDown, SongFx& fx);
  // Envelopes, fadeout, auto vibrato and the final volume / pan / period, after row() or tick().
  void updateEnvelopes();
  // Takes the status flags of the last tick (and clears them).
  uint8_t takeStatus() {
    const uint8_t st = s.status;
    s.status = 0;
    return st;
  }

  void keyOff();
  void triggerNote(uint8_t note, uint8_t efx, uint8_t efxData);
  void triggerInstrument();
  void resetVolumes();

 private:
  const InstrumentDef* resolve(int n) const;
  void tickZeroEffects(SongFx& fx);
  void moreTickZero(SongFx& fx);
  void prepPorta(const Cell& c, uint8_t inst);
  void volSlide(uint8_t p);
  void portamento();
  void doVibrato();
  void arpeggio(uint8_t p, int tickDown);
  void doRetrig();
  void setEnvelopePos(uint8_t p);
  uint16_t notePeriodOf(uint16_t period, uint8_t offset) const;
};

}  // namespace gl::tracker
