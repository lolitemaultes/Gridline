#include "AppIconImage.h"

#include <cstring>

#include "../../third_party/stb/stb_image.h"

namespace gl::appicon {

Image decode(int size) {
  Image out;
  int n = 0;
  const EmbeddedPng* t = embeddedPngs(&n);
  const EmbeddedPng* pick = nullptr;    // the smallest one at least `size`
  const EmbeddedPng* largest = nullptr;
  for (int i = 0; i < n; ++i) {
    const EmbeddedPng& e = t[i];
    if (e.size >= size && (!pick || e.size < pick->size)) pick = &e;
    if (!largest || e.size > largest->size) largest = &e;
  }
  if (!pick) pick = largest;
  if (!pick) return out;
  int w = 0, h = 0, comp = 0;
  unsigned char* px = stbi_load_from_memory(pick->data, static_cast<int>(pick->bytes), &w, &h, &comp, 4);
  if (!px) return out;
  out.w = w;
  out.h = h;
  out.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  std::memcpy(out.rgba.data(), px, out.rgba.size());
  stbi_image_free(px);
  return out;
}

}  // namespace gl::appicon
