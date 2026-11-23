// Gridline - K-142 (0.7.8): the shape memo of the plugin editors' drawing (src/app/PluginSkinMemo.h).
// A drawing copied from an earlier frame must give the very same vertices and indices as building
// it again, and any change of its inputs or of the draw list's state must build it fresh (nothing
// stale: a knob moved by automation, undo, a preset or a collaborator shows its new value at once).
// Headless: no GL context.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "Check.h"
#include "app/PluginSkinMemo.h"

using namespace gl::skin;

namespace {

struct Knob {
  ImVec2 c{100, 100};
  float r = 14, value = 0.3f;
  ImU32 fill = IM_COL32(230, 140, 40, 255);
  bool hot = false;
};

// Shapes like a knob face: shadows, a gradient cap, stroked arcs (thick and 1 px), lines, dots.
void drawShapes(ImDrawList* dl, const Knob& k) {
  const float a0 = 3.1416f * 0.75f, a1 = 3.1416f * 2.25f, an = a0 + k.value * (a1 - a0);
  dl->AddCircleFilled(ImVec2(k.c.x, k.c.y + 1), k.r, IM_COL32(0, 0, 0, 40), 40);
  dl->PathArcTo(k.c, k.r - 2, a0, a1, 32);
  dl->PathStroke(IM_COL32(40, 44, 48, 255), 0, 3.2f);
  dl->PathArcTo(k.c, k.r - 2, a0, an, 24);
  dl->PathStroke(k.hot ? IM_COL32(255, 180, 90, 255) : k.fill, 0, 3.2f);
  const int v0 = dl->VtxBuffer.Size;
  dl->AddCircleFilled(k.c, k.r * 0.6f, IM_COL32(255, 255, 255, 255), 24);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(k.c.x, k.c.y - k.r), ImVec2(k.c.x, k.c.y + k.r), IM_COL32(90, 96, 100, 255),
                                                IM_COL32(50, 54, 58, 255));
  dl->AddCircle(k.c, k.r * 0.6f, IM_COL32(20, 20, 20, 200), 24, 1.0f);
  dl->AddLine(k.c, ImVec2(k.c.x + std::cos(an) * k.r * 0.5f, k.c.y + std::sin(an) * k.r * 0.5f), IM_COL32_WHITE, 1.0f);
  dl->PathLineTo(k.c);
  dl->PathLineTo(ImVec2(k.c.x + std::cos(an) * k.r * 0.5f, k.c.y + std::sin(an) * k.r * 0.5f));
  dl->PathStroke(IM_COL32_WHITE, 0, 2.1f);
}

memo::Key keyOf(const Knob& k) {
  memo::Key key;
  key.add(0x54455354u);
  key.add(k.c), key.add(k.r), key.add(k.value), key.add(k.fill), key.add(k.hot);
  return key;
}

// As knobFace does it.
void drawMemo(ImDrawList* dl, const Knob& k) {
  const memo::Key key = keyOf(k);
  if (memo::replay(dl, key)) return;
  memo::Record rec(dl, key);
  drawShapes(dl, k);
}

struct Frame {
  std::vector<ImDrawVert> vtx;
  std::vector<ImDrawIdx> idx;
  int cmds = 0;
};

// One frame: a window with some drawing before the knobs (so the knobs start at a different
// index), then the knobs, memo or not. `aa`: anti-aliased lines and fills.
Frame frame(const std::vector<Knob>& knobs, bool useMemo, bool aa = true, float fringe = 1.0f) {
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = 1.0f / 60.0f;
  ImGuiStyle& st = ImGui::GetStyle();
  st.AntiAliasedLines = st.AntiAliasedFill = aa;
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(800, 600));
  ImGui::Begin("editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->_FringeScale = fringe;
  dl->AddRectFilled(ImVec2(10, 10), ImVec2(50, 30), IM_COL32(10, 20, 30, 255), 4.0f);
  for (const Knob& k : knobs) {
    if (useMemo) drawMemo(dl, k);
    else drawShapes(dl, k);
  }
  Frame f;
  f.vtx.assign(dl->VtxBuffer.Data, dl->VtxBuffer.Data + dl->VtxBuffer.Size);
  f.idx.assign(dl->IdxBuffer.Data, dl->IdxBuffer.Data + dl->IdxBuffer.Size);
  f.cmds = dl->CmdBuffer.Size;
  ImGui::End();
  ImGui::Render();
  return f;
}

bool same(const Frame& a, const Frame& b) {
  return a.cmds == b.cmds && a.vtx.size() == b.vtx.size() && a.idx.size() == b.idx.size() &&
         std::memcmp(a.vtx.data(), b.vtx.data(), sizeof(ImDrawVert) * a.vtx.size()) == 0 && a.idx == b.idx;
}

std::vector<Knob> row(float value, bool hot = false) {
  std::vector<Knob> ks;
  for (int i = 0; i < 12; ++i) {
    Knob k;
    k.c = ImVec2(60.0f + 40.0f * static_cast<float>(i), 120.0f + 0.37f * static_cast<float>(i));
    k.r = 10.0f + static_cast<float>(i % 3);
    k.value = std::fmod(value + 0.07f * static_cast<float>(i), 1.0f);
    k.hot = hot && i == 3;
    ks.push_back(k);
  }
  return ks;
}

}  // namespace

int main() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // headless: textures are never uploaded
  io.DisplaySize = ImVec2(800, 600);

  memo::setEnabled(true);
  memo::resetStats();
  const std::vector<Knob> ks = row(0.3f);
  for (int i = 0; i < 3; ++i) frame(ks, false);  // the window settles
  const Frame fresh = frame(ks, false);
  // Frame 1 notes the keys, frame 2 keeps the drawings, frame 3 and on copy them.
  for (int i = 0; i < 2; ++i) expect(same(frame(ks, true), fresh), "frames before any copy draw as without the memo");
  memo::resetStats();
  const Frame copied = frame(ks, true);
  expect(memo::stats().hits == ks.size(), "an unchanged editor copies every knob from the frame before");
  expect(same(copied, fresh), "the copied knobs are the same vertices and indices as built ones");
  expect(same(frame(ks, true), fresh), "and again the frame after");

  // A value change (automation, undo, a preset, a collaborator's edit, the plugin itself): built
  // fresh in that very frame, the others still copied.
  std::vector<Knob> moved = ks;
  moved[5].value = 0.81f;
  memo::resetStats();
  const Frame m = frame(moved, true);
  expect(same(m, frame(moved, false)), "a knob whose value changed draws its new value in the same frame");
  // (the check frame above ran without the memo; count from the memo frame)
  expect(memo::stats().hits == ks.size() - 1, "only the changed knob is built again");
  expect(same(frame(ks, true), fresh), "back to the old value: drawn as the old value");

  // Hover, colour, position and size are inputs too.
  std::vector<Knob> hov = row(0.3f, true);
  expect(same(frame(hov, true), frame(hov, false)), "a hovered knob draws hovered at once");
  std::vector<Knob> tinted = ks;
  tinted[2].fill = IM_COL32(40, 200, 90, 255);
  expect(same(frame(tinted, true), frame(tinted, false)), "a colour change draws at once");
  std::vector<Knob> shifted = ks;
  for (Knob& k : shifted) k.c.y += 0.25f;
  expect(same(frame(shifted, true), frame(shifted, false)), "a scrolled editor draws at its new place");

  // The draw list's state is part of the key: anti-aliasing off, another fringe.
  for (int i = 0; i < 3; ++i) frame(ks, true);
  expect(same(frame(ks, true, false), frame(ks, false, false)), "anti-aliasing turned off: built fresh, not copied");
  expect(same(frame(ks, true, true, 0.5f), frame(ks, false, true, 0.5f)), "another fringe scale (a zoomed editor): built fresh");
  expect(same(frame(ks, true), fresh), "and back");

  // Verify mode builds every kept drawing again and compares: no mismatch over many frames.
  memo::setVerify(true);
  memo::resetStats();
  for (int i = 0; i < 6; ++i) frame(row(0.3f + 0.01f * static_cast<float>(i % 2)), true);
  expect(memo::stats().verified > 0, "verify mode compared kept drawings");
  expect(memo::stats().mismatches == 0, "no kept drawing differs from a built one");
  memo::setVerify(false);

  // Off: nothing kept, nothing copied, drawing as before.
  memo::setEnabled(false);
  memo::resetStats();
  for (int i = 0; i < 3; ++i) expect(same(frame(ks, true), fresh), "memo off draws as before");
  expect(memo::stats().hits == 0 && memo::stats().entries == 0, "memo off keeps and copies nothing");
  memo::setEnabled(true);

  // Context blocks: equal bytes, the same number; different bytes, another one; never reused.
  const char a[] = "palette A", b[] = "palette B";
  const uint32_t ia = memo::contextId(a, sizeof a), ib = memo::contextId(b, sizeof b);
  expect(ia != ib && memo::contextId(a, sizeof a) == ia, "context numbers follow the bytes");
  for (int i = 0; i < 20; ++i) {
    char c[16];
    std::snprintf(c, sizeof c, "other %d", i);
    memo::contextId(c, sizeof c);
  }
  const uint32_t ia2 = memo::contextId(a, sizeof a);
  expect(ia2 != ia && ia2 != ib, "a block dropped and seen again gets a new number, never an old one");

  // Unused drawings are dropped after a while.
  Knob other;
  other.c = ImVec2(700, 500);
  for (int i = 0; i < 140; ++i) frame({other}, true);
  expect(memo::stats().entries < ks.size(), "drawings unused for a while are dropped");

  ImGui::DestroyContext();
  return finish();
}
