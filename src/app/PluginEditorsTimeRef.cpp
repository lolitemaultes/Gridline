// Gridline — Reverb and Delay editors on the reference panels' grids
// (reverb 530 × 160, delay 747 × 217 panel units), drawn with per-plugin
// rack skins. Positions are panel units scaled to the window width.
#include <map>
#include "../engine/Dsp.h"
#include "../engine/GraphModels.h"
#include "../engine/LiveGraph.h"
#include "../engine/ReverbRoomViz.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

// Wheel with a small caption under it (centred on the wheel).
void wheelL(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k, const char* label,
            int flags = 0, ImU32 fill = 0) {
  skin::wheel(a, t, param, F.p(x, y), size * F.s, k, flags, fill);
  if (label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    skin::label(dl, F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 1.5f).y, label, k.textDim, std::clamp(7.5f * F.s, 10.0f, 13.0f));
  }
}

// ======================================================================= Reverb
const skin::Skin& reverbSkin() {
  static skin::Skin k = [] {
    skin::Skin s = skin::Skin::make(0x434B4D, 0x3F484D, 0x9B575D, 0xB06A70, 0x2E3538, 0x959D9F);
    s.panelHi = skin::hex(0x4A5355);
    s.panelLo = skin::hex(0x212526);
    s.graphBg = skin::hex(0x1B1F21);
    s.graphGrid = skin::hex(0x2B3133);
    s.graphGridStrong = skin::hex(0x3A4245);
    s.graphText = skin::hex(0x959D9F);
    s.lcdBg = skin::hex(0x212526);
    s.lcdText = skin::hex(0xD6A2A6);
    s.handle = skin::hex(0x93969F);
    s.select = skin::hex(0xC77A80);
    return s;
  }();
  return k;
}

// Room visual and decay view (Q-5). The reference's left box shows a
// wireframe prism "room"; here every control has its own cue, computed in
// roomviz (engine/ReverbRoomViz.h) from the active network's view and eased
// there, so this code only strokes prepared geometry: anti-aliased vectors,
// one line-weight scale per box, far edges dimmer and thinner, a soft glow
// under the cores. Both boxes share one palette from the reverb skin's roles.
struct RoomInk {
  ImU32 bgTop, bgBot, bgMid, frame, line, lineWarm, glow, spark, grid, text;
};
RoomInk roomInk(const skin::Skin& k) {
  RoomInk c;
  c.bgTop = skin::darken(k.fill, 0.45f);
  c.bgBot = skin::darken(k.fill, 0.74f);
  c.bgMid = theme::mix(c.bgTop, c.bgBot, 0.5f);
  c.frame = skin::darken(k.panel, 0.62f);
  c.line = skin::lighten(k.fill, 0.86f);      // lit edges
  c.lineWarm = skin::lighten(k.fill, 0.04f);  // damped highs: the top ring sinks into the plugin's colour
  c.glow = skin::lighten(k.fill, 0.55f);
  c.spark = skin::lighten(k.select, 0.6f);
  c.grid = theme::mix(c.bgMid, c.line, 0.13f);
  c.text = theme::mix(c.bgMid, c.line, 0.62f);
  return c;
}

// Per editor instance: the eased look and the geometry built from it.
struct RoomAnim {
  roomviz::Ease ease;
  roomviz::Look target, built;
  roomviz::Geometry geo;
  roomviz::Decay decay;
  uint64_t viewKey = 0;
  ImVec4 box{0, 0, 0, 0};
  bool haveTarget = false, haveGeo = false;
};

// Advances the easing and rebuilds geometry only when the look or the box moved.
void updateRoom(RoomAnim& r, const graphmodel::ReverbView& rv, uint64_t viewKey, ImVec2 s0, ImVec2 s1) {
  if (!r.haveTarget || viewKey != r.viewKey) r.target = roomviz::lookFor(rv), r.viewKey = viewKey, r.haveTarget = true;
  r.ease.step(r.target, std::clamp(static_cast<double>(ImGui::GetIO().DeltaTime), 0.0, 0.1));
  const ImVec4 box(s0.x, s0.y, s1.x, s1.y);
  const bool boxMoved = box.x != r.box.x || box.y != r.box.y || box.z != r.box.z || box.w != r.box.w;
  if (!r.haveGeo || boxMoved || r.ease.cur.v != r.built.v) {
    roomviz::build(r.ease.cur, s0.x, s0.y, s1.x, s1.y, r.geo);
    roomviz::buildDecay(r.ease.cur, r.decay);
    r.built = r.ease.cur, r.box = box, r.haveGeo = true;
  }
}

void boxBackground(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const RoomInk& c) {
  dl->AddRectFilledMultiColor(p0, p1, c.bgTop, c.bgTop, c.bgBot, c.bgBot);
}
void boxFrame(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const RoomInk& c) {
  dl->AddRect(ImVec2(p0.x - 0.5f, p0.y - 0.5f), ImVec2(p1.x + 0.5f, p1.y + 0.5f), c.frame, 0, 0, 1.0f);
}
ImVec2 iv(const roomviz::Pt& p) { return ImVec2(p.x, p.y); }

// Thick anti-aliased polyline with per-vertex colour and width: one mesh with
// mitred joins and a 1 px fringe, so joints have no notches and translucent
// strokes never blend twice where segments meet. n <= 64.
void strokeShaded(ImDrawList* dl, const ImVec2* p, const ImU32* col, const float* wid, int n, bool closed) {
  constexpr int kMax = 64;
  if (n < 2 || n > kMax) return;
  const int segs = closed ? n : n - 1;
  const float aa = dl->_FringeScale;
  ImVec2 sn[kMax];  // segment normals
  bool ok[kMax];
  for (int s = 0; s < segs; ++s) {
    const ImVec2 a = p[s], b = p[(s + 1) % n];
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    ok[s] = len > 1e-4f;
    sn[s] = ok[s] ? ImVec2(dy / len, -dx / len) : ImVec2(0, 0);
  }
  for (int pass = 0; pass < 2; ++pass)  // zero-length segments (side-count morph) borrow a neighbour's normal
    for (int s = 0; s < segs; ++s)
      if (!ok[s]) {
        const int q = (s + segs - 1) % segs, r = (s + 1) % segs;
        if (ok[q]) sn[s] = sn[q], ok[s] = true;
        else if (ok[r]) sn[s] = sn[r], ok[s] = true;
      }
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(segs * 18, n * 4);
  const unsigned int base = dl->_VtxCurrentIdx;  // after the reserve (it may start a new vertex offset)
  for (int i = 0; i < n; ++i) {
    ImVec2 m;
    if (!closed && i == 0) m = sn[0];
    else if (!closed && i == n - 1) m = sn[segs - 1];
    else {
      const ImVec2 n0 = sn[(i + segs - 1) % segs], n1 = sn[i % segs];
      m = ImVec2((n0.x + n1.x) * 0.5f, (n0.y + n1.y) * 0.5f);
      const float d2 = m.x * m.x + m.y * m.y;
      const float inv = d2 > 1e-6f ? std::min(1.0f / d2, 4.0f) : 1.0f;  // mitre, at most 2x
      m = ImVec2(m.x * inv, m.y * inv);
    }
    const float w = std::max(0.0f, wid[i]);
    const float h = std::max(0.0f, (w - aa) * 0.5f);
    ImU32 c = col[i];
    if (w < aa) c = theme::withAlpha(c, w / aa);  // hairlines fade instead of thinning below a pixel
    const ImU32 c0 = c & ~IM_COL32_A_MASK;
    dl->PrimWriteVtx(ImVec2(p[i].x + m.x * (h + aa), p[i].y + m.y * (h + aa)), uv, c0);
    dl->PrimWriteVtx(ImVec2(p[i].x + m.x * h, p[i].y + m.y * h), uv, c);
    dl->PrimWriteVtx(ImVec2(p[i].x - m.x * h, p[i].y - m.y * h), uv, c);
    dl->PrimWriteVtx(ImVec2(p[i].x - m.x * (h + aa), p[i].y - m.y * (h + aa)), uv, c0);
  }
  for (int s = 0; s < segs; ++s) {
    const unsigned int i0 = base + static_cast<unsigned int>(s * 4), i1 = base + static_cast<unsigned int>(((s + 1) % n) * 4);
    for (unsigned int k = 0; k < 3; ++k) {
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + k)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + k + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(i1 + k + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + k)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(i1 + k + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(i1 + k));
    }
  }
}

void roomVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const RoomAnim& r, const RoomInk& c) {
  using namespace roomviz;
  const Geometry& g = r.geo;
  const Look& l = r.built;
  boxBackground(dl, p0, p1, c);
  dl->PushClipRect(p0, p1, true);
  const float u = std::clamp(g.unit * 0.9f, 1.0f, 4.0f);
  const float wet = std::clamp(l[kWet], 0.f, 1.f), bright = std::clamp(l[kBright], 0.f, 1.f), bass = std::clamp(l[kBass], 0.f, 1.f);
  // Floor: soft shadow under the room, then the decay ripples (level = the
  // tail's level at each ripple's time), then the reflection sparks.
  {
    ImVec2 pts[kRipplePts];
    for (int i = 0; i < kRipplePts; ++i) pts[i] = iv(g.shadow[static_cast<size_t>(i)]);
    dl->AddConvexPolyFilled(pts, kRipplePts, theme::withAlpha(skin::darken(c.bgBot, 0.5f), 0.55f));
    for (int k = kRipples - 1; k >= 0; --k) {
      const float lv = g.rippleLevel[static_cast<size_t>(k)];
      for (int i = 0; i < kRipplePts; ++i) pts[i] = iv(g.ripple[static_cast<size_t>(k)][static_cast<size_t>(i)]);
      dl->AddPolyline(pts, kRipplePts, theme::mix(c.bgMid, c.glow, 0.10f + 0.72f * lv), ImDrawFlags_Closed, u * (0.65f + 0.35f * lv));
    }
  }
  for (int e = 0; e < kMaxTaps; ++e) {
    const float lv = g.sparkLevel[static_cast<size_t>(e)];
    if (lv < 0.01f) continue;
    const ImVec2 q = iv(g.spark[static_cast<size_t>(e)]);
    dl->AddCircleFilled(q, u * (1.8f + 1.6f * lv), theme::withAlpha(c.spark, 0.22f * lv), 16);
    dl->AddCircleFilled(q, u * (0.55f + 0.75f * lv), theme::mix(c.bgMid, c.spark, 0.35f + 0.65f * lv), 12);
  }
  // Room: faint top cap, then glow and cores. Rings are single meshes shaded
  // per vertex by depth (no notches at the joints, no double blending); the
  // uprights behind the rings go first, the ones in front last.
  {
    ImVec2 cap[kMaxSides];
    for (int i = 0; i < g.capCount; ++i) cap[i] = iv(g.cap[static_cast<size_t>(i)]);
    dl->AddConvexPolyFilled(cap, g.capCount, theme::withAlpha(c.glow, (0.03f + 0.08f * wet) * (0.4f + 0.6f * bright)));
  }
  const ImU32 topCol = theme::mix(c.lineWarm, c.line, bright);
  // Many sides: thinner uprights so a near-cylinder reads as a surface, not a hatch.
  const float upW = 1.0f - 0.35f * std::clamp((l[kSides] - 3.f) / 25.f, 0.f, 1.f);
  const float kindW[3] = {0.7f + 0.45f * bright, 0.75f + 0.6f * bass, upW};
  const ImU32 kindCol[3] = {topCol, c.line, theme::mix(topCol, c.line, 0.5f)};
  auto width = [&](int kind, float depth) { return u * (1.25f - 0.6f * depth) * kindW[kind]; };
  auto core = [&](int kind, float depth, float alpha) { return theme::withAlpha(theme::mix(c.bgMid, kindCol[kind], 0.97f - 0.7f * depth), alpha); };
  auto glowA = [&](float depth, int pass) { return (1.0f - 0.8f * depth) * (pass == 0 ? 0.06f + 0.16f * wet : 0.09f + 0.24f * wet); };
  ImVec2 pts[kMaxSides];
  ImU32 cols[kMaxSides];
  float wids[kMaxSides];
  auto ring = [&](const std::array<Pt, kMaxSides>& r, int kind, int pass) {  // pass 0/1 glow, 2 core
    for (int i = 0; i < g.capCount; ++i) {
      const Pt& q = r[static_cast<size_t>(i)];
      pts[i] = iv(q);
      wids[i] = width(kind, q.depth) * (pass == 0 ? 4.6f : pass == 1 ? 2.4f : 1.0f);
      cols[i] = pass == 2 ? core(kind, q.depth, 1.f) : theme::withAlpha(c.glow, glowA(q.depth, pass));
    }
    strokeShaded(dl, pts, cols, wids, g.capCount, true);
  };
  auto upright = [&](const Seg& s, int pass) {
    const ImVec2 e[2] = {iv(s.a), iv(s.b)};
    const float w2[2] = {width(2, s.a.depth) * (pass == 0 ? 4.6f : pass == 1 ? 2.4f : 1.0f), width(2, s.b.depth) * (pass == 0 ? 4.6f : pass == 1 ? 2.4f : 1.0f)};
    // core: shaded from the base ring's colour to the top ring's
    const ImU32 c2[2] = {pass == 2 ? theme::withAlpha(theme::mix(c.bgMid, c.line, 0.97f - 0.7f * s.a.depth), s.alpha)
                                   : theme::withAlpha(c.glow, glowA(s.a.depth, pass) * s.alpha),
                         pass == 2 ? theme::withAlpha(theme::mix(c.bgMid, topCol, 0.97f - 0.7f * s.b.depth), s.alpha)
                                   : theme::withAlpha(c.glow, glowA(s.b.depth, pass) * s.alpha)};
    strokeShaded(dl, e, c2, w2, 2, false);
  };
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < g.segCount; ++i) {  // far uprights (segs are sorted far to near)
      const Seg& s = g.segs[static_cast<size_t>(i)];
      if (s.kind == 2 && s.a.depth + s.b.depth > 1.0f) upright(s, pass);
    }
    ring(g.base, 1, pass);
    ring(g.cap, 0, pass);
    for (int i = 0; i < g.segCount; ++i) {
      const Seg& s = g.segs[static_cast<size_t>(i)];
      if (s.kind == 2 && s.a.depth + s.b.depth <= 1.0f) upright(s, pass);
    }
  }
  dl->PopClipRect();
  boxFrame(dl, p0, p1, c);
  const float fz = std::clamp(l[kFreeze], 0.f, 1.f);
  if (fz > 0.01f) {
    const float fs = std::clamp((p1.y - p0.y) * 0.075f, 9.0f, 12.0f);
    monoText(dl, ImVec2(p0.x + 5, p1.y - fs - 4), theme::withAlpha(c.spark, fz), "HOLD", fs);
  }
}

void decayVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const RoomAnim& r, const graphmodel::ReverbView& rv, const RoomInk& c) {
  using namespace roomviz;
  const Decay& d = r.decay;
  const Look& l = r.built;
  boxBackground(dl, p0, p1, c);
  dl->PushClipRect(p0, p1, true);
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const float u = std::clamp(r.geo.unit * 0.9f, 1.0f, 4.0f);
  const float lx = p0.x + std::round(w * 0.035f), rx = p1.x - std::round(w * 0.035f);
  const float top = p0.y + std::round(h * 0.24f), base = p1.y - std::round(h * 0.14f);
  const float span = std::max(0.05f, d.span);
  auto X = [&](double sec) { return lx + static_cast<float>(std::clamp(sec / span, 0.0, 1.0)) * (rx - lx); };
  auto Y = [&](float lv) { return base - std::clamp(lv, 0.f, 1.f) * (base - top); };
  auto px = [](float v) { return std::floor(v) + 0.5f; };  // 1 px grid lines on pixel centres
  // Grid: time steps (at most ~6 lines), -20 / -40 dB, the -60 dB floor.
  static const float steps[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f};
  float st = 10.f;
  for (float s : steps)
    if (span / s <= 6.f) {
      st = s;
      break;
    }
  for (int i = 1; i * st < span; ++i) dl->AddLine(ImVec2(px(X(i * st)), top - 2), ImVec2(px(X(i * st)), base), c.grid, 1.0f);
  for (int db = 1; db <= 2; ++db) dl->AddLine(ImVec2(lx, px(Y(db / 3.f))), ImVec2(rx, px(Y(db / 3.f))), c.grid, 1.0f);
  // Pre-delay: the tail has not started yet.
  if (d.pre > 0) dl->AddRectFilled(ImVec2(X(0), top - 2), ImVec2(X(d.pre), base), theme::withAlpha(c.glow, 0.07f));
  // Mid-band envelope as one convex fill (dB lines: straight, then the floor).
  const float wet = std::clamp(l[kWet], 0.f, 1.f), bright = std::clamp(l[kBright], 0.f, 1.f);
  ImVec2 pts[kDecayPts + 3];
  int n = 0;
  pts[n++] = ImVec2(X(d.pre), base);
  pts[n++] = ImVec2(X(d.pre), Y(1));
  for (int i = 0; i <= kDecayPts; ++i) {
    const double t = static_cast<double>(span) * i / kDecayPts;
    if (t > d.pre) pts[n++] = ImVec2(X(t), Y(d.mid[static_cast<size_t>(i)]));
  }
  pts[n++] = ImVec2(rx, base);
  dl->AddConvexPolyFilled(pts, n, theme::withAlpha(c.glow, 0.13f + 0.12f * wet));
  dl->AddLine(ImVec2(lx, px(base)), ImVec2(rx, px(base)), theme::mix(c.bgMid, c.line, 0.25f), 1.0f);
  // Band envelopes: below the crossover (base ring colour), 6 kHz (top ring colour), mid on top.
  auto band = [&](const std::array<float, kDecayPts + 1>& v, ImU32 col, float thick) {
    ImVec2 q[kDecayPts + 2];
    int m = 0;
    q[m++] = ImVec2(X(d.pre), Y(1));
    for (int i = 0; i <= kDecayPts; ++i) {
      const double t = static_cast<double>(span) * i / kDecayPts;
      if (t > d.pre) q[m++] = ImVec2(X(t), Y(v[static_cast<size_t>(i)]));
    }
    dl->AddPolyline(q, m, col, 0, thick);
  };
  band(d.lo, theme::mix(c.bgMid, c.line, 0.45f), u * 0.8f);
  band(d.hi, theme::mix(c.bgMid, theme::mix(c.lineWarm, c.line, bright), 0.75f), u * 0.8f);
  band(d.mid, c.line, u * 1.1f);
  // Direct sound and the reflections where the engine taps them (same sparks as the floor).
  dl->AddLine(ImVec2(X(0), base), ImVec2(X(0), top - 2), theme::mix(c.bgMid, c.line, 0.8f), u);
  for (int e = 0; e < kMaxTaps; ++e) {
    const float lv = std::clamp(l[kErLvl0 + e], 0.f, 1.f);
    if (lv < 0.01f) continue;
    const float x = X(l[kErSec0 + e]), y = Y(lv * 0.9f);
    dl->AddLine(ImVec2(x, base), ImVec2(x, y), theme::mix(c.bgMid, c.spark, 0.3f + 0.5f * lv), std::max(1.0f, u * 0.7f));
    dl->AddCircleFilled(ImVec2(x, y), u * (0.6f + 0.5f * lv), theme::mix(c.bgMid, c.spark, 0.4f + 0.6f * lv), 10);
  }
  dl->PopClipRect();
  boxFrame(dl, p0, p1, c);
  // Read-outs: pre-delay (left), the network's T60 (right).
  const float fs = std::clamp(h * 0.17f, 9.0f, 12.0f);
  char buf[32];
  if (rv.freeze) std::snprintf(buf, sizeof buf, "T60 hold");
  else if (rv.rt60 < 10) std::snprintf(buf, sizeof buf, "T60 %.2f s", rv.rt60);
  else std::snprintf(buf, sizeof buf, "T60 %.1f s", rv.rt60);
  ImGui::PushFont(app().fontMono, fs);
  const float tw = ImGui::CalcTextSize(buf).x;
  ImGui::PopFont();
  const float ty = p0.y + std::max(2.0f, (top - p0.y - fs) * 0.5f - 1);
  monoText(dl, ImVec2(p1.x - tw - 5, ty), c.text, buf, fs);
  if (rv.preSec >= 0.0005) {
    std::snprintf(buf, sizeof buf, "PRE %.0f ms", rv.preSec * 1000);
    monoText(dl, ImVec2(p0.x + 5, ty), c.text, buf, fs);
  }
}

}  // namespace

void editorReverbRef(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = reverbSkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kW = 530, kH = 160;
  skin::Form F{ImGui::GetCursorScreenPos(), width / kW};
  const float s = F.s;
  skin::panel(dl, F.o, ImVec2(F.o.x + width, F.o.y + kH * s), K, 4);
  // Visual (0,0) 160×160; main panel (160,0) 370×160.
  const double sr = a.engine.sampleRate(), tempo = a.doc.song().tempo;
  const uint64_t inputs = livegraph::Hasher().floats(st.params).d(sr).d(tempo).value();
  const graphmodel::ReverbView& rv =
      livegraph::memo<graphmodel::ReverbView>(t.key(), 0x52563030).get(inputs, [&](graphmodel::ReverbView& o) { o = graphmodel::reverbView(st, sr, tempo); });
  {  // Q-5: room visual (prism + floor) over the decay view, one eased state
    const ImVec2 r0 = F.p(4, 4), r1 = F.p(156, 108);
    const float rw = r1.x - r0.x, rh = r1.y - r0.y;
    RoomAnim& ra = livegraph::memo<RoomAnim>(t.key(), 0x52563032).value;
    // Stage: clear of the model selector strip at the top, even side margins.
    updateRoom(ra, rv, inputs, ImVec2(r0.x + rw * 0.05f, r0.y + rh * 0.2f), ImVec2(r1.x - rw * 0.05f, r1.y - rh * 0.05f));
    const RoomInk ink = roomInk(K);
    roomVisual(dl, r0, r1, ra, ink);
    decayVisual(dl, F.p(4, 112), F.p(156, 156), ra, rv, ink);
  }
  {  // J-26: network model (hall / plate reference) in the room view's corner
    static const char* md[3] = {"HALL", "PLATE", "FULL"};
    skin::switchSel(a, t, RvbModel, F.p(80, 6), ImVec2(74 * s, 15 * s), K, md, 3, true);
  }
  dl->AddRectFilled(F.p(160, 0), F.p(530, 160), skin::hex(0x434B4D), 0);
  dl->AddLine(F.p(160, 0), F.p(160, 160), skin::darken(K.panel, 0.5f), 1.0f);
  const float fsL = std::clamp(7.5f * s, 10.0f, 13.0f);
  // Mid / side input selector (169,18) 45×30.
  {
    skin::label(dl, F.p(169 + 22.5f, 0).x, F.p(0, 6).y, "INPUT", K.textDim, fsL);
    static const char* io[2] = {"MID", "M+S"};
    skin::switchSel(a, t, RvbInputMode, F.p(169, 18), ImVec2(45 * s, 30 * s), K, io, 2);
  }
  // J-35: the full model edits the reference's own integer controls.
  const bool full = static_cast<int>(std::lround(val(st, RvbModel))) == 2;
  auto P = [full](int legacy, int fl) { return full ? fl : legacy; };
  wheelL(a, t, P(RvbHighCut, RvbFHighCut), F, 172, 57, 36, K, "H.CUT");
  wheelL(a, t, P(RvbLowCut, RvbFLowCut), F, 172, 108, 36, K, "L.CUT");
  wheelL(a, t, P(RvbPreDelay, RvbFPredelay), F, 231, 5, 36, K, "PRE");
  // Tempo-based pre-delay check (287,27) + the pre-delay read-out in steps:ticks when on.
  skin::check(a, t, RvbPreSync, F.p(281, 24), ImVec2(30 * s, 14 * s), K, "TEMPO");
  if (bval(st, RvbPreSync)) {
    int ticks = full ? static_cast<int>(std::lround(val(st, RvbFPredelay))) : static_cast<int>(std::lround(val(st, RvbPreDelay) * 0.384));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", ticks / 48, ticks % 48);
    skin::lcd(dl, F.p(274, 7), F.p(316, 20), K, buf, fsL);
  }
  wheelL(a, t, P(RvbSize, RvbFRoom), F, 231, 57, 36, K, "SIZE");
  wheelL(a, t, P(RvbDiffusion, RvbFDiffusion), F, 231, 108, 36, K, "DIFF");
  wheelL(a, t, P(RvbModDepth, RvbFModDepth), F, 275, 57, 36, K, "DEPTH");
  wheelL(a, t, P(RvbModRate, RvbFModSpeed), F, 275, 108, 36, K, "SPEED");
  wheelL(a, t, P(RvbBassMult, RvbFBass), F, 334, 5, 36, K, "BASS");
  wheelL(a, t, P(RvbCrossover, RvbFCross), F, 334, 57, 36, K, "CROSS");
  wheelL(a, t, P(RvbDecayTime, RvbFDecay), F, 378, 5, 36, K, "DEC");
  wheelL(a, t, P(RvbDampHz, RvbFDamp), F, 377, 57, 36, K, "DAMP");
  // Gridline additions in the free cell under the decay group (not in the full model).
  if (!full) wheelL(a, t, RvbColor, F, 334, 108, 36, K, "COLOR", skin::WheelCentered);
  if (!full) {
    skin::label(dl, F.p(378 + 18, 0).x, F.p(0, 103).y, "FREEZE", K.textDim, fsL);
    static const char* fz[3] = {"OFF", "FRZ", "SUS"};
    skin::switchSel(a, t, RvbFreeze, F.p(378, 113), ImVec2(36 * s, 36 * s), K, fz, 3);
  }
  // Level sliders (434/466/497, 1) 24×90 and separation wheel (460,108).
  const int lv[3] = {P(RvbDry, RvbFDry), P(RvbEarly, RvbFEr), P(RvbWet, RvbFWet)};
  const float lx[3] = {434, 466, 497};
  const char* ln[3] = {"DRY", "ER", "WET"};
  for (int i = 0; i < 3; ++i) {
    skin::vslider(a, t, lv[i], F.p(lx[i], 1), ImVec2(24 * s, 90 * s), K);
    skin::label(dl, F.p(lx[i] + 12, 0).x, F.p(0, 92).y, ln[i], K.textDim, fsL);
  }
  wheelL(a, t, P(RvbWidth, RvbFSep), F, 460, 108, 36, K, "SEP", skin::WheelCentered);
  ImGui::SetCursorScreenPos(F.o);
  ImGui::Dummy(ImVec2(width, kH * s));
}

}  // namespace gl
