// Gridline — "dynamics" pack: runtimes (dispatch; the effects live in
// master_dsp.cpp, dist_dsp.cpp and rack_dsp.cpp).
#include "../../core/ProjectInterop.h"
#include "../Packs.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
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

namespace {

// The pedal board's cabinet files, in their order: the folder's .wav files by name (a shared
// project's folder: its files under their content names). Full paths.
std::vector<std::string> boardFiles(const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> wavs;  // (name, path)
  if (isFolderManifest(dir)) {
    for (auto& f : folderRefFiles(dir))
      if (std::filesystem::u8path(f.second).extension() == ".wav") wavs.emplace_back(f.second, f.first);
  } else {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec))
      if (e.path().extension() == ".wav") wavs.emplace_back(e.path().filename().u8string(), dir + "/" + e.path().filename().u8string());
  }
  std::sort(wavs.begin(), wavs.end());
  std::vector<std::string> out;
  for (auto& w : wavs) out.push_back(std::move(w.second));
  return out;
}

}  // namespace

std::string cab::boardFile(const std::string& dir, int n) {
  if (n < 1) return {};
  std::vector<std::string> files;
  if (mustNotReadNow(dir)) {
    // FRZ-1118 (0.7.8): a folder on a web or network drive is listed in the background, never by a build on
    // the interface thread (a mount that stops answering held it in the kernel); the build after
    // the listing has it.
    static std::mutex mu;
    static auto& listed = *new std::map<std::string, std::vector<std::string>>;  // (never destroyed: a reader may end during exit)
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (auto it = listed.find(dir); it != listed.end()) files = it->second, have = true;
    }
    if (!have) {
      readInBackground("cab folder\n" + dir, [dir] {
        if (const char* d = std::getenv("GRIDLINE_DEV_SLOW_LIST_MS")) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(d)));  // tests
        std::vector<std::string> found = boardFiles(dir);
        std::lock_guard<std::mutex> lk(mu);
        listed[dir] = std::move(found);
      });
      return {};
    }
  } else {
    files = boardFiles(dir);
  }
  return n <= static_cast<int>(files.size()) ? files[static_cast<size_t>(n - 1)] : std::string();
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
    std::string path = cab::fileFor(dir, ir);
    if (isFolderManifest(dir)) {
      const std::string wanted = std::filesystem::u8path(path).filename().u8string();
      path.clear();
      for (const auto& f : folderRefFiles(dir))
        if (f.second == wanted) path = f.first;
      if (path.empty()) continue;
    }
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
