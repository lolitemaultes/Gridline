// Gridline - tracker modules (K-76): the Tracker Sampler's parameters and its instrument text.
//
// An imported module instrument becomes a channel playing "gl.tracker". Its PluginState::data holds
// the instrument as text, in the layout every instrument that uses samples has (the first line is
// a sample file, later sample files are "file <path>" lines, so collecting, relinking and zipping
// the project find them), followed by the instrument's definition:
//   <sample file 0>
//   file <sample file 1>                                   (one per further sample)
//   #gl-tracker 1
//   name <instrument name>
//   ins <fadeout> <vibType> <vibSweep> <vibDepth> <vibRate>
//   map <96 hex digits: the sample of every note>
//   env v|p <flags> <count> <sustain> <loopStart> <loopEnd> <x>:<y> ...
//   smp <slot> <file> <length> <loopStart> <loopLength> <loop> <volume> <finetune> <pan> <relNote> [name]
// <file> numbers the sample files above (0 = the first line), -1 = none.
#pragma once
#include <array>
#include <string>
#include <vector>
#include "TrackerChannel.h"

namespace gl::tracker {

inline constexpr const char* kInstrumentType = "gl.tracker";

enum Param {
  PInterp,       // sample interpolation (Interp)
  PAmiga,        // pitch table: 0 = linear, 1 = Amiga periods
  PRowsPerBeat,  // pattern rows (steps) per beat, a choice of 1, 2, 4, 8, 16: how the instrument counts ticks
  PTicks,        // ticks per row for notes without tracker cells (drawn or played notes)
  PLevel,        // output level, linear
  PRamp,         // volume ramps (click removal) on / off
  PSound,        // K-120: built-in sound played while the instrument has no sample (0 = none;
                 // imported instruments and states saved before it keep 0)
  PCount
};
enum Interp { InterpNone, InterpLinear, InterpCubic, InterpSinc };
inline int rowsPerBeatOf(float choice) {
  const int i = choice < 0.5f ? 0 : choice > 3.5f ? 4 : static_cast<int>(choice + 0.5f);
  return 1 << i;
}
inline int rowsPerBeatChoice(int rows) {
  int i = 0;
  while (i < 4 && (1 << i) < rows) ++i;
  return i;
}

// MIDI pitch of a tracker note (1 = C-0 ... 96 = B-7): C-4 (49) is middle C (60).
inline constexpr int kPitchOffset = 11;

struct InstrumentText {
  std::string name;
  InstrumentDef def;
  std::vector<std::string> files;          // sample files, in order
  std::array<int, kMaxSamples> fileOf{};   // sample slot -> index in files (-1 = none)
  std::array<std::string, kMaxSamples> sampleNames;
  InstrumentText() { fileOf.fill(-1); }
};

std::string encodeInstrument(const InstrumentText& t);
// Reads what encodeInstrument wrote; anything malformed is skipped (missing values keep defaults).
// false when the text has no instrument part at all.
bool decodeInstrument(const std::string& data, InstrumentText& out);

}  // namespace gl::tracker
