// Gridline — note processors ("MIDI FX"): per-channel chains that transform
// note events before they reach the instrument (arpeggiator, chords, step
// sequencer, key mapping, keyboard splits, velocity curves, note scripts).
// Runtimes are used only by the audio thread; everything is fixed-size so a
// chain never allocates. Timing is computed from the transport (or a free
// running clock when stopped) in samples, so results do not depend on the
// audio block size.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "../core/PluginInfo.h"
#include "../core/Types.h"
#include "Generators.h"

namespace gl {

// One note event inside a block.
struct NoteMsg {
  enum Kind : uint8_t { On, Off, AllOff };
  Kind kind = On;
  int offset = 0;          // sample offset within the block
  uint32_t id = 0;         // matches a note-on with its note-off
  float pitch = 60;        // MIDI note (fractional = fine pitch)
  float velocity = 0.8f;   // 0..1
  int16_t color = 0;       // colour group 0..15 (MIDI channel for live input)
  int16_t target = -1;     // -1 = this channel; else a Program channel index
  // Per-note properties (see NoteProps); processors that create notes may leave them neutral.
  float pan = 0, release = 0.5f, modX = 0.5f, modY = 0.5f;
  // J-180: semitones of `pitch` beyond the note's key (its fine pitch, NoteProps::detune);
  // processors that set a new pitch reset it.
  float detune = 0;
};

// Fixed-capacity event list (stays sorted by offset when sort() is called).
struct NoteBuffer {
  static constexpr int kCap = 512;
  NoteMsg ev[kCap];
  int count = 0;
  void clear() { count = 0; }
  bool push(const NoteMsg& m) {
    if (count >= kCap) return false;
    ev[count++] = m;
    return true;
  }
  void sort() {  // stable insertion sort (lists are short)
    for (int i = 1; i < count; ++i) {
      NoteMsg e = ev[i];
      int j = i;
      while (j > 0 && ev[j - 1].offset > e.offset) {
        ev[j] = ev[j - 1];
        --j;
      }
      ev[j] = e;
    }
  }
};

// Timing of one transport segment of the block (the block splits where the
// loop wraps). Ticks map to samples exactly as the sequencer does it.
struct NoteFxContext {
  double sampleRate = 44100;
  double tempo = 140;
  int ppq = 96;
  int from = 0, len = 0;   // segment = block samples [from, from + len)
  double anchorTick = 0;   // tick at clock 0
  int64_t clock = 0;       // samples since the anchor at `from`
  double spt = 1;          // samples per tick
  bool playing = false;    // transport running (else a free-running clock)
  uint32_t* idCounter = nullptr;

  // First block offset at or after tick `t` (may lie outside the segment).
  int64_t offsetOf(double t) const {
    return from + static_cast<int64_t>(std::ceil((t - anchorTick) * spt - 1e-9)) - clock;
  }
  // Tick at block offset `off`.
  double tickAt(int64_t off) const { return anchorTick + static_cast<double>(clock + off - from) / spt; }
  int end() const { return from + len; }
  uint32_t newId() const {
    if (!idCounter) return 0x30000000u;
    uint32_t id = 0x20000000u | ((*idCounter)++ & 0x0FFFFFFFu);
    return id;
  }
};

// Data prepared on the UI thread when the program is built.
struct NoteFxAux {
  static constexpr int kTargets = 16;
  int targets[kTargets] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // channel indices
  std::shared_ptr<const AuxData> custom;  // e.g. a compiled note script
};

class NoteFx {
 public:
  virtual ~NoteFx() = default;
  // Processes one segment. `in` holds the incoming events of the segment
  // (offsets inside [ctx.from, ctx.end()), sorted); results are appended to
  // `out` with offsets inside the same range.
  virtual void process(const PluginState& state, const NoteFxAux& aux, const NoteFxContext& ctx, const NoteBuffer& in,
                       NoteBuffer& out) = 0;
  // Forgets held notes without emitting anything (panic / chain rebuilt).
  virtual void reset() = 0;
  // Current step / activity for editors (-1 = idle).
  virtual int position() const { return -1; }
};

// Defined by the modulation pack: runtime for a PluginKind::NoteFx type
// (nullptr if unknown), and aux preparation (UI thread).
std::unique_ptr<NoteFx> createNoteFx(const std::string& type);
void prepareNoteFxAux(const PluginState& state, NoteFxAux& aux);

// PluginState::data of note processors: an optional first line
// "targets=<channel id>,…" (routing processors) followed by free text
// (scripts). Helpers shared by the engine and the editors.
inline std::vector<ObjectId> noteFxTargets(const std::string& data) {
  std::vector<ObjectId> out;
  if (data.compare(0, 8, "targets=") != 0) return out;
  size_t e = data.find('\n');
  std::string line = data.substr(8, e == std::string::npos ? std::string::npos : e - 8);
  size_t p = 0;
  while (p <= line.size()) {
    size_t c = line.find(',', p);
    std::string tok = line.substr(p, c == std::string::npos ? std::string::npos : c - p);
    out.push_back(static_cast<ObjectId>(std::strtoul(tok.c_str(), nullptr, 10)));
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}
inline std::string noteFxBody(const std::string& data) {
  if (data.compare(0, 8, "targets=") != 0) return data;
  size_t e = data.find('\n');
  return e == std::string::npos ? std::string() : data.substr(e + 1);
}
inline std::string makeNoteFxData(const std::vector<ObjectId>& targets, const std::string& body) {
  if (targets.empty()) return body;
  std::string s = "targets=";
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i) s += ',';
    s += std::to_string(targets[i]);
  }
  return s + "\n" + body;
}

}  // namespace gl
