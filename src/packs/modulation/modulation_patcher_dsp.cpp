// Gridline — "modulation" pack: Patch Host. Hosts a small graph of other
// Gridline plugins: layered instruments (instrument version) feeding a chain
// of effects, where an effect can run in parallel with the previous one.
// Eight macro knobs drive any inner parameters. The graph lives in
// PluginState::data (JSON); inner plugin instances are created on the UI
// thread when the program is built and handed to the runtime, which keeps
// its own instances while their types stay the same (so tails survive edits).
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../core/Json.h"
#include "../../core/PluginParams.h"
#include "../../engine/Dsp.h"
#include "../../engine/Program.h"
#include "../Packs.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "patchv2.h"

namespace gl::packs::modulation {

namespace {

constexpr int kMaxFrames = 1024;  // AudioEngine::kMaxBlock

uint64_t typeHash(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  return h;
}

bool hostable(const std::string& type, bool fxOnly) {
  if (type == "gl.patcher" || type == "gl.patcherfx") return false;  // no nesting
  const PluginInfo* p = findPlugin(type);
  if (!p || p->kind == PluginKind::NoteFx) return false;
  return !(fxOnly && p->kind != PluginKind::Effect);
}

PatchGraph defaultGraph(bool fx) {
  PatchGraph g;
  auto node = [](const char* type) {
    PatchNode n;
    n.plugin = defaultPluginState(type);
    return n;
  };
  if (!fx) g.nodes.push_back(node("gl.synth"));
  g.nodes.push_back(node("gl.filter"));
  PatchNode dly = node("gl.delay");
  dly.parallel = fx;
  dly.mix = fx ? 0.5f : 0.35f;
  g.nodes.push_back(dly);
  int filt = fx ? 0 : 1;
  g.macros[0].push_back({filt, pp::FltCutoff, 0.15f, 1.0f});
  g.macros[1].push_back({filt, pp::FltReso, 0.0f, 0.8f});
  g.macros[2].push_back({fx ? 1 : 2, pp::DlyFeedback, 0.0f, 0.85f});
  g.macroNames[0] = "Tone";
  g.macroNames[1] = "Bite";
  g.macroNames[2] = "Echoes";
  return g;
}

// Prepared graph: parsed nodes, their aux data, and ready instances.
struct PatchAux final : AuxData {
  PatchGraph graph;
  bool fx = false;
  std::vector<ChannelAux> nodeAux;
  std::vector<uint64_t> hash;  // per node type hash
  std::vector<bool> isGen;
  // Audio thread swaps instances with these (index = node); replaced
  // instances end up here and are freed with the graph on the UI thread.
  mutable std::unique_ptr<Generator> gens[kPatchNodes];
  mutable std::unique_ptr<Effect> fxs[kPatchNodes];
  mutable std::vector<PluginState> live;  // node states with macros applied
  std::vector<ParamDesc> targetDesc[8];   // per macro target
};

// The hosted graph runtime shared by the instrument and effect versions.
class PatchRuntime {
 public:
  PatchRuntime() {
    for (auto* b : {&aL_, &aR_, &bL_, &bR_, &cL_, &cR_, &dL_, &dR_}) b->assign(kMaxFrames, 0.f);
  }
  // Adopts the instances of a newly prepared graph where node types changed.
  void sync(const PatchAux* pa) {
    if (pa == synced_) return;
    synced_ = pa;
    if (!pa) return;
    for (int i = 0; i < kPatchNodes; ++i) {
      uint64_t h = static_cast<size_t>(i) < pa->hash.size() ? pa->hash[static_cast<size_t>(i)] : 0;
      if (h != hash_[i] || (!gen_[i] && !fx_[i])) {
        std::swap(gen_[i], pa->gens[i]);  // old instance goes to the graph (freed off the audio thread)
        std::swap(fx_[i], pa->fxs[i]);
        hash_[i] = h;
      }
    }
  }
  void applyMacros(const PluginState& host, const PatchAux& pa) {
    for (int m = 0; m < kMacros; ++m) {
      double v = static_cast<size_t>(m) < host.params.size() ? host.params[static_cast<size_t>(m)] : 0.5;
      const auto& tg = pa.graph.macros[m];
      for (size_t k = 0; k < tg.size(); ++k) {
        const PatchMacroTarget& t = tg[k];
        if (t.node < 0 || static_cast<size_t>(t.node) >= pa.live.size()) continue;
        PluginState& st = pa.live[static_cast<size_t>(t.node)];
        if (t.param < 0 || static_cast<size_t>(t.param) >= st.params.size()) continue;
        double nrm = t.min + v * (t.max - t.min);
        st.params[static_cast<size_t>(t.param)] = static_cast<float>(pa.targetDesc[m][k].fromNormalized(nrm));
      }
    }
  }
  void noteOn(uint32_t id, double pitch, float vel) {
    for (int i = 0; i < kPatchNodes; ++i)
      if (gen_[i]) gen_[i]->noteOn(id, pitch, vel, 0);
  }
  void noteOff(uint32_t id) {
    for (int i = 0; i < kPatchNodes; ++i)
      if (gen_[i]) gen_[i]->noteOff(id, 0);
  }
  void allOff() {
    for (int i = 0; i < kPatchNodes; ++i)
      if (gen_[i]) gen_[i]->allNotesOff(0);
  }
  void stopSound() {  // J-110: the user's Stop
    allOff();
    for (int i = 0; i < kPatchNodes; ++i)
      if (fx_[i]) fx_[i]->stopSound();
  }
  void kill() {
    for (int i = 0; i < kPatchNodes; ++i) {
      if (gen_[i]) gen_[i]->kill();
      if (fx_[i]) fx_[i]->reset();
    }
  }
  bool active() const {
    for (int i = 0; i < kPatchNodes; ++i)
      if (gen_[i] && gen_[i]->active()) return true;
    return false;
  }
  // Renders generators (instrument version) into cur, then the effect chain.
  void run(const PatchAux& pa, const ProcessContext& ctx, float* L, float* R, int n, bool withGenerators) {
    n = std::min(n, kMaxFrames);
    float* cL = cL_.data();
    float* cR = cR_.data();
    if (withGenerators) {
      std::memset(cL, 0, sizeof(float) * static_cast<size_t>(n));
      std::memset(cR, 0, sizeof(float) * static_cast<size_t>(n));
      for (size_t i = 0; i < pa.graph.nodes.size() && i < static_cast<size_t>(kPatchNodes); ++i)
        if (pa.isGen[i] && gen_[i] && !pa.graph.nodes[i].bypass)
          gen_[i]->process(pa.live[i], pa.nodeAux[i], ctx, cL, cR, n);
    } else {
      std::memcpy(cL, L, sizeof(float) * static_cast<size_t>(n));
      std::memcpy(cR, R, sizeof(float) * static_cast<size_t>(n));
    }
    // Effects: serial stages; a parallel effect shares the input of the stage before it.
    float* inL = aL_.data();
    float* inR = aR_.data();
    float* accL = bL_.data();
    float* accR = bR_.data();
    float* tL = dL_.data();
    float* tR = dR_.data();
    bool open = false;  // a stage is collecting in acc
    for (size_t i = 0; i < pa.graph.nodes.size() && i < static_cast<size_t>(kPatchNodes); ++i) {
      if (pa.isGen[i] || !fx_[i] || pa.graph.nodes[i].bypass) continue;
      const PatchNode& nd = pa.graph.nodes[i];
      if (!open || !nd.parallel) {  // close the previous stage: its sum feeds the next
        if (open) {
          std::memcpy(cL, accL, sizeof(float) * static_cast<size_t>(n));
          std::memcpy(cR, accR, sizeof(float) * static_cast<size_t>(n));
        }
        std::memcpy(inL, cL, sizeof(float) * static_cast<size_t>(n));
        std::memcpy(inR, cR, sizeof(float) * static_cast<size_t>(n));
        std::memset(accL, 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(accR, 0, sizeof(float) * static_cast<size_t>(n));
        open = true;
      }
      std::memcpy(tL, inL, sizeof(float) * static_cast<size_t>(n));
      std::memcpy(tR, inR, sizeof(float) * static_cast<size_t>(n));
      fx_[i]->processAux(pa.live[i], pa.nodeAux[i], ctx, tL, tR, n);
      float mix = std::clamp(nd.mix, 0.0f, 1.0f);
      bool parallelStage = nd.parallel;
      for (int k = 0; k < n; ++k) {
        // serial effects blend wet/dry; parallel branches add their wet part
        float wl = inL[k] + (tL[k] - inL[k]) * mix, wr = inR[k] + (tR[k] - inR[k]) * mix;
        if (parallelStage) {
          accL[k] += tL[k] * mix;
          accR[k] += tR[k] * mix;
        } else {
          accL[k] += wl;
          accR[k] += wr;
        }
      }
    }
    if (open) {
      std::memcpy(cL, accL, sizeof(float) * static_cast<size_t>(n));
      std::memcpy(cR, accR, sizeof(float) * static_cast<size_t>(n));
    }
    if (withGenerators) {
      for (int k = 0; k < n; ++k) {
        L[k] += cL[k];
        R[k] += cR[k];
      }
    } else {
      std::memcpy(L, cL, sizeof(float) * static_cast<size_t>(n));
      std::memcpy(R, cR, sizeof(float) * static_cast<size_t>(n));
    }
  }

 private:
  const PatchAux* synced_ = nullptr;
  uint64_t hash_[kPatchNodes] = {};
  std::unique_ptr<Generator> gen_[kPatchNodes];
  std::unique_ptr<Effect> fx_[kPatchNodes];
  std::vector<float> aL_, aR_, bL_, bR_, cL_, cR_, dL_, dR_;
};

class PatchHostGenerator final : public Generator {
 public:
  bool active() const override { return v2on_ ? v2_.active() : rt_.active(); }

 protected:
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override {
    if (v2on_) v2_.noteOn(id, pitch, velocity, props);
    else rt_.noteOn(id, pitch, velocity);
  }
  void onNoteOn(uint32_t id, double pitch, float velocity) override { onNoteOnEx(id, pitch, velocity, NoteProps{}); }
  void onNoteOff(uint32_t id) override {
    if (v2on_) v2_.noteOff(id);
    else rt_.noteOff(id);
  }
  void onAllOff() override {
    v2_.allOff();
    rt_.allOff();
  }
  void onKill() override {
    v2_.kill();
    rt_.kill();
  }
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    v2on_ = v2_.sync(aux.custom.get());  // J-41: FL Patcher graphs
    if (v2on_) {
      st_ = &s;
      ctx_ = ctx;
      gain_ = static_cast<float>(dsp::dbToGain(static_cast<size_t>(PhGain) < s.params.size() ? s.params[PhGain] : 0.0));
      return;
    }
    pa_ = dynamic_cast<const PatchAux*>(aux.custom.get());
    rt_.sync(pa_);
    if (pa_) rt_.applyMacros(s, *pa_);
    ctx_ = ctx;
    gain_ = static_cast<float>(dsp::dbToGain(static_cast<size_t>(PhGain) < s.params.size() ? s.params[PhGain] : 0.0));
  }
  void render(float* L, float* R, int n) override {
    if (v2on_) {
      while (n > 0) {
        const int m = std::min(n, 256);
        std::memset(tmpL_, 0, sizeof(float) * static_cast<size_t>(m));
        std::memset(tmpR_, 0, sizeof(float) * static_cast<size_t>(m));
        v2_.run(*st_, ctx_, tmpL_, tmpR_, m, true);
        for (int k = 0; k < m; ++k) L[k] += tmpL_[k] * gain_, R[k] += tmpR_[k] * gain_;
        L += m, R += m, n -= m;
      }
      return;
    }
    if (!pa_) return;
    // Inner instruments render whole sub-blocks between this host's events.
    float* tl = tmpL_;
    float* tr = tmpR_;
    while (n > 0) {
      int m = std::min(n, 256);
      std::memset(tl, 0, sizeof(float) * static_cast<size_t>(m));
      std::memset(tr, 0, sizeof(float) * static_cast<size_t>(m));
      rt_.run(*pa_, ctx_, tl, tr, m, true);
      for (int k = 0; k < m; ++k) {
        L[k] += tl[k] * gain_;
        R[k] += tr[k] * gain_;
      }
      L += m;
      R += m;
      n -= m;
    }
  }

 private:
  PatchRuntime rt_;
  const PatchAux* pa_ = nullptr;
  ProcessContext ctx_;
  float gain_ = 1;
  float tmpL_[256] = {}, tmpR_[256] = {};
  PatchV2Runtime v2_;
  bool v2on_ = false;
  const PluginState* st_ = nullptr;
};

class PatchHostEffect final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    (void)s;
    (void)ctx;
    (void)L;
    (void)R;
    (void)n;  // needs its prepared graph: see processAux
  }
  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R,
                  int n) override {
    if (v2_.sync(aux.custom.get())) {  // J-41: FL Patcher graphs
      const float mix = static_cast<size_t>(PhMix) < s.params.size() ? s.params[PhMix] : 1.f;
      const float gain = static_cast<float>(dsp::dbToGain(static_cast<size_t>(PhGain) < s.params.size() ? s.params[PhGain] : 0.0));
      for (int done = 0; done < n;) {
        const int p = std::min(n - done, 256);
        std::memcpy(dryL_, L + done, sizeof(float) * static_cast<size_t>(p));
        std::memcpy(dryR_, R + done, sizeof(float) * static_cast<size_t>(p));
        v2_.run(s, ctx, L + done, R + done, p, false);
        for (int k = 0; k < p; ++k) {
          L[done + k] = (dryL_[k] + (L[done + k] - dryL_[k]) * mix) * gain;
          R[done + k] = (dryR_[k] + (R[done + k] - dryR_[k]) * mix) * gain;
        }
        done += p;
      }
      return;
    }
    const auto* pa = dynamic_cast<const PatchAux*>(aux.custom.get());
    rt_.sync(pa);
    if (!pa) return;
    rt_.applyMacros(s, *pa);
    float mix = static_cast<size_t>(PhMix) < s.params.size() ? s.params[PhMix] : 1.f;
    float gain = static_cast<float>(dsp::dbToGain(static_cast<size_t>(PhGain) < s.params.size() ? s.params[PhGain] : 0.0));
    int done = 0;
    while (done < n) {
      int p = std::min(n - done, 256);  // pieces small enough for the dry copy
      std::memcpy(dryL_, L + done, sizeof(float) * static_cast<size_t>(p));
      std::memcpy(dryR_, R + done, sizeof(float) * static_cast<size_t>(p));
      rt_.run(*pa, ctx, L + done, R + done, p, false);
      for (int k = 0; k < p; ++k) {
        L[done + k] = (dryL_[k] + (L[done + k] - dryL_[k]) * mix) * gain;
        R[done + k] = (dryR_[k] + (R[done + k] - dryR_[k]) * mix) * gain;
      }
      done += p;
    }
  }
  void reset() override {
    rt_.kill();
    v2_.kill();
  }
  // J-110: Stop reaches the hosted plugins (their notes, their own playback).
  void stopSound() override {
    rt_.stopSound();
    v2_.stopSound();
  }
  // Sum over the serial stages of the latest hosted effect in each (macros
  // applied as the runtime does). A node's undelayed dry part is mixed back
  // in, so a serial node counts while its wet part dominates (mix >= 50 %);
  // the same holds for the host's own mix.
  int latencyFor(const PluginState& s, double sr) const override {
    auto prm = [&](int k, float d) { return static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d; };
    if (prm(PhMix, 1.f) < 0.5f || isPatchV2(s.data)) return 0;  // v2 graphs: hosted latencies are not compensated
    const PatchGraph g = patchGraphOf(s);
    std::vector<PluginState> live;
    for (const PatchNode& nd : g.nodes) live.push_back(nd.plugin);
    for (int m = 0; m < kMacros; ++m) {
      const double v = prm(m, 0.5f);
      for (const PatchMacroTarget& t : g.macros[m]) {
        if (t.node < 0 || static_cast<size_t>(t.node) >= live.size()) continue;
        PluginState& st = live[static_cast<size_t>(t.node)];
        const PluginInfo* info = findPlugin(st.type);
        if (!info || t.param < 0 || static_cast<size_t>(t.param) >= st.params.size() ||
            static_cast<size_t>(t.param) >= info->params.size())
          continue;
        st.params[static_cast<size_t>(t.param)] =
            static_cast<float>(info->params[static_cast<size_t>(t.param)].fromNormalized(t.min + v * (t.max - t.min)));
      }
    }
    int total = 0, stage = 0;
    bool open = false;
    for (size_t i = 0; i < g.nodes.size() && i < static_cast<size_t>(kPatchNodes); ++i) {
      const PatchNode& nd = g.nodes[i];
      const PluginInfo* info = findPlugin(nd.plugin.type);
      if (nd.bypass || !info || info->kind != PluginKind::Effect) continue;
      if (!open || !nd.parallel) total += stage, stage = 0, open = true;
      const float mix = std::clamp(nd.mix, 0.0f, 1.0f);
      if (nd.parallel ? mix > 0.0f : mix >= 0.5f) stage = std::max(stage, gl::effectLatency(live[i], sr));
    }
    return total + stage;
  }

 private:
  PatchRuntime rt_;
  PatchV2Runtime v2_;
  float dryL_[256] = {}, dryR_[256] = {};
};

Json pluginJson(const PluginState& p) {
  Json j = Json::makeObject();
  j.object["type"] = Json::makeString(p.type);
  Json a = Json::makeArray();
  for (float v : p.params) a.array.push_back(Json::makeNumber(v));
  j.object["params"] = std::move(a);
  if (!p.data.empty()) j.object["data"] = Json::makeString(p.data);
  return j;
}

}  // namespace

PatchGraph parsePatchGraph(const std::string& data) {
  PatchGraph g;
  Json j;
  if (data.empty() || !jsonParse(data, j) || !j.is(Json::Type::Object)) return g;
  if (const Json* nodes = j.find("nodes"); nodes && nodes->is(Json::Type::Array))
    for (const auto& jn : nodes->array) {
      if (g.nodes.size() >= static_cast<size_t>(kPatchNodes)) break;
      PatchNode n;
      n.plugin.type = jn.text("type");
      n.plugin.data = jn.text("data");
      if (const Json* a = jn.find("params"); a && a->is(Json::Type::Array))
        for (const auto& v : a->array) n.plugin.params.push_back(static_cast<float>(v.is(Json::Type::Number) ? v.number : 0));
      normalizePluginState(n.plugin);
      const Json* par = jn.find("parallel");
      n.parallel = par && par->is(Json::Type::Bool) && par->boolean;
      const Json* byp = jn.find("bypass");
      n.bypass = byp && byp->is(Json::Type::Bool) && byp->boolean;
      n.mix = static_cast<float>(std::clamp(jn.num("mix", 1), 0.0, 1.0));
      if (findPlugin(n.plugin.type)) g.nodes.push_back(std::move(n));
    }
  if (const Json* macros = j.find("macros"); macros && macros->is(Json::Type::Array))
    for (size_t m = 0; m < macros->array.size() && m < 8; ++m) {
      const Json& jm = macros->array[m];
      g.macroNames[m] = jm.text("name");
      if (const Json* t = jm.find("targets"); t && t->is(Json::Type::Array))
        for (const auto& jt : t->array) {
          PatchMacroTarget mt;
          mt.node = jsonInt<int>(jt.num("node", 0));
          mt.param = jsonInt<int>(jt.num("param", 0));
          mt.min = static_cast<float>(std::clamp(jt.num("min", 0), 0.0, 1.0));
          mt.max = static_cast<float>(std::clamp(jt.num("max", 1), 0.0, 1.0));
          if (mt.node >= 0 && static_cast<size_t>(mt.node) < g.nodes.size()) g.macros[m].push_back(mt);
        }
    }
  return g;
}

std::string serializePatchGraph(const PatchGraph& g) {
  Json j = Json::makeObject();
  Json nodes = Json::makeArray();
  for (const auto& n : g.nodes) {
    Json jn = pluginJson(n.plugin);
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = n.parallel;
    jn.object["parallel"] = b;
    b.boolean = n.bypass;
    jn.object["bypass"] = b;
    jn.object["mix"] = Json::makeNumber(n.mix);
    nodes.array.push_back(std::move(jn));
  }
  j.object["nodes"] = std::move(nodes);
  Json macros = Json::makeArray();
  for (int m = 0; m < 8; ++m) {
    Json jm = Json::makeObject();
    jm.object["name"] = Json::makeString(g.macroNames[m]);
    Json t = Json::makeArray();
    for (const auto& mt : g.macros[m]) {
      Json jt = Json::makeObject();
      jt.object["node"] = Json::makeNumber(mt.node);
      jt.object["param"] = Json::makeNumber(mt.param);
      jt.object["min"] = Json::makeNumber(mt.min);
      jt.object["max"] = Json::makeNumber(mt.max);
      t.array.push_back(std::move(jt));
    }
    jm.object["targets"] = std::move(t);
    macros.array.push_back(std::move(jm));
  }
  j.object["macros"] = std::move(macros);
  return jsonStringify(j, 0);
}

PatchGraph patchGraphOf(const PluginState& state) {
  bool fx = state.type == "gl.patcherfx";
  PatchGraph g = state.data.empty() ? defaultGraph(fx) : parsePatchGraph(state.data);
  // Drop what this host cannot run (nested hosts, instruments in the effect version).
  PatchGraph out;
  std::vector<int> remap(g.nodes.size(), -1);
  for (size_t i = 0; i < g.nodes.size(); ++i)
    if (hostable(g.nodes[i].plugin.type, fx)) {
      remap[i] = static_cast<int>(out.nodes.size());
      out.nodes.push_back(g.nodes[i]);
    }
  for (int m = 0; m < 8; ++m) {
    out.macroNames[m] = g.macroNames[m];
    for (auto t : g.macros[m])
      if (t.node >= 0 && static_cast<size_t>(t.node) < remap.size() && remap[static_cast<size_t>(t.node)] >= 0) {
        t.node = remap[static_cast<size_t>(t.node)];
        out.macros[m].push_back(t);
      }
  }
  return out;
}

void preparePatchHost(const PluginState& state, ChannelAux& aux) {
  if (isPatchV2(state.data)) {  // J-41: FL Patcher graphs
    PatchV2 g;
    if (parsePatchV2(state.data, g)) {
      preparePatchV2(state, g, aux);
      return;
    }
  }
  auto pa = std::make_shared<PatchAux>();
  pa->fx = state.type == "gl.patcherfx";
  pa->graph = patchGraphOf(state);
  for (size_t i = 0; i < pa->graph.nodes.size(); ++i) {
    const PluginState& ps = pa->graph.nodes[i].plugin;
    const PluginInfo* info = findPlugin(ps.type);
    bool gen = info && info->kind == PluginKind::Generator;
    pa->isGen.push_back(gen);
    pa->hash.push_back(typeHash(ps.type));
    ChannelAux na;
    if (gen) {
      prepareChannelAux(ps, na);
      pa->gens[i] = gl::createGenerator(ps.type);
      // J-30: instruments allocate the memory their settings need here (UI thread).
      if (pa->gens[i]) pa->gens[i]->prepareHost(ps, 44100.0);
    } else {
      packs::prepareAux(ps, na);
      pa->fxs[i] = gl::createEffect(ps.type);
      if (pa->fxs[i]) pa->fxs[i]->prepareHost(ps, 44100.0);
    }
    pa->nodeAux.push_back(std::move(na));
    pa->live.push_back(ps);
  }
  for (int m = 0; m < 8; ++m)
    for (const auto& t : pa->graph.macros[m]) {
      const PluginInfo* info = findPlugin(pa->graph.nodes[static_cast<size_t>(t.node)].plugin.type);
      pa->targetDesc[m].push_back(info && static_cast<size_t>(t.param) < info->params.size() ? info->params[static_cast<size_t>(t.param)]
                                                                                              : ParamDesc{});
    }
  aux.custom = pa;
}

std::unique_ptr<Generator> createPatchHostGenerator() { return std::make_unique<PatchHostGenerator>(); }
std::unique_ptr<Effect> createPatchHostEffect() { return std::make_unique<PatchHostEffect>(); }

}  // namespace gl::packs::modulation
