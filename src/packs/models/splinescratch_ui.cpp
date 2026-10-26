// Gridline — Spline Scratcher editor on the reference spline scratcher's
// 715 × 224 form: the sample row (load button, file name, waveform with the
// window markers), the patch name, the global speed / spline type / attack /
// release / start / end controls and the patch keyboard (click a key to edit
// its patch, right-click to audition) on the left; the spline editor of the
// current patch (position or volume spline, chosen under the vertical sample
// view) with its mute bar underneath; the vertical sample view on the right.
#include <filesystem>
#include "../../app/App.h"
#include "metal_widgets.h"
#include "../../app/Widgets.h"

namespace gl::packs::models::metal {

using namespace ui;

namespace {

constexpr float kW = 715, kH = 224;

const skin::Skin& ssSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(bgr(3880753), 0x272B2E, 0x22262A, 0xB4C46A, 0x585E63, 0xC9CED2);
    s.select = skin::hex(0xB4C46A);
    s.accent2 = skin::hex(0xD06AD0);
    s.graphBg = skin::hex(0x1C2023);
    s.graphGrid = skin::hex(0x2A3034);
    s.graphGridStrong = skin::hex(0x3A4248);
    s.graphCurve = skin::hex(0xB4C46A);
    s.lcdBg = skin::hex(bgr(5326389));
    s.lcdText = skin::hex(0xD8DCDF);
    return s;
  }();
  return k;
}

struct SsUi {
  bool volume = false;  // editing the volume spline
  int sel = -1, oct = 4;
};

void commit(App& a, const PluginTarget& t, const PluginState& st, const SsData& d, const char* label) {
  PluginState ns = st;
  ns.data = serializeScratchSpl(d);
  if (!(ns == st)) setPluginState(a, t, ns, label, (t.key() * 1099511628211ull) ^ 0x5353ull);
}

// Spline editor over x (time 0..1) and y (0..1), points as SsPoint.
bool splineEdit(const flk::Ctx& c, ImVec2 p0, ImVec2 p1, std::vector<SsPoint>& pts, int& sel, ImU32 col) {
  ImDrawList* dl = c.dl;
  skin::graphWell(dl, p0, p1, c.K);
  const ImVec2 q0(p0.x + 5, p0.y + 5), q1(p1.x - 5, p1.y - 5);
  const float W = q1.x - q0.x, H = q1.y - q0.y;
  for (int i = 1; i < 8; ++i) dl->AddLine(ImVec2(q0.x + W * i / 8, q0.y), ImVec2(q0.x + W * i / 8, q1.y), i % 2 ? c.K.graphGrid : c.K.graphGridStrong, 1);
  for (int i = 1; i < 4; ++i) dl->AddLine(ImVec2(q0.x, q0.y + H * i / 4), ImVec2(q1.x, q0.y + H * i / 4), c.K.graphGrid, 1);
  auto sx = [&](float x) { return q0.x + x * W; };
  auto sy = [&](float y) { return q1.y - y * H; };
  ImGui::SetCursorScreenPos(q0);
  ImGui::InvisibleButton("##spl", ImVec2(std::max(1.0f, W), std::max(1.0f, H)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const ImVec2 m = ImGui::GetIO().MousePos;
  int hot = -1;
  for (size_t i = 0; i < pts.size(); ++i)
    if (std::fabs(m.x - sx(pts[i].x)) < 6 && std::fabs(m.y - sy(pts[i].y)) < 6) hot = static_cast<int>(i);
  bool changed = false;
  if (hov && ImGui::IsMouseClicked(0)) {
    sel = hot;
    if (hot < 0) {  // click on empty space adds a point
      SsPoint p;
      p.x = std::clamp((m.x - q0.x) / W, 0.0f, 1.0f), p.y = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
      pts.push_back(p);
      std::stable_sort(pts.begin(), pts.end(), [](const SsPoint& a, const SsPoint& b) { return a.x < b.x; });
      for (size_t i = 0; i < pts.size(); ++i)
        if (pts[i].x == p.x && pts[i].y == p.y) sel = static_cast<int>(i);
      changed = true;
    }
  }
  if (hov && ImGui::IsMouseClicked(1) && hot > 0 && hot + 1 < static_cast<int>(pts.size())) {
    pts.erase(pts.begin() + hot);
    sel = -1;
    changed = true;
  }
  if (act && sel >= 0 && sel < static_cast<int>(pts.size()) && ImGui::IsMouseDragging(0, 0) && !pts[static_cast<size_t>(sel)].locked) {
    SsPoint& p = pts[static_cast<size_t>(sel)];
    const float lo = sel > 0 ? pts[static_cast<size_t>(sel) - 1].x : 0.0f;
    const float hi = sel + 1 < static_cast<int>(pts.size()) ? pts[static_cast<size_t>(sel) + 1].x : 1.0f;
    if (sel > 0 && sel + 1 < static_cast<int>(pts.size())) p.x = std::clamp((m.x - q0.x) / W, lo, hi);
    p.y = std::clamp((q1.y - m.y) / H, 0.0f, 1.0f);
    changed = true;
  }
  ImVec2 prev;
  for (int i = 0; i <= 240; ++i) {
    const float x = i / 240.0f;
    const ImVec2 q(sx(x), sy(ssEval(pts, x)));
    if (i) dl->AddLine(prev, q, col, 2);
    prev = q;
  }
  for (size_t i = 0; i < pts.size(); ++i)
    dl->AddCircleFilled(ImVec2(sx(pts[i].x), sy(pts[i].y)), static_cast<int>(i) == sel ? 5.0f : 3.5f, static_cast<int>(i) == sel ? c.K.text : col);
  if (hov) c.a.setHint("Click: add point · drag: move · right-click: delete");
  return changed;
}

}  // namespace

void drawSplineScratch(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = ssSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  flk::Ctx c{a, t, F, K, dl};
  SsUi& u = stateFor<SsUi>(t);
  SsData d;
  parseScratchSpl(st.data, d);
  SamplePtr smp = sampleOf(d.path);
  SsPatch& cur = d.keys[static_cast<size_t>(d.current)];
  if (!cur.used) cur = ssDefaultPatch();
  ImGui::PushID("gl.splinescratch");
  // ---- sample row
  PluginTarget tt = t;
  if (flk::btn(c, "##load", 1, 1, 34, 18, "LOAD", false))
    openSampleDialog(a, "Load sample", [&a, tt](const std::string& p) {
      const PluginState* now = tt.state(a.doc.song());
      if (!now) return;
      SsData dd;
      parseScratchSpl(now->data, dd);
      dd.path = p;
      commit(a, tt, *now, dd, "load sample");
    });
  {
    std::string nm = d.path.empty() ? std::string("(built-in phrase — drop a sample on the channel)") : std::filesystem::path(d.path).filename().string();
    flk::lcd(c, 39, 1, 212, 18, nm.c_str());
  }
  const ImVec2 w0 = F.p(0, 24), w1 = F.p(252, 97);
  skin::graphWell(dl, w0, w1, K);
  const float wa = prmf(st, SsStartWave, 0) / 10000.0f, wb = prmf(st, SsEndWave, 10000) / 10000.0f;
  dl->AddRectFilled(ImVec2(w0.x + (w1.x - w0.x) * std::min(wa, wb), w0.y), ImVec2(w0.x + (w1.x - w0.x) * std::max(wa, wb), w1.y),
                    skin::hex(0xB4C46A, 0.12f));
  waveform(dl, smp.get(), ImVec2(w0.x + 2, w0.y + 2), ImVec2(w1.x - 2, w1.y - 2), K.fill);
  for (float m : {wa, wb}) dl->AddLine(ImVec2(w0.x + (w1.x - w0.x) * m, w0.y), ImVec2(w0.x + (w1.x - w0.x) * m, w1.y), K.accent2C(), 2);
  // ---- patch name + global controls
  flk::wheel(c, SsStartWave, 2, 102, 20, nullptr);
  flk::wheel(c, SsEndWave, 230, 102, 20, nullptr);
  {
    char nm[96];
    std::snprintf(nm, sizeof nm, "%s %d%s%s", "PATCH", d.current, cur.name.empty() ? "" : " · ", cur.name.c_str());
    flk::lcd(c, 32, 100, 188, 17, nm);
  }
  flk::wheel(c, SsSpeed, 105, 121, 26, "Speed");
  static const char* modes[2] = {"POS", "SPD"};
  flk::sw(c, SsSplineMode, 136, 121, 22, 26, modes, 2);
  flk::wheel(c, SsAttack, 163, 121, 26, "Att");
  flk::wheel(c, SsRelease, 196, 121, 26, "Rel");
  flk::wheel(c, SsVolume, 8, 121, 26, "Vol");
  flk::chk(c, SsContinuous, 40, 127, 60, 14, "LOOP");
  // ---- patch keyboard (two octaves from u.oct)
  {
    const ImVec2 k0 = F.p(0, 162), k1 = F.p(252, 224);
    dl->AddRectFilled(k0, k1, skin::hex(bgr(8024943)), 2);
    if (flk::btn(c, "##octdn", 228, 146, 12, 14, GL_ICON_LABEL("chevron-left", "<"), false)) u.oct = std::max(0, u.oct - 1);
    if (flk::btn(c, "##octup", 238, 146, 12, 14, GL_ICON_LABEL("chevron-right", ">"), false)) u.oct = std::min(8, u.oct + 1);
    const int first = u.oct * 12, whites = 14;
    const float kw = (k1.x - k0.x) / whites;
    static const int wsemi[7] = {0, 2, 4, 5, 7, 9, 11};
    for (int w = 0; w < whites; ++w) {
      const int key = first + (w / 7) * 12 + wsemi[w % 7];
      if (key >= kSsKeys) break;
      const ImVec2 a0(k0.x + w * kw + 1, k0.y + 1), a1(k0.x + (w + 1) * kw - 1, k1.y - 1);
      const bool edited = d.keys[static_cast<size_t>(key)].used;
      dl->AddRectFilled(a0, a1, key == d.current ? K.fill : edited ? skin::hex(0xE6EBD0) : skin::hex(0xC8CCCF), 2);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(key);
      ImGui::InvisibleButton("##w", ImVec2(std::max(1.0f, a1.x - a0.x), std::max(1.0f, a1.y - a0.y)));
      if (ImGui::IsItemClicked(0) && key != d.current) {
        d.current = key;
        commit(a, t, st, d, "select patch");
      }
      ImGui::PopID();
    }
    for (int w = 0; w < whites; ++w) {
      const int semi = wsemi[w % 7];
      if (semi == 4 || semi == 11) continue;
      const int key = first + (w / 7) * 12 + semi + 1;
      if (key >= kSsKeys) break;
      const ImVec2 a0(k0.x + (w + 0.65f) * kw, k0.y + 1), a1(k0.x + (w + 1.35f) * kw, k0.y + (k1.y - k0.y) * 0.6f);
      const bool edited = d.keys[static_cast<size_t>(key)].used;
      dl->AddRectFilled(a0, a1, key == d.current ? K.fill : edited ? skin::hex(0x5C6A2E) : skin::hex(0x24282B), 2);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(1000 + key);
      ImGui::InvisibleButton("##b", ImVec2(std::max(1.0f, a1.x - a0.x), std::max(1.0f, a1.y - a0.y)));
      if (ImGui::IsItemClicked(0) && key != d.current) {
        d.current = key;
        commit(a, t, st, d, "select patch");
      }
      ImGui::PopID();
    }
  }
  // ---- spline editor + mute bar
  {
    std::vector<SsPoint>& pts = u.volume ? cur.vol : cur.pos;
    if (splineEdit(c, F.p(253, 0), F.p(653, 210), pts, u.sel, u.volume ? skin::hex(0xD06AD0) : K.fill)) {
      cur.used = true;
      commit(a, t, st, d, u.volume ? "edit volume spline" : "edit position spline");
    }
    const ImVec2 m0 = F.p(253, 211), m1 = F.p(653, 224);
    dl->AddRectFilled(m0, m1, skin::hex(0x1C2023), 2);
    const int steps = 64;
    for (int i = 0; i < steps; ++i) {
      const float x = (i + 0.5f) / steps;
      if (!ssSoundOn(cur, x))
        dl->AddRectFilled(ImVec2(m0.x + (m1.x - m0.x) * i / steps, m0.y + 2), ImVec2(m0.x + (m1.x - m0.x) * (i + 1) / steps, m1.y - 2),
                          skin::hex(0xC0504D));
    }
    ImGui::SetCursorScreenPos(m0);
    ImGui::InvisibleButton("##mute", ImVec2(std::max(1.0f, m1.x - m0.x), std::max(1.0f, m1.y - m0.y)));
    if (ImGui::IsItemClicked(0)) {  // toggle the step under the mouse
      const float x = std::clamp((ImGui::GetIO().MousePos.x - m0.x) / (m1.x - m0.x), 0.0f, 1.0f);
      const int i = std::clamp(static_cast<int>(x * steps), 0, steps - 1);
      const float xa = static_cast<float>(i) / steps, xb = static_cast<float>(i + 1) / steps;
      const bool on = ssSoundOn(cur, (xa + xb) * 0.5f);
      const bool after = ssSoundOn(cur, std::min(1.0f, xb + 0.5f / steps));
      std::vector<std::pair<bool, float>> mt;
      for (const auto& mm : cur.mutes)
        if (mm.second < xa - 1e-6f || mm.second > xb + 1e-6f) mt.push_back(mm);
      mt.push_back({!on, xa});
      mt.push_back({after, xb});
      std::stable_sort(mt.begin(), mt.end(), [](const auto& p, const auto& q) { return p.second < q.second; });
      cur.mutes = mt;
      cur.used = true;
      commit(a, t, st, d, "toggle mute");
    }
    if (ImGui::IsItemHovered()) a.setHint("Click: mute / unmute this step of the patch");
  }
  // ---- vertical sample view + part selector
  {
    const ImVec2 v0 = F.p(654, 0), v1 = F.p(715, 210);
    skin::graphWell(dl, v0, v1, K);
    waveform(dl, smp.get(), ImVec2(v0.x + 2, v0.y + 2), ImVec2(v1.x - 2, v1.y - 2), K.textDim, true, cur.start, cur.end);
    if (flk::btn(c, "##part", 654, 211, 61, 13, u.volume ? "VOLUME" : "POSITION", u.volume)) u.volume = !u.volume, u.sel = -1;
  }
  ImGui::PopID();
  F.end(kW, kH);
}

}  // namespace gl::packs::models::metal
