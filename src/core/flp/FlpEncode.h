// Gridline — FLP export: plugin state encoders, the reverse of the plugin
// decoders (J-34).
//
// An encoder describes where an FL plugin's state stores each value its
// decoder reads (a Field per Gridline parameter: offset, stored type, range),
// how to build a fresh state with the plugin's defaults, and how to write the
// values that are not one number (strings, sample paths, point lists). The
// engine (FlpEncode.cpp) does the rest through the decoder itself:
//   1. template: the state the instance was imported with (FL origin), else
//      the encoder's defaults;
//   2. unchanged (the template decodes to the instance's values): written
//      verbatim — an unchanged import exports byte for byte;
//   3. otherwise each field whose decoded value differs is solved by search
//      over its stored range (bisection for monotonic laws, exhaustive for
//      small ranges), decoding the candidate with the plugin's own decoder,
//      so value laws never have to be inverted by hand and stay in step with
//      decoder changes;
//   4. the result is verified by decoding it: Exact when every parameter
//      decodes back to the instance's value, else Approx with the list of
//      parameters that differ.
// Encoders are grouped by family (FlpEnc<Family>.cpp), one registration
// function each (see registerAll in FlpEncode.cpp).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../PluginInfo.h"
#include "Flp.h"
#include "FlpWriter.h"

namespace gl::flp::enc {

// Stored type of a field.
enum class Type : uint8_t { U8, I8, U16, I16, U32, I32, F32, F64 };
size_t typeSize(Type t);
double readField(const Bytes& b, size_t offset, Type t);   // NaN when out of range
void writeField(Bytes& b, size_t offset, Type t, double v);  // rounds / clamps to the type

// One stored value a Gridline parameter is decoded from.
struct Field {
  int gl = -1;          // Gridline parameter index
  size_t offset = 0;    // byte offset in the state (event 213 payload)
  Type type = Type::I32;
  double lo = 0, hi = -1;  // stored range searched, inclusive (hi < lo: the type's range; F32/F64 need a range)
  // Optional direct inverse: Gridline value → stored value. Checked through
  // the decoder; the engine searches when it does not reproduce the value.
  std::function<double(double gl)> inverse = {};
};
// Shorthand constructors.
inline Field field(int gl, size_t offset, Type t, double lo, double hi) { return Field{gl, offset, t, lo, hi, {}}; }
inline Field i32(int gl, size_t offset, double lo, double hi) { return Field{gl, offset, Type::I32, lo, hi, {}}; }

struct EncodeContext;

// Writes PluginState::ins (engine/ChannelIns.h text; "" = neutral) into a
// state's copy of the channel envelope records (5 × 68 bytes at envAt) and
// filter levels (6 dwords at levAt: 0, 0, cutoff, resonance, 0, type) (J-33).
void writeInsCopy(Bytes& b, size_t envAt, size_t levAt, const std::string& ins);

struct Encoder {
  std::string flName;  // FL plugin name (event 201), as the decoder is registered
  std::string glType;  // Gridline type the decoder produces
  // Native export (Gridline instances that were not imported from FL, or
  // whose origin is gone): the encoder with the lowest rank whose `accepts`
  // takes the state is used. < 0: only for instances imported from this FL plugin.
  int nativeRank = -1;
  std::function<bool(const PluginState&)> accepts = {};  // empty = any state of glType
  bool generator = false;  // instrument (channel) plugin
  // J-44: FL stores no state for this plugin at all (no event 213): an
  // instance the decoder reproduces from nothing is written exactly.
  bool stateless = false;
  // A fresh state with the plugin's default settings. Empty function or empty
  // result: unknown — the plugin is then written without a state (FL loads it
  // with its defaults) and the export reports the settings as not carried.
  std::function<Bytes()> defaults = {};
  // J-39: a fresh state taken from the FL installation (its plugin database
  // preset, read at export time; empty when FL is not installed) — for
  // plugins whose default layout is not built here. Used when `defaults` is empty.
  std::function<Bytes(const EncodeContext& c)> defaultsIn = {};
  // Layout of the given state (may depend on its version word / sizes).
  std::function<std::vector<Field>(const Bytes& state)> fields = {};
  // Values that are not one number each (strings, sample paths, tables).
  // Called first; may resize the state. Fields are solved afterwards.
  std::function<void(const PluginState& gl, Bytes& state, const EncodeContext& c)> write = {};
  // Runs after the fields are solved, before the result is verified
  // (checksums, sizes that depend on the solved values).
  std::function<void(Bytes& state)> finish = {};
  // J-53: Gridline-only settings of the state that the FL plugin does not
  // store (a stand-in's own settings): reset to what the decoder produces
  // before the instance is compared and encoded, so editing them never
  // changes the FL state (an unchanged origin stays verbatim).
  std::function<void(PluginState& s)> strip = {};
};

// Settings the decoders depend on (sync laws, sample folders).
struct EncodeContext {
  double tempo = 140.0;
  int ppq = 96;
  std::string factoryData;  // folder %FLStudioFactoryData% stands for
  std::string projectDir;
  // J-39: the FL plugin the "Fruity Wrapper" encoder wraps (empty = its own
  // choice); encode() tries each one that fits (see wrapCandidates).
  std::string wrapAs;
  // J-44: the channel's sample path (event 196, FL text) the export writes
  // beside a plugin that plays the channel sample (Granulizer, a custom-wave
  // 3x Osc, …): decodeState presents it to the decoder as the channel does.
  std::string channelSample;
};

// FL plugins (by decoder name) that can carry this Gridline state inside FL's
// plugin wrapper: every parameter not at its default maps. Preferred first.
std::vector<std::string> wrapCandidates(const PluginState& g);

// FL path for a sample path that the decoders resolved: absolute paths inside
// the factory data folder become "%FLStudioFactoryData%\…" again (backslashes).
std::string flPath(const std::string& path, const EncodeContext& c);

// Registry.
const std::vector<Encoder>& encoders();
const Encoder* findEncoder(const std::string& flName, const std::string& glType);
// Encoder for a Gridline-native instance (lowest nativeRank accepting it).
const Encoder* nativeEncoder(const PluginState& s);

// Decodes one plugin state with the registered decoder (a throwaway import
// context). False when the decoder rejects it. `state` may be empty (no 213).
bool decodeState(const std::string& flName, const Bytes& state, PluginMapping& out, const EncodeContext& c);

struct Result {
  enum Quality {
    Verbatim,     // the origin state, unchanged
    Exact,        // encoded; decodes to the instance's values
    Approx,       // encoded; some parameters differ (see `mismatched`)
    Defaults,     // no state could be written: FL loads the plugin's defaults
    None,         // no FL plugin for this Gridline type
  } quality = None;
  std::string flName;
  Bytes state;
  bool hasState = false;
  std::vector<int> mismatched;  // Gridline params that do not decode back
  bool dataMismatch = false;    // PluginState::data differs after decoding
  std::string note;
};
const char* qualityName(Result::Quality q);

// Encodes a Gridline plugin state as an FL plugin. `originFlName` /
// `originState`: what the instance was imported from (empty when native).
Result encode(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& c);

// Per-family registration (FlpEnc*.cpp). Each appends its encoders.
void registerFxEq(std::vector<Encoder>& v);      // FlpFxEq.cpp family
void registerFxDyn(std::vector<Encoder>& v);     // FlpFxDyn.cpp
void registerFxTime(std::vector<Encoder>& v);    // FlpFxTime.cpp
void registerFxK3(std::vector<Encoder>& v);      // FlpFxK3.cpp
void registerFxMisc(std::vector<Encoder>& v);    // FlpFxMisc.cpp
void registerFx2Mod(std::vector<Encoder>& v);    // FlpFx2Mod.cpp, FlpFx2Patcher.cpp
void registerFx2Pitch(std::vector<Encoder>& v);  // FlpFx2Pitch.cpp
void registerFx2Space(std::vector<Encoder>& v);  // FlpFx2Space.cpp
void registerFx2Tone(std::vector<Encoder>& v);   // FlpFx2Tone.cpp
void registerDec3(std::vector<Encoder>& v);      // FlpDec3Fx/Inst/Kepler/Vfx.cpp, FlpDec4.cpp, FlpDecoders3.cpp
void registerGenA(std::vector<Encoder>& v);      // FlpGenOsc/Samplers/Simple/Synths(+Sytrus, Harmor, Harmless, GMS).cpp
void registerGenB(std::vector<Encoder>& v);      // FlpGen2*.cpp
void registerJ22(std::vector<Encoder>& v);       // FlpJ22*.cpp
void registerWrapper(std::vector<Encoder>& v);   // Fruity Wrapper (third-party plugins), FlpEncWrapper.cpp

// Full state writers in their own files (J-39), registered by their family.
Encoder sytrusEncoder();  // FlpEncSytrus.cpp
Encoder harmorEncoder();  // FlpEncHarmor.cpp
// Structure checks (tests): the writer's lossless structure reproduces an
// FL state byte for byte (parse + serialize). `raw` / `again`: the
// uncompressed patch and its re-serialization.
bool sytrusStructureRoundTrip(const Bytes& state, Bytes* raw = nullptr, Bytes* again = nullptr);
Bytes sytrusRaw(const Bytes& state);         // uncompressed, info text removed
Bytes harmorComparable(const Bytes& state);  // info text removed
bool harmorStructureRoundTrip(const Bytes& state, Bytes* again = nullptr);

// Corpus check (gridline flp-encode-check): every plugin state in the given
// files is decoded, encoded from its Gridline state without the origin
// (native path) and with a changed parameter on the origin (patch path), and
// decoded again. One line per FL plugin + details.
struct CheckOptions {
  std::vector<std::string> plugins;  // FL names to check (empty = all)
  size_t maxPerPlugin = 200;         // states per plugin
  bool verbose = false;
};
std::string encodeCheck(const std::vector<std::string>& files, const CheckOptions& o, const EncodeContext& c);

}  // namespace gl::flp::enc
