// Gridline — beat slicing (transient detection or equal divisions).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Sample.h"

namespace gl {

// Slice start frames. mode 0 = transients (sensitivity 0..1), 1..4 = 4/8/16/32 equal slices.
std::vector<int64_t> computeSlices(const Sample& s, int mode, double sensitivity);

// Drum Pads data text: one line per pad of bank A (layer 1: path, "" = built-in
// kit sound, "none" = empty), then keyed lines (pad / layer numbers 1-based):
//   "L<pad>.<layer> <path>"   sample of any other layer (bank B layer 1 too)
//   "S<pad>.<layer> lo hi level pan tune reverse"   settings of layers 5..32
//   "N<pad> <name>"  "C<pad> <RRGGBB>"   pad name / colour
//   "E<pad>.v <env>"  "E<pad>.p <env>"   volume / pan envelope (see PadEnvelope)
std::string padSamplePath(const std::string& data, int pad, int layer);
std::string setPadSamplePath(const std::string& data, int pad, int layer, const std::string& path);
// Keyed lines: value of "<key> <value>" ("" when missing); set (empty value removes).
std::string padDataGet(const std::string& data, const std::string& key);
std::string padDataSet(const std::string& data, const std::string& key, const std::string& value);

// Multi-point envelope (pad volume 0..1 / pan 0..1 with 0.5 centre): point k
// is reached `t[k]` seconds after point k-1 (t[0] = 0) with a curve bent by
// `ten[k]` (-1..1, positive = slow start). While the note is held the
// envelope holds at `sustain` (or loops loopStart..loopEnd); after the
// release it runs on to the last point. Text: "<on> <sus> <loopStart> <loopEnd> t,v,ten t,v,ten ...".
struct PadEnvelope {
  static constexpr int kMax = 16;
  bool on = false;
  int sustain = -1, loopStart = -1, loopEnd = -1, n = 0;
  float t[kMax] = {}, v[kMax] = {}, ten[kMax] = {};
};
bool parsePadEnvelope(const std::string& text, PadEnvelope& e);
std::string formatPadEnvelope(const PadEnvelope& e);
// Envelope curve between two points (x 0..1) for a tension.
double padEnvCurve(double tension, double x);

// Settings of a layer beyond the 4 parameter layers.
struct PadLayerX {
  int lo = 0, hi = 127;
  float level = 1, pan = 0, tune = 0;
  bool reverse = false;
};
bool parsePadLayer(const std::string& text, PadLayerX& l);
std::string formatPadLayer(const PadLayerX& l);
// Number of layers of a pad (1 + the highest layer index with a sample line).
int padLayerCount(const std::string& data, int pad);

// Prepares the aux data of the built-in sample instruments (sampler, slicer,
// drum pads with all layers) on the UI thread. Returns false for other types.
// (The program builder should call this; it supersedes its inline pad/slicer code.)
struct ChannelAux;
struct PluginState;
bool prepareBuiltinAux(const PluginState& st, ChannelAux& aux);

// Drum Pads data the audio thread needs besides the samples (built on the UI thread).
struct PadsAux;
const PadsAux* padsAuxOf(const ChannelAux& aux);
struct PadsAuxView {
  const PadEnvelope* vol = nullptr;  // [kPadsAll]
  const PadEnvelope* pan = nullptr;  // [kPadsAll]
  const PadLayerX* extra = nullptr;  // [kPadsAll * kPadMaxLayers]
  const int* layers = nullptr;       // [kPadsAll] layer count
};
PadsAuxView padsAuxView(const PadsAux* a);

// Names of the built-in drum kit ("builtin:<name>" sample paths) in pad order.
const std::vector<std::string>& builtinKit();

}  // namespace gl
