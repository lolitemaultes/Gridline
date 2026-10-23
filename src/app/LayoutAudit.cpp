// Gridline — editor layout audit collection (Q-7), see LayoutAudit.h.
#include "LayoutAudit.h"

namespace gl::audit {

namespace {
bool active = false;
Report cur;
inset::Rect R(ImVec2 a, ImVec2 b) { return inset::Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)}; }
}  // namespace

bool on() { return active; }

void begin() {
  cur = Report{};
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
}

void lastItem(const char* what) {
  if (!active) return;
  control(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), what);
}

Report end(float min) {
  active = false;
  Report r = std::move(cur);
  cur = Report{};
  r.issues = inset::check(r.frames, r.controls, min);
  return r;
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
