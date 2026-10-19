// Gridline — "spectral" pack: catalogue entries.
#include "../Packs.h"
#include "spectral.h"

namespace gl::packs::spectral {

namespace {

ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lg(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const char* n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const char* n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
using V = std::vector<std::pair<int, float>>;

PluginInfo vocoder() {
  PluginInfo p{"gl.vocoder", "Vocoder", "Spectral", PluginKind::Effect, {}, false, {}};
  p.midiInput = true;  // carrier synth keys from a MIDI port
  p.params.resize(VcCount);
  p.params[VcBands] = ch("Bands", {"4", "8", "12", "16", "24", "32", "48", "64", "96", "128"}, 5);
  p.params[VcLowFreq] = lg("Low band", "Hz", 20, 2000, 90);
  p.params[VcHighFreq] = lg("High band", "Hz", 1000, 20000, 11000);
  p.params[VcSpacing] = lin("Spacing", "", -1, 1, 0);
  p.params[VcFormant] = lin("Formant", "st", -12, 12, 0);
  p.params[VcBandwidth] = lg("Bandwidth", "%", 0.1, 3, 1);
  p.params[VcOrder] = ch("Slope", {"12 dB", "24 dB", "36 dB"}, 1);
  p.params[VcAttack] = lg("Attack", "ms", 0.5, 200, 3);
  p.params[VcRelease] = lg("Release", "ms", 5, 1500, 40);
  p.params[VcHold] = tog("Hold", 0);
  p.params[VcInvert] = tog("Invert bands", 0);
  p.params[VcCarrier] = ch("Carrier", {"Input", "Saw", "Pulse", "Noise", "Saw+Noise"}, 0);
  p.params[VcRoot] = lin("Carrier note", "note", 24, 84, 48);
  p.params[VcChord] = ch("Chord", {"Single", "Octave", "Fifth", "Major", "Minor", "Sus4", "Maj7", "Min7"}, 0);
  p.params[VcFlatten] = lin("Flatten", "%", 0, 1, 0.5);
  p.params[VcSibilance] = lin("Sibilance", "%", 0, 1, 0.3);
  p.params[VcSpread] = lin("Band spread", "%", 0, 1, 0);
  p.params[VcModLevel] = lin("Modulator", "dB", -60, 6, -60);
  p.params[VcCarLevel] = lin("Carrier", "dB", -60, 6, -60);
  p.params[VcWetLevel] = lin("Wet", "dB", -60, 12, 0);
  p.params[VcSwap] = tog("Swap inputs", 0);
  p.params[VcListen] = ch("Listen", {"Wet", "Modulator", "Carrier"}, 0);
  for (int i = 0; i < kVocContour; ++i) {
    static const char* names[kVocContour] = {"Contour 1", "Contour 2", "Contour 3", "Contour 4",
                                             "Contour 5", "Contour 6", "Contour 7", "Contour 8"};
    p.params[static_cast<size_t>(VcContour0 + i)] = lin(names[i], "dB", -24, 12, 0);
  }
  p.params[VcModSource] = ch("Modulator", {"Input pair", "Sidechain"}, 0);
  p.params[VcWetLevel].max = 18;  // reference wet wheel reaches +18 dB
  // J-1 reference model.
  p.params[VxModel] = ch("Vocoder model", {"Legacy", "Reference"}, 1);
  p.params[VxBands] = lin("Band count", "", 5, kVxMaxBands, 47);
  p.params[VxOrder] = ch("Filter order", {"1", "2", "3", "4"}, 1);
  p.params[VxFlatness] = lin("Filter flatness", "%", 0, 1, 1);
  p.params[VxWidth] = lg("Band width", "times", 0.008, 4, 0.7529);
  p.params[VxModWidth] = lg("Modulator width", "times", 0.25, 4, 1);
  p.params[VxHoldMs] = lin("Hold time", "ms", 0, 250, vxHoldMs(1666));
  p.params[VxAttackMs] = lin("Attack time", "ms", 0, 1000, vxAttackMs(1115));
  p.params[VxAttCurve] = lin("Attack curve", "times", 1, 4, 1);
  p.params[VxReleaseMs] = lin("Release time", "ms", 0, 2000, vxReleaseMs(3500));
  p.params[VxRelCurve] = lin("Release curve", "times", 1, 4, 1);
  p.params[VxHoldMode] = ch("Hold mode", {"Peak", "RMS"}, 0);
  p.params[VxMinTimes] = tog("Minimum times", 1);
  p.params[VxModHp] = lg("Modulator high-pass", "Hz", kVxOffLow, 15000, vxFilterHz(55000));
  p.params[VxCarLp] = lg("Carrier low-pass", "Hz", 40, kVxOffHigh, vxFilterHz(28000));
  p.params[VxNoiseHp] = lg("Noise high-pass", "Hz", kVxOffLow, 15000, vxFilterHz(55000));
  p.params[VxNoise] = lin("Carrier noise", "%", 0, 1, 0);
  p.params[VxEnhance] = lin("Enhance", "%", 0, 1, 0.15);
  p.params[VxUnison] = lin("Unison", "", 1, kVxMaxUnison, 1);
  p.params[VxUnisonShift] = lin("Unison shift", "%", 0, 1, 0.25);
  p.params[VxUnisonPan] = lin("Unison pan", "%", -1, 1, 0.5);
  p.params[VxContourMod] = tog("Contour to modulator", 1);
  p.params[VxContourNoise] = tog("Contour to noise", 1);
  p.params[VxContourCar] = tog("Contour from carrier", 0);
  p.params[VxContourCons] = tog("Contour from consonants", 1);
  p.params[VxDenoise] = tog("Subtract noise floor", 1);
  p.params[VxCarSource] = ch("Carrier source", {"Built-in", "Input", "Sidechain"}, 0);
  p.params[VxLrEncoding] = tog("L/R encoding", 0);
  p.params[VxTone] = ch("Carrier tone", {"Saw", "Square", "Thin pulse", "Triangle", "Stack saw", "Strings", "Organ", "Buzz", "Noise"}, 0);
  p.params[VxCarVoices] = lin("Carrier voices", "", 1, 4, 1);
  p.params[VxCarDetune] = lin("Carrier detune", "ct", 0, 50, 12);
  p.params[VxMidiPort] = lin("Carrier MIDI port", "", -1, 255, -1);
  p.params[VxEditTarget] = ch("Editor curve", {"Band gain", "Band pan", "Gain offset", "Noise floor", "Mod pass-through", "Hold",
                                               "Attack", "Release", "Band spread", "Band width", "Mod pitch", "Saturation mix",
                                               "Saturation curve"},
                              0);
  p.legacyModelParam = VxModel;
  p.presets = {
      {"Talk box", {}},
      {"Robot choir", V{{VxTone, 4.f}, {VcChord, 3.f}, {VxCarVoices, 3.f}, {VxBands, 32.f}, {VxUnison, 2.f}}},
      {"Whisper", V{{VxTone, 8.f}, {VxBands, 64.f}, {VxReleaseMs, 80.f}, {VxNoise, 0.5f}}},
      {"Bright formant", V{{VcFormant, 4.f}, {VxEnhance, 0.4f}, {VxBands, 64.f}}},
      {"Retro 16 band", V{{VxBands, 16.f}, {VxOrder, 0.f}, {VxTone, 1.f}, {VcRoot, 45.f}, {VxAttackMs, 8.f}, {VxReleaseMs, 90.f}}},
      {"Wide pad", V{{VxTone, 5.f}, {VcChord, 7.f}, {VxUnison, 3.f}, {VxUnisonPan, 1.f}, {VxReleaseMs, 250.f}, {VxBands, 80.f}}},
  };
  p.editorWidth = 780;
  return p;
}

PluginInfo pitch() {
  PluginInfo p{"gl.pitch", "Pitch Shifter", "Spectral", PluginKind::Effect, {}, false, {}};
  p.params.resize(PsCount);
  p.params[PsMode] = ch("Mode", {"Fast", "Spectral"}, 1);
  p.params[PsPitch] = lin("Pitch", "st", -24, 24, 0);
  p.params[PsFine] = lin("Fine", "ct", -100, 100, 0);
  p.params[PsFormant] = lin("Formant", "st", -12, 12, 0);
  p.params[PsPreserve] = tog("Keep formants", 0);
  p.params[PsGrain] = lg("Window", "ms", 10, 120, 45);
  p.params[PsDetune] = lin("Stereo detune", "ct", 0, 50, 0);
  p.params[PsDelay] = lin("Delay", "ms", 0, 1000, 0);
  p.params[PsFeedback] = lin("Feedback", "%", 0, 0.95, 0);
  p.params[PsOutput] = lin("Output", "dB", -24, 12, 0);
  p.params[PsMix] = lin("Mix", "%", 0, 1, 1);
  // J-1 reference engines (pre-J-1 states keep the classic engines).
  p.params[PsEngine] = ch("Engine", {"Classic", "Voice", "Music"}, kPsMusic);
  p.params[PsCorrection] = lin("Formant correction", "%", 0, 1, 1);
  p.params[PsDuration] = lin("Grain length", "ms", 20, 150, 85);
  p.params[PsDensity] = lin("Density", "x", 1, 5, 3);
  p.params[PsJitter] = lin("Jitter", "%", 0, 1, 0.3);
  p.params[PsRand] = lin("Randomize", "%", 0, 1, 0.2);
  p.params[PsDelayMode] = ch("Delay mode", {"Off", "Post shift", "Pre shift", "No shift"}, 0);
  p.params[PsDelaySync] = tog("Delay tempo sync", 0);
  p.params[PsDelaySteps] = lin("Delay steps", "step", 0, 16, 4);
  p.params[PsAnalysis] = ch("Analysis", {"Left", "Right", "Left + Right"}, 0);
  p.params[PsPitchMode] = ch("Pitch mode", {"Relative", "Fixed C4", "Fixed C5", "Fixed C6"}, 0);
  p.params[PsDetector] = ch("Detector", {"Fast", "Accurate"}, 1);
  p.legacyModelParam = PsEngine;
  p.presets = {
      {"Unity", {}},
      {"Octave up", V{{PsPitch, 12.f}}},
      {"Deep voice", V{{PsEngine, 1.f}, {PsPitch, -5.f}, {PsCorrection, 1.f}}},
      {"Doubler", V{{PsEngine, 0.f}, {PsMode, 0.f}, {PsDetune, 12.f}, {PsGrain, 30.f}}},
      {"Rising spiral", V{{PsPitch, 7.f}, {PsDelayMode, 1.f}, {PsDelay, 250.f}, {PsFeedback, 0.6f}, {PsOutput, -3.f}}},
      {"Chipmunk", V{{PsEngine, 1.f}, {PsPitch, 7.f}, {PsFormant, 5.f}, {PsCorrection, 1.f}}},
  };
  p.editorWidth = 760;
  return p;
}

PluginInfo pitcher() {
  PluginInfo p{"gl.pitcher", "Pitch Corrector", "Spectral", PluginKind::Effect, {}, false, {}};
  p.midiInput = true;  // MIDI target notes / harmony voices from a MIDI port
  p.params.resize(PcCount);
  p.params[PcKey] = ch("Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0);
  p.params[PcScale] = ch("Scale", {"Chromatic", "Major", "Minor", "Harm. minor", "Dorian", "Mixolydian", "Pent. major",
                                    "Pent. minor", "Blues", "Custom"},
                         1);
  p.params[PcSpeed] = lg("Speed", "ms", 1, 500, 40);
  p.params[PcAmount] = lin("Amount", "%", 0, 1, 1);
  p.params[PcLowest] = ch("Range", {"55 Hz", "80 Hz", "110 Hz", "170 Hz"}, 1);
  p.params[PcReference] = lin("Reference A4", "Hz", 415, 466, 440);
  p.params[PcFormant] = lin("Gender", "st", -12, 12, 0);
  p.params[PcPreserve] = tog("Keep formants", 1);
  const char* vn[3] = {"Voice 1", "Voice 2", "Voice 3"};
  const char* vl[3] = {"Voice 1 level", "Voice 2 level", "Voice 3 level"};
  const char* vp[3] = {"Voice 1 pan", "Voice 2 pan", "Voice 3 pan"};
  const float vd[3] = {4, 7, 12};
  for (int i = 0; i < 3; ++i) {
    p.params[static_cast<size_t>(PcVoice1 + i * 3)] = lin(vn[i], "st", -24, 24, vd[i]);
    p.params[static_cast<size_t>(PcVoice1Level + i * 3)] = lin(vl[i], "%", 0, 1, 0);
    p.params[static_cast<size_t>(PcVoice1Pan + i * 3)] = lin(vp[i], "pan", -1, 1, i == 0 ? -0.5 : (i == 1 ? 0.5 : 0));
  }
  p.params[PcDiatonic] = tog("Snap voices to scale", 1);
  p.params[PcLeadLevel] = lin("Lead level", "%", 0, 1, 1);
  p.params[PcLeadPan] = lin("Lead pan", "pan", -1, 1, 0);
  static const char* notes[12] = {"Note C", "Note C#", "Note D", "Note D#", "Note E", "Note F",
                                  "Note F#", "Note G", "Note G#", "Note A", "Note A#", "Note B"};
  for (int i = 0; i < 12; ++i) p.params[static_cast<size_t>(PcNote0 + i)] = tog(notes[i], 1);
  // J-1 reference model (pre-J-1 states keep the legacy engine).
  p.params[PcModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  p.params[PcCorrSpeed] = lin("Correction speed", "%", 0, 1, 0.5);
  p.params[PcFine] = lin("Fine tune", "ct", -100, 100, 0);
  p.params[PcStereo] = lin("Stereo spread", "%", 0, 1, 1);
  p.params[PcRefChannel] = ch("Pitch reference", {"Left", "Both", "Right"}, 1);
  p.params[PcBypassAll] = tog("Bypass all keys", 0);
  static const char* kb[12] = {"Bypass C", "Bypass C#", "Bypass D", "Bypass D#", "Bypass E", "Bypass F",
                               "Bypass F#", "Bypass G", "Bypass G#", "Bypass A", "Bypass A#", "Bypass B"};
  for (int i = 0; i < 12; ++i) p.params[static_cast<size_t>(PcKeyBypass0 + i)] = tog(kb[i], 0);
  p.params[PcRange6] = ch("Lowest pitch", {"220 Hz", "170 Hz", "110 Hz", "80 Hz", "55 Hz", "25 Hz"}, 2);
  p.params[PcMidiTargets] = tog("MIDI targets", 0);
  p.params[PcMidiOctaves] = tog("MIDI octaves", 0);
  p.params[PcHarmonize] = tog("Harmonize", 0);
  static const char* hp[4] = {"Harmony 1 pan", "Harmony 2 pan", "Harmony 3 pan", "Harmony 4 pan"};
  for (int i = 0; i < 4; ++i) p.params[static_cast<size_t>(PcHarmPan1 + i)] = lin(hp[i], "pan", -1, 1, 0);
  p.params[PcHarmMix] = tog("Harmony mix", 0);
  p.params[PcHarmVel] = tog("Harmony velocity", 0);
  p.params[PcMidiPort] = lin("MIDI port", "", -1, 255, -1);
  p.legacyModelParam = PcModel;
  p.presets = {
      {"Natural", {}},
      {"Hard tune", V{{PcSpeed, 1.f}, {PcCorrSpeed, 1.f}, {PcScale, 2.f}, {PcKey, 9.f}}},
      {"Gentle", V{{PcSpeed, 150.f}, {PcCorrSpeed, 0.3f}, {PcAmount, 0.7f}}},
      {"Thirds & fifths", V{{PcVoice1Level, 0.7f}, {PcVoice2Level, 0.6f}, {PcSpeed, 20.f}, {PcCorrSpeed, 0.7f}}},
      {"MIDI choir", V{{PcHarmonize, 1.f}, {PcHarmMix, 1.f}, {PcHarmPan1, -0.6f}, {PcHarmPan1 + 1, 0.6f},
                       {PcHarmPan1 + 2, -0.3f}, {PcHarmPan1 + 3, 0.3f}, {PcCorrSpeed, 0.9f}}},
      {"Chipmunk choir", V{{PcFormant, 6.f}, {PcVoice1Level, 0.5f}, {PcVoice2Level, 0.5f}, {PcVoice3Level, 0.4f}}},
  };
  p.editorWidth = 680;
  return p;
}

PluginInfo scratcher() {
  PluginInfo p{"gl.scratcher", "Scratcher", "Spectral", PluginKind::Effect, {}, false, {}};
  p.params.resize(ScCount);
  p.params[ScHand] = lin("Platter", "%", 0, 1, 0);
  p.params[ScGrab] = tog("Hold platter", 0);
  p.params[ScMotor] = tog("Motor", 1);
  p.params[ScStart] = lg("Start time", "s", 0.01, 3, 0.25);
  p.params[ScStop] = lg("Stop time", "s", 0.01, 5, 0.9);
  p.params[ScRange] = lg("Range", "s", 0.1, 4, 0.6);
  p.params[ScSmooth] = lg("Smoothing", "ms", 1, 200, 18);
  p.params[ScPattern] = ch("Pattern", {"Off", "Baby", "Forward", "Chirp", "Transform", "Tear"}, 0);
  p.params[ScRate] = ch("Rate", {"1/2", "1/4", "1/8", "1/16"}, 1);
  p.params[ScFader] = lin("Crossfader", "%", 0, 1, 1);
  p.params[ScCurve] = ch("Fader curve", {"Smooth", "Sharp", "Cut"}, 0);
  p.params[ScVolume] = lin("Volume", "dB", -24, 12, 0);
  p.params[ScPan] = lin("Pan", "pan", -1, 1, 0);
  p.presets = {
      {"Deck", {}},
      {"Baby scratch", V{{ScPattern, 1.f}, {ScRate, 2.f}}},
      {"Chirps", V{{ScPattern, 3.f}, {ScRate, 2.f}, {ScCurve, 1.f}}},
      {"Transformer", V{{ScPattern, 4.f}, {ScRate, 3.f}, {ScCurve, 2.f}}},
      {"Power down", V{{ScMotor, 0.f}, {ScStop, 1.5f}}},
  };
  p.editorWidth = 680;
  return p;
}

PluginInfo splitter() {
  PluginInfo p{"gl.splitter", "Band Splitter", "Spectral", PluginKind::Effect, {}, false, {}};
  p.params.resize(SpCount);
  p.params[SpBands] = ch("Bands", {"2", "3", "4"}, 1);
  p.params[SpSlope] = ch("Slope", {"12 dB", "24 dB", "48 dB"}, 1);
  p.params[SpFreq1] = lg("Split 1", "Hz", 20, 20000, 200);
  p.params[SpFreq2] = lg("Split 2", "Hz", 20, 20000, 2000);
  p.params[SpFreq3] = lg("Split 3", "Hz", 20, 20000, 8000);
  static const char* names[4][5] = {
      {"Band 1 gain", "Band 1 pan", "Band 1 width", "Band 1 mute", "Band 1 solo"},
      {"Band 2 gain", "Band 2 pan", "Band 2 width", "Band 2 mute", "Band 2 solo"},
      {"Band 3 gain", "Band 3 pan", "Band 3 width", "Band 3 mute", "Band 3 solo"},
      {"Band 4 gain", "Band 4 pan", "Band 4 width", "Band 4 mute", "Band 4 solo"}};
  for (int b = 0; b < 4; ++b) {
    p.params[static_cast<size_t>(splitParam(b, SbGain))] = lin(names[b][0], "dB", -60, 12, 0);
    p.params[static_cast<size_t>(splitParam(b, SbPan))] = lin(names[b][1], "pan", -1, 1, 0);
    p.params[static_cast<size_t>(splitParam(b, SbWidth))] = lin(names[b][2], "%", 0, 2, 1);
    p.params[static_cast<size_t>(splitParam(b, SbMute))] = tog(names[b][3], 0);
    p.params[static_cast<size_t>(splitParam(b, SbSolo))] = tog(names[b][4], 0);
  }
  p.params[SpFreq2].def = 3000;
  // J-1 reference model.
  static const std::vector<std::string> slopes = {"6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB"};
  p.params[SpModel] = ch("Splitter model", {"Legacy", "Reference"}, 1);
  p.params[SpSlopeA] = ch("Slope 1", slopes, 3);
  p.params[SpSlopeB] = ch("Slope 2", slopes, 3);
  p.params[SpSlopeC] = ch("Slope 3", slopes, 3);
  p.params[SpLinear] = tog("Linear phase", 0);
  p.params[SpQuality] = ch("Linear phase quality", {"Lowest", "Low", "Medium", "High", "Higher", "Highest"}, 2);
  p.params[SpLinSpeed] = lg("Linear phase speed", "ms", 2, 200, 50);
  p.params[SpMonitor] = ch("Graph shows", {"Main", "Sends"}, 0);
  p.params[SpLinked] = tog("Linked crossovers", 0);
  p.params[SpViewMain] = ch("Main view", {"Bands", "Sum"}, 0);
  p.params[SpViewSend] = ch("Send view", {"Bands", "Sum"}, 0);
  static const char* n2[4][4] = {
      {"Band 1 level", "Band 1 send level", "Band 1 send mute", "Band 1 send to"},
      {"Band 2 level", "Band 2 send level", "Band 2 send mute", "Band 2 send to"},
      {"Band 3 level", "Band 3 send level", "Band 3 send mute", "Band 3 send to"},
      {"Band 4 level", "Band 4 send level", "Band 4 send mute", "Band 4 send to"}};
  for (int b = 0; b < 4; ++b) {
    p.params[static_cast<size_t>(splitParam2(b, SxMain))] = lin(n2[b][0], "dB", kSplitSilentDb, 18, 0);
    p.params[static_cast<size_t>(splitParam2(b, SxSend))] = lin(n2[b][1], "dB", kSplitSilentDb, 18, 0);
    p.params[static_cast<size_t>(splitParam2(b, SxSendMute))] = tog(n2[b][2], 0);
    p.params[static_cast<size_t>(splitParam2(b, SxSendTo))] = lin(n2[b][3], "", -1, 500, -1);
    p.sendParams.push_back(splitParam2(b, SxSendTo));
  }
  p.legacyModelParam = SpModel;
  p.presets = {
      {"Three way", {}},
      {"Mono lows, wide highs", V{{splitParam(0, SbWidth), 0.f}, {splitParam(2, SbWidth), 1.5f}}},
      {"Scoop", V{{splitParam2(1, SxMain), -6.f}}},
      {"Four band", V{{SpBands, 2.f}, {SpFreq1, 120.f}, {SpFreq2, 800.f}, {SpFreq3, 5000.f}}},
      {"Steep split", V{{SpSlopeA, 7.f}, {SpSlopeB, 7.f}}},
      {"Linear phase", V{{SpLinear, 1.f}, {SpSlopeA, 5.f}, {SpSlopeB, 5.f}}},
  };
  p.editorWidth = 780;
  return p;
}

PluginInfo spreader() {
  PluginInfo p{"gl.spread", "Spreader", "Spectral", PluginKind::Effect, {}, false, {}};
  p.params.resize(SrCount);
  p.params[SrMode] = ch("Method", {"Complement", "Haas", "Chorus", "Mid/Side"}, 0);
  p.params[SrSpread] = lin("Spread", "%", 0, 1, 0.5);
  p.params[SrSeparation] = lin("Separation", "%", -1, 1, 0);
  p.params[SrBassMono] = tog("Mono bass", 1);
  p.params[SrBassFreq] = lg("Bass freq", "Hz", 20, 500, 120);
  p.params[SrOffset] = lin("L/R offset", "ms", -20, 20, 0);
  p.params[SrInvert] = ch("Polarity", {"Normal", "Invert L", "Invert R", "Invert both"}, 0);
  p.params[SrPosition] = ch("Separation", {"Before", "After"}, 1);
  p.params[SrPan] = lin("Pan", "pan", -1, 1, 0);
  p.params[SrVolume] = lin("Volume", "dB", -24, 12, 0);
  p.params[SrMonoCheck] = tog("Mono check", 0);
  p.params[SrDcBlock] = tog("Remove DC", 0);
  // J-1 reference model (pre-J-1 states keep the legacy engine).
  p.params[SrModel] = ch("Engine", {"Legacy", "Reference"}, 1);
  p.params[SrLowBypass] = tog("Low bypass", 0);
  p.params[SrLowFreq] = lg("Low bypass freq", "Hz", 10, 10000, 120);
  p.params[SrEnabled] = tog("Enabled", 1);
  p.params[SrPreMonitor] = tog("Show input", 1);
  p.params[SrPostMonitor] = tog("Show output", 1);
  p.legacyModelParam = SrModel;
  p.presets = {
      {"Gentle width", {}},
      {"Super wide", V{{SrSpread, 1.f}, {SrSeparation, 0.4f}, {SrLowBypass, 1.f}, {SrLowFreq, 180.f}}},
      {"Haas", V{{SrMode, 1.f}, {SrSpread, 0.4f}}},
      {"Chorus spread", V{{SrMode, 2.f}, {SrSpread, 0.7f}}},
      {"Narrow", V{{SrSpread, 0.f}, {SrSeparation, -0.6f}}},
      {"Mono", V{{SrSpread, 0.f}, {SrSeparation, -1.f}}},
  };
  p.editorWidth = 720;
  return p;
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  v.push_back(vocoder());
  v.push_back(pitch());
  v.push_back(pitcher());
  v.push_back(scratcher());
  v.push_back(splitter());
  v.push_back(spreader());
}

}  // namespace gl::packs::spectral
