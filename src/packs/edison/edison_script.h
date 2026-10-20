// Gridline — "edison" pack: audio editor scripts (SC-05 / ED-09).
//
// A script reads and rewrites the sample data of the editor's selection (or
// the whole file when nothing is selected) with block operations on float
// buffers, so a script never needs one Lua call per sample:
//
//   -- name: Soft fade in
//   -- category: Amplitude
//   function form(f) f:knob("Curve", 1, 0.2, 4) end
//   function apply(audio, p)
//     for c, buf in audio:buffers() do buf:ramp(0, 1, "slow") end
//   end
//
// `audio` = {rate, channels, frames, start, fileFrames, selected, cursor, seed}
// with channel(c), setChannel(c, buf), buffers(), resize(n), read(c, from, to),
// addMarker(pos, name), select(a, b). Buffers are userdata with vector
// operations (gain, ramps, mixing, filters, tones, noise, analysis, zero
// crossings, …); docs/scripting.md lists them all. The C loops are charged to
// the instruction budget, so the usual time / instruction / memory limits
// apply. Header "-- target: cursor" makes a script work at the cursor when
// nothing is selected (generators insert there) instead of on the whole file.
#pragma once
#include <string>
#include <vector>
#include "../../core/PianoRollScript.h"
#include "edison.h"

namespace gl::packs::edison {

struct AudioScriptContext {
  Content content;
  Range sel;            // empty = whole file (or the cursor, see above)
  int64_t cursor = 0;
  uint32_t seed = 1;
};

struct AudioScriptResult {
  bool ok = false;
  bool changed = false;
  bool limitHit = false;
  std::string error;
  std::vector<std::string> log;
  Content content;      // the new version (== the input when !changed)
  Range target;         // the range the script worked on (in the input)
  Range out;            // where the result sits in the new version
  Range sel;            // selection to show afterwards (in the new version)
  Audio preview;        // the processed frames (for listening before Accept)
};

class AudioScript {
 public:
  static constexpr int64_t kMaxFrames = int64_t{1} << 28;

  bool loadFile(const std::string& path, std::string* err = nullptr);
  bool loadSource(const std::string& source, const std::string& name, std::string* err = nullptr);
  const std::string& name() const { return name_; }
  const std::string& path() const { return path_; }
  const script::Header& header() const { return header_; }
  const script::Form& form() const { return form_; }
  bool loaded() const { return !source_.empty(); }
  bool atCursor() const { return atCursor_; }

  // The range a run works on.
  Range targetRange(const AudioScriptContext& ctx) const;
  // Memory needed for a run on `frames` frames is added to limits.memoryBytes.
  AudioScriptResult run(const AudioScriptContext& ctx, const std::vector<script::FormParam>& values = {},
                        const script::Limits& limits = {}) const;

 private:
  std::string source_, name_, path_;
  script::Header header_;
  script::Form form_;
  bool atCursor_ = false;
};

// Registers `buffer`, `dsp` and the buffer type in a VM (tests, the console).
void openAudioApi(script::Vm& vm);

}  // namespace gl::packs::edison
