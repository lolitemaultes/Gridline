// Gridline — "synths" pack, J-33 group "pixel": Pixel Tone editor
// (gl.pixeltone): the picture (red = left, green = right, blue = pitch bend),
// span / length wheels, scale selector, switches and the row limit.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../flc_kit.h"
#include "pixel.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace pixel;

std::string pixelToneDefaultData();  // pixel_info.cpp

namespace {

const skin::Skin& pixelSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x26302E, 0x171E1C, 0x35423F, 0x6FD3C0, 0x40504C, 0xDDEBE8);
    s.select = skin::hex(0x6FD3C0);
    s.accent2 = skin::hex(0x6FD3C0);
    return s;
  }();
  return k;
}

}  // namespace

void drawPixelToneEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = pixelSkin();
  constexpr float kW = 620, kH = 250;
  ImGui::PushID("gl.pixeltone");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  skin::text(dl, F.p(12, 10), K.accent2C(), "PIXEL TONE", F.font(14));

  // Picture.
  static thread_local std::string cacheKey;
  static thread_local Picture pic;
  const std::string& data = st.data.empty() ? pixelToneDefaultData() : st.data;
  if (data != cacheKey) {
    cacheKey = data;
    if (!parsePicture(data, pic)) pic = Picture{};
  }
  const float x0 = 8, y0 = 32, pw = 604, ph = 120;
  ImVec2 p0 = F.p(x0, y0), p1 = F.p(x0 + pw, y0 + ph);
  skin::graphWell(dl, p0, p1, K);
  if (pic.w > 0 && pic.h > 0) {
    const int cols = std::min(pic.w, 302);
    const float cw = (p1.x - p0.x) / static_cast<float>(cols), rh = (p1.y - p0.y) / static_cast<float>(pic.h);
    for (int x = 0; x < cols; ++x) {
      const int sx = x * pic.w / cols;
      for (int y = 0; y < pic.h; ++y) {
        const uint8_t* px = &pic.rgb[(static_cast<size_t>(y) * static_cast<size_t>(pic.w) + static_cast<size_t>(sx)) * 3];
        if (px[0] == 0 && px[1] == 0 && px[2] == 0) continue;
        dl->AddRectFilled(ImVec2(p0.x + cw * x, p0.y + rh * y), ImVec2(p0.x + cw * (x + 1), p0.y + rh * (y + 1)),
                          IM_COL32(px[0], px[1], px[2], 255));
      }
    }
  }
  char info[64];
  std::snprintf(info, sizeof info, "%d x %d", pic.w, pic.h);
  flk::text(c, 520, 14, info);

  flk::section(c, 4, 158, 612, 88, "");
  flk::wheel(c, PxRange, 14, 168, 34, "Span");
  flk::wheel(c, PxLength, 62, 168, 34, "Length");
  {
    char b[32];
    const int n = columnSamples44(ival(st, PxLength));
    std::snprintf(b, sizeof b, "%.1f ms / col", n / 44.1);
    flk::lcd(c, 110, 172, 84, 16, b);
  }
  static const char* scales[3] = {"LOG", "LINEAR", "HARMONICS"};
  flk::sw(c, PxScale, 206, 168, 120, 54, scales, 3);
  flk::chk(c, PxUseBlue, 340, 170, 110, 14, "BLUE = PITCH");
  flk::chk(c, PxGrainy, 340, 190, 110, 14, "GRAINY");
  flk::chk(c, PxLoop, 460, 170, 80, 14, "LOOP");
  flk::chk(c, PxWiden, 460, 190, 80, 14, "WIDEN");
  flk::wheel(c, PxMaxHeight, 556, 168, 34, "Max rows");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
