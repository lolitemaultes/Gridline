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
#include <set>
#include "../Json.h"
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

bool decodePatcher(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
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

void registerFx2Patcher(std::map<std::string, PluginDecoder>& d) { d["Patcher"] = decodePatcher; }

}  // namespace gl::flp::fx2
