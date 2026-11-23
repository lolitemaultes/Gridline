// Gridline — "edison" pack: Loupe's audio tools as plain functions, for any caller (Loupe's
// dialogs, the playlist's clip menu, scripts, tests). No UI: an audio buffer (Audio: rate,
// 1 or 2 channels), the tool and its settings, an optional Progress (report / Stop), and the
// processed buffer back. Implemented in edison_process_dsp.cpp on top of the edit operations
// (edison.h) and the repair tools (edison_repair.h).
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "edison.h"
#include "edison_repair.h"

namespace gl::packs::edison {

enum class ToolKind {
  Denoise, Declick, Declip, Deverb, Gate, Blur, Reverb, Equalize, Normalize, Amplify, Envelope, FadeIn, FadeOut, Reverse, DrumStretch,
  SpectralBrush, Count
};

// Every tool's settings; `kind` picks the one used.
struct ToolParams {
  ToolKind kind = ToolKind::Denoise;
  DenoiseSettings denoise;
  DeclickSettings declick;
  DeclipSettings declip;
  DeverbSettings deverb;
  GateSettings gate;
  double blurMs = 40, blurMix = 0.5;
  ReverbSettings reverb;
  std::vector<EqPoint> eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}};
  double normalizeDb = 0;   // peak level, dBFS
  double amplifyDb = 3;
  std::vector<std::pair<double, double>> envelope = {{0, 1}, {1, 1}};  // (x 0..1, gain 0..2)
  Curve fade = Curve::Linear;
  DrumStretchSettings drum;
  std::vector<SpectralRect> brush;  // frames of the buffer the tool runs on
  double brushDb = -30;
};

const char* toolName(ToolKind k);  // English, for undo labels and titles ("Deverb")
// The tool changes the length of what it works on (drum loop stretch; reverb when its tail
// runs past the end).
bool changesLength(ToolKind k);
// Whether a "listen to what is removed" (input minus output) means something for the tool.
bool hasRemoved(ToolKind k);

// The tool on c over r (empty r: all of it); markers, regions and loop points follow as the
// edit operations keep them. A stopped run returns c unchanged.
Content processContent(const Content& c, Range r, const ToolParams& p, Progress* prog = nullptr);
// The tool on a whole buffer.
Audio processAudio(const Audio& in, const ToolParams& p, Progress* prog = nullptr);

}  // namespace gl::packs::edison
