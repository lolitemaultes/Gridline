// Gridline — "dynamics" pack: runtimes (dispatch; the effects live in
// master_dsp.cpp, dist_dsp.cpp and rack_dsp.cpp).
#include "../Packs.h"
#include <filesystem>
#include <map>
#include <mutex>
#include "../../engine/Resample.h"
#include "../../engine/Sample.h"
#include "cabinet_ir.h"
#include "dynamics.h"

namespace gl::packs::dynamics {

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  (void)type;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type.compare(0, 3, "gl.") != 0) return nullptr;
  if (auto fx = createMasterEffect(type)) return fx;
  if (auto fx = createDistEffect(type)) return fx;
  if (auto fx = createRackEffect(type)) return fx;
  if (auto fx = createPocketEffect(type)) return fx;
  return nullptr;
}

std::string cab::boardFile(const std::string& dir, int n) {
  if (n < 1) return {};
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().extension() == ".wav") names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  return n <= static_cast<int>(names.size()) ? dir + "/" + names[static_cast<size_t>(n - 1)] : std::string();
}

std::atomic<double>& cabRate() {
  static std::atomic<double> r{0.0};
  return r;
}

// J-42: module-rack speaker impulses (FL Studio's files, read in place; cached per file and rate).
bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.amprack") return false;
  auto pv = [&](int i) { return static_cast<size_t>(i) < state.params.size() ? static_cast<int>(std::lround(state.params[static_cast<size_t>(i)])) : 0; };
  const std::string dir = cab::dirOf(state.data);
  const int model = pv(RkModel);
  if ((model != 1 && model != 2) || dir.empty()) return false;
  const double r0 = cabRate().load(std::memory_order_relaxed), rate = r0 > 0 ? r0 : 44100.0;
  static std::mutex mu;
  static std::map<std::pair<std::string, double>, std::shared_ptr<const cab::Kernel>> cache;
  auto a = std::make_shared<cab::Aux>();
  bool any = false;
  if (model == 2) {  // pedal board cabinet (value 11)
    const float v = static_cast<size_t>(PbValue0 + 11) < state.params.size() ? state.params[static_cast<size_t>(PbValue0 + 11)] : 0.f;
    const std::string path = cab::boardFile(dir, static_cast<int>(std::trunc(v)));
    if (path.empty()) return false;
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = cache[{path, rate}];
    if (!slot) {
      SamplePtr smp = loadSample(path);
      if (!smp || smp->frames() < 2) return false;
      std::vector<float> ch[2] = {cab::boardResample(smp->left, smp->sampleRate, rate),
                                  cab::boardResample(std::vector<float>(smp->right), smp->sampleRate, rate)};
      slot = cab::build(ch, rate, 1.0);
    }
    a->k[0] = slot, a->rate = rate;
    aux.custom = a;
    return true;
  }
  for (int k = 0; k < kDrModules; ++k) {
    const int ir = pv(drParam(k, DmImpulse));
    if (pv(drParam(k, DmKind)) != 3 || !cab::valid(ir)) continue;
    const std::string path = cab::fileFor(dir, ir);
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = cache[{path, rate}];
    if (!slot) {
      SamplePtr smp = loadSample(path);
      if (!smp || smp->frames() == 0) continue;
      std::vector<float> ch[2] = {smp->left, std::vector<float>(smp->right)};
      if (smp->sampleRate != rate)
        for (auto& c : ch) c = resampleChannel(c.data(), c.size(), smp->sampleRate, rate, 32);
      slot = cab::build(ch, rate, cab::entry(ir).gain);
    }
    a->k[k] = slot, any = true;
  }
  if (!any) return false;
  a->rate = rate;
  aux.custom = a;
  return true;
}

}  // namespace gl::packs::dynamics
