// Gridline — J-130 plugin wordmarks (see Wordmark.h).
#include "Wordmark.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include "../../../third_party/imgui/imgui_internal.h"
#include "NanoSvg.h"

namespace gl::icons::wordmark {

namespace {

enum Role { kMark, kCut, kWord, kDim };

Role roleOf(const char* id) {
  if (std::strncmp(id, "mark", 4) == 0) return kMark;
  if (std::strncmp(id, "cut", 3) == 0) return kCut;
  if (std::strncmp(id, "dim", 3) == 0) return kDim;
  return kWord;
}

const std::unordered_map<std::string, int>& index() {
  static const std::unordered_map<std::string, int> m = [] {
    std::unordered_map<std::string, int> r;
    int n = 0;
    const Source* s = embedded(&n);
    for (int i = 0; i < n; ++i) r.emplace(s[i].key, i);
    return r;
  }();
  return m;
}

int indexOf(const std::string& key) {
  const auto& m = index();
  auto it = m.find(key);
  return it == m.end() ? -1 : it->second;
}

// Coverage of the shapes of `role` (all other shapes hidden), kSs x supersampled
// then box-filtered to w x h.
void layer(NSVGrasterizer* r, NSVGimage* img, Role role, float scale, int w, int h, std::vector<uint8_t>& out) {
  constexpr int kSs = 3;
  const int bw = w * kSs, bh = h * kSs;
  std::vector<unsigned char> rgba(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4, 0);
  std::vector<char> flags;
  bool any = false;
  for (NSVGshape* s = img->shapes; s; s = s->next) {
    flags.push_back(s->flags);
    if (roleOf(s->id) != role) s->flags = static_cast<char>(s->flags & ~NSVG_FLAGS_VISIBLE);
    else any = true;
  }
  out.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  if (any) nsvgRasterize(r, img, 0, 0, scale * kSs, rgba.data(), bw, bh, bw * 4);
  size_t k = 0;
  for (NSVGshape* s = img->shapes; s; s = s->next) s->flags = flags[k++];
  if (!any) return;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int sum = 0;
      for (int sy = 0; sy < kSs; ++sy) {
        const unsigned char* row = &rgba[(static_cast<size_t>(y * kSs + sy) * static_cast<size_t>(bw) + static_cast<size_t>(x * kSs)) * 4 + 3];
        for (int sx = 0; sx < kSs; ++sx) sum += row[sx * 4];
      }
      out[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = static_cast<uint8_t>((sum + kSs * kSs / 2) / (kSs * kSs));
    }
}

// ---- on screen ----------------------------------------------------------------

struct Entry {
  ImTextureData* tex = nullptr;  // white RGBA, three bands of h rows (mark, word, dim), one clear row between
  int w = 0, h = 0;
  bool ok = false;
};

struct State {
  std::map<std::pair<int, int>, Entry> entries;  // (wordmark, device height)
  std::vector<ImTextureData*> retired;            // asked to be destroyed (density change)
  float density = 0;
};

std::unordered_map<ImGuiContext*, std::unique_ptr<State>>& states() {
  static std::unordered_map<ImGuiContext*, std::unique_ptr<State>> m;
  return m;
}

float density() {
  const float d = ImGui::GetIO().DisplayFramebufferScale.x;
  return d > 0 ? d : 1.0f;
}

void registerTexture(ImTextureData* t) {
  t->SetTexID(ImTextureID_Invalid);
  t->BackendUserData = nullptr;
  t->Status = ImTextureStatus_WantCreate;
  ImGui::RegisterUserTexture(t);
}

// Frees the textures the backend has destroyed; on a density change retires
// every cached wordmark (they are rasterised again at the new device height).
State& state() {
  auto& p = states()[ImGui::GetCurrentContext()];
  if (!p) p = std::make_unique<State>();
  State& st = *p;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (size_t i = 0; i < st.retired.size();) {
    ImTextureData* t = st.retired[i];
    const bool listed = g.UserTextures.contains(t);
    if (!listed || t->Status == ImTextureStatus_Destroyed) {
      if (listed) ImGui::UnregisterUserTexture(t);
      IM_DELETE(t);
      st.retired[i] = st.retired.back();
      st.retired.pop_back();
    } else {
      ++i;
    }
  }
  const float d = density();
  if (d != st.density) {
    for (auto& [k, e] : st.entries)
      if (e.tex) {
        e.tex->WantDestroyNextFrame = true;
        st.retired.push_back(e.tex);
      }
    st.entries.clear();
    st.density = d;
  }
  return st;
}

const Entry* entry(const std::string& key, float height) {
  const int idx = indexOf(key);
  if (idx < 0 || height <= 0) return nullptr;
  State& st = state();
  const int devH = std::max(1, static_cast<int>(std::lround(height * st.density)));
  auto it = st.entries.find({idx, devH});
  if (it == st.entries.end()) {
    Entry e;
    Raster r;
    if (rasterize(embedded(nullptr)[idx].svg, devH, r) && r.w > 0) {
      e.w = r.w;
      e.h = r.h;
      e.tex = IM_NEW(ImTextureData)();
      e.tex->Create(ImTextureFormat_RGBA32, r.w, r.h * 3 + 2);
      std::memset(e.tex->GetPixels(), 0, static_cast<size_t>(e.tex->GetSizeInBytes()));
      const std::vector<uint8_t>* planes[3] = {&r.mark, &r.word, &r.dim};
      for (int b = 0; b < 3; ++b)
        for (int y = 0; y < r.h; ++y) {
          uint32_t* out = reinterpret_cast<uint32_t*>(e.tex->GetPixelsAt(0, b * (r.h + 1) + y));
          const uint8_t* a = &(*planes[b])[static_cast<size_t>(y) * static_cast<size_t>(r.w)];
          for (int x = 0; x < r.w; ++x) out[x] = IM_COL32(255, 255, 255, a[x]);
        }
      registerTexture(e.tex);
      e.ok = true;
    }
    it = st.entries.emplace(std::make_pair(idx, devH), e).first;
  }
  Entry& e = it->second;
  if (e.ok && !ImGui::GetCurrentContext()->UserTextures.contains(e.tex)) registerTexture(e.tex);  // a new context at this address
  return e.ok ? &e : nullptr;
}

}  // namespace

const char* find(const std::string& key) {
  const int i = indexOf(key);
  return i < 0 ? nullptr : embedded(nullptr)[i].svg;
}

std::string title(const char* svg) {
  if (!svg) return {};
  const char* a = std::strstr(svg, "<title>");
  const char* b = a ? std::strstr(a, "</title>") : nullptr;
  if (!a || !b) return {};
  return std::string(a + 7, b);
}

ImU32 markColour(const std::string& key) {
  const char* svg = find(key);
  const char* p = svg ? std::strstr(svg, "id=\"mark") : nullptr;
  const char* f = p ? std::strstr(p, "fill=\"#") : nullptr;
  if (!f) return 0;
  const unsigned long v = std::strtoul(f + 7, nullptr, 16);
  return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255);
}

bool rasterize(const char* svg, int deviceHeight, Raster& out) {
  if (!svg || deviceHeight <= 0) return false;
  std::string copy(svg);  // nsvgParse writes into its input
  NSVGimage* img = nsvgParse(copy.data(), "px", 96.0f);
  if (!img) return false;
  if (img->width <= 0 || img->height <= 0) {
    nsvgDelete(img);
    return false;
  }
  const float scale = static_cast<float>(deviceHeight) / img->height;
  out.h = deviceHeight;
  out.w = std::max(1, static_cast<int>(std::ceil(img->width * scale - 0.01f)));
  NSVGrasterizer* r = nsvgCreateRasterizer();
  std::vector<uint8_t> cut;
  layer(r, img, kMark, scale, out.w, out.h, out.mark);
  layer(r, img, kCut, scale, out.w, out.h, cut);
  layer(r, img, kWord, scale, out.w, out.h, out.word);
  layer(r, img, kDim, scale, out.w, out.h, out.dim);
  nsvgDeleteRasterizer(r);
  nsvgDelete(img);
  for (size_t i = 0; i < out.mark.size(); ++i)  // cut-outs: true holes in the mark
    out.mark[i] = static_cast<uint8_t>((out.mark[i] * (255 - cut[i]) + 127) / 255);
  return true;
}

bool size(const std::string& key, float height, ImVec2* out) {
  const Entry* e = entry(key, height);
  if (!e) return false;
  if (out) *out = ImVec2(static_cast<float>(e->w) / states()[ImGui::GetCurrentContext()]->density, height);
  return true;
}

bool draw(ImDrawList* dl, const std::string& key, ImVec2 pos, float height, const Colours& c) {
  if (!dl) return false;
  const Entry* e = entry(key, height);
  if (!e) return false;
  const float d = states()[ImGui::GetCurrentContext()]->density;
  const float x0 = std::round(pos.x * d) / d, y0 = std::round(pos.y * d) / d;
  const ImVec2 p1(x0 + static_cast<float>(e->w) / d, y0 + static_cast<float>(e->h) / d);
  const ImU32 cols[3] = {c.mark, c.word, c.dim};
  for (int b = 0; b < 3; ++b) {
    if ((cols[b] & IM_COL32_A_MASK) == 0) continue;
    const float th = static_cast<float>(e->tex->Height), v0 = static_cast<float>(b * (e->h + 1));
    dl->AddImage(e->tex->GetTexRef(), ImVec2(x0, y0), p1, ImVec2(0, v0 / th), ImVec2(1, (v0 + static_cast<float>(e->h)) / th), cols[b]);
  }
  return true;
}

int cachedCount() {
  auto it = states().find(ImGui::GetCurrentContext());
  return it == states().end() ? 0 : static_cast<int>(it->second->entries.size());
}

}  // namespace gl::icons::wordmark
