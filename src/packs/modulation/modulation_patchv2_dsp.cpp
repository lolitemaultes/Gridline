// Gridline — Patch Host graphs, version 2 (see patchv2.h).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include "../../core/Json.h"
#include "../../engine/Program.h"
#include "../Packs.h"
#include "modulation.h"
#include "patchv2.h"

namespace gl::packs::modulation {

namespace {

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
float rdf(const uint8_t* p) {
  const uint32_t u = rd32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

Json num(double v) { return Json::makeNumber(v); }

Json stateJson(const PluginState& p) {
  Json j = Json::makeObject();
  j.object["type"] = Json::makeString(p.type);
  Json a = Json::makeArray();
  for (float v : p.params) a.array.push_back(num(v));
  j.object["params"] = std::move(a);
  if (!p.data.empty()) j.object["data"] = Json::makeString(p.data);
  if (!p.ins.empty()) j.object["ins"] = Json::makeString(p.ins);
  return j;
}

}  // namespace

// ---------------------------------------------------------------- chunks
std::vector<PatchChunk> patchChunks(const uint8_t* p, size_t n) {
  std::vector<PatchChunk> out;
  size_t o = 0;
  while (o + 12 <= n) {
    PatchChunk c;
    c.id = rd32(p + o);
    const uint64_t size = rd64(p + o + 4);
    if (size > n - o - 12) break;
    c.data.assign(p + o + 12, p + o + 12 + size);
    out.push_back(std::move(c));
    o += 12 + static_cast<size_t>(size);
  }
  return out;
}

std::string patchHex(const std::vector<uint8_t>& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (uint8_t c : b) s += d[c >> 4], s += d[c & 15];
  return s;
}

std::vector<uint8_t> patchUnhex(const std::string& s) {
  std::vector<uint8_t> out;
  auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(v(s[i]) * 16 + v(s[i + 1])));
  return out;
}

std::vector<uint8_t> patchChunkBytes(uint32_t id, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> o;
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(id >> (8 * i)));
  const uint64_t n = payload.size();
  for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(n >> (8 * i)));
  o.insert(o.end(), payload.begin(), payload.end());
  return o;
}

// ------------------------------------------------------------------ maps
double PatchV2Map::eval(double n) const {
  n = std::clamp(n, 0.0, 1.0);
  double v;
  if (tab.size() >= 2) {
    const double x = n * static_cast<double>(tab.size() - 1);
    const size_t i = std::min(tab.size() - 2, static_cast<size_t>(x));
    const double f = x - static_cast<double>(i);
    v = tab[i] + (tab[i + 1] - tab[i]) * f;
  } else {
    v = lo + (hi - lo) * n;
  }
  return integer ? std::round(v) : v;
}

double PatchV2Map::invert(double v) const {
  if (tab.size() < 2) return hi != lo ? std::clamp((v - lo) / (hi - lo), 0.0, 1.0) : 0.0;
  double best = 0, err = 1e300;
  for (size_t i = 0; i < tab.size(); ++i) {
    const double e = std::fabs(tab[i] - v);
    if (e < err) err = e, best = static_cast<double>(i) / static_cast<double>(tab.size() - 1);
  }
  return best;
}

const PatchV2Map* PatchV2Node::map(int fl) const {
  for (const auto& m : maps)
    if (m.fl == fl) return &m;
  return nullptr;
}

bool PatchV2Node::operator==(const PatchV2Node& o) const {
  return plugin == o.plugin && raw == o.raw && orig == o.orig && flName == o.flName && display == o.display && id == o.id && head == o.head && tail == o.tail && maps == o.maps;
}

bool PatchV2::operator==(const PatchV2& o) const { return version == o.version && items == o.items && nodes == o.nodes && slots == o.slots; }

// ------------------------------------------------------------------- JSON
bool isPatchV2(const std::string& data) { return data.size() > 8 && data.compare(0, 7, "{\"v\":2,") == 0; }

std::string serializePatchV2(const PatchV2& g) {
  // Written by hand: a fixed key order starting with "v" (isPatchV2).
  Json items = Json::makeArray();
  for (const auto& s : g.items) items.array.push_back(Json::makeString(s));
  Json nodes = Json::makeArray();
  for (const auto& n : g.nodes) {
    Json j = stateJson(n.plugin);
    j.object["fl"] = Json::makeString(n.flName);
    j.object["disp"] = Json::makeString(n.display);
    j.object["id"] = num(static_cast<double>(n.id));
    j.object["head"] = Json::makeString(n.head);
    j.object["tail"] = Json::makeString(n.tail);
    if (!n.raw.empty()) j.object["raw"] = Json::makeString(n.raw);
    if (!n.orig.empty()) j.object["orig"] = Json::makeString(n.orig);
    Json maps = Json::makeArray();
    for (const auto& m : n.maps) {
      Json e = Json::makeArray();
      e.array.push_back(num(m.fl));
      e.array.push_back(num(m.gl));
      if (m.tab.size() >= 2) {
        Json t = Json::makeArray();
        for (float v : m.tab) t.array.push_back(num(v));
        e.array.push_back(std::move(t));
        e.array.push_back(num(m.integer ? 1 : 0));
      } else {
        e.array.push_back(num(m.lo));
        e.array.push_back(num(m.hi));
        e.array.push_back(num(m.integer ? 1 : 0));
      }
      maps.array.push_back(std::move(e));
    }
    j.object["maps"] = std::move(maps);
    nodes.array.push_back(std::move(j));
  }
  Json slots = Json::makeArray();
  for (int s : g.slots) slots.array.push_back(num(s));
  return "{\"v\":2,\"ver\":" + std::to_string(g.version) + ",\"items\":" + jsonStringify(items, 0) + ",\"nodes\":" + jsonStringify(nodes, 0) +
         ",\"slots\":" + jsonStringify(slots, 0) + "}";
}

bool parsePatchV2(const std::string& data, PatchV2& out) {
  out = PatchV2{};
  if (!isPatchV2(data)) return false;
  Json j;
  if (!jsonParse(data, j) || !j.is(Json::Type::Object)) return false;
  out.version = jsonInt<uint32_t>(j.num("ver", 4));
  if (const Json* it = j.find("items"); it && it->is(Json::Type::Array))
    for (const auto& s : it->array)
      if (s.is(Json::Type::String)) out.items.push_back(s.str);
  if (const Json* ns = j.find("nodes"); ns && ns->is(Json::Type::Array))
    for (const auto& jn : ns->array) {
      PatchV2Node n;
      n.plugin.type = jn.text("type");
      n.plugin.data = jn.text("data");
      n.plugin.ins = jn.text("ins");
      if (const Json* a = jn.find("params"); a && a->is(Json::Type::Array))
        for (const auto& v : a->array) n.plugin.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
      n.flName = jn.text("fl");
      n.display = jn.text("disp");
      n.id = jsonInt<uint64_t>(jn.num("id", 0));
      n.head = jn.text("head");
      n.tail = jn.text("tail");
      n.raw = jn.text("raw");
      n.orig = jn.text("orig");
      if (const Json* ms = jn.find("maps"); ms && ms->is(Json::Type::Array))
        for (const auto& e : ms->array) {
          if (!e.is(Json::Type::Array) || e.array.size() < 3) continue;
          PatchV2Map m;
          m.fl = jsonInt<int>(e.array[0].number);
          m.gl = jsonInt<int>(e.array[1].number);
          if (e.array[2].is(Json::Type::Array)) {
            for (const auto& v : e.array[2].array) m.tab.push_back(static_cast<float>(v.number));
            m.integer = e.array.size() > 3 && e.array[3].number != 0;
          } else {
            m.lo = e.array[2].number;
            m.hi = e.array.size() > 3 ? e.array[3].number : 1.0;
            m.integer = e.array.size() > 4 && e.array[4].number != 0;
          }
          n.maps.push_back(std::move(m));
        }
      out.nodes.push_back(std::move(n));
    }
  if (const Json* sl = j.find("slots"); sl && sl->is(Json::Type::Array))
    for (const auto& v : sl->array) out.slots.push_back(jsonInt<int>(v.number));
  return true;
}

// --------------------------------------------------------------- topology
PatchV2Topo patchV2Topology(const PatchV2& g) {
  PatchV2Topo t;
  const int N = static_cast<int>(g.nodes.size());
  const int NC = std::min(N, kV2MaxNodes);  // nodes past the cap are kept (export) but not run
  t.notes.assign(static_cast<size_t>(N), 0);
  if (N > NC) t.unsupported.push_back("more than " + std::to_string(kV2MaxNodes) + " plugins");
  struct Port {
    int node = -1;          // −1: patch I/O
    uint32_t cls = 0, index = 0, kind = 0;
    std::string io;         // I/O name
  };
  std::multimap<uint64_t, Port> ports;
  for (int k = 0; k < NC; ++k) {
    const std::vector<uint8_t> h = patchUnhex(g.nodes[static_cast<size_t>(k)].head);
    const std::vector<uint8_t> tl = patchUnhex(g.nodes[static_cast<size_t>(k)].tail);
    for (const auto* buf : {&h, &tl})
      for (const PatchChunk& c : patchChunks(buf->data(), buf->size()))
        if (c.id == 5009 && c.data.size() >= 20) {
          Port p;
          p.node = k;
          p.cls = rd32(c.data.data());
          p.index = rd32(c.data.data() + 4);
          p.kind = rd32(c.data.data() + 16);
          ports.emplace(rd64(c.data.data() + 8), p);
        }
  }
  struct Link {
    uint64_t dst, src;
    float gain;
  };
  std::vector<Link> links;
  std::vector<std::pair<uint64_t, int>> exposed;  // port, FL index
  for (const auto& s : g.items) {
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
        for (const auto& k2 : kids)
          if (k2.id == 1402) {
            const bool wide = k2.data.size() >= 2 && k2.data[1] == 0;
            for (size_t i = 0; i < k2.data.size(); i += wide ? 2 : 1)
              if (k2.data[i]) p.io += static_cast<char>(k2.data[i]);
          }
        ports.emplace(rd64(kids[0].data.data()), p);
      } else if (kids[0].id == 1300 && kids[0].data.size() >= 20) {
        links.push_back({rd64(kids[0].data.data()), rd64(kids[0].data.data() + 8), rdf(kids[0].data.data() + 16)});
      }
    }
  }
  auto findPort = [&](uint64_t id, bool output) -> const Port* {
    auto [a, b] = ports.equal_range(id);
    const Port* any = nullptr;
    for (auto it = a; it != b; ++it) {
      const Port& p = it->second;
      any = &p;
      const bool isOut = p.node >= 0 ? (p.cls == 2 ? (p.index & 0x10000000u) != 0 || p.kind == 2 : p.kind == 2 || p.kind == 0)
                                     : (p.kind == 7 || p.kind == 5 || p.kind == 1);
      if (isOut == output) return &p;
    }
    return any;
  };
  for (const Link& l : links) {
    const Port* s = findPort(l.src, true);
    const Port* d = findPort(l.dst, false);
    if (!s || !d) {
      t.unsupported.push_back("link to an unknown port");
      continue;
    }
    if (s->cls == 0 && d->cls == 0) {  // audio
      PatchV2Topo::AEdge e;
      e.gain = std::isfinite(l.gain) ? l.gain : 1.0f;
      if (s->node < 0) {
        if (s->kind != 7) {
          t.unsupported.push_back("audio from " + s->io);
          continue;
        }
        e.src = -1;
      } else {
        if (s->kind != 2 || s->index != 0) {
          t.unsupported.push_back("auxiliary audio output");
          continue;
        }
        e.src = s->node;
      }
      if (d->node < 0) {
        if (d->kind != 6) {
          t.unsupported.push_back("audio to " + d->io);
          continue;
        }
        e.dst = -2;
      } else {
        if (d->kind != 3 || d->index != 0) {
          t.unsupported.push_back("sidechain / auxiliary audio input");
          continue;
        }
        e.dst = d->node;
      }
      t.audio.push_back(e);
    } else if (s->cls == 1 && d->cls == 1) {  // notes
      if (s->node < 0 && s->io == "Notes" && d->node >= 0 && d->kind == 3) {
        t.notes[static_cast<size_t>(d->node)] = 1;
      } else {
        t.unsupported.push_back("note routing" + (s->node < 0 ? " from " + s->io : std::string(" between plugins")));
      }
    } else if (s->cls == 2 && d->cls == 2 && s->node >= 0 && d->node >= 0 && (s->index & 0x10000000u) && s->kind == 10 && d->kind == 11) {
      PatchV2Topo::CEdge e;
      e.src = s->node, e.out = static_cast<int>(s->index & 0x0FFFFFFFu);
      e.dst = d->node, e.fl = static_cast<int>(d->index);
      t.controls.push_back(e);
    } else {
      t.unsupported.push_back("control port kind " + std::to_string(s->kind) + " → " + std::to_string(d->kind));
    }
  }
  // Host parameter slots.
  for (int fl : g.slots) {
    PatchV2Topo::Slot sl;
    for (const auto& [port, idx] : exposed)
      if (idx == fl) {
        auto [a, b] = ports.equal_range(port);
        for (auto it = a; it != b; ++it)
          if (it->second.node >= 0 && it->second.cls == 2 && it->second.kind == 11) sl.node = it->second.node, sl.fl = static_cast<int>(it->second.index);
      }
    t.slots.push_back(sl);
  }
  // Order: Kahn over audio and control dependencies; cycles keep FL's order.
  std::vector<std::vector<int>> out(static_cast<size_t>(N));
  std::vector<int> indeg(static_cast<size_t>(N), 0);
  auto edge = [&](int a, int b) {
    if (a < 0 || b < 0 || a == b) return;
    out[static_cast<size_t>(a)].push_back(b);
    ++indeg[static_cast<size_t>(b)];
  };
  for (const auto& e : t.audio) edge(e.src, e.dst);
  for (const auto& e : t.controls) edge(e.src, e.dst);
  std::vector<char> placed(static_cast<size_t>(N), 0);
  for (bool progress = true; progress;) {
    progress = false;
    for (int k = 0; k < NC; ++k) {
      if (placed[static_cast<size_t>(k)] || indeg[static_cast<size_t>(k)] > 0) continue;
      placed[static_cast<size_t>(k)] = 1;
      t.order.push_back(k);
      for (int b : out[static_cast<size_t>(k)]) --indeg[static_cast<size_t>(b)];
      progress = true;
    }
  }
  for (int k = 0; k < NC; ++k)
    if (!placed[static_cast<size_t>(k)]) t.order.push_back(k);  // feedback: previous block's values
  return t;
}

// ---------------------------------------------------------------- runtime
namespace {

uint64_t typeHash(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  return h;
}

struct V2Aux final : AuxData {
  PatchV2 graph;
  PatchV2Topo topo;
  std::vector<ChannelAux> nodeAux;
  std::vector<uint64_t> hash;
  std::vector<char> isGen, runs;
  mutable std::vector<std::unique_ptr<Generator>> gens;
  mutable std::vector<std::unique_ptr<Effect>> fxs;
  mutable std::vector<PluginState> live;
  // Per control edge / host slot: the value last applied (NaN = never).
  mutable std::vector<float> lastControl, lastSlot;
  std::vector<std::vector<int>> controlsInto;  // per node: control edges into it
  std::vector<std::vector<int>> audioInto;     // per node (and the host output at index N)
};

constexpr int kFrames = 256;

}  // namespace

void preparePatchV2(const PluginState& state, const PatchV2& g, ChannelAux& aux) {
  auto a = std::make_shared<V2Aux>();
  a->graph = g;
  a->topo = patchV2Topology(g);
  const size_t N = g.nodes.size();
  a->gens.resize(N);
  a->fxs.resize(N);
  a->isGen.assign(N, 0);
  a->runs.assign(N, 0);
  a->controlsInto.resize(N);
  a->audioInto.resize(N + 1);
  for (size_t i = 0; i < N; ++i) {
    PluginState ps = g.nodes[i].plugin;
    normalizePluginState(ps);
    const PluginInfo* info = findPlugin(ps.type);
    const bool hostable = info && ps.type != "gl.patcher" && ps.type != "gl.patcherfx" && info->kind != PluginKind::NoteFx;
    const bool gen = hostable && info->kind == PluginKind::Generator;
    a->isGen[i] = gen;
    a->hash.push_back(typeHash(ps.type));
    ChannelAux na;
    if (hostable) {
      if (gen) {
        prepareChannelAux(ps, na);
        a->gens[i] = gl::createGenerator(ps.type);
        if (a->gens[i]) a->gens[i]->prepareHost(ps, 44100.0);
      } else {
        packs::prepareAux(ps, na);
        a->fxs[i] = gl::createEffect(ps.type);
        if (a->fxs[i]) a->fxs[i]->prepareHost(ps, 44100.0);
      }
      a->runs[i] = (a->gens[i] || a->fxs[i]) ? 1 : 0;
    }
    a->nodeAux.push_back(std::move(na));
    a->live.push_back(ps);
  }
  for (size_t e = 0; e < a->topo.controls.size(); ++e) {
    const auto& c = a->topo.controls[e];
    if (c.dst >= 0 && static_cast<size_t>(c.dst) < N) a->controlsInto[static_cast<size_t>(c.dst)].push_back(static_cast<int>(e));
  }
  for (size_t e = 0; e < a->topo.audio.size(); ++e) {
    const auto& l = a->topo.audio[e];
    const size_t at = l.dst == -2 ? N : static_cast<size_t>(l.dst);
    if (at <= N) a->audioInto[at].push_back(static_cast<int>(e));
  }
  a->lastControl.assign(a->topo.controls.size(), std::nanf(""));
  // Host slots start at the values their targets hold (nothing to apply).
  a->lastSlot.assign(a->topo.slots.size(), std::nanf(""));
  for (size_t k = 0; k < a->topo.slots.size() && k < static_cast<size_t>(kPatchMaps); ++k)
    a->lastSlot[k] = static_cast<size_t>(PhMap0 + static_cast<int>(k)) < state.params.size() ? state.params[static_cast<size_t>(PhMap0) + k] : 0.f;
  aux.custom = a;
}

struct PatchV2Runtime::Impl {
  const V2Aux* synced = nullptr;
  std::vector<uint64_t> hash;
  std::vector<std::unique_ptr<Generator>> gens;
  std::vector<std::unique_ptr<Effect>> fxs;
  std::vector<float> bufL, bufR;  // kV2MaxNodes × kFrames
  std::vector<float> inL, inR, outs;
  Impl() {
    hash.assign(kV2MaxNodes, 0);
    gens.resize(kV2MaxNodes);
    fxs.resize(kV2MaxNodes);
    bufL.assign(static_cast<size_t>(kV2MaxNodes) * kFrames, 0.f);
    bufR.assign(static_cast<size_t>(kV2MaxNodes) * kFrames, 0.f);
    inL.assign(kFrames, 0.f), inR.assign(kFrames, 0.f);
    outs.assign(static_cast<size_t>(kV2MaxNodes) * kV2MaxOutputs, 0.f);
  }
};

PatchV2Runtime::PatchV2Runtime() : d_(std::make_unique<Impl>()) {}
PatchV2Runtime::~PatchV2Runtime() = default;

bool PatchV2Runtime::sync(const AuxData* aux) {
  const auto* a = dynamic_cast<const V2Aux*>(aux);
  if (!a) {
    d_->synced = nullptr;
    return false;
  }
  if (a == d_->synced) return true;
  d_->synced = a;
  const size_t N = std::min(a->graph.nodes.size(), static_cast<size_t>(kV2MaxNodes));
  for (size_t i = 0; i < static_cast<size_t>(kV2MaxNodes); ++i) {
    const uint64_t h = i < N ? a->hash[i] : 0;
    if (h != d_->hash[i] || (!d_->gens[i] && !d_->fxs[i])) {
      if (i < N) {
        std::swap(d_->gens[i], a->gens[i]);  // the old instance is freed with the graph (UI thread)
        std::swap(d_->fxs[i], a->fxs[i]);
      }
      d_->hash[i] = h;
    }
  }
  return true;
}

void PatchV2Runtime::noteOn(uint32_t id, double pitch, float velocity, const NoteProps& props) {
  const V2Aux* a = d_->synced;
  if (!a) return;
  for (size_t i = 0; i < a->graph.nodes.size() && i < static_cast<size_t>(kV2MaxNodes); ++i)
    if (a->topo.notes[i] && d_->gens[i]) d_->gens[i]->noteOn(id, pitch, velocity, 0, props);
}

void PatchV2Runtime::noteOff(uint32_t id) {
  const V2Aux* a = d_->synced;
  if (!a) return;
  for (size_t i = 0; i < a->graph.nodes.size() && i < static_cast<size_t>(kV2MaxNodes); ++i)
    if (a->topo.notes[i] && d_->gens[i]) d_->gens[i]->noteOff(id, 0);
}

void PatchV2Runtime::allOff() {
  for (auto& g : d_->gens)
    if (g) g->allNotesOff(0);
}

void PatchV2Runtime::kill() {
  for (auto& g : d_->gens)
    if (g) g->kill();
  for (auto& f : d_->fxs)
    if (f) f->reset();
}

bool PatchV2Runtime::active() const {
  for (const auto& g : d_->gens)
    if (g && g->active()) return true;
  return false;
}

void PatchV2Runtime::run(const PluginState& host, const ProcessContext& ctx, float* L, float* R, int n, bool instrument) {
  const V2Aux* a = d_->synced;
  if (!a) return;
  n = std::min(n, kFrames);
  const size_t N = std::min(a->graph.nodes.size(), static_cast<size_t>(kV2MaxNodes));
  // Host Param slots: applied to their targets when they change.
  for (size_t k = 0; k < a->topo.slots.size() && k < static_cast<size_t>(kPatchMaps); ++k) {
    const auto& sl = a->topo.slots[k];
    const float v = static_cast<size_t>(PhMap0) + k < host.params.size() ? host.params[static_cast<size_t>(PhMap0) + k] : 0.f;
    if (sl.node < 0 || static_cast<size_t>(sl.node) >= N || v == a->lastSlot[k]) continue;
    a->lastSlot[k] = v;
    const PatchV2Map* m = a->graph.nodes[static_cast<size_t>(sl.node)].map(sl.fl);
    PluginState& st = a->live[static_cast<size_t>(sl.node)];
    if (m && m->gl >= 0 && static_cast<size_t>(m->gl) < st.params.size()) st.params[static_cast<size_t>(m->gl)] = static_cast<float>(m->eval(v));
  }
  for (int i : a->topo.order) {
    if (i < 0 || static_cast<size_t>(i) >= N) continue;
    const size_t ui = static_cast<size_t>(i);
    // Control links into this node: applied when the source output changes.
    for (int e : a->controlsInto[ui]) {
      const auto& c = a->topo.controls[static_cast<size_t>(e)];
      if (c.src < 0 || static_cast<size_t>(c.src) >= N || c.out < 0 || c.out >= kV2MaxOutputs) continue;
      const float v = d_->outs[static_cast<size_t>(c.src) * kV2MaxOutputs + static_cast<size_t>(c.out)];
      if (v == a->lastControl[static_cast<size_t>(e)]) continue;
      a->lastControl[static_cast<size_t>(e)] = v;
      const PatchV2Map* m = a->graph.nodes[ui].map(c.fl);
      PluginState& st = a->live[ui];
      if (m && m->gl >= 0 && static_cast<size_t>(m->gl) < st.params.size()) st.params[static_cast<size_t>(m->gl)] = static_cast<float>(m->eval(v));
    }
    float* bl = d_->bufL.data() + ui * kFrames;
    float* br = d_->bufR.data() + ui * kFrames;
    std::memset(bl, 0, sizeof(float) * static_cast<size_t>(n));
    std::memset(br, 0, sizeof(float) * static_cast<size_t>(n));
    if (!a->runs[ui]) continue;
    if (a->isGen[ui]) {
      if (d_->gens[ui]) d_->gens[ui]->process(a->live[ui], a->nodeAux[ui], ctx, bl, br, n);
      if (d_->gens[ui]) d_->gens[ui]->controlOutputs(d_->outs.data() + ui * kV2MaxOutputs, kV2MaxOutputs);
    } else if (d_->fxs[ui]) {
      for (int e : a->audioInto[ui]) {
        const auto& l = a->topo.audio[static_cast<size_t>(e)];
        const float* sl = l.src == -1 ? L : d_->bufL.data() + static_cast<size_t>(l.src) * kFrames;
        const float* sr = l.src == -1 ? R : d_->bufR.data() + static_cast<size_t>(l.src) * kFrames;
        if (l.src == -1 && instrument) continue;  // the instrument version has no audio input
        for (int k = 0; k < n; ++k) bl[k] += sl[k] * l.gain, br[k] += sr[k] * l.gain;
      }
      d_->fxs[ui]->processAux(a->live[ui], a->nodeAux[ui], ctx, bl, br, n);
      d_->fxs[ui]->controlOutputs(d_->outs.data() + ui * kV2MaxOutputs, kV2MaxOutputs);
    }
  }
  // Patch output.
  float* ol = d_->inL.data();
  float* orr = d_->inR.data();
  std::memset(ol, 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(orr, 0, sizeof(float) * static_cast<size_t>(n));
  for (int e : a->audioInto[N]) {
    const auto& l = a->topo.audio[static_cast<size_t>(e)];
    if (l.src == -1 && instrument) continue;
    const float* sl = l.src == -1 ? L : d_->bufL.data() + static_cast<size_t>(l.src) * kFrames;
    const float* sr = l.src == -1 ? R : d_->bufR.data() + static_cast<size_t>(l.src) * kFrames;
    for (int k = 0; k < n; ++k) ol[k] += sl[k] * l.gain, orr[k] += sr[k] * l.gain;
  }
  if (instrument) {
    for (int k = 0; k < n; ++k) L[k] += ol[k], R[k] += orr[k];
  } else {
    std::memcpy(L, ol, sizeof(float) * static_cast<size_t>(n));
    std::memcpy(R, orr, sizeof(float) * static_cast<size_t>(n));
  }
}

}  // namespace gl::packs::modulation
