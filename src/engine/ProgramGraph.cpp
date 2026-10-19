// Gridline — mixer graph compilation: plugin delay compensation (PDC) and the
// task graph the engine runs (serially or on worker threads).
//
// Latency model. Every channel arrives at its mixer track with its
// instrument's latency. A track's input latency `latIn` is the largest
// arrival over all its inputs (channels, sends, send effects, sidechain
// routes, bus returns relative to their slot); its output adds the latency of
// its enabled effects (`latOwn`). Each input is delayed by `latIn - arrival`
// so everything meets aligned at every summing point; the master output is
// then `outputLatency` late, which offline renders skip.
//
// Determinism. Every track pulls its inputs in a fixed order (channels by
// index, then source tracks by processing order: their send effects by slot,
// then their sends), so the floating-point sums do not depend on which thread
// produced each input or when it finished.
#include <algorithm>
#include "Program.h"

namespace gl {

namespace {
constexpr int kMaxLatency = 1 << 20;  // ~22 s at 48 kHz; anything larger is a bogus report

int clampLatency(int v) { return std::clamp(v, 0, kMaxLatency); }
}  // namespace

bool ProgramBuilder::latencyChanged() const {
  for (const auto& l : liveLatency_) {
    int v = 0;
    if (l.fx && l.fx->fx) v = l.fx->fx->latencySamples();
    else if (l.gen && l.gen->gen) v = l.gen->gen->latencySamples();
    if (clampLatency(v) != l.value) return true;
  }
  return false;
}

bool ProgramBuilder::staleInstances() const {
  for (const auto& [id, c] : channels_)
    if (c && c->gen && c->gen->stale()) return true;
  for (const auto& [k, e] : effects_)
    if (e && e->fx && e->fx->stale()) return true;
  return false;
}

void ProgramBuilder::compileGraph(Program& prog) {
  const size_t M = prog.mixer.size(), C = prog.channels.size();
  liveLatency_.clear();

  // --- latencies ---
  std::vector<int> chanLat(C, 0);
  for (size_t c = 0; c < C; ++c) {
    const ProgChannel& ch = prog.channels[c];
    if (!ch.rt || !ch.rt->gen) continue;
    chanLat[c] = clampLatency(ch.rt->gen->latencyFor(ch.plugin, sampleRate_));
    if (isHostedType(ch.plugin.type)) liveLatency_.push_back({nullptr, ch.rt, chanLat[c]});
  }
  for (auto& m : prog.mixer) {
    m.latOwn = 0;
    for (auto& s : m.slots) {
      s.latency = 0;
      if (!s.rt || !s.rt->fx) continue;
      int lat = clampLatency(s.rt->fx->latencyFor(s.plugin, sampleRate_));
      if (isHostedType(s.plugin.type)) liveLatency_.push_back({s.rt, nullptr, lat});
      s.latency = s.enabled ? lat : 0;
      m.latOwn += s.latency;
    }
    // Manual track latency (FL's insert "delay" setting): reported like an
    // effect's, so compensation delays every other path by that much.
    m.latOwn = clampLatency(m.latOwn + m.latencyManual);
  }
  std::vector<int> pos(M, -1);
  for (size_t k = 0; k < prog.order.size(); ++k) {
    int j = prog.order[k];
    if (j >= 0 && static_cast<size_t>(j) < M && pos[static_cast<size_t>(j)] < 0) pos[static_cast<size_t>(j)] = static_cast<int>(k);
  }
  auto slotArrival = [&](size_t s, size_t k) {  // latency of track s's signal right after slot k
    int a = prog.mixer[s].latIn;
    for (size_t i = 0; i <= k && i < prog.mixer[s].slots.size(); ++i) a += prog.mixer[s].slots[i].latency;
    return a;
  };
  auto slotPre = [&](size_t j, size_t r) {  // latency added by track j's slots before slot r
    int a = 0;
    for (size_t i = 0; i < r && i < prog.mixer[j].slots.size(); ++i) a += prog.mixer[j].slots[i].latency;
    return a;
  };

  // --- inputs per track, in processing order ---
  struct Pending {
    ProgInput in;
    int arrival;
    int slot;  // bus returns: receiving slot; -1 otherwise
  };
  std::map<EdgeKey, EdgeLine> lines;
  auto chanTarget = [&](const ProgChannel& ch) {
    return static_cast<size_t>(ch.mixer) < M ? static_cast<size_t>(ch.mixer) : static_cast<size_t>(prog.master);
  };
  for (int jj : prog.order) {
    if (jj < 0 || static_cast<size_t>(jj) >= M) continue;
    const size_t j = static_cast<size_t>(jj);
    ProgMixer& dst = prog.mixer[j];
    dst.inputs.clear();
    dst.scInputs.clear();
    std::vector<Pending> main, sc, aux;
    for (size_t c = 0; c < C; ++c) {
      const ProgChannel& ch = prog.channels[c];
      if (!ch.rt || !ch.rt->gen) continue;
      Pending p;
      p.in.src = static_cast<int>(c);
      p.arrival = chanLat[c];
      p.slot = -1;
      if (chanTarget(ch) == j) {
        p.in.kind = ProgInput::Channel;
        main.push_back(p);
      }
      // Multi-output instruments: extra outputs routed to this track (summed
      // right after the channel's main output, as the serial engine did).
      for (size_t k = 0; k < ch.auxMixer.size(); ++k)
        if (ch.auxMixer[k] == jj) {
          p.in.kind = ProgInput::ChannelAux;
          p.in.index = static_cast<int>(k);
          main.push_back(p);
        }
    }
    for (int k = 0; k < pos[j]; ++k) {
      const int si = prog.order[static_cast<size_t>(k)];
      if (si < 0 || static_cast<size_t>(si) >= M || si == prog.master) continue;
      const ProgMixer& src = prog.mixer[static_cast<size_t>(si)];
      for (size_t q = 0; q < src.slots.size(); ++q)
        if (src.slots[q].sendTo == jj && src.slots[q].rt) {
          Pending p;
          p.in.kind = ProgInput::SlotSend;
          p.in.src = si;
          p.in.index = static_cast<int>(q);
          p.arrival = slotArrival(static_cast<size_t>(si), q);
          p.slot = -1;
          main.push_back(p);
        }
      for (size_t q = 0; q < src.slots.size(); ++q)  // multi-output send effects (band splitters…)
        for (int k = 0; k < kMaxSendOutputs; ++k)
          if (src.slots[q].multiSend[k] == jj && src.slots[q].rt) {
            Pending p;
            p.in.kind = ProgInput::SlotSend;
            p.in.src = si;
            p.in.index = static_cast<int>(q);
            p.in.out = k;
            p.arrival = slotArrival(static_cast<size_t>(si), q);
            p.slot = -2 - k;  // keeps the delay-line key apart from the slot's single send
            main.push_back(p);
          }
      for (size_t q = 0; q < src.sends.size(); ++q)
        if (src.sends[q].first == jj) {
          Pending p;
          p.in.kind = ProgInput::Send;
          p.in.src = si;
          p.in.index = static_cast<int>(q);
          p.arrival = src.latOut();
          p.slot = -1;
          main.push_back(p);
        }
      for (size_t q = 0; q < src.sidechain.size(); ++q)
        if (src.sidechain[q].first == jj) {
          Pending p;
          p.in.kind = ProgInput::Sidechain;
          p.in.src = si;
          p.in.index = static_cast<int>(q);
          p.arrival = src.latOut();
          p.slot = -1;
          sc.push_back(p);
        }
    }
    // Bus returns: every bus send that runs earlier (earlier tracks, then this
    // track's own slots before the return).
    for (size_t r = 0; r < dst.slots.size(); ++r) {
      dst.slots[r].auxIn.clear();
      const int bus = dst.slots[r].busRecv;
      if (bus < 0 || !dst.slots[r].rt) continue;
      for (int k = 0; k <= pos[j]; ++k) {
        const int si = prog.order[static_cast<size_t>(k)];
        if (si < 0 || static_cast<size_t>(si) >= M) continue;
        const bool self = si == jj;
        if (!self && si == prog.master) continue;
        const ProgMixer& src = prog.mixer[static_cast<size_t>(si)];
        for (size_t q = 0; q < src.slots.size() && (!self || q < r); ++q)
          if (src.slots[q].busSend == bus && src.slots[q].rt) {
            Pending p;
            p.in.kind = ProgInput::SlotSend;
            p.in.src = si;
            p.in.index = static_cast<int>(q);
            p.arrival = self ? 0 : slotArrival(static_cast<size_t>(si), q) - slotPre(j, r);  // relative to latIn
            p.slot = static_cast<int>(r);
            aux.push_back(p);
          }
      }
    }
    int latIn = 0;
    for (const auto& p : main) latIn = std::max(latIn, p.arrival);
    for (const auto& p : sc) latIn = std::max(latIn, p.arrival);
    for (const auto& p : aux)
      if (p.in.src != jj) latIn = std::max(latIn, p.arrival);
    dst.latIn = latIn;

    auto makeTap = [&](Pending& p, int kind) {
      int need = 0;
      if (p.in.kind == ProgInput::SlotSend && p.slot >= 0 && p.in.src == jj)  // own earlier bus send
        need = slotPre(j, static_cast<size_t>(p.slot)) -
               (slotArrival(j, static_cast<size_t>(p.in.index)) - latIn);
      else
        need = latIn - p.arrival;
      need = pdc_ ? std::max(0, need) : 0;
      const ObjectId srcId = p.in.fromChannel() ? prog.channels[static_cast<size_t>(p.in.src)].id
                                                             : prog.mixer[static_cast<size_t>(p.in.src)].id;
      EdgeKey key{dst.id, kind, srcId, p.in.index, p.slot};
      auto it = lines_.find(key);
      EdgeLine el;
      if (it != lines_.end()) el = it->second;
      if (need > 0 || el.line) {
        if (el.line && el.line->maxDelay() >= need) {
          if (el.old && el.line->adopted()) el.old.reset();
        } else {
          auto grown = std::make_shared<PdcDelay>(std::max(need * 2, need + kEngineMaxBlock), built_ ? 0 : need);
          el.old = el.line;  // the new line takes over its history on its first run
          el.line = std::move(grown);
        }
        p.in.tap.line = el.line.get();
        p.in.tap.adoptFrom = el.old.get();
        prog.pdcLines.push_back(el.line);
        if (el.old) prog.pdcLines.push_back(el.old);
        lines[key] = el;
      }
      p.in.tap.delay = need;
    };
    for (auto& p : main) {
      makeTap(p, static_cast<int>(p.in.kind));
      dst.inputs.push_back(p.in);
    }
    for (auto& p : sc) {
      makeTap(p, 10);
      dst.scInputs.push_back(p.in);
    }
    for (auto& p : aux) {
      makeTap(p, 20);
      dst.slots[static_cast<size_t>(p.slot)].auxIn.push_back(p.in);
    }
  }
  lines_ = std::move(lines);
  prog.pdc = pdc_;
  prog.outputLatency = pdc_ && static_cast<size_t>(prog.master) < M ? prog.mixer[static_cast<size_t>(prog.master)].latOut() : 0;

  // --- task graph ---
  ProgGraph& g = prog.graph;
  g.channels = static_cast<int>(C);
  g.tasks = static_cast<int>(C + M);
  g.deps.assign(static_cast<size_t>(g.tasks), -1);
  g.dependents.assign(static_cast<size_t>(g.tasks), {});
  g.serial.clear();
  g.mainOnly.assign(static_cast<size_t>(g.tasks), 0);
  for (size_t j = 0; j < M; ++j) g.mainOnly[C + j] = prog.mixer[j].threaded ? 0 : 1;
  for (size_t c = 0; c < C; ++c)
    if (prog.channels[c].rt && prog.channels[c].rt->gen) {
      g.deps[c] = 0;
      g.serial.push_back(static_cast<int>(c));
    }
  for (int jj : prog.order) {
    if (jj < 0 || static_cast<size_t>(jj) >= M || g.deps[C + static_cast<size_t>(jj)] >= 0) continue;
    const size_t j = static_cast<size_t>(jj);
    const int task = static_cast<int>(C + j);
    std::vector<int> preds;
    auto addPred = [&](const ProgInput& in) {
      int t = in.fromChannel() ? in.src : static_cast<int>(C) + in.src;
      if (t != task && std::find(preds.begin(), preds.end(), t) == preds.end()) preds.push_back(t);
    };
    for (const auto& in : prog.mixer[j].inputs) addPred(in);
    for (const auto& in : prog.mixer[j].scInputs) addPred(in);
    for (const auto& s : prog.mixer[j].slots)
      for (const auto& in : s.auxIn) addPred(in);
    // MIDI-input effects read the port bus that MIDI Out channels fill.
    bool midiIn = false;
    for (const auto& s : prog.mixer[j].slots)
      if (s.rt) {
        const PluginInfo* pi = findPlugin(s.plugin.type);
        midiIn = midiIn || (pi && pi->midiInput);
      }
    if (midiIn)
      for (size_t c = 0; c < C; ++c)
        if (prog.channels[c].rt && prog.channels[c].rt->gen && prog.channels[c].plugin.type == "gl.midiout") {
          const int t = static_cast<int>(c);
          if (std::find(preds.begin(), preds.end(), t) == preds.end()) preds.push_back(t);
        }
    g.deps[static_cast<size_t>(task)] = static_cast<int>(preds.size());
    for (int t : preds) g.dependents[static_cast<size_t>(t)].push_back(task);
    g.serial.push_back(task);
  }
  g.active = static_cast<int>(g.serial.size());
  g.pending.reset(new std::atomic<int>[static_cast<size_t>(std::max(1, g.tasks))]);
  g.ready.reset(new std::atomic<int>[static_cast<size_t>(std::max(1, g.active))]);
  for (int t = 0; t < g.tasks; ++t) g.pending[static_cast<size_t>(t)].store(0, std::memory_order_relaxed);
  for (int t = 0; t < g.active; ++t) g.ready[static_cast<size_t>(t)].store(-1, std::memory_order_relaxed);
  built_ = true;
}

}  // namespace gl
