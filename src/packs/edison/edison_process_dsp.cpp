// Gridline — "edison" pack: Loupe's tools as plain functions (edison_process.h).
#include "edison_process.h"

namespace gl::packs::edison {

const char* toolName(ToolKind k) {
  switch (k) {
    case ToolKind::Denoise: return "Clean up";
    case ToolKind::Declick: return "Declick";
    case ToolKind::Declip: return "Declip";
    case ToolKind::Deverb: return "Deverb";
    case ToolKind::Gate: return "Noise gate";
    case ToolKind::Blur: return "Blur";
    case ToolKind::Reverb: return "Reverb";
    case ToolKind::Equalize: return "Equalize";
    case ToolKind::Normalize: return "Normalize";
    case ToolKind::Amplify: return "Amplify";
    case ToolKind::Envelope: return "Envelope";
    case ToolKind::FadeIn: return "Fade in";
    case ToolKind::FadeOut: return "Fade out";
    case ToolKind::Reverse: return "Reverse";
    case ToolKind::DrumStretch: return "Drum loop stretch";
    case ToolKind::SpectralBrush: return "Spectral edit";
    default: return "Tool";
  }
}

bool changesLength(ToolKind k) { return k == ToolKind::DrumStretch || k == ToolKind::Reverb; }

bool hasRemoved(ToolKind k) {
  return k == ToolKind::Denoise || k == ToolKind::Declick || k == ToolKind::Declip || k == ToolKind::Deverb || k == ToolKind::Gate ||
         k == ToolKind::SpectralBrush;
}

Content processContent(const Content& c, Range r, const ToolParams& p, Progress* prog) {
  const Range rr = rangeOrAll(c, r);
  Content out = c;
  switch (p.kind) {
    case ToolKind::Denoise: out = denoiseHq(c, rr, p.denoise, prog); break;
    case ToolKind::Declick: out = declickHq(c, rr, p.declick, prog); break;
    case ToolKind::Declip: out = declip(c, rr, p.declip, prog); break;
    case ToolKind::Deverb: out = deverb(c, rr, p.deverb, prog); break;
    case ToolKind::Gate: out = gateHq(c, rr, p.gate, prog); break;
    case ToolKind::Blur: out = blur(c, rr, p.blurMs, p.blurMix); break;
    case ToolKind::Reverb: out = reverb(c, rr, p.reverb); break;
    case ToolKind::Equalize: out = equalize(c, rr, p.eq); break;
    case ToolKind::Normalize: out = normalize(c, rr, p.normalizeDb); break;
    case ToolKind::Amplify: out = gain(c, rr, p.amplifyDb); break;
    case ToolKind::Envelope: out = applyEnvelope(c, rr, p.envelope); break;
    case ToolKind::FadeIn: out = fadeIn(c, rr, p.fade); break;
    case ToolKind::FadeOut: out = fadeOut(c, rr, p.fade); break;
    case ToolKind::Reverse: out = reverse(c, rr); break;
    case ToolKind::DrumStretch: out = drumStretch(c, rr, p.drum, prog); break;
    case ToolKind::SpectralBrush: out = spectralGainSoft(c, p.brush, p.brushDb, prog); break;
    default: break;
  }
  if (stopped(prog)) return c;
  report(prog, 1.0);
  return out;
}

Audio processAudio(const Audio& in, const ToolParams& p, Progress* prog) {
  const Content c = makeContent(in);
  return processContent(c, {0, c.frames()}, p, prog).a();
}

}  // namespace gl::packs::edison
