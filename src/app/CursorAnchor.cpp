// Gridline - J-111d: collaborator pointer anchors. See CursorAnchor.h.
#include "CursorAnchor.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "../../third_party/imgui/imgui_internal.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::canchor {
namespace {

struct Surface {
  int kind = 0;
  uint32_t a = 0, b = 0;
  ImVec2 origin, scale;
  ImRect clip;
  ImGuiWindow* root = nullptr;
};

struct Hit {  // the smallest item under a probe
  ImGuiID id = 0;
  ImRect bb;
  float area = FLT_MAX;
};

struct Found {  // a wanted item, as drawn this frame
  ImGuiID id = 0;
  ImRect bb, clip;
  ImGuiWindow* root = nullptr;
  bool seen = false;
};

struct State {
  bool on = false;
  std::vector<Probe> probes;
  std::vector<Hit> hits;
  std::vector<Found> found;
  std::vector<Surface> surfaces;
};

State& S() {
  static State s;
  return s;
}

void onItem(ImGuiID id, ImVec2 mn, ImVec2 mx) {
  State& s = S();
  ImGuiContext* g = ImGui::GetCurrentContext();
  ImGuiWindow* w = g ? g->CurrentWindow : nullptr;
  if (!w || id == 0 || w->SkipItems) return;
  const ImRect bb(mn, mx);
  if (bb.GetWidth() < 1 || bb.GetHeight() < 1) return;
  ImGuiWindow* root = nullptr;
  for (size_t i = 0; i < s.probes.size(); ++i) {
    const Probe& pr = s.probes[i];
    if (!pr.root || !bb.Contains(pr.p) || !w->ClipRect.Contains(pr.p)) continue;
    if (!root) root = rootOf(w);
    if (root != pr.root) continue;
    const float area = bb.GetWidth() * bb.GetHeight();
    if (area <= s.hits[i].area) s.hits[i] = Hit{id, bb, area};  // ties: the later (inner) item
  }
  for (Found& f : s.found) {
    if (f.id != id || f.seen) continue;
    if (!root) root = rootOf(w);
    f.bb = bb;
    f.clip = w->ClipRect;
    f.root = root;
    f.seen = true;
  }
}

const char* const kNames[KindCount] = {"window",      "item",       "playlist", "playlist-header", "playlist-ruler", "piano",
                                       "piano-keys", "piano-ruler", "piano-velocity", "rack-row",   "mixer-strip"};

}  // namespace

const char* kindName(int kind) { return kind >= 0 && kind < KindCount ? kNames[kind] : "?"; }

int kindByName(const std::string& name) {
  for (int k = 0; k < KindCount; ++k)
    if (name == kNames[k]) return k;
  return -1;
}

ImGuiWindow* rootOf(ImGuiWindow* w) {
  while (w && (w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive && w->ParentWindow) w = w->ParentWindow;
  return w;
}

void beginFrame(bool on, const std::vector<Probe>& probes, const std::vector<uint32_t>& wanted) {
  State& s = S();
  s.on = on;
  s.surfaces.clear();
  s.probes = on ? probes : std::vector<Probe>();
  s.hits.assign(s.probes.size(), Hit());
  s.found.clear();
  if (on)
    for (uint32_t id : wanted) {
      Found f;
      f.id = id;
      s.found.push_back(f);
    }
  imhook::setCursorHook(on ? &onItem : nullptr);
}

bool collecting() { return S().on; }

void publish(int kind, uint32_t a, uint32_t b, ImVec2 origin, ImVec2 scale, ImVec2 clipMin, ImVec2 clipMax) {
  State& s = S();
  if (!s.on) return;
  ImGuiContext* g = ImGui::GetCurrentContext();
  ImGuiWindow* w = g ? g->CurrentWindow : nullptr;
  if (!w || w->SkipItems || std::fabs(scale.x) < 1e-9f || std::fabs(scale.y) < 1e-9f) return;
  ImRect clip(clipMin, clipMax);
  clip.ClipWithFull(w->ClipRect);
  if (clip.GetWidth() <= 0 || clip.GetHeight() <= 0) return;  // nothing of it shows
  Surface sf;
  sf.kind = kind;
  sf.a = a;
  sf.b = b;
  sf.origin = origin;
  sf.scale = scale;
  sf.clip = clip;
  sf.root = rootOf(w);
  s.surfaces.push_back(sf);
}

bool anchorAt(size_t probe, Anchor& out) {
  State& s = S();
  if (!s.on || probe >= s.probes.size() || !s.probes[probe].root) return false;
  const Probe& pr = s.probes[probe];
  const Surface* best = nullptr;
  float bestArea = FLT_MAX;
  for (const Surface& sf : s.surfaces) {
    if (sf.root != pr.root || !sf.clip.Contains(pr.p)) continue;
    const float area = sf.clip.GetWidth() * sf.clip.GetHeight();
    if (area < bestArea) best = &sf, bestArea = area;
  }
  const Hit& h = s.hits[probe];
  if (h.id && h.area < bestArea) {
    out.kind = Item;
    out.a = h.id;
    out.b = 0;
    out.cx = (pr.p.x - h.bb.Min.x) / std::max(1.0f, h.bb.GetWidth());
    out.cy = (pr.p.y - h.bb.Min.y) / std::max(1.0f, h.bb.GetHeight());
    return true;
  }
  if (!best) return false;
  out.kind = best->kind;
  out.a = best->a;
  out.b = best->b;
  out.cx = (pr.p.x - best->origin.x) / best->scale.x;
  out.cy = (pr.p.y - best->origin.y) / best->scale.y;
  return true;
}

bool resolve(const Anchor& an, ImVec2& out, bool& inView, ImGuiWindow** root) {
  State& s = S();
  inView = false;
  if (!s.on || an.kind <= Window || an.kind >= KindCount) return false;
  ImRect clip;
  if (an.kind == Item) {
    const Found* f = nullptr;
    for (const Found& x : s.found)
      if (x.id == an.a && x.seen) f = &x;
    if (!f) return false;
    out = ImVec2(f->bb.Min.x + an.cx * f->bb.GetWidth(), f->bb.Min.y + an.cy * f->bb.GetHeight());
    clip = f->clip;
    clip.ClipWithFull(f->bb);
    if (root) *root = f->root;
  } else {
    const Surface* sf = nullptr;
    for (const Surface& x : s.surfaces)
      if (x.kind == an.kind && x.a == an.a && x.b == an.b) {
        sf = &x;
        // several pieces (a surface split by clipping): the one that holds the point
        const ImVec2 p(x.origin.x + an.cx * x.scale.x, x.origin.y + an.cy * x.scale.y);
        if (x.clip.Contains(p)) break;
      }
    if (!sf) return false;
    out = ImVec2(sf->origin.x + an.cx * sf->scale.x, sf->origin.y + an.cy * sf->scale.y);
    clip = sf->clip;
    if (root) *root = sf->root;
  }
  clip.Expand(0.5f);
  inView = clip.Contains(out);
  return true;
}

}  // namespace gl::canchor
