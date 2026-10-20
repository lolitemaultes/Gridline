// Gridline — the master chain (RN-09): a small mastering chain on the master
// track built from Gridline's own effects, driven by a handful of macros.
//
//   Parametric EQ   tilt around ~1 kHz (low shelf down / high shelf up) + a
//                   gentle sub-sonic low cut
//   Compressor      "glue": slow RMS bus compression, detector high-passed
//   Stereo Shaper   width of the sides, bass kept mono below 120 Hz
//   Limiter         drive (input gain) into a peak ceiling
//   Level Meter     loudness readout (short-term / integrated LUFS)
//
// The chain occupies five consecutive master slots. The macros are stored in
// the effects' own parameters (readMasterChain recovers them), so the chain
// saves, loads, undoes and automates like any other effects.
#pragma once
#include <string>
#include <vector>
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::mastering {

struct Macros {
  float tilt = 0.0f;      // dB, -6..6: positive = brighter (highs up, lows down)
  float glue = 0.3f;      // 0..1 compression amount
  float width = 1.0f;     // 0..2 side level (1 = unchanged)
  float drive = 0.0f;     // dB, 0..12 limiter input gain
  float ceiling = -1.0f;  // dBFS, -6..0 limiter output ceiling
};
inline constexpr int kChainSlots = 5;
// Effect types of the chain, in order.
const std::vector<std::string>& chainTypes();

// Factory presets (original names).
struct Preset {
  const char* name;
  Macros macros;
};
const std::vector<Preset>& presets();

// First master slot of the chain (-1 = no chain on the master).
int chainSlot(const Song& s);
// The macros the chain's parameters stand for (false: no chain).
bool readMasterChain(const Song& s, Macros& out);
// Writes the macros into the chain's effect parameters (the chain must exist).
bool applyMacros(Song& s, const Macros& m);
// Adds the chain (in the last five free master slots, after the existing
// effects) and sets the macros; an existing chain only gets the macros.
bool installMasterChain(Song& s, const Macros& m, std::string* why = nullptr);
bool removeMasterChain(Song& s);

// Undoable versions. setMacros merges successive edits of one gesture
// (`mergeKey` != 0) into one undo step.
bool installMasterChain(Document& d, const Macros& m, std::string* why = nullptr);
bool removeMasterChain(Document& d);
bool setMacros(Document& d, const Macros& m, uint64_t mergeKey = 0);

// Limiter drive that moves a mix measured at `measuredLufs` (with the current
// macros) to `targetLufs`, within the drive range (a limiter makes this an
// estimate: measure again and repeat to converge).
float driveForTarget(const Macros& m, double measuredLufs, double targetLufs);

}  // namespace gl::mastering
