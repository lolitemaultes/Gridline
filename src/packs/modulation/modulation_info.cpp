// Gridline — "modulation" pack: catalogue entries.
#include "../Packs.h"
#include "modulation.h"

namespace gl::packs::modulation {

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

std::vector<std::string> rateNames() { return std::vector<std::string>(kRateNames, kRateNames + kRateCount); }
const std::vector<std::string> kLfoShapes = {"Sine", "Triangle", "Square", "Saw", "Random"};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

PluginInfo make(const char* type, const char* name, const char* cat, PluginKind kind) {
  PluginInfo p;
  p.type = type;
  p.name = name;
  p.category = cat;
  p.kind = kind;
  return p;
}

}  // namespace

const std::vector<ChordShape>& chordShapes() {
  static const std::vector<ChordShape> v = {
      {"Major", {0, 4, 7}, 3},          {"Minor", {0, 3, 7}, 3},         {"Dominant 7", {0, 4, 7, 10}, 4},
      {"Major 7", {0, 4, 7, 11}, 4},    {"Minor 7", {0, 3, 7, 10}, 4},   {"Sus 2", {0, 2, 7}, 3},
      {"Sus 4", {0, 5, 7}, 3},          {"Diminished", {0, 3, 6}, 3},    {"Augmented", {0, 4, 8}, 3},
      {"Major 6", {0, 4, 7, 9}, 4},     {"Minor 6", {0, 3, 7, 9}, 4},    {"Add 9", {0, 4, 7, 14}, 4},
      {"Minor 9", {0, 3, 7, 10, 14}, 5}, {"Major 9", {0, 4, 7, 11, 14}, 5}, {"Power", {0, 7, 12}, 3},
      {"Octaves", {0, 12}, 2},          {"Fifth stack", {0, 7, 14}, 3},  {"Fourth stack", {0, 5, 10, 15}, 4},
  };
  return v;
}

const std::vector<ScaleDef>& scales() {
  // bit i set = pitch class i (from the root) belongs to the scale
  static const std::vector<ScaleDef> v = {
      {"Chromatic", 0xFFF},        {"Major", 0xAB5},           {"Natural minor", 0x5AD},
      {"Harmonic minor", 0x9AD},   {"Melodic minor", 0xAAD},   {"Dorian", 0x6AD},
      {"Phrygian", 0x5AB},         {"Lydian", 0xAD5},          {"Mixolydian", 0x6B5},
      {"Locrian", 0x56B},          {"Major pentatonic", 0x295}, {"Minor pentatonic", 0x4A9},
      {"Blues", 0x4E9},            {"Whole tone", 0x555},       {"Diminished", 0xB6D},
  };
  return v;
}

void appendInfos(std::vector<PluginInfo>& v) {
  const PluginKind G = PluginKind::Generator, E = PluginKind::Effect, N = PluginKind::NoteFx;

  // ---------------------------------------------------------- Peak Follower
  {
    PluginInfo p = make("gl.peakctl", "Peak Follower", "Controller", E);
    p.params.resize(PkCount);
    p.params[PkBase] = lin("Peak base", "%", 0, 1, 0);
    p.params[PkAmount] = lin("Peak amount", "%", -1, 1, 1);
    p.params[PkTension] = lin("Peak tension", "", -1, 1, 0);
    p.params[PkAttack] = lg("Peak attack", "s", 0.0005, 0.5, 0.002);
    p.params[PkDecay] = lg("Peak decay", "s", 0.01, 5, 0.25);
    p.params[LfBase] = lin("LFO base", "%", 0, 1, 0);
    p.params[LfAmount] = lin("LFO amount", "%", -1, 1, 1);
    p.params[LfTension] = lin("LFO tension", "", -1, 1, 0);
    p.params[LfShape] = ch("LFO shape", kLfoShapes, 0);
    p.params[LfSync] = tog("LFO tempo sync", 1);
    p.params[LfRate] = lg("LFO rate", "Hz", 0.01, 20, 1);
    p.params[LfSyncRate] = ch("LFO length", rateNames(), 2);
    p.params[LfPhase] = lin("LFO phase", "%", 0, 1, 0);
    p.params[PkRamp] = tog("Smooth output", 1);
    p.params[PkMute] = tog("Mute audio", 0);
    p.params[PkModel] = ch("Engine", {"Legacy", "Reference"}, 1);
    p.legacyModelParam = PkModel;
    p.params[PkRBase] = lin("Peak base (ref.)", "", 0, 65536, 0);
    p.params[PkRAmount] = lin("Peak amount (ref.)", "", -256, 256, 128);
    p.params[PkRTension] = lin("Peak tension (ref.)", "", -128, 128, 0);
    p.params[PkRDecay] = lin("Peak decay (ref.)", "", 5, 128, 66);
    p.params[LfRBase] = lin("LFO base (ref.)", "", 0, 65536, 0);
    p.params[LfRAmount] = lin("LFO amount (ref.)", "", -256, 256, 0);
    p.params[LfRTension] = lin("LFO tension (ref.)", "", -128, 128, 0);
    p.params[LfRSpeed] = lin("LFO speed (ref.)", "", 200, 65536, 32950);
    p.params[LfRPhase] = lin("LFO phase (ref.)", "", 0, 65535, 0);
    p.outputs = {"Peak + LFO", "Peak", "LFO"};
    // J-42: factory presets on the reference model's stored controls.
    p.presets = {{"Pump follower", {{PkRDecay, 80.f}, {PkRTension, -40.f}, {PkRAmount, 160.f}}},
                 {"Slow swell", {{PkRDecay, 30.f}, {PkRTension, 50.f}, {PkRAmount, 200.f}}},
                 {"Inverted ducker", {{PkRBase, 65536.f}, {PkRAmount, -200.f}, {PkRDecay, 70.f}}},
                 {"Bar wobble", {{PkRAmount, 0.f}, {LfRAmount, 256.f}, {LfRSpeed, 32950.f}, {LfShape, 0.f}}},
                 {"Random steps", {{PkRAmount, 0.f}, {LfRAmount, 256.f}, {LfShape, 4.f}, {LfRSpeed, 16384.f}}}};
    p.editorWidth = 640;
    v.push_back(std::move(p));
  }
  // ------------------------------------------------------------- X-Y Pad
  {
    PluginInfo p = make("gl.xyctl", "X-Y Pad", "Controller", E);
    p.params.resize(XyCount);
    p.params[XyX] = lin("X", "%", 0, 1, 0.5);
    p.params[XyY] = lin("Y", "%", 0, 1, 0.5);
    p.params[XyXMin] = lin("X min", "%", 0, 1, 0);
    p.params[XyXMax] = lin("X max", "%", 0, 1, 1);
    p.params[XyXTension] = lin("X tension", "", -1, 1, 0);
    p.params[XyYMin] = lin("Y min", "%", 0, 1, 0);
    p.params[XyYMax] = lin("Y max", "%", 0, 1, 1);
    p.params[XyYTension] = lin("Y tension", "", -1, 1, 0);
    p.params[XySmooth] = lg("Smoothing", "s", 0.001, 2, 0.05);
    p.params[XyInvertY] = tog("Invert Y", 0);
    p.outputs = {"X", "Y"};
    p.presets = {{"Direct", {{XySmooth, 0.001f}}},
                 {"Glide", {{XySmooth, 0.6f}}},
                 {"Narrow window", {{XyXMin, 0.25f}, {XyXMax, 0.75f}, {XyYMin, 0.25f}, {XyYMax, 0.75f}}},
                 {"Exponential", {{XyXTension, 0.6f}, {XyYTension, 0.6f}}}};
    p.editorWidth = 560;
    v.push_back(std::move(p));
  }
  // ----------------------------------------------------------- X-Y-Z Pad
  {
    PluginInfo p = make("gl.xyzctl", "X-Y-Z Pad", "Controller", E);
    p.params.resize(XzCount);
    p.params[XzX] = lin("X", "%", 0, 1, 0.5);
    p.params[XzY] = lin("Y", "%", 0, 1, 0.5);
    p.params[XzZ] = lin("Z", "%", 0, 1, 0.5);
    p.params[XzXMin] = lin("X min", "%", 0, 1, 0);
    p.params[XzXMax] = lin("X max", "%", 0, 1, 1);
    p.params[XzYMin] = lin("Y min", "%", 0, 1, 0);
    p.params[XzYMax] = lin("Y max", "%", 0, 1, 1);
    p.params[XzZMin] = lin("Z min", "%", 0, 1, 0);
    p.params[XzZMax] = lin("Z max", "%", 0, 1, 1);
    p.params[XzSmooth] = lg("Smoothing", "s", 0.001, 2, 0.08);
    p.outputs = {"X", "Y", "Z"};
    p.presets = {{"Direct", {{XzSmooth, 0.001f}}}, {"Floating", {{XzSmooth, 0.9f}}}, {"Upper half", {{XzZMin, 0.5f}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // --------------------------------------------------- Formula Controller
  {
    PluginInfo p = make("gl.formulactl", "Formula Mod", "Controller", E);
    p.params.resize(FmCount);
    p.params[FmA] = lin("a", "%", 0, 1, 0.5);
    p.params[FmB] = lin("b", "%", 0, 1, 0.5);
    p.params[FmC] = lin("c", "%", 0, 1, 0.5);
    p.params[FmSmooth] = lg("Smoothing", "s", 0.001, 2, 0.005);
    p.outputs = {"Output"};
    // The formula is the plugin's data; presets set knobs, the editor offers formula examples.
    p.presets = {{"Centre", {}}, {"Low knobs", {{FmA, 0.0f}, {FmB, 0.0f}, {FmC, 0.0f}}}, {"High knobs", {{FmA, 1.0f}, {FmB, 1.0f}, {FmC, 1.0f}}}};
    p.editorWidth = 620;
    v.push_back(std::move(p));
  }
  // -------------------------------------------------- Envelope Controller
  {
    PluginInfo p = make("gl.envctl", "Envelope Mod", "Controller", G);
    p.params.resize(EcCount);
    for (int a = 0; a < kEnvArts; ++a) {
      std::string pre = std::string("Env ") + static_cast<char>('A' + a) + " ";
      p.params[envParam(a, EaBase)] = lin(pre + "base", "%", 0, 1, 0);
      p.params[envParam(a, EaEnvAmt)] = lin(pre + "envelope", "%", -1, 1, 1);
      p.params[envParam(a, EaLfoAmt)] = lin(pre + "LFO", "%", -1, 1, 0);
      p.params[envParam(a, EaMode)] = ch(pre + "mode", {"Sum", "Product"}, 0);
      p.params[envParam(a, EaNumPoints)] = lin(pre + "points", "", 2, 6, 4);
      p.params[envParam(a, EaSustain)] = ch(pre + "sustain", {"None", "Point 1", "Point 2", "Point 3", "Point 4", "Point 5", "Point 6"}, 2);
      p.params[envParam(a, EaLoop)] = tog(pre + "loop", 0);
      p.params[envParam(a, EaLfoShape)] = ch(pre + "LFO shape", kLfoShapes, 0);
      p.params[envParam(a, EaLfoSync)] = tog(pre + "LFO sync", 1);
      p.params[envParam(a, EaLfoRate)] = lg(pre + "LFO rate", "Hz", 0.01, 20, 2);
      p.params[envParam(a, EaLfoSyncRate)] = ch(pre + "LFO length", rateNames(), 3);
      p.params[envParam(a, EaLfoDelay)] = lin(pre + "LFO delay", "s", 0, 4, 0);
      p.params[envParam(a, EaKeyLow)] = lin(pre + "low key", "note", 0, 127, 0);
      p.params[envParam(a, EaKeyHigh)] = lin(pre + "high key", "note", 0, 127, 127);
      p.params[envParam(a, EaVelAmt)] = lin(pre + "velocity", "%", 0, 1, 0);
      p.params[envParam(a, EaRetrig)] = tog(pre + "retrigger", 1);
      static const float kTimes[kEnvPoints] = {0.005f, 0.2f, 0.3f, 0.4f, 0.4f, 0.4f};
      static const float kLevels[kEnvPoints] = {1.0f, 0.6f, 0.6f, 0.0f, 0.0f, 0.0f};
      for (int k = 0; k < kEnvPoints; ++k) {
        std::string pt = pre + "point " + std::to_string(k + 1);
        p.params[envPointTime(a, k)] = lg(pt + " time", "s", 0.001, 10, kTimes[k]);
        p.params[envPointLevel(a, k)] = lin(pt + " level", "%", 0, 1, kLevels[k]);
      }
    }
    p.params[EcContinuous] = tog("Continuous output", 0);
    // J-41: articulator model (default for new instances; older states keep the simple model).
    p.params[EcModel] = ch("Engine", {"Simple", "Articulators"}, 1);
    p.legacyModelParam = EcModel;
    for (int a = 0; a < kArts; ++a) {
      const std::string pre = "Art " + std::to_string(a + 1) + " ";
      p.params[artParam(a, AfBase)] = lin(pre + "base", "%", 0, 1, 0.5);
      p.params[artParam(a, AfEnvAmt)] = lin(pre + "envelope", "%", -1, 1, 1);
      p.params[artParam(a, AfLfoAmt)] = lin(pre + "LFO", "%", -1, 1, 0.5);
      p.params[artParam(a, AfAttack)] = lin(pre + "attack scale", "x", 0, 2, 1);
      p.params[artParam(a, AfDecay)] = lin(pre + "decay scale", "x", 0, 2, 1);
      p.params[artParam(a, AfSustain)] = lin(pre + "sustain offset", "", -1, 1, 0);
      p.params[artParam(a, AfRelease)] = lin(pre + "release scale", "x", 0, 2, 1);
      p.params[artParam(a, AfLfoSpeed)] = lin(pre + "LFO speed", "", 0, 1, 40898.0 / 65536.0);
      p.params[artParam(a, AfLfoTension)] = lin(pre + "LFO tension", "", -1, 1, 0);
      p.params[artParam(a, AfLfoSkew)] = lin(pre + "LFO skew", "", -0.5, 0.5, 0);
      p.params[artParam(a, AfLfoPw)] = lin(pre + "LFO width", "", 0, 1, 0);
    }
    p.params[EcModX] = lin("Mod X", "%", 0, 1, 0.5);
    p.params[EcModY] = lin("Mod Y", "%", 0, 1, 0.5);
    p.outputs = {"Env A", "Env B", "Env C", "Env D", "Env E", "Env F", "Env G", "Env H"};
    p.presets = {
        {"Default ADSR", {}},
        {"Long release", {{artParam(0, AfRelease), 2.0f}, {artParam(0, AfDecay), 1.5f}}},
        {"Soft attack", {{artParam(0, AfAttack), 2.0f}, {artParam(0, AfSustain), 0.25f}}},
        {"Pluck shape", {{EcModel, 0}, {envPointTime(0, 0), 0.002f}, {envPointTime(0, 1), 0.15f}, {envPointLevel(0, 1), 0.0f},
                         {envParam(0, EaNumPoints), 2}, {envParam(0, EaSustain), 0}}},
        {"Slow swell", {{EcModel, 0}, {envPointTime(0, 0), 1.5f}, {envPointTime(0, 1), 0.5f}, {envPointLevel(0, 1), 0.8f}}},
        {"Gated LFO", {{EcModel, 0}, {envParam(0, EaLfoAmt), 0.5f}, {envParam(0, EaMode), 1}, {envParam(0, EaLfoSyncRate), 4}}}};
    p.editorWidth = 740;
    v.push_back(std::move(p));
  }
  // -------------------------------------------------- Keyboard Controller
  {
    PluginInfo p = make("gl.keyctl", "Keyboard Mod", "Controller", G);
    p.params.resize(KcCount);
    p.params[KcAttack] = lg("Attack", "s", 0.001, 2, 0.01);
    p.params[KcRelease] = lg("Release", "s", 0.001, 2, 0.05);
    p.params[KcNoteOff] = tog("Note-off value", 0);
    p.params[KcNoteOffValue] = lin("Note-off level", "%", 0, 1, 0);
    p.outputs = {"Key value", "Velocity", "Gate", "Note"};
    p.presets = {{"Instant", {{KcAttack, 0.001f}, {KcRelease, 0.001f}}},
                 {"Smooth glide", {{KcAttack, 0.3f}, {KcRelease, 0.3f}}},
                 {"Falls to zero", {{KcNoteOff, 1}, {KcRelease, 0.4f}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // --------------------------------------------------------- Voltage Out
  {
    PluginInfo p = make("gl.cvctl", "Voltage Out", "Controller", G);
    p.params.resize(CvCount);
    std::vector<std::string> src = {"Off", "Pitch", "Gate", "Velocity", "Clock", "Trigger"};
    p.params[CvEnable] = tog("Output enabled", 0);
    p.params[CvLeftSrc] = ch("Left source", src, CvsPitch);
    p.params[CvRightSrc] = ch("Right source", src, CvsGate);
    p.params[CvLeftOfs] = lin("Left offset", "", -1, 1, 0);
    p.params[CvRightOfs] = lin("Right offset", "", -1, 1, 0);
    p.params[CvLeftScale] = lin("Left scale", "%", 0, 1, 1);
    p.params[CvRightScale] = lin("Right scale", "%", 0, 1, 1);
    p.params[CvLeftBipolar] = tog("Left bipolar", 1);
    p.params[CvRightBipolar] = tog("Right bipolar", 0);
    p.params[CvPitchScale] = ch("Pitch scale", {"1 V/oct", "1.2 V/oct"}, 0);
    p.params[CvRange] = ch("Voltage range", {"+/-5 V", "+/-10 V", "0 to 10 V"}, 0);
    p.params[CvClockPpq] = ch("Clock rate", {"2 PPQ", "4 PPQ", "8 PPQ", "12 PPQ", "16 PPQ", "24 PPQ", "32 PPQ", "48 PPQ", "64 PPQ", "96 PPQ"}, 5);
    p.params[CvPulse] = lg("Pulse length", "s", 0.001, 0.2, 0.01);
    p.params[CvGlide] = lin("Glide", "s", 0, 1, 0);
    p.outputs = {"Pitch", "Gate", "Velocity"};
    p.presets = {{"Pitch + gate", {}},
                 {"Clock + trigger", {{CvLeftSrc, CvsClock}, {CvRightSrc, CvsTrigger}}},
                 {"Velocity + gate", {{CvLeftSrc, CvsVelocity}, {CvLeftBipolar, 0}}},
                 {"Gliding pitch", {{CvGlide, 0.08f}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // ------------------------------------------------------ Control Surface
  {
    PluginInfo p = make("gl.surface", "Control Deck", "Controller", E);
    p.params.resize(SfCountV2);
    for (int k = 0; k < kSurfaceMax; ++k) {
      p.params[surfaceParam(k)] = lin("Control " + std::to_string(k + 1), "%", 0, 1, 0);
      p.outputs.push_back("Control " + std::to_string(k + 1));
    }
    p.params[SfSmooth] = lg("Smoothing", "s", 0.001, 1, 0.02);
    p.presets = {{"All centred", {{0, 0.5f}, {1, 0.5f}, {2, 0.5f}, {3, 0.5f}, {4, 0.5f}, {5, 0.5f}, {6, 0.5f}, {7, 0.5f}}},
                 {"Snappy", {{SfSmooth, 0.001f}}},
                 {"Lazy", {{SfSmooth, 0.4f}}}};
    p.editorWidth = 680;
    v.push_back(std::move(p));
  }

  // -------------------------------------------------------- Arpeggiator
  {
    PluginInfo p = make("gl.arp", "Arpeggiator", "Note FX", N);
    p.params.resize(ArCount);
    p.params[ArDirection] = ch("Direction", {"Up", "Down", "Up-down", "Down-up", "Random", "As played", "Chord"}, 0);
    p.params[ArRange] = ch("Range", {"1 octave", "2 octaves", "3 octaves", "4 octaves"}, 0);
    p.params[ArRate] = ch("Rate", rateNames(), 4);
    p.params[ArGate] = lin("Gate", "%", 0.05, 1, 0.5);
    p.params[ArRepeat] = lin("Repeats", "", 1, 8, 1);
    p.params[ArSwing] = lin("Swing", "%", 0, 0.75, 0);
    p.params[ArVelMode] = ch("Velocity", {"As played", "Fixed", "Accent first"}, 0);
    p.params[ArVelocity] = lin("Fixed velocity", "%", 0, 1, 0.8);
    p.params[ArLatch] = tog("Latch", 0);
    p.params[ArTranspose] = lin("Transpose", "st", -24, 24, 0);
    p.presets = {{"Classic up", {}},
                 {"Two-octave bounce", {{ArDirection, AdUpDown}, {ArRange, 1}}},
                 {"Shuffled eighths", {{ArRate, 3}, {ArSwing, 0.4f}, {ArGate, 0.7f}}},
                 {"Random sparkle", {{ArDirection, AdRandom}, {ArRange, 2}, {ArRate, 5}, {ArGate, 0.3f}}},
                 {"Held stabs", {{ArDirection, AdChord}, {ArRate, 3}, {ArGate, 0.25f}, {ArLatch, 1}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // ---------------------------------------------------- Chord Generator
  {
    PluginInfo p = make("gl.chord", "Chord Generator", "Note FX", N);
    p.params.resize(ChCount);
    std::vector<std::string> names;
    for (const auto& c : chordShapes()) names.push_back(c.name);
    p.params[ChType] = ch("Chord", names, 0);
    p.params[ChInversion] = ch("Inversion", {"Root", "First", "Second", "Third"}, 0);
    p.params[ChSpread] = ch("Voicing", {"Close", "Drop 2", "Open", "Wide"}, 0);
    p.params[ChStrum] = lin("Strum", "s", 0, 0.25, 0);
    p.params[ChStrumDir] = ch("Strum direction", {"Up", "Down", "Random"}, 0);
    p.params[ChVelSpread] = lin("Velocity spread", "%", 0, 1, 0);
    p.params[ChTranspose] = lin("Transpose", "st", -24, 24, 0);
    p.params[ChBass] = tog("Bass note", 0);
    p.presets = {{"Major triad", {}},
                 {"Minor seventh", {{ChType, 4}}},
                 {"Strummed nine", {{ChType, 13}, {ChStrum, 0.03f}}},
                 {"Wide power", {{ChType, 14}, {ChSpread, 3}, {ChBass, 1}}},
                 {"Soft sus", {{ChType, 6}, {ChVelSpread, 0.4f}, {ChInversion, 1}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // ----------------------------------------------------- Note Sequencer
  {
    PluginInfo p = make("gl.notesq", "Note Sequencer", "Note FX", N);
    p.params.resize(SqCount);
    std::vector<std::string> sel = {"Note 1", "Note 2", "Note 3", "Note 4", "Note 5", "Note 6", "Lowest", "Highest", "All", "Random"};
    for (int s = 0; s < kSeqSteps; ++s) {
      std::string pre = "Step " + std::to_string(s + 1) + " ";
      p.params[seqParam(s, SqOn)] = tog(pre + "on", 1);
      p.params[seqParam(s, SqNote)] = ch(pre + "note", sel, s % 4 == 0 ? 0 : (s % 4 == 1 ? 1 : (s % 4 == 2 ? 2 : 1)));
      p.params[seqParam(s, SqOctave)] = lin(pre + "octave", "", -2, 2, 0);
      p.params[seqParam(s, SqVelocity)] = lin(pre + "velocity", "%", 0, 1, s % 4 == 0 ? 1.0 : 0.75);
      p.params[seqParam(s, SqGate)] = lin(pre + "gate", "%", 0.05, 1, 0.5);
      p.params[seqParam(s, SqTie)] = tog(pre + "tie", 0);
    }
    p.params[SqLength] = lin("Length", "", 1, 16, 16);
    p.params[SqRate] = ch("Rate", rateNames(), 4);
    p.params[SqSwing] = lin("Swing", "%", 0, 0.75, 0);
    p.params[SqTranspose] = lin("Transpose", "st", -24, 24, 0);
    p.params[SqThru] = tog("Pass held notes", 0);
    p.presets = {{"Rolling triad", {}},
                 {"Octave pulse", {{seqParam(1, SqOctave), 1}, {seqParam(3, SqOctave), 1}, {seqParam(5, SqOctave), 1},
                                   {seqParam(7, SqOctave), 1}, {SqLength, 8}}},
                 {"Gated chords", {{seqParam(0, SqNote), 8}, {seqParam(2, SqOn), 0}, {seqParam(4, SqNote), 8}, {seqParam(5, SqOn), 0},
                                   {seqParam(6, SqNote), 8}, {SqLength, 8}}},
                 {"Long ties", {{seqParam(1, SqTie), 1}, {seqParam(5, SqTie), 1}, {SqRate, 3}}}};
    p.editorWidth = 760;
    v.push_back(std::move(p));
  }
  // ---------------------------------------------------------- Key Mapper
  {
    PluginInfo p = make("gl.keymap", "Key Remap", "Note FX", N);
    p.params.resize(KmCount);
    for (int k = 0; k < 12; ++k) p.params[KmMap + k] = lin(std::string(kNoteNames[k]) + " offset", "", -13, 12, 0);
    std::vector<std::string> notes(kNoteNames, kNoteNames + 12);
    std::vector<std::string> sc;
    for (const auto& s : scales()) sc.push_back(s.name);
    p.params[KmRoot] = ch("Root", notes, 0);
    p.params[KmScale] = ch("Scale", sc, 0);
    p.params[KmSnap] = ch("Snap", {"Off", "Nearest", "Up", "Down"}, 0);
    p.params[KmTranspose] = lin("Transpose", "st", -24, 24, 0);
    p.params[KmOctave] = lin("Octave", "", -4, 4, 0);
    p.params[KmMulti] = ch("Mode", {"Mapped only", "Mapped + original"}, 0);
    p.presets = {{"Pass through", {}},
                 {"C major snap", {{KmScale, 1}, {KmSnap, 1}}},
                 {"A minor pentatonic", {{KmRoot, 9}, {KmScale, 11}, {KmSnap, 1}}},
                 {"Fifth harmony", {{KmMap + 0, 7}, {KmMap + 1, 7}, {KmMap + 2, 7}, {KmMap + 3, 7}, {KmMap + 4, 7}, {KmMap + 5, 7},
                                    {KmMap + 6, 7}, {KmMap + 7, 7}, {KmMap + 8, 7}, {KmMap + 9, 7}, {KmMap + 10, 7}, {KmMap + 11, 7},
                                    {KmMulti, 1}}},
                 {"White keys only", {{KmMap + 1, -13}, {KmMap + 3, -13}, {KmMap + 6, -13}, {KmMap + 8, -13}, {KmMap + 10, -13}}}};
    p.editorWidth = 640;
    v.push_back(std::move(p));
  }
  // -------------------------------------------------------- Key Splitter
  {
    PluginInfo p = make("gl.split", "Key Zones", "Note FX", N);
    p.params.resize(SpCount);
    static const float lows[kSplitZones] = {0, 60, 0, 0}, highs[kSplitZones] = {59, 127, 127, 127};
    for (int z = 0; z < kSplitZones; ++z) {
      std::string pre = "Zone " + std::to_string(z + 1) + " ";
      p.params[splitParam(z, SpOn)] = tog(pre + "on", z < 2 ? 1 : 0);
      p.params[splitParam(z, SpLow)] = lin(pre + "low key", "note", 0, 127, lows[z]);
      p.params[splitParam(z, SpHigh)] = lin(pre + "high key", "note", 0, 127, highs[z]);
      p.params[splitParam(z, SpTranspose)] = lin(pre + "transpose", "st", -48, 48, 0);
      p.params[splitParam(z, SpVelLow)] = lin(pre + "min velocity", "%", 0, 1, 0);
      p.params[splitParam(z, SpVelHigh)] = lin(pre + "max velocity", "%", 0, 1, 1);
    }
    p.params[SpMode] = ch("Overlap", {"All matching zones", "First matching zone"}, 0);
    p.presets = {{"Split at C4", {}},
                 {"Bass octave down", {{splitParam(0, SpTranspose), -12}}},
                 {"Velocity layers", {{splitParam(0, SpHigh), 127}, {splitParam(0, SpVelHigh), 0.6f}, {splitParam(1, SpLow), 0},
                                      {splitParam(1, SpVelLow), 0.6f}}},
                 {"Three zones", {{splitParam(0, SpHigh), 47}, {splitParam(1, SpLow), 48}, {splitParam(1, SpHigh), 71},
                                  {splitParam(2, SpOn), 1}, {splitParam(2, SpLow), 72}}}};
    p.editorWidth = 680;
    v.push_back(std::move(p));
  }
  // -------------------------------------------------------- Level Scaler
  {
    PluginInfo p = make("gl.velscale", "Velocity Scale", "Note FX", N);
    p.params.resize(VsCount);
    p.params[VsTarget] = ch("Target", {"Velocity", "Fine pitch"}, 0);
    p.params[VsCenter] = lin("Centre", "%", 0, 1, 0);
    p.params[VsMultiply] = lin("Multiply", "x", -2, 2, 1);
    p.params[VsOffset] = lin("Offset", "", -1, 1, 0);
    p.params[VsTension] = lin("Tension", "", -1, 1, 0);
    p.params[VsHumanize] = lin("Humanize", "%", 0, 1, 0);
    p.params[VsHumanMode] = ch("Humanize mode", {"Subtract", "Spread", "Add"}, 1);
    p.params[VsMin] = lin("Minimum", "%", 0, 1, 0);
    p.params[VsMax] = lin("Maximum", "%", 0, 1, 1);
    p.params[VsModel] = ch("Engine", {"Legacy", "Reference"}, 0);  // imports run the reference
    p.params[VsEdit] = ch("Shown target", {"Velocity", "Release", "Fine pitch", "Pan", "Mod X", "Mod Y"}, 0);
    {
      static const char* tn[kVsTargets] = {"Velocity", "Release", "Pitch", "Pan", "Mod X", "Mod Y"};
      for (int t = 0; t < kVsTargets; ++t) {
        const std::string n = tn[t];
        p.params[vsRef(t, VfCenter)] = lin(n + " centre", "%", 0, 1, t < 2 ? 0 : 0.5);
        p.params[vsRef(t, VfMultiply)] = lin(n + " multiply", "x", -2, 2, 1);
        p.params[vsRef(t, VfOffset)] = lin(n + " offset", "", -1, 1, 0);
        p.params[vsRef(t, VfTension)] = lin(n + " tension", "", -1, 1, 0);
        p.params[vsRef(t, VfHumanize)] = lin(n + " humanize", "%", 0, 1, 0);
        p.params[vsRef(t, VfMode)] = ch(n + " humanize mode", {"Subtract", "Spread", "Add"}, 1);
      }
    }
    p.legacyModelParam = VsModel;
    p.presets = {{"Unity", {}},
                 {"Compress dynamics", {{VsCenter, 0.7f}, {VsMultiply, 0.4f}}},
                 {"Expand dynamics", {{VsCenter, 0.5f}, {VsMultiply, 1.6f}}},
                 {"Fixed loud", {{VsMultiply, 0.0f}, {VsOffset, 0.9f}}},
                 {"Loose player", {{VsHumanize, 0.25f}}},
                 {"Detune drift", {{VsTarget, 1}, {VsMultiply, 1.0f}, {VsHumanize, 0.002f}}}};
    p.editorWidth = 600;
    v.push_back(std::move(p));
  }
  // -------------------------------------------------------- Colour Router
  {
    PluginInfo p = make("gl.colormap", "Colour Router", "Note FX", N);
    p.params.resize(CmCount);
    std::vector<std::string> outs = {"This channel", "Target 1", "Target 2", "Target 3", "Target 4",
                                     "Target 5", "Target 6", "Target 7", "Target 8", "Mute"};
    for (int k = 0; k < kColorGroups; ++k) p.params[CmMap + k] = ch("Group " + std::to_string(k + 1), outs, 0);
    p.params[CmSource] = ch("Group by", {"Note colour", "Velocity layer", "Pitch class", "Octave"}, 0);
    p.presets = {{"Everything here", {}},
                 {"Soft / hard layers", {{CmSource, CsVelocity}, {CmMap + 8, 1}, {CmMap + 9, 1}, {CmMap + 10, 1}, {CmMap + 11, 1},
                                         {CmMap + 12, 1}, {CmMap + 13, 1}, {CmMap + 14, 1}, {CmMap + 15, 1}}},
                 {"Octave fan-out", {{CmSource, CsOctave}, {CmMap + 3, 1}, {CmMap + 4, 2}, {CmMap + 5, 3}, {CmMap + 6, 4}}}};
    p.editorWidth = 620;
    v.push_back(std::move(p));
  }
  // ---------------------------------------------------------- Note Script
  {
    PluginInfo p = make("gl.notescript", "Note Script", "Note FX", N);
    p.params.resize(NsCount);
    p.params[NsA] = lin("a", "%", 0, 1, 0.5);
    p.params[NsB] = lin("b", "%", 0, 1, 0.5);
    p.params[NsC] = lin("c", "%", 0, 1, 0.5);
    p.params[NsEnable] = tog("Enabled", 1);
    p.presets = {{"Neutral knobs", {}}, {"Knobs low", {{NsA, 0.0f}, {NsB, 0.0f}, {NsC, 0.0f}}}, {"Knobs high", {{NsA, 1.0f}, {NsB, 1.0f}, {NsC, 1.0f}}}};
    p.editorWidth = 640;
    v.push_back(std::move(p));
  }

  // ----------------------------------------------------------- Patch Host
  for (int fx = 0; fx < 2; ++fx) {
    PluginInfo p = make(fx ? "gl.patcherfx" : "gl.patcher", fx ? "Junction FX" : "Junction", "Utility", fx ? E : G);
    p.params.resize(PhCountV2);
    for (int k = 0; k < kMacros; ++k) p.params[k] = lin("Macro " + std::to_string(k + 1), "%", 0, 1, 0.5);
    p.params[PhMix] = lin("Mix", "%", 0, 1, 1);
    p.params[PhGain] = lin("Output", "dB", -24, 12, 0);
    for (int k = 0; k < kPatchMaps; ++k) p.params[PhMap0 + k] = lin("Param " + std::to_string(k + 1), "%", 0, 1, 0);
    p.presets = {{"Macros centred", {}}, {"Macros low", {{0, 0.0f}, {1, 0.0f}, {2, 0.0f}, {3, 0.0f}}}, {"Half wet", {{PhMix, 0.5f}}}};
    p.editorWidth = fx ? 760.0f : 600.0f;  // instruments open in the 600 px channel window
    v.push_back(std::move(p));
  }
  // ----------------------------------------------------------- Mixer Send
  {
    PluginInfo p = make("gl.send", "Mixer Send", "Routing", E);
    p.params.resize(SnCount);
    p.params[SnTarget] = lin("Target track", "", -1, 500, 1);
    p.params[SnLevel] = lin("Send level", "%", 0, 1.25, 1);
    p.params[SnPan] = lin("Send pan", "pan", -1, 1, 0);
    p.params[SnDry] = lin("Dry level", "%", 0, 1, 1);
    p.params[SnRoute] = ch("Target numbering", {"Mixer track", "Route"}, 0);
    p.routeModeParam = SnRoute;
    p.presets = {{"Parallel copy", {}}, {"Send only", {{SnDry, 0.0f}}}, {"Quiet send", {{SnLevel, 0.4f}}}};
    p.editorWidth = 460;
    v.push_back(std::move(p));
  }
  // ------------------------------------------------------ Bus send / return
  {
    std::vector<std::string> buses;
    for (int b = 0; b < 8; ++b) buses.push_back("Bus " + std::to_string(b + 1));
    PluginInfo s = make("gl.bussend", "Bus Send", "Routing", E);
    s.params.resize(BsCount);
    s.params[BsBus] = ch("Bus", buses, 0);
    s.params[BsLevel] = lin("Send level", "%", 0, 1.25, 1);
    s.params[BsDry] = lin("Dry level", "%", 0, 1, 1);
    s.presets = {{"Copy to bus 1", {}}, {"Move to bus 2", {{BsBus, 1}, {BsDry, 0.0f}}}, {"Bus 3 at half", {{BsBus, 2}, {BsLevel, 0.5f}}}};
    s.editorWidth = 440;
    v.push_back(std::move(s));
    PluginInfo r = make("gl.busreturn", "Bus Return", "Routing", E);
    r.params.resize(BrCount);
    r.params[BrBus] = ch("Bus", buses, 0);
    r.params[BrLevel] = lin("Return level", "%", 0, 1.25, 1);
    r.params[BrDry] = lin("Own signal", "%", 0, 1, 1);
    r.presets = {{"Add bus 1", {}}, {"Bus 2 only", {{BrBus, 1}, {BrDry, 0.0f}}}, {"Bus 3 blend", {{BrBus, 2}, {BrLevel, 0.5f}}}};
    r.editorWidth = 440;
    v.push_back(std::move(r));
  }
  // ---------------------------------------------------- Sidechain Ducker
  {
    PluginInfo p = make("gl.scduck", "Sidechain Ducker", "Dynamics", E);
    p.params.resize(SdCount);
    p.params[SdThreshold] = lin("Threshold", "dB", -60, 0, -24);
    p.params[SdRatio] = lin("Ratio", "x", 1, 20, 4);
    p.params[SdAttack] = lg("Attack", "s", 0.0005, 0.2, 0.003);
    p.params[SdRelease] = lg("Release", "s", 0.01, 2, 0.15);
    p.params[SdDepth] = lin("Range", "dB", 0, 60, 60);
    p.params[SdSource] = ch("Key", {"Sidechain input", "Own signal"}, 0);
    p.params[SdListen] = tog("Listen to key", 0);
    p.presets = {{"Kick duck", {}},
                 {"Gentle glue", {{SdThreshold, -18.0f}, {SdRatio, 2.0f}, {SdRelease, 0.3f}}},
                 {"Hard pump", {{SdThreshold, -36.0f}, {SdRatio, 20.0f}, {SdRelease, 0.25f}}},
                 {"Fast tuck", {{SdAttack, 0.0005f}, {SdRelease, 0.06f}, {SdDepth, 12.0f}}}};
    p.editorWidth = 560;
    v.push_back(std::move(p));
  }
}

}  // namespace gl::packs::modulation
