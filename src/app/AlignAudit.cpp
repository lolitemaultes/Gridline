// Gridline — J-128 panel alignment audit, see AlignAudit.h.
#include "AlignAudit.h"
#include <cstdio>
#include <map>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "AlignCheck.h"
#include "LayoutAudit.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::alignaudit {

namespace {
struct Item {
  ImGuiWindow* root = nullptr;
  ImGuiWindow* win = nullptr;
  align::Box r, clip;
  uint8_t kind = align::kButton;
};
std::vector<Item> items;
bool collecting = false;
uint8_t pending = align::kButton;

void onItem(ImGuiID id, ImVec2 a, ImVec2 b) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  ImGuiWindow* w = g ? g->CurrentWindow : nullptr;
  const uint8_t kind = pending;
  pending = align::kButton;
  if (!w || !w->RootWindow || id == w->ID || id == w->MoveId) return;
  if (b.x - a.x < 2 || b.y - a.y < 2) return;
  Item it;
  it.kind = kind;
  it.root = w->RootWindow, it.win = w;
  it.r = align::Box{a.x, a.y, b.x, b.y};
  const ImVec2 c0 = w->DrawList->GetClipRectMin(), c1 = w->DrawList->GetClipRectMax();
  it.clip = align::Box{c0.x, c0.y, c1.x, c1.y};
  items.push_back(it);
}
}  // namespace

void begin() {
  items.clear();
  collecting = true;
  imhook::setItemHook(&onItem);
}

void nextKind(uint8_t kind) {
  if (collecting) pending = kind;
}

int finish(const std::string& path) {
  imhook::setItemHook(nullptr);
  collecting = false;
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return 0;
  std::string out = "window\trule\toff_px\tat\tdetail\n";
  int count = 0;
  for (ImGuiWindow* root : g->Windows) {
    if (root->RootWindow != root || root->LastFrameActive != g->FrameCount || root->Hidden || (root->Flags & ImGuiWindowFlags_Tooltip)) continue;
    align::Scene sc;
    for (ImGuiWindow* w : g->Windows)
      if (w->RootWindow == root && w->LastFrameActive == g->FrameCount && !w->Hidden)
        sc.frames.push_back(align::Frame{align::Box{w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x, w->Pos.y + w->Size.y}, w->Name});
    for (const Item& it : items) {
      if (it.root != root) continue;
      align::Ctl c;
      c.r = it.r;
      // Items in a scrolling region are cut by it on purpose: only the window's own edge counts.
      c.clip = (it.win->ScrollMax.x > 0 || it.win->ScrollMax.y > 0) ? align::Box{} : it.clip;
      c.what = it.kind == align::kKnob ? "knob" : "item";
      c.kind = it.kind;
      sc.ctls.push_back(c);
    }
    sc.texts = audit::textRuns(root);
    align::Options o;
    o.gaps = false, o.columns = false, o.ctlOverlaps = false, o.ctlClip = false;
    for (const align::Finding& f : align::check(sc, o)) {
      char at[96], off[24];
      std::snprintf(at, sizeof at, "%.1f,%.1f %.1fx%.1f", static_cast<double>(f.a.x0 - root->Pos.x), static_cast<double>(f.a.y0 - root->Pos.y),
                    static_cast<double>(f.a.w()), static_cast<double>(f.a.h()));
      std::snprintf(off, sizeof off, "%.2f", static_cast<double>(f.off));
      std::string name = root->Name;
      for (char& ch : name)
        if (ch == '\t' || ch == '\n') ch = ' ';
      out += name + "\t" + f.rule + "\t" + off + "\t" + at + "\t" + f.detail + "\n";
      ++count;
    }
  }
  items.clear();
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
  std::fprintf(stderr, "align audit: %d findings\n", count);
  return count;
}

}  // namespace gl::alignaudit
