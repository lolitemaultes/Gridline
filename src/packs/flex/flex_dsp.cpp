// Gridline — "flex" pack (J-53): the stand-in player runtime. One instance of
// each stand-in engine (Analog, Keys, FM Synth, Drum Model, Plucker) plus a
// reverb and a delay, created on the UI thread. prepareAux (UI thread)
// resolves the category and builds the engine's template state; each block
// copies it into a pre-sized work state and applies the live FLEX controls
// (automation of macros, cutoff, … follows), without allocating.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "../../core/FlexStandin.h"
#include "../../core/PluginParams.h"
#include "../../engine/Effects.h"
#include "../../engine/Program.h"
#include "../Packs.h"

namespace gl::packs::flexpack {

namespace {

using namespace gl::flex;

// Prepared on the UI thread, read-only on the audio thread.
struct FlexAux final : AuxData {
  int cat = Neutral;
  int engine = EAnalog;
  bool kit = false;  // whole drum kit across the keys (GM-style map)
  const PluginInfo* info = nullptr;
  PluginState base;  // template state of the engine
  ChannelAux engineAux;
  PluginState rev, dly;  // effect states (wet / time set per block)
};

class FlexGen final : public Generator {
 public:
  FlexGen() {
    for (int e = 0; e < EngineCount; ++e) {
      eng_[static_cast<size_t>(e)] = gl::createGenerator(engineType(e));
      work_[static_cast<size_t>(e)] = defaultPluginState(engineType(e));
    }
    rev_ = gl::createEffect("gl.reverb");
    dly_ = gl::createEffect("gl.delay");
    revWork_ = defaultPluginState("gl.reverb");
    dlyWork_ = defaultPluginState("gl.delay");
  }

  bool active() const override {
    for (const auto& g : eng_)
      if (g && g->active()) return true;
    return tail_ > 0;
  }

 protected:
  bool wholeBlock() const override { return true; }
  bool handlesNotePan() const override { return true; }

  void onNoteOn(uint32_t id, double pitch, float velocity) override { onNoteOnEx(id, pitch, velocity, NoteProps{}); }
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override {
    if (!aux_) return;
    Ev e;
    e.kind = Ev::On, e.id = id, e.pitch = mapPitch(pitch), e.vel = velocity, e.props = props, e.offset = eventOffset();
    push(e);
  }
  void onNoteOff(uint32_t id) override {
    Ev e;
    e.kind = Ev::Off, e.id = id, e.offset = eventOffset();
    push(e);
  }
  void onCut(uint32_t id) override {
    Ev e;
    e.kind = Ev::Cut, e.id = id, e.offset = eventOffset();
    push(e);
  }
  void onAllOff() override {
    Ev e;
    e.kind = Ev::AllOff, e.offset = eventOffset();
    push(e);
  }
  void onKill() override {
    for (auto& g : eng_)
      if (g) g->kill();
    if (rev_) rev_->reset();
    if (dly_) dly_->reset();
    count_ = 0, tail_ = 0;
  }

  void configure(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx) override {
    ctx_ = ctx;
    for (int k = 0; k < PCount; ++k) p_[k] = k < static_cast<int>(state.params.size()) ? state.params[static_cast<size_t>(k)] : 0.5f;
    const auto* fa = dynamic_cast<const FlexAux*>(aux.custom.get());
    if (fa && (fa->engine < 0 || fa->engine >= EngineCount || !eng_[static_cast<size_t>(fa->engine)])) fa = nullptr;
    if (fa != aux_ && aux_ && (!fa || fa->engine != aux_->engine) && eng_[static_cast<size_t>(aux_->engine)])
      eng_[static_cast<size_t>(aux_->engine)]->kill();  // the stand-in changed: the old engine stops
    aux_ = fa;
    if (!aux_) return;
    PluginState& w = work_[static_cast<size_t>(aux_->engine)];
    if (w.params.size() == aux_->base.params.size()) {
      std::copy(aux_->base.params.begin(), aux_->base.params.end(), w.params.begin());
      applyControls(aux_->cat, p_, *aux_->info, w);
    }
    gain_ = outputGain(aux_->cat, p_);
    pitch_ = pitchOffset(aux_->cat, p_);
    if (aux_->cat == Drums && !aux_->kit) pitch_ = (p_[Pitch] - 0.5) * 48.0;  // one pitched piece: FLEX pitch moves it
    clip_ = softClip(p_);
    fx_ = fxSettings(aux_->cat, p_);
    using namespace pp;
    auto put = [](PluginState& s, int i, double v) {
      if (static_cast<size_t>(i) < s.params.size()) s.params[static_cast<size_t>(i)] = static_cast<float>(v);
    };
    if (revWork_.params.size() == aux_->rev.params.size()) {
      std::copy(aux_->rev.params.begin(), aux_->rev.params.end(), revWork_.params.begin());
      put(revWork_, RvbWet, fx_.revWet), put(revWork_, RvbSize, fx_.revSize), put(revWork_, RvbDecayTime, fx_.revDecay);
      put(revWork_, RvbHighCut, fx_.revHighCut);
    }
    if (dlyWork_.params.size() == aux_->dly.params.size()) {
      std::copy(aux_->dly.params.begin(), aux_->dly.params.end(), dlyWork_.params.begin());
      put(dlyWork_, pp::DlyWet, fx_.dlyWet), put(dlyWork_, pp::DlyFeedback, fx_.dlyFb), put(dlyWork_, pp::DlyTime, fx_.dlyDiv);
    }
  }

  void render(float* L, float* R, int n) override {
    if (!aux_) {
      count_ = 0;
      return;
    }
    Generator* g = eng_[static_cast<size_t>(aux_->engine)].get();
    const PluginState& w = work_[static_cast<size_t>(aux_->engine)];
    size_t ei = 0;
    for (int c0 = 0; c0 < n; c0 += kChunk) {
      const int len = std::min(kChunk, n - c0);
      for (; ei < count_ && evs_[ei].offset < c0 + len; ++ei) {
        const Ev& e = evs_[ei];
        const int off = std::clamp(e.offset - c0, 0, len - 1);
        switch (e.kind) {
          case Ev::On: g->noteOn(e.id, e.pitch, e.vel, off, e.props); break;
          case Ev::Off: g->noteOff(e.id, off); break;
          case Ev::Cut: g->cut(e.id, off); break;
          case Ev::AllOff: g->allNotesOff(off); break;
        }
      }
      std::fill(bufL_.begin(), bufL_.begin() + len, 0.0f);
      std::fill(bufR_.begin(), bufR_.begin() + len, 0.0f);
      const bool sounding = g->active() || ei > 0;
      g->process(w, aux_->engineAux, ctx_, bufL_.data(), bufR_.data(), len);
      if (sounding || g->active()) tail_ = static_cast<int64_t>(ctx_.sampleRate * (fx_.revOn ? fx_.revDecay + 0.5 : 0.0) +
                                                                (fx_.dlyOn ? ctx_.sampleRate * 4.0 : 0.0));
      if (fx_.dlyOn && dly_) dly_->process(dlyWork_, ctx_, bufL_.data(), bufR_.data(), len);
      if (fx_.revOn && rev_) rev_->process(revWork_, ctx_, bufL_.data(), bufR_.data(), len);
      for (int i = 0; i < len; ++i) {
        float l = bufL_[static_cast<size_t>(i)] * gain_, r = bufR_[static_cast<size_t>(i)] * gain_;
        if (clip_) l = soft(l), r = soft(r);
        L[c0 + i] += l;
        R[c0 + i] += r;
      }
      if (!g->active()) tail_ = std::max<int64_t>(0, tail_ - len);
    }
    count_ = 0;
  }

 private:
  static constexpr int kChunk = 1024;
  struct Ev {
    enum Kind { On, Off, Cut, AllOff } kind = On;
    uint32_t id = 0;
    double pitch = 0;
    float vel = 0;
    NoteProps props{};
    int offset = 0;
  };
  void push(const Ev& e) {
    if (count_ < evs_.size()) evs_[count_++] = e;
  }
  double mapPitch(double pitch) const {
    if (aux_ && aux_->kit) return drumNote(static_cast<int>(std::lround(pitch)));
    return pitch + pitch_;
  }
  static float soft(float x) {  // gentle limiter: transparent below ~-6 dBFS
    const float a = std::fabs(x);
    if (a < 0.5f) return x;
    const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) * 2.0f);
    return x < 0 ? -y : y;
  }

  std::array<std::unique_ptr<Generator>, EngineCount> eng_;
  std::array<PluginState, EngineCount> work_;
  std::unique_ptr<Effect> rev_, dly_;
  PluginState revWork_, dlyWork_;
  std::array<float, kChunk> bufL_{}, bufR_{};
  std::array<Ev, 512> evs_{};
  size_t count_ = 0;
  const FlexAux* aux_ = nullptr;
  ProcessContext ctx_{};
  float p_[PCount] = {};
  float gain_ = 1;
  double pitch_ = 0;
  bool clip_ = false;
  FxSettings fx_{};
  int64_t tail_ = 0;
};

}  // namespace

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == kType) return std::make_unique<FlexGen>();
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != kType) return false;
  auto fa = std::make_shared<FlexAux>();
  fa->cat = categoryOf(state);
  fa->engine = engineOf(fa->cat);
  const Identity id = identity(state.data);
  const int piece = fa->cat == Drums ? drumPiece(id.preset) : -1;
  fa->kit = fa->cat == Drums && piece < 0;
  fa->base = templateState(fa->cat, piece);
  normalizePluginState(fa->base);
  fa->info = findPlugin(fa->base.type);
  if (!fa->info) return true;  // (engine missing: silent)
  prepareChannelAux(fa->base, fa->engineAux);
  {
    using namespace pp;
    fa->rev = defaultPluginState("gl.reverb");
    auto put = [](PluginState& s, int i, double v) {
      if (static_cast<size_t>(i) < s.params.size()) s.params[static_cast<size_t>(i)] = static_cast<float>(v);
    };
    put(fa->rev, RvbDry, 1), put(fa->rev, RvbPreDelay, 12), put(fa->rev, RvbLowCut, 120);
    fa->dly = defaultPluginState("gl.delay");
    put(fa->dly, DlyModel, 0), put(fa->dly, DlySync, 1), put(fa->dly, DlyDry, 1), put(fa->dly, DlyMode, 1);
    put(fa->dly, DlyCutoff, 6000);
  }
  aux.custom = fa;
  return true;
}

}  // namespace gl::packs::flexpack
