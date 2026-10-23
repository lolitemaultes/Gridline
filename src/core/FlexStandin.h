// Gridline — J-53: stand-ins for FL Studio's preset player (FLEX).
//
// FLEX plays sound content from packs FL keeps access-protected. Gridline
// never reads that content. A FLEX channel ("gl.flex") keeps the open part of
// the FLEX instance state exactly (pack and preset names, the 45 control
// values) so an FL export reopens with the real FLEX sound, and plays a
// Gridline stand-in while in Gridline: one of Gridline's own instruments,
// chosen by a category classified from the pack / preset names, with the
// FLEX controls (envelopes, filter, macros, effects, volume, pitch) applied
// on top as offsets.
//
// Parameter layout of "gl.flex":
//   0..44   the FLEX controls, stored as FLEX stores them (0..1, control-tag order)
//   45..48  stand-in only (never written to FLEX): template, tone, level, octave
// PluginState::data: "flex 2\npack <pack>\npreset <preset>\n".
#pragma once
#include <string>
#include <vector>
#include "PluginInfo.h"

namespace gl::flex {

inline constexpr const char* kType = "gl.flex";
inline constexpr int kControls = 45;

// FLEX control tags (FL's own form order).
enum Ctl {
  VolA, VolH, VolD, VolS, VolR,
  FltA, FltH, FltD, FltS, FltR,
  Macro1, Macro2, Macro3, Macro4, Macro5, Macro6, Macro7, Macro8,
  Cutoff, EnvAmt, Reso,
  PostCut, PostReso, PostType,
  DlyType, DlyMix, DlyTime, DlyFb, DlyMod, DlyColor,
  RevMix, RevDecay, RevSize, RevMod, RevColor,
  LimPre, OutVol, DlyOn, RevOn, Pitch, PostOn, LimOn, BandMix, LimType, RevModSpeed,
};
static_assert(RevModSpeed == kControls - 1, "FLEX control layout");

// Stand-in only parameters.
enum StandIn { PTemplate = kControls, PTone, PLevel, POctave, PCount };

// Stand-in categories (PTemplate: 0 = automatic, else the category).
enum Cat {
  CatAuto, Bass, SubBass, Lead, Pad, Keys, EPiano, Pluck, Bell, Strings, Brass, Choir, Stab, Arp, Guitar, Organ, Fx, Drums, Neutral,
  CatCount
};
const char* catName(int cat);                  // "Bass", "Sub bass", …; "Auto" for 0
const std::vector<std::string>& catChoices();  // PTemplate choice labels (index = Cat)

// Category from the pack and preset names (keyword table, several languages).
// `why` (optional) receives the word that decided it.
int classify(const std::string& pack, const std::string& preset, std::string* why = nullptr);

// Pack / preset kept in PluginState::data.
struct Identity {
  bool ok = false;
  std::string pack, preset;
};
Identity identity(const std::string& data);
std::string makeData(const std::string& pack, const std::string& preset);

// The category a state plays: its PTemplate choice, else classified.
int categoryOf(const PluginState& s);

// Gridline engines the stand-ins are built from.
enum Engine { EAnalog, EKeys, EFm, EDrums, EPlucker, EngineCount };
const char* engineType(int engine);  // "gl.analog", …
const char* engineName(int engine);  // the engine's catalogue display name
int engineOf(int cat);

// Single-piece percussion presets ("Cowbell", "Conga", …): the kit pad that
// plays pitched across the keys; -1 = a whole kit across the keys.
int drumPiece(const std::string& preset);

// Template (UI thread): the engine state before the FLEX controls apply.
PluginState templateState(int cat, int piece = -1);

// Applies the FLEX controls and stand-in settings `p` (PCount values) to an
// engine state that holds templateState(cat) — in place, without allocating
// (audio thread). `info` is the engine's catalogue entry (ranges).
void applyControls(int cat, const float* p, const PluginInfo& info, PluginState& work);

// Stand-in effects (Gridline's reverb and delay after the engine).
struct FxSettings {
  bool revOn = false, dlyOn = false;
  float revWet = 0, revSize = 0.5f, revDecay = 1.5f, revHighCut = 12000;
  float dlyWet = 0, dlyFb = 0.3f;
  int dlyDiv = 8;  // gl.delay time choice
};
FxSettings fxSettings(int cat, const float* p);

// Output gain of the stand-in (template trim, FLEX output volume, level).
float outputGain(int cat, const float* p);
// Note pitch offset in semitones (FLEX pitch + octave); drum kits: 0.
double pitchOffset(int cat, const float* p);
bool softClip(const float* p);  // FLEX limiter switch

// Kit layout: MIDI note → pad note of the Drum Model template (base 60).
int drumNote(int note);

// Full stand-in engine state for a FLEX state (UI thread; "Convert to
// Gridline instrument"): the template with the controls applied.
PluginState standInState(const PluginState& flexState);

// The FLEX state as a fresh "gl.flex" state (decoder, tests).
PluginState makeState(const std::string& pack, const std::string& preset, const float* controls /* kControls */);

// States saved before J-53 played FLEX on Analog / Keys / FM with
// data "flex 1\npack …\npreset …\nv <45 values>": converts one to "gl.flex".
bool migrateLegacy(PluginState& s);
bool isLegacy(const PluginState& s);

}  // namespace gl::flex
