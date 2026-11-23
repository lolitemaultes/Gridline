// Gridline - K-230: one snapping for every delay time knob.
//
// A delay's time knob gets soft magnets (core/KnobMagnet.h) at the values people aim for:
// note values (1/64 to bars, dotted and triplet) on a synced time, round milliseconds
// (1, 2, 5, 10, 20, 50 ...) on a free time. Between them the knob moves freely and smoothly.
//
// A time knob also travels evenly: each doubling of the time gets the same room on the knob
// (1/16 to 1/8 takes as much drag as 1/4 to 1/2), so the note values are spread out instead of
// crowded at one end. For a parameter whose own mapping is linear (ticks, linear ms) the knob's
// travel is a log curve with a small offset at the bottom (so the first sliver of a millisecond
// does not take a third of the knob); for a parameter that is already logarithmic it is the
// parameter's own mapping. The travel changes how the knob is dragged and drawn only: stored
// values, automation and the sound are untouched.
//
// The readout names the value the way a musician says it: "1/8 dotted (321 ms)", "1/4",
// "0.83 beat" between note values, "250 ms". Pure functions (tests/test_k230.cpp).
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "PluginInfo.h"

namespace gl {
struct Song;
struct ParamId;
}  // namespace gl

namespace gl::timeknob {

// ticks: 1/192 beat; steps: sixteenth notes. K-240: Beats (a length in beats), Hz (a rate: its
// magnets are the note values at the song's tempo, one cycle per note).
enum class Unit { Ticks, Steps, Ms, Seconds, Beats, Hz };
inline constexpr double kPpq = 192;

struct Spec {
  Unit unit = Unit::Ms;
  std::vector<double> magnets;  // parameter units, ascending
  double offset = 0;            // > 0: the knob travels by log(v - min + offset)
  double quantum = 0;           // > 0: a coarse drag lands on whole multiples (whole ticks)
};

// The note values between lo and hi, in ticks (straight 1/64 to 8 bars, dotted and triplet 1/16 to 1/2).
std::vector<double> noteTicks(double lo, double hi);
// Round milliseconds between lo and hi: the 1, 2, 5 series (1, 2, 5, 10, 20, 50, 100 ... 5000).
std::vector<double> roundMs(double lo, double hi);

// The time knob of parameter `param` of plugin `type`, or nullopt when it is not a time control.
// K-240: besides the delay times named for K-230, every built-in plugin's time control is one,
// found from its descriptor (classify): milliseconds and seconds (round values), lengths in beats,
// steps or ticks (note values) and slow rates in Hz (note values at `tempo`). Installed plugins
// keep their own knobs.
std::optional<Spec> forParam(const std::string& type, int param, double tempo = 120.0);
// K-240: the time knob a descriptor stands for (nullopt: not a time control).
std::optional<Spec> classify(const ParamDesc& d, double tempo = 120.0);
// The same for a song parameter id (a channel generator or a mixer slot effect).
std::optional<Spec> forParam(const Song& song, ParamId pid);

// The knob position (0..1) of value v and back, with the spec's travel (the parameter's own
// mapping when the spec has no offset).
double toPos(const Spec& s, const ParamDesc& d, double v);
double toValue(const Spec& s, const ParamDesc& d, double pos, bool quantise);

// The note name of a tick value ("1/4", "1/8 dotted", "1/16 triplet", "1 bar", "2 bars"), empty
// when it is not a note value. Compact: "1/8 D", "1/16 T".
std::string noteName(double ticks, bool compact = false);
// The readout of value v: compact for a small display, else with milliseconds at `tempo`.
std::string readout(const Spec& s, double v, double tempo, bool compact);

}  // namespace gl::timeknob
