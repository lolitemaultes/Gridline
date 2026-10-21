// Gridline — Q-13 interface icons: SVG rasterisation and the alpha atlas.
#include "IconAtlas.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "NanoSvg.h"

namespace gl::icons {

const char* findSvg(const std::string& name) {
  int n = 0;
  const SvgSource* s = embeddedSvgs(&n);
  for (int i = 0; i < n; ++i)
    if (name == s[i].name) return s[i].svg;
  return nullptr;
}

float strokePx(float logicalPx, float density) {
  return std::max(1.5f, logicalPx / 12.0f) * density;
}

namespace {

struct Parsed {
  NSVGimage* img = nullptr;
  std::vector<float> baseStroke;  // per shape, as parsed (24-unit grid)
  explicit Parsed(const char* text) {
    std::string copy(text);  // nsvgParse writes into its input
    img = nsvgParse(copy.data(), "px", 96.0f);
    if (!img) return;
    for (NSVGshape* s = img->shapes; s; s = s->next) baseStroke.push_back(s->strokeWidth);
  }
  ~Parsed() {
    if (img) nsvgDelete(img);
  }
  Parsed(const Parsed&) = delete;
  Parsed& operator=(const Parsed&) = delete;
  // Scales every stroke so a 2-unit source stroke draws `units` wide.
  void setStroke(float units) {
    size_t i = 0;
    for (NSVGshape* s = img->shapes; s; s = s->next, ++i) s->strokeWidth = baseStroke[i] * units / 2.0f;
  }
};

struct Rasterizer {
  NSVGrasterizer* r = nsvgCreateRasterizer();
  ~Rasterizer() { nsvgDeleteRasterizer(r); }
};

// Box filter: px x px coverage from the ss x supersampled `big`.
void downsample(const std::vector<uint8_t>& big, int px, int ss, std::vector<uint8_t>& out) {
  const int bw = px * ss, n = ss * ss;
  out.assign(static_cast<size_t>(px) * px, 0);
  for (int y = 0; y < px; ++y)
    for (int x = 0; x < px; ++x) {
      int sum = 0;
      for (int sy = 0; sy < ss; ++sy) {
        const uint8_t* row = &big[static_cast<size_t>(y * ss + sy) * bw + static_cast<size_t>(x * ss)];
        for (int sx = 0; sx < ss; ++sx) sum += row[sx];
      }
      out[static_cast<size_t>(y) * px + x] = static_cast<uint8_t>((sum + n / 2) / n);
    }
}

// Renders `img` (only shape `only`, or all shapes when -1) at px x px with
// `ss` x supersampling, shifted by (dx, dy) device px. `big` receives the
// supersampled coverage, `out` the box-filtered (exact area) coverage.
void render(NSVGrasterizer* r, NSVGimage* img, int px, int ss, int only, float dx, float dy, std::vector<uint8_t>& rgba,
            std::vector<uint8_t>& big, std::vector<uint8_t>& out) {
  const int bw = px * ss;
  rgba.assign(static_cast<size_t>(bw) * bw * 4, 0);
  std::vector<char> flags;
  int k = 0;
  for (NSVGshape* s = img->shapes; s; s = s->next, ++k) {
    flags.push_back(s->flags);
    if (only >= 0 && k != only) s->flags = static_cast<char>(s->flags & ~NSVG_FLAGS_VISIBLE);
  }
  const float viewW = img->width > 0 ? img->width : 24.0f;
  nsvgRasterize(r, img, dx * ss, dy * ss, static_cast<float>(bw) / viewW, rgba.data(), bw, bw, bw * 4);
  k = 0;
  for (NSVGshape* s = img->shapes; s; s = s->next, ++k) s->flags = flags[static_cast<size_t>(k)];
  big.resize(static_cast<size_t>(bw) * bw);
  for (size_t i = 0; i < big.size(); ++i) big[i] = rgba[i * 4 + 3];
  downsample(big, px, ss, out);
}

// Soft (partially covered) texels: the fewer, the crisper the edges.
long blur(const std::vector<uint8_t>& a) {
  long b = 0;
  for (uint8_t v : a) b += std::min<int>(v, 255 - v);
  return b;
}

}  // namespace

bool rasterize(const char* svgText, int px, float strokeDevicePx, std::vector<uint8_t>& alpha) {
  if (!svgText || px <= 0) return false;
  Parsed p(svgText);
  if (!p.img) return false;
  const float viewW = p.img->width > 0 ? p.img->width : 24.0f;
  p.setStroke(strokeDevicePx * viewW / static_cast<float>(px));
  static thread_local Rasterizer rast;
  const int ss = px <= 48 ? 4 : px <= 128 ? 2 : 1;
  std::vector<uint8_t> rgba, big, cand;
  // Grid fitting, in two passes, each keeping the shift with the fewest soft
  // texels (vertical first, then horizontal; a shift is kept only when it is
  // clearly crisper). 1: the whole glyph by up to 3/8 px. 2: each shape (path)
  // by up to a further 1/4 px, so e.g. every bar of a menu icon lands on whole
  // pixels; shapes keep their places to within a quarter pixel. Curves are
  // never bent: shapes only move.
  static const float kShift[] = {0.0f, 0.125f, -0.125f, 0.25f, -0.25f, 0.375f, -0.375f};
  auto search = [&](int only, float range, float& bx, float& by, std::vector<uint8_t>& bestBig) {
    std::vector<uint8_t> a, b;
    render(rast.r, p.img, px, ss, only, bx, by, rgba, bestBig, a);
    long best = blur(a);
    const float ox = bx, oy = by;
    for (int axis = 0; axis < 2; ++axis)
      for (float s : kShift) {
        if (s == 0.0f || std::fabs(s) > range + 1e-4f) continue;
        const float dx = axis == 0 ? bx : ox + s, dy = axis == 0 ? oy + s : by;
        render(rast.r, p.img, px, ss, only, dx, dy, rgba, b, cand);
        const long c = blur(cand);
        if (c * 100 < best * 97) {
          best = c;
          bestBig.swap(b);
          if (axis == 0) by = dy;
          else bx = dx;
        }
      }
  };
  float gx = 0, gy = 0;
  std::vector<uint8_t> whole;
  search(-1, 0.375f, gx, gy, whole);
  int shapes = 0;
  for (NSVGshape* s = p.img->shapes; s; s = s->next) shapes += (s->flags & NSVG_FLAGS_VISIBLE) != 0;
  if (shapes > 1) {
    // Composite the fitted shapes in paint order (source-over, as the SVG draws them).
    std::vector<float> acc(whole.size(), 0.0f);
    int k = 0;
    for (NSVGshape* s = p.img->shapes; s; s = s->next, ++k) {
      if (!(s->flags & NSVG_FLAGS_VISIBLE)) continue;
      float bx = gx, by = gy;
      search(k, 0.25f, bx, by, big);
      for (size_t i = 0; i < acc.size(); ++i) {
        const float a = static_cast<float>(big[i]) / 255.0f;
        acc[i] = acc[i] + a * (1.0f - acc[i]);
      }
    }
    for (size_t i = 0; i < whole.size(); ++i) whole[i] = static_cast<uint8_t>(std::lround(std::min(1.0f, acc[i]) * 255.0f));
  }
  downsample(whole, px, ss, alpha);
  return true;
}

// ---- Atlas ----

Atlas::Atlas() {
  int n = 0;
  const SvgSource* s = embeddedSvgs(&n);
  for (int i = 0; i < n; ++i) index_[s[i].name] = i;
}
Atlas::~Atlas() = default;

int Atlas::indexOf(const std::string& name) const {
  auto it = index_.find(name);
  return it == index_.end() ? -1 : it->second;
}

int Atlas::devicePx(float logicalPx) const {
  return std::max(4, static_cast<int>(std::lround(logicalPx * density_)));
}

void Atlas::clear() {
  rects_.clear();
  for (Page& p : pages_) {
    std::fill(p.alpha.begin(), p.alpha.end(), static_cast<uint8_t>(0));
    p.shelfY = p.shelfH = p.cursorX = 0;
    p.dirtyX0 = p.dirtyY0 = kPage;
    p.dirtyX1 = p.dirtyY1 = 0;
    p.cleared = true;
  }
  ++resets_;
}

bool Atlas::setDensity(float d) {
  if (!(d > 0.0f)) d = 1.0f;
  if (std::fabs(d - density_) < 1e-4f) return false;
  density_ = d;
  clear();
  return true;
}

bool Atlas::place(int w, int h, Rect& out) {
  if (w > kPage || h > kPage) return false;
  auto fit = [&](Page& p, int pi) {
    if (p.cursorX + w > kPage) {  // next shelf
      p.shelfY += p.shelfH;
      p.shelfH = 0;
      p.cursorX = 0;
    }
    if (p.shelfY + h > kPage) return false;
    out.page = pi;
    out.x = p.cursorX;
    out.y = p.shelfY;
    p.cursorX += w;
    p.shelfH = std::max(p.shelfH, h);
    return true;
  };
  for (size_t i = 0; i < pages_.size(); ++i)
    if (fit(pages_[i], static_cast<int>(i))) return true;
  if (pages_.size() >= 16) return false;  // 16 MB of RGBA: never reached in practice
  pages_.emplace_back();
  pages_.back().alpha.assign(static_cast<size_t>(kPage) * kPage, 0);
  return fit(pages_.back(), static_cast<int>(pages_.size() - 1));
}

const Atlas::Rect* Atlas::get(const std::string& name, float logicalPx) { return get(indexOf(name), logicalPx); }

const Atlas::Rect* Atlas::get(int svgIndex, float logicalPx) {
  int n = 0;
  const SvgSource* src = embeddedSvgs(&n);
  if (svgIndex < 0 || svgIndex >= n) return nullptr;
  const int px = std::min(devicePx(logicalPx), kPage - 2 * kPad);
  const uint64_t key = (static_cast<uint64_t>(svgIndex) << 32) | static_cast<uint32_t>(px);
  auto it = rects_.find(key);
  if (it != rects_.end()) return it->second.page >= 0 ? &it->second : nullptr;
  Rect r;
  std::vector<uint8_t> a;
  ++rasterCount_;
  if (rasterize(src[svgIndex].svg, px, strokePx(static_cast<float>(px) / density_, density_), a) &&
      place(px + 2 * kPad, px + 2 * kPad, r)) {
    r.x += kPad;
    r.y += kPad;
    r.size = px;
    Page& pg = pages_[static_cast<size_t>(r.page)];
    for (int y = 0; y < px; ++y)
      std::memcpy(&pg.alpha[static_cast<size_t>(r.y + y) * kPage + static_cast<size_t>(r.x)], &a[static_cast<size_t>(y) * px],
                  static_cast<size_t>(px));
    pg.dirtyX0 = std::min(pg.dirtyX0, r.x - kPad);
    pg.dirtyY0 = std::min(pg.dirtyY0, r.y - kPad);
    pg.dirtyX1 = std::max(pg.dirtyX1, r.x + px + kPad);
    pg.dirtyY1 = std::max(pg.dirtyY1, r.y + px + kPad);
  } else {
    r.page = -1;  // remembered as missing: never retried every frame
  }
  auto& slot = rects_[key] = r;
  return slot.page >= 0 ? &slot : nullptr;
}

}  // namespace gl::icons
