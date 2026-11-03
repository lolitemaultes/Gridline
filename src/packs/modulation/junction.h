// Gridline - Junction graphs, version 3 (K-7): a free graph of plugins the user wires by
// hand. Every plugin is a module with ports; three kinds of cable connect them:
//   audio:  stereo, with a gain per cable (a module's main and sidechain inputs, its main and
//           extra outputs, the host's audio input / sidechain / outputs);
//   notes:  the host's notes through note processors to instruments;
//   param:  a controller output or a host parameter (macro, exposed parameter) drives any
//           parameter of a module over a range.
// Rules for loops (jConnect, prepare): a note loop is refused. An audio cable closing a loop
// is a feedback cable: it carries the signal kJFeedbackFrames later, whatever the block size.
// A param cable closing a loop reads the value of the previous step.
//
// Data (JSON, PluginState::data): {"v":3,"next":…,"in":[x,y],"out":[x,y],"nodes":[…],
//   "links":[[kind,src,srcPort,dst,dstPort,gain,lo,hi,fl]…],"controls":[…],"snaps":[…]}
//
// Older data keeps running as it did (version 1 chains, version 2 FL patches): junctionOf()
// reads any of them as a graph for the editor, and the state only becomes version 3 when the
// wiring is edited.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Effects.h"
#include "../../engine/Generators.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "patchv2.h"

namespace gl::packs::modulation {

inline constexpr uint32_t kJInputs = 1, kJOutputs = 2, kJFirstId = 16;  // the host's two I/O modules
inline constexpr int kJMaxNodes = kV2MaxNodes;
inline constexpr int kJMaxOutputs = 64;        // controller outputs read per module
inline constexpr int kJFeedbackFrames = 256;   // delay of a feedback cable
inline constexpr int kJMaxLatency = 1 << 18;   // compensation per cable is capped here

enum class JKind : uint8_t { Audio = 0, Notes = 1, Param = 2 };
enum class JMode : uint8_t { On = 0, Bypass = 1, Mute = 2 };  // Bypass: inputs pass through. Mute: silent.

struct JLink {
  JKind kind = JKind::Audio;
  uint32_t src = 0;
  int sp = 0;  // source port: audio / notes output, controller output, or host parameter index
  uint32_t dst = 0;
  int dp = 0;  // destination port: audio input (0 main, 1 sidechain), notes input, or parameter index
  float gain = 1;        // audio
  float lo = 0, hi = 1;  // param: normalized range of the target
  int fl = -1;           // param: FL parameter index of an imported link (dp comes from the module's map)
  bool operator==(const JLink& o) const {
    return kind == o.kind && src == o.src && sp == o.sp && dst == o.dst && dp == o.dp && gain == o.gain && lo == o.lo && hi == o.hi && fl == o.fl;
  }
};

struct JNode {
  uint32_t id = 0;
  PluginState plugin;
  std::string name;  // the user's label (empty: the plugin's name)
  float x = 0, y = 0;
  int color = 0;     // 0: the role colour of its kind, else a palette entry
  JMode mode = JMode::On;
  float mix = 1;     // effects: dry / wet
  // FL import: what export writes back (see FlpEncFx2Mod.cpp).
  std::string flName;  // FL's plugin name
  std::string flOrig;  // the state imported, hex: template of the encoder
  std::string flRaw;   // the state when Gridline has no equivalent, hex: written back as is
  std::vector<PatchV2Map> maps;  // FL parameter → this plugin's parameter
  const PatchV2Map* map(int fl) const {
    for (const auto& m : maps)
      if (m.fl == fl) return &m;
    return nullptr;
  }
  bool operator==(const JNode& o) const {
    return id == o.id && plugin == o.plugin && name == o.name && x == o.x && y == o.y && color == o.color && mode == o.mode && mix == o.mix &&
           flName == o.flName && flOrig == o.flOrig && flRaw == o.flRaw && maps == o.maps;
  }
};

// A knob of the Panel page: a host parameter with its name.
struct JControl {
  int param = 0;  // host parameter index (macro k, or PhMap0 + k)
  std::string name;
  bool operator==(const JControl& o) const { return param == o.param && name == o.name; }
};
// A named set of Panel values.
struct JSnapshot {
  std::string name;
  std::vector<std::pair<int, float>> values;
  bool operator==(const JSnapshot& o) const { return name == o.name && values == o.values; }
};

struct JGraph {
  std::vector<JNode> nodes;
  std::vector<JLink> links;
  std::vector<JControl> controls;
  std::vector<JSnapshot> snaps;
  uint32_t next = kJFirstId;
  float inX = 40, inY = 160, outX = 640, outY = 160;
  const JNode* node(uint32_t id) const;
  JNode* node(uint32_t id);
  bool operator==(const JGraph& o) const {
    return nodes == o.nodes && links == o.links && controls == o.controls && snaps == o.snaps && next == o.next && inX == o.inX && inY == o.inY &&
           outX == o.outX && outY == o.outY;
  }
};

bool isJunction3(const std::string& data);
bool parseJunction(const std::string& data, JGraph& out);
std::string serializeJunction(const JGraph& g);

// Any host state (default, version 1, 2 or 3) as a graph.
JGraph junctionOf(const PluginState& state);
// Stores one module's plugin state; the data keeps its version (chain, FL patch or graph).
bool junctionSetNodeState(PluginState& host, uint32_t node, const PluginState& inner);
JGraph junctionFromChain(const PatchGraph& chain);
// `notRun`: routing the graph leaves out. `inserts`: FL mixer inserts of the track outputs (Outputs port k + 1).
JGraph junctionFromV2(const PatchV2& g, std::vector<std::string>* notRun = nullptr, std::vector<int>* inserts = nullptr);
// A module of an FL patch whose level is not 1 (FL's module level; the version 2 runtime has none).
bool jV2HasLevels(const PatchV2& g);

// ------------------------------------------------------------------- ports
struct JPort {
  JKind kind = JKind::Audio;
  bool out = false;
  int index = 0;
  std::string name;
};
// A module's ports. Param inputs are one port (index -1): the parameter is picked on connect.
// Controllers with many outputs get one port too (index -1, the output is picked on connect).
std::vector<JPort> jNodePorts(const JNode& n);
std::vector<JPort> jHostPorts(bool inputs, bool fxHost);
inline constexpr int kJManyOutputs = 6;  // more controller outputs than this share one port

// ------------------------------------------------------------- graph edits
uint32_t jAddNode(JGraph& g, const PluginState& plugin, float x, float y);
void jRemoveNode(JGraph& g, uint32_t id);
void jDisconnectNode(JGraph& g, uint32_t id);
uint32_t jDuplicateNode(JGraph& g, uint32_t id, float dx, float dy);
// Adds the cable when it is valid. A param input takes one source: a new one replaces the old.
// `why` says why not; a note loop and a repeated cable are refused.
bool jConnect(JGraph& g, const JLink& l, bool fxHost, std::string* why = nullptr);
// The cables of `g` that close a loop, as flags per link (audio and param kinds only).
std::vector<char> jFeedbackLinks(const JGraph& g);
// Layered left to right by signal order.
void jAutoArrange(JGraph& g);
// Makes a parameter of a module a host parameter (the first free slot) and returns its index, -1 when full.
int jExpose(JGraph& g, uint32_t node, int param, const std::string& name);
// Removes a Panel control and the cables it drives.
void jRemoveControl(JGraph& g, int param);
// Name of a host parameter on the Panel ("Macro 2" when unnamed).
std::string jControlName(const JGraph& g, int param);

// ------------------------------------------------------------ factory patches
struct JPatch {
  const char* name;
  bool forFx, forInstrument;
};
const std::vector<JPatch>& junctionPatches();
JGraph junctionPatch(size_t index, bool fxHost);

// ----------------------------------------------------------------- runtime
// Prepares graph data (version 3, or an FL patch that runs as a graph) for the host that was
// just given this state by prepareHost: edits that change values only reach the running graph
// without a rebuild. False when `state` is not graph data.
bool prepareJunctionFor(const PluginState& state, ChannelAux& aux);
void prepareJunction(const PluginState& state, const JGraph& g, ChannelAux& aux);
// An FL patch that runs in the graph runtime: routing the version 2 runtime can't run, or
// (imported as FL runs it, see PatchV2::align) a plugin with latency or a module level inside.
bool patchV2RunsAsGraph(const PatchV2& g, double sampleRate);
// Latency of the whole graph in samples (UI thread).
int junctionLatency(const JGraph& g, double sampleRate);
// Cables that get a compensating delay: parallel paths of different latency (UI thread).
int junctionUnevenCables(const JGraph& g, double sampleRate);
void junctionCompensationForTests(bool on);  // tests: compare with the version 2 runtime, which sums paths unaligned

struct JStats {
  static constexpr int kMax = kJMaxNodes;
  std::array<std::atomic<uint32_t>, kMax> id{};
  std::array<std::atomic<float>, kMax> level{};  // peak of the latest step
  std::array<std::atomic<float>, kMax> load{};   // fraction of real time (when measuring)
  std::atomic<bool> measure{false};
};

class JRuntime {
 public:
  JRuntime();
  ~JRuntime();
  bool sync(const AuxData* aux);  // adopts a prepared graph; false when `aux` is not version 3
  void noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props);
  void noteOff(uint32_t id);
  void midi(uint8_t status, uint8_t d1, uint8_t d2);
  void allOff();
  void stopSound();
  bool ownPlayback() const;
  void stopOwnPlayback();
  void kill();
  bool active() const;
  void setSideIo(const SideIo& io);
  // n <= 256 frames: instruments add into L / R, the effect version replaces them.
  void run(const PluginState& host, const ProcessContext& ctx, float* L, float* R, int n, bool instrument);
  void beginBlock();  // the block's position restarts (the host's process call)
  int auxOutputCount() const;
  bool auxOutput(int k, const float*& L, const float*& R) const;
  // Installed plugins (UI thread, every Program build): created once per module, kept
  // while the module stays, given the module's state.
  void prepareHosted(const PluginState& host, double sampleRate);
  Generator* hostedGenerator(uint32_t node) const;
  Effect* hostedEffect(uint32_t node) const;
  int latency(const JGraph& g, double sampleRate) const;  // with the installed plugins' own (UI thread)
  // How the Program builds reached this graph (UI thread): prepared anew, values sent to the
  // running graph, or nothing changed.
  struct GraphCounts {
    uint64_t builds = 0, valueEdits = 0, unchanged = 0;
  };
  GraphCounts graphCounts() const;
  bool prepareFor(const PluginState& state, ChannelAux& aux);  // see prepareJunctionFor
  JStats& stats();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// The running host behind a channel instrument / mixer slot (UI thread; null for other plugins).
JRuntime* junctionRuntime(Generator* g);
JRuntime* junctionRuntime(Effect* fx);

}  // namespace gl::packs::modulation
