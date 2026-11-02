// Gridline — "spectral" pack: Turntable catalogue entries and factory presets:
// the effect (gl.turntablefx, K-23) and the 0.7.1 instrument (gl.turntable,
// hidden: old projects only). Ranges and defaults follow the reference deck
// (its stored internal values, linearly: Speed 1 = the sample's own speed,
// Turns 0 = no extra revolution); the instrument's note settings are Gridline's own.
#include "turntable.h"

namespace gl::packs::spectral {

namespace {
ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const char* n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
using V = std::vector<std::pair<int, float>>;

// The deck's parameters (the reference's 14 and the kept state), laid out as `L`.
void deckParams(PluginInfo& p, const TurntableLayout& L) {
  p.params.resize(static_cast<size_t>(L.count));
  p.params[TtPosition] = lin("Position", "%", 0, 1, 0);
  p.params[TtMute] = tog("Mute", 0);
  p.params[TtSpeed] = lin("Speed", "%", 0, 2, 1);
  p.params[TtAccel] = lin("Acceleration", "%", 0, 1, 0.25);
  p.params[TtPan] = lin("Pan", "pan", -1, 1, 0);
  p.params[TtVolume] = lin("Volume", "%", 0, 1, 0.78);
  p.params[TtHold] = tog("Hold", 0);
  p.params[TtSense] = lin("Sensitivity", "%", 0, 1, 0.5);
  p.params[TtTransport] = ch("Motor", {"Reverse", "Pause", "Forward"}, 1);
  p.params[TtSmooth1] = lin("Smooth 1", "%", 0, 1, 0.5);
  p.params[TtSmooth2] = lin("Smooth 2", "%", 0, 1, 0.908);
  p.params[TtSmooth3] = lin("Smooth 3", "%", 0, 1, 0.999);
  p.params[TtSmooth4] = lin("Smooth 4", "%", 0, 1, 0.4);
  p.params[TtTurns] = lin("Turns", "", -9, 9, 0);
  p.params[static_cast<size_t>(L.timecode)] =
      ch("Timecode", {"Off", "Vinyl 1 A", "Vinyl 1 B", "CD 1", "Vinyl 2 A", "Vinyl 2 B", "Vinyl 3 A", "Vinyl 3 B", "CD 2"}, 0);
  p.params[static_cast<size_t>(L.riaa)] = tog("Phono preamp", 0);
  p.params[static_cast<size_t>(L.zoom)] = tog("Large editor", 0);
  p.params[static_cast<size_t>(L.empty)] = tog("Empty deck", 0);
}
}  // namespace

PluginInfo turntableFxInfo() {
  PluginInfo p{kTurntableFx, "Turntable", "Misc", PluginKind::Effect, {}, true, {}};
  deckParams(p, kTtFxLayout);
  p.presets = {
      {"Deck", {}},
      {"Spinning deck", V{{TtTransport, 2.f}}},
      {"Heavy platter", V{{TtAccel, 0.04f}, {TtSmooth2, 0.98f}, {TtSmooth4, 0.7f}}},
      {"Quick hand", V{{TtAccel, 0.8f}, {TtSmooth1, 0.1f}, {TtSmooth2, 0.6f}, {TtSense, 0.75f}}},
      {"Half-speed spin", V{{TtTransport, 2.f}, {TtSpeed, 0.5f}, {TtAccel, 0.1f}}},
  };
  p.editorWidth = 470;
  return p;
}

PluginInfo turntableInfo() {
  PluginInfo p{kTurntableInst, "Turntable (old)", "Sampler", PluginKind::Generator, {}, true, {}};
  deckParams(p, kTtInstLayout);
  p.params[TtNotes] = ch("Notes", {"Off", "Motor", "Cue + motor"}, 2);
  p.params[TtKeyTrack] = tog("Key track", 1);
  p.params[TtCue] = lin("Cue", "%", 0, 1, 0);
  p.presets = {
      {"Deck", {}},
      {"Spinning deck", V{{TtTransport, 2.f}, {TtNotes, 0.f}}},
      {"Heavy platter", V{{TtAccel, 0.04f}, {TtSmooth2, 0.98f}, {TtSmooth4, 0.7f}}},
      {"Quick hand", V{{TtAccel, 0.8f}, {TtSmooth1, 0.1f}, {TtSmooth2, 0.6f}, {TtSense, 0.75f}}},
      {"Tape stop keys", V{{TtNotes, 1.f}, {TtAccel, 0.08f}, {TtKeyTrack, 0.f}}},
  };
  p.editorWidth = 470;
  p.hidden = true;  // K-23: Turntable is the effect now; this plays 0.7.1 projects as they were
  return p;
}

}  // namespace gl::packs::spectral
