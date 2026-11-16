// Gridline - K-311: typed parameter values. The text a person types into a control's type-in box
// becomes the parameter's value: a number with or without its unit ("2.5k", "2.5 kHz", "-6",
// "-6 dB", "250 ms", "0.5 s"), a percentage ("50%", of the value for percent parameters, of the
// travel for the others), a note value on a delay time ("1/4", "1/8 dotted", "1/16 t", "2 bars",
// "1.5 beats"), a plain fraction elsewhere ("1/4" = 0.25), a note name ("C4", "A#3"), a choice's
// name, on / off, pan ("50% left", "30 r", "centre"), a ratio ("4:1"). Every value the readout
// shows (ParamDesc::format) reads back as itself. The result is clamped to the range.
// Pure functions (tests/test_k311_typein.cpp).
#pragma once
#include <string>
#include "PluginInfo.h"
#include "TimeKnob.h"

namespace gl::paramtext {

// The value of `text` for parameter `d`; false when it is not a value (the box keeps its text).
// `time`: the parameter's delay time spec (timeknob::forParam), for note values; `tempo` turns
// note values into milliseconds on a free time.
bool parse(const ParamDesc& d, const std::string& text, double& out, const timeknob::Spec* time = nullptr, double tempo = 120.0);

// The text the type-in box starts with: the value with its unit (a delay time as its readout).
std::string initial(const ParamDesc& d, double value, const timeknob::Spec* time = nullptr, double tempo = 120.0);

}  // namespace gl::paramtext
