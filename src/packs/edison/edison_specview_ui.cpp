// Gridline — "edison" pack: Loupe's spectrogram view. Images come from analyseSpecView on a
// worker, one column per screen pixel and one row per pixel, and become a texture when they
// are ready: the whole file once per edit (the stand-in), and the frames in view once the view
// has rested for a moment (zooming and scrolling stretch the last image until then). Levels
// are coloured through a lightness-ordered ramp in Loupe's own colours (slate, green, amber,
// white), interpolated in a perceptual colour space (the image and its texture: edison_specimage).
#include <algorithm>
#include <cmath>
#include "../../app/DevSignal.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace {

// Draws the part of a layer's image inside x0..x1 of the lanes.
void drawLayer(ImDrawList* dl, const SpecLayer& L, ImVec2 p0, ImVec2 p1, double start, double len) {
  if (!L.tex || L.img.empty()) return;
  const double f0 = static_cast<double>(L.img.req.from), f1 = static_cast<double>(L.img.req.to);
  const double W = p1.x - p0.x;
  auto xOf = [&](double f) { return p0.x + (f - start) / len * W; };
  const double xa = std::max<double>(p0.x, xOf(f0)), xb = std::min<double>(p1.x, xOf(f1));
  if (xb <= xa) return;
  auto uOf = [&](double x) { return static_cast<float>(((start + (x - p0.x) / W * len) - f0) / (f1 - f0)); };
  dl->AddImage(ImTextureRef(static_cast<ImTextureID>(L.tex)), ImVec2(static_cast<float>(xa), p0.y), ImVec2(static_cast<float>(xb), p1.y),
               ImVec2(uOf(xa), 0), ImVec2(uOf(xb), 1));
}

bool levelIn(const SpecLayer& L, double frame, double hz, float& db) {
  const SpecView& v = L.img;
  if (v.empty() || frame < static_cast<double>(v.req.from) || frame >= static_cast<double>(v.req.to)) return false;
  const double ny = v.rate * 0.5, top = v.req.maxHz > 0 ? v.req.maxHz : ny;
  if (hz < v.req.minHz || hz > top) return false;
  const int c = std::clamp(static_cast<int>((frame - static_cast<double>(v.req.from)) / static_cast<double>(v.req.to - v.req.from) * v.req.cols), 0, v.req.cols - 1);
  const double t = std::log(hz / v.req.minHz) / std::log(top / v.req.minHz);
  const int r = std::clamp(static_cast<int>((1.0 - t) * v.req.rows), 0, v.req.rows - 1);
  db = v.at(c, r);
  return true;
}

}  // namespace

int specRange(App& a) {
  const int v = static_cast<int>(std::lround(a.settings.getNumber("loupe.specRange")));
  return v == 60 || v == 80 || v == 120 ? v : 100;
}
int specResolution(App& a) { return std::clamp(static_cast<int>(std::lround(a.settings.getNumber("loupe.specResolution"))), 0, 2); }
void setSpecRange(App& a, int db) { a.settings.set("loupe.specRange", static_cast<double>(db)); }
void setSpecResolution(App& a, int r) { a.settings.set("loupe.specResolution", static_cast<double>(r)); }

void drawSpecView(App& a, Session& s, ImDrawList* dl, ImVec2 p0, ImVec2 p1, double start, double len) {
  const uint64_t serial = s.hist.serial();
  const AudioPtr audio = s.cur().audio;
  const int64_t n = s.frames();
  const int res = specResolution(a), range = specRange(a);
  const float fb = std::max(1.f, ImGui::GetIO().DisplayFramebufferScale.y);
  const int cols = std::clamp(static_cast<int>(std::lround((p1.x - p0.x) * fb)), 0, 8192);
  const int rows = std::clamp(static_cast<int>(std::lround((p1.y - p0.y) * fb)), 0, 4096);
  if (n <= 0 || cols < 2 || rows < 2) return;

  // The whole file, at most 2048 columns: made first after an edit, then whenever the view rests.
  SpecViewRequest whole;
  whole.from = 0, whole.to = n, whole.cols = static_cast<int>(std::min<int64_t>(2048, std::max<int64_t>(2, n / 8)));
  whole.rows = std::min(rows, 768), whole.minHz = kSpecMinHz, whole.resolution = res;
  // The view: one column per pixel (frames rounded out to whole frames).
  SpecViewRequest want;
  want.from = static_cast<int64_t>(std::floor(start)), want.to = std::max(want.from + 1, static_cast<int64_t>(std::ceil(start + len)));
  want.cols = cols, want.rows = rows, want.minHz = kSpecMinHz, want.resolution = res;
  const double now = ImGui::GetTime();
  if (want != s.specWant) s.specWant = want, s.specWantSince = now;
  const bool rested = now - s.specWantSince >= 0.12 || s.specView.img.empty();
  const bool wholeView = want.from <= 0 && want.to >= n;
  const SpecMaker make = [audio](const SpecViewRequest& r, const std::atomic<bool>* cancel) { return analyseSpecView(*audio, r, cancel); };
  specStep(s.specBase, whole, serial, audio && (!wholeView || s.specBase.imgSerial != serial), make, "gl-loupe-spec");
  specStep(s.specView, want, serial, audio && rested && (s.specBase.imgSerial == serial || !s.specBase.job.valid()), make, "gl-loupe-spec");
  const bool viewWasDirty = s.specView.dirty;
  specUpload(s.specBase, range, loupeSpecRamp());
  specUpload(s.specView, range, loupeSpecRamp());
  if (viewWasDirty && s.specView.tex && s.specView.imgSerial == serial && s.specView.img.req == want) devsig::reach("loupe-spectrogram");  // dev checks

  dl->PushClipRect(p0, p1, true);
  // The stand-in first (unless the view's image covers everything), then the view's image.
  const bool viewCovers = s.specView.tex && s.specView.imgSerial == serial && s.specView.img.req.from <= want.from && s.specView.img.req.to >= want.to;
  if (!viewCovers || s.specView.imgSerial != serial) drawLayer(dl, s.specBase, p0, p1, start, len);
  if (s.specView.imgSerial == serial || s.specBase.tex == 0) drawLayer(dl, s.specView, p0, p1, start, len);
  dl->PopClipRect();
}

bool specLevelAt(const Session& s, double frame, double hz, float& db) {
  const uint64_t serial = s.hist.serial();
  if (s.specView.imgSerial == serial && levelIn(s.specView, frame, hz, db)) return true;
  return s.specBase.imgSerial == serial && levelIn(s.specBase, frame, hz, db);
}

}  // namespace gl::packs::edison
