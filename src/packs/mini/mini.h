// Gridline — "mini" pack: Gridline Suite Mini, a cohesive set of 35 compact
// single-purpose effects (one plain word per plugin: Trim, Echo, Orbit...).
// Shared by the catalogue (mini_info.cpp), the DSP (mini_*_dsp.cpp) and the
// editors (mini_ui.cpp): parameter indices, families and the type list.
#pragma once
#include <string>

namespace gl::packs::mini {

// Browser / picker category of every Mini plugin.
inline constexpr const char* kCategory = "Gridline Suite Mini";

// Families give the suite its colour accents (one per family, not per plugin).
enum class Family { Dynamics, Tone, Drive, Pitch, Motion, Time, Utility };

// --- utility ------------------------------------------------------------------
namespace trim { enum { Gain, FlipL, FlipR, Count }; }
namespace breadth { enum { Width, Mid, Pan, MonoBelow, Count }; }
namespace skew { enum { Delay, Side, Balance, Count }; }
namespace weave { enum { LL, RL, LR, RR, Count }; }

// --- tone (filters and EQ) ----------------------------------------------------
namespace bands { enum { Low, Mid, High, LowSplit, HighSplit, Count }; }
namespace sieve { enum { Type, Cutoff, Q, Gain, Slope, Count }; }
namespace rungs { enum { Cutoff, Resonance, Drive, Mode, Mix, Count }; }
namespace scorch { enum { Type, Cutoff, Q, Drive, Flavour, Mix, Count }; }
namespace vowel { enum { X, Y, Sharpness, Shift, Mix, Count }; }
namespace tines { enum { Freq, Feedback, Damp, Stereo, Polarity, Mix, Count }; }
namespace chime { enum { Note, Decay, Bright, Partials, Mix, Count }; }

// --- dynamics -----------------------------------------------------------------
namespace press { enum { Threshold, Ratio, Attack, Release, Knee, Makeup, Detect, Sidechain, Count }; }
namespace ceiling { enum { Input, Ceiling, Release, Count }; }
namespace shear { enum { Drive, Ceiling, Soft, Output, Count }; }
namespace pack { enum { Threshold, Amount, Attack, Release, Link, Detect, Count }; }
namespace span { enum { LowThresh, LowRatio, HighThresh, HighRatio, Speed, Output, Count }; }
namespace latch { enum { Threshold, Hysteresis, Attack, Hold, Release, Floor, Flip, Sidechain, Count }; }
namespace punch { enum { Attack, Sustain, Speed, Output, Clip, Count }; }

// --- drive --------------------------------------------------------------------
namespace burn { enum { Drive, Type, Bias, Tone, Output, Mix, Count }; }
namespace bend { enum { Shape, Amount, Drive, Edge, Mix, Count }; }
namespace warp { enum { Drive, Tone, Bias, Stereo, Mix, Count }; }
namespace crumble { enum { Rate, Bits, Jitter, Smooth, Dither, Mix, Count }; }
namespace clang { enum { Freq, Shape, Bias, Spread, Source, Mix, Count }; }

// --- pitch --------------------------------------------------------------------
namespace slide { enum { Shift, Stereo, Feedback, Mix, Count }; }
namespace interval { enum { Pitch, Fine, Grain, Jitter, Mix, Count }; }

// --- motion -------------------------------------------------------------------
namespace dbl { enum { Delay, Depth, Rate, Voices, Spread, Mix, Count }; }  // "Double"
namespace crowd { enum { Voices, Detune, Spread, Motion, Mix, Count }; }
namespace jet { enum { Delay, Depth, Rate, Feedback, Stereo, Mix, Count }; }
namespace orbit { enum { Stages, Centre, Depth, Rate, Feedback, Stereo, Mix, Count }; }

// --- time and space -----------------------------------------------------------
namespace echo { enum { Time, Sync, Steps, Feedback, PingPong, Tone, Duck, Mix, Count }; }
namespace volley { enum { Time, Ratio, Feedback, Cross, Tone, Mix, Count }; }
namespace vault { enum { Size, Decay, Damp, PreDelay, Width, Early, Mix, Count }; }
namespace rewind { enum { Length, Sync, Steps, Fade, Mix, Count }; }
namespace halt { enum { Run, StopTime, StartTime, Curve, Count }; }
namespace pulse {
enum { Step0, Rate = 16, Attack, Release, Floor, Swing, Mix, Count };
inline constexpr int kSteps = 16;
}

// Synced note values shared by Echo, Rewind and Pulse (beats per value).
inline constexpr int kNoteValues = 9;
inline constexpr const char* kNoteNames[kNoteValues] = {"1/32", "1/16 T", "1/16", "1/8 T", "1/16 D", "1/8", "1/4 T", "1/8 D", "1/4"};
inline constexpr double kNoteBeats[kNoteValues] = {0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.375, 0.5, 2.0 / 3.0, 0.75, 1.0};

// One row per plugin: type, display name, family.
struct Entry {
  const char* type;
  const char* name;
  Family family;
};
inline constexpr Entry kEntries[] = {
    {"gl.minitrim", "Trim", Family::Utility},       {"gl.minibreadth", "Breadth", Family::Utility},
    {"gl.miniskew", "Skew", Family::Utility},       {"gl.miniweave", "Weave", Family::Utility},
    {"gl.minibands", "Bands", Family::Tone},        {"gl.minisieve", "Sieve", Family::Tone},
    {"gl.minirungs", "Rungs", Family::Tone},        {"gl.miniscorch", "Scorch", Family::Tone},
    {"gl.minivowel", "Vowel", Family::Tone},        {"gl.minitines", "Tines", Family::Tone},
    {"gl.minichime", "Chime", Family::Tone},        {"gl.minipress", "Press", Family::Dynamics},
    {"gl.miniceiling", "Ceiling", Family::Dynamics}, {"gl.minishear", "Shear", Family::Dynamics},
    {"gl.minipack", "Pack", Family::Dynamics},      {"gl.minispan", "Span", Family::Dynamics},
    {"gl.minilatch", "Latch", Family::Dynamics},    {"gl.minipunch", "Punch", Family::Dynamics},
    {"gl.miniburn", "Burn", Family::Drive},         {"gl.minibend", "Bend", Family::Drive},
    {"gl.miniwarp", "Warp", Family::Drive},         {"gl.minicrumble", "Crumble", Family::Drive},
    {"gl.miniclang", "Clang", Family::Drive},       {"gl.minislide", "Slide", Family::Pitch},
    {"gl.miniinterval", "Interval", Family::Pitch}, {"gl.minidouble", "Double", Family::Motion},
    {"gl.minicrowd", "Crowd", Family::Motion},      {"gl.minijet", "Jet", Family::Motion},
    {"gl.miniorbit", "Orbit", Family::Motion},      {"gl.miniecho", "Echo", Family::Time},
    {"gl.minivolley", "Volley", Family::Time},      {"gl.minivault", "Vault", Family::Time},
    {"gl.minirewind", "Rewind", Family::Time},      {"gl.minihalt", "Halt", Family::Time},
    {"gl.minipulse", "Pulse", Family::Time},
};
inline constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

inline const Entry* findEntry(const std::string& type) {
  for (const Entry& e : kEntries)
    if (type == e.type) return &e;
  return nullptr;
}

// Choice labels shared by the catalogue and the editors.
inline constexpr const char* kSieveTypes[] = {"Low pass", "High pass", "Band pass", "Notch", "Peak", "Low shelf", "High shelf"};
inline constexpr const char* kSieveSlopes[] = {"12 dB", "24 dB", "36 dB", "48 dB"};
inline constexpr const char* kRungsModes[] = {"LP 24", "LP 12", "Band", "HP 24"};
inline constexpr const char* kScorchTypes[] = {"Low pass", "Band pass", "High pass", "Notch"};
inline constexpr const char* kScorchFlavours[] = {"Soft", "Hot", "Fold", "Lean"};
inline constexpr const char* kChimePartials[] = {"All", "Odd", "Hollow"};
inline constexpr const char* kDetects[] = {"Peak", "RMS"};
inline constexpr const char* kBurnTypes[] = {"Warm", "Tube", "Hard", "Fold", "Sine", "Step"};
inline constexpr const char* kBendShapes[] = {"Smooth S", "Steep S", "Steps", "Ripple", "Lean", "Bounce"};
inline constexpr const char* kBendEdges[] = {"Clamp", "Mirror", "Wrap"};
inline constexpr const char* kClangShapes[] = {"Sine", "Triangle", "Square", "Ramp"};
inline constexpr const char* kClangSources[] = {"Oscillator", "Sidechain"};
inline constexpr const char* kVoiceCounts[] = {"2", "3", "4"};
inline constexpr const char* kCrowdMotions[] = {"Drift", "Swirl"};
inline constexpr const char* kOrbitStages[] = {"2", "4", "6", "8", "12"};
inline constexpr const char* kSides[] = {"Left", "Right"};
inline constexpr const char* kPolarities[] = {"Add", "Subtract"};
inline constexpr const char* kRewindNames[] = {"1/16", "1/8", "1/4", "1/2", "1 bar"};
inline constexpr double kRewindBeats[] = {0.25, 0.5, 1.0, 2.0, 4.0};
inline constexpr const char* kPulseRates[] = {"1/8", "1/16 T", "1/16", "1/32 T", "1/32"};
inline constexpr double kPulseBeats[] = {0.5, 1.0 / 6.0, 0.25, 1.0 / 12.0, 0.125};

}  // namespace gl::packs::mini
