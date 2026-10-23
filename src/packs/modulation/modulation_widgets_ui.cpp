// Gridline — "modulation" pack: shared editor drawing helpers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>
#include "modulation_widgets.h"
#include "../../app/DrawSmoothUi.h"
#include "../../app/PluginSkin.h"
#include "modulation_skin.h"

namespace gl::packs::modulation {

using theme::pal;

float EditTarget::output(int k) const {
  if (!a) return 0;
  return a->builder.controllerOutput(owner, slot, k);
}

EditTarget targetFor(App& a, const PluginTarget& t, float width) {
  EditTarget e;
  e.a = &a;
  e.pid = [t](int k) { return t.pid(k); };
  App* ap = &a;
  e.set = [ap, t](const PluginState& st, const std::string& label, uint64_t key) { setPluginState(*ap, t, st, label, key); };
  e.st = t.state(a.doc.song());
  e.owner = t.owner;
  e.slot = t.slot;
  e.key = t.key();
  e.width = width;
  return e;
}

EditTarget noteFxTarget(App& a, ObjectId channel, int index, float width) {
  EditTarget e;
  e.a = &a;
  e.pid = [channel, index](int k) {
    const ParamId p = noteFxParam(channel, index, k);
    audit::bindTouch(p);  // J-92 binding audit
    return p;
  };
  App* ap = &a;
  e.set = [ap, channel, index](const PluginState& st, const std::string& label, uint64_t key) {
    ap->doc.setNoteFxState(channel, index, st, label, key);
  };
  const Channel* c = a.doc.song().channel(channel);
  e.st = c && index >= 0 && static_cast<size_t>(index) < c->noteFx.size() ? &c->noteFx[static_cast<size_t>(index)] : nullptr;
  e.owner = channel;
  e.slot = -1;
  e.noteFx = index;
  e.key = (static_cast<uint64_t>(channel) << 8) ^ 0x4E4F544546580000ull ^ static_cast<uint64_t>(index + 1);
  e.width = width;
  return e;
}

namespace w {

void caption(const char* text) {
  ImGui::PushFont(app().fontMono, 10.5f);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", text);
  ImGui::PopFont();
}

void knob(const EditTarget& e, int param, const char* label, float cell, float radius) {
  const Song& s = e.a->doc.song();
  ParamId pid = e.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return;
  ImGui::PushID(param);
  ImGui::BeginGroup();
  float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (cell - radius * 2) * 0.5f);
  bool bip = d->min < 0 && d->max > 0;
  if (const skin::Skin* sk = skin::active()) {  // plugin skin: reference-style wheel
    ImVec2 c = ImGui::GetCursorScreenPos();
    sk::wheel(e, param, c, radius * 2, *sk);
    ImGui::SetCursorScreenPos(c);
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
  } else {
    ui::ParamKnob("k", pid, radius, nullptr, bip);
  }
  std::string val = d->format(*getParam(s, pid));
  ImGui::PushFont(e.a->fontMono, 10.5f);
  ImVec2 ts = ImGui::CalcTextSize(val.c_str());
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().text), "%s", val.c_str());
  ImGui::PopFont();
  const char* lbl = label ? label : d->name.c_str();
  ImGui::PushFont(nullptr, 12.0f);
  ts = ImGui::CalcTextSize(lbl);
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().textDim), "%s", lbl);
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
}

void choice(const EditTarget& e, int param, float width, const std::vector<std::string>* labels) {
  const Song& s = e.a->doc.song();
  ParamId pid = e.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return;
  std::vector<std::string> names = labels ? *labels : d->choices;
  if (names.empty() && d->kind == ParamKind::Toggle) names = {"Off", "On"};
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  ImGui::PushID(param);
  float wEach = width > 0 ? (width - static_cast<float>(names.size() - 1) * 2.0f) / static_cast<float>(names.size()) : 0;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) ImGui::SameLine(0, 2);
    if (ui::TextButton(names[i].c_str(), static_cast<int>(i) == cur, wEach)) e.a->doc.setParam(pid, static_cast<double>(i), false);
  }
  ImGui::PopID();
}

void combo(const EditTarget& e, int param, float width) {
  const Song& s = e.a->doc.song();
  ParamId pid = e.pid(param);
  auto d = describeParam(s, pid);
  if (!d || d->choices.empty()) return;
  int cur = static_cast<int>(getParam(s, pid).value_or(0) + 0.5);
  cur = std::clamp(cur, 0, static_cast<int>(d->choices.size()) - 1);
  ImGui::PushID(param);
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo("##c", d->choices[static_cast<size_t>(cur)].c_str())) {
    for (size_t i = 0; i < d->choices.size(); ++i)
      if (ImGui::Selectable(d->choices[i].c_str(), static_cast<int>(i) == cur)) e.a->doc.setParam(pid, static_cast<double>(i), false);
    ImGui::EndCombo();
  }
  ui::Hint(d->name);
  ImGui::PopID();
}

void toggle(const EditTarget& e, int param, const char* label) {
  const Song& s = e.a->doc.song();
  ParamId pid = e.pid(param);
  auto d = describeParam(s, pid);
  if (!d) return;
  bool on = getParam(s, pid).value_or(0) >= 0.5;
  ImGui::PushID(param);
  if (ui::TextButton(label ? label : d->name.c_str(), on)) e.a->doc.setParam(pid, on ? 0 : 1, false);
  ui::Hint(d->name);
  ImGui::PopID();
}

void dragValue(const EditTarget& e, int param, float width, const char* id) {
  const Song& s = e.a->doc.song();
  ParamId pid = e.pid(param);
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return;
  ImGui::PushID(id);
  float f = static_cast<float>(*v);
  ImGui::SetNextItemWidth(width);
  std::string fmt;
  for (char ch : d->format(*v)) fmt += ch == '%' ? std::string("%%") : std::string(1, ch);  // printf-safe
  float speed = static_cast<float>((d->max - d->min) / 200.0);
  if (ImGui::DragFloat("##v", &f, speed, static_cast<float>(d->min), static_cast<float>(d->max), fmt.c_str()))
    e.a->doc.setParam(pid, f, true);
  if (ImGui::IsItemDeactivatedAfterEdit()) e.a->doc.endGesture();
  ui::Hint(d->name + " (drag or double-click to type)");
  ImGui::PopID();
}

namespace {
struct Ring {
  float v[240] = {};
  int head = 0;
  int count = 0;
};
std::map<std::pair<uint64_t, int>, Ring>& rings() {
  static std::map<std::pair<uint64_t, int>, Ring> r;
  return r;
}
}  // namespace

void pushHistory(uint64_t key, int lane, float v) {
  Ring& r = rings()[{key, lane}];
  r.v[r.head] = v;
  r.head = (r.head + 1) % 240;
  r.count = std::min(240, r.count + 1);
}

void drawHistory(ImDrawList* dl, ImVec2 p0, ImVec2 size, uint64_t key, int lane, ImU32 col, float thickness) {
  auto it = rings().find({key, lane});
  if (it == rings().end() || it->second.count < 2) return;
  const Ring& r = it->second;
  ImVec2 pts[240];
  int n = r.count;
  for (int i = 0; i < n; ++i) {
    int idx = (r.head - n + i + 240) % 240;
    float x = p0.x + size.x * static_cast<float>(i + 240 - n) / 239.0f;
    float y = p0.y + size.y - std::clamp(r.v[idx], 0.0f, 1.0f) * size.y;
    pts[i] = ImVec2(x, y);
  }
  dl->AddPolyline(pts, n, col, 0, thickness);
}

void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  dl->AddRectFilled(p0, p1, pal().surface950, 6);
  dl->AddRect(p0, p1, pal().line, 6);
}

int keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 size, int lo, int hi, const std::function<ImU32(int)>& tint) {
  const auto& P = pal();
  static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  int whites = 0;
  for (int k = lo; k <= hi; ++k) whites += black[k % 12] ? 0 : 1;
  if (whites == 0) return -1;
  float ww = size.x / static_cast<float>(whites);
  ImGuiIO& io = ImGui::GetIO();
  int hover = -1;
  bool inside = io.MousePos.x >= p0.x && io.MousePos.x < p0.x + size.x && io.MousePos.y >= p0.y && io.MousePos.y < p0.y + size.y;
  // white keys
  int wi = 0;
  std::vector<std::pair<int, float>> whiteX;
  for (int k = lo; k <= hi; ++k) {
    if (black[k % 12]) continue;
    float x = p0.x + wi * ww;
    whiteX.push_back({k, x});
    ImU32 c = P.paper;
    if (ImU32 t = tint ? tint(k) : 0) c = theme::mix(c, t, 0.55f);
    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + ww - 1, p0.y + size.y), c, 2);
    if (k % 12 == 0) {
      ImGui::PushFont(app().fontMono, 9.0f);
      dl->AddText(ImVec2(x + 2, p0.y + size.y - 12), P.ink, noteName(k).c_str());
      ImGui::PopFont();
    }
    if (inside && io.MousePos.x >= x && io.MousePos.x < x + ww) hover = k;
    ++wi;
  }
  // black keys on top
  float bw = ww * 0.62f, bh = size.y * 0.6f;
  int prevWhite = -1;
  float prevX = p0.x;
  for (int k = lo; k <= hi; ++k) {
    if (!black[k % 12]) {
      prevWhite = k;
      for (const auto& wx : whiteX)
        if (wx.first == k) prevX = wx.second;
      continue;
    }
    if (prevWhite < 0) continue;
    float x = prevX + ww - bw * 0.5f;
    ImU32 c = P.ink;
    if (ImU32 t = tint ? tint(k) : 0) c = theme::mix(c, t, 0.6f);
    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + bw, p0.y + bh), c, 2);
    if (inside && io.MousePos.x >= x && io.MousePos.x < x + bw && io.MousePos.y < p0.y + bh) hover = k;
  }
  return hover;
}

void barLane(const EditTarget& e, const char* id, int first, int count, int stride, ImVec2 size, bool bipolar, int highlight,
             const char* const* labels) {
  const Song& s = e.a->doc.song();
  const auto& P = pal();
  ImGui::PushID(id);
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##lane", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  well(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y));
  float bw = size.x / static_cast<float>(count);
  float mid = p0.y + size.y * 0.5f;
  if (bipolar) dl->AddLine(ImVec2(p0.x, mid), ImVec2(p0.x + size.x, mid), P.line);
  for (int i = 0; i < count; ++i) {
    ParamId pid = e.pid(first + i * stride);
    auto d = describeParam(s, pid);
    auto v = getParam(s, pid);
    if (!d || !v) continue;
    float n = static_cast<float>(d->toNormalized(*v));
    float x0 = p0.x + i * bw + 2, x1 = p0.x + (i + 1) * bw - 2;
    float y = p0.y + 3 + (size.y - 6) * (1.0f - n);
    const skin::Skin* sk = skin::active();
    ImU32 col = i == highlight ? (sk ? skin::lighten(sk->fill, 0.35f) : P.accent) : (sk ? sk->fill : P.text);
    if (bipolar)
      dl->AddRectFilled(ImVec2(x0, std::min(y, mid)), ImVec2(x1, std::max(y, mid) + 1), theme::withAlpha(col, 0.85f), 2);
    else
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, p0.y + size.y - 3), theme::withAlpha(col, 0.85f), 2);
    if (labels && labels[i]) {
      ImGui::PushFont(app().fontMono, 9.5f);
      dl->AddText(ImVec2(x0 + 1, p0.y + 2), P.textFaint, labels[i]);
      ImGui::PopFont();
    }
  }
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActivated()) e.a->doc.beginGroup("draw values");
  // J-99: fast strokes fill every step between samples. Step lanes stay raw
  // (no smoothing): each step is a deliberate value (octave, gate, velocity).
  {
    std::vector<float> cur(static_cast<size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
      ParamId pid = e.pid(first + i * stride);
      auto d = describeParam(s, pid);
      auto v = getParam(s, pid);
      if (d && v) cur[static_cast<size_t>(i)] = static_cast<float>(d->toNormalized(*v));
    }
    drawui::Spec spec;
    spec.surface = "steps";
    spec.raw = true;
    const float n = std::clamp(1.0f - (io.MousePos.y - p0.y - 3) / (size.y - 6), 0.0f, 1.0f);
    if (const std::vector<float>* res = drawui::stroke(*e.a, ImGui::GetItemID(), ImGui::IsItemActive() && !io.KeyCtrl,
                                                       (io.MousePos.x - p0.x) / size.x, n, cur.data(), count, spec)) {
      const std::vector<uint8_t>& touched = drawui::strokeTouched();
      for (int i = 0; i < count; ++i) {
        if (!touched[static_cast<size_t>(i)]) continue;
        ParamId pid = e.pid(first + i * stride);
        auto d = describeParam(s, pid);
        if (!d) continue;
        const double v = d->fromNormalized((*res)[static_cast<size_t>(i)]);
        auto old = getParam(s, pid);
        if (!old || *old != v) e.a->doc.setParam(pid, v, true);
      }
    }
  }
  if (ImGui::IsItemActive() && io.KeyCtrl) {
    int i = std::clamp(static_cast<int>((io.MousePos.x - p0.x) / bw), 0, count - 1);
    ParamId pid = e.pid(first + i * stride);
    if (auto d = describeParam(s, pid)) e.a->doc.setParam(pid, d->def, true);
  }
  if (ImGui::IsItemDeactivated()) {
    e.a->doc.endGroup();
    e.a->doc.endGesture();
  }
  if (ImGui::IsItemHovered()) {
    int i = std::clamp(static_cast<int>((io.MousePos.x - p0.x) / bw), 0, count - 1);
    ParamId pid = e.pid(first + i * stride);
    auto d = describeParam(s, pid);
    auto v = getParam(s, pid);
    if (d && v) e.a->setHint(d->name + ": " + d->format(*v) + "   (drag to draw, Ctrl = reset)");
  }
  ImGui::PopID();
}

std::string noteName(int n) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  n = std::clamp(n, 0, 127);
  return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

}  // namespace w

}  // namespace gl::packs::modulation
