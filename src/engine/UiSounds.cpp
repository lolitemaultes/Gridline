#include "UiSounds.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "Sample.h"

namespace gl::uisound {

namespace {
struct Info {
  const char* id;
  const char* label;
  bool on;
};
constexpr Info kInfo[kEventCount] = {
    {"startup", "Startup", true},
    {"open", "Project opened", true},
    {"save", "Project saved", true},
    {"export", "Export finished", true},
    {"import", "Import finished", true},
    {"task_done", "Background task finished", true},
    {"warning", "Needs attention", false},
    {"error", "Error", true},
    {"record_start", "Recording started", false},
    {"record_stop", "Recording stopped", false},
};
constexpr int kStealFade = 96;  // samples (2 ms at 48 kHz) to fade a stolen voice
}  // namespace

const char* eventId(Event e) { return kInfo[static_cast<int>(e)].id; }
const char* eventLabel(Event e) { return kInfo[static_cast<int>(e)].label; }
bool defaultOn(Event e) { return kInfo[static_cast<int>(e)].on; }
std::string settingKey(Event e) { return std::string("sounds.") + eventId(e); }

bool shouldPlay(Event e, const Context& c) {
  if (!c.enabled || !c.eventEnabled) return false;
  if (e == Event::Error) return true;
  if (e == Event::RecordStart || e == Event::RecordStop) return !c.audioInputRecording;
  return !c.transportPlaying && !c.recording;
}

bool loadClip(const std::string& path, double rate, Clip& out, std::string* err) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (err) *err = "missing " + path;
    return false;
  }
  SamplePtr s = loadSample(path, err);
  if (!s || s->frames() == 0 || s->sampleRate <= 0 || rate <= 0) return false;
  const std::vector<float>& L = s->left;
  const std::vector<float>& R = s->right.vec();
  const double step = s->sampleRate / rate;
  const size_t n = static_cast<size_t>(std::floor(static_cast<double>(s->frames() - 1) / step)) + 1;
  Clip c;
  c.lr.resize(n * 2);
  for (size_t i = 0; i < n; ++i) {
    const double p = static_cast<double>(i) * step;
    const size_t k = static_cast<size_t>(p);
    const size_t k1 = std::min(k + 1, s->frames() - 1);
    const float f = static_cast<float>(p - static_cast<double>(k));
    c.lr[2 * i] = L[k] + (L[k1] - L[k]) * f;
    c.lr[2 * i + 1] = R[k] + (R[k1] - R[k]) * f;
  }
  out = std::move(c);
  return true;
}

int Player::loadAll(const std::string& dir, double rate) {
  int n = 0;
  for (int i = 0; i < kEventCount; ++i) {
    Clip c;
    if (loadClip((std::filesystem::path(dir) / (std::string(kInfo[i].id) + ".wav")).string(), rate, c)) ++n;
    setClip(static_cast<Event>(i), std::move(c));
  }
  return n;
}

void Player::setClip(Event e, Clip clip) {
  // The device is closed or paused (contract): drop voices that point at the old clip.
  for (auto& v : voices_)
    if (v.clip == &clips_[static_cast<size_t>(e)]) v = Voice{};
  clips_[static_cast<size_t>(e)] = std::move(clip);
}

bool Player::trigger(Event e, float gain) {
  if (!hasClip(e)) return false;
  const unsigned h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= static_cast<unsigned>(kQueue)) return false;
  queue_[h % kQueue] = Request{static_cast<int>(e), gain};
  head_.store(h + 1, std::memory_order_release);
  return true;
}

int Player::activeVoices() const noexcept {
  int n = 0;
  for (const auto& v : voices_) n += v.clip != nullptr;
  return n;
}

void Player::mix(float* out, int frames) noexcept {
  // New requests: a free voice, else steal the one nearest its end (short fade).
  unsigned t = tail_.load(std::memory_order_relaxed);
  const unsigned h = head_.load(std::memory_order_acquire);
  for (; t != h; ++t) {
    const Request r = queue_[t % kQueue];
    const Clip* c = &clips_[static_cast<size_t>(r.event)];
    if (c->lr.empty()) continue;
    Voice* slot = nullptr;
    for (auto& v : voices_)
      if (!v.clip) {
        slot = &v;
        break;
      }
    if (!slot) {  // all busy: fade out the voice nearest its end, then start this one in its slot
      Voice* old = nullptr;
      for (auto& v : voices_)
        if (!v.pending && (!old || v.pos * old->clip->frames() > old->pos * v.clip->frames())) old = &v;
      if (!old) continue;  // every slot already hands over: drop
      if (old->fade == 0) old->fade = kStealFade;
      old->pending = c;
      old->pendingGain = r.gain;
      continue;
    }
    *slot = Voice{c, 0, r.gain, 0, nullptr, 0};
  }
  tail_.store(t, std::memory_order_release);

  const float vol = volume_.load(std::memory_order_relaxed);
  if (vol <= 0) {  // still advance, so muted sounds do not resume later
    for (auto& v : voices_)
      if (v.clip && (v.pos += static_cast<size_t>(frames)) >= v.clip->frames()) v = Voice{};
    return;
  }
  for (auto& v : voices_) {
    if (!v.clip) continue;
    const float* src = v.clip->lr.data();
    const size_t len = v.clip->frames();
    const float g = v.gain * vol;
    for (int i = 0; i < frames && v.pos < len; ++i, ++v.pos) {
      float gi = g;
      if (v.fade > 0) {
        gi *= static_cast<float>(v.fade) / kStealFade;
        if (--v.fade == 0) {
          v.pos = len;
          break;
        }
      }
      out[2 * i] += src[2 * v.pos] * gi;
      out[2 * i + 1] += src[2 * v.pos + 1] * gi;
    }
    if (v.pos >= len) v = v.pending ? Voice{v.pending, 0, v.pendingGain, 0, nullptr, 0} : Voice{};
  }
}

bool Gate::allow(Event e, double now) {
  double& l = last_[static_cast<size_t>(e)];
  if (now - l < cooldown_) return false;
  l = now;
  return true;
}

}  // namespace gl::uisound
