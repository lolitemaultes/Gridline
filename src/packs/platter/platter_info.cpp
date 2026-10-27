// Gridline — "platter" pack: catalogue entry for Platter (gl.platter).
#include "../Packs.h"
#include "platter.h"

namespace gl::packs::platter {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Platter", "Sampler", PluginKind::Generator, {}, true, {}};
  p.params.resize(PCount);
  p.params[PVolume] = lin("Channel fader", "%", 0, 1, 1);
  p.params[PTrim] = lin("Trim", "dB", -12, 12, 0);
  p.params[PEqHigh] = lin("EQ high", "dB", kEqKillDb, 6, 0);
  p.params[PEqMid] = lin("EQ mid", "dB", kEqKillDb, 6, 0);
  p.params[PEqLow] = lin("EQ low", "dB", kEqKillDb, 6, 0);
  p.params[PFilter] = lin("Filter", "", -1, 1, 0);
  p.params[PFilterRes] = lin("Filter resonance", "%", 0, 1, 0.25);
  p.params[PPitch] = lin("Pitch fader", "", -1, 1, 0);
  p.params[PPitchRange] = ch("Pitch range", {"4%", "8%", "16%", "50%"}, Range8);
  p.params[PSync] = tog("Tempo sync", 1);
  p.params[PKeyLock] = tog("Key lock", 1);
  p.params[PQuantize] = tog("Quantize", 1);
  p.params[PSlip] = tog("Slip", 0);
  p.params[PVinyl] = tog("Vinyl mode", 1);
  p.params[PStartTime] = lin("Start time", "s", 0, 3, 0.08);
  p.params[PStopTime] = lin("Stop time", "s", 0, 3, 0.2);
  p.params[PFeel] = lin("Hand feel", "%", 0, 1, 0.5);
  p.params[PXfader] = lin("Crossfader", "", 0, 1, 0.5);
  p.params[PXfSide] = ch("Crossfader side", {"Thru", "A", "B"}, XfThru);
  p.params[PXfCurve] = lin("Crossfader curve", "%", 0, 1, 0.5);
  p.params[PFaderCurve] = ch("Fader curve", {"Smooth", "Linear", "Sharp"}, FcSmooth);
  p.params[PNoteMode] = ch("Note mode", {"Gate", "Trigger"}, NoteGate);
  p.params[PLoopSize] = ch("Loop size", {"1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32"}, 5);
  p.editorWidth = 1120;

  // Factory set-ups (original).
  p.presets.push_back({"Club Deck", {}});
  p.presets.push_back({"Scratch Deck", {{PXfSide, XfA}, {PXfCurve, 1.0f}, {PFaderCurve, FcSharp}, {PStartTime, 0.02f}, {PStopTime, 0.08f},
                                        {PFeel, 0.75f}, {PKeyLock, 0.f}}});
  p.presets.push_back({"Vinyl Feel", {{PStartTime, 0.7f}, {PStopTime, 1.2f}, {PKeyLock, 0.f}, {PFeel, 0.4f}}});
  p.presets.push_back({"Free Tempo", {{PSync, 0.f}, {PKeyLock, 0.f}, {PPitchRange, Range16}}});
  p.presets.push_back({"Trigger Pads", {{PNoteMode, NoteTrigger}, {PQuantize, 1.f}}});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::platter
