// Gridline: Plasma's readers of Serum states, shared by the preset importer
// (plasma_import_dsp.cpp) and the Serum state / stand-in decoders
// (plasma_serum_dsp.cpp). Internal to the pack.
//
// Every reader produces the same intermediate form: a Serum 2 style document
// ("root": module objects with "plainParams", as Serum 2 stores them after
// its zstd / CBOR layers) plus a "meta" object (preset name, author, tags).
// Serum 1 chunks are translated into that form, so one importer maps both.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/Json.h"
#include "plasma.h"

namespace gl::packs::plasma::serum {

// Embedded audio in a document: a Json string starting with this tag holds
// float32 values (native byte order), written by the CBOR decoder for the
// audio arrays (embeddedWTData, embeddedNoiseData, embeddedIR, embedded_audio
// "data") and by the Serum 1 reader. floatsOf() also reads plain number arrays.
inline constexpr const char kBlobTag[] = "\x01" "f32:";
Json floatBlob(const float* p, size_t n);
bool floatsOf(const Json& j, std::vector<float>& out);
// Channels of an embedded audio value: an array of channel blobs, or one blob (mono).
bool channelsOf(const Json& j, std::vector<std::vector<float>>& out);

// "XferJson\0" containers (.SerumPreset files, Serum 2 plugin states).
bool decodeXferJson(const uint8_t* p, size_t n, Json& meta, Json& root, std::string& err);
// Serum 1 preset chunk (the zlib data of an .fxp / a VST2 chunk) or a whole
// .fxp file ("CcnK"). `params` (299 normalized values) can override the
// chunk's parameters (automation links).
struct Serum1 {
  std::vector<float> params;           // 299 VST parameters, normalized
  std::vector<uint8_t> state;          // decompressed state
  // Audio stored after the state (second zlib stream): oscillator A and B
  // tables (2048-sample frames) and the noise sample, mono float32.
  std::vector<float> tableA, tableB, noise;
  std::string name, author;
};
bool decodeSerum1(const uint8_t* p, size_t n, Serum1& out, std::string& err);
// The Serum 2 style document of a Serum 1 state (notes: what is approximated).
void serum1Root(const Serum1& s, Json& meta, Json& root, std::vector<std::string>& notes);
int serum1ParamCount();
const char* serum1ParamName(int i);

// Serum's filter type tokens in menu order (Serum 1 indices are the same).
int filterKeyCount();
const char* filterKey(int i);
// Wavetable warp tokens (Plasma warp order 0..69).
const char* warpKey(int i);

// Runs the importer on a document. files = false: no wavetable / sample
// lookups and no Model data (automation link probes).
void runImporter(const Json& meta, const Json& root, PluginState& out, ImportReport& rep, const std::vector<std::string>& roots,
                 bool files, const std::string& presetDir = {});

}  // namespace gl::packs::plasma::serum
