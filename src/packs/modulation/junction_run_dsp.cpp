// Gridline - Junction graphs, version 3: the runtime (see junction.h). preparing runs on the
// UI thread and fills everything the audio thread needs; JRuntime::run never allocates, locks
// or does I/O. Work buffers, feedback and compensation lines live in the prepared graph; the
// plugin instances live in the runtime and stay across edits (matched by module id), so a
// module keeps its tails while the rest of the patch is rewired. Installed plugins are created
// by prepareHosted once and never recreated by an edit.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include "../../engine/NoteFx.h"
#include "../../engine/Program.h"
#include "../Packs.h"
#include "junction.h"

namespace gl::packs::modulation {

namespace {

constexpr int kFrames = 256;

uint64_t typeHash(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  return h;
}

enum NodeKind : uint8_t { KNone, KGen, KFx, KNote };

NodeKind kindOf(const PluginState& ps) {
  if (ps.type == "gl.patcher" || ps.type == "gl.patcherfx") return KNone;  // no nesting
  const PluginInfo* info = findPlugin(ps.type);
  if (!info) return KNone;
  return info->kind == PluginKind::Generator ? KGen : info->kind == PluginKind::NoteFx ? KNote : KFx;
}

// A delay line for plugin delay compensation of one cable.
struct DelayLine {
  std::vector<float> l, r;
  int pos = 0;
  void process(float* L, float* R, int n) {
    const int d = static_cast<int>(l.size());
    if (d <= 0) return;
    for (int k = 0; k < n; ++k) {
      const float a = l[static_cast<size_t>(pos)], b = r[static_cast<size_t>(pos)];
      l[static_cast<size_t>(pos)] = L[k], r[static_cast<size_t>(pos)] = R[k];
      L[k] = a, R[k] = b;
      if (++pos == d) pos = 0;
    }
  }
};

struct Edge {
  int src = -1;       // node index; -1 the host's audio input, -2 its sidechain
  int port = 0;
  int dst = -2;       // node index; -2 the host's outputs
  int dp = 0;
  mutable float gain = 1;
  int ring = -1;      // feedback: index of its ring
  int comp = -1;      // compensation: index of its delay line
};

// A value on its way to the running graph.
struct ValueMsg {
  int target = 0;     // node index, or edge index for a cable's level
  int param = 0;      // >= 0 a parameter; kMsgMix a module's mix; kMsgGain a cable's level
  float value = 0;
};
constexpr int kMsgMix = -1, kMsgGain = -2;
constexpr uint32_t kValueSlots = 4096;

struct Ctl {
  int src = -1;       // node index; -1 a host parameter
  int port = 0;
  int dst = 0;
  int param = -1;     // target's parameter index
  float lo = 0, hi = 1;
  const ParamDesc* desc = nullptr;
  const PatchV2Map* map = nullptr;  // imported link: FL's law
  bool fromImport = false;
};

struct NoteEdge {
  int src = -1;  // node index; -1 the host's notes
  int port = 0;
  int dst = 0;
};

struct RunNode {
  uint32_t id = 0;
  uint64_t hash = 0;
  NodeKind kind = KNone;
  bool hosted = false;
  JMode mode = JMode::On;
  mutable float mix = 1;
  int firstOut = 0;   // index of its output buffer
  int outs = 1;       // output buffers (main + extra)
  int noteBuf = -1;   // note processors: first out-buffer index (one per note output)
  bool wantsNotes = false;
  std::vector<int> audioIn, ctlIn, noteIn;
  ChannelAux aux;
  NoteFxAux naux;
};

struct Prepared final : AuxData {
  mutable JGraph graph;         // the UI thread's copy of what runs (values follow the edits)
  std::vector<RunNode> nodes;
  mutable std::vector<int> edgeOfLink;  // per graph link: its edge, -1 none
  mutable std::array<ValueMsg, kValueSlots> values;
  mutable std::atomic<uint32_t> valueWrite{0}, valueRead{0};
  std::vector<int> order;       // audio / control order
  std::vector<int> noteOrder;   // note processors in signal order
  std::vector<Edge> edges;
  std::vector<int> outEdges[1 + kMaxSendOutputs];
  std::vector<Ctl> ctls;
  std::vector<int> hostCtls;    // host parameter links (their sources are not modules)
  std::vector<NoteEdge> noteEdges;
  bool anyNotes = false;
  // Prepared instances (non-hosted), adopted by the runtime when it has none for the id.
  mutable std::vector<std::unique_ptr<Generator>> gens;
  mutable std::vector<std::unique_ptr<Effect>> fxs;
  mutable std::vector<std::unique_ptr<NoteFx>> nfxs;
  // Places for the instances of modules that left (kJMaxNodes each; freed with the graph).
  mutable std::vector<std::unique_ptr<Generator>> deadGens;
  mutable std::vector<std::unique_ptr<Effect>> deadFxs;
  mutable std::vector<std::unique_ptr<NoteFx>> deadNfxs;
  mutable std::vector<PluginState> live;
  mutable std::vector<float> lastCtl;
  // Work memory.
  mutable std::vector<float> bufL, bufR;        // output buffers × kFrames
  mutable std::vector<float> tmpL, tmpR, sideL, sideR, dryL, dryR, wrkL, wrkR, outL, outR;
  mutable std::vector<float> ringL, ringR;      // feedback rings × kJFeedbackFrames
  mutable int ringPos = 0;
  int rings = 0;
  mutable std::vector<int> ringSrc;             // output buffer each ring records
  mutable std::vector<DelayLine> comps;
  mutable std::vector<NoteBuffer> noteBufs;
  mutable NoteBuffer noteA, noteB;
  int totalLatency = 0;
};

// What prepareHosted measured last on this thread: latency of installed plugins per module.
thread_local const std::map<uint32_t, int>* tHostedLatency = nullptr;
thread_local const std::string* tHostedData = nullptr;
thread_local JRuntime* tOwner = nullptr;  // the host prepareHost just ran on; taken by the next prepare
std::atomic<double> gSampleRate{44100.0};
std::atomic<bool> gCompensate{true};  // off only in tests

int nodeLatency(const JNode& n, double sr, const std::map<uint32_t, int>* hosted) {
  if (!gCompensate.load()) return 0;
  if (n.mode != JMode::On) return 0;
  if (isHostedType(n.plugin.type)) {
    if (hosted) {
      auto it = hosted->find(n.id);
      if (it != hosted->end()) return it->second;
    }
    return 0;
  }
  const PluginInfo* info = findPlugin(n.plugin.type);
  if (!info || info->kind != PluginKind::Effect || n.mix < 0.5f) return 0;
  return gl::effectLatency(n.plugin, sr);
}

// Arrival of the signal at every module and at the host's outputs; cables that arrive early
// get a compensation delay.
struct Latency {
  std::vector<int> arrive;  // per node: latest input
  std::vector<int> leave;   // per node: arrive + its own latency
  int total = 0;
};

Latency latencyOf(const JGraph& g, const std::vector<int>& order, const std::vector<char>& feedback, double sr, const std::map<uint32_t, int>* hosted) {
  Latency r;
  const size_t N = g.nodes.size();
  r.arrive.assign(N, 0);
  r.leave.assign(N, 0);
  std::map<uint32_t, int> index;
  for (size_t i = 0; i < N; ++i) index[g.nodes[i].id] = static_cast<int>(i);
  for (int i : order) {
    int a = 0;
    for (size_t k = 0; k < g.links.size(); ++k) {
      const JLink& l = g.links[k];
      if (l.kind != JKind::Audio || feedback[k] || l.dst != g.nodes[static_cast<size_t>(i)].id || l.src == kJInputs) continue;
      const auto it = index.find(l.src);
      if (it != index.end()) a = std::max(a, r.leave[static_cast<size_t>(it->second)]);
    }
    r.arrive[static_cast<size_t>(i)] = a;
    r.leave[static_cast<size_t>(i)] = a + nodeLatency(g.nodes[static_cast<size_t>(i)], sr, hosted);
  }
  for (const JLink& l : g.links) {
    if (l.kind != JKind::Audio || l.dst != kJOutputs || l.dp != 0 || l.src == kJInputs) continue;
    const auto it = index.find(l.src);
    if (it != index.end()) r.total = std::max(r.total, r.leave[static_cast<size_t>(it->second)]);
  }
  return r;
}

// Processing order: Kahn over audio and param cables that are not feedback.
std::vector<int> orderOf(const JGraph& g, const std::vector<char>& feedback) {
  const size_t N = g.nodes.size();
  std::map<uint32_t, int> index;
  for (size_t i = 0; i < N; ++i) index[g.nodes[i].id] = static_cast<int>(i);
  std::vector<std::vector<int>> out(N);
  std::vector<int> indeg(N, 0);
  for (size_t k = 0; k < g.links.size(); ++k) {
    const JLink& l = g.links[k];
    if (feedback[k] || l.kind == JKind::Notes) continue;
    const auto a = index.find(l.src), b = index.find(l.dst);
    if (a == index.end() || b == index.end() || a->second == b->second) continue;
    out[static_cast<size_t>(a->second)].push_back(b->second);
    ++indeg[static_cast<size_t>(b->second)];
  }
  std::vector<int> order;
  std::vector<char> placed(N, 0);
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t k = 0; k < N; ++k) {
      if (placed[k] || indeg[k] > 0) continue;
      placed[k] = 1;
      order.push_back(static_cast<int>(k));
      for (int b : out[k]) --indeg[static_cast<size_t>(b)];
      progress = true;
    }
  }
  for (size_t k = 0; k < N; ++k)
    if (!placed[k]) order.push_back(static_cast<int>(k));
  return order;
}

}  // namespace

void prepareJunction(const PluginState& state, const JGraph& gIn, ChannelAux& aux) {
  auto a = std::make_shared<Prepared>();
  a->graph = gIn;
  JGraph& g = a->graph;
  if (static_cast<int>(g.nodes.size()) > kJMaxNodes) g.nodes.resize(static_cast<size_t>(kJMaxNodes));
  const size_t N = g.nodes.size();
  a->gens.resize(N);
  a->fxs.resize(N);
  a->nfxs.resize(N);
  a->deadGens.resize(kJMaxNodes);
  a->deadFxs.resize(kJMaxNodes);
  a->deadNfxs.resize(kJMaxNodes);
  a->nodes.resize(N);
  std::map<uint32_t, int> index;
  int nOut = 0;
  for (size_t i = 0; i < N; ++i) {
    JNode& gn = g.nodes[i];
    index[gn.id] = static_cast<int>(i);
    normalizePluginState(gn.plugin);
    RunNode& n = a->nodes[i];
    n.id = gn.id;
    n.hash = typeHash(gn.plugin.type);
    n.kind = kindOf(gn.plugin);
    n.hosted = isHostedType(gn.plugin.type);
    n.mode = gn.mode;
    n.mix = gn.mix;
    n.firstOut = nOut;
    const PluginInfo* info = findPlugin(gn.plugin.type);
    if (n.kind == KFx && info) n.outs = 1 + std::min<int>(static_cast<int>(info->sendParams.size()), kMaxSendOutputs);
    nOut += n.outs;
    if (n.kind == KGen) {
      prepareChannelAux(gn.plugin, n.aux);
      if (!n.hosted) {
        a->gens[i] = gl::createGenerator(gn.plugin.type);
        if (a->gens[i]) {
          a->gens[i]->prepareHost(gn.plugin, gSampleRate.load());
          warmUpBuiltin(*a->gens[i], gn.plugin.type, gn.plugin, n.aux, gSampleRate.load());  // K-59
        }
      }
    } else if (n.kind == KFx) {
      packs::prepareAux(gn.plugin, n.aux);
      if (!n.hosted) {
        a->fxs[i] = gl::createEffect(gn.plugin.type);
        if (a->fxs[i]) {
          a->fxs[i]->prepareHost(gn.plugin, gSampleRate.load());
          // Built-in effects size their buffers on their first block: that block runs here,
          // on the preparing thread, so an instance the audio path adopts is ready.
          ProcessContext warm;
          warm.sampleRate = gSampleRate.load();
          float wl[32] = {}, wr[32] = {};
          a->fxs[i]->processAux(gn.plugin, n.aux, warm, wl, wr, 32);
          a->fxs[i]->reset();
        }
      }
    } else if (n.kind == KNote) {
      a->nfxs[i] = gl::createNoteFx(gn.plugin.type);
      prepareNoteFxAux(gn.plugin, n.naux);
      for (int k = 0; k < NoteFxAux::kTargets; ++k) n.naux.targets[k] = k;  // output port k + 1
    }
    a->live.push_back(gn.plugin);
  }
  const std::vector<char> feedback = jFeedbackLinks(g);
  a->order = orderOf(g, feedback);
  // Latency.
  const std::map<uint32_t, int>* hostedLat = tHostedData && *tHostedData == state.data ? tHostedLatency : nullptr;
  const Latency lat = latencyOf(g, a->order, feedback, gSampleRate.load(), hostedLat);
  a->totalLatency = lat.total;
  // Cables.
  a->edgeOfLink.assign(g.links.size(), -1);
  for (size_t k = 0; k < g.links.size(); ++k) {
    const JLink& l = g.links[k];
    const auto si = index.find(l.src), di = index.find(l.dst);
    const int src = si == index.end() ? -1 : si->second, dst = di == index.end() ? -2 : di->second;
    if ((src == -1 && l.src != kJInputs) || (dst == -2 && l.dst != kJOutputs)) continue;
    if (l.kind == JKind::Audio) {
      Edge e;
      e.src = src == -1 ? (l.sp == 1 ? -2 : -1) : src;
      e.port = l.sp;
      e.dst = dst;
      e.dp = l.dp;
      e.gain = l.gain;
      const int leave = src >= 0 ? lat.leave[static_cast<size_t>(src)] : 0;
      const int arrive = dst >= 0 ? lat.arrive[static_cast<size_t>(dst)] : lat.total;
      if (feedback[k] && src >= 0) {
        e.ring = a->rings++;
        const RunNode& sn = a->nodes[static_cast<size_t>(src)];
        a->ringSrc.push_back(sn.firstOut + std::clamp(l.sp, 0, sn.outs - 1));
      } else if (arrive > leave) {
        const int d = std::min(arrive - leave, kJMaxLatency);
        DelayLine dl;
        dl.l.assign(static_cast<size_t>(d), 0.f);
        dl.r.assign(static_cast<size_t>(d), 0.f);
        e.comp = static_cast<int>(a->comps.size());
        a->comps.push_back(std::move(dl));
      }
      const int ei = static_cast<int>(a->edges.size());
      a->edgeOfLink[k] = ei;
      a->edges.push_back(e);
      if (dst >= 0) {
        a->nodes[static_cast<size_t>(dst)].audioIn.push_back(ei);
      } else if (l.dp >= 0 && l.dp <= kMaxSendOutputs) {
        a->outEdges[l.dp].push_back(ei);
      }
    } else if (l.kind == JKind::Param) {
      if (dst < 0) continue;
      Ctl c;
      c.src = src;
      c.port = l.sp;
      c.dst = dst;
      c.lo = l.lo, c.hi = l.hi;
      const JNode& tn = g.nodes[static_cast<size_t>(dst)];
      if (l.fl >= 0) {
        c.fromImport = true;
        c.map = tn.map(l.fl);
        c.param = c.map ? c.map->gl : -1;
      } else {
        c.param = l.dp;
      }
      const PluginInfo* info = findPlugin(tn.plugin.type);
      if (info && c.param >= 0 && static_cast<size_t>(c.param) < info->params.size()) c.desc = &info->params[static_cast<size_t>(c.param)];
      const int ci = static_cast<int>(a->ctls.size());
      a->ctls.push_back(c);
      if (src == -1) {
        a->hostCtls.push_back(ci);
      } else {
        a->nodes[static_cast<size_t>(dst)].ctlIn.push_back(ci);
      }
    } else {
      if (dst < 0) continue;
      if (src >= 0 && a->nodes[static_cast<size_t>(src)].kind != KNote) continue;  // only note processors send notes
      NoteEdge e;
      e.src = src;
      e.port = l.sp;
      e.dst = dst;
      const int ei = static_cast<int>(a->noteEdges.size());
      a->noteEdges.push_back(e);
      a->nodes[static_cast<size_t>(dst)].noteIn.push_back(ei);
      a->anyNotes = true;
    }
  }
  // Note processors in signal order; their output buffers.
  std::vector<int> pending;
  for (size_t i = 0; i < N; ++i)
    if (a->nodes[i].kind == KNote) pending.push_back(static_cast<int>(i));
  int noteBufs = 0;
  for (bool progress = true; progress && !pending.empty();) {
    progress = false;
    for (size_t p = 0; p < pending.size();) {
      const int i = pending[p];
      bool ready = true;
      for (int ei : a->nodes[static_cast<size_t>(i)].noteIn) {
        const int s = a->noteEdges[static_cast<size_t>(ei)].src;
        ready = ready && (s < 0 || std::find(a->noteOrder.begin(), a->noteOrder.end(), s) != a->noteOrder.end());
      }
      if (!ready) {
        ++p;
        continue;
      }
      a->noteOrder.push_back(i);
      a->nodes[static_cast<size_t>(i)].noteBuf = noteBufs;
      noteBufs += 1 + NoteFxAux::kTargets;
      pending.erase(pending.begin() + static_cast<long>(p));
      progress = true;
    }
  }
  if (noteBufs > 0) a->noteBufs.resize(static_cast<size_t>(noteBufs));
  a->bufL.assign(static_cast<size_t>(std::max(1, nOut)) * kFrames, 0.f);
  a->bufR.assign(static_cast<size_t>(std::max(1, nOut)) * kFrames, 0.f);
  for (auto* b : {&a->tmpL, &a->tmpR, &a->sideL, &a->sideR, &a->dryL, &a->dryR, &a->wrkL, &a->wrkR}) b->assign(kFrames, 0.f);
  a->outL.assign(static_cast<size_t>(1 + kMaxSendOutputs) * kFrames, 0.f);
  a->outR.assign(static_cast<size_t>(1 + kMaxSendOutputs) * kFrames, 0.f);
  a->ringL.assign(static_cast<size_t>(a->rings) * kJFeedbackFrames, 0.f);
  a->ringR.assign(static_cast<size_t>(a->rings) * kJFeedbackFrames, 0.f);
  a->lastCtl.assign(a->ctls.size(), std::nanf(""));
  for (size_t k = 0; k < a->ctls.size(); ++k) {
    const Ctl& c = a->ctls[k];
    // An imported host parameter starts at its target's value (nothing to apply).
    if (c.src == -1 && c.fromImport)
      a->lastCtl[k] = c.port >= 0 && static_cast<size_t>(c.port) < state.params.size() ? state.params[static_cast<size_t>(c.port)] : 0.f;
  }
  aux.custom = a;
}

void junctionCompensationForTests(bool on) { gCompensate.store(on); }

int junctionUnevenCables(const JGraph& g, double sr) {
  const std::vector<char> fb = jFeedbackLinks(g);
  const std::vector<int> order = orderOf(g, fb);
  const Latency lat = latencyOf(g, order, fb, sr, nullptr);
  std::map<uint32_t, int> index;
  for (size_t i = 0; i < g.nodes.size(); ++i) index[g.nodes[i].id] = static_cast<int>(i);
  int uneven = 0;
  for (size_t k = 0; k < g.links.size(); ++k) {
    const JLink& l = g.links[k];
    if (l.kind != JKind::Audio || fb[k]) continue;
    const auto s = index.find(l.src), d = index.find(l.dst);
    if ((s == index.end() && l.src != kJInputs) || (d == index.end() && l.dst != kJOutputs)) continue;
    const int leave = s != index.end() ? lat.leave[static_cast<size_t>(s->second)] : 0;
    const int arrive = d != index.end() ? lat.arrive[static_cast<size_t>(d->second)] : lat.total;
    if (arrive > leave) ++uneven;
  }
  return uneven;
}

int junctionLatency(const JGraph& g, double sr) {
  const std::vector<char> fb = jFeedbackLinks(g);
  return latencyOf(g, orderOf(g, fb), fb, sr, nullptr).total;
}

bool patchV2RunsAsGraph(const PatchV2& v2, double sr) {
  if (v2.run3) return true;
  if (!v2.align) return false;
  return jV2HasLevels(v2) || junctionLatency(junctionFromV2(v2), sr) > 0;
}

// ----------------------------------------------------------------- runtime
namespace {

// Installed plugin instances of one Junction: created and released on the UI thread.
struct HostedEntry {
  uint32_t id = 0;
  uint64_t hash = 0;
  std::shared_ptr<Generator> gen;
  std::shared_ptr<Effect> fx;
  int latency = 0;
};
struct HostedSet {
  std::vector<HostedEntry> entries;
  uint64_t serial = 0;
};

}  // namespace

struct JRuntime::Impl {
  const Prepared* synced = nullptr;
  // Instances by node index of the synced graph; the rest idle at the end until swapped out.
  std::array<uint32_t, kJMaxNodes> ids{};
  std::array<uint64_t, kJMaxNodes> hashes{};
  std::array<std::unique_ptr<Generator>, kJMaxNodes> gens;
  std::array<std::unique_ptr<Effect>, kJMaxNodes> fxs;
  std::array<std::unique_ptr<NoteFx>, kJMaxNodes> nfxs;
  // Scratch of sync().
  std::array<uint32_t, kJMaxNodes> oldIds{};
  std::array<uint64_t, kJMaxNodes> oldHashes{};
  std::array<std::unique_ptr<Generator>, kJMaxNodes> oldGens;
  std::array<std::unique_ptr<Effect>, kJMaxNodes> oldFxs;
  std::array<std::unique_ptr<NoteFx>, kJMaxNodes> oldNfxs;
  std::array<Generator*, kJMaxNodes> hgen{};
  std::array<Effect*, kJMaxNodes> hfx{};
  std::vector<float> outs;  // kJMaxNodes × kJMaxOutputs
  NoteBuffer hostNotes;
  uint32_t idCounter = 0;
  int64_t frames = 0;       // since this host started (note clock when stopped)
  int blockPos = 0;
  SideIo io;
  JStats stats;
  // Installed plugins.
  std::vector<std::shared_ptr<HostedSet>> retired;  // UI thread: sets the audio thread may still read
  std::shared_ptr<HostedSet> current;               // UI thread's newest
  std::atomic<HostedSet*> published{nullptr};
  std::atomic<uint64_t> ack{0};                     // serial the audio thread moved to
  HostedSet* seen = nullptr;                        // audio thread
  const Prepared* hostedFor = nullptr;              // audio thread: hgen / hfx were filled for this graph
  uint64_t serials = 0;
  std::map<uint32_t, int> latency;                  // UI thread
  // The graph this host runs, for edits that change values only (UI thread).
  std::shared_ptr<const Prepared> own;
  std::string ownData, ownKey;
  double ownRate = 0;
  std::map<uint32_t, int> ownLatency;
  std::atomic<const Prepared*> adopted{nullptr};    // written by sync()
  GraphCounts counts;
  Impl() { outs.assign(static_cast<size_t>(kJMaxNodes) * kJMaxOutputs, 0.f); }

  void refreshHosted(const Prepared& a) {
    HostedSet* s = published.load(std::memory_order_acquire);
    if (s == seen && hostedFor == &a) return;
    seen = s;
    hostedFor = &a;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
      hgen[i] = nullptr, hfx[i] = nullptr;
      if (!a.nodes[i].hosted || !s) continue;
      for (const HostedEntry& e : s->entries)
        if (e.id == a.nodes[i].id && e.hash == a.nodes[i].hash) {
          hgen[i] = e.gen.get();
          hfx[i] = e.fx.get();
        }
    }
    ack.store(s ? s->serial : 0, std::memory_order_release);
  }
  Generator* gen(size_t i) const { return gens[i] ? gens[i].get() : hgen[i]; }
  Effect* fx(size_t i) const { return fxs[i] ? fxs[i].get() : hfx[i]; }
};

JRuntime::JRuntime() : d_(std::make_unique<Impl>()) {}
JRuntime::~JRuntime() = default;
JStats& JRuntime::stats() { return d_->stats; }

bool JRuntime::sync(const AuxData* aux) {
  const auto* a = dynamic_cast<const Prepared*>(aux);
  if (!a) {
    d_->synced = nullptr;
    d_->adopted.store(nullptr, std::memory_order_release);
    return false;
  }
  if (a == d_->synced) return true;
  d_->synced = a;
  const size_t N = std::min(a->nodes.size(), static_cast<size_t>(kJMaxNodes));
  // Each module takes its running instance (same id and type) to its new index, or the
  // prepared one. What is left goes to the prepared graph, freed with it on the UI thread.
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i) {
    std::swap(d_->ids[i], d_->oldIds[i]);
    std::swap(d_->hashes[i], d_->oldHashes[i]);
    std::swap(d_->gens[i], d_->oldGens[i]);
    std::swap(d_->fxs[i], d_->oldFxs[i]);
    std::swap(d_->nfxs[i], d_->oldNfxs[i]);
    d_->ids[i] = 0;
    d_->hashes[i] = 0;
  }
  for (size_t i = 0; i < N; ++i) {
    const RunNode& n = a->nodes[i];
    d_->ids[i] = n.id;
    d_->hashes[i] = n.hash;
    bool found = false;
    for (size_t k = 0; k < static_cast<size_t>(kJMaxNodes) && !found; ++k)
      if (d_->oldIds[k] == n.id && d_->oldHashes[k] == n.hash) {
        std::swap(d_->gens[i], d_->oldGens[k]);
        std::swap(d_->fxs[i], d_->oldFxs[k]);
        std::swap(d_->nfxs[i], d_->oldNfxs[k]);
        d_->oldIds[k] = 0;
        found = d_->gens[i] || d_->fxs[i] || d_->nfxs[i];
      }
    if (!found) {
      std::swap(d_->gens[i], a->gens[i]);
      std::swap(d_->fxs[i], a->fxs[i]);
      std::swap(d_->nfxs[i], a->nfxs[i]);
    }
  }
  for (size_t k = 0; k < static_cast<size_t>(kJMaxNodes); ++k) {
    std::swap(a->deadGens[k], d_->oldGens[k]);
    std::swap(a->deadFxs[k], d_->oldFxs[k]);
    std::swap(a->deadNfxs[k], d_->oldNfxs[k]);
    d_->oldIds[k] = 0;
    d_->oldHashes[k] = 0;
  }
  d_->hostedFor = nullptr;
  d_->adopted.store(a, std::memory_order_release);
  return true;
}

void JRuntime::exchangeEffects(PatchV2Runtime& v2) {
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i)
    if (d_->ids[i] >= kJFirstId) {
      v2.exchangeEffect(static_cast<int>(d_->ids[i] - kJFirstId), d_->hashes[i], d_->fxs[i]);
      v2.exchangeInstrument(static_cast<int>(d_->ids[i] - kJFirstId), d_->hashes[i], d_->gens[i]);  // K-132: notes keep sounding
    }
}

void JRuntime::noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props) {
  NoteMsg m;
  m.kind = NoteMsg::On;
  m.id = id;
  m.pitch = static_cast<float>(pitch);
  m.velocity = velocity;
  m.pan = props.pan, m.release = props.release, m.modX = props.modX, m.modY = props.modY;
  m.color = static_cast<int16_t>(props.color);
  m.detune = props.detune;
  d_->hostNotes.push(m);
}

void JRuntime::noteOff(uint32_t id) {
  NoteMsg m;
  m.kind = NoteMsg::Off;
  m.id = id;
  d_->hostNotes.push(m);
}

void JRuntime::midi(uint8_t status, uint8_t d1, uint8_t d2) {
  const Prepared* a = d_->synced;
  if (!a) return;
  for (size_t i = 0; i < a->nodes.size() && i < static_cast<size_t>(kJMaxNodes); ++i)
    if (a->nodes[i].kind == KGen && !a->nodes[i].noteIn.empty())
      if (Generator* g = d_->gen(i)) g->midi(status, d1, d2, 0);
}

void JRuntime::glide(uint32_t id, double pitch, double seconds) {
  const Prepared* a = d_->synced;
  if (!a) return;
  for (size_t i = 0; i < a->nodes.size() && i < static_cast<size_t>(kJMaxNodes); ++i)
    if (a->nodes[i].kind == KGen && !a->nodes[i].noteIn.empty())
      if (Generator* g = d_->gen(i)) g->glide(id, pitch, seconds, 0);
}

void JRuntime::allOff() {
  d_->hostNotes.clear();
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i)
    if (Generator* g = d_->gen(i)) g->allNotesOff(0);
  for (auto& n : d_->nfxs)
    if (n) n->reset();
}

void JRuntime::stopSound() {
  allOff();
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i)
    if (Effect* f = d_->fx(i)) f->stopSound();
}

bool JRuntime::ownPlayback() const {
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i) {
    if (Generator* g = d_->gen(i); g && g->ownPlayback()) return true;
    if (Effect* f = d_->fx(i); f && f->ownPlayback()) return true;
  }
  return false;
}

void JRuntime::stopOwnPlayback() {
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i) {
    if (Generator* g = d_->gen(i); g && g->ownPlayback()) g->stopOwnPlayback();
    if (Effect* f = d_->fx(i); f && f->ownPlayback()) f->stopSound();
  }
}

void JRuntime::kill() {
  d_->hostNotes.clear();
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i) {
    if (Generator* g = d_->gen(i)) g->kill();
    if (Effect* f = d_->fx(i)) f->reset();
    if (d_->nfxs[i]) d_->nfxs[i]->reset();
  }
  if (const Prepared* a = d_->synced) {
    std::fill(a->ringL.begin(), a->ringL.end(), 0.f);
    std::fill(a->ringR.begin(), a->ringR.end(), 0.f);
    for (auto& c : a->comps) {
      std::fill(c.l.begin(), c.l.end(), 0.f);
      std::fill(c.r.begin(), c.r.end(), 0.f);
    }
  }
}

bool JRuntime::active() const {
  for (size_t i = 0; i < static_cast<size_t>(kJMaxNodes); ++i)
    if (Generator* g = d_->gen(i); g && g->active()) return true;
  return false;
}

void JRuntime::setSideIo(const SideIo& io) { d_->io = io; }
void JRuntime::beginBlock() { d_->blockPos = 0; }

int JRuntime::auxOutputCount() const {
  const Prepared* a = d_->synced;
  if (!a) return 0;
  int n = 0;
  for (int k = 1; k <= kMaxSendOutputs; ++k)
    if (!a->outEdges[k].empty()) n = k;
  return n;
}

bool JRuntime::auxOutput(int k, const float*& L, const float*& R) const {
  const Prepared* a = d_->synced;
  if (!a || k < 0 || k >= kMaxSendOutputs) return false;
  L = a->outL.data() + static_cast<size_t>(k + 1) * kFrames;
  R = a->outR.data() + static_cast<size_t>(k + 1) * kFrames;
  return !a->outEdges[k + 1].empty();
}

namespace {

inline void applyCtl(const Ctl& c, const Prepared& a, float v) {
  if (c.param < 0) return;
  PluginState& st = a.live[static_cast<size_t>(c.dst)];
  if (static_cast<size_t>(c.param) >= st.params.size()) return;
  if (c.fromImport) {
    if (c.map) st.params[static_cast<size_t>(c.param)] = static_cast<float>(c.map->eval(v));
  } else if (c.desc) {
    st.params[static_cast<size_t>(c.param)] = static_cast<float>(c.desc->fromNormalized(static_cast<double>(c.lo) + static_cast<double>(v) * (static_cast<double>(c.hi) - static_cast<double>(c.lo))));
  }
}

inline void deliver(Generator* g, const NoteMsg& m) {
  if (m.kind == NoteMsg::On) {
    NoteProps p;
    p.pan = m.pan, p.release = m.release, p.modX = m.modX, p.modY = m.modY;
    p.color = m.color;
    p.detune = m.detune;
    g->noteOn(m.id, static_cast<double>(m.pitch), m.velocity, m.offset, p);
  } else if (m.kind == NoteMsg::Off) {
    g->noteOff(m.id, m.offset);
  } else {
    g->allNotesOff(m.offset);
  }
}

}  // namespace

void JRuntime::run(const PluginState& host, const ProcessContext& ctx, float* L, float* R, int n, bool instrument) {
  const Prepared* ap = d_->synced;
  if (!ap) return;
  const Prepared& a = *ap;
  n = std::min(n, kFrames);
  d_->refreshHosted(a);
  {  // Values edited since the last block.
    uint32_t r = a.valueRead.load(std::memory_order_relaxed);
    const uint32_t w = a.valueWrite.load(std::memory_order_acquire);
    for (; r != w; ++r) {
      const ValueMsg& v = a.values[r % kValueSlots];
      if (v.param == kMsgGain) a.edges[static_cast<size_t>(v.target)].gain = v.value;
      else if (v.param == kMsgMix) a.nodes[static_cast<size_t>(v.target)].mix = v.value;
      else a.live[static_cast<size_t>(v.target)].params[static_cast<size_t>(v.param)] = v.value;
    }
    a.valueRead.store(r, std::memory_order_release);
  }
  const size_t N = std::min(a.nodes.size(), static_cast<size_t>(kJMaxNodes));
  const auto bytes = [](int f) { return sizeof(float) * static_cast<size_t>(f); };
  float* oLb = a.outL.data();
  float* oRb = a.outR.data();
  std::memset(oLb, 0, sizeof(float) * a.outL.size());
  std::memset(oRb, 0, sizeof(float) * a.outR.size());
  float* tmpL = a.tmpL.data();
  float* tmpR = a.tmpR.data();
  std::memcpy(tmpL, L, bytes(n));  // the host's input, for the whole call
  std::memcpy(tmpR, R, bytes(n));
  const bool measure = d_->stats.measure.load(std::memory_order_relaxed);
  for (int done = 0; done < n;) {
    const int m = n - done;
    // ---- Host parameters.
    for (int ci : a.hostCtls) {
      const Ctl& c = a.ctls[static_cast<size_t>(ci)];
      const float v = c.port >= 0 && static_cast<size_t>(c.port) < host.params.size() ? host.params[static_cast<size_t>(c.port)] : 0.f;
      if (v == a.lastCtl[static_cast<size_t>(ci)]) continue;
      a.lastCtl[static_cast<size_t>(ci)] = v;
      applyCtl(c, a, v);
    }
    // ---- Notes.
    if (a.anyNotes || d_->hostNotes.count > 0) {
      NoteFxContext nc;
      nc.sampleRate = ctx.sampleRate;
      nc.tempo = ctx.tempo;
      nc.ppq = ctx.ppq;
      nc.from = 0;
      nc.len = m;
      nc.spt = ctx.sampleRate * 60.0 / (std::max(1.0, ctx.tempo) * ctx.ppq);
      nc.playing = ctx.playing;
      if (ctx.playing) {
        nc.anchorTick = ctx.tick + static_cast<double>(d_->blockPos) / nc.spt;
        nc.clock = 0;
      } else {
        nc.clock = d_->frames;
      }
      nc.idCounter = &d_->idCounter;
      for (int i : a.noteOrder) {
        const size_t ui = static_cast<size_t>(i);
        const RunNode& rn = a.nodes[ui];
        NoteBuffer& in = a.noteA;
        in.clear();
        for (int ei : rn.noteIn) {
          const NoteEdge& e = a.noteEdges[static_cast<size_t>(ei)];
          if (e.src < 0) {
            for (int k = 0; k < d_->hostNotes.count; ++k) in.push(d_->hostNotes.ev[k]);
          } else {
            const RunNode& sn = a.nodes[static_cast<size_t>(e.src)];
            if (sn.noteBuf < 0) continue;
            const NoteBuffer& b = a.noteBufs[static_cast<size_t>(sn.noteBuf + std::clamp(e.port, 0, NoteFxAux::kTargets))];
            for (int k = 0; k < b.count; ++k) in.push(b.ev[k]);
          }
        }
        in.sort();
        NoteBuffer& out = a.noteB;
        out.clear();
        if (rn.mode == JMode::On && d_->nfxs[ui]) d_->nfxs[ui]->process(a.live[ui], rn.naux, nc, in, out);
        else if (rn.mode == JMode::Bypass) out = in;
        for (int p = 0; p <= NoteFxAux::kTargets; ++p) a.noteBufs[static_cast<size_t>(rn.noteBuf + p)].clear();
        for (int k = 0; k < out.count; ++k) {
          const NoteMsg& msg = out.ev[k];
          const int p = msg.target < 0 ? 0 : std::min<int>(msg.target + 1, NoteFxAux::kTargets);
          NoteMsg copy = msg;
          copy.target = -1;
          if (msg.kind == NoteMsg::AllOff) {
            for (int q = 0; q <= NoteFxAux::kTargets; ++q) a.noteBufs[static_cast<size_t>(rn.noteBuf + q)].push(copy);
          } else {
            a.noteBufs[static_cast<size_t>(rn.noteBuf + p)].push(copy);
          }
        }
      }
      for (size_t i = 0; i < N; ++i) {
        const RunNode& rn = a.nodes[i];
        if (rn.kind != KGen || rn.noteIn.empty()) continue;
        Generator* g = d_->gen(i);
        if (!g) continue;
        for (int ei : rn.noteIn) {
          const NoteEdge& e = a.noteEdges[static_cast<size_t>(ei)];
          if (e.src < 0) {
            for (int k = 0; k < d_->hostNotes.count; ++k) deliver(g, d_->hostNotes.ev[k]);
          } else {
            const RunNode& sn = a.nodes[static_cast<size_t>(e.src)];
            if (sn.noteBuf < 0) continue;
            const NoteBuffer& b = a.noteBufs[static_cast<size_t>(sn.noteBuf + std::clamp(e.port, 0, NoteFxAux::kTargets))];
            for (int k = 0; k < b.count; ++k) deliver(g, b.ev[k]);
          }
        }
      }
      d_->hostNotes.clear();
    }
    // ---- Modules.
    float* inL = tmpL + done;
    float* inR = tmpR + done;
    const auto mix = [&](const Edge& e, float* xL, float* xR) {
      // Adds what the cable carries into x.
      const float* sl = nullptr;
      const float* sr = nullptr;
      if (e.ring >= 0) {
        const float* rl = a.ringL.data() + static_cast<size_t>(e.ring) * kJFeedbackFrames;
        const float* rr = a.ringR.data() + static_cast<size_t>(e.ring) * kJFeedbackFrames;
        for (int k = 0; k < m; ++k) {
          const int p = (a.ringPos + k) % kJFeedbackFrames;
          // A feedback cable must not grow without bound.
          xL[k] += std::clamp(rl[p] * e.gain, -8.0f, 8.0f);
          xR[k] += std::clamp(rr[p] * e.gain, -8.0f, 8.0f);
        }
        return;
      }
      if (e.src == -1) {
        if (instrument) return;
        sl = inL, sr = inR;
      } else if (e.src == -2) {
        if (instrument || !d_->io.scL || !d_->io.scR) return;
        sl = d_->io.scL + d_->blockPos, sr = d_->io.scR + d_->blockPos;
      } else {
        const size_t si = static_cast<size_t>(e.src);
        const RunNode& sn = a.nodes[si];
        if (sn.kind == KGen && e.port > 0) {
          Generator* g = d_->gen(si);
          if (!g || e.port > g->auxOutputCount() || !g->auxOutput(e.port - 1, sl, sr) || !sl || !sr) return;
        } else {
          const size_t b = static_cast<size_t>(sn.firstOut + std::clamp(e.port, 0, sn.outs - 1)) * kFrames;
          sl = a.bufL.data() + b, sr = a.bufR.data() + b;
        }
      }
      if (e.comp >= 0) {
        float* wl = a.wrkL.data();
        float* wr = a.wrkR.data();
        std::memcpy(wl, sl, bytes(m));
        std::memcpy(wr, sr, bytes(m));
        a.comps[static_cast<size_t>(e.comp)].process(wl, wr, m);
        sl = wl, sr = wr;
      }
      if (e.gain == 1.0f) {
        for (int k = 0; k < m; ++k) xL[k] += sl[k], xR[k] += sr[k];
      } else {
        for (int k = 0; k < m; ++k) xL[k] += sl[k] * e.gain, xR[k] += sr[k] * e.gain;
      }
    };
    for (int i : a.order) {
      if (i < 0 || static_cast<size_t>(i) >= N) continue;
      const size_t ui = static_cast<size_t>(i);
      const RunNode& rn = a.nodes[ui];
      if (rn.kind == KNote) continue;
      for (int ci : rn.ctlIn) {
        const Ctl& c = a.ctls[static_cast<size_t>(ci)];
        if (c.src < 0 || c.port < 0 || c.port >= kJMaxOutputs) continue;
        const float v = d_->outs[static_cast<size_t>(c.src) * kJMaxOutputs + static_cast<size_t>(c.port)];
        if (v == a.lastCtl[static_cast<size_t>(ci)]) continue;
        a.lastCtl[static_cast<size_t>(ci)] = v;
        applyCtl(c, a, v);
      }
      const size_t base = static_cast<size_t>(rn.firstOut) * kFrames;
      float* bl = a.bufL.data() + base;
      float* br = a.bufR.data() + base;
      for (int p = 0; p < rn.outs; ++p) {
        std::memset(bl + static_cast<size_t>(p) * kFrames, 0, bytes(m));
        std::memset(br + static_cast<size_t>(p) * kFrames, 0, bytes(m));
      }
      const auto t0 = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (rn.kind == KGen) {
        Generator* g = d_->gen(ui);
        if (g) {
          if (rn.mode == JMode::On) g->process(a.live[ui], rn.aux, ctx, bl, br, m);
          g->controlOutputs(d_->outs.data() + ui * kJMaxOutputs, kJMaxOutputs);
        }
      } else if (rn.kind == KFx || rn.kind == KNone) {
        float* sl = nullptr;
        float* sr = nullptr;
        for (int ei : rn.audioIn) {
          const Edge& e = a.edges[static_cast<size_t>(ei)];
          if (e.dp == 1) {
            if (!sl) {
              sl = a.sideL.data(), sr = a.sideR.data();
              std::memset(sl, 0, bytes(m));
              std::memset(sr, 0, bytes(m));
            }
            mix(e, sl, sr);
          } else {
            mix(e, bl, br);
          }
        }
        Effect* f = rn.kind == KFx ? d_->fx(ui) : nullptr;
        if (rn.hosted && !f && rn.mode == JMode::On) {
          // An installed plugin missing here: the signal goes on (its state stays in the project).
        } else if (rn.mode == JMode::Mute || rn.kind == KNone || (!f && rn.mode == JMode::On)) {
          std::memset(bl, 0, bytes(m));
          std::memset(br, 0, bytes(m));
        } else if (rn.mode == JMode::On && f) {
          const bool blend = rn.mix < 1.0f;
          if (blend) {
            std::memcpy(a.dryL.data(), bl, bytes(m));
            std::memcpy(a.dryR.data(), br, bytes(m));
          }
          SideIo io;
          io.scL = sl;
          io.scR = sr;
          for (int p = 1; p < rn.outs && p <= kMaxSendOutputs; ++p) {
            io.multiL[p - 1] = bl + static_cast<size_t>(p) * kFrames;
            io.multiR[p - 1] = br + static_cast<size_t>(p) * kFrames;
          }
          f->setSideIo(io);
          f->processAux(a.live[ui], rn.aux, ctx, bl, br, m);
          if (blend)
            for (int k = 0; k < m; ++k) {
              bl[k] = a.dryL[static_cast<size_t>(k)] + (bl[k] - a.dryL[static_cast<size_t>(k)]) * rn.mix;
              br[k] = a.dryR[static_cast<size_t>(k)] + (br[k] - a.dryR[static_cast<size_t>(k)]) * rn.mix;
            }
        }
        if (f) f->controlOutputs(d_->outs.data() + ui * kJMaxOutputs, kJMaxOutputs);
      }
      float peak = 0;
      for (int k = 0; k < m; ++k) peak = std::max(peak, std::max(std::fabs(bl[k]), std::fabs(br[k])));
      d_->stats.id[ui].store(rn.id, std::memory_order_relaxed);
      d_->stats.level[ui].store(peak, std::memory_order_relaxed);
      if (measure) {
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float load = static_cast<float>(spent * ctx.sampleRate / std::max(1, m));
        const float old = d_->stats.load[ui].load(std::memory_order_relaxed);
        d_->stats.load[ui].store(old + (load - old) * 0.05f, std::memory_order_relaxed);
      }
    }
    // ---- Feedback rings record what their sources made in this step.
    for (int r = 0; r < a.rings; ++r) {
      float* rl = a.ringL.data() + static_cast<size_t>(r) * kJFeedbackFrames;
      float* rr = a.ringR.data() + static_cast<size_t>(r) * kJFeedbackFrames;
      const size_t b = static_cast<size_t>(a.ringSrc[static_cast<size_t>(r)]) * kFrames;
      const float* sl = a.bufL.data() + b;
      const float* sr = a.bufR.data() + b;
      for (int k = 0; k < m; ++k) {
        const int p = (a.ringPos + k) % kJFeedbackFrames;
        rl[p] = std::isfinite(sl[k]) ? sl[k] : 0.f;
        rr[p] = std::isfinite(sr[k]) ? sr[k] : 0.f;
      }
    }
    a.ringPos = (a.ringPos + m) % kJFeedbackFrames;
    // ---- Outputs.
    for (int p = 0; p <= kMaxSendOutputs; ++p) {
      float* ol = oLb + static_cast<size_t>(p) * kFrames + done;
      float* orr = oRb + static_cast<size_t>(p) * kFrames + done;
      for (int ei : a.outEdges[p]) mix(a.edges[static_cast<size_t>(ei)], ol, orr);
    }
    done += m;
    d_->frames += m;
    d_->blockPos += m;
  }
  if (instrument) {
    for (int k = 0; k < n; ++k) L[k] += oLb[k], R[k] += oRb[k];
  } else {
    std::memcpy(L, oLb, bytes(n));
    std::memcpy(R, oRb, bytes(n));
    for (int p = 1; p <= kMaxSendOutputs; ++p) {
      if (a.outEdges[p].empty() || !d_->io.multiL[p - 1] || !d_->io.multiR[p - 1]) continue;
      const float* ol = oLb + static_cast<size_t>(p) * kFrames;
      const float* orr = oRb + static_cast<size_t>(p) * kFrames;
      const int at = d_->blockPos - n;
      for (int k = 0; k < n; ++k) d_->io.multiL[p - 1][at + k] += ol[k], d_->io.multiR[p - 1][at + k] += orr[k];
    }
  }
}

// ------------------------------------------------- installed plugins (UI thread)
void JRuntime::prepareHosted(const PluginState& host, double sampleRate) {
  gSampleRate.store(sampleRate);
  tHostedLatency = nullptr;
  tHostedData = nullptr;
  tOwner = nullptr;
  if (!isJunction3(host.data)) {
    if (!isPatchV2(host.data)) return;
    if (host.data.find("\"run3\":1") == std::string::npos) {
      if (host.data.find("\"align\":1") == std::string::npos) return;
      PatchV2 v2;
      if (!parsePatchV2(host.data, v2) || !patchV2RunsAsGraph(v2, sampleRate)) return;
    }
  }
  tOwner = this;
  static thread_local std::map<uint32_t, int> latencyCopy;
  static thread_local std::string dataCopy;
  // Nothing changed for this host (an edit somewhere else in the project), and no installed
  // plugin to refresh: nothing to read.
  if (d_->own && d_->ownData == host.data && d_->ownRate == sampleRate && (!d_->current || d_->current->entries.empty())) {
    latencyCopy = d_->latency;
    dataCopy = host.data;
    tHostedLatency = &latencyCopy;
    tHostedData = &dataCopy;
    return;
  }
  const JGraph g = junctionOf(host);
  // Sets the audio thread has moved past can go.
  const uint64_t ack = d_->ack.load(std::memory_order_acquire);
  d_->retired.erase(std::remove_if(d_->retired.begin(), d_->retired.end(), [&](const std::shared_ptr<HostedSet>& s) { return s->serial < ack; }), d_->retired.end());
  auto next = std::make_shared<HostedSet>();
  next->serial = ++d_->serials;
  d_->latency.clear();
  bool changed = false;
  for (const JNode& n : g.nodes) {
    if (!isHostedType(n.plugin.type)) continue;
    const NodeKind kind = kindOf(n.plugin);
    if (kind != KGen && kind != KFx) continue;
    const uint64_t h = typeHash(n.plugin.type);
    HostedEntry e;
    if (d_->current)
      for (const HostedEntry& o : d_->current->entries)
        if (o.id == n.id && o.hash == h && !(o.gen && o.gen->stale()) && !(o.fx && o.fx->stale())) e = o;
    if (!e.gen && !e.fx) {
      e.id = n.id;
      e.hash = h;
      if (kind == KGen) e.gen = std::shared_ptr<Generator>(gl::createGenerator(n.plugin.type));
      else e.fx = std::shared_ptr<Effect>(gl::createEffect(n.plugin.type));
      changed = true;
    }
    PluginState ps = n.plugin;
    normalizePluginState(ps);
    if (e.gen) e.gen->prepareHost(ps, sampleRate);
    if (e.fx) e.fx->prepareHost(ps, sampleRate);
    e.latency = e.gen ? e.gen->latencyFor(ps, sampleRate) : e.fx ? e.fx->latencyFor(ps, sampleRate) : 0;
    d_->latency[n.id] = e.latency;
    next->entries.push_back(std::move(e));
  }
  if (d_->current && d_->current->entries.size() != next->entries.size()) changed = true;
  if (changed || !d_->current) {
    if (d_->current) d_->retired.push_back(d_->current);
    d_->current = next;
    d_->published.store(next.get(), std::memory_order_release);
  }
  latencyCopy = d_->latency;
  dataCopy = host.data;
  tHostedLatency = &latencyCopy;
  tHostedData = &dataCopy;
}

JRuntime::GraphCounts JRuntime::graphCounts() const { return d_->counts; }

namespace {

// What the graph's audio depends on beyond values: everything but parameter values, module
// mixes, cable levels and the look of the map.
std::string structureKey(JGraph g) {
  for (JNode& n : g.nodes) {
    n.plugin.params.clear();
    n.x = n.y = 0;
    n.name.clear();
    n.color = 0;
    n.mix = 1;
  }
  for (JLink& l : g.links) l.gain = 1;
  g.inX = g.inY = g.outX = g.outY = 0;
  g.controls.clear();
  g.snaps.clear();
  return serializeJunction(g);
}

bool holdsData(const ChannelAux& a) { return a.custom || a.ins || !a.samples.empty() || !a.slices.empty(); }

bool readGraph(const PluginState& state, JGraph& g) {
  if (isJunction3(state.data)) return parseJunction(state.data, g);
  if (isPatchV2(state.data)) {
    PatchV2 v2;
    if (!parsePatchV2(state.data, v2) || !patchV2RunsAsGraph(v2, gSampleRate.load())) return false;
    g = junctionFromV2(v2);
    return true;
  }
  return false;
}

}  // namespace

bool JRuntime::prepareFor(const PluginState& state, ChannelAux& aux) {
  Impl& d = *d_;
  const double sr = gSampleRate.load();
  const bool valid = d.own && d.ownRate == sr && d.ownLatency == d.latency;
  if (valid && d.ownData == state.data) {  // nothing changed for this graph
    aux.custom = d.own;
    ++d.counts.unchanged;
    return true;
  }
  JGraph g;
  if (!readGraph(state, g)) return false;
  // Values only, on the graph that is running: they travel to it; the graph stays.
  if (valid && d.adopted.load(std::memory_order_acquire) == d.own.get() && g.nodes.size() == d.own->graph.nodes.size() && g.links.size() == d.own->graph.links.size() &&
      structureKey(g) == d.ownKey) {
    const Prepared& a = *d.own;
    std::vector<ValueMsg> msgs;
    std::vector<size_t> touched;
    bool ok = g.nodes.size() <= static_cast<size_t>(kJMaxNodes);
    for (size_t i = 0; ok && i < g.nodes.size(); ++i) {
      JNode& next = g.nodes[i];
      const JNode& cur = a.graph.nodes[i];
      normalizePluginState(next.plugin);
      const RunNode& rn = a.nodes[i];
      if (next.mix != cur.mix) msgs.push_back({static_cast<int>(i), kMsgMix, next.mix});
      if (next.plugin.params != cur.plugin.params) {
        touched.push_back(i);
        // A plugin whose prepared data follows its values needs the rebuild.
        ChannelAux fresh;
        NoteFxAux freshNote;
        if (rn.kind == KGen) prepareChannelAux(next.plugin, fresh);
        else if (rn.kind == KFx) packs::prepareAux(next.plugin, fresh);
        else if (rn.kind == KNote) prepareNoteFxAux(next.plugin, freshNote);
        if (holdsData(fresh) || holdsData(rn.aux) || freshNote.custom || rn.naux.custom) ok = false;
        const size_t n = std::min(next.plugin.params.size(), cur.plugin.params.size());
        if (next.plugin.params.size() != cur.plugin.params.size()) ok = false;
        for (size_t k = 0; ok && k < n; ++k)
          if (next.plugin.params[k] != cur.plugin.params[k]) {
            msgs.push_back({static_cast<int>(i), static_cast<int>(k), next.plugin.params[k]});
            // A cable drives this parameter: the rebuild lets the cable take it back.
            for (const Ctl& c : a.ctls) ok = ok && !(c.dst == static_cast<int>(i) && c.param == static_cast<int>(k));
          }
      }
      // (A new latency moves the compensation delays.)
      if (ok && nodeLatency(next, sr, &d.latency) != nodeLatency(cur, sr, &d.latency)) ok = false;
    }
    for (size_t k = 0; ok && k < g.links.size(); ++k)
      if (g.links[k].gain != a.graph.links[k].gain && a.edgeOfLink[k] >= 0) msgs.push_back({a.edgeOfLink[k], kMsgGain, g.links[k].gain});
    const uint32_t w = a.valueWrite.load(std::memory_order_relaxed), r = a.valueRead.load(std::memory_order_acquire);
    if (ok && msgs.size() <= kValueSlots - (w - r)) {
      // Built-in instruments and effects reserve what the new values need before they arrive.
      for (size_t i : touched) {
        if (d.gens[i]) d.gens[i]->prepareHost(g.nodes[i].plugin, sr);
        if (d.fxs[i]) d.fxs[i]->prepareHost(g.nodes[i].plugin, sr);
      }
      uint32_t at = w;
      for (const ValueMsg& m : msgs) a.values[at++ % kValueSlots] = m;
      a.valueWrite.store(at, std::memory_order_release);
      a.graph = std::move(g);
      d.ownData = state.data;
      aux.custom = d.own;
      ++d.counts.valueEdits;
      return true;
    }
  }
  std::string key = structureKey(g);
  prepareJunction(state, g, aux);
  d.own = std::static_pointer_cast<const Prepared>(aux.custom);
  d.ownData = state.data;
  d.ownKey = std::move(key);
  d.ownRate = sr;
  d.ownLatency = d.latency;
  ++d.counts.builds;
  return true;
}

double junctionPrepareRate() { return gSampleRate.load(); }

bool prepareJunctionFor(const PluginState& state, ChannelAux& aux) {
  JRuntime* owner = tOwner;
  tOwner = nullptr;  // one prepare per prepareHost: another host never shares this graph
  if (owner && tHostedData && *tHostedData == state.data) return owner->prepareFor(state, aux);
  JGraph g;
  if (!readGraph(state, g)) return false;
  prepareJunction(state, g, aux);
  return true;
}

Generator* JRuntime::hostedGenerator(uint32_t node) const {
  if (d_->current)
    for (const HostedEntry& e : d_->current->entries)
      if (e.id == node) return e.gen.get();
  return nullptr;
}
Effect* JRuntime::hostedEffect(uint32_t node) const {
  if (d_->current)
    for (const HostedEntry& e : d_->current->entries)
      if (e.id == node) return e.fx.get();
  return nullptr;
}
int JRuntime::latency(const JGraph& g, double sampleRate) const {
  const std::vector<char> fb = jFeedbackLinks(g);
  return latencyOf(g, orderOf(g, fb), fb, sampleRate, &d_->latency).total;
}

}  // namespace gl::packs::modulation
