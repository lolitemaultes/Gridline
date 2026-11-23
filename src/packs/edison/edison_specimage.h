// Gridline: "edison" pack: the spectrogram image, shared by Loupe's spectrogram view and the audio
// clip editor's spectrum (edison_specimage_ui.cpp). An image (analyseSpecView, edison.h) is made on
// a worker for the pixels it is shown on and becomes an OpenGL texture once it is ready; levels are
// coloured through a lightness-ordered ramp interpolated in a perceptual colour space (1024 steps,
// no banding). Nothing is made again until what is asked for changes.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include "../../../third_party/imgui/imgui.h"
#include "edison.h"

namespace gl {
class App;
}

namespace gl::packs::edison {

// Frequencies run from kSpecMinHz at the bottom to the Nyquist frequency at the top, on a log axis.
inline constexpr double kSpecMinHz = 30.0;
// Loupe's settings, which the clip editor's spectrum follows too: the level range shown (dB below
// 0 dBFS) and the resolution (0 sharper in time, 1 balanced, 2 sharper in frequency).
int specRange(App& a);
int specResolution(App& a);

// One spectrogram image: made on a worker, shown as a texture.
struct SpecLayer {
  SpecView img;                               // what the texture shows
  std::future<SpecView> job;                  // the image being made
  std::shared_ptr<std::atomic<bool>> cancel;  // stops `job` early
  SpecViewRequest asked;                      // what `job` (or `img`) was made for
  uint64_t askedSerial = ~0ull, imgSerial = ~0ull;  // the audio versions
  unsigned tex = 0;
  uint64_t texEpoch = 0;  // K-47: the OpenGL context it was made in
  int texW = 0, texH = 0;
  int texLook = -1;  // the level range (and ramp) the texture was coloured for
  bool dirty = false;
  int made = 0, uploads = 0;  // images started and textures written (dev checks: nothing while idle)
  SpecLayer() = default;
  SpecLayer(const SpecLayer&) = delete;
  SpecLayer& operator=(const SpecLayer&) = delete;
  ~SpecLayer() {
    if (cancel) cancel->store(true);  // (the future's destructor then waits for a short stop)
  }
};

// The colour ramps (kSpecLut steps, from the floor of the level range to 0 dB).
inline constexpr int kSpecLut = 1024;
using SpecRamp = std::array<ImU32, kSpecLut>;
// stops: (0..1, 0xRRGGBB), the first at 0 and the last at 1. fadeTo > 0: the floor is clear and
// the colours grow opaque by `fadeTo` (an image drawn over its own background).
SpecRamp makeSpecRamp(const std::pair<double, uint32_t>* stops, size_t n, double fadeTo = 0);
const SpecRamp& loupeSpecRamp();  // Loupe's own: slate, green, amber, white over its lanes

// Makes the image's texture again when the image, the range or the ramp changed (`look` names
// the ramp: a texture coloured for another look is coloured again).
void specUpload(SpecLayer& L, int range, const SpecRamp& ramp, int look = 0);

// Makes the image for a request (on a worker; `cancel` stops it early, the result then empty).
using SpecMaker = std::function<SpecView(const SpecViewRequest&, const std::atomic<bool>*)>;
// Takes a finished image; starts one for `want` when the layer has none for it and `start`. An
// image that is out of date while it is made is stopped (the next call asks again). The worker
// wakes the interface when it is done.
void specStep(SpecLayer& L, const SpecViewRequest& want, uint64_t serial, bool start, const SpecMaker& make, const char* threadName);

}  // namespace gl::packs::edison
