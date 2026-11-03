// Gridline - Junction graphs, version 3: the model (see junction.h). No UI, no audio.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include "../../core/Json.h"
#include "../../core/PluginParams.h"
#include "modulation_dsp.h"
#include "junction.h"

namespace gl::packs::modulation {

const JNode* JGraph::node(uint32_t id) const {
  for (const auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}
JNode* JGraph::node(uint32_t id) {
  for (auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

// ------------------------------------------------------------------- JSON
namespace {

Json num(double v) { return Json::makeNumber(v); }

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
float rdf(const uint8_t* p) {
  const uint32_t u = rd32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

}  // namespace

bool isJunction3(const std::string& data) { return data.size() > 8 && data.compare(0, 7, "{\"v\":3,") == 0; }

std::string serializeJunction(const JGraph& g) {
  Json nodes = Json::makeArray();
  for (const JNode& n : g.nodes) {
    Json j = Json::makeObject();
    j.object["id"] = num(n.id);
    j.object["type"] = Json::makeString(n.plugin.type);
    Json a = Json::makeArray();
    for (float v : n.plugin.params) a.array.push_back(num(v));
    j.object["params"] = std::move(a);
    if (!n.plugin.data.empty()) j.object["data"] = Json::makeString(n.plugin.data);
    if (!n.plugin.ins.empty()) j.object["ins"] = Json::makeString(n.plugin.ins);
    if (!n.plugin.original.empty()) j.object["orig"] = Json::makeString(n.plugin.original);
    if (!n.name.empty()) j.object["name"] = Json::makeString(n.name);
    j.object["x"] = num(n.x);
    j.object["y"] = num(n.y);
    if (n.color) j.object["color"] = num(n.color);
    if (n.mode != JMode::On) j.object["mode"] = num(static_cast<int>(n.mode));
    if (n.mix != 1.0f) j.object["mix"] = num(n.mix);
    if (!n.flName.empty()) j.object["fl"] = Json::makeString(n.flName);
    if (!n.flOrig.empty()) j.object["flOrig"] = Json::makeString(n.flOrig);
    if (!n.flRaw.empty()) j.object["flRaw"] = Json::makeString(n.flRaw);
    if (!n.maps.empty()) {
      Json maps = Json::makeArray();
      for (const auto& m : n.maps) {
        Json e = Json::makeArray();
        e.array.push_back(num(m.fl));
        e.array.push_back(num(m.gl));
        if (m.tab.size() >= 2) {
          Json t = Json::makeArray();
          for (float v : m.tab) t.array.push_back(num(v));
          e.array.push_back(std::move(t));
        } else {
          e.array.push_back(num(m.lo));
          e.array.push_back(num(m.hi));
        }
        e.array.push_back(num(m.integer ? 1 : 0));
        maps.array.push_back(std::move(e));
      }
      j.object["maps"] = std::move(maps);
    }
    nodes.array.push_back(std::move(j));
  }
  Json links = Json::makeArray();
  for (const JLink& l : g.links) {
    Json e = Json::makeArray();
    for (double v : {static_cast<double>(l.kind), static_cast<double>(l.src), static_cast<double>(l.sp), static_cast<double>(l.dst), static_cast<double>(l.dp),
                     static_cast<double>(l.gain), static_cast<double>(l.lo), static_cast<double>(l.hi), static_cast<double>(l.fl)})
      e.array.push_back(num(v));
    links.array.push_back(std::move(e));
  }
  Json controls = Json::makeArray();
  for (const JControl& c : g.controls) {
    Json e = Json::makeArray();
    e.array.push_back(num(c.param));
    e.array.push_back(Json::makeString(c.name));
    controls.array.push_back(std::move(e));
  }
  Json snaps = Json::makeArray();
  for (const JSnapshot& s : g.snaps) {
    Json e = Json::makeArray();
    e.array.push_back(Json::makeString(s.name));
    for (const auto& [k, v] : s.values) {
      e.array.push_back(num(k));
      e.array.push_back(num(v));
    }
    snaps.array.push_back(std::move(e));
  }
  // By hand: the key order starts with "v" (isJunction3).
  Json io = Json::makeArray();
  for (float v : {g.inX, g.inY, g.outX, g.outY}) io.array.push_back(num(v));
  return "{\"v\":3,\"next\":" + std::to_string(g.next) + ",\"io\":" + jsonStringify(io, 0) + ",\"nodes\":" + jsonStringify(nodes, 0) + ",\"links\":" + jsonStringify(links, 0) +
         ",\"controls\":" + jsonStringify(controls, 0) + ",\"snaps\":" + jsonStringify(snaps, 0) + "}";
}

bool parseJunction(const std::string& data, JGraph& out) {
  out = JGraph{};
  if (!isJunction3(data)) return false;
  Json j;
  if (!jsonParse(data, j) || !j.is(Json::Type::Object)) return false;
  out.next = jsonInt<uint32_t>(j.num("next", kJFirstId));
  if (const Json* io = j.find("io"); io && io->is(Json::Type::Array) && io->array.size() >= 4) {
    out.inX = static_cast<float>(io->array[0].number), out.inY = static_cast<float>(io->array[1].number);
    out.outX = static_cast<float>(io->array[2].number), out.outY = static_cast<float>(io->array[3].number);
  }
  std::set<uint32_t> seen;
  if (const Json* ns = j.find("nodes"); ns && ns->is(Json::Type::Array))
    for (const Json& jn : ns->array) {
      JNode n;
      n.id = jsonInt<uint32_t>(jn.num("id", 0));
      n.plugin.type = jn.text("type");
      n.plugin.data = jn.text("data");
      n.plugin.ins = jn.text("ins");
      n.plugin.original = jn.text("orig");
      if (const Json* a = jn.find("params"); a && a->is(Json::Type::Array))
        for (const Json& v : a->array) n.plugin.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
      n.name = jn.text("name");
      n.x = static_cast<float>(jn.num("x", 0));
      n.y = static_cast<float>(jn.num("y", 0));
      n.color = jsonInt<int>(jn.num("color", 0));
      const int mode = jsonInt<int>(jn.num("mode", 0));
      n.mode = mode == 1 ? JMode::Bypass : mode == 2 ? JMode::Mute : JMode::On;
      n.mix = static_cast<float>(std::clamp(jn.num("mix", 1), 0.0, 1.0));
      n.flName = jn.text("fl");
      n.flOrig = jn.text("flOrig");
      n.flRaw = jn.text("flRaw");
      if (const Json* ms = jn.find("maps"); ms && ms->is(Json::Type::Array))
        for (const Json& e : ms->array) {
          if (!e.is(Json::Type::Array) || e.array.size() < 3) continue;
          PatchV2Map m;
          m.fl = jsonInt<int>(e.array[0].number);
          m.gl = jsonInt<int>(e.array[1].number);
          if (e.array[2].is(Json::Type::Array)) {
            for (const Json& v : e.array[2].array) m.tab.push_back(static_cast<float>(v.number));
            m.integer = e.array.size() > 3 && e.array[3].number != 0;
          } else {
            m.lo = e.array[2].number;
            m.hi = e.array.size() > 3 ? e.array[3].number : 1.0;
            m.integer = e.array.size() > 4 && e.array[4].number != 0;
          }
          n.maps.push_back(std::move(m));
        }
      if (n.id < kJFirstId || !seen.insert(n.id).second || static_cast<int>(out.nodes.size()) >= kJMaxNodes) continue;
      out.next = std::max(out.next, n.id + 1);
      out.nodes.push_back(std::move(n));
    }
  auto known = [&](uint32_t id) { return id == kJInputs || id == kJOutputs || seen.count(id) > 0; };
  if (const Json* ls = j.find("links"); ls && ls->is(Json::Type::Array))
    for (const Json& e : ls->array) {
      if (!e.is(Json::Type::Array) || e.array.size() < 5) continue;
      auto at = [&](size_t k, double dflt) { return k < e.array.size() && e.array[k].is(Json::Type::Number) ? e.array[k].number : dflt; };
      JLink l;
      const int kind = jsonInt<int>(at(0, 0));
      if (kind < 0 || kind > 2) continue;
      l.kind = static_cast<JKind>(kind);
      l.src = jsonInt<uint32_t>(at(1, 0));
      l.sp = jsonInt<int>(at(2, 0));
      l.dst = jsonInt<uint32_t>(at(3, 0));
      l.dp = jsonInt<int>(at(4, 0));
      const double gain = at(5, 1);
      l.gain = std::isfinite(gain) ? static_cast<float>(std::clamp(gain, 0.0, 16.0)) : 1.0f;
      l.lo = static_cast<float>(std::clamp(at(6, 0), 0.0, 1.0));
      l.hi = static_cast<float>(std::clamp(at(7, 1), 0.0, 1.0));
      l.fl = jsonInt<int>(at(8, -1));
      if (known(l.src) && known(l.dst)) out.links.push_back(l);
    }
  if (const Json* cs = j.find("controls"); cs && cs->is(Json::Type::Array))
    for (const Json& e : cs->array)
      if (e.is(Json::Type::Array) && e.array.size() >= 2) out.controls.push_back({jsonInt<int>(e.array[0].number), e.array[1].str});
  if (const Json* ss = j.find("snaps"); ss && ss->is(Json::Type::Array))
    for (const Json& e : ss->array) {
      if (!e.is(Json::Type::Array) || e.array.empty()) continue;
      JSnapshot s;
      s.name = e.array[0].str;
      for (size_t k = 1; k + 1 < e.array.size(); k += 2) s.values.push_back({jsonInt<int>(e.array[k].number), static_cast<float>(e.array[k + 1].number)});
      out.snaps.push_back(std::move(s));
    }
  return true;
}

// ------------------------------------------------------------- conversions
bool junctionSetNodeState(PluginState& host, uint32_t node, const PluginState& inner) {
  if (isJunction3(host.data)) {
    JGraph g;
    if (!parseJunction(host.data, g)) return false;
    JNode* n = g.node(node);
    if (!n) return false;
    n->plugin = inner;
    host.data = serializeJunction(g);
    return true;
  }
  const size_t index = node >= kJFirstId ? node - kJFirstId : ~size_t{0};
  if (isPatchV2(host.data)) {
    PatchV2 v;
    if (!parsePatchV2(host.data, v) || index >= v.nodes.size()) return false;
    v.nodes[index].plugin = inner;
    host.data = serializePatchV2(v);
    return true;
  }
  PatchGraph c = patchGraphOf(host);
  if (index >= c.nodes.size()) return false;
  c.nodes[index].plugin = inner;
  host.data = serializePatchGraph(c);
  return true;
}

JGraph junctionFromChain(const PatchGraph& chain) {
  JGraph g;
  std::vector<uint32_t> ids;
  // Instruments in one column, the effects in stages (a parallel effect shares the stage
  // of the effect before it, as the chain runtime runs them).
  std::vector<uint32_t> gens;
  std::vector<std::vector<size_t>> stages;
  for (size_t i = 0; i < chain.nodes.size(); ++i) {
    const PatchNode& pn = chain.nodes[i];
    JNode n;
    n.id = g.next++;
    n.plugin = pn.plugin;
    ids.push_back(n.id);
    const PluginInfo* info = findPlugin(pn.plugin.type);
    if (info && info->kind == PluginKind::Generator) {
      n.mode = pn.bypass ? JMode::Mute : JMode::On;
      gens.push_back(n.id);
    } else {
      if (pn.parallel && !stages.empty()) stages.back().push_back(i);
      else stages.push_back({i});
      if (pn.bypass) n.mode = pn.parallel ? JMode::Mute : JMode::Bypass;
      if (!pn.parallel) n.mix = std::clamp(pn.mix, 0.0f, 1.0f);
    }
    g.nodes.push_back(std::move(n));
  }
  std::vector<JLink> cur;  // what feeds the next stage: source, gain
  const bool anyGen = !gens.empty();
  auto src = [&](uint32_t id, float gain) {
    JLink l;
    l.src = id;
    l.gain = gain;
    return l;
  };
  for (uint32_t id : gens) cur.push_back(src(id, 1));
  (void)anyGen;
  bool first = cur.empty();
  for (const auto& st : stages) {
    std::vector<JLink> next;
    for (size_t i : st) {
      const uint32_t id = ids[i];
      if (first) {
        JLink l = src(kJInputs, 1);
        l.dst = id;
        g.links.push_back(l);  // (the instrument host has no audio input: it reads silence)
      } else {
        for (JLink l : cur) {
          l.dst = id;
          g.links.push_back(l);
        }
      }
      next.push_back(src(id, chain.nodes[i].parallel ? std::clamp(chain.nodes[i].mix, 0.0f, 1.0f) : 1.0f));
    }
    cur = std::move(next);
    first = false;
  }
  if (first) cur.push_back(src(kJInputs, 1));  // no effects, no instruments: straight through
  for (JLink l : cur) {
    l.dst = kJOutputs;
    g.links.push_back(l);
  }
  for (uint32_t id : gens) {
    JLink l;
    l.kind = JKind::Notes;
    l.src = kJInputs;
    l.dst = id;
    g.links.push_back(l);
  }
  for (int m = 0; m < kMacros; ++m) {
    g.controls.push_back({m, chain.macroNames[m]});
    for (const PatchMacroTarget& t : chain.macros[m]) {
      if (t.node < 0 || static_cast<size_t>(t.node) >= ids.size()) continue;
      JLink l;
      l.kind = JKind::Param;
      l.src = kJInputs;
      l.sp = m;
      l.dst = ids[static_cast<size_t>(t.node)];
      l.dp = t.param;
      l.lo = t.min;
      l.hi = t.max;
      g.links.push_back(l);
    }
  }
  jAutoArrange(g);
  return g;
}

bool jV2HasLevels(const PatchV2& v2) {
  for (const PatchV2Node& n : v2.nodes) {
    std::vector<uint8_t> h = patchUnhex(n.head);
    if (n.tail != "-") {
      const std::vector<uint8_t> t = patchUnhex(n.tail);
      h.insert(h.end(), t.begin(), t.end());
    }
    for (const PatchChunk& c : patchChunks(h.data(), h.size()))
      if (c.id == 5007 && c.data.size() >= 4) {
        const float m = rdf(c.data.data());
        if (std::isfinite(m) && std::clamp(m, 0.0f, 1.0f) != 1.0f) return true;
      }
  }
  return false;
}

JGraph junctionFromV2(const PatchV2& v2, std::vector<std::string>* notRun, std::vector<int>* inserts) {
  JGraph g;
  auto skip = [&](const std::string& why) {
    if (notRun) notRun->push_back(why);
  };
  const int N = std::min(static_cast<int>(v2.nodes.size()), kJMaxNodes);
  if (static_cast<int>(v2.nodes.size()) > N) skip("more than " + std::to_string(kJMaxNodes) + " plugins");
  struct Port {
    int node = -1;
    uint32_t cls = 0, index = 0, kind = 0;
    int insert = -1;
    std::string io;
  };
  std::multimap<uint64_t, Port> ports;
  auto idOf = [](int k) { return kJFirstId + static_cast<uint32_t>(k); };
  std::map<uint64_t, std::pair<float, float>> pos;
  std::vector<std::pair<uint64_t, int>> exposed;
  struct Lnk {
    uint64_t dst, src;
    float gain;
  };
  std::vector<Lnk> links;
  std::pair<float, float> inAt{-1e9f, 0}, outAt{-1e9f, 0};
  std::vector<std::pair<uint64_t, bool>> ioNodes;
  for (int k = 0; k < N; ++k) {
    const PatchV2Node& vn = v2.nodes[static_cast<size_t>(k)];
    JNode n;
    n.id = idOf(k);
    n.plugin = vn.plugin;
    n.name = vn.display;
    n.flName = vn.flName;
    n.flOrig = vn.orig;
    n.flRaw = vn.raw;
    n.maps = vn.maps;
    std::vector<uint8_t> h = patchUnhex(vn.head);
    if (vn.tail != "-") {
      const std::vector<uint8_t> t = patchUnhex(vn.tail);
      h.insert(h.end(), t.begin(), t.end());
    }
    for (const PatchChunk& c : patchChunks(h.data(), h.size())) {
      if (c.id == 5009 && c.data.size() >= 20) {
        Port p;
        p.node = k;
        p.cls = rd32(c.data.data());
        p.index = rd32(c.data.data() + 4);
        p.kind = rd32(c.data.data() + 16);
        ports.emplace(rd64(c.data.data() + 8), p);
      } else if (c.id == 5007 && c.data.size() >= 4) {
        const float m = rdf(c.data.data());
        if (std::isfinite(m)) n.mix = std::clamp(m, 0.0f, 1.0f);
      }
    }
    g.nodes.push_back(std::move(n));
  }
  g.next = idOf(N);
  for (const std::string& s : v2.items) {
    if (s.empty() || s[0] == '#') continue;
    const std::vector<uint8_t> b = patchUnhex(s);
    for (const PatchChunk& c : patchChunks(b.data(), b.size())) {
      if (c.id == 1100 && c.data.size() >= 12) exposed.push_back({rd64(c.data.data()), static_cast<int>(rd32(c.data.data() + 8))});
      if (c.id != 0) continue;
      const std::vector<PatchChunk> kids = patchChunks(c.data.data(), c.data.size());
      if (kids.empty()) continue;
      if (kids[0].id == 1400 && kids[0].data.size() >= 16) {
        Port p;
        p.cls = rd32(kids[0].data.data() + 8);
        p.kind = rd32(kids[0].data.data() + 12);
        if (kids[0].data.size() >= 20 && p.cls == 0 && p.kind == 4) p.insert = kids[0].data[17] | (kids[0].data.size() > 18 ? kids[0].data[18] << 8 : 0);
        for (const auto& k2 : kids)
          if (k2.id == 1402) {
            const bool wide = k2.data.size() >= 2 && k2.data[1] == 0;
            for (size_t i = 0; i < k2.data.size(); i += wide ? 2 : 1)
              if (k2.data[i]) p.io += static_cast<char>(k2.data[i]);
          }
        const uint64_t id = rd64(kids[0].data.data());
        ports.emplace(id, p);
        ioNodes.push_back({id, p.kind == 7 || p.kind == 5 || p.kind == 1});
      } else if (kids[0].id == 1300 && kids[0].data.size() >= 20) {
        links.push_back({rd64(kids[0].data.data()), rd64(kids[0].data.data() + 8), rdf(kids[0].data.data() + 16)});
      } else if (kids[0].id == 1600 && kids[0].data.size() >= 16) {
        pos[rd64(kids[0].data.data())] = {rdf(kids[0].data.data() + 8), rdf(kids[0].data.data() + 12)};
      }
    }
  }
  bool placed = false;
  for (int k = 0; k < N; ++k) {
    const auto it = pos.find(v2.nodes[static_cast<size_t>(k)].id);
    if (it == pos.end()) continue;
    g.nodes[static_cast<size_t>(k)].x = it->second.first;
    g.nodes[static_cast<size_t>(k)].y = it->second.second;
    placed = true;
  }
  for (const auto& [id, input] : ioNodes) {
    const auto it = pos.find(id);
    if (it == pos.end()) continue;
    (input ? inAt : outAt) = it->second;
  }
  auto findPort = [&](uint64_t id, bool output) -> const Port* {
    auto [a, b] = ports.equal_range(id);
    const Port* any = nullptr;
    for (auto it = a; it != b; ++it) {
      const Port& p = it->second;
      any = &p;
      const bool isOut = p.node >= 0 ? (p.cls == 2 ? (p.index & 0x10000000u) != 0 || p.kind == 2 || p.kind == 10 : p.kind == 2 || p.kind == 0)
                                     : (p.kind == 7 || p.kind == 5 || p.kind == 1);
      if (isOut == output) return &p;
    }
    return any;
  };
  std::vector<int> outs;  // Track outs: FL insert of Outputs port k + 1
  auto addMap = [&](int node, int fl) -> int {
    const PatchV2Node& vn = v2.nodes[static_cast<size_t>(node)];
    const PatchV2Map* m = vn.map(fl);
    return m ? m->gl : -1;
  };
  for (const Lnk& l : links) {
    const Port* s = findPort(l.src, true);
    const Port* d = findPort(l.dst, false);
    if (!s || !d) {
      skip("link to an unknown port");
      continue;
    }
    if (s->cls == 0 && d->cls == 0) {
      JLink e;
      e.gain = std::isfinite(l.gain) ? l.gain : 1.0f;
      if (s->node < 0) {
        if (s->kind != 7) {
          skip("audio from " + s->io);
          continue;
        }
        e.src = kJInputs;
      } else {
        if (s->kind != 2 && s->kind != 0) {
          skip("audio output of kind " + std::to_string(s->kind));
          continue;
        }
        e.src = idOf(s->node);
        e.sp = static_cast<int>(s->index);
      }
      if (d->node < 0) {
        if (d->kind == 6) {
          e.dst = kJOutputs;
        } else if (d->kind == 4 && d->insert >= 0) {
          size_t at = std::find(outs.begin(), outs.end(), d->insert) - outs.begin();
          if (at == outs.size()) {
            if (outs.size() >= static_cast<size_t>(kMaxSendOutputs)) {
              skip("more than " + std::to_string(kMaxSendOutputs) + " track outputs");
              continue;
            }
            outs.push_back(d->insert);
          }
          e.dst = kJOutputs;
          e.dp = static_cast<int>(at) + 1;
        } else {
          skip("audio to " + d->io);
          continue;
        }
      } else {
        if (d->kind != 3 && d->kind != 7 && d->kind != 0) {
          skip("audio input of kind " + std::to_string(d->kind));
          continue;
        }
        e.dst = idOf(d->node);
        e.dp = d->index == 0 ? 0 : 1;  // every extra input is the sidechain
      }
      g.links.push_back(e);
    } else if (s->cls == 1 && d->cls == 1) {
      JLink e;
      e.kind = JKind::Notes;
      if (d->node < 0) {
        skip("notes to " + d->io);
        continue;
      }
      e.dst = idOf(d->node);
      if (s->node < 0) {
        if (s->io.find("Note") == std::string::npos) {
          skip("notes from " + s->io);
          continue;
        }
        e.src = kJInputs;
      } else {
        e.src = idOf(s->node);
        // FL numbers note outputs per plugin; only the routing processors have several.
        const std::string& srcType = v2.nodes[static_cast<size_t>(s->node)].plugin.type;
        e.sp = srcType == "gl.split" || srcType == "gl.colormap" ? static_cast<int>(s->index) : 0;
      }
      g.links.push_back(e);
    } else if (s->cls == 2 && d->cls == 2 && s->node >= 0 && d->node >= 0 && s->kind == 10 && d->kind == 11) {
      JLink e;
      e.kind = JKind::Param;
      e.src = idOf(s->node);
      e.sp = static_cast<int>(s->index & 0x0FFFFFFFu);
      e.dst = idOf(d->node);
      e.fl = static_cast<int>(d->index);
      e.dp = addMap(d->node, e.fl);
      g.links.push_back(e);
    } else {
      skip("control port kind " + std::to_string(s->kind) + " to " + std::to_string(d->kind));
    }
  }
  // Host parameter slots.
  for (size_t k = 0; k < v2.slots.size() && k < static_cast<size_t>(kPatchMaps); ++k) {
    const int fl = v2.slots[k];
    int node = -1, tfl = 0;
    for (const auto& [port, idx] : exposed)
      if (idx == fl) {
        auto [a, b] = ports.equal_range(port);
        for (auto it = a; it != b; ++it)
          if (it->second.node >= 0 && it->second.cls == 2 && it->second.kind == 11) node = it->second.node, tfl = static_cast<int>(it->second.index);
      }
    const int hp = PhMap0 + static_cast<int>(k);
    std::string name;
    if (node >= 0) {
      JLink e;
      e.kind = JKind::Param;
      e.src = kJInputs;
      e.sp = hp;
      e.dst = idOf(node);
      e.fl = tfl;
      e.dp = addMap(node, tfl);
      g.links.push_back(e);
      const PluginInfo* info = findPlugin(v2.nodes[static_cast<size_t>(node)].plugin.type);
      if (info && e.dp >= 0 && static_cast<size_t>(e.dp) < info->params.size()) name = info->params[static_cast<size_t>(e.dp)].name;
    }
    g.controls.push_back({hp, name});
  }
  // FL's module level scales what leaves the module and leaves its latency alone (measured on
  // FL's own library, K-AC). Junction's mix is dry / wet: the level moves onto the cables.
  if (v2.align)
    for (JNode& n : g.nodes) {
      if (n.mix == 1.0f) continue;
      for (JLink& l : g.links)
        if (l.kind == JKind::Audio && l.src == n.id) l.gain *= n.mix;
      n.mix = 1.0f;
    }
  if (inserts) *inserts = outs;
  if (inAt.first > -1e8f) g.inX = inAt.first, g.inY = inAt.second;
  if (outAt.first > -1e8f) g.outX = outAt.first, g.outY = outAt.second;
  if (!placed) jAutoArrange(g);
  return g;
}

JGraph junctionOf(const PluginState& state) {
  JGraph g;
  if (isJunction3(state.data) && parseJunction(state.data, g)) return g;
  if (isPatchV2(state.data)) {
    PatchV2 v2;
    if (parsePatchV2(state.data, v2)) return junctionFromV2(v2);
  }
  return junctionFromChain(patchGraphOf(state));
}

// ------------------------------------------------------------------- ports
namespace {

int builtinAuxOutputs(const std::string& type) {
  static std::map<std::string, int> cache;
  auto it = cache.find(type);
  if (it != cache.end()) return it->second;
  int n = 0;
  if (auto gen = gl::createGenerator(type)) n = gen->auxOutputCount();
  cache[type] = n;
  return n;
}

JPort port(JKind k, bool out, int index, const std::string& name) {
  JPort p;
  p.kind = k, p.out = out, p.index = index, p.name = name;
  return p;
}

}  // namespace

std::vector<JPort> jNodePorts(const JNode& n) {
  std::vector<JPort> v;
  const PluginInfo* info = findPlugin(n.plugin.type);
  const PluginKind kind = info ? info->kind : PluginKind::Effect;
  if (kind == PluginKind::NoteFx) {
    v.push_back(port(JKind::Notes, false, 0, "Notes in"));
    v.push_back(port(JKind::Notes, true, 0, "Notes out"));
    if (n.plugin.type == "gl.split") {
      for (int k = 0; k < kSplitZones; ++k) v.push_back(port(JKind::Notes, true, k + 1, "Zone " + std::to_string(k + 1)));
    } else if (n.plugin.type == "gl.colormap") {
      for (int k = 0; k < 8; ++k) v.push_back(port(JKind::Notes, true, k + 1, "Target " + std::to_string(k + 1)));
    }
  } else if (kind == PluginKind::Generator) {
    v.push_back(port(JKind::Notes, false, 0, "Notes in"));
    v.push_back(port(JKind::Audio, true, 0, "Audio out"));
    const int aux = isHostedType(n.plugin.type) ? 0 : builtinAuxOutputs(n.plugin.type);
    for (int k = 0; k < aux; ++k) v.push_back(port(JKind::Audio, true, k + 1, "Out " + std::to_string(k + 2)));
  } else {
    v.push_back(port(JKind::Audio, false, 0, "Audio in"));
    v.push_back(port(JKind::Audio, false, 1, "Sidechain in"));
    v.push_back(port(JKind::Audio, true, 0, "Audio out"));
    if (info)
      for (size_t k = 0; k < info->sendParams.size(); ++k) v.push_back(port(JKind::Audio, true, static_cast<int>(k) + 1, "Out " + std::to_string(k + 2)));
  }
  v.push_back(port(JKind::Param, false, -1, "Parameters"));
  if (info && !info->outputs.empty()) {
    if (static_cast<int>(info->outputs.size()) > kJManyOutputs) {
      v.push_back(port(JKind::Param, true, -1, "Outputs"));
    } else {
      for (size_t k = 0; k < info->outputs.size(); ++k) v.push_back(port(JKind::Param, true, static_cast<int>(k), info->outputs[k]));
    }
  }
  return v;
}

std::vector<JPort> jHostPorts(bool inputs, bool fxHost) {
  std::vector<JPort> v;
  if (inputs) {
    if (fxHost) {
      v.push_back(port(JKind::Audio, true, 0, "Audio in"));
      v.push_back(port(JKind::Audio, true, 1, "Sidechain"));
    } else {
      v.push_back(port(JKind::Notes, true, 0, "Notes"));
    }
    v.push_back(port(JKind::Param, true, -1, "Panel"));
  } else {
    v.push_back(port(JKind::Audio, false, 0, "Audio out"));
    for (int k = 0; k < kMaxSendOutputs; ++k) v.push_back(port(JKind::Audio, false, k + 1, "Out " + std::to_string(k + 2)));
  }
  return v;
}

// ------------------------------------------------------------------- edits
uint32_t jAddNode(JGraph& g, const PluginState& plugin, float x, float y) {
  if (static_cast<int>(g.nodes.size()) >= kJMaxNodes) return 0;
  JNode n;
  n.id = g.next++;
  n.plugin = plugin;
  n.x = x, n.y = y;
  g.nodes.push_back(std::move(n));
  return g.nodes.back().id;
}

void jDisconnectNode(JGraph& g, uint32_t id) {
  g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const JLink& l) { return l.src == id || l.dst == id; }), g.links.end());
}

void jRemoveNode(JGraph& g, uint32_t id) {
  jDisconnectNode(g, id);
  g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const JNode& n) { return n.id == id; }), g.nodes.end());
}

uint32_t jDuplicateNode(JGraph& g, uint32_t id, float dx, float dy) {
  const JNode* n = g.node(id);
  if (!n || static_cast<int>(g.nodes.size()) >= kJMaxNodes) return 0;
  JNode c = *n;
  c.id = g.next++;
  c.x += dx, c.y += dy;
  c.flOrig.clear();
  g.nodes.push_back(std::move(c));
  const uint32_t nid = g.nodes.back().id;
  // Its inputs come along (a copy hears what the original hears); its outputs stay free.
  const size_t count = g.links.size();
  for (size_t k = 0; k < count; ++k)
    if (g.links[k].dst == id && g.links[k].kind != JKind::Param) {
      JLink l = g.links[k];
      l.dst = nid;
      g.links.push_back(l);
    }
  return nid;
}

namespace {

// Reaches `to` from `from` over the cables of `kind`?
bool reaches(const JGraph& g, JKind kind, uint32_t from, uint32_t to, bool anyKind = false) {
  std::vector<uint32_t> todo{from};
  std::set<uint32_t> seen;
  while (!todo.empty()) {
    const uint32_t at = todo.back();
    todo.pop_back();
    if (at == to) return true;
    if (!seen.insert(at).second) continue;
    for (const JLink& l : g.links)
      if ((anyKind || l.kind == kind) && l.src == at) todo.push_back(l.dst);
  }
  return false;
}

}  // namespace

bool jConnect(JGraph& g, const JLink& l, bool fxHost, std::string* why) {
  auto no = [&](const char* text) {
    if (why) *why = text;
    return false;
  };
  if (l.src == kJOutputs || l.dst == kJInputs) return no("Cables run from outputs to inputs.");
  if (l.src != kJInputs && !g.node(l.src)) return no("That module is gone.");
  if (l.dst != kJOutputs && !g.node(l.dst)) return no("That module is gone.");
  if (l.src == kJInputs) {
    if (l.kind == JKind::Audio && !fxHost) return no("The instrument version has no audio input.");
    if (l.kind == JKind::Notes && fxHost) return no("The effect version has no notes input.");
  }
  if (l.dst == kJOutputs && l.kind != JKind::Audio) return no("Only audio goes to the outputs.");
  if (l.kind == JKind::Notes) {
    if (l.src == l.dst || reaches(g, JKind::Notes, l.dst, l.src)) return no("Notes cannot run in a loop.");
  }
  for (const JLink& o : g.links)
    if (o.kind == l.kind && o.src == l.src && o.sp == l.sp && o.dst == l.dst && o.dp == l.dp && o.fl == l.fl) return no("Already connected.");
  if (l.kind == JKind::Param)
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                 [&](const JLink& o) { return o.kind == JKind::Param && o.dst == l.dst && o.dp == l.dp && o.dp >= 0; }),
                  g.links.end());
  g.links.push_back(l);
  return true;
}

std::vector<char> jFeedbackLinks(const JGraph& g) {
  std::vector<char> out(g.links.size(), 0);
  JGraph acc;  // cables accepted so far, in order
  acc.links.reserve(g.links.size());
  for (size_t k = 0; k < g.links.size(); ++k) {
    const JLink& l = g.links[k];
    if (l.kind == JKind::Notes) continue;
    if (l.src != kJInputs && l.dst != kJOutputs && (l.src == l.dst || reaches(acc, JKind::Audio, l.dst, l.src, true))) {
      out[k] = 1;
      continue;
    }
    acc.links.push_back(l);
  }
  return out;
}

void jAutoArrange(JGraph& g) {
  // Column: the longest path from a source (controllers count, feedback cables do not).
  const std::vector<char> fb = jFeedbackLinks(g);
  std::map<uint32_t, int> col;
  for (const JNode& n : g.nodes) col[n.id] = 0;
  for (size_t pass = 0; pass <= g.nodes.size(); ++pass) {
    bool moved = false;
    for (size_t k = 0; k < g.links.size(); ++k) {
      const JLink& l = g.links[k];
      if (fb[k] || l.src == kJInputs || l.dst == kJOutputs) continue;
      const auto a = col.find(l.src);
      const auto b = col.find(l.dst);
      if (a == col.end() || b == col.end()) continue;
      if (b->second < a->second + 1) b->second = a->second + 1, moved = true;
    }
    if (!moved) break;
  }
  int cols = 0;
  std::map<int, std::vector<uint32_t>> rows;
  for (const JNode& n : g.nodes) {
    rows[col[n.id]].push_back(n.id);
    cols = std::max(cols, col[n.id] + 1);
  }
  const float kW = 210, kH = 88;
  size_t tallest = 1;
  for (const auto& [c, r] : rows) tallest = std::max(tallest, r.size());
  const float H = static_cast<float>(tallest - 1) * kH;
  for (const auto& [c, r] : rows)
    for (size_t k = 0; k < r.size(); ++k)
      if (JNode* n = g.node(r[k])) {
        n->x = 200.0f + static_cast<float>(c) * kW;
        n->y = 60.0f + (H - static_cast<float>(r.size() - 1) * kH) * 0.5f + static_cast<float>(k) * kH;
      }
  g.inX = 20, g.inY = 60 + H * 0.5f;
  g.outX = 200.0f + static_cast<float>(cols) * kW, g.outY = 60 + H * 0.5f;
}

std::string jControlName(const JGraph& g, int param) {
  for (const JControl& c : g.controls)
    if (c.param == param && !c.name.empty()) return c.name;
  return param < kMacros ? "Macro " + std::to_string(param + 1) : "Param " + std::to_string(param - PhMap0 + 1);
}

int jExpose(JGraph& g, uint32_t node, int param, const std::string& name) {
  if (!g.node(node)) return -1;
  for (const JLink& l : g.links)
    if (l.kind == JKind::Param && l.src == kJInputs && l.dst == node && l.dp == param) return l.sp;
  for (int k = 0; k < kPatchMaps; ++k) {
    const int hp = PhMap0 + k;
    bool used = false;
    for (const JControl& c : g.controls) used = used || c.param == hp;
    for (const JLink& l : g.links) used = used || (l.kind == JKind::Param && l.src == kJInputs && l.sp == hp);
    if (used) continue;
    g.controls.push_back({hp, name});
    JLink l;
    l.kind = JKind::Param;
    l.src = kJInputs;
    l.sp = hp;
    l.dst = node;
    l.dp = param;
    jConnect(g, l, false);
    return hp;
  }
  return -1;
}

void jRemoveControl(JGraph& g, int param) {
  g.controls.erase(std::remove_if(g.controls.begin(), g.controls.end(), [&](const JControl& c) { return c.param == param; }), g.controls.end());
  g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const JLink& l) { return l.kind == JKind::Param && l.src == kJInputs && l.sp == param; }),
                g.links.end());
}

// ------------------------------------------------------------ factory patches
const std::vector<JPatch>& junctionPatches() {
  static const std::vector<JPatch> p = {
      {"Split Stereo Spaces", true, false},  {"Wet Tunnel", true, false},       {"Ducked Echoes", true, false},
      {"Layered Keys", false, true},         {"Arp Through Echo", false, true}, {"Key Split Duo", false, true},
  };
  return p;
}

namespace {

JLink audio(uint32_t src, uint32_t dst, float gain = 1, int dp = 0) {
  JLink l;
  l.src = src, l.dst = dst, l.gain = gain, l.dp = dp;
  return l;
}
JLink notes(uint32_t src, uint32_t dst, int sp = 0) {
  JLink l;
  l.kind = JKind::Notes;
  l.src = src, l.dst = dst, l.sp = sp;
  return l;
}
JLink macro(int m, uint32_t dst, int param, float lo, float hi) {
  JLink l;
  l.kind = JKind::Param;
  l.src = kJInputs, l.sp = m, l.dst = dst, l.dp = param, l.lo = lo, l.hi = hi;
  return l;
}

}  // namespace

JGraph junctionPatch(size_t index, bool fxHost) {
  (void)fxHost;
  JGraph g;
  for (int m = 0; m < kMacros; ++m) g.controls.push_back({m, ""});
  auto add = [&](const char* type) { return jAddNode(g, defaultPluginState(type), 0, 0); };
  auto ctl = [&](int m, const char* name) { g.controls[static_cast<size_t>(m)].name = name; };
  switch (index) {
    case 0: {  // two delays and a reverb side by side
      const uint32_t a = add("gl.delay"), b = add("gl.reverb"), c = add("gl.filter");
      g.links = {audio(kJInputs, c), audio(c, a, 0.7f), audio(c, b, 0.5f), audio(kJInputs, kJOutputs, 0.8f), audio(a, kJOutputs), audio(b, kJOutputs)};
      g.links.push_back(macro(0, c, pp::FltCutoff, 0.2f, 1.0f));
      g.links.push_back(macro(1, a, pp::DlyFeedback, 0.0f, 0.8f));
      ctl(0, "Tone"), ctl(1, "Echoes");
      break;
    }
    case 1: {  // a delay fed back through a filter
      const uint32_t a = add("gl.delay"), f = add("gl.filter");
      g.links = {audio(kJInputs, a), audio(a, f), audio(f, a, 0.45f), audio(kJInputs, kJOutputs), audio(f, kJOutputs, 0.6f)};
      g.links.push_back(macro(0, f, pp::FltCutoff, 0.1f, 0.9f));
      ctl(0, "Tunnel");
      break;
    }
    case 2: {  // the echoes duck under the input
      const uint32_t a = add("gl.delay"), d = add("gl.scduck");
      g.links = {audio(kJInputs, a), audio(a, d), audio(kJInputs, d, 1, 1), audio(kJInputs, kJOutputs), audio(d, kJOutputs)};
      g.links.push_back(macro(0, d, SdDepth, 0.0f, 1.0f));
      ctl(0, "Duck");
      break;
    }
    case 3: {  // two synths into one reverb
      const uint32_t a = add("gl.synth"), b = add("gl.fm"), r = add("gl.reverb");
      g.links = {notes(kJInputs, a), notes(kJInputs, b), audio(a, r), audio(b, r, 0.6f), audio(r, kJOutputs)};
      break;
    }
    case 4: {  // arpeggiated synth through an echo
      const uint32_t arp = add("gl.arp"), a = add("gl.synth"), d = add("gl.delay");
      g.links = {notes(kJInputs, arp), notes(arp, a), audio(a, d), audio(d, kJOutputs)};
      g.links.push_back(macro(0, d, pp::DlyFeedback, 0.0f, 0.8f));
      ctl(0, "Echoes");
      break;
    }
    default: {  // low keys play one instrument, high keys another
      const uint32_t sp = add("gl.split"), a = add("gl.fm"), b = add("gl.pluck");
      if (JNode* n = g.node(sp)) {
        const auto set = [&](int zone, int field, float v) { n->plugin.params[static_cast<size_t>(splitParam(zone, field))] = v; };
        set(0, SpOn, 1), set(0, SpLow, 0), set(0, SpHigh, 59);
        set(1, SpOn, 1), set(1, SpLow, 60), set(1, SpHigh, 127);
      }
      g.links = {notes(kJInputs, sp), notes(sp, a, 1), notes(sp, b, 2), audio(a, kJOutputs), audio(b, kJOutputs)};
      break;
    }
  }
  jAutoArrange(g);
  return g;
}

}  // namespace gl::packs::modulation
