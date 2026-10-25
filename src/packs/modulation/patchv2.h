// Gridline — Patch Host graphs, version 2 (J-41): FL Patcher patches as they
// are wired. Any number of hosted plugins (up to kV2MaxNodes) connected by
// audio links (with gains), note links and control links (a controller
// output driving a parameter), plus the patch's own exposed parameters
// (host "Param" slots). The FL patch layout (I/O ports, links, editor
// layout, port lists) is kept as FL stores it (raw chunks); the plugins
// themselves are Gridline states, re-encoded on export.
//
// data (JSON): {"v":2, "ver":<state version>, "items":[ "<hex chunk>" | "#<node>" … ],
//   "nodes":[ {"type","params","data","ins","fl","disp","id","head","tail",
//              "maps":[[fl, gl, lo, hi] | [fl, gl, [table…]] …]} … ],
//   "slots":[<FL parameter index of host Param slot k> …] }
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"
#include "../../engine/Generators.h"

namespace gl::packs::modulation {

inline constexpr int kV2MaxNodes = 256;
inline constexpr int kV2MaxOutputs = 16;  // controller outputs read per node

// FL parameter (normalized 0..1) → Gridline parameter value of a node.
struct PatchV2Map {
  int fl = -1, gl = -1;
  double lo = 0, hi = 1;     // linear law
  std::vector<float> tab;    // else a table over 0..1 (linear interpolation; integers rounded)
  bool integer = false;
  double eval(double n) const;
  double invert(double v) const;  // the FL value giving v (closest)
  bool operator==(const PatchV2Map& o) const { return fl == o.fl && gl == o.gl && lo == o.lo && hi == o.hi && tab == o.tab && integer == o.integer; }
};

struct PatchV2Node {
  PluginState plugin;
  std::string flName, display;
  uint64_t id = 0;
  std::string head, tail;  // FL node chunks before / after the plugin state (hex; tail "-" = no state chunk)
  std::string raw;         // the FL plugin state when Gridline has no equivalent (hex), written back as is
  std::string orig;        // the FL plugin state it was imported from (hex): the encoder's template
  std::vector<PatchV2Map> maps;
  const PatchV2Map* map(int fl) const;
  bool operator==(const PatchV2Node& o) const;
};

struct PatchV2 {
  uint32_t version = 4;
  std::vector<std::string> items;  // hex chunks, or "#n" for node n
  std::vector<PatchV2Node> nodes;
  std::vector<int> slots;          // host Param slot → FL parameter index of the patch
  bool operator==(const PatchV2& o) const;
};

bool isPatchV2(const std::string& data);
bool parsePatchV2(const std::string& data, PatchV2& out);
std::string serializePatchV2(const PatchV2& g);

// Wiring read from the FL chunks.
struct PatchV2Topo {
  struct AEdge {
    int src = -1, dst = -2;  // node, or −1 = host audio input / −2 = host audio output
    float gain = 1;
  };
  struct CEdge {
    int src = -1, out = 0;  // controller node and output
    int dst = -1, fl = 0;   // target node and FL parameter
  };
  struct Slot {
    int node = -1, fl = 0;  // target of host Param slot k
  };
  std::vector<AEdge> audio;
  std::vector<CEdge> controls;
  std::vector<char> notes;  // per node: plays the host's notes
  std::vector<Slot> slots;
  std::vector<int> order;   // processing order (audio and control sources first)
  std::vector<std::string> unsupported;  // links / ports Gridline does not run
};
PatchV2Topo patchV2Topology(const PatchV2& g);

// FL chunk helpers (shared with the FL decoder / encoder).
struct PatchChunk {
  uint32_t id = 0;
  std::vector<uint8_t> data;
};
std::vector<PatchChunk> patchChunks(const uint8_t* p, size_t n);
std::string patchHex(const std::vector<uint8_t>& b);
std::vector<uint8_t> patchUnhex(const std::string& s);
std::vector<uint8_t> patchChunkBytes(uint32_t id, const std::vector<uint8_t>& payload);

// Runtime prepare (UI thread) / creation; used by the Patch Host for v2 data.
void preparePatchV2(const PluginState& state, const PatchV2& g, ChannelAux& aux);

// The graph runtime of one Patch Host instance (audio thread; buffers are
// reserved at construction, hosted instances come prepared with the aux).
class PatchV2Runtime {
 public:
  PatchV2Runtime();
  ~PatchV2Runtime();
  // Adopts a newly prepared graph (keeps running instances whose node type
  // is unchanged). Returns false when `aux` is not a v2 graph.
  bool sync(const AuxData* aux);
  void noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props);
  void noteOff(uint32_t id);
  void allOff();
  // J-110: the user's Stop: every note off, hosted effects end their own playback.
  void stopSound();
  void kill();
  bool active() const;
  // Renders n (<= 256) frames: instruments add into L / R (`instrument`),
  // the effect version replaces L / R with the patch output.
  void run(const PluginState& host, const ProcessContext& ctx, float* L, float* R, int n, bool instrument);

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace gl::packs::modulation
