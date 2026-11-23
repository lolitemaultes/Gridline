// Gridline — "edison" pack: the tool panels (edison_repair_ui.cpp). Each tool's settings are
// drawn by one function that works on a ToolParams (edison_process.h) and a PanelContext (the
// audio it is for and a few helpers), so any window can show the same settings: Loupe's tool
// dialogs, and a clip's menu in the playlist. The Preview section (loop the result while the
// settings change, A: processed, B: original, C: what is removed) works from the same context.
// Loupe's own parts (the dialog, its Accept footer with progress and Stop, the marks on its
// waveform) are at the end.
#pragma once
#include <functional>
#include <string>
#include "edison_process.h"
#include "edison_ui.h"

namespace gl::packs::edison {

struct PanelContext {
  uint64_t key = 0;           // the caller's identity: analysis and the preview are kept per key
  AudioPtr audio;             // the audio the tool is for
  Range target;               // the part it works on (in audio frames)
  int64_t cursor = -1;        // where a preview of a long target starts (when inside it)
  double songTempo = 120;
  int beatsPerBar = 4;
  std::function<std::string(int64_t frames)> timeText;  // lengths and positions as the caller shows them
  // Denoise: learn a profile from the caller's selection (true when one was taken).
  std::function<bool(NoiseProfile& out)> takeProfile;
  const PeakCache* peaks = nullptr;  // the audio's peaks, for the panels' overviews (optional)
};

// The settings of `p.kind` (sections and presets). False when there is nothing to apply yet
// (no clipping found, no profile, an analysis still running...).
bool drawToolPanel(ToolParams& p, const PanelContext& c);
// A key that changes with every setting that changes the result.
std::string paramsKey(const ToolParams& p);
// The Preview section. `beforeListen` runs when listening starts (the caller stops its own
// playback). The result is made with processContent on a part of up to 12 s (the whole target
// for a tool that changes the length).
void drawPreview(App& a, const PanelContext& c, const ToolParams& p, const std::function<void()>& beforeListen = {});
// The preview of `key` stops (and its renders are dropped).
void stopPreview(App& a, uint64_t key);
// What a panel found, in audio frames (declip: clipped regions; declick: clicks; drum loop
// stretch: hits), for the caller's waveform. Null when none.
struct PanelMarks {
  std::vector<Range> regions;
  std::vector<int64_t> ticks;
};
const PanelMarks* panelMarks(uint64_t key);

// ---- Loupe
// The tool's kind for one of Loupe's tool dialogs (Count: not a panel tool).
ToolKind panelKind(Tool t);
// The panel tools' dialogs (inside the tool popup). False when `s.tool` is not one.
bool drawRepairDialog(App& a, Session& s, Range tr);
// The tool popup closed: the preview stops.
void repairToolClosed(App& a, Session& s);
// The marks of the open dialog, drawn by the waveform view inside the lanes' clip rect.
void drawRepairMarks(const Session& s, ImDrawList* dl, ImVec2 p0, ImVec2 p1, double viewStart, double viewLen);

}  // namespace gl::packs::edison
