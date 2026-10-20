// Gridline — FLP import, second wave: Patcher (effect and instrument) → Patch
// Host (gl.patcherfx / gl.patcher).
//
// State layout: u32 version (3, 4), then items, each a chunk
//   { u32 id, u64 size, payload }.
// Items with id 0 are containers holding a chunk list:
//   plugin node   5000 header (u32 kind, u64 node id, …), 5004 file path,
//                 5008 internal name, 5010 display name (ANSI or UTF-16),
//                 5005 the plugin's own state (as in a mixer slot), 5007 (f32
//                 mix, …), 5009 ports (u32 ?, u32 index, u64 port id, u32 kind:
//                 3 = audio in, 2 = audio out, others = event / parameter
//                 ports), 5001 end.
//   I/O node      1400 (u64 id, …, u32 kind: 7 = audio input, 6 = audio
//                 output), 1402 name, 1401 end.
//   link          1300 (u64 destination port, u64 source port, f32 gain, …),
//                 1301 end.
//   layout        1600 (u64 node id, f32 x, f32 y, …).
// Gridline's host runs a chain of up to eight plugins where an effect can run
// in parallel with the previous one, so the graph is linearised: nodes in
// signal order, siblings fed by the same sources marked parallel.
#include <cstring>
#include <set>
#include "../Json.h"
#include "../../packs/modulation/modulation.h"
#include "../../packs/modulation/modulation_dsp.h"
#include "../../packs/modulation/patchv2.h"
#include "FlpFx2.h"

namespace gl::flp::fx2 {

namespace {

constexpr int kMaxNodes = 8;  // Patch Host graph size

struct Chunk {
  uint32_t id = 0;
  const uint8_t* p = nullptr;
  size_t n = 0;
};

std::vector<Chunk> chunks(const uint8_t* p, size_t n) {
  std::vector<Chunk> out;
  size_t o = 0;
  while (o + 12 <= n) {
    Reader r(p + o, n - o);
    Chunk c;
    c.id = r.u32();
    uint64_t lo = r.u32(), hi = r.u32();
    uint64_t size = lo | (hi << 32);
    if (size > n - o - 12) break;
    c.p = p + o + 12;
    c.n = static_cast<size_t>(size);
    out.push_back(c);
    o += 12 + c.n;
  }
  return out;
}

std::string chunkText(const Chunk& c) {
  bool wide = c.n >= 2 && c.n % 2 == 0;
  for (size_t i = 1; wide && i < c.n; i += 2)
    if (c.p[i] != 0) wide = false;
  return wide ? utf16(c.p, c.n / 2) : latin1(c.p, c.n);
}

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
  return at + 4 <= b.size() ? static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16) |
                                  (static_cast<uint32_t>(b[at + 3]) << 24)
                            : 0u;
}

uint64_t u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

struct Node {
  uint64_t id = 0;
  std::string name, display;
  const uint8_t* state = nullptr;
  size_t stateSize = 0;
  float mix = 1.0f;
  std::set<int> preds;  // node indices feeding this node (-1 = audio input)
  bool toOutput = false;
};


// ---------------------------------------------------------------- v2 (J-41)
// The patch as FL wires it (packs/modulation/patchv2.h): every hosted plugin
// decoded to its Gridline state, the FL layout chunks (I/O, links, exposed
// parameters, editor) kept, control-link and exposed-parameter targets
// resolved through each inner decoder's parameter table. The instrument and
// effect versions share it.
namespace pv2 = gl::packs::modulation;
namespace md = gl::packs::modulation;

std::string nameFromDllPath(const std::string& path) {
  std::string p = path;
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const size_t slash = p.rfind('/');
  if (slash == std::string::npos) return p;
  const std::string dir = p.substr(0, slash);
  const size_t s2 = dir.rfind('/');
  return s2 == std::string::npos ? dir : dir.substr(s2 + 1);
}

std::string chunkString(const std::vector<uint8_t>& d) {
  const bool wide = d.size() >= 2 && d.size() % 2 == 0 && d[1] == 0;
  return wide ? utf16(d.data(), d.size() / 2) : latin1(d.data(), d.size());
}

// FL normalized → Gridline value through an inner decoder's table.
bool buildMap(const PluginMapping& im, int fl, pv2::PatchV2Map& out) {
  if (!im.param || !im.value) return false;
  const int gl = im.param(fl);
  if (gl < 0) return false;
  constexpr int kN = 256;
  std::vector<double> v(kN + 1);
  for (int i = 0; i <= kN; ++i) v[static_cast<size_t>(i)] = im.value(fl, static_cast<double>(i) / kN);
  for (double x : v)
    if (!std::isfinite(x)) return false;
  out.fl = fl, out.gl = gl;
  bool integer = true, linear = true;
  const double lo = v.front(), hi = v.back(), tol = 1e-6 * std::max(1.0, std::fabs(hi - lo));
  for (int i = 0; i <= kN; ++i) {
    const double x = v[static_cast<size_t>(i)];
    if (x != std::round(x)) integer = false;
    if (std::fabs(x - (lo + (hi - lo) * i / kN)) > tol) linear = false;
  }
  out.integer = integer && !linear;
  if (linear) {
    out.lo = lo, out.hi = hi;
  } else {
    for (double x : v) out.tab.push_back(static_cast<float>(x));
  }
  return true;
}

// A patch the chain host (graph version 1) runs exactly as wired: up to eight
// plugins, no controller links or exposed parameters, unit link gains,
// instruments on the note input, effects in stages (every node of a stage fed
// by exactly the previous stage's outputs), the last stage to the output.
// Gridline's own exported chains come back as they were written.
bool chainOf(const pv2::PatchV2& g, const pv2::PatchV2Topo& t, bool instrument, md::PatchGraph& out) {
  const int N = static_cast<int>(g.nodes.size());
  if (N > md::kPatchNodes || !t.controls.empty() || !g.slots.empty() || !t.unsupported.empty()) return false;
  for (const auto& e : t.audio)
    if (e.gain != 1.0f) return false;
  std::vector<int> gens, fxs;
  for (int i = 0; i < N; ++i) {
    const PluginInfo* info = findPlugin(g.nodes[static_cast<size_t>(i)].plugin.type);
    if (!info || info->kind == PluginKind::NoteFx || g.nodes[static_cast<size_t>(i)].plugin.type == "gl.patcher" ||
        g.nodes[static_cast<size_t>(i)].plugin.type == "gl.patcherfx" || !g.nodes[static_cast<size_t>(i)].raw.empty())
      return false;
    if (info->kind == PluginKind::Generator) {
      if (!instrument || !t.notes[static_cast<size_t>(i)]) return false;
      gens.push_back(i);
    } else {
      if (t.notes[static_cast<size_t>(i)]) return false;
      fxs.push_back(i);
    }
  }
  std::vector<std::set<int>> srcs(static_cast<size_t>(N));
  std::set<int> outSrcs;
  for (const auto& e : t.audio) {
    if (e.dst == -2) outSrcs.insert(e.src);
    else srcs[static_cast<size_t>(e.dst)].insert(e.src);
  }
  for (int i : gens)
    if (!srcs[static_cast<size_t>(i)].empty()) return false;
  std::set<int> cur;
  if (instrument) cur.insert(gens.begin(), gens.end());
  else cur.insert(-1);
  std::vector<std::vector<int>> stages;
  std::vector<char> done(static_cast<size_t>(N), 0);
  for (size_t left = fxs.size(); left > 0;) {
    std::vector<int> st;
    for (int i : fxs)  // (an instrument patch without instruments: its first stage takes no input)
      if (!done[static_cast<size_t>(i)] && (!srcs[static_cast<size_t>(i)].empty() || cur.empty()) && srcs[static_cast<size_t>(i)] == cur)
        st.push_back(i);
    if (st.empty()) return false;
    for (int i : st) done[static_cast<size_t>(i)] = 1;
    left -= st.size();
    cur = std::set<int>(st.begin(), st.end());
    stages.push_back(std::move(st));
  }
  if (outSrcs != cur) return false;
  auto mixOf = [&](int i) {
    std::vector<uint8_t> h = pv2::patchUnhex(g.nodes[static_cast<size_t>(i)].head);
    if (g.nodes[static_cast<size_t>(i)].tail != "-") {
      const std::vector<uint8_t> t2 = pv2::patchUnhex(g.nodes[static_cast<size_t>(i)].tail);
      h.insert(h.end(), t2.begin(), t2.end());
    }
    for (const auto& ch : pv2::patchChunks(h.data(), h.size()))
      if (ch.id == 5007 && ch.data.size() >= 4) {
        float f;
        std::memcpy(&f, ch.data.data(), 4);
        return std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 1.0f;
      }
    return 1.0f;
  };
  out = md::PatchGraph{};
  for (int i : gens) {
    md::PatchNode n;
    n.plugin = g.nodes[static_cast<size_t>(i)].plugin;
    n.mix = mixOf(i);
    out.nodes.push_back(n);
  }
  for (const auto& st : stages)
    for (size_t k = 0; k < st.size(); ++k) {
      md::PatchNode n;
      n.plugin = g.nodes[static_cast<size_t>(st[k])].plugin;
      n.parallel = k > 0;
      n.mix = mixOf(st[k]);
      out.nodes.push_back(n);
    }
  return true;
}

}  // namespace

bool decodePatcherV2(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 16) return false;
  pv2::PatchV2 g;
  g.version = rd32(b, 0);
  if (g.version < 3 || g.version > 4) return false;
  const std::vector<pv2::PatchChunk> top = pv2::patchChunks(b.data() + 4, b.size() - 4);
  {  // the chunk list must cover the state
    size_t total = 4;
    for (const auto& t : top) total += 12 + t.data.size();
    if (total != b.size()) return false;
  }
  bool exact = true, hasNotes = false, hasAudioIn = false;
  std::vector<PluginMapping> inner;
  std::vector<size_t> standIn;  // undecodable instruments
  for (const auto& t : top) {
    std::vector<pv2::PatchChunk> kids;
    if (t.id == 0) kids = pv2::patchChunks(t.data.data(), t.data.size());
    if (t.id == 0 && !kids.empty() && kids[0].id == 1400) {
      if (kids[0].data.size() >= 16 && rd32(kids[0].data, 12) == 7) {  // an input port: notes (group 1) / audio (group 0)
        if (rd32(kids[0].data, 8) == 1) hasNotes = true;
        else hasAudioIn = true;
      }
      for (const auto& k : kids)
        if (k.id == 1402) {
          const std::string nm = chunkString(k.data);
          hasNotes = hasNotes || nm == "Notes";
          hasAudioIn = hasAudioIn || nm == "Audio input";
        }
    }
    if (t.id != 0 || kids.empty() || kids[0].id != 5000) {
      g.items.push_back(pv2::patchHex(pv2::patchChunkBytes(t.id, t.data)));
      continue;
    }
    pv2::PatchV2Node n;
    n.id = kids[0].data.size() >= 12 ? (static_cast<uint64_t>(rd32(kids[0].data, 4)) | (static_cast<uint64_t>(rd32(kids[0].data, 8)) << 32)) : 0;
    std::vector<uint8_t> head, tail, state;
    bool seen = false;
    std::string plug, internal, disp;
    for (const auto& k : kids) {
      if (k.id == 5005 && !seen) {
        state = k.data;
        seen = true;
        continue;
      }
      std::vector<uint8_t> bytes = pv2::patchChunkBytes(k.id, k.data);
      (seen ? tail : head).insert((seen ? tail : head).end(), bytes.begin(), bytes.end());
      if (k.id == 5004) plug = chunkString(k.data);
      else if (k.id == 5008) internal = chunkString(k.data);
      else if (k.id == 5010) disp = chunkString(k.data);
    }
    std::string name = internal.empty() ? plug : internal;
    if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos) name = nameFromDllPath(name);
    n.flName = name;
    n.display = disp;
    n.head = pv2::patchHex(head);
    n.tail = pv2::patchHex(tail);
    if (!seen) n.tail = "", n.head = pv2::patchHex(head);
    PluginMapping im;
    const bool ok = seen && decodeEmbedded(c, name, state.data(), state.size(), im) && im.glType != "gl.patcher" && im.glType != "gl.patcherfx";
    if (ok) {
      n.plugin = im.state;
      n.orig = pv2::patchHex(state);  // the export template (unchanged plugins are written as they were)
      exact = exact && im.exact;
      if (!im.exact) c.report.warn("Patcher: inner plugin " + name + " decoded approximately");
    } else {
      exact = false;
      c.report.warn("Patcher: \"" + (disp.empty() ? name : disp) + "\" (" + name + ") has no Gridline equivalent; left out");
      im = PluginMapping{};
      if (seen) n.raw = pv2::patchHex(state);  // kept for export
      const uint32_t kind = kids[0].data.size() >= 4 ? rd32(kids[0].data, 0) : 0;  // 2 effect, 3 instrument
      bool looksFx = kind == 2;
      for (const char* pfx : {"Fruity", "Control Surface", "Maximus", "Soundgoodizer", "Gross Beat", "Edison", "Pitcher"})
        looksFx = looksFx || name.rfind(pfx, 0) == 0;
      if (kind == 3 || !looksFx) standIn.push_back(g.nodes.size());
    }
    if (!seen) n.tail = "-";  // (no state chunk)
    g.items.push_back("#" + std::to_string(g.nodes.size()));
    g.nodes.push_back(std::move(n));
    inner.push_back(std::move(im));
  }
  const bool instrument = isChannelPlugin(c, evs) || hasNotes || !hasAudioIn;
  // An instrument patch whose instruments Gridline cannot decode still has to
  // sound: the first one becomes a stand-in synth (its FL state is kept).
  if (instrument && !standIn.empty()) {
    bool anyGen = false;
    for (const auto& n : g.nodes) {
      const PluginInfo* info = findPlugin(n.plugin.type);
      anyGen = anyGen || (info && info->kind == PluginKind::Generator);
    }
    if (!anyGen) g.nodes[standIn.front()].plugin = defaultPluginState("gl.synth");
  }
  // Exposed parameters (1100: port, FL index), in FL's order → host Param slots.
  for (const auto& t : top)
    if (t.id == 1100 && t.data.size() >= 12) {
      if (g.slots.size() >= static_cast<size_t>(pv2::kPatchMaps)) {
        exact = false;
        break;
      }
      g.slots.push_back(static_cast<int>(rd32(t.data, 8)));
    }
  pv2::PatchV2Topo topo = pv2::patchV2Topology(g);
  for (const auto& u : topo.unsupported) {
    exact = false;
    c.report.warn("Patcher: " + u + " not run");
  }
  // Surfaces: their ports must address existing value controls (FL keeps
  // ports of controls that were removed; those do not run here).
  auto surfaceParams = [&](int node) {
    return pv2::surfaceControlCount(g.nodes[static_cast<size_t>(node)].plugin.data);
  };
  auto isSurface = [&](int node) { return node >= 0 && static_cast<size_t>(node) < g.nodes.size() && g.nodes[static_cast<size_t>(node)].plugin.type == "gl.surface"; };
  bool stale = false;
  for (const auto& e : topo.controls) {
    if (isSurface(e.src) && e.out >= surfaceParams(e.src)) stale = true;
    if (isSurface(e.dst) && e.fl >= surfaceParams(e.dst)) stale = true;
  }
  for (const auto& sl : topo.slots)
    if (isSurface(sl.node) && sl.fl >= surfaceParams(sl.node)) stale = true;
  if (stale) {
    exact = false;
    c.report.warn("Patcher: links to surface controls that no longer exist not run");
  }
  // Parameter maps for every control-link / slot target.
  auto need = [&](int node, int fl) {
    if (node < 0 || static_cast<size_t>(node) >= g.nodes.size()) return false;
    pv2::PatchV2Node& n = g.nodes[static_cast<size_t>(node)];
    if (n.map(fl)) return true;
    pv2::PatchV2Map mp;
    if (!buildMap(inner[static_cast<size_t>(node)], fl, mp)) return false;
    n.maps.push_back(std::move(mp));
    return true;
  };
  for (const auto& e : topo.controls)
    if (!need(e.dst, e.fl)) {
      exact = false;
      const std::string who = e.dst >= 0 && static_cast<size_t>(e.dst) < g.nodes.size() ? g.nodes[static_cast<size_t>(e.dst)].flName : "?";
      c.report.warn("Patcher: a control link drives " + who + " parameter " + std::to_string(e.fl) + ", which Gridline does not map");
    }
  fx::begin(m, instrument ? "gl.patcher" : "gl.patcherfx");
  for (size_t k = 0; k < topo.slots.size(); ++k) {
    const auto& sl = topo.slots[k];
    if (sl.node < 0) continue;  // (an exposed controller output, or an unknown port)
    if (!need(sl.node, sl.fl)) {
      exact = false;
      continue;
    }
    // The slot starts at its target's current value.
    const pv2::PatchV2Map* mp = g.nodes[static_cast<size_t>(sl.node)].map(sl.fl);
    const PluginState& ps = g.nodes[static_cast<size_t>(sl.node)].plugin;
    if (mp && static_cast<size_t>(mp->gl) < ps.params.size())
      fx::set(m, pv2::PhMap0 + static_cast<int>(k), mp->invert(ps.params[static_cast<size_t>(mp->gl)]));
  }
  // Generators in the effect version get no notes; plugins Gridline cannot host.
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const PluginInfo* info = findPlugin(g.nodes[i].plugin.type);
    if (!info) continue;
    if (info->kind == PluginKind::NoteFx) exact = false, c.report.warn("Patcher: note processor \"" + g.nodes[i].flName + "\" not run");
    if (isHostedType(g.nodes[i].plugin.type)) exact = false, c.report.warn("Patcher: installed plugin \"" + g.nodes[i].flName + "\" cannot run inside the Patcher");
  }
  if (g.nodes.size() > static_cast<size_t>(pv2::kV2MaxNodes)) exact = false;
  md::PatchGraph chain;
  m.state.data = chainOf(g, topo, instrument, chain) ? md::serializePatchGraph(chain) : pv2::serializePatchV2(g);
  m.exact = exact;
  c.report.counts["patcher graphs (v2)"]++;
  // FL automation of the patch's parameters → host Param slots.
  auto slots = std::make_shared<std::vector<int>>(g.slots);
  m.param = [slots](int fl) {
    for (size_t k = 0; k < slots->size(); ++k)
      if ((*slots)[k] == fl) return pv2::PhMap0 + static_cast<int>(k);
    return -1;
  };
  m.value = [](int, double n) { return std::clamp(n, 0.0, 1.0); };
  return true;
}

namespace {
bool decodePatcherV1(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  std::vector<uint8_t> b = stateBytes(evs);
  if (b.size() < 16) return false;
  bool notesInput = false;
  std::vector<Node> nodes;
  std::map<uint64_t, std::pair<int, int>> ports;  // port id → (node index, kind)
  uint64_t inputId = ~0ull, outputId = ~0ull;
  std::vector<std::pair<uint64_t, uint64_t>> links;  // (destination, source)
  for (const Chunk& item : chunks(b.data() + 4, b.size() - 4)) {
    if (item.id != 0) continue;
    std::vector<Chunk> parts = chunks(item.p, item.n);
    if (parts.empty()) continue;
    if (parts[0].id == 5000 && parts[0].n >= 12) {
      Node nd;
      nd.id = u64(parts[0].p + 4);
      for (const Chunk& ch : parts) {
        if (ch.id == 5008) nd.name = chunkText(ch);
        else if (ch.id == 5010) nd.display = chunkText(ch);
        else if (ch.id == 5005) nd.state = ch.p, nd.stateSize = ch.n;
        else if (ch.id == 5007 && ch.n >= 4) {
          Reader r(ch.p, ch.n);
          float f = r.f32();
          nd.mix = std::isfinite(f) ? static_cast<float>(clampd(f, 0, 1)) : 1.0f;
        } else if (ch.id == 5009 && ch.n >= 20) {
          Reader r(ch.p, ch.n);
          r.skip(8);
          uint64_t pid = u64(ch.p + 8);
          r.skip(8);
          int kind = static_cast<int>(r.u32());
          ports[pid] = {static_cast<int>(nodes.size()), kind};
        }
      }
      nodes.push_back(nd);
    } else if (parts[0].id == 1400 && parts[0].n >= 16) {
      uint64_t id = u64(parts[0].p);
      Reader r(parts[0].p + 8, parts[0].n - 8);
      r.u32();
      int kind = static_cast<int>(r.u32());
      if (kind == 7) inputId = id;
      else if (kind == 6) outputId = id;
      for (const Chunk& ch : parts)
        if (ch.id == 1402 && chunkText(ch) == "Notes") notesInput = true;
    } else if (parts[0].id == 1300 && parts[0].n >= 16) {
      links.push_back({u64(parts[0].p), u64(parts[0].p + 8)});
    }
  }
  // The instrument version (on a channel, or with a note input and no audio
  // input) is decoded by the instrument importer.
  if (isChannelPlugin(c, evs) || notesInput || inputId == ~0ull) {
    static const PluginDecoder genPatcher = [] {
      std::map<std::string, PluginDecoder> g;
      registerGeneratorDecoders2(g);
      auto it = g.find("Patcher");
      return it == g.end() ? PluginDecoder{} : it->second;
    }();
    if (genPatcher) return genPatcher(c, evs, m);
  }
  const bool instrument = false;
  // Audio connections.
  for (const auto& [dst, src] : links) {
    int from = -2;  // -1 = audio input
    if (src == inputId) from = -1;
    else if (auto it = ports.find(src); it != ports.end() && it->second.second == 2) from = it->second.first;
    if (from == -2) continue;
    if (dst == outputId) {
      if (from >= 0) nodes[static_cast<size_t>(from)].toOutput = true;
      continue;
    }
    auto it = ports.find(dst);
    if (it == ports.end() || it->second.second != 3) continue;
    nodes[static_cast<size_t>(it->second.first)].preds.insert(from);
  }
  // Signal order (Kahn), from the audio input.
  std::vector<int> order;
  std::vector<bool> placed(nodes.size(), false);
  bool progress = true;
  while (progress) {
    progress = false;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (placed[i] || nodes[i].preds.empty()) continue;
      bool ready = true;
      for (int p : nodes[i].preds)
        if (p >= 0 && !placed[static_cast<size_t>(p)]) ready = false;
      if (!ready) continue;
      placed[i] = true;
      order.push_back(static_cast<int>(i));
      progress = true;
    }
  }
  // Instruments in the instrument version: generator nodes have no audio input.
  std::vector<int> sources;
  if (instrument)
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!placed[i] && nodes[i].toOutput) sources.push_back(static_cast<int>(i));

  Json jnodes = Json::makeArray();
  int kept = 0, skipped = 0;
  bool allExact = true, simple = true;
  std::set<int> prevPreds;
  bool havePrev = false;
  auto addNode = [&](const Node& nd, bool parallel) {
    PluginMapping inner;
    if (!nd.state || !decodeEmbedded(c, nd.name, nd.state, nd.stateSize, inner) || inner.glType == "gl.patcher" ||
        inner.glType == "gl.patcherfx") {
      ++skipped;
      c.report.warn("Patcher: inner plugin \"" + (nd.display.empty() ? nd.name : nd.display) + "\" (" + nd.name +
                    ") not mapped");
      return false;
    }
    const PluginInfo* info = findPlugin(inner.glType);
    if (!info || (!instrument && info->kind != PluginKind::Effect) || info->kind == PluginKind::NoteFx) {
      ++skipped;
      if (isHostedType(inner.glType))  // never silently: the user has to re-add it
        c.report.warn("Patcher: installed plugin \"" + (nd.display.empty() ? nd.name : nd.display) + "\" (" + inner.glType +
                      ") cannot run inside the Patcher" + (info ? "" : " and is not installed") + "; it was left out");
      return false;
    }
    if (kept >= kMaxNodes) {
      ++skipped;
      c.report.warn("Patcher: more than 8 plugins; \"" + nd.name + "\" dropped");
      return false;
    }
    Json jn = Json::makeObject();
    jn.object["type"] = Json::makeString(inner.glType);
    jn.object["data"] = Json::makeString(inner.state.data);
    Json ps = Json::makeArray();
    for (float v : inner.state.params) ps.array.push_back(Json::makeNumber(v));
    jn.object["params"] = std::move(ps);
    Json bl;
    bl.type = Json::Type::Bool;
    bl.boolean = parallel;
    jn.object["parallel"] = bl;
    bl.boolean = false;
    jn.object["bypass"] = bl;
    jn.object["mix"] = Json::makeNumber(nd.mix);
    jnodes.array.push_back(std::move(jn));
    ++kept;
    allExact = allExact && inner.exact;
    c.report.counts["patcher inner plugins mapped"]++;
    return true;
  };
  for (int i : sources) addNode(nodes[static_cast<size_t>(i)], false);
  for (int i : order) {
    const Node& nd = nodes[static_cast<size_t>(i)];
    bool parallel = havePrev && nd.preds == prevPreds;
    if (!parallel && havePrev) {
      // Serial only when fed by exactly the previous chain output.
      if (nd.preds.size() != 1) simple = false;
    }
    if (addNode(nd, parallel)) {
      prevPreds = nd.preds;
      havePrev = true;
    }
  }
  for (size_t i = 0; i < nodes.size(); ++i)
    if (!placed[i] && std::find(sources.begin(), sources.end(), static_cast<int>(i)) == sources.end()) {
      // Not on the audio path (controllers, visualisers): their control links are not imported.
      ++skipped;
      c.report.warn("Patcher: \"" + (nodes[i].display.empty() ? nodes[i].name : nodes[i].display) +
                    "\" is not on the audio path; skipped");
    }
  fx::begin(m, instrument ? "gl.patcher" : "gl.patcherfx");
  Json j = Json::makeObject();
  j.object["nodes"] = std::move(jnodes);
  Json macros = Json::makeArray();
  for (int k = 0; k < 8; ++k) {
    Json jm = Json::makeObject();
    jm.object["name"] = Json::makeString("");
    jm.object["targets"] = Json::makeArray();
    macros.array.push_back(std::move(jm));
  }
  j.object["macros"] = std::move(macros);
  m.state.data = jsonStringify(j, 0);
  m.exact = allExact && simple && skipped == 0 && kept > 0;
  if (kept == 0) c.report.warn("Patcher: no inner plugin could be mapped; empty host");
  return true;
}

}  // namespace

bool decodePatcher(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  return decodePatcherV2(c, evs, m) || decodePatcherV1(c, evs, m);
}

void registerFx2Patcher(std::map<std::string, PluginDecoder>& d) { d["Patcher"] = decodePatcher; }

}  // namespace gl::flp::fx2
