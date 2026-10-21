// Gridline - "Keep this change?" state (J-77), see DisplayConfirm.h.
#include "DisplayConfirm.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../core/I18n.h"
#include "../core/Settings.h"

namespace gl::dispconf {

void Confirm::request(Change c, double now) {
  if (pending_ && cur_.id == c.id) {
    if (c.after == cur_.before) {  // back where it started: nothing to confirm
      if (cur_.mode == Mode::ApplyFirst && c.apply) c.apply();
      finish(Result::Reverted);
      return;
    }
    if (cur_.mode == Mode::ApplyFirst && c.apply) c.apply();
    cur_.after = c.after;
    cur_.afterValue = c.afterValue;
    cur_.toSnap = std::move(c.toSnap);
    cur_.apply = std::move(c.apply);  // revert / before stay the first change's
    opened_ = now;
    ++generation_;
    return;
  }
  if (pending_) finish(cur_.mode == Mode::ApplyFirst ? Result::Kept : Result::Reverted);
  if (c.before == c.after) return;
  cur_ = std::move(c);
  pending_ = true;
  opened_ = now;
  last_ = Result::None;
  ++generation_;
  if (cur_.mode == Mode::ApplyFirst && cur_.apply) cur_.apply();
}

void Confirm::finish(Result r) {
  pending_ = false;
  last_ = r;
  cur_ = Change{};
}

void Confirm::keep() {
  if (!pending_) return;
  if (cur_.mode == Mode::PreviewFirst && cur_.apply) cur_.apply();
  finish(Result::Kept);
}

void Confirm::revert() {
  if (!pending_) return;
  if (cur_.mode == Mode::ApplyFirst && cur_.revert) cur_.revert();
  finish(Result::Reverted);
}

void Confirm::abandon() {
  if (!pending_) return;
  if (cur_.mode == Mode::ApplyFirst) {
    if (cur_.revertQuiet)
      cur_.revertQuiet();
    else if (cur_.revert)
      cur_.revert();
  }
  finish(Result::Reverted);
}

Result Confirm::tick(double now) {
  if (!pending_ || !autoRevert || now - opened_ < timeout) return Result::None;
  if (cur_.mode == Mode::ApplyFirst && cur_.revert) cur_.revert();
  finish(Result::TimedOut);
  return Result::TimedOut;
}

double Confirm::remaining(double now) const {
  if (!autoRevert) return timeout;
  return std::max(0.0, timeout - (now - opened_));
}

Confirm& confirm() {
  static Confirm c;
  return c;
}

// ---- snapshots

bool Snapshot::operator==(const Snapshot& o) const {
  if (vals.size() != o.vals.size()) return false;
  for (size_t i = 0; i < vals.size(); ++i) {
    const Val &a = vals[i], &b = o.vals[i];
    if (a.key.key != b.key.key || a.key.kind != b.key.kind) return false;
    if (a.key.kind == 'b' && a.b != b.b) return false;
    if (a.key.kind == 'n' && std::fabs(a.n - b.n) > 1e-9) return false;
    if (a.key.kind == 's' && a.s != b.s) return false;
  }
  return true;
}

Snapshot take(const Settings& st, const std::vector<Key>& keys) {
  Snapshot s;
  for (const Key& k : keys) {
    Snapshot::Val v;
    v.key = k;
    if (k.kind == 'b') v.b = st.getBool(k.key);
    else if (k.kind == 'n') v.n = st.getNumber(k.key);
    else v.s = st.getString(k.key);
    s.vals.push_back(std::move(v));
  }
  return s;
}

void restore(Settings& st, const Snapshot& snap) {
  for (const auto& v : snap.vals) {
    if (v.key.kind == 'b') st.set(v.key.key, v.b);
    else if (v.key.kind == 'n') st.set(v.key.key, v.n);
    else st.set(v.key.key, v.s);
  }
}

const std::vector<Key>& themeKeys() {
  static const std::vector<Key> k = {{"theme.mode", 's'},   {"ui.darkTheme", 'b'},     {"theme.accent", 's'},     {"theme.hot", 's'},
                                     {"theme.led", 's'},    {"theme.playhead", 's'},   {"theme.contrast", 'n'},   {"theme.brightness", 'n'}};
  return k;
}

const std::vector<Key>& audioKeys() {
  static const std::vector<Key> k = {{"audio.sampleRate", 'n'}, {"audio.bufferFrames", 'n'}};
  return k;
}

Change snapshotChange(Settings& st, std::string id, std::string title, std::string label, std::string before, std::string after,
                      Snapshot from, Snapshot to, Preview preview, std::function<void()> onApplied) {
  Change c;
  c.id = std::move(id);
  c.title = std::move(title);
  c.label = std::move(label);
  c.before = std::move(before);
  c.after = std::move(after);
  if (from != to && c.before == c.after) c.after += " *";  // different settings that read alike still confirm
  c.mode = Mode::ApplyFirst;
  c.preview = preview;
  c.fromSnap = from;
  c.toSnap = to;
  Settings* s = &st;
  c.apply = [s, to, onApplied]() {
    restore(*s, to);
    if (onApplied) onApplied();
  };
  c.revert = [s, from, onApplied]() {
    restore(*s, from);
    if (onApplied) onApplied();
  };
  c.revertQuiet = [s, from]() { restore(*s, from); };
  return c;
}

std::string scaleText(double scale) {
  char b[16];
  std::snprintf(b, sizeof b, "%.0f%%", std::round(scale * 100.0));
  return b;
}

Change scaleChange(Settings& st, double to) {
  to = std::clamp(to, 0.75, 2.0);
  Change c;
  c.id = "ui.scale";
  c.title = i18n::N_("Use this interface size?");
  c.label = i18n::N_("Interface size");
  c.beforeValue = st.getNumber("ui.scale");
  c.afterValue = to;
  c.before = scaleText(c.beforeValue);
  c.after = scaleText(to);
  c.mode = Mode::PreviewFirst;
  c.preview = Preview::Scale;
  Settings* s = &st;
  c.apply = [s, to]() { s->set("ui.scale", to); };
  return c;
}

bool DeferredValue::release(double committed, double* out) {
  if (!editing) return false;
  editing = false;
  if (std::fabs(value - committed) < 1e-6) return false;
  if (out) *out = value;
  return true;
}

}  // namespace gl::dispconf
