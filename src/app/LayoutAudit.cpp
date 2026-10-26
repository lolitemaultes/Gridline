// Gridline — editor layout audit collection (Q-7), see LayoutAudit.h.
#include "LayoutAudit.h"
#include <cstring>
#include <unordered_map>
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::audit {

namespace {
bool active = false;
Report cur;
uint8_t pendingKind = align::kOther;
std::vector<align::Box> hookItems;  // J-128: ImGui items seen while collecting
inset::Rect R(ImVec2 a, ImVec2 b) { return inset::Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)}; }
}  // namespace

bool on() { return active; }

void begin() {
  cur = Report{};
  hookItems.clear();
  active = true;
}

void frame(ImVec2 p0, ImVec2 p1, const char* what) {
  if (!active) return;
  cur.frames.push_back(R(p0, p1));
  cur.frameWhat.emplace_back(what ? what : "");
}

void control(ImVec2 p0, ImVec2 p1, const char* what) {
  if (!active) return;
  cur.controls.push_back(R(p0, p1));
  cur.controlWhat.emplace_back(what ? what : "");
  cur.controlKind.push_back(pendingKind);
  pendingKind = align::kOther;
  inset::Rect clip{};
  if (ImGui::GetCurrentContext() && ImGui::GetCurrentWindowRead()) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    clip = R(dl->GetClipRectMin(), dl->GetClipRectMax());
  }
  cur.controlClip.push_back(clip);
}

void item(unsigned id, ImVec2 p0, ImVec2 p1) {
  if (!active) return;
  if (ImGui::FindWindowByID(static_cast<ImGuiID>(id))) return;  // a child window, not a control
  if (p1.x - p0.x < 3 || p1.y - p0.y < 3) return;
  hookItems.push_back(align::Box{p0.x, p0.y, p1.x, p1.y});
}

void group(ImVec2 p0, ImVec2 p1, const char* what) {
  if (!active) return;
  const inset::Rect r = R(p0, p1);
  cur.scene.frames.push_back(align::Frame{align::Box{r.x0, r.y0, r.x1, r.y1}, what ? what : ""});
}

void nextKind(uint8_t kind) {
  if (active) pendingKind = kind;
}

void lastItem(const char* what) {
  if (!active) return;
  control(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), what);
}

Report end(float min) {
  active = false;
  pendingKind = align::kOther;
  Report r = std::move(cur);
  cur = Report{};
  r.issues = inset::check(r.frames, r.controls, min);
  const std::vector<align::Text> texts = std::move(r.scene.texts);
  const std::vector<align::Frame> groups = std::move(r.scene.frames);
  r.scene = sceneOf(r);
  r.scene.texts = texts;
  r.scene.frames.insert(r.scene.frames.end(), groups.begin(), groups.end());
  for (const align::Box& b : hookItems) {  // items no helper registered
    bool known = false;
    for (const align::Ctl& c : r.scene.ctls)
      if (std::fabs(c.r.x0 - b.x0) < 0.6f && std::fabs(c.r.y0 - b.y0) < 0.6f && std::fabs(c.r.x1 - b.x1) < 0.6f && std::fabs(c.r.y1 - b.y1) < 0.6f) {
        known = true;
        break;
      }
    if (!known) r.scene.ctls.push_back(align::Ctl{b, align::Box{}, align::kButton, "item"});
  }
  hookItems.clear();
  r.findings = align::check(r.scene);
  return r;
}

// ------------------------------------------------------------ J-128 text runs

namespace {
align::Box B(const inset::Rect& r) { return align::Box{r.x0, r.y0, r.x1, r.y1}; }
// Registered controls that only hold text (their text is read back from the draw lists).
bool textHolder(const std::string& w) {
  return w == "label" || w == "text" || w == "header" || w == "pill" || w == "value" || w == "lcd" || w == "name";
}

struct GlyphRef {
  const ImFontGlyph* g = nullptr;
  float ascent = 0, size = 0;  // baked font units
};
uint64_t uvKey(float u, float v) {
  uint32_t a, b;
  std::memcpy(&a, &u, 4);
  std::memcpy(&b, &v, 4);
  return (static_cast<uint64_t>(a) << 32) | b;
}

void appendUtf8(std::string& s, unsigned c) {
  char buf[5] = {};
  ImTextCharToUtf8(buf, c);
  s += buf;
}

void scanList(const ImDrawList* dl, const std::unordered_map<uint64_t, GlyphRef>& glyphs, const ImTextureData* atlasTex, std::vector<align::Text>& out) {
  if (!dl || dl->VtxBuffer.Size < 4) return;
  // Each command's vertex range and clip rectangle (commands follow vertex order).
  struct Range {
    unsigned v0, v1;
    ImVec4 clip;
    bool atlas;
  };
  std::vector<Range> ranges;
  for (const ImDrawCmd& c : dl->CmdBuffer) {
    if (c.ElemCount == 0 || c.UserCallback) continue;
    unsigned lo = 0xFFFFFFFFu, hi = 0;
    for (unsigned k = 0; k < c.ElemCount; ++k) {
      const unsigned v = dl->IdxBuffer[static_cast<int>(c.IdxOffset + k)] + c.VtxOffset;
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    ranges.push_back(Range{lo, hi, c.ClipRect, atlasTex && c.TexRef._TexData == atlasTex});
  }
  auto rangeOf = [&](unsigned v) -> const Range* {
    for (const Range& r : ranges)
      if (v >= r.v0 && v <= r.v1) return &r;
    return nullptr;
  };
  const ImDrawVert* V = dl->VtxBuffer.Data;
  const int n = dl->VtxBuffer.Size;
  align::Text run;
  bool open = false;
  float penEnd = 0, top = 0;
  auto flush = [&]() {
    if (open) out.push_back(run);
    open = false;
  };
  for (int i = 0; i + 3 < n;) {
    const ImDrawVert &a = V[i], &b = V[i + 1], &c = V[i + 2], &d = V[i + 3];
    const bool quad = a.uv.y == b.uv.y && b.uv.x == c.uv.x && c.uv.y == d.uv.y && d.uv.x == a.uv.x && a.uv.x != b.uv.x && a.uv.y != d.uv.y &&
                      a.pos.y == b.pos.y && b.pos.x == c.pos.x && c.pos.y == d.pos.y && d.pos.x == a.pos.x;
    if (!quad) {
      ++i;
      continue;
    }
    const Range* rg = rangeOf(static_cast<unsigned>(i));
    if (!rg || !rg->atlas) {
      ++i;
      continue;
    }
    auto it = glyphs.find(uvKey(a.uv.x, a.uv.y));
    const bool exact = it != glyphs.end() && it->second.g->U1 == c.uv.x && it->second.g->V1 == c.uv.y;
    if (!exact) {
      // A glyph cut by the fine clip (CPU side): its quad ends on the clip rectangle.
      const ImVec4& cr = rg->clip;
      const bool onEdge = std::fabs(a.pos.x - cr.x) < 0.01f || std::fabs(c.pos.x - cr.z) < 0.01f || std::fabs(a.pos.y - cr.y) < 0.01f ||
                          std::fabs(c.pos.y - cr.w) < 0.01f;
      if (onEdge && c.pos.x - a.pos.x < 40 && c.pos.y - a.pos.y < 40) {
        // J-128: a sliver trimmed off the top or bottom of a full-height glyph
        // (an accent or the quad's padding at a region's edge) is not visible.
        const bool sideCut = std::fabs(a.pos.x - cr.x) < 0.01f || std::fabs(c.pos.x - cr.z) < 0.01f;
        const bool sliver = open && !sideCut && c.pos.y - a.pos.y > 0.6f * run.size;
        if (open) {
          if (!sliver) run.clipped = true;
        } else {
          align::Text t;
          t.r = t.ink = align::Box{a.pos.x, a.pos.y, c.pos.x, c.pos.y};
          t.baseline = c.pos.y, t.size = c.pos.y - a.pos.y, t.clipped = true, t.str = "?";
          out.push_back(t);
        }
        i += 4;
        continue;
      }
      ++i;
      continue;
    }
    const ImFontGlyph& g = *it->second.g;
    const float gh = g.Y1 - g.Y0, gw = g.X1 - g.X0;
    const float scale = gh > 0 ? (c.pos.y - a.pos.y) / gh : gw > 0 ? (c.pos.x - a.pos.x) / gw : 1.0f;
    const float pen = a.pos.x - g.X0 * scale;
    const float lineTop = a.pos.y - g.Y0 * scale;
    const bool cont = open && std::fabs(lineTop - top) < 0.5f && pen >= penEnd - 1.0f && pen - penEnd < 0.75f * run.size;
    if (!cont) {
      flush();
      run = align::Text{};
      open = true;
      top = lineTop;
      run.r = align::Box{pen, lineTop, pen, lineTop};
      run.ink = align::Box{a.pos.x, a.pos.y, c.pos.x, c.pos.y};
      run.baseline = lineTop + it->second.ascent * scale;
      run.size = it->second.size * scale;
    }
    penEnd = pen + g.AdvanceX * scale;
    run.r.x1 = std::max(run.r.x1, penEnd);
    run.ink.x0 = std::min(run.ink.x0, a.pos.x), run.ink.y0 = std::min(run.ink.y0, a.pos.y);
    run.ink.x1 = std::max(run.ink.x1, c.pos.x), run.ink.y1 = std::max(run.ink.y1, c.pos.y);
    appendUtf8(run.str, g.Codepoint);
    // GPU clip: any part outside the command's clip rectangle is cut.
    const ImVec4& cr = rg->clip;
    if (a.pos.x < cr.x - 0.5f || a.pos.y < cr.y - 0.5f || c.pos.x > cr.z + 0.5f || c.pos.y > cr.w + 0.5f) run.clipped = true;  // >= 1 px at 200 %
    i += 4;
  }
  flush();
}
}  // namespace

std::vector<align::Text> textRuns(ImGuiWindow* root) {
  std::vector<align::Text> out;
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g || !root) return out;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (!atlas || !atlas->Builder) return out;
  // uv -> glyph of every baked size (the atlas holds each glyph once).
  std::unordered_map<uint64_t, GlyphRef> glyphs;
  ImFontAtlasBuilder* bd = atlas->Builder;
  for (int k = 0; k < bd->BakedPool.Size; ++k) {
    const ImFontBaked& bk = bd->BakedPool[k];
    for (const ImFontGlyph& gl : bk.Glyphs) {
      if (!gl.Visible) continue;
      glyphs[uvKey(gl.U0, gl.V0)] = GlyphRef{&gl, bk.Ascent, bk.Size};
    }
  }
  const ImTextureData* tex = atlas->TexData;
  for (ImGuiWindow* w : g->Windows) {
    if (w->RootWindow != root || w->LastFrameActive != g->FrameCount || w->Hidden) continue;
    const size_t first = out.size();
    scanList(w->DrawList, glyphs, tex, out);
    // A scrolling region cuts the text at its scrolled edge on purpose: only the
    // other edges count.
    const ImRect cr = w->InnerClipRect;
    for (size_t i = first; i < out.size(); ++i) {
      align::Text& t = out[i];
      if (!t.clipped) continue;
      const bool atY = t.ink.y0 < cr.Min.y + 0.5f || t.ink.y1 > cr.Max.y - 0.5f;
      const bool atX = t.ink.x0 < cr.Min.x + 0.5f || t.ink.x1 > cr.Max.x - 0.5f;
      if ((w->ScrollMax.y > 0 && atY && !atX) || (w->ScrollMax.x > 0 && atX && !atY)) t.clipped = false;
    }
  }
  for (align::Text& t : out)
    if (t.str != "?") t.r.y1 = t.r.y0 + t.size;
  return out;
}

void collectText(ImGuiWindow* root) {
  if (!active) return;
  std::vector<align::Text> t = textRuns(root);
  cur.scene.texts.insert(cur.scene.texts.end(), t.begin(), t.end());
}

align::Scene sceneOf(const Report& r) {
  align::Scene s;
  for (size_t i = 0; i < r.frames.size(); ++i) s.frames.push_back(align::Frame{B(r.frames[i]), r.frameWhat[i]});
  for (size_t i = 0; i < r.controls.size(); ++i) {
    if (r.controlWhat[i] == "pill" || r.controlWhat[i] == "header") s.captions.push_back(B(r.controls[i]));
    if (textHolder(r.controlWhat[i])) continue;
    align::Ctl c;
    c.r = B(r.controls[i]);
    c.clip = i < r.controlClip.size() ? B(r.controlClip[i]) : align::Box{};
    c.kind = i < r.controlKind.size() ? r.controlKind[i] : align::kOther;
    c.what = r.controlWhat[i];
    s.ctls.push_back(c);
  }
  return s;
}

namespace {
bool binding = false;
Bindings bind;
void onRead(ParamId pid) { ++bind.reads[pid.packed()]; }
}  // namespace

void bindBegin() {
  bind = Bindings{};
  binding = true;
  setParamReadHook(&onRead);
}

void bindTouch(const ParamId& pid) {
  if (binding) ++bind.reads[pid.packed()];
}

void stateRead(int k) {
  if (binding) ++bind.stateReads[k];
}

void bindControl(ParamId pid) {
  if (binding) ++bind.controls[pid.packed()];
}

Bindings bindEnd() {
  setParamReadHook(nullptr);
  binding = false;
  Bindings b = std::move(bind);
  bind = Bindings{};
  return b;
}

}  // namespace gl::audit
