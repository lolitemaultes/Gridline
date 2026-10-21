// Gridline — "spectral" pack: the reference vocoder's built-in carrier tones
// (J-65, see vxcarrier.h).
#include "vxcarrier.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include "../../core/flp/Flp.h"
#include "../../core/flp/FlpPresets.h"
#include "../synths/mfmref.h"

namespace gl::packs::spectral {

namespace mr = gl::packs::synths::mfmref;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string factoryOr(const std::string& factoryData) { return factoryData.empty() ? flp::defaultFactoryData() : factoryData; }

// The tone's patch from the installation's preset; `exact` when the reference
// model reproduces it.
bool loadTone(const std::string& tone, const std::string& factoryData, mr::Patch& out, bool* exact) {
  const std::string path = vxTonePath(tone, factoryData);
  if (path.empty()) return false;
  flp::PresetResult r;
  if (!flp::importFlPreset(path, r, factoryOr(factoryData)) || r.kind != flp::PresetResult::Plugin || r.state.type != "gl.matrixfm")
    return false;
  out = mr::patchFromState(r.state);
  if (exact) *exact = r.exact && mr::exactPatch(out, nullptr);
  return true;
}

// The vocoder's voice for the tone: host volume 0.5; the voice queries answer
// velocity 100 on the instrument's 128 scale (0.78125) whatever the volume.
constexpr double kVol = 0.5, kVelocity = 0.78125;

}  // namespace

std::string vxTonePath(const std::string& tone, const std::string& factoryData) {
  if (tone.empty()) return "";
  const std::string fd = factoryOr(factoryData);
  if (fd.empty()) return "";
  const fs::path dir = fs::path(fd) / "Plugins" / "Fruity" / "Effects" / "Vocodex" / "Synthesizer" / "Data" / "Presets";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "";
  const std::string want = lower(tone) + ".fst";
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (lower(it->path().filename().string()) == want && it->is_regular_file(ec)) return it->path().string();
  return "";
}

bool vxToneExact(const std::string& tone, const std::string& factoryData) {
  mr::Patch p;
  bool exact = false;
  return loadTone(tone, factoryData, p, &exact) && exact;
}

// ------------------------------------------------------------------ VxCarrier
struct VxCarrier::Impl {
  struct Pack {
    mr::EnginePtr engine;  // null: no tone
  };
  // UI thread
  std::string tone;
  int rate = 0;
  bool prepared = false;
  // hand-over
  std::atomic<Pack*> pending{nullptr};
  std::atomic<Pack*> retired[4] = {};
  Pack* leak = nullptr;
  // audio thread
  Pack* cur = nullptr;
  int keys[16] = {};
  int nk = 0;

  ~Impl() {
    delete pending.exchange(nullptr);
    for (auto& r : retired) delete r.exchange(nullptr);
    delete leak;
    delete cur;
  }
  void retire(Pack* p) {
    if (!p) return;
    for (auto& r : retired) {
      Pack* expect = nullptr;
      if (r.compare_exchange_strong(expect, p)) return;
    }
    leak = p;  // every slot taken (never seen): kept until the instance goes
  }
};

VxCarrier::VxCarrier() : d_(new Impl) {}
VxCarrier::~VxCarrier() = default;

void VxCarrier::prepare(const std::string& tone, int rate) {
  for (auto& r : d_->retired) delete r.exchange(nullptr);
  if (d_->prepared && tone == d_->tone && rate == d_->rate) return;
  d_->prepared = true;
  d_->tone = tone;
  d_->rate = rate;
  auto* pk = new Impl::Pack;
  mr::Patch patch;
  if (rate > 0 && loadTone(tone, "", patch, nullptr)) pk->engine = mr::makeEngine(patch, rate, 120.0);
  delete d_->pending.exchange(pk);
}

bool VxCarrier::render(const int* keys, int nk, double tempo, float* L, float* R, int n) {
  Impl& d = *d_;
  if (Impl::Pack* pk = d.pending.exchange(nullptr)) {
    d.retire(d.cur);
    d.cur = pk;
    d.nk = 0;  // a new tone: every key starts again
  }
  if (!d.cur || !d.cur->engine) return false;
  mr::Engine& e = *d.cur->engine;
  if (tempo > 1) mr::setTempo(e, tempo);
  nk = std::clamp(nk, 0, 16);
  // Keys that stopped, then keys that started (in the held order).
  for (int i = 0; i < d.nk; ++i)
    if (std::find(keys, keys + nk, d.keys[i]) == keys + nk) mr::noteOff(e, static_cast<uint32_t>(d.keys[i]) + 1);
  for (int i = 0; i < nk; ++i)
    if (std::find(d.keys, d.keys + d.nk, keys[i]) == d.keys + d.nk)
      mr::noteOn(e, static_cast<uint32_t>(keys[i]) + 1, (keys[i] - 60) * 100.0, kVol, 0.0, 0.0, 0.0, kVelocity, 0);
  std::copy(keys, keys + nk, d.keys);
  d.nk = nk;
  std::fill(L, L + n, 0.0f);
  std::fill(R, R + n, 0.0f);
  mr::render(e, L, R, n);
  return true;
}

void VxCarrier::reset() {
  Impl& d = *d_;
  if (d.cur && d.cur->engine) mr::kill(*d.cur->engine);
  d.nk = 0;
}

}  // namespace gl::packs::spectral
