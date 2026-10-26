// Gridline — "modulation" pack: reference-family skin helpers (see modulation_skin.h).
#include "modulation_skin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

namespace gl::packs::modulation::sk {

namespace {

struct Bound {
  ParamId pid;
  ParamDesc d;
  double val = 0;
  bool ok = false;
};

Bound bind(const EditTarget& e, int param) {
  Bound b;
  if (!e.a || param < 0) return b;
  const Song& s = e.a->doc.song();
  b.pid = e.pid(param);
  auto d = describeParam(s, b.pid);
  auto v = getParam(s, b.pid);
  if (!d || !v) return b;
  b.d = *d;
  b.val = *v;
  b.ok = true;
  return b;
}

ImU32 mix(ImU32 a, ImU32 b, float t) { return theme::mix(a, b, std::clamp(t, 0.0f, 1.0f)); }

skin::Skin ctl(uint32_t fill) {
  skin::Skin s = skin::Skin::fl(0x31373B, 0x363C40, 0x293034, fill, 0x40474D, 0xB9BFC3);
  s.lcdBg = skin::hex(0x262B2F);
  s.lcdText = skin::hex(fill);
  s.graphBg = skin::hex(0x292F33);
  s.graphGrid = skin::hex(0x343B40);
  s.graphGridStrong = skin::hex(0x434B51);
  s.graphText = skin::hex(0x8E979C);
  s.graphCurve = skin::hex(fill);
  s.pillBg = skin::hex(0x3B4246);
  s.valueBg = skin::hex(0x262B2F);
  return s;
}

}  // namespace

const skin::Skin& controller() {
  static const skin::Skin k = ctl(0xFFA830);
  return k;
}
const skin::Skin& controllerLfo() {
  static const skin::Skin k = [] {
    skin::Skin s = ctl(0xBCFF31);
    s.fillPressed = skin::hex(0xE6FF31);
    return s;
  }();
  return k;
}
const skin::Skin& formula() {
  static const skin::Skin k = [] {
    skin::Skin s = ctl(0xE9EFF3);
    s.rail = skin::hex(0x3B4144);
    s.cap = skin::hex(0x464C50);
    s.lcdText = skin::hex(0xE9EFF3);
    s.graphCurve = skin::hex(0xE9EFF3);
    s.accent2 = skin::hex(0x68C7CC);
    return s;
  }();
  return k;
}
const skin::Skin& xyPad() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3C4154, 0x525C5B, 0xC6C7C8, 0x7F2020, 0xC6C7C8, 0xB9C5C4);
    s.fillPressed = skin::hex(0xCE0000);
    s.panelLo = skin::hex(0x383F3E);
    s.lcdBg = skin::hex(0x2B3040);
    s.lcdText = skin::hex(0xB0EDB3);
    s.graphBg = skin::hex(0x2B3040);
    s.graphGrid = skin::hex(0x373D50);
    s.graphGridStrong = skin::hex(0x4A5168);
    s.graphCurve = skin::hex(0xB0EDB3);
    s.select = skin::hex(0xCE3030);
    s.accent2 = skin::hex(0xB0EDB3);
    return s;
  }();
  return k;
}
const skin::Skin& xyzPad() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x363C40, 0x3E4448, 0x353B3F, 0xC2D4DE, 0x40474D, 0xB9BFC3);
    s.fillPressed = skin::hex(0xFFBC40);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0xAFEE51);
    s.graphBg = skin::hex(0x262B2F);
    s.graphCurve = skin::hex(0xC2D4DE);
    s.select = skin::hex(0xAFEE51);
    s.accent2 = skin::hex(0x49B5F5);
    return s;
  }();
  return k;
}
const skin::Skin& voltage() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3C4346, 0x3C5375, 0x3B4144, 0x5A7AAB, 0x3C4346, 0x97C0FF);
    s.fillPressed = skin::hex(0x97C0FF);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0x97C0FF);
    s.pillBg = skin::hex(0x3C5375);
    s.pillText = skin::hex(0xB3D1FF);
    s.valueBg = skin::hex(0xB9D5FF);
    s.valueText = skin::hex(0x10151A);
    s.select = skin::hex(0xB9D5FF);
    s.graphBg = skin::hex(0x262B2F);
    return s;
  }();
  return k;
}
const skin::Skin& send() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x363C40, 0x2E3438, 0x4ECCFF, 0x62676A, 0xBFC8CD);
    s.fillPressed = skin::hex(0x40A8DE);
    s.lcdBg = skin::hex(0x7E343C);
    s.lcdText = skin::hex(0xFFEAAA);
    s.accent2 = skin::hex(0x825CEE);
    s.select = skin::hex(0xFFEAAA);
    return s;
  }();
  return k;
}
const skin::Skin& surface() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x2F383D, 0x262B2F, 0x87F279, 0x40474D, 0xC7D0D6);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0x87F279);
    s.select = skin::hex(0x87F279);
    return s;
  }();
  return k;
}
const skin::Skin& patcher() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3F484D, 0x374045, 0x2F383D, 0xFFBC40, 0x40474D, 0xB7C0C6);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0xFFBC40);
    s.graphBg = skin::hex(0x2A3135);
    s.graphGrid = skin::hex(0x343C41);
    s.graphGridStrong = skin::hex(0x485156);
    s.select = skin::hex(0xFFBC40);
    s.accent2 = skin::hex(0x26A9FF);
    return s;
  }();
  return k;
}
const skin::Skin& noteFx(uint32_t fill) {
  static std::deque<std::pair<uint32_t, skin::Skin>> cache;
  for (const auto& c : cache)
    if (c.first == fill) return c.second;
  skin::Skin s = skin::Skin::fl(0x3F484D, 0x363C40, 0x2A3236, fill, 0x40474D, 0xBFC8CD);
  s.panelLo = skin::hex(0x21272B);
  s.lcdBg = skin::hex(0x262B2F);
  s.lcdText = skin::hex(fill);
  s.graphBg = skin::hex(0x2F383D);
  s.graphGrid = skin::hex(0x3A444A);
  s.graphGridStrong = skin::hex(0x4C575D);
  s.graphText = skin::hex(0x9FA8AD);
  s.graphCurve = skin::hex(fill);
  s.pillBg = skin::hex(0x273035);
  s.textDim = skin::hex(0x9FA8AD);
  cache.emplace_back(fill, s);
  return cache.back().second;
}

bool wheel(const EditTarget& e, int param, ImVec2 tl, float size, const skin::Skin& k, int flags, ImU32 fill) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5100);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##w", ImVec2(size, size));
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool ch = pui::dragParamItem(*e.a, b.pid, b.d, b.val, 170.0f, false);
  bool centred = (flags & skin::WheelCentered) || (b.d.min < 0 && b.d.max > 0);
  ImU32 f = fill ? fill : k.fill;
  skin::drawWheel(ImGui::GetWindowDrawList(), ImVec2(tl.x + size * 0.5f, tl.y + size * 0.5f), size * 0.5f, b.d.toNormalized(b.val), centred,
                  flags & skin::WheelBigLine, hov || act, k, act ? skin::lighten(f, 0.2f) : f);
  if (pui::paramLinked(*e.a, b.pid)) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(tl.x + size * 0.9f, tl.y + size * 0.1f), 2.5f, f);
  ImGui::PopID();
  return ch;
}

bool wheelL(const EditTarget& e, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k, const char* label, int flags,
            ImU32 fill) {
  bool ch = wheel(e, param, F.p(x, y), size * F.s, k, flags, fill);
  if (label && *label) skin::label(ImGui::GetWindowDrawList(), F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 1.5f).y, label, k.textDim, F.font(7.5f));
  return ch;
}

bool vslider(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, ImU32 fill) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5200);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##vs", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool ch = pui::dragParamItem(*e.a, b.pid, b.d, b.val, size.y, false);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!fill) fill = k.fill;
  float cx = std::round(tl.x + size.x * 0.5f);
  float hh = std::clamp(size.x * 0.3f, 6.0f, 12.0f);
  float top = tl.y + hh * 0.5f, bot = tl.y + size.y - hh * 0.5f, travel = bot - top;
  float rw = std::max(3.0f, size.x * 0.16f);
  for (int i = 0; i <= 10; ++i) {
    float y = top + travel * i / 10.0f;
    float w = i % 5 == 0 ? size.x * 0.42f : size.x * 0.3f;
    dl->AddLine(ImVec2(cx - w, y), ImVec2(cx - rw, y), theme::withAlpha(k.textDim, 0.35f), 1.0f);
  }
  dl->AddRectFilled(ImVec2(cx - rw * 0.5f, top), ImVec2(cx + rw * 0.5f, bot), k.lcdBg, rw * 0.5f);
  float n = static_cast<float>(b.d.toNormalized(b.val));
  bool bip = b.d.min < 0 && b.d.max > 0;
  float y0 = bip ? top + (1 - static_cast<float>(b.d.toNormalized(0))) * travel : bot;
  float yv = top + (1 - n) * travel;
  dl->AddRectFilled(ImVec2(cx - rw * 0.5f, std::min(y0, yv)), ImVec2(cx + rw * 0.5f, std::max(y0, yv)), act ? skin::lighten(fill, 0.2f) : fill, rw * 0.5f);
  float hw = std::min(size.x * 0.45f, 14.0f);
  ImU32 hc = act ? skin::lighten(k.handle, 0.3f) : hov ? skin::lighten(k.handle, 0.15f) : k.handle;
  dl->AddRectFilled(ImVec2(cx - hw, yv - hh * 0.5f + 1), ImVec2(cx + hw, yv + hh * 0.5f + 1), theme::withAlpha(IM_COL32_BLACK, 0.35f), 2);
  dl->AddRectFilled(ImVec2(cx - hw, yv - hh * 0.5f), ImVec2(cx + hw, yv + hh * 0.5f), hc, 2);
  dl->AddLine(ImVec2(cx - hw + 2, yv), ImVec2(cx + hw - 2, yv), fill, 1.5f);
  ImGui::PopID();
  return ch;
}

bool hslider(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, ImU32 fill) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5300);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##hs", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool ch = pui::dragParamItem(*e.a, b.pid, b.d, b.val, size.x, true);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!fill) fill = k.fill;
  float cy = std::round(tl.y + size.y * 0.5f);
  float hw = std::clamp(size.y * 0.45f, 6.0f, 10.0f);
  float l = tl.x + hw * 0.5f, r = tl.x + size.x - hw * 0.5f;
  float rh = std::max(3.0f, size.y * 0.22f);
  dl->AddRectFilled(ImVec2(l, cy - rh * 0.5f), ImVec2(r, cy + rh * 0.5f), k.lcdBg, rh * 0.5f);
  bool bip = b.d.min < 0 && b.d.max > 0;
  float x0 = bip ? l + static_cast<float>(b.d.toNormalized(0)) * (r - l) : l;
  float xv = l + static_cast<float>(b.d.toNormalized(b.val)) * (r - l);
  dl->AddRectFilled(ImVec2(std::min(x0, xv), cy - rh * 0.5f), ImVec2(std::max(x0, xv), cy + rh * 0.5f), act ? skin::lighten(fill, 0.2f) : fill, rh * 0.5f);
  ImU32 hc = act ? skin::lighten(k.handle, 0.3f) : hov ? skin::lighten(k.handle, 0.15f) : k.handle;
  dl->AddRectFilled(ImVec2(xv - hw * 0.5f, tl.y + 1), ImVec2(xv + hw * 0.5f, tl.y + size.y - 1), hc, 2);
  dl->AddLine(ImVec2(xv, tl.y + 3), ImVec2(xv, tl.y + size.y - 3), fill, 1.5f);
  ImGui::PopID();
  return ch;
}

bool check(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* s) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5400);
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton("##ck", size);
  bool hov = ImGui::IsItemHovered();
  bool on = b.val >= 0.5 * (b.d.min + b.d.max) && b.val > b.d.min;
  if (pressed) e.a->doc.setParam(b.pid, on ? b.d.min : b.d.max, false), on = !on;
  if (hov) e.a->setHint(b.d.name + ": " + (on ? "on" : "off") + "   (click to toggle)");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float bs = std::min(size.y - 2, 13.0f);
  ImVec2 b0(tl.x + 1, tl.y + (size.y - bs) * 0.5f), b1(b0.x + bs, b0.y + bs);
  dl->AddRectFilled(b0, b1, k.lcdBg, 2);
  dl->AddRect(b0, b1, hov ? k.text : skin::darken(k.panel, 0.6f), 2, 0, 1.0f);
  if (on) {
    dl->AddRectFilled(ImVec2(b0.x + 2.5f, b0.y + 2.5f), ImVec2(b1.x - 2.5f, b1.y - 2.5f), k.fill, 1);
    dl->AddRectFilled(ImVec2(b0.x - 1, b0.y - 1), ImVec2(b1.x + 1, b1.y + 1), theme::withAlpha(k.fill, 0.15f), 3);
  }
  if (s && *s) {
    ImGui::PushFont(app().fontMonoBold, std::clamp(size.y * 0.62f, 9.0f, 12.5f));
    ImVec2 ts = ImGui::CalcTextSize(s);
    dl->PushClipRect(tl, ImVec2(tl.x + size.x, tl.y + size.y), true);
    dl->AddText(ImVec2(b1.x + 5, std::round(tl.y + (size.y - ts.y) * 0.5f)), on ? k.text : k.textDim, s);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  ImGui::PopID();
  return pressed;
}

bool button(const char* id, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* s, bool on) {
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  ImU32 base = skin::lighten(k.lcdBg, 0.08f);
  ImU32 bg = on ? mix(base, k.select, act ? 0.45f : 0.28f) : act ? skin::lighten(base, 0.12f) : hov ? skin::lighten(base, 0.07f) : base;
  dl->AddRectFilled(tl, p1, bg, 3);
  dl->AddRect(tl, p1, on ? k.select : skin::darken(k.panel, 0.5f), 3, 0, 1.0f);
  dl->AddLine(ImVec2(tl.x + 3, tl.y + 1), ImVec2(p1.x - 3, tl.y + 1), theme::withAlpha(IM_COL32_WHITE, 0.06f));
  if (s && *s) {
    ImGui::PushFont(app().fontMonoBold, std::clamp(size.y * 0.5f, 9.0f, 12.5f));
    ImVec2 ts = ImGui::CalcTextSize(ui::labelText(s));
    dl->PushClipRect(tl, p1, true);
    ui::drawLabel(dl, ImVec2(std::round(std::max(tl.x + 3, (tl.x + p1.x - ts.x) * 0.5f)), std::round((tl.y + p1.y - ts.y) * 0.5f)),
                  on ? skin::lighten(k.select, 0.45f) : hov ? k.text : k.textDim, s);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  return pressed;
}

bool toggle(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* s) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5500);
  bool on = b.val >= 0.5 * (b.d.min + b.d.max) && b.val > b.d.min;
  std::string t = s ? std::string(s) : b.d.name;
  bool pressed = sk::button("##tg", tl, size, k, t.c_str(), on);
  if (ImGui::IsItemHovered()) e.a->setHint(b.d.name + ": " + (on ? "on" : "off") + "   (click to toggle)");
  if (pressed) e.a->doc.setParam(b.pid, on ? b.d.min : b.d.max, false);
  ImGui::PopID();
  return pressed;
}

bool selector(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const std::vector<std::string>* labels, int cols) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  std::vector<std::string> names = labels ? *labels : b.d.choices;
  if (names.empty() && b.d.kind == ParamKind::Toggle) names = {"Off", "On"};
  if (names.empty()) return false;
  int n = static_cast<int>(names.size());
  cols = cols > 0 ? std::min(cols, n) : n;
  int rows = (n + cols - 1) / cols;
  ImGui::PushID(param + 0x5600);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##sel", size);
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  float cw = size.x / cols, ch = size.y / rows;
  int cur = std::clamp(static_cast<int>(std::lround(b.val - b.d.min)), 0, n - 1);
  int hc = -1;
  if (hov) {
    int c = std::clamp(static_cast<int>((io.MousePos.x - tl.x) / cw), 0, cols - 1), r = std::clamp(static_cast<int>((io.MousePos.y - tl.y) / ch), 0, rows - 1);
    hc = std::min(n - 1, r * cols + c);
  }
  bool changed = false;
  if (ImGui::IsItemClicked() && hc >= 0 && hc != cur) e.a->doc.setParam(b.pid, b.d.min + hc, false), cur = hc, changed = true;
  if (hov && io.MouseWheel != 0) {
    int s = std::clamp(cur - static_cast<int>(io.MouseWheel), 0, n - 1);
    if (s != cur) e.a->doc.setParam(b.pid, b.d.min + s, false), cur = s, changed = true;
  }
  if (hc >= 0) e.a->setHint(b.d.name + ": " + names[static_cast<size_t>(hc)]);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, k.panelLo, 3);
  float fs = std::clamp(ch * 0.55f, 9.0f, 12.5f);
  ImGui::PushFont(app().fontMonoBold, fs);
  for (int i = 0; i < n; ++i) {
    int r = i / cols, c = i % cols;
    ImVec2 c0(tl.x + c * cw + 1, tl.y + r * ch + 1), c1(tl.x + (c + 1) * cw - 1, tl.y + (r + 1) * ch - 1);
    bool on = i == cur;
    ImU32 bg = on ? skin::lighten(k.wellC(), 0.04f) : i == hc ? skin::lighten(k.panelLo, 0.10f) : skin::lighten(k.panelLo, 0.04f);
    dl->AddRectFilled(c0, c1, bg, 2);
    if (on) dl->AddLine(ImVec2(c0.x + 3, c1.y - 1), ImVec2(c1.x - 3, c1.y - 1), k.select, 2.0f);
    const char* t = names[static_cast<size_t>(i)].c_str();
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->PushClipRect(c0, c1, true);
    dl->AddText(ImVec2(std::round(std::max(c0.x + 2, (c0.x + c1.x - ts.x) * 0.5f)), std::round((c0.y + c1.y - ts.y) * 0.5f)),
                on ? skin::lighten(k.select, 0.2f) : i == hc ? k.text : k.textDim, t);
    dl->PopClipRect();
  }
  ImGui::PopFont();
  ImGui::PopID();
  return changed;
}

bool menuBox(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5700);
  ImGui::SetCursorScreenPos(tl);
  bool pressed = ImGui::InvisibleButton("##mb", size);
  bool hov = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  int n = b.d.choices.empty() ? static_cast<int>(b.d.max - b.d.min) + 1 : static_cast<int>(b.d.choices.size());
  n = std::max(1, n);
  int cur = std::clamp(static_cast<int>(std::lround(b.val - b.d.min)), 0, n - 1);
  bool changed = false;
  if (hov && io.MouseWheel != 0) {
    int s = std::clamp(cur - static_cast<int>(io.MouseWheel), 0, n - 1);
    if (s != cur) e.a->doc.setParam(b.pid, b.d.min + s, false), cur = s, changed = true;
  }
  if (hov) e.a->setHint(b.d.name + ": " + b.d.format(b.d.min + cur) + "   (click for the list · wheel steps)");
  if (pressed) ImGui::OpenPopup("##mbp");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  dl->AddRectFilled(tl, p1, hov ? skin::lighten(k.lcdBg, 0.06f) : k.lcdBg, 3);
  dl->AddRect(tl, p1, skin::darken(k.panel, 0.55f), 3, 0, 1.0f);
  std::string v = b.d.format(b.d.min + cur);
  float fs = std::clamp(size.y * 0.55f, 9.0f, 12.5f);
  ImGui::PushFont(app().fontMono, fs);
  ImVec2 ts = ImGui::CalcTextSize(v.c_str());
  float aw = std::min(size.y * 0.8f, 14.0f);
  dl->PushClipRect(tl, ImVec2(p1.x - aw, p1.y), true);
  dl->AddText(ImVec2(std::round(tl.x + 6), std::round((tl.y + p1.y - ts.y) * 0.5f)), k.text, v.c_str());
  dl->PopClipRect();
  ImGui::PopFont();
  ImVec2 ac(p1.x - aw * 0.5f - 3, (tl.y + p1.y) * 0.5f);
  float r = std::min(4.0f, size.y * 0.18f);
  ui::drawIconFit(dl, ui::Icon::Down, ImVec2(ac.x - r, ac.y - r * 0.5f), ImVec2(ac.x + r, ac.y + r * 0.7f), k.textDim);
  if (ImGui::BeginPopup("##mbp")) {
    for (int i = 0; i < n; ++i) {
      std::string nm = b.d.format(b.d.min + i);
      if (ImGui::MenuItem(nm.c_str(), nullptr, i == cur) && i != cur) e.a->doc.setParam(b.pid, b.d.min + i, false), changed = true;
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

bool digi(const EditTarget& e, int param, ImVec2 tl, ImVec2 size, const skin::Skin& k, const char* s) {
  Bound b = bind(e, param);
  if (!b.ok) return false;
  ImGui::PushID(param + 0x5800);
  ImGui::SetCursorScreenPos(tl);
  ImGui::InvisibleButton("##dg", size);
  bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  bool ch = pui::dragParamItem(*e.a, b.pid, b.d, b.val, 80.0f, false);
  std::string v = s ? std::string(s) : b.d.format(b.val);
  skin::Skin kk = k;
  if (hov || act) kk.lcdText = skin::lighten(k.lcdText, 0.3f);
  skin::lcd(ImGui::GetWindowDrawList(), tl, ImVec2(tl.x + size.x, tl.y + size.y), kk, v.c_str(), std::clamp(size.y * 0.62f, 9.0f, 13.0f));
  ImGui::PopID();
  return ch;
}

skin::Form openForm(const EditTarget& e, const skin::Skin& k, float formW, float formH, float unitsAcross) {
  float across = unitsAcross > 0 ? unitsAcross : formW;
  skin::Form F = skin::Form::fit(e.width, across);
  F.o.x += std::max(0.0f, (e.width - formW * F.s) * 0.5f);
  skin::panel(ImGui::GetWindowDrawList(), F.o, F.p(formW, formH), k, 4);
  return F;
}

void panelBox(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* caption) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, k.wellC(), 4 * F.s);
  dl->AddRect(p0, p1, theme::withAlpha(skin::darken(k.panel, 0.5f), 0.7f), 4 * F.s, 0, 1.0f);
  if (caption && *caption) {
    ImGui::PushFont(app().fontMonoBold, F.font(8.5f));
    dl->AddText(ImVec2(std::round(p0.x + 6 * F.s), std::round(p0.y + 4 * F.s)), k.textDim, caption);
    ImGui::PopFont();
  }
}

void titleStrip(ImDrawList* dl, const skin::Form& F, float x, float y, float w, float h, const skin::Skin& k, const char* title) {
  ImVec2 p0 = F.p(x, y), p1 = F.p(x + w, y + h);
  dl->AddRectFilled(p0, p1, k.panelLo, 0);
  float big = std::clamp(h * 0.62f * F.s, 14.0f, 34.0f);
  ImGui::PushFont(app().fontMonoBold, big);
  ImVec2 ts = ImGui::CalcTextSize("NOTE");
  dl->AddText(ImVec2(std::round(p0.x + 8 * F.s), std::round((p0.y + p1.y - ts.y) * 0.5f)), skin::lighten(k.panelLo, 0.12f), "NOTE");
  ImGui::PopFont();
  ImGui::PushFont(app().fontMonoBold, F.font(10.0f));
  ImVec2 tt = ImGui::CalcTextSize(title);
  dl->AddText(ImVec2(std::round(p0.x + 14 * F.s + ts.x), std::round(p1.y - tt.y - 6 * F.s)), k.textDim, title);
  ImGui::PopFont();
}

void text(ImDrawList* dl, const skin::Form& F, float x, float y, const skin::Skin& k, const char* s, float px, bool centre, ImU32 col) {
  ImGui::PushFont(app().fontMonoBold, F.font(px));
  ImVec2 ts = ImGui::CalcTextSize(s);
  ImVec2 p = F.p(x, y);
  dl->AddText(ImVec2(std::round(centre ? p.x - ts.x * 0.5f : p.x), std::round(p.y)), col ? col : k.textDim, s);
  ImGui::PopFont();
}

void readout(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, const char* label, float v01) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%s %5.1f%%", label, static_cast<double>(v01) * 100.0);
  skin::lcd(dl, p0, p1, k, buf, std::clamp((p1.y - p0.y) * 0.62f, 9.0f, 12.5f));
}

}  // namespace gl::packs::modulation::sk
