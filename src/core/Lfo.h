// Gridline - K-247: the LFO board (song-level LFOs, Song::lfos).
//
// An LFO belongs to the song, so it can move any parameter Gridline addresses: a built-in or installed
// plugin's parameter, a channel or mixer strip control, an effect slot's mix or switch, a macro (which
// then moves everything it drives) and another LFO's own settings. Each target is a ModLink whose
// source is the LFO (sourceSlot kSongLfoSlot). New targets swing around the knob (ModLink::mode
// kModAround) by 25 percent; "Between two values" (kModAbsolute) sweeps a range as controller links do.
//
// The shape functions here are pure and allocation-free: the engine (audio thread) and the board's
// pictures share them, so what the board draws is what plays.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "Params.h"

namespace gl {
class Document;
}

namespace gl::lfo {

// --- the rate --------------------------------------------------------------
inline constexpr int kRateCount = 10;
inline constexpr const char* kRateNames[kRateCount] = {"8 bars", "4 bars", "2 bars", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
// Quarter notes per cycle (bars of 4/4).
inline constexpr double kRateBeats[kRateCount] = {32, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 0.0625};
inline constexpr int kDefaultRateNote = 5;  // 1/4
inline constexpr float kMinHz = 0.01f, kMaxHz = 40.0f;
// Quarter notes per cycle of a tempo-synced LFO (dotted 1.5x, triplet 2/3).
double rateBeats(int rateNote, int rateKind);
inline double rateBeats(const SongLfo& l) { return rateBeats(l.rateNote, l.rateKind); }
// "1/8 D", "1/16 T", "4 bars", "0.21 Hz".
std::string rateLabel(const SongLfo& l);
// Cycles per second at `bpm`.
double rateHz(const SongLfo& l, double bpm);

// --- names -----------------------------------------------------------------
const char* shapeName(LfoShape s);       // "Sine", "Random glide", ...
const char* shapeShortName(LfoShape s);  // the same, short enough for a card header
const char* restartName(LfoRestart r);   // "Free", "Play", "Note", "Bar"
const char* kindName(LfoKind k);         // "LFO", "Chaos", "Random"
const char* triggerName(RandomTrigger t);  // "Tempo", "Hz", "Note", "Roll"
// Chaos's character in a word: "Calm", "Lively", "Wild".
const char* characterName(float character);
// What a source's card header and the knob menu show beside its name: the shape ("Sine"), Chaos's
// character ("Calm"), Random's trigger ("Tempo").
const char* kindSummary(const SongLfo& l);
// "LFO 3" (its place among the board's sources of its kind: "Chaos 1", "Random 2"), the name given to
// it after: "LFO 3 Drift".
std::string number(const Song& s, ObjectId id);
std::string label(const Song& s, ObjectId id);
// The board colour (0..7) a new LFO takes: the first one no LFO uses, else round the colours.
int nextColour(const Song& s);

// --- the LFO's own parameters (ParamId {lfo id, LfoParam}) ------------------
// (0.7.8: Character, Chaos's calm..wild; a parameter of every source, used by Chaos.)
enum class LfoParam : uint32_t { Depth = 0, Offset, Phase, Smooth, RateHz, RateNote, Character, Count };
inline ParamId lfoParam(ObjectId lfo, LfoParam p) { return {lfo, static_cast<uint32_t>(p)}; }
// True when `pid` is a parameter of a board LFO of `s`.
bool isLfoParam(const Song& s, ParamId pid);

// --- shapes (pure, allocation-free: the audio thread uses these) -----------
inline constexpr int kTableSize = 512;
using DrawnTable = std::array<float, kTableSize>;
// The drawn shape as a table of one cycle (x 0..1), from its points and curves (UI thread).
void buildTable(const SongLfo& l, DrawnTable& out);
// The points a drawn shape without points of its own starts from (a gentle hump).
std::vector<LfoPoint> defaultPoints();
// 0.7.9: editing a drawn shape (pure; the board wraps each in one undo step).
// The built-in shape of `l` as points that draw the same cycle ("Edit this shape"): Sine as two smooth
// peaks, Triangle and the saws as straight lines, Square and Random hold as steps, Random glide as
// smooth steps (Random's values of its first cycle). A drawn shape: its own points.
std::vector<LfoPoint> pointsFromShape(const SongLfo& l);
// Mirrored left to right (the cycle runs backwards), and upside down.
std::vector<LfoPoint> flipPoints(const std::vector<LfoPoint>& pts);
std::vector<LfoPoint> invertPoints(const std::vector<LfoPoint>& pts);
// A flat line at the middle, to draw on.
std::vector<LfoPoint> clearPoints();
// The points after a pencil stroke over `pts`: `stroke` is the pointer's path (x 0..1, y -1..1) in the
// order drawn. `steps` >= 2: the cycle is that many cells and every cell the stroke crosses holds the
// stroke's value there (a step); else the stroke is drawn freehand as straight lines (64 to a cycle).
// The shape outside the stroke stays as it was. At most kLfoMaxPoints points.
std::vector<LfoPoint> drawStroke(const SongLfo& l, const std::vector<std::pair<float, float>>& stroke, int steps);
// `x` snapped to the nearest of `steps` even cells' edges (steps < 2: unchanged).
float snapX(float x, int steps);
// A repeatable random value (-1..1) for step `step` of cycle `cycle`.
inline float randomValue(uint32_t seed, uint64_t cycle, uint32_t step) {
  uint64_t z = (static_cast<uint64_t>(seed) << 40) ^ (cycle * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(step) * 0xD1B54A32D192ED03ull);
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<float>(static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0);
}
// --- Chaos and Random (pure, allocation-free) ---------------------------------
// Chaos is fractal value noise, a smoothed random walk: a repeatable random value (randomValue) at
// every whole cycle, joined by a Catmull-Rom curve (smooth, no corners), so one cycle of the rate is
// one bend of the line. Character adds up to three faster layers (about 2, 4.5 and 9.5 times the rate,
// each one weaker) and bends time itself by a slow layer, so the line goes from a calm drift to a wild
// scribble; the sum is scaled back and soft-limited to -1..1. A pure function of (seed, x): the same
// seed at the same place of the song is the same value on every computer and every export.
inline float smoothNoise(uint32_t seed, double x, uint32_t layer) {
  if (x < 0) x = 0;
  const double fl = std::floor(x);
  const uint64_t i = static_cast<uint64_t>(fl);
  const float t = static_cast<float>(x - fl);
  const float p0 = i > 0 ? randomValue(seed, i - 1, layer) : randomValue(seed, 0x7FFFFFFFull, layer);
  const float p1 = randomValue(seed, i, layer), p2 = randomValue(seed, i + 1, layer), p3 = randomValue(seed, i + 2, layer);
  // Catmull-Rom between p1 and p2.
  const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
  const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
  const float c = -0.5f * p0 + 0.5f * p2;
  return ((a * t + b) * t + c) * t + p1;
}
inline float chaosValue(uint32_t seed, double x, float character) {
  const float ch = character < 0 ? 0.0f : (character > 1 ? 1.0f : character);
  // Time bends with the character: a slow layer pushes the walk ahead and holds it back.
  const double warp = x + static_cast<double>(ch) * 0.85 * static_cast<double>(smoothNoise(seed, x * 0.31, 7));
  float sum = smoothNoise(seed, warp, 0), weight = 1.0f;
  const double freq[3] = {2.03, 4.47, 9.61};
  float amp = 1.0f;
  for (int k = 0; k < 3; ++k) {
    amp *= ch * 0.72f;
    if (amp < 1e-4f) break;
    sum += amp * smoothNoise(seed, warp * freq[k] + 17.3 * (k + 1), static_cast<uint32_t>(k + 1));
    weight += amp;
  }
  // Scaled back to about the range one layer has, then a soft limit (the curve can overshoot).
  const float v = sum / weight * (1.35f + 0.5f * ch);
  const float o = v / (1.0f + std::fabs(v) * 0.35f) * 1.35f;
  return o > 1.0f ? 1.0f : (o < -1.0f ? -1.0f : o);
}
// Random: the value (-1..1) of trigger number `step`. Centred: the mean of three values (they gather
// round the middle); quantise >= 2: snapped to that many even levels across the range.
inline float randomSample(uint32_t seed, uint64_t step, bool centred, int quantise) {
  const uint32_t sd = seed ^ 0x5EED5A3Du;  // its own stream (an LFO with the same seed differs)
  float v = randomValue(sd, step, 0);
  if (centred) v = (v + randomValue(sd, step, 1) + randomValue(sd, step, 2)) * (1.0f / 3.0f);
  if (quantise >= 2) {
    const int q = quantise > 16 ? 16 : quantise;
    const float u = (v + 1.0f) * 0.5f;
    const float k = std::floor(u * static_cast<float>(q - 1) + 0.5f);
    v = k / static_cast<float>(q - 1) * 2.0f - 1.0f;
  }
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

// The shape's raw value (-1..1) at `phase` (0..1) of cycle `cycle`. `table`: the drawn shape (Drawn only).
inline float shapeValue(LfoShape shape, double phase, uint64_t cycle, float pulseWidth, int randomSteps, uint32_t seed,
                        const DrawnTable* table) {
  phase = phase - std::floor(phase);
  constexpr double kTwoPi = 6.283185307179586;
  switch (shape) {
    case LfoShape::Sine: return static_cast<float>(std::sin(kTwoPi * phase));
    case LfoShape::Triangle: return static_cast<float>(phase < 0.25 ? phase * 4.0 : phase < 0.75 ? 2.0 - phase * 4.0 : phase * 4.0 - 4.0);
    case LfoShape::SawUp: return static_cast<float>(phase * 2.0 - 1.0);
    case LfoShape::SawDown: return static_cast<float>(1.0 - phase * 2.0);
    case LfoShape::Square: return phase < static_cast<double>(pulseWidth) ? 1.0f : -1.0f;
    case LfoShape::RandomHold:
    case LfoShape::RandomGlide: {
      const int n = randomSteps < 1 ? 1 : (randomSteps > 16 ? 16 : randomSteps);
      const double pos = phase * n;
      const int k = static_cast<int>(pos) < n ? static_cast<int>(pos) : n - 1;
      const float a = randomValue(seed, cycle, static_cast<uint32_t>(k));
      if (shape == LfoShape::RandomHold) return a;
      const float b = k + 1 < n ? randomValue(seed, cycle, static_cast<uint32_t>(k + 1)) : randomValue(seed, cycle + 1, 0);
      const double f = pos - k;
      const double w = 0.5 - 0.5 * std::cos(f * 3.141592653589793);
      return static_cast<float>(a + (b - a) * w);
    }
    case LfoShape::Drawn: {
      if (!table) return 0.0f;
      const double x = phase * kTableSize;
      const int i = static_cast<int>(x) % kTableSize;
      const int j = (i + 1) % kTableSize;
      const float f = static_cast<float>(x - std::floor(x));
      return (*table)[static_cast<size_t>(i)] + ((*table)[static_cast<size_t>(j)] - (*table)[static_cast<size_t>(i)]) * f;
    }
    default: return 0.0f;
  }
}
// After the shape: depth, offset, polarity and the fade (0..1). Bipolar: -1..1; Up only: 0..1.
inline float shapeOutput(float raw, float depth, float offset, bool unipolar, float fade) {
  if (unipolar) {
    const float u = (raw + 1.0f) * 0.5f * depth * fade + offset;
    return u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
  }
  const float v = raw * depth * fade + offset;
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}
// What one link adds to (kModAround, in normalized units) or sets (kModAbsolute: the target 0..1) from
// the LFO's output `v`. `curve` maps the unit value (0..1) first. Pure.
struct LinkShape {
  float amount = 0, min = 0, max = 1;
  uint8_t mode = kModAround, direction = kModBothWays;
  bool invert = false, unipolar = false;
  ModCurve curve = ModCurve::Linear;
};
double linkValue(const LinkShape& k, float v);

// --- links -----------------------------------------------------------------
// Every link of LFO `id`, in the song's order.
std::vector<const ModLink*> linksOf(const Song& s, ObjectId id);
// True when the parameter can be an LFO's target (it resolves, it is not a song parameter).
bool canTarget(const Song& s, ParamId target);
// "Lead · Filter 1 cutoff", "Mixer 3 Drums · Slot 2 Reverb · Mix", "LFO 2 · Depth".
std::string targetLabel(const Song& s, ParamId target);
// The owner's colour for a target's dot (0xAARRGGBB, 0: none).
uint32_t targetColour(const Song& s, ParamId target);
// True when `link` (added to the song) would close a loop through LFOs and macros (an LFO moving a
// macro that moves the same LFO's depth, say). `why` receives a sentence naming it.
bool wouldLoop(const Song& s, const ModLink& link, std::string* why = nullptr);
// A new target's link: around the knob, `amount` of the range (default 25 percent).
ModLink makeLink(ObjectId lfo, ParamId target, float amount = 0.25f);

// --- edits (one undo step each) ---------------------------------------------
// A new LFO with the board's defaults (a sine at 1/4, On play) at the end of the board. 0.7.8: a new
// source of another kind (Chaos: calm at 1/1, locked to the song; Random: a value every 1/8).
SongLfo makeDefault(const Song& s, LfoKind kind = LfoKind::Lfo);
ObjectId addNew(Document& d, LfoKind kind = LfoKind::Lfo);
// Links `target` to LFO `lfo` around the knob at `amount` (an existing link keeps its settings).
// False: no such LFO, not a target, or a loop (Document::lastLinkRefusal says which).
bool link(Document& d, ObjectId lfo, ParamId target, float amount = 0.25f);
// A new LFO moving `target` (25 percent around the knob), in one step. Its id (kNoId: refused).
ObjectId newForKnob(Document& d, ParamId target, const SongLfo* preset = nullptr, LfoKind kind = LfoKind::Lfo);
// Random's Roll: a new value now (one undo step, synced as any edit).
bool roll(Document& d, ObjectId id);

// --- presets ---------------------------------------------------------------
// A whole source's settings by name: the shape (a drawn one too), the rate, restart, knobs and the
// kind's own settings. Never its targets, name, colour or on switch (applyPreset keeps those).
struct Preset {
  std::string name;     // factory: untranslated (i18n::tr when shown); user: the file name
  SongLfo lfo;
  bool factory = false;
  std::string folder;   // user presets: below lfos/ ("" = top level)
  std::string file;     // user presets: the absolute path
};
// The factory LFO presets (original settings).
const std::vector<Preset>& presets();
// The preset's settings on an LFO (keeps its id, name, colour, on switch and note channel).
void applyPreset(SongLfo& l, const SongLfo& preset);
// What a preset keeps of `l` (no id, name, colour, on switch, note channel or Roll count).
SongLfo presetSettings(const SongLfo& l);
// User presets: <presetRoot>/lfos/<folder...>/<name>.json ("gridline-lfo" files).
std::string presetFolder(const std::string& presetRoot);
std::vector<Preset> listUserPresets(const std::string& presetRoot);
// Factory first, then the user's.
std::vector<Preset> allPresets(const std::string& presetRoot);
std::string presetToText(const std::string& name, const SongLfo& l);
bool presetFromText(const std::string& text, Preset& out, std::string* err = nullptr);
bool loadPresetFile(const std::string& file, Preset& out, std::string* err = nullptr);
// Saves `l`'s settings as <root>/lfos/<relName>.json (relName may hold folders). Its path to `path`.
bool saveUserPreset(const std::string& presetRoot, const SongLfo& l, const std::string& relName, std::string* path = nullptr,
                    std::string* err = nullptr);
bool deleteUserPreset(const Preset& p);

}  // namespace gl::lfo
