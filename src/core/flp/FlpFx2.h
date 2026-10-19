// Gridline — FLP import, second-wave effects: helpers shared by the
// FlpFx2*.cpp decoder files (registered by FlpEffects2.cpp).
//
// State layouts and value curves were established from FL Studio's own
// supplied projects and presets and, where noted, from the plugins'
// behaviour; notes: analysis/demos/effects_wave2.md. The code is original.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "FlpFx.h"

namespace gl::flp::fx2 {

using fx::Knob;
using fx::KnobTable;
using fx::Reader;

inline double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
inline double dbOf(double gain, double floorDb = -120.0) { return gain > 1e-9 ? std::max(floorDb, 20.0 * std::log10(gain)) : floorDb; }
// Exponential (log-scaled) interpolation lo..hi for x in 0..1.
inline double expMap(double x, double lo, double hi) { return lo * std::pow(hi / lo, clampd(x, 0.0, 1.0)); }

// The raw state payload (event 213) as bytes (empty when missing).
std::vector<uint8_t> stateBytes(const std::vector<Event>& evs);
// `count` int32 values (stops early at the end of the payload).
std::vector<double> ints(Reader& r, size_t count);
// UTF-16LE (`units` code units) → UTF-8; stops at NUL.
std::string utf16(const uint8_t* p, size_t units);
// Latin-1 bytes → UTF-8.
std::string latin1(const uint8_t* p, size_t n);
// Plain text of an RTF document (control words dropped, \par → newline,
// \'xx → Latin-1 character, \uN → Unicode). Non-RTF input is returned as is.
std::string rtfToText(const std::string& rtf);
// CR / CRLF → LF, trailing whitespace trimmed.
std::string normalizeLines(const std::string& s);
// Index of the value in `choices` nearest to v.
int nearestChoice(double v, const std::vector<double>& choices);
// Mapping that forwards automation through a table and also sets glType/state.
void bindKnobs(PluginMapping& m, const KnobTable& table);
// Decodes one embedded plugin (name + raw state) through the decoder
// registry, as if it sat in a mixer slot. false when unknown / undecodable.
bool decodeEmbedded(Context& c, const std::string& flName, const uint8_t* state, size_t size, PluginMapping& out);
// True when `evs` is a channel's event list (generator) rather than a mixer slot's.
bool isChannelPlugin(const Context& c, const std::vector<Event>& evs);

// Per-family registries (FlpFx2<Family>.cpp), called by FlpEffects2.cpp.
void registerFx2Mod(std::map<std::string, PluginDecoder>& d);     // pan, stereo, controllers, routing, notes, hosts
void registerFx2Pitch(std::map<std::string, PluginDecoder>& d);   // pitch, frequency shift, vocoder, scratcher
void registerFx2Tone(std::map<std::string, PluginDecoder>& d);    // filters, EQ, distortion, loudness, splitter
void registerFx2Space(std::map<std::string, PluginDecoder>& d);   // reverbs, convolver, delay, chorus, phasers
void registerFx2Patcher(std::map<std::string, PluginDecoder>& d); // Patcher (effect and instrument)

}  // namespace gl::flp::fx2
