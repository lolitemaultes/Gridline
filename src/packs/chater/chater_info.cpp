// Gridline - "chater" pack: catalogue entry for Chater (gl.chater) and its factory presets.
#include "../Packs.h"
#include "chater.h"

namespace gl::packs::chater {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc lgp(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Log, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  const double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}
ParamDesc spare(int i) { return lin("Reserved " + std::to_string(i), "", 0, 1, 0); }

using V = std::vector<std::pair<int, float>>;
// Macro positions (percent) in tile order, then the page values that make the sound.
V macros(int sp, int hz, int mem, int dr, int shi, int gl, int spc, int mix) {
  return {{PSparkle, sp / 100.f}, {PHaze, hz / 100.f},   {PMemory, mem / 100.f}, {PDrift, dr / 100.f},
          {PShine, shi / 100.f},  {PGlitch, gl / 100.f}, {PSpace, spc / 100.f},  {PMix, mix / 100.f}};
}
// The sharp glitch character: hard edges, nothing washed into the space.
V sharp() { return {{PSoftness, 0.f}, {PWash, 0.f}}; }
V operator+(V a, const V& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
// Break odds in tile order (Repeat, Reverse, Tape, Scrub, Jump, Skip).
V odds(int rp, int rv, int tp, int sc, int jp, int sk) {
  return {{POddRepeat, static_cast<float>(rp)}, {POddReverse, static_cast<float>(rv)}, {POddTape, static_cast<float>(tp)},
          {POddScrub, static_cast<float>(sc)},  {POddJump, static_cast<float>(jp)},    {POddSkip, static_cast<float>(sk)}};
}
float chips(std::initializer_list<int> semis) {
  int m = 0;
  for (int s : semis)
    for (int i = 0; i < kChips; ++i)
      if (kChipSemis[static_cast<size_t>(i)] == s) m |= 1 << i;
  return static_cast<float>(m);
}
V link(int slot, int src, int target, float depth) {
  return {{linkParam(slot, LkSource), static_cast<float>(src)}, {linkParam(slot, LkTarget), static_cast<float>(target)}, {linkParam(slot, LkDepth), depth}};
}

}  // namespace

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Chater", "Delay & reverb", PluginKind::Effect, {}, false, {}};
  p.params.resize(PCount);
  for (int i = 0; i < PCount; ++i) p.params[static_cast<size_t>(i)] = spare(i);
  const std::vector<std::string> grids = {"1/2", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
  const std::vector<std::string> divs = {"1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "1/8T", "1/16T", "1/32T"};
  // macros
  // macros: the defaults are the Init sound (dreamy, safe on any source)
  p.params[PSparkle] = lin("Sparkle", "%", 0, 1, 0.35);
  p.params[PHaze] = lin("Haze", "%", 0, 1, 0.45);
  p.params[PMemory] = lin("Memory", "%", 0, 1, 0.35);
  p.params[PDrift] = lin("Drift", "%", 0, 1, 0.25);
  p.params[PShine] = lin("Shine", "%", 0, 1, 0.2);
  p.params[PGlitch] = lin("Glitch", "%", 0, 1, 0);
  p.params[PSpace] = lin("Space", "%", 0, 1, 0.45);
  p.params[PMix] = lin("Mix", "%", 0, 1, 0.4);
  // Catch
  p.params[PLength] = ch("Catch length", {"1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars"}, 2);
  p.params[PQuantise] = ch("Catch quantise", {"1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "1/32T"}, 2);
  p.params[PSmooth] = lgp("Catch smooth", "ms", 0.5, 20, 4);
  p.params[PHold] = tog("Hold capture", 0);
  // Triggers
  p.params[PTrigMode] = ch("Trigger", {"Grid", "Hits", "Both"}, TrigGrid);
  p.params[PGrid] = ch("Grid", grids, 2);
  p.params[PDensity] = lin("Density", "%", 0, 1, 0.04);
  p.params[PVariation] = lin("Variation", "%", 0, 1, 0.25);
  p.params[PLean] = ch("Lean", {"Even", "Downbeats", "Offbeats", "Bar end"}, LeanEven);
  p.params[PSpan] = ch("Span", {"1 step", "2 steps", "3 steps", "4 steps", "6 steps", "8 steps"}, 1);
  p.params[PSensitivity] = lin("Hit sensitivity", "%", 0, 1, 0.5);
  p.params[PHitGap] = lgp("Hit gap", "ms", 20, 1000, 120);
  p.params[PHitChance] = lin("Hit chance", "%", 0, 1, 0.7);
  p.params[PSeed] = lin("Seed", "", 1, 999, 1);
  // odds
  p.params[POddRepeat] = lin("Repeat odds", "pct", 0, 100, 35);
  p.params[POddReverse] = lin("Reverse odds", "pct", 0, 100, 45);
  p.params[POddTape] = lin("Tape odds", "pct", 0, 100, 40);
  p.params[POddScrub] = lin("Scrub odds", "pct", 0, 100, 10);
  p.params[POddJump] = lin("Jump odds", "pct", 0, 100, 10);
  p.params[POddSkip] = lin("Skip odds", "pct", 0, 100, 0);
  // Repeat, Tape and scrub
  p.params[PDivision] = ch("Division", divs, 2);
  p.params[PAccel] = lin("Accel", "%", -1, 1, 0);
  p.params[PRamp] = lin("Pitch ramp", "st", -24, 24, 0);
  p.params[PGate] = lin("Gate", "%", 0.1, 1, 1);
  p.params[PTapeTime] = ch("Tape time", {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars"}, 2);
  p.params[PTapeCurve] = lin("Tape curve", "%", 0, 1, 0.6);
  p.params[PScrubDepth] = lin("Scrub depth", "%", 0, 1, 0.3);
  p.params[PScrubRate] = ch("Scrub rate", {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"}, 2);
  // Haze and soft breaks
  p.params[PDamp] = lgp("Damp", "Hz", 1000, 20000, 20000);
  p.params[PWarmth] = lin("Warmth", "%", 0, 1, 0);
  p.params[PSoftness] = lin("Soft edges", "%", 0, 1, 0.4);
  p.params[PWash] = lin("Wash", "%", 0, 1, 0.15);
  // Memory
  p.params[PWow] = lin("Wow", "%", 0, 1, 0);
  p.params[PFlutter] = lin("Flutter", "%", 0, 1, 0);
  p.params[PAge] = lin("Tape age", "%", 0, 1, 0);
  p.params[PEnsemble] = lin("Ensemble", "%", 0, 1, 0);
  p.params[PHiss] = lin("Hiss", "%", 0, 1, 0);
  p.params[PCrackle] = lin("Crackle", "%", 0, 1, 0);
  p.params[PWowRate] = lgp("Wow rate", "Hz", 0.1, 2, 0.5);
  // Glitter
  p.params[PGrDensity] = lgp("Grain density", "/s", 1, 200, 10);
  p.params[PGrSize] = lgp("Grain size", "s", 0.005, 0.25, 0.16);
  p.params[PGrScatter] = lin("Scatter", "%", 0, 1, 0.5);
  p.params[PGrReverse] = lin("Grain reverse", "%", 0, 1, 0.25);
  p.params[PGrLevel] = lin("Glitter level", "%", 0, 1, 0);
  p.params[PGrWindow] = ch("Window", {"Soft", "Bell", "Sharp", "Click", "Swell"}, WinSwell);
  p.params[PIntervals] = lin("Intervals", "", 0, 255, chips({0, 12, 19, 24}));
  p.params[PReach] = lin("Reach", "%", 0, 1, 0.45);
  p.params[PDetune] = lin("Detune", "ct", 0, 50, 8);
  p.params[PScale] = ch("Scale", {"Off", "Major", "Minor", "Pentatonic major", "Pentatonic minor", "Dorian", "Lydian", "Whole tone", "Chromatic"}, 3);
  p.params[PRoot] = ch("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0);
  p.params[PSnap] = lin("Snap", "%", 0, 1, 1);
  // Shimmer
  p.params[PShFeedback] = lin("Shimmer feedback", "%", 0, 0.95, 0);
  p.params[PShInterval] = ch("Shimmer interval", {"+7", "+12", "+19"}, 1);
  p.params[PShTone] = lgp("Shimmer tone", "Hz", 1000, 16000, 5000);
  p.params[PShSpread] = lin("Shimmer spread", "%", 0, 1, 0.7);
  // Crystal
  p.params[PCrChord] = ch("Chord", {"Octaves", "Fifths", "Triad", "Sus", "Ninth"}, 1);
  p.params[PCrDecay] = lgp("Crystal decay", "s", 0.05, 8, 1.8);
  p.params[PCrBright] = lin("Brightness", "%", 0, 1, 0.6);
  p.params[PCrStrike] = lin("Strike", "%", 0, 1, 0.45);
  p.params[PCrSpread] = lin("Crystal spread", "%", 0, 1, 0.3);
  p.params[PCrMix] = lin("Crystal mix", "%", 0, 1, 0);
  // Halo
  p.params[PHaLift] = lin("Halo lift", "%", 0, 1, 0);
  p.params[PHaFrom] = lgp("Halo from", "Hz", 1000, 12000, 4200);
  p.params[PHaTwinkle] = lin("Twinkle", "%", 0, 1, 0);
  p.params[PHaSmear] = lin("Smear", "%", 0, 1, 0);
  p.params[PHaFreeze] = tog("Freeze halo", 0);
  p.params[PHaMix] = lin("Halo mix", "%", 0, 1, 1);
  // Grit
  p.params[PGtBits] = lin("Bits", "bits", 2, 24, 24);
  p.params[PGtRate] = lgp("Rate", "Hz", 500, 48000, 48000);
  p.params[PGtSmooth] = ch("Grit smooth", {"Raw", "Soft", "Clean"}, GsClean);
  p.params[PGtJitter] = lin("Jitter", "%", 0, 1, 0);
  p.params[PGtDrive] = lin("Drive", "dB", 0, 24, 0);
  p.params[PGtMix] = lin("Grit mix", "%", 0, 1, 1);
  // Spray
  p.params[PWidth] = lin("Width", "%", 0, 2, 1);
  p.params[PPanScatter] = lin("Pan scatter", "%", 0, 1, 0.5);
  p.params[PMicroDelay] = lin("Micro delay", "ms", 0, 30, 0);
  p.params[POrbit] = ch("Orbit", {"Off", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"}, 4);
  // Room
  p.params[PRmSize] = lin("Room size", "%", 0, 1, 0.7);
  p.params[PRmDecay] = lgp("Room decay", "s", 0.2, 20, 4);
  p.params[PRmDamping] = lin("Damping", "%", 0, 1, 0.4);
  p.params[PRmMix] = lin("Room mix", "%", 0, 1, 0);
  // Echo
  p.params[PEcTime] = ch("Echo time", {"1/32", "1/16", "1/8T", "3/32", "1/8", "3/16", "1/4T", "1/4", "3/8", "1/2", "1 bar"}, 5);
  p.params[PEcFeedback] = lin("Echo feedback", "%", 0, 0.95, 0.35);
  p.params[PEcTone] = lgp("Echo tone", "Hz", 500, 12000, 3000);
  p.params[PEcMix] = lin("Echo mix", "%", 0, 1, 0);
  p.params[PEcPingPong] = tog("Ping-pong", 1);
  p.params[PEcDiffuse] = lin("Echo diffuse", "%", 0, 1, 0.3);
  p.params[PRmShimmer] = lin("Room shimmer", "%", 0, 1, 0);
  p.params[PRmFreeze] = tog("Freeze space", 0);
  p.params[PRmMod] = lin("Room mod", "%", 0, 1, 0.35);
  // Tone and output
  p.params[PLowCut] = lgp("Low cut", "Hz", 20, 1000, 20);
  p.params[PHighCut] = lgp("High cut", "Hz", 1000, 20000, 20000);
  p.params[PTilt] = lin("Tilt", "dB", -6, 6, 0);
  p.params[PDuck] = lin("Duck", "%", 0, 1, 0);
  p.params[POutput] = lin("Output", "dB", -24, 12, 0);
  p.params[PSafety] = tog("Safety limit", 1);
  // Sways and the Follower
  const std::vector<std::string> shapes = {"Sine", "Tri", "Ramp", "Steps", "Drift"};
  const std::vector<std::string> rates = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"};
  for (int s = 0; s < 2; ++s) {
    const std::string n = s == 0 ? "Sway A " : "Sway B ";
    p.params[static_cast<size_t>(swayParam(s, SwShape))] = ch(n + "shape", shapes, s == 0 ? ShSine : ShSteps);
    p.params[static_cast<size_t>(swayParam(s, SwRate))] = ch(n + "rate", rates, s == 0 ? 4 : 6);
    p.params[static_cast<size_t>(swayParam(s, SwDepth))] = lin(n + "depth", "%", 0, 1, 1);
    p.params[static_cast<size_t>(swayParam(s, SwPhase))] = lin(n + "phase", "deg", 0, 360, s == 0 ? 0 : 90);
    p.params[static_cast<size_t>(swayParam(s, SwSmooth))] = lin(n + "smooth", "%", 0, 1, s == 0 ? 0.1 : 0.4);
  }
  p.params[PFoAttack] = lgp("Follower attack", "ms", 0.1, 100, 5);
  p.params[PFoRelease] = lgp("Follower release", "ms", 10, 2000, 180);
  p.params[PFoGain] = lin("Follower gain", "dB", -12, 24, 0);
  p.params[PFoFloor] = lin("Follower floor", "dB", -60, -12, -48);
  for (int l = 0; l < kLinks; ++l) {
    const std::string n = "Link " + std::to_string(l + 1) + " ";
    p.params[static_cast<size_t>(linkParam(l, LkSource))] = ch(n + "source", {"Off", "Sway A", "Sway B", "Follower", "Breaks"}, SrcOff);
    p.params[static_cast<size_t>(linkParam(l, LkTarget))] = lin(n + "target", "", 0, PCount - 1, 0);
    p.params[static_cast<size_t>(linkParam(l, LkDepth))] = lin(n + "depth", "%", -1, 1, 0);
  }
  // settings
  p.params[PQuality] = ch("Quality", {"Light", "Normal", "High"}, QNormal);
  p.params[PHaloEngine] = ch("Halo engine", {"Off", "Low", "Full"}, HaloLow);
  p.params[PCaptureUpTo] = ch("Capture up to", {"2 bars", "4 bars", "8 bars"}, 1);
  p.params[PTempoMode] = ch("Tempo", {"Song", "Free"}, 0);
  p.params[PFreeBpm] = lin("Free tempo", "BPM", 40, 240, 120);
  p.params[PRestart] = tog("Restart on play", 1);
  p.params[PKeepVibe] = tog("Dice keeps the vibe", 1);
  for (int m = 0; m < kLockMasks; ++m) p.params[static_cast<size_t>(PLock0 + m)] = lin("Dice locks " + std::to_string(m + 1), "", 0, 16777215, 0);
  p.params[PVersion] = lin("Version", "", 0, 100, kVersion);
  p.editorWidth = 1100;
  p.ownPresetBar = true;

  // ---- factory presets (original). Init first, then eight banks of five, by source.
  p.presets.push_back({"Init", {}});
  // Pads
  p.presets.push_back({"Cloud Bed", macros(30, 55, 30, 15, 20, 0, 70, 50) + V{{PRmDecay, 6.0f}, {PRmSize, 0.8f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Velvet Fog", macros(15, 80, 50, 20, 10, 0, 50, 50) + V{{PWarmth, 0.2f}, {PDensity, 0.03f}}});
  p.presets.push_back({"Polar Glow", macros(40, 40, 20, 10, 40, 0, 85, 50) + V{{PRmShimmer, 0.3f}, {PRmDecay, 8.0f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Slow Bloom", macros(25, 50, 25, 55, 20, 0, 65, 50) + odds(10, 90, 30, 0, 0, 0) +
                                         V{{PGrid, 1}, {PSpan, 3}, {PSoftness, 0.6f}, {PWash, 0.4f}}});
  p.presets.push_back({"Faded Choir", macros(20, 50, 60, 10, 50, 0, 55, 50) + V{{PEnsemble, 0.3f}, {PCrChord, 2}, {PCrDecay, 4.0f}, {PDensity, 0.02f}}});
  // Vocals
  p.presets.push_back({"Whisper Halo", macros(20, 35, 15, 0, 55, 0, 45, 40) + V{{PHaLift, 0.2f}, {PHaFrom, 6000}, {PEcMix, 0.15f}, {PDensity, 0.0f}}});
  p.presets.push_back({"Dream Double", macros(10, 35, 70, 0, 10, 0, 35, 45) + V{{PEnsemble, 0.3f}, {PWow, 0.1f}, {PDensity, 0.0f}, {PWidth, 1.6f}}});
  p.presets.push_back({"Angel Trail", macros(45, 35, 15, 0, 25, 0, 75, 40) +
                                          V{{PShInterval, 0}, {PRmShimmer, 0.35f}, {PDensity, 0.0f}, {PIntervals, chips({12, 19})}}});
  p.presets.push_back({"Lullaby Tape", macros(15, 55, 55, 45, 10, 0, 45, 45) + odds(0, 80, 40, 0, 0, 0) + V{{PGrid, 1}, {PSpan, 3}, {PDensity, 0.03f}}});
  p.presets.push_back({"Echo Diary", macros(10, 40, 20, 0, 10, 0, 40, 40) +
                                         V{{PEcMix, 0.45f}, {PEcFeedback, 0.5f}, {PEcDiffuse, 0.6f}, {PEcTime, 7}, {PDensity, 0.0f}}});
  // Keys and piano
  p.presets.push_back({"Music Box Memory", macros(60, 40, 55, 15, 30, 0, 45, 50) + V{{PIntervals, chips({12, 24})}, {PDensity, 0.02f}}});
  p.presets.push_back({"Felt Haze", macros(10, 85, 35, 10, 0, 0, 35, 55) + V{{PDamp, 6000}, {PDensity, 0.02f}}});
  p.presets.push_back({"Glass Lullaby", macros(25, 35, 20, 10, 70, 0, 50, 45) + V{{PCrChord, 1}, {PCrDecay, 3.0f}, {PCrStrike, 0.6f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Old Upright", macros(5, 50, 80, 15, 0, 0, 25, 55) + V{{PRmSize, 0.3f}, {PRmDecay, 1.2f}, {PCrackle, 0.15f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Starlit Keys", macros(55, 30, 20, 10, 50, 0, 55, 45) + V{{PHaTwinkle, 0.3f}, {PDensity, 0.02f}}});
  // Guitars and plucks
  p.presets.push_back({"Lake Shimmer", macros(35, 35, 20, 10, 25, 0, 80, 45) + V{{PRmShimmer, 0.25f}, {PRmDecay, 6.0f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Harp Mist", macros(20, 60, 20, 20, 15, 0, 60, 50) + odds(100, 0, 0, 0, 0, 0) +
                                        V{{PDensity, 0.06f}, {PDivision, 2}, {PGrid, 2}, {PSpan, 1}, {PSoftness, 0.5f}, {PWash, 0.5f}}});
  p.presets.push_back({"Worn Strings", macros(5, 60, 75, 10, 0, 0, 30, 55) + V{{PDamp, 5000}, {PDensity, 0.02f}}});
  p.presets.push_back({"Pluck Comets", macros(70, 30, 15, 10, 20, 0, 55, 45) + V{{PEcMix, 0.25f}, {PEcTime, 5}, {PGrScatter, 0.8f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Tremolo Dust", macros(45, 45, 25, 10, 20, 0, 45, 50) + link(0, SrcSwayA, PDamp, -0.35f) + V{{PDensity, 0.02f}}});
  // Drums and loops
  p.presets.push_back({"Soft Shatter", macros(15, 50, 20, 15, 0, 25, 35, 60) + odds(80, 20, 10, 5, 10, 0) +
                                           V{{PSoftness, 0.6f}, {PWash, 0.3f}, {PDensity, 0.12f}, {PGrid, 3}}});
  p.presets.push_back({"Dusty Breaks", macros(0, 55, 70, 40, 0, 10, 25, 70) + odds(30, 10, 80, 10, 0, 0) + V{{PCrackle, 0.25f}, {PDensity, 0.08f}, {PGrid, 2}}});
  p.presets.push_back({"Tape Swell Loop", macros(10, 50, 30, 70, 0, 0, 45, 60) + odds(10, 70, 70, 10, 0, 0) +
                                              V{{PGrid, 1}, {PSpan, 3}, {PLean, LeanBarEnd}, {PDensity, 0.15f}}});
  p.presets.push_back({"Fairy Stutter", macros(50, 40, 15, 10, 20, 20, 40, 60) + odds(90, 20, 0, 0, 10, 0) + V{{PDivision, 4}, {PDensity, 0.1f}, {PGrid, 3}}});
  p.presets.push_back({"Moon Groove", macros(15, 60, 20, 20, 10, 0, 60, 45) + V{{PDensity, 0.03f}, {PWidth, 1.6f}}});
  // Ambient and textures
  p.presets.push_back({"Frozen Lake", macros(30, 40, 20, 10, 40, 0, 95, 55) + V{{PRmDecay, 18.0f}, {PRmShimmer, 0.35f}, {PRmMod, 0.6f}, {PDensity, 0.0f}}});
  p.presets.push_back({"Aurora Field", macros(35, 30, 15, 30, 55, 0, 75, 55) + link(0, SrcSwayB, PHaLift, 0.5f) +
                                           V{{POrbit, 7}, {PDensity, 0.0f}, {PHaTwinkle, 0.25f}}});
  p.presets.push_back({"Deep Nebula", macros(25, 70, 25, 10, 40, 0, 95, 60) + V{{PRmDecay, 20.0f}, {PRmShimmer, 0.5f}, {PDensity, 0.0f}}});
  p.presets.push_back({"Glass Rain", macros(75, 25, 10, 10, 30, 0, 70, 55) +
                                         V{{PScale, 1}, {PGrReverse, 0.5f}, {PIntervals, chips({0, 7, 12, 19, 24})}, {PDensity, 0.0f}}});
  p.presets.push_back({"Northern Hum", macros(10, 75, 60, 20, 20, 0, 75, 60) + V{{PEnsemble, 0.4f}, {PDamp, 4000}, {PDensity, 0.0f}}});
  // Lo-fi memories
  p.presets.push_back({"Cassette Sunday", macros(5, 55, 80, 10, 0, 0, 25, 60) + V{{PHiss, 0.25f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Faded Polaroid", macros(15, 65, 65, 35, 0, 0, 35, 60) + V{{PAge, 0.2f}, {PDensity, 0.04f}}});
  p.presets.push_back({"Attic Radio", macros(0, 70, 70, 10, 0, 0, 20, 70) +
                                          V{{PLowCut, 350}, {PHighCut, 4500}, {PWidth, 0.3f}, {PCrackle, 0.35f}, {PDensity, 0.02f}}});
  p.presets.push_back({"Summer Reel", macros(10, 50, 70, 50, 0, 0, 30, 60) + odds(0, 10, 100, 0, 0, 0) +
                                          V{{PLean, LeanBarEnd}, {PGrid, 1}, {PSpan, 3}, {PDensity, 0.12f}, {PTapeTime, 3}}});
  p.presets.push_back({"Home Video", macros(10, 50, 75, 40, 0, 0, 35, 60) + odds(0, 90, 20, 0, 0, 0) + V{{PFlutter, 0.3f}, {PHiss, 0.15f}}});
  // Glitch: the sharp character
  p.presets.push_back({"Broken Clock", macros(10, 0, 0, 10, 0, 70, 15, 100) + sharp() + odds(90, 10, 5, 5, 15, 5) +
                                           V{{PLean, LeanBarEnd}, {PGrid, 3}, {PDensity, 0.25f}, {PAccel, 0.6f}, {PSpan, 2}, {PVariation, 0.3f}}});
  p.presets.push_back({"Shredder", macros(15, 0, 0, 20, 0, 90, 10, 100) + sharp() +
                                       V{{PDensity, 0.5f}, {PVariation, 0.7f}, {PGrid, 4}, {PSpan, 1}, {PTrigMode, TrigBoth}, {PSensitivity, 0.6f}}});
  p.presets.push_back({"Signal Loss", macros(0, 0, 0, 15, 0, 75, 0, 100) + sharp() + odds(40, 5, 10, 0, 20, 70) +
                                          V{{PGtBits, 6}, {PGtRate, 6000}, {PGtSmooth, GsRaw}, {PDensity, 0.2f}, {PGrid, 3}}});
  p.presets.push_back({"Arcade Sparks", macros(20, 0, 0, 0, 0, 80, 10, 100) + sharp() +
                                            V{{PGtBits, 8}, {PGtSmooth, GsRaw}, {PGrWindow, WinClick}, {PDensity, 0.2f}}});
  p.presets.push_back({"Hiccup", macros(0, 0, 0, 0, 0, 55, 0, 100) + sharp() + odds(70, 0, 0, 0, 0, 0) +
                                     V{{PLean, LeanOff}, {PGrid, 2}, {PDensity, 0.3f}, {PDivision, 4}, {PSpan, 0}, {PVariation, 0.1f}}});
  v.push_back(std::move(p));
}

}  // namespace gl::packs::chater
