// Gridline — "dynamics" pack, J-42: Pocket Rack runtime (gl.pocketrack).
// Up to four modules (pocket_mods.h) in series. Each slot runs through the
// reference's module wrapper: off / zero mix = bypass; mix 1 with a centred
// wet balance = in place, then a smoothed post gain (starting at −6 dB, as the
// reference); otherwise the module runs on a copy, post gain on the wet only,
// and a smoothed per-side wet amount mixes it back.
#include <atomic>
#include "dyn_dsp.h"
#include "pocket_mods.h"

namespace gl::packs::dynamics {

namespace {

namespace pk = pocket;

int latencyOfModule(int type, const float* p, double sr) {
  switch (type) {
    case 3: return 1;
    case 7: case 30: return 8;
    case 9: {
      const int sl = std::clamp(static_cast<int>(2 * p[4]), 0, 2);
      return sl == 0 ? 1 : sl == 1 ? 2 : 4;
    }
    case 27: return 256;
    case 28: return std::max(1, static_cast<int>(0.002 * sr));
    default: return 0;
  }
}

class PocketRackFx final : public Effect {
 public:
  PocketRackFx() = default;
  PocketRackFx(const PocketRackFx&) = delete;
  PocketRackFx& operator=(const PocketRackFx&) = delete;
  ~PocketRackFx() override {
    for (auto& s : slots_) delete s.pending.exchange(nullptr), delete s.retired.exchange(nullptr);
  }

  // UI thread: builds the modules (allocation happens here).
  void prepareHost(const PluginState& s, double sr) override {
    for (int k = 0; k < kPkSlots; ++k) {
      const int ty = typeOf(s, k);
      Slot& sl = slots_[k];
      delete sl.retired.exchange(nullptr);  // a module the audio thread replaced
      if (ty == sl.builtType && sr == sl.builtSr) continue;
      auto m = pk::makeModule(ty);
      if (m) {
        float p[pk::kParams];
        params(s, k, p);
        m->set(p);
        m->prepare(sr);
      }
      Pending* np = new Pending{std::move(m), ty, sr};
      delete sl.pending.exchange(np);
      sl.builtType = ty, sl.builtSr = sr;
    }
  }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    pk::Host h;
    h.bpm = ctx.tempo, h.playing = ctx.playing;
    const double beat = ctx.tick / std::max(1, ctx.ppq);
    for (int k = 0; k < kPkSlots; ++k) adopt(s, k, ctx.sampleRate);
    for (int o = 0; o < n; o += pk::kMaxBlock) {
      const int m = std::min(pk::kMaxBlock, n - o);
      h.pos = ctx.playing ? beat + o / (60.0 / std::max(1.0, ctx.tempo) * ctx.sampleRate) : -1;
      for (int k = 0; k < kPkSlots; ++k) runSlot(s, k, L + o, R + o, m, h);
    }
  }
  void reset() override {
    for (auto& sl : slots_) {
      if (sl.mod) sl.mod->prepare(sl.mod->sr());
      sl.gPost = 0.5f, sl.mixS = 1, sl.panS = 0.5f;
    }
  }
  int latencyFor(const PluginState& s, double sr) const override {
    int lat = 0;
    for (int k = 0; k < kPkSlots; ++k) {
      float p[pk::kParams];
      params(s, k, p);
      if (p[0] >= 0.5f && s.params.size() > static_cast<size_t>(pkParam(k, PkMix)) && s.params[static_cast<size_t>(pkParam(k, PkMix))] != 0)
        lat += latencyOfModule(typeOf(s, k), p, sr);
    }
    return lat;
  }

 private:
  struct Pending {
    std::unique_ptr<pk::Module> mod;
    int type = 0;
    double sr = 0;
  };
  struct Slot {
    std::unique_ptr<pk::Module> mod;
    int type = -1;
    double sr = 0;
    std::atomic<Pending*> pending{nullptr}, retired{nullptr};
    int builtType = -1;
    double builtSr = 0;
    float gPost = 0.5f, mixS = 1, panS = 0.5f;
  };
  static int typeOf(const PluginState& s, int k) {
    const size_t i = static_cast<size_t>(pkParam(k, PkType));
    return i < s.params.size() ? std::clamp(static_cast<int>(std::lround(s.params[i])), 0, kPkTypes - 1) : 0;
  }
  static void params(const PluginState& s, int k, float* p) {
    for (int i = 0; i < pk::kParams; ++i) {
      const size_t at = static_cast<size_t>(pkParam(k, PkP0 + i));
      p[i] = at < s.params.size() ? s.params[at] : 0.f;
    }
  }
  void adopt(const PluginState& s, int k, double sr) {
    Slot& sl = slots_[k];
    if (!sl.retired.load(std::memory_order_acquire))
      if (Pending* p = sl.pending.exchange(nullptr, std::memory_order_acq_rel)) {
        if (p->type == typeOf(s, k) && p->sr == sr) {
          std::swap(sl.mod, p->mod);  // the old module goes back to the UI thread inside p
          sl.type = p->type, sl.sr = p->sr;
          sl.gPost = 0.5f, sl.mixS = 1, sl.panS = 0.5f;
        }
        sl.retired.store(p, std::memory_order_release);
      }
    const int ty = typeOf(s, k);
    if (ty != sl.type || sr != sl.sr) {  // not prepared by the host (tests, offline use): build here
      sl.mod = pk::makeModule(ty);
      if (sl.mod) {
        float p[pk::kParams];
        params(s, k, p);
        sl.mod->set(p);
        sl.mod->prepare(sr);
      }
      sl.type = ty, sl.sr = sr;
      sl.gPost = 0.5f, sl.mixS = 1, sl.panS = 0.5f;
    }
  }
  void runSlot(const PluginState& s, int k, float* L, float* R, int n, const pk::Host& h) {
    Slot& sl = slots_[k];
    if (!sl.mod) return;
    float p[pk::kParams];
    params(s, k, p);
    sl.mod->set(p);
    auto P = [&](int f, float d) {
      const size_t i = static_cast<size_t>(pkParam(k, f));
      return i < s.params.size() ? s.params[i] : d;
    };
    const float post = P(PkPost, 0.5f), mix = P(PkMix, 1), pan = P(PkPan, 0.5f);
    const float postDb = 40 * post - 20;
    const float postLin = postDb < -60 ? 0.f : std::pow(10.f, postDb / 20);
    if (mix == 0 || p[0] < 0.5f) return;
    auto ramp = [&](float* A, float* B) {
      for (int i = 0; i < n; ++i) {
        sl.gPost += (postLin - sl.gPost) * 0.01f;
        if (std::fabs(sl.gPost - postLin) < 0.001f) sl.gPost = postLin;
        if (sl.gPost != 1) A[i] *= sl.gPost, B[i] *= sl.gPost;
      }
    };
    if (mix == 1 && pan == 0.5f) {
      sl.mod->process(L, R, n, h);
      ramp(L, R);
      return;
    }
    std::copy(L, L + n, wetL_), std::copy(R, R + n, wetR_);
    sl.mod->process(wetL_, wetR_, n, h);
    ramp(wetL_, wetR_);
    for (int i = 0; i < n; ++i) {
      sl.mixS -= (sl.mixS - mix) * 0.001f;
      sl.panS -= (sl.panS - pan) * 0.001f;
      const float wL = std::min(1.f, 2 * (1 - sl.panS)) * sl.mixS, wR = std::min(1.f, 2 * sl.panS) * sl.mixS;
      L[i] = L[i] * (1 - wL) + wetL_[i] * wL;
      R[i] = R[i] * (1 - wR) + wetR_[i] * wR;
    }
  }

  Slot slots_[kPkSlots];
  float wetL_[pk::kMaxBlock] = {}, wetR_[pk::kMaxBlock] = {};
};

}  // namespace

bool pocketModuleExact(int type, const float* params) { return pocket::moduleExact(type, params); }

std::unique_ptr<Effect> createPocketEffect(const std::string& type) {
  if (type == "gl.pocketrack") return std::make_unique<PocketRackFx>();
  return nullptr;
}

}  // namespace gl::packs::dynamics
