// Gridline — "platter" pack: catalogue entry for Platter (gl.platter): two
// decks (A keeps J-150's parameter ids, B is appended) and the mixer.
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
  for (int d = 0; d < kDecks; ++d) {
    const std::string k = d == 0 ? "A " : "B ";
    auto at = [&](int dp) -> ParamDesc& { return p.params[static_cast<size_t>(deckParam(d, dp))]; };
    at(DVolume) = lin(k + "channel fader", "%", 0, 1, 1);
    at(DTrim) = lin(k + "trim", "dB", -12, 12, 0);
    at(DEqHigh) = lin(k + "EQ high", "dB", kEqKillDb, 6, 0);
    at(DEqMid) = lin(k + "EQ mid", "dB", kEqKillDb, 6, 0);
    at(DEqLow) = lin(k + "EQ low", "dB", kEqKillDb, 6, 0);
    at(DFilter) = lin(k + "filter", "", -1, 1, 0);
    at(DFilterRes) = lin(k + "filter resonance", "%", 0, 1, 0.25);
    at(DPitch) = lin(k + "pitch fader", "", -1, 1, 0);
    at(DPitchRange) = ch(k + "pitch range", {"4%", "8%", "16%", "50%"}, Range8);
    at(DSync) = tog(k + "tempo sync", 1);
    at(DKeyLock) = tog(k + "key lock", 1);
    at(DQuantize) = tog(k + "quantize", 1);
    at(DSlip) = tog(k + "slip", 0);
    at(DVinyl) = tog(k + "vinyl mode", 1);
    at(DStartTime) = lin(k + "start time", "s", 0, 3, 0.08);
    at(DStopTime) = lin(k + "stop time", "s", 0, 3, 0.2);
    at(DFeel) = lin(k + "hand feel", "%", 0, 1, 0.5);
    at(DXfSide) = ch(k + "crossfader side", {"Thru", "A", "B"}, d == 0 ? XfA : XfB);
    at(DLoopSize) = ch(k + "loop size", {"1/8", "1/4", "1/2", "1", "2", "4", "8", "16", "32"}, 5);
    at(DJog) = lin(k + "jog sensitivity", "times", 0.25, 2, kJogSensDefault);
  }
  p.params[PXfader] = lin("Crossfader", "", 0, 1, 0.5);
  p.params[PXfCurve] = lin("Crossfader curve", "%", 0, 1, 0.5);
  p.params[PFaderCurve] = ch("Fader curve", {"Smooth", "Linear", "Sharp"}, FcSmooth);
  p.params[PNoteMode] = ch("Note mode", {"Gate", "Trigger"}, NoteGate);
  p.params[PSyncTo] = ch("Sync to", {"Project", "Other deck"}, SyncProject);
  p.params[PMaster] = lin("Master level", "dB", kMasterOffDb, 6, 0);
  p.params[PXfCut] = tog("Crossfader cut", 0);
  p.params[PXfReverse] = tog("Crossfader reverse", 0);
  p.editorWidth = 1240;
  p.editorMinWidth = 940;  // reflows in a narrower window
  p.noteFxPanel = false;  // a DJ setup: notes trigger cues and takes, no note processors

  // Factory set-ups (original).
  const int bXf = deckParam(1, DXfSide);
  p.presets.push_back({"Club Mix", {}});
  p.presets.push_back({"Scratch Setup", {{PXfCurve, 1.0f}, {PXfCut, 1.f}, {PFaderCurve, FcSharp}, {PStartTime, 0.02f}, {PStopTime, 0.08f},
                                         {PFeel, 0.75f}, {PKeyLock, 0.f}, {deckParam(1, DStartTime), 0.02f}, {deckParam(1, DStopTime), 0.08f},
                                         {deckParam(1, DFeel), 0.75f}, {deckParam(1, DKeyLock), 0.f}}});
  p.presets.push_back({"Vinyl Feel", {{PStartTime, 0.7f}, {PStopTime, 1.2f}, {PKeyLock, 0.f}, {PFeel, 0.4f}, {deckParam(1, DStartTime), 0.7f},
                                      {deckParam(1, DStopTime), 1.2f}, {deckParam(1, DKeyLock), 0.f}, {deckParam(1, DFeel), 0.4f}}});
  p.presets.push_back({"Free Tempo", {{PSync, 0.f}, {PKeyLock, 0.f}, {PPitchRange, Range16}, {deckParam(1, DSync), 0.f},
                                      {deckParam(1, DKeyLock), 0.f}, {deckParam(1, DPitchRange), Range16}}});
  p.presets.push_back({"Deck to Deck", {{PSyncTo, SyncDeck}, {PSync, 0.f}}});
  p.presets.push_back({"Trigger Pads", {{PNoteMode, NoteTrigger}, {PXfSide, XfThru}, {bXf, XfThru}}});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::platter
