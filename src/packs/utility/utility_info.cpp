// Gridline — "utility" pack: catalogue entries.
#include "../Packs.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include "utility.h"

namespace gl::packs::utility {

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

// Curve EQ snapshot from a function of frequency (dB, clamped to the point range).
void curve(Values& v, int snap, const std::function<double(double)>& f) {
  for (int b = 0; b < kCeBands; ++b) {
    double db = std::clamp(f(ceBandHz(b)), -kCeRange, kCeRange);
    if (std::fabs(db) > 1e-4) v.push_back({cePoint(snap, b), static_cast<float>(db)});
  }
}
double oct(double hz, double centre) { return std::log2(hz / centre); }
double bell(double hz, double centre, double widthOct, double db) {
  double x = oct(hz, centre) / widthOct;
  return db * std::exp(-x * x);
}
double shelfLo(double hz, double corner, double db) { return db / (1.0 + std::pow(hz / corner, 2.0)); }
double shelfHi(double hz, double corner, double db) { return db / (1.0 + std::pow(corner / hz, 2.0)); }

void curveEq(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.drawneq", "Curve EQ", "Filter", PluginKind::Effect, {}, false, {}};
  p.params.resize(CeCount);
  const char* snaps = "ABCD";
  for (int k = 0; k < kCeSnaps; ++k)
    for (int b = 0; b < kCeBands; ++b) {
      double hz = ceBandHz(b);
      char name[48];
      if (hz >= 1000)
        std::snprintf(name, sizeof name, "%c %.1f kHz", snaps[k], hz / 1000);
      else
        std::snprintf(name, sizeof name, "%c %.0f Hz", snaps[k], hz);
      p.params[static_cast<size_t>(cePoint(k, b))] = lin(name, "dB", -kCeRange, kCeRange, 0);
    }
  p.params[CeMorphX] = lin("Morph X", "%", 0, 1, 0);
  p.params[CeMorphY] = lin("Morph Y", "%", 0, 1, 0);
  p.params[CeDepth] = lin("Depth", "%", -1, 1, 1);
  p.params[CeShift] = lin("Shift", "oct", -2, 2, 0);
  p.params[CeSmooth] = lg("Smoothing", "s", 0.001, 2, 0.08);
  p.params[CeOutput] = lin("Output", "dB", -24, 12, 0);
  p.params[CePhase] = ch("Phase", {"Minimum", "Linear"}, 0);

  Values smile, morphTone, air, sweep;
  curve(smile, 0, [](double f) { return shelfLo(f, 90, 5) + shelfHi(f, 7000, 4) + bell(f, 600, 1.2, -2); });
  // Morph pad: dark (A) / bright (B) / thin (C) / scooped (D).
  for (int s = 0; s < 4; ++s)
    curve(morphTone, s, [s](double f) {
      switch (s) {
        case 0: return shelfHi(f, 3000, -9) + shelfLo(f, 120, 2);
        case 1: return shelfHi(f, 5000, 6) + bell(f, 250, 1, -2);
        case 2: return shelfLo(f, 400, -14) + bell(f, 2500, 1.5, 3);
        default: return bell(f, 800, 1.6, -8) + shelfLo(f, 80, 3) + shelfHi(f, 9000, 3);
      }
    });
  morphTone.push_back({CeMorphX, 0.5f});
  morphTone.push_back({CeMorphY, 0.5f});
  curve(air, 0, [](double f) { return shelfHi(f, 10000, 5) + bell(f, 3500, 0.8, -1.5) + shelfLo(f, 30, -12); });
  air.push_back({CePhase, 1.f});
  // Band-limited "radio" at A, flat at B: sweep X to open up.
  curve(sweep, 0, [](double f) { return shelfLo(f, 500, -18) + shelfHi(f, 2500, -18) + bell(f, 1200, 0.7, 4); });
  p.presets = {{"Flat", {}}, {"Smile", smile}, {"Tone pad", morphTone}, {"Air (linear)", air}, {"Radio to full", sweep}};
  p.editorWidth = 860;
  v.push_back(p);
}

// Morph EQ: one bank's level curve from a function of frequency (gain, linear).
void bankLevel(Values& v, int bank, const std::function<double(double)>& gain) {
  for (int b = 0; b < kBqBands; ++b) {
    double x = std::clamp(bqValueOfGain(gain(bqBandHz(b))), 0.0, 2.0);
    if (std::fabs(x - 1.0) > 1e-4) v.push_back({bqValue(bank, BtLevel, b), static_cast<float>(x)});
  }
}
double dbg(double db) { return std::pow(10.0, db / 20.0); }

void morphEq(std::vector<PluginInfo>& v) {
  PluginInfo p{"gl.bankeq", "Morph EQ", "Filter", PluginKind::Effect, {}, false, {}};
  p.params.resize(kBqCount);
  p.params[BqShift] = lin("Shift", "oct", -10, 10, 0);
  p.params[BqMix] = lin("Mix", "%", -1, 1, 1);
  p.params[BqMorph] = lin("Morph", "", 0, 7, 0);
  p.params[BqSmooth] = lin("Smoothing", "%", 0, 1, 170.0 / 1024.0);
  p.params[BqOutput] = lin("Output", "dB", -60, 21.5836, 0);
  p.params[BqBandwidth] = lin("Bandwidth", "oct", 80.0 / 300.0, 500.0 / 300.0, 0.5);
  p.params[BqOversample] = tog("Oversample", 0);
  p.params[BqAddMode] = tog("Add mode", 0);
  p.params[BqSendOnly] = tog("Send only", 0);
  p.params[BqSendTo] = lin("Send target", "", -1, 125, -1);
  p.params[BqEditTarget] = ch("Edit curve", {"Level", "Pan", "Send"}, 0);
  p.params[BqEditBank] = ch("Edit bank", {"1", "2", "3", "4", "5", "6", "7", "8"}, 0);
  p.params[BqEditMaster] = tog("Edit master", 0);
  p.params[BqDrawMode] = ch("Draw tool", {"Free", "Line", "Curve"}, 2);
  static const char* tn[kBqTargets] = {"level", "pan", "send"};
  for (int k = 0; k <= kBqBanks; ++k)
    for (int t = 0; t < kBqTargets; ++t)
      for (int b = 0; b < kBqBands; ++b) {
        double hz = bqBandHz(b);
        char name[64], f[24];
        if (hz >= 1000) std::snprintf(f, sizeof f, "%.1f kHz", hz / 1000);
        else std::snprintf(f, sizeof f, "%.0f Hz", hz);
        if (k == kBqMaster) std::snprintf(name, sizeof name, "Master %s %s", tn[t], f);
        else std::snprintf(name, sizeof name, "Bank %d %s %s", k + 1, tn[t], f);
        p.params[static_cast<size_t>(bqValue(k, t, b))] = lin(name, "", 0, 2, 1);
      }
  for (int t = 0; t < kBqTargets; ++t)
    for (int b = 0; b < kBqBands; ++b) {
      char name[64];
      double hz = bqBandHz(b);
      if (hz >= 1000) std::snprintf(name, sizeof name, "Mode %s %.1f kHz", tn[t], hz / 1000);
      else std::snprintf(name, sizeof name, "Mode %s %.0f Hz", tn[t], hz);
      p.params[static_cast<size_t>(bqMode(t, b))] = ch(name, {"On", "Off", "Locked"}, 0);
    }
  p.sendParams = {BqSendTo};

  Values smile, sweep, wide, phone, vowels;
  bankLevel(smile, 0, [](double f) { return dbg(shelfLo(f, 90, 5) + shelfHi(f, 7000, 4) + bell(f, 700, 1.2, -2.5)); });
  // Dark (bank 1) → bright (bank 8): turn Morph to open up.
  for (int k = 0; k < kBqBanks; ++k) {
    double t = k / 7.0;
    bankLevel(sweep, k, [t](double f) { return dbg(shelfHi(f, 600 * std::pow(20.0, t), -14 * (1 - t) + 3 * t) + shelfLo(f, 120, 3 * (1 - t))); });
  }
  // Alternating pan per third-octave above 200 Hz.
  for (int b = 0; b < kBqBands; ++b)
    if (bqBandHz(b) > 200) wide.push_back({bqValue(0, BtPan, b), (b & 1) ? 1.55f : 0.45f});
  bankLevel(phone, 0, [](double f) { return dbg(shelfLo(f, 400, -30) + shelfHi(f, 3200, -30) + bell(f, 1500, 0.8, 5)); });
  phone.push_back({BqBandwidth, 1.0f});
  // Formant banks a / e / i / o / u spread over banks 1..5 (morph 0..4).
  static const double fm[5][2] = {{800, 1150}, {400, 2000}, {300, 2600}, {450, 800}, {330, 700}};
  for (int k = 0; k < 5; ++k)
    bankLevel(vowels, k, [k](double f) { return dbg(-14 + bell(f, fm[k][0], 0.35, 22) + bell(f, fm[k][1], 0.3, 18)); });
  vowels.push_back({BqMorph, 1.0f});
  vowels.push_back({BqSmooth, 0.4f});
  p.presets = {{"Flat", {}}, {"Smile", smile}, {"Dark to bright", sweep}, {"Wide bands", wide}, {"Telephone", phone}, {"Vowel morph", vowels}};
  p.editorWidth = 860;
  v.push_back(p);
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  curveEq(v);
  morphEq(v);
  {
    PluginInfo p{"gl.freefilter", "Free Filter", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(FfCount);
    p.params[FfType] = ch("Type", {"Low pass", "Band pass", "High pass", "Notch", "Low shelf", "Peak", "High shelf"}, 0);
    p.params[FfFreq] = lg("Frequency", "Hz", 10, 22000, 1000);
    p.params[FfQ] = lg("Q", "", 0.1, 20, 0.707);
    p.params[FfGain] = lin("Gain", "dB", -24, 24, 0);
    p.params[FfDc] = tog("Remove DC", 1);
    p.presets = {{"Soft lowpass", {}},
                 {"Phone band", {{FfType, 1.f}, {FfFreq, 1400.f}, {FfQ, 1.1f}}},
                 {"Rumble cut", {{FfType, 2.f}, {FfFreq, 45.f}, {FfQ, 0.8f}}},
                 {"Hum notch", {{FfType, 3.f}, {FfFreq, 50.f}, {FfQ, 8.f}}},
                 {"Warm lows", {{FfType, 4.f}, {FfFreq, 140.f}, {FfGain, 5.f}}},
                 {"Air lift", {{FfType, 6.f}, {FfFreq, 9000.f}, {FfGain, 4.f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.fastlp", "Quick Lowpass", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(QlCount);
    p.params[QlCutoff] = lg("Cutoff", "Hz", 20, 20000, 20000);
    p.params[QlReso] = lin("Resonance", "%", 0, 1, 0);
    p.presets = {{"Open", {}}, {"Muffled", {{QlCutoff, 900.f}, {QlReso, 0.1f}}}, {"Squelch", {{QlCutoff, 600.f}, {QlReso, 0.8f}}},
                 {"Underwater", {{QlCutoff, 300.f}, {QlReso, 0.35f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.bassboost", "Bass Boost", "Filter", PluginKind::Effect, {}, false, {}};
    p.params.resize(BbCount);
    p.params[BbFreq] = lg("Frequency", "Hz", 20, 500, 80);
    p.params[BbAmount] = lin("Amount", "%", 0, 1, 0.4);
    p.presets = {{"Gentle", {}}, {"Sub weight", {{BbFreq, 50.f}, {BbAmount, 0.5f}}}, {"Punch", {{BbFreq, 110.f}, {BbAmount, 0.35f}}},
                 {"Boom", {{BbFreq, 70.f}, {BbAmount, 0.8f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.scope", "Scope", "Visual", PluginKind::Effect, {}, false, {}};
    p.params.resize(ScCount);
    p.params[ScMode] = ch("View", {"Wave", "Spectrum", "Sonogram", "Vector", "Meters", "Grid"}, SvGrid);
    p.params[ScGain] = lin("Display gain", "dB", -24, 24, 0);
    p.params[ScWindow] = lg("Wave window", "s", 0.005, 0.5, 0.04);
    p.params[ScRange] = lin("Spectrum range", "", 48, 144, 96);
    p.params[ScTilt] = lin("Spectrum slope", "dB/oct", 0, 6, 3);
    p.params[ScRes] = ch("Resolution", {"1024", "2048", "4096", "8192"}, 2);
    p.params[ScMeterMode] = ch("Meter", {"Peak", "RMS", "VU", "Loudness"}, 3);
    p.params[ScHold] = lin("Peak hold", "s", 0, 5, 1.5);
    p.params[ScDecay] = lin("Meter fall", "dB/s", 3, 60, 18);
    p.params[ScVecMode] = ch("Vector style", {"Lines", "Dots"}, 0);
    p.params[ScFreeze] = tog("Freeze", 0);
    p.params[ScAutoView] = tog("Auto view", 0);
    p.params[ScWaveRefresh] = lin("Wave refresh", "ms", 5, 50, 10);
    p.params[ScSplit] = tog("Stereo split", 0);
    p.params[ScSplitGap] = lin("Split gap", "%", 0, 100, 0);
    p.params[ScTaper] = tog("Edge taper", 1);
    p.params[ScSmoothZoom] = tog("Smooth zoom", 1);
    p.params[ScSpecRefresh] = lin("Spectrum refresh", "ms", 5, 100, 15);
    p.params[ScSpecBands] = ch("Spectrum bands", {"32", "64", "128", "256", "512", "1024", "2048", "4096", "8192"}, 5);
    p.params[ScFreqCurve] = lin("Frequency curve", "%", 0, 1, 0);
    p.params[ScPink] = tog("Pink weighting", 1);
    p.params[ScSharpen] = tog("Sharpen peaks", 1);
    p.params[ScMeterType] = ch("Meter type", {"Peak", "PPM", "RMS", "VU", "A-weighted Leq", "BS.1770 loudness"}, 0);
    p.params[ScMeterCurve] = lin("Meter curve", "%", 0, 1, 0.5);
    p.params[ScHoldInf] = tog("Infinite hold", 0);
    p.params[ScFallTime] = lg("Fall time", "ms", 1, 5000, 750);
    p.params[ScVecStyle] = ch("Vector style", {"Thin", "Ribbon", "Tube", "Glow"}, SvsTube);
    p.params[ScVecWidth] = lin("Vector width", "px", 1, 6, 3.5);
    p.params[ScVecAdd] = tog("Additive", 1);
    p.params[ScVecRefresh] = lin("Vector refresh", "ms", 5, 50, 10);
    p.params[ScFade] = tog("Fade on silence", 0);
    p.params[ScFadeDelay] = lin("Fade delay", "ms", 0, 5000, 5000);
    // Old projects (11 parameters) are normalised to the legacy display (see normalizePluginState).
    p.params[ScEngine] = ch("Display engine", {"Legacy", "Reference"}, 1);
    p.presets = {{"Overview", {}}, {"Waveform", {{ScMode, SvOsc}, {ScWindow, 0.02f}}}, {"Spectrum", {{ScMode, SvSpectrum}, {ScFreqCurve, 1.f}}},
                 {"Sonogram", {{ScMode, SvSonogram}}}, {"Stereo field", {{ScMode, SvVector}}}, {"Loudness", {{ScMode, SvMeters}, {ScMeterType, SmtLoudness}}}};
    p.editorWidth = 860;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.spectro", "Spectrogram", "Visual", PluginKind::Effect, {}, false, {}};
    p.params.resize(SgCount);
    p.params[SgMode] = ch("Mode", {"Bars", "Lines", "Sonogram"}, 0);
    p.params[SgAmp] = lin("Amp", "dB", -24, 24, 0);
    p.params[SgScale] = lin("Log scale", "%", 0, 1, 0.8);
    p.params[SgPeaks] = tog("Peaks", 1);
    p.params[SgWindow] = tog("Window", 1);
    p.params[SgStereo] = tog("Stereo", 0);
    p.params[SgSpeed] = ch("Speed", {"Slow", "Normal", "Fast"}, 1);
    p.presets = {{"Bars", {}}, {"Lines", {{SgMode, 1.f}}}, {"Sonogram", {{SgMode, 2.f}}}, {"Stereo bars", {{SgStereo, 1.f}}}};
    p.editorWidth = 680;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.meter", "Level Meter", "Visual", PluginKind::Effect, {}, false, {}};
    p.params.resize(LmCount);
    p.params[LmMode] = ch("Mode", {"Peak", "RMS", "Peak + RMS"}, 2);
    p.params[LmRange] = ch("Scale", {"36 dB", "60 dB", "96 dB"}, 0);
    p.params[LmHold] = lin("Hold", "s", 0, 5, 1.5);
    p.params[LmDecay] = lin("Fall", "dB/s", 3, 60, 20);
    p.params[LmTruePeak] = tog("True peak", 1);
    p.presets = {{"Default", {}}, {"Wide scale", {{LmRange, 1.f}}}, {"Peak only", {{LmMode, 0.f}, {LmHold, 3.f}}}, {"Slow RMS", {{LmMode, 1.f}, {LmDecay, 8.f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.clock", "Clock", "Visual", PluginKind::Effect, {}, false, {}};
    p.params.resize(CkCount);
    p.params[CkDisplay] = ch("Display", {"Bars + time", "Bars", "Time", "Samples"}, 0);
    p.params[CkSource] = ch("Source", {"Song position", "Since play"}, 0);
    p.presets = {{"Bars and time", {}}, {"Bars only", {{CkDisplay, 1.f}}}, {"Elapsed time", {{CkDisplay, 2.f}, {CkSource, 1.f}}}, {"Samples", {{CkDisplay, 3.f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.notes", "Notes", "Visual", PluginKind::Effect, {}, false, {}};
    p.params.resize(NtCount);
    p.params[NtPage] = lin("Page", "", 1, kNotePages, 1);
    p.presets = {{"First page", {}}, {"Page 2", {{NtPage, 2.f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.mute", "Mute", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(MuCount);
    p.params[MuMute] = tog("Mute", 0);
    p.params[MuSide] = ch("Side", {"Both", "Left", "Right"}, 0);
    p.params[MuFade] = lg("Fade", "s", 0.0005, 0.25, 0.005);
    p.presets = {{"Open", {}}, {"Muted", {{MuMute, 1.f}}}, {"Left only off", {{MuMute, 1.f}, {MuSide, 1.f}}}, {"Slow fade", {{MuFade, 0.2f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
  {
    PluginInfo p{"gl.stereomatrix", "Stereo Matrix", "Misc", PluginKind::Effect, {}, false, {}};
    p.params.resize(SmCount);
    p.params[SmLL] = lin("Left to left", "%", -2, 2, 1);
    p.params[SmRL] = lin("Right to left", "%", -2, 2, 0);
    p.params[SmLR] = lin("Left to right", "%", -2, 2, 0);
    p.params[SmRR] = lin("Right to right", "%", -2, 2, 1);
    p.params[SmDelay] = lin("Side delay", "ms", -20, 20, 0);
    p.params[SmDephase] = lin("Dephase", "%", -1, 1, 0);
    p.params[SmFxPos] = ch("Effects position", {"Before matrix", "After matrix"}, 1);
    p.params[SmDelayK] = lin("Delay", "%", -1, 1, 0);
    p.params[SmDephaseK] = lin("Dephase", "%", -1, 1, 0);
    p.params[SmClassic] = tog("Reference engine", 1);
    p.presets = {{"Pass", {}},
                 {"Swap sides", {{SmLL, 0.f}, {SmRL, 1.f}, {SmLR, 1.f}, {SmRR, 0.f}}},
                 {"Mono", {{SmLL, 0.6309f}, {SmRL, 0.6309f}, {SmLR, 0.6309f}, {SmRR, 0.6309f}}},
                 {"Flip right", {{SmRR, -1.f}}},
                 {"Wider", {{SmLL, 1.2619f}, {SmRL, -0.6309f}, {SmLR, -0.6309f}, {SmRR, 1.2619f}}},
                 {"Offset right", {{SmDelayK, 0.35f}, {SmDephaseK, 0.4f}}}};
    p.editorWidth = 580;
    v.push_back(p);
  }
}

}  // namespace gl::packs::utility
