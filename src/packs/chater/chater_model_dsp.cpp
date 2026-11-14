// Gridline - "chater" pack: the model shared by the engine and the editor: choice tables,
// the macro mappings, value display, the dice and the preset notes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../Packs.h"
#include "chater.h"

namespace gl::packs::chater {

// ------------------------------------------------------------------ tables

double gridBeats(int i) {
  static const double t[kGridCount] = {2, 1, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0};
  return t[std::clamp(i, 0, kGridCount - 1)];
}
double divisionBeats(int i) {
  static const double t[kDivisionCount] = {2, 1, 0.5, 0.25, 0.125, 0.0625, 0.03125, 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0};
  return t[std::clamp(i, 0, kDivisionCount - 1)];
}
double lengthBars(int i) {
  static const double t[6] = {0.25, 0.5, 1, 2, 4, 8};
  return t[std::clamp(i, 0, 5)];
}
double quantiseBeats(int i) {
  static const double t[8] = {1, 0.5, 0.25, 0.125, 0.0625, 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0};
  return t[std::clamp(i, 0, 7)];
}
double tapeBeats(int i) {
  static const double t[6] = {0.25, 0.5, 1, 2, 4, 8};
  return t[std::clamp(i, 0, 5)];
}
double scrubBeats(int i) {
  static const double t[6] = {0.125, 0.25, 0.5, 1, 2, 4};
  return t[std::clamp(i, 0, 5)];
}
double echoBeats(int i) {
  static const double t[11] = {0.125, 0.25, 1.0 / 3.0, 0.375, 0.5, 0.75, 2.0 / 3.0, 1, 1.5, 2, 4};
  return t[std::clamp(i, 0, 10)];
}
double orbitBeats(int i) {
  static const double t[8] = {0, 0.25, 0.5, 1, 2, 4, 8, 16};
  return t[std::clamp(i, 0, 7)];
}
double swayBeats(int i) {
  static const double t[9] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32};
  return t[std::clamp(i, 0, 8)];
}
int spanSteps(int i) {
  static const int t[6] = {1, 2, 3, 4, 6, 8};
  return t[std::clamp(i, 0, 5)];
}
int captureBars(int i) {
  static const int t[3] = {2, 4, 8};
  return t[std::clamp(i, 0, 2)];
}
int chordSemis(int chord, int voice) {
  static const int t[5][6] = {{-12, 0, 12, 24, 36, 48}, {0, 7, 12, 19, 24, 31}, {0, 4, 7, 12, 16, 19}, {0, 5, 7, 12, 17, 19}, {0, 4, 7, 10, 14, 19}};
  return t[std::clamp(chord, 0, 4)][std::clamp(voice, 0, 5)];
}
int shimmerSemis(int i) {
  static const int t[3] = {7, 12, 19};
  return t[std::clamp(i, 0, 2)];
}
int scaleMask(int scale) {
  // bit n = semitone n above the root
  static const int t[9] = {0,
                           0b101010110101,   // major 0 2 4 5 7 9 11
                           0b010110101101,   // natural minor 0 2 3 5 7 8 10
                           0b001010010101,   // pentatonic major 0 2 4 7 9
                           0b010010101001,   // pentatonic minor 0 3 5 7 10
                           0b011010101101,   // dorian 0 2 3 5 7 9 10
                           0b101011010101,   // lydian 0 2 4 6 7 9 11
                           0b010101010101,   // whole tone
                           0b111111111111};  // chromatic
  return t[std::clamp(scale, 0, 8)];
}

// ------------------------------------------------------------------ macros

const std::vector<MacroRow>& macroRows() {
  static const std::vector<MacroRow> rows = {
      // Sparkle: soft, twinkly grains above the source
      {PSparkle, PGrDensity, MkAdd, 60, McExp2, 0, "Grain density"},
      {PSparkle, PGrLevel, MkAdd, 0.7, McLin, 0, "Glitter level"},
      {PSparkle, PReach, MkAdd, 0.5, McLin, 0, "Pitch reach"},
      {PSparkle, PShFeedback, MkAdd, 0.5, McExp2, 0.3, "Shimmer feedback"},
      {PSparkle, PHaTwinkle, MkAdd, 0.3, McLin, 0, "Halo twinkle"},
      // Haze: warm, damp, soft edges, breaks washed into the space
      {PHaze, PDamp, MkToward, 2500, McLin, 0, "Damp"},
      {PHaze, PWarmth, MkAdd, 0.6, McLin, 0, "Warmth"},
      {PHaze, PSoftness, MkAdd, 0.5, McLin, 0, "Soft edges"},
      {PHaze, PWash, MkAdd, 0.6, McLin, 0, "Wash"},
      {PHaze, PRmDamping, MkAdd, 0.35, McLin, 0, "Room damping"},
      // Memory: a worn tape memory of the sound
      {PMemory, PWow, MkAdd, 0.7, McLin, 0, "Wow"},
      {PMemory, PFlutter, MkAdd, 0.45, McLin, 0, "Flutter"},
      {PMemory, PAge, MkAdd, 0.8, McLin, 0, "Tape age"},
      {PMemory, PEnsemble, MkAdd, 0.6, McLin, 0, "Ensemble"},
      {PMemory, PHiss, MkAdd, 0.25, McExp2, 0.5, "Hiss"},
      // Drift: slow, gentle breaks: tape slowing, reverse swells, falling pitch
      {PDrift, PDensity, MkAdd, 0.12, McLin, 0, "Grid density"},
      {PDrift, POddTape, MkAdd, 60, McLin, 0, "Tape odds"},
      {PDrift, POddReverse, MkAdd, 50, McLin, 0, "Reverse odds"},
      {PDrift, PRamp, MkAdd, -7, McLin, 0, "Pitch ramp"},
      {PDrift, PScrubDepth, MkAdd, 0.4, McLin, 0, "Scrub depth"},
      {PDrift, PDetune, MkAdd, 25, McLin, 0, "Detune"},
      {PDrift, -1, MkInner, 40, McExp2, 0, "Grain wobble (Sway A)"},
      // Shine: crystal and halo
      {PShine, PCrMix, MkAdd, 0.7, McExp2, 0, "Crystal mix"},
      {PShine, PCrDecay, MkAdd, 3, McLin, 0, "Crystal decay"},
      {PShine, PHaLift, MkAdd, 0.8, McLin, 0, "Halo lift"},
      {PShine, PHaTwinkle, MkAdd, 0.3, McLin, 0, "Halo twinkle"},
      {PShine, PTilt, MkAdd, 2, McLin, 0, "Tilt"},
      // Glitch: the sharp character (busier, finer, crushed, hard edges)
      {PGlitch, PDensity, MkAdd, 0.45, McLin, 0, "Grid density"},
      {PGlitch, POddRepeat, MkAdd, 60, McLin, 0, "Repeat odds"},
      {PGlitch, PVariation, MkAdd, 0.5, McLin, 0, "Variation"},
      {PGlitch, PDivision, MkSteps, 3, McLin, 0, "Division"},
      {PGlitch, POddJump, MkAdd, 40, McLin, 0, "Jump odds"},
      {PGlitch, POddSkip, MkAdd, 40, McLin, 0, "Skip odds"},
      {PGlitch, PGate, MkAdd, -0.4, McLin, 0, "Gate"},
      {PGlitch, PSoftness, MkMul, -0.9, McLin, 0, "Soft edges"},
      {PGlitch, PWash, MkMul, -0.8, McLin, 0, "Wash"},
      {PGlitch, PAccel, MkAdd, 0.4, McLin, 0.5, "Accel"},
      {PGlitch, PGtBits, MkToward, 6, McExp2, 0.5, "Bits"},
      {PGlitch, PGtRate, MkToward, 4000, McLin, 0.5, "Rate"},
      // Space: a big, lush room, shimmer, echoes
      {PSpace, PRmMix, MkAdd, 0.7, McLin, 0, "Room mix"},
      {PSpace, PRmDecay, MkAdd, 6, McLin, 0, "Room decay"},
      {PSpace, PRmSize, MkAdd, 0.3, McLin, 0, "Room size"},
      {PSpace, PRmShimmer, MkAdd, 0.45, McLin, 0.55, "Room shimmer"},
      {PSpace, PEcMix, MkAdd, 0.3, McExp2, 0, "Echo mix"},
      {PSpace, PEcFeedback, MkAdd, 0.3, McLin, 0, "Echo feedback"},
      {PSpace, PEcDiffuse, MkAdd, 0.5, McLin, 0, "Echo diffuse"},
      {PSpace, PWidth, MkAdd, 0.5, McLin, 0, "Width"},
      {PSpace, PPanScatter, MkAdd, 0.5, McLin, 0, "Pan scatter"},
      {PMix, PMix, MkInner, 1, McLin, 0, "Dry and wet"},
  };
  return rows;
}

double macroAmount(const MacroRow& r, double m) {
  m = std::clamp(m, 0.0, 1.0);
  if (r.gate > 0) m = m <= r.gate ? 0.0 : (m - r.gate) / (1.0 - r.gate);
  return r.curve == McExp2 ? m * m : m;
}

namespace {
const ParamDesc* desc(int k) {
  static const PluginInfo* info = findPlugin(kType);
  return info && k >= 0 && static_cast<size_t>(k) < info->params.size() ? &info->params[static_cast<size_t>(k)] : nullptr;
}
// Rows per parameter (built once).
const std::vector<std::vector<int>>& rowsByParam() {
  static const std::vector<std::vector<int>> by = [] {
    std::vector<std::vector<int>> v(PCount);
    const auto& rows = macroRows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].param >= 0 && rows[i].kind != MkInner) v[static_cast<size_t>(rows[i].param)].push_back(static_cast<int>(i));
    return v;
  }();
  return by;
}
}  // namespace

double applyMacros(int param, double page, const double* macros) {
  if (param < 0 || param >= PCount) return page;
  const auto& idx = rowsByParam()[static_cast<size_t>(param)];
  if (idx.empty()) return page;
  const auto& rows = macroRows();
  double v = page;
  for (int i : idx) {
    const MacroRow& r = rows[static_cast<size_t>(i)];
    const double a = macroAmount(r, macros[r.macro]);
    if (a <= 0) continue;
    switch (r.kind) {
      case MkAdd: v += a * r.span; break;
      case MkMul: v *= 1.0 + a * r.span; break;
      case MkToward:
        if (param == PGtRate || param == PDamp) v = std::exp(std::log(std::max(1.0, v)) + (std::log(r.span) - std::log(std::max(1.0, v))) * a);  // log domain
        else v += (r.span - v) * a;
        break;
      case MkSteps: v += std::round(a * r.span); break;
      default: break;
    }
  }
  if (const ParamDesc* d = desc(param)) {
    if (d->kind == ParamKind::Choice) {
      // Division: Shatter steps finer through the straight values only
      if (param == PDivision && page < kDivisionStraight) return std::clamp(v, 0.0, static_cast<double>(kDivisionStraight - 1));
      return d->clamp(v);
    }
    return std::clamp(v, d->min, d->max);
  }
  return v;
}

bool needsMigration(const float* p, size_t n) { return n > static_cast<size_t>(PVersion) && p[PVersion] < 1.5f; }

void migrateV1(float* p, size_t n) {
  if (!needsMigration(p, n)) return;
  const float oldGlitch = p[1], oldShatter = p[2];
  p[PGlitch] = std::max(oldGlitch, oldShatter);  // the old Grit tile has no own macro any more
  p[PHaze] = 0, p[PMemory] = 0;
  p[PDamp] = 20000, p[PWarmth] = 0, p[PSoftness] = 0, p[PWash] = 0;
  p[PWow] = p[PFlutter] = p[PAge] = p[PEnsemble] = p[PHiss] = p[PCrackle] = 0;
  p[PWowRate] = 0.5f;
  p[PRmShimmer] = 0, p[PRmFreeze] = 0, p[PRmMod] = 0, p[PEcDiffuse] = 0;
  p[PVersion] = static_cast<float>(kVersion);
}

double wobbleCents(const double* macros) {
  for (const auto& r : macroRows())
    if (r.kind == MkInner && r.macro == PDrift) return macroAmount(r, macros[PDrift]) * r.span;
  return 0;
}

// ------------------------------------------------------------------ display

const char* shortLabel(int k) {
  switch (k) {
    case PLength: return "Length";
    case PQuantise: return "Quantise";
    case PSmooth: return "Smooth";
    case PHold: return "Hold";
    case PTrigMode: return "Trigger";
    case PSensitivity: return "Sensitivity";
    case PHitChance: return "Hit chance";
    case POddRepeat: return "Repeat";
    case POddReverse: return "Reverse";
    case POddTape: return "Tape";
    case POddScrub: return "Scrub";
    case POddJump: return "Jump";
    case POddSkip: return "Skip";
    case PTapeCurve: return "Curve";
    case PGrDensity: return "Density";
    case PGrSize: return "Size";
    case PGrReverse: return "Reverse";
    case PGrLevel: return "Level";
    case PShFeedback: return "Feedback";
    case PShInterval: return "Interval";
    case PShTone: return "Tone";
    case PShSpread: case PCrSpread: return "Spread";
    case PCrDecay: case PRmDecay: return "Decay";
    case PCrMix: case PHaMix: case PGtMix: case PRmMix: case PEcMix: return "Mix";
    case PHaLift: return "Lift";
    case PHaFrom: return "From";
    case PHaFreeze: return "Freeze";
    case PGtSmooth: return "Smooth";
    case PRmSize: return "Size";
    case PEcTime: return "Time";
    case PEcFeedback: return "Feedback";
    case PEcTone: return "Tone";
    case PMix: return "Dry/Wet";
    case PFoAttack: return "Attack";
    case PFoRelease: return "Release";
    case PFoGain: return "Gain";
    case PFoFloor: return "Floor";
    case PDamp: return "Damp";
    case PWarmth: return "Warmth";
    case PSoftness: return "Soft edges";
    case PWash: return "Wash";
    case PWow: return "Wow";
    case PFlutter: return "Flutter";
    case PAge: return "Age";
    case PEnsemble: return "Ensemble";
    case PHiss: return "Hiss";
    case PCrackle: return "Crackle";
    case PWowRate: return "Wow rate";
    case PRmShimmer: return "Shimmer";
    case PRmMod: return "Mod";
    case PEcDiffuse: return "Diffuse";
    default: break;
  }
  if (k >= PSwA && k < PSwB + SwCount) {
    static const char* n[SwCount] = {"Shape", "Rate", "Depth", "Phase", "Smooth"};
    return n[(k - PSwA) % SwCount];
  }
  const ParamDesc* d = desc(k);
  return d ? d->name.c_str() : "";
}

std::string display(int k, double v) {
  const ParamDesc* d = desc(k);
  if (!d) return "";
  v = d->clamp(v);
  char b[48];
  if (d->kind == ParamKind::Choice || d->kind == ParamKind::Toggle) return d->format(v);
  const std::string& u = d->unit;
  if (u == "%") {
    if (d->min < 0) std::snprintf(b, sizeof b, "%+.0f%%", v * 100.0);
    else std::snprintf(b, sizeof b, "%.0f%%", v * 100.0);
  } else if (u == "pct") {
    std::snprintf(b, sizeof b, "%.0f", v);
  } else if (u == "Hz" && v < 20) {
    std::snprintf(b, sizeof b, "%.2f Hz", v);
  } else if (u == "Hz") {
    if (k == PDamp && v >= 19900) return "Off";
    if (v >= 10000) std::snprintf(b, sizeof b, "%.0f kHz", v / 1000.0);
    else if (v >= 1000) std::snprintf(b, sizeof b, "%.1f kHz", v / 1000.0);
    else std::snprintf(b, sizeof b, "%.0f Hz", v);
  } else if (u == "s") {
    if (v < 1) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0);
    else std::snprintf(b, sizeof b, "%.1f s", v);
  } else if (u == "ms") {
    if (v >= 1000) std::snprintf(b, sizeof b, "%.1f s", v / 1000.0);
    else if (v < 10) std::snprintf(b, sizeof b, "%.1f ms", v);
    else std::snprintf(b, sizeof b, "%.0f ms", v);
  } else if (u == "dB") {
    if (k == POutput) std::snprintf(b, sizeof b, "%.1f dB", v);
    else if (k == PFoFloor) std::snprintf(b, sizeof b, "%.0f dB", v);
    else if (k == PTilt) std::snprintf(b, sizeof b, "%+.1f dB", v);
    else std::snprintf(b, sizeof b, "%+.0f dB", v);
    if (k == PGtDrive) std::snprintf(b, sizeof b, "%.0f dB", v);
  } else if (u == "st") {
    std::snprintf(b, sizeof b, "%+.0f st", v);
  } else if (u == "ct") {
    std::snprintf(b, sizeof b, "%.0f ct", v);
  } else if (u == "bits") {
    std::snprintf(b, sizeof b, "%.0f", v);
  } else if (u == "/s") {
    std::snprintf(b, sizeof b, "%.0f /s", v);
  } else if (u == "deg") {
    std::snprintf(b, sizeof b, "%.0f\xC2\xB0", v);
  } else if (u == "BPM") {
    std::snprintf(b, sizeof b, "%.0f BPM", v);
  } else {
    return d->format(v);
  }
  return b;
}

const std::vector<int>& linkTargets() {
  static const std::vector<int> t = [] {
    std::vector<int> v;
    const int order[] = {PSparkle, PHaze, PMemory, PDrift, PShine, PGlitch, PSpace, PMix, PDensity, PVariation, PSensitivity, PHitChance, PSmooth,
                         POddRepeat, POddReverse, POddTape, POddScrub, POddJump, POddSkip, PAccel, PRamp, PGate, PTapeCurve, PScrubDepth,
                         PGrDensity, PGrSize, PGrScatter, PGrReverse, PGrLevel, PReach, PDetune, PSnap, PShFeedback, PShTone, PShSpread,
                         PCrDecay, PCrBright, PCrStrike, PCrSpread, PCrMix, PHaLift, PHaFrom, PHaTwinkle, PHaSmear, PHaMix, PGtBits, PGtRate,
                         PGtJitter, PGtDrive, PGtMix, PWidth, PPanScatter, PMicroDelay, PRmSize, PRmDecay, PRmDamping, PRmMix, PEcFeedback,
                         PEcTone, PEcMix, PLowCut, PHighCut, PTilt, PDuck, PDamp, PWarmth, PSoftness, PWash, PWow, PFlutter, PAge, PEnsemble,
                         PHiss, PCrackle, PWowRate, PRmShimmer, PRmMod, PEcDiffuse};
    for (int k : order) v.push_back(k);
    return v;
  }();
  return t;
}

int pageOf(int k) {
  if (k < PLength) return 0;
  if ((k >= PDamp && k <= PEnsemble) || (k >= PHiss && k <= PWowRate)) return 7;  // the Haze page
  if (k >= PRmShimmer && k <= PEcDiffuse) return 4;
  if (k < PGrDensity) return 1;
  if (k < PCrChord) return 2;
  if (k < PWidth) return 3;
  if (k < PSwA) return 4;
  if (k < PQuality) return 5;
  return 6;
}

// ------------------------------------------------------------------ presets

const char* bankName(int b) {
  static const char* n[kBanks] = {"Pads", "Vocals", "Keys", "Plucks", "Drums", "Ambient", "Lo-fi", "Glitch"};
  return n[std::clamp(b, 0, kBanks - 1)];
}

const std::vector<PresetMeta>& presetMeta() {
  static const std::vector<PresetMeta> m = {
      {"Init", -1, "Dreamy and safe on any source: a starting point", "init dreamy"},
      // Pads
      {"Cloud Bed", 0, "Soft haze and a long hall under a slow pad", "pad hall haze"},
      {"Velvet Fog", 0, "Dark, damp warmth with gentle tape wow", "pad dark warm wow"},
      {"Polar Glow", 0, "Octave shimmer rising out of a long hall", "pad shimmer hall"},
      {"Slow Bloom", 0, "Reverse swells that bloom into the room", "pad reverse swell"},
      {"Faded Choir", 0, "Worn ensemble voices and a quiet crystal ring", "pad ensemble crystal"},
      // Vocals
      {"Whisper Halo", 1, "Airy halo and soft echoes around a voice", "vocal air halo echo"},
      {"Dream Double", 1, "A wobbly tape double with a wide ensemble", "vocal double ensemble wow"},
      {"Angel Trail", 1, "Fifth and octave shimmer trailing each phrase", "vocal shimmer fifth octave"},
      {"Lullaby Tape", 1, "Warm worn tape with slow reverse swells", "vocal tape reverse warm"},
      {"Echo Diary", 1, "Diffuse ping-pong echoes that fade softly", "vocal echo diffuse"},
      // Keys and piano
      {"Music Box Memory", 2, "Sparkling grains and a worn tape memory", "keys piano sparkle tape"},
      {"Felt Haze", 2, "Muted, warm and close, with gentle wow", "keys piano muted warm"},
      {"Glass Lullaby", 2, "Crystal rings in fifths over soft keys", "keys piano crystal fifths"},
      {"Old Upright", 2, "Tape age, flutter and a small dusty room", "keys piano lofi room"},
      {"Starlit Keys", 2, "Twinkling halo and glitter above each chord", "keys piano twinkle glitter"},
      // Guitars and plucks
      {"Lake Shimmer", 3, "Plucks ripple into a shimmering hall", "guitar pluck shimmer hall"},
      {"Harp Mist", 3, "Soft repeats washed into a misty room", "guitar pluck harp repeat wash"},
      {"Worn Strings", 3, "Old tape, flutter and a dark high cut", "guitar pluck tape dark"},
      {"Pluck Comets", 3, "Sparkle trails flying off each note", "guitar pluck sparkle echo"},
      {"Tremolo Dust", 3, "A slow sway on the haze, fine glitter on top", "guitar pluck sway glitter"},
      // Drums and loops
      {"Soft Shatter", 4, "Gentle repeats with soft edges on the beat", "drums loop soft repeat"},
      {"Dusty Breaks", 4, "Worn tape, crackle and soft tape stops", "drums loop lofi tape crackle"},
      {"Tape Swell Loop", 4, "Reverse swells and slowdowns into a room", "drums loop reverse tape"},
      {"Fairy Stutter", 4, "Light stutters that sparkle and wash away", "drums loop stutter sparkle"},
      {"Moon Groove", 4, "Damp, wide and spacious, rare soft breaks", "drums loop space damp"},
      // Ambient and textures
      {"Frozen Lake", 5, "A very long hall with drifting shimmer", "ambient texture hall shimmer"},
      {"Aurora Field", 5, "Swaying halo and wide spray, no glitch", "ambient texture halo orbit"},
      {"Deep Nebula", 5, "A vast, dark hall with octave shimmer", "ambient texture dark shimmer"},
      {"Glass Rain", 5, "Pitched glitter falling through a long room", "ambient texture glitter scale"},
      {"Northern Hum", 5, "Ensemble and wow on a slow, dark wash", "ambient texture ensemble dark"},
      // Lo-fi memories
      {"Cassette Sunday", 6, "Wow, flutter, hiss and a soft high cut", "lofi cassette hiss wow"},
      {"Faded Polaroid", 6, "Washed-out tape tone and gentle breaks", "lofi faded tape breaks"},
      {"Attic Radio", 6, "Narrow, crackling and far away", "lofi radio crackle narrow"},
      {"Summer Reel", 6, "Warm wow and slow tape stops at bar ends", "lofi reel wow tape stop"},
      {"Home Video", 6, "Flutter, hiss and reverse swells", "lofi video flutter reverse"},
      // Glitch (the sharp character)
      {"Broken Clock", 7, "Repeats that speed up into each downbeat", "glitch repeat accel bar end"},
      {"Shredder", 7, "Fine divisions, jumps and skips at high odds", "glitch fine chaos hits"},
      {"Signal Loss", 7, "Skips and crushed repeats, a dropped stream", "glitch skip crush digital"},
      {"Arcade Sparks", 7, "Square-edged repeats at 8 bits", "glitch 8 bit square"},
      {"Hiccup", 7, "Short stutters on the off-beats", "glitch stutter offbeat"},
  };
  return m;
}

// ------------------------------------------------------------------ dice

const char* scopeName(int i) {
  static const char* n[kDiceScopes] = {"Macros", "Breaks", "Glitter", "Colour", "Space", "Motion"};
  return n[std::clamp(i, 0, kDiceScopes - 1)];
}

int diceScope(int k) {
  if (k >= PSparkle && k <= PMix) return ScMacros;
  if (k == PLength || k == PQuantise || k == PSmooth) return ScBreaks;
  if (k >= PGrid && k <= PSpan) return ScBreaks;
  if (k == PSensitivity || k == PHitChance) return ScBreaks;
  if (k >= POddRepeat && k <= POddSkip) return ScBreaks;
  if (k >= PDivision && k <= PScrubRate) return ScBreaks;
  if (k == PSoftness || k == PWash) return ScBreaks;
  if (k == PDamp || k == PWarmth || (k >= PWow && k <= PEnsemble) || k == PHiss || k == PCrackle || k == PWowRate) return ScColour;
  if (k == PRmShimmer || k == PRmMod || k == PEcDiffuse) return ScSpace;
  if ((k >= PGrDensity && k <= PSnap) || (k >= PShFeedback && k <= PShSpread)) return ScGlitter;
  if (k >= PCrChord && k <= PGtMix && k != PHaFreeze) return ScColour;
  if (k >= PWidth && k <= PEcPingPong) return ScSpace;
  if (k == PLowCut || k == PHighCut || k == PTilt || k == PDuck) return ScSpace;
  if (k >= PSwA && k < PSwB + SwCount) return ScMotion;
  if (k >= PFoAttack && k <= PFoFloor) return ScMotion;
  for (int l = 0; l < kLinks; ++l)
    if (k == linkParam(l, LkDepth)) return ScMotion;
  return 0;  // Output, Safety, Hold, Freeze, Seed, trigger mode, link routing, settings, locks
}

bool locked(const std::vector<float>& p, int k) {
  if (k < 0 || k >= PCount) return false;
  const size_t m = static_cast<size_t>(PLock0 + k / kLockBits);
  if (m >= p.size()) return false;
  const auto bits = static_cast<uint32_t>(std::max(0.0f, p[m]));
  return (bits >> (k % kLockBits)) & 1u;
}

void setLocked(std::vector<float>& p, int k, bool on) {
  if (k < 0 || k >= PCount) return;
  const size_t m = static_cast<size_t>(PLock0 + k / kLockBits);
  if (m >= p.size()) return;
  auto bits = static_cast<uint32_t>(std::max(0.0f, p[m]));
  if (on) bits |= 1u << (k % kLockBits);
  else bits &= ~(1u << (k % kLockBits));
  p[m] = static_cast<float>(bits);
}

namespace {
uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
double rnd(uint32_t seed, int k, int salt) { return (mix32(seed * 0x9E3779B9U ^ static_cast<uint32_t>(k * 7919 + salt * 104729)) >> 8) / 16777216.0; }
// The musical range of a parameter for New sound (normalised travel): keeps feedback,
// levels and the mix inside safe, useful bounds.
void musicalRange(int k, double& lo, double& hi) {
  lo = 0, hi = 1;
  switch (k) {
    case PMix: lo = 0.45; break;
    case PSparkle: case PHaze: case PMemory: case PDrift: case PShine: case PSpace: hi = 0.85; break;
    case PGlitch: hi = 0.6; break;
    case PHiss: hi = 0.3; break;
    case PCrackle: hi = 0.25; break;
    case PRmShimmer: hi = 0.6; break;
    case PShFeedback: case PEcFeedback: hi = 0.7; break;
    case PCrDecay: case PRmDecay: hi = 0.8; break;
    case PDensity: hi = 0.6; break;
    case PGtBits: lo = 0.2; break;
    case PGtRate: lo = 0.35; break;
    case PGtDrive: hi = 0.6; break;
    case PLowCut: hi = 0.5; break;
    case PHighCut: lo = 0.45; break;
    case PGrLevel: lo = 0.1; hi = 0.8; break;
    case PSmooth: lo = 0.3; break;
    case PGate: lo = 0.35; break;
    default: break;
  }
}
// Keeps a roll near a bank's character: the most a macro may reach (percent).
double bankCap(int bank, int k) {
  static const double caps[kBanks][7] = {
      // Sparkle Haze Memory Drift Shine Glitch Space
      {0.8, 1, 0.8, 0.6, 0.8, 0.15, 1},   // Pads
      {0.7, 0.9, 0.7, 0.5, 0.7, 0.15, 0.9},  // Vocals
      {0.9, 0.9, 0.8, 0.5, 0.9, 0.2, 0.9},   // Keys
      {0.9, 0.9, 0.8, 0.6, 0.8, 0.25, 0.9},  // Plucks
      {0.6, 0.9, 0.8, 0.8, 0.5, 0.5, 0.7},   // Drums
      {1, 1, 0.8, 0.7, 1, 0.15, 1},          // Ambient
      {0.5, 1, 1, 0.8, 0.4, 0.3, 0.7},       // Lo-fi
      {0.4, 0.3, 0.4, 0.6, 0.4, 1, 0.5},     // Glitch
  };
  if (bank < 0 || bank >= kBanks || k < PSparkle || k > PSpace) return 1;
  return caps[bank][k];
}
}  // namespace

std::vector<float> roll(const std::vector<float>& params, int scopes, int amount, uint32_t seed, int bank, int only) {
  std::vector<float> out = params;
  const PluginInfo* info = findPlugin(kType);
  if (!info) return out;
  if (out.size() < info->params.size()) out.resize(info->params.size());
  const bool vibe = bank >= 0 && out.size() > static_cast<size_t>(PKeepVibe) && out[PKeepVibe] >= 0.5f;
  for (int k = 0; k < PCount; ++k) {
    if (only >= 0 && k != only) continue;
    const int sc = diceScope(k);
    if (!sc || (only < 0 && !(scopes & sc))) continue;
    if (locked(out, k)) continue;
    const ParamDesc& d = info->params[static_cast<size_t>(k)];
    const double cur = d.toNormalized(d.clamp(out[static_cast<size_t>(k)]));
    double lo, hi;
    musicalRange(k, lo, hi);
    double n;
    if (amount == DiceNew) {
      n = lo + (hi - lo) * rnd(seed, k, 1);
    } else {
      const double w = amount == DiceNudge ? 0.1 : 0.3;
      n = cur + (rnd(seed, k, 1) * 2 - 1) * w;
      if (amount == DiceShake) n = std::clamp(n, std::min(lo, cur), std::max(hi, cur));
    }
    n = std::clamp(n, 0.0, 1.0);
    if (vibe && amount != DiceNew) n = std::min(n, std::max(cur, bankCap(bank, k)));
    double v = d.fromNormalized(n);
    if (d.kind == ParamKind::Choice) v = std::round(v);
    if (k == PIntervals) v = std::round(v) == 0 ? 2 : std::round(v);  // at least one chip lit
    out[static_cast<size_t>(k)] = static_cast<float>(d.clamp(v));
  }
  return out;
}

}  // namespace gl::packs::chater
