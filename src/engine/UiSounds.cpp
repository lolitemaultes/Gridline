#include "UiSounds.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "Resample.h"
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
struct ThemeInfo {
  const char* id;
  const char* label;
  const char* desc;
};
constexpr ThemeInfo kThemes[kThemeCount] = {
    {"graphite", "Graphite", "Warm wood: a tuned bar under a felt mallet, close and dry"},
    {"prism", "Prism", "Struck crystal with a short sparkle and a small bright hall"},
    {"tine", "Tine", "Electric-piano tine: a warm bell with a gentle bark"},
    {"signal", "Signal", "Precise electronics: clean tones that snap into place"},
    {"classic", "Classic", "The original Gridline glass sounds"},
};
constexpr int kStealFade = 96;  // samples (2 ms at 48 kHz) to fade a stolen voice
const Clip kNoClip{};
}  // namespace

const char* eventId(Event e) { return kInfo[static_cast<int>(e)].id; }
const char* eventLabel(Event e) { return kInfo[static_cast<int>(e)].label; }
bool defaultOn(Event e) { return kInfo[static_cast<int>(e)].on; }
std::string settingKey(Event e) { return std::string("sounds.") + eventId(e); }

const char* themeId(int i) { return kThemes[std::clamp(i, 0, kThemeCount - 1)].id; }
const char* themeLabel(int i) { return kThemes[std::clamp(i, 0, kThemeCount - 1)].label; }
const char* themeDescription(int i) { return kThemes[std::clamp(i, 0, kThemeCount - 1)].desc; }
int themeIndex(const std::string& id) {
  for (int i = 0; i < kThemeCount; ++i)
    if (id == kThemes[i].id) return i;
  return -1;
}
std::string resolveTheme(const std::string& id) { return themeIndex(id) >= 0 ? id : std::string(kDefaultTheme); }
std::vector<std::string> fallbackChain(const std::string& id) {
  std::vector<std::string> v{resolveTheme(id)};
  for (const char* t : {kDefaultTheme, kFallbackTheme})
    if (std::find(v.begin(), v.end(), t) == v.end()) v.emplace_back(t);
  return v;
}

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
  std::vector<float> l, r;
  if (std::fabs(s->sampleRate - rate) < 1e-6) {
    l = L;
    r = R;
  } else {  // J-133: windowed sinc, so bright sounds keep their top and gain no aliases
    l = resampleChannel(L.data(), s->frames(), s->sampleRate, rate);
    r = resampleChannel(R.data(), s->frames(), s->sampleRate, rate);
  }
  const size_t n = std::min(l.size(), r.size());
  if (n == 0) return false;
  Clip c;
  c.lr.resize(n * 2);
  for (size_t i = 0; i < n; ++i) {
    c.lr[2 * i] = l[i];
    c.lr[2 * i + 1] = r[i];
  }
  out = std::move(c);
  return true;
}

bool loadThemeClip(const std::string& soundsDir, const std::string& theme, Event e, double rate, Clip& out) {
  const std::filesystem::path dir(soundsDir);
  const std::string file = std::string(eventId(e)) + ".wav";
  for (const std::string& t : fallbackChain(theme))
    if (loadClip((dir / t / file).string(), rate, out)) return true;
  return loadClip((dir / file).string(), rate, out);
}

std::unique_ptr<Bank> loadTheme(const std::string& soundsDir, const std::string& theme, double rate) {
  auto b = std::make_unique<Bank>();
  b->theme = resolveTheme(theme);
  b->rate = rate;
  const std::filesystem::path dir(soundsDir);
  const std::vector<std::string> chain = fallbackChain(theme);
  for (int i = 0; i < kEventCount; ++i) {
    const std::string file = std::string(kInfo[i].id) + ".wav";
    bool ok = false;
    for (const std::string& t : chain)
      if (loadClip((dir / t / file).string(), rate, b->clips[static_cast<size_t>(i)])) {
        b->from[static_cast<size_t>(i)] = t;
        ok = true;
        break;
      }
    if (!ok && loadClip((dir / file).string(), rate, b->clips[static_cast<size_t>(i)])) b->from[static_cast<size_t>(i)] = "default";
  }
  return b;
}

// ---------------------------------------------------------------- Player ---
Player::~Player() = default;  // owned_ holds every bank the audio thread could still name

void Player::submit(Lane& lane, std::unique_ptr<Bank> bank) {
  Bank* raw = bank.get();
  owned_.push_back(std::move(bank));
  if (Bank* old = lane.pending.exchange(raw, std::memory_order_acq_rel)) {  // never adopted: free now
    owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const std::unique_ptr<Bank>& p) { return p.get() == old; }),
                 owned_.end());
  }
}

void Player::install(std::unique_ptr<Bank> bank) {
  if (!bank) bank = std::make_unique<Bank>();
  bank_ = bank.get();
  submit(theme_, std::move(bank));
}

int Player::loadAll(const std::string& dir, double rate) {
  auto b = std::make_unique<Bank>();
  int n = 0;
  for (int i = 0; i < kEventCount; ++i)
    if (loadClip((std::filesystem::path(dir) / (std::string(kInfo[i].id) + ".wav")).string(), rate, b->clips[static_cast<size_t>(i)])) {
      b->from[static_cast<size_t>(i)] = "default";
      ++n;
    }
  install(std::move(b));
  return n;
}

void Player::setClip(Event e, Clip clip) {
  auto b = bank_ ? std::make_unique<Bank>(*bank_) : std::make_unique<Bank>();
  b->clips[static_cast<size_t>(e)] = std::move(clip);
  install(std::move(b));
}

const Clip& Player::clip(Event e) const { return bank_ ? bank_->clips[static_cast<size_t>(e)] : kNoClip; }

bool Player::trigger(Event e, float gain) {
  if (!hasClip(e)) return false;
  const unsigned h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= static_cast<unsigned>(kQueue)) return false;
  queue_[h % kQueue] = Request{static_cast<int>(e), gain};
  head_.store(h + 1, std::memory_order_release);
  return true;
}

bool Player::preview(Clip clip, float gain) {
  if (clip.lr.empty()) return false;
  const unsigned h = head_.load(std::memory_order_relaxed);
  if (h - tail_.load(std::memory_order_acquire) >= static_cast<unsigned>(kQueue)) return false;
  auto b = std::make_unique<Bank>();
  b->clips[0] = std::move(clip);
  submit(preview_, std::move(b));
  queue_[h % kQueue] = Request{kEventCount, gain};
  head_.store(h + 1, std::memory_order_release);
  return true;
}

int Player::collect() {
  int n = 0;
  unsigned t = ringTail_.load(std::memory_order_relaxed);
  const unsigned h = ringHead_.load(std::memory_order_acquire);
  for (; t != h; ++t, ++n) {
    Bank* b = ring_[t % kRing];
    owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const std::unique_ptr<Bank>& p) { return p.get() == b; }),
                 owned_.end());
  }
  ringTail_.store(t, std::memory_order_release);
  return n;
}

void Player::adopt(Lane& lane) noexcept {
  if (!lane.pending.load(std::memory_order_acquire)) return;
  Bank** slot = nullptr;
  if (lane.active) {
    for (auto& r : retiring_)
      if (!r) {
        slot = &r;
        break;
      }
    if (!slot) return;  // the old banks are still sounding: adopt on a later block
  }
  Bank* p = lane.pending.exchange(nullptr, std::memory_order_acq_rel);
  if (!p) return;
  if (slot) *slot = lane.active;
  lane.active = p;
}

void Player::retire() noexcept {
  for (auto& r : retiring_) {
    if (!r) continue;
    bool used = false;
    for (const auto& v : voices_) used = used || v.owner == r || v.pendingOwner == r;
    if (used) continue;
    const unsigned h = ringHead_.load(std::memory_order_relaxed);
    if (h - ringTail_.load(std::memory_order_acquire) >= static_cast<unsigned>(kRing)) return;  // UI has not collected yet
    ring_[h % kRing] = r;
    ringHead_.store(h + 1, std::memory_order_release);
    r = nullptr;
  }
}

int Player::activeVoices() const noexcept {
  int n = 0;
  for (const auto& v : voices_) n += v.clip != nullptr;
  return n;
}

void Player::mix(float* out, int frames) noexcept {
  adopt(theme_);
  adopt(preview_);
  // New requests: a free voice, else steal the one nearest its end (short fade).
  unsigned t = tail_.load(std::memory_order_relaxed);
  const unsigned h = head_.load(std::memory_order_acquire);
  for (; t != h; ++t) {
    const Request r = queue_[t % kQueue];
    const Bank* bank = r.event == kEventCount ? preview_.active : theme_.active;
    if (!bank) continue;
    const Clip* c = &bank->clips[r.event == kEventCount ? 0 : static_cast<size_t>(r.event)];
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
      old->pendingOwner = bank;
      old->pendingGain = r.gain;
      continue;
    }
    *slot = Voice{c, bank, 0, r.gain, 0, nullptr, nullptr, 0};
  }
  tail_.store(t, std::memory_order_release);

  const float vol = volume_.load(std::memory_order_relaxed);
  if (vol <= 0) {  // still advance, so muted sounds do not resume later
    for (auto& v : voices_)
      if (v.clip && (v.pos += static_cast<size_t>(frames)) >= v.clip->frames()) v = Voice{};
    retire();
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
    if (v.pos >= len) v = v.pending ? Voice{v.pending, v.pendingOwner, 0, v.pendingGain, 0, nullptr, nullptr, 0} : Voice{};
  }
  retire();
}

bool Gate::allow(Event e, double now) {
  double& l = last_[static_cast<size_t>(e)];
  if (now - l < cooldown_) return false;
  l = now;
  return true;
}

}  // namespace gl::uisound
