// Gridline — "tideline" pack: catalogue entry for Tideline (gl.tideline).
#include "../Packs.h"
#include "tideline.h"

namespace gl::packs::tideline {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

using Values = std::vector<std::pair<int, float>>;

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.tideline", "Tideline", "Dynamics", PluginKind::Effect, {}, false, {}};
  p.params.resize(BwCount);
  std::vector<std::string> shapes(kShapeNames, kShapeNames + kShapeCount + 1);
  std::vector<std::string> rates(kRateNames, kRateNames + kRateCount);
  p.params[BwAmount] = lin("Amount", "%", 0, 1, 1);
  p.params[BwShape] = ch("Shape", shapes, 0);
  p.params[BwRate] = ch("Rate", rates, kRateDefault);
  p.params[BwTrigger] = ch("Trigger", {"Tempo", "Audio", "MIDI"}, TrigTempo);
  p.params[BwLoop] = tog("Loop", 0);
  p.params[BwEnd] = lin("One-shot end", "%", 0, 1, 1);
  p.params[BwDuration] = lin("Duration", "%", -1, 1, 0);
  p.params[BwShift] = lin("Shift", "%", -0.5, 0.5, 0);
  p.params[BwSmooth] = lin("Smoothing", "ms", 0, 20, 1);
  p.params[BwBand] = ch("Band", {"Full", "Lows", "Highs"}, BandFull);
  p.params[BwBandFreq] = lg("Band split", "Hz", 20, 5120, 120);
  p.params[BwBandListen] = tog("Band listen", 0);
  p.params[BwStereo] = ch("Stereo", {"Stereo", "Mid", "Side"}, StStereo);
  p.params[BwScIn] = lin("Trigger input", "", -1, 500, -1);
  p.params[BwThreshold] = lin("Threshold", "dB", -60, 0, -24);
  p.params[BwRetrigger] = lg("Retrigger gap", "ms", 5, 2000, 60);
  p.params[BwDetLow] = lg("Detect low cut", "Hz", 20, 2000, 30);
  p.params[BwDetHigh] = lg("Detect high cut", "Hz", 40, 20000, 200);
  p.params[BwMidiPort] = lin("MIDI port", "", -1, 255, -1);
  p.params[BwMidiNote] = lin("MIDI note", "", -1, 127, -1);
  p.params[BwTension] = lin("Curve tension", "%", -1, 1, 0.3);
  for (int i = 0; i < kNodes; ++i) {
    p.params[static_cast<size_t>(BwNodeX0 + i)] = lin("Point " + std::to_string(i + 1) + " time", "%", 0, 1, kDefNodeX[i]);
    p.params[static_cast<size_t>(BwNodeY0 + i)] = lin("Point " + std::to_string(i + 1) + " level", "%", 0, 1, kDefNodeY[i]);
  }
  p.params[BwNoteSrc] = ch("Note source", {"MIDI Out ports", "Channel", "MIDI input"}, SrcPorts);
  p.params[BwMidiNoteHi] = lin("MIDI note to", "", -1, 127, -1);
  p.params[BwLookahead] = lin("Look-ahead", "ms", 0, kMaxLookaheadMs, 0);
  p.sideInParam = BwScIn;
  p.midiInput = true;
  p.editorWidth = 720;

  // Factory presets (original).
  p.presets.push_back({"House Pump", {{BwShape, 0}}});
  p.presets.push_back({"Gentle Breathing", {{BwShape, 6}, {BwAmount, 0.35f}}});
  p.presets.push_back({"Sub Clearance", {{BwShape, 8}, {BwBand, BandLows}, {BwBandFreq, 110}}});
  p.presets.push_back({"Eighth Chug", {{BwShape, 1}, {BwRate, 1}, {BwAmount, 0.8f}}});
  p.presets.push_back({"Half-Time Swell", {{BwShape, 5}, {BwRate, 3}}});
  p.presets.push_back({"Reverse Suck", {{BwShape, 12}, {BwAmount, 0.9f}}});
  p.presets.push_back({"Kick Follower", {{BwShape, 2}, {BwTrigger, TrigAudio}, {BwThreshold, -30}}});
  p.presets.push_back({"MIDI Gate", {{BwShape, 3}, {BwTrigger, TrigMidi}}});
  p.presets.push_back({"Wide Pads Only", {{BwShape, 4}, {BwStereo, StSide}, {BwAmount, 0.7f}}});
  p.presets.push_back({"Triplet Stutter", {{BwShape, 15}, {BwRate, 6}, {BwAmount, 0.6f}}});
  p.presets.push_back({"Hand Drawn", {{BwShape, kShapeCustom}}});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::tideline
