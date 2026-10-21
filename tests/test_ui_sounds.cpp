// J-48: interface sounds — loader, real-time mixing (no allocation), muting
// rules, cool-down, settings round trip, shipped assets.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "core/AudioBuffer.h"
#include "core/Settings.h"
#include "core/WavWriter.h"
#include "engine/UiSounds.h"

// Count heap allocations while a flag is set (the audio-thread path must not allocate).
static std::atomic<bool> gCount{false};
static std::atomic<int> gAllocs{0};
void* operator new(std::size_t n) {
  if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::uisound;

namespace {
Clip tone(size_t frames, float amp = 0.5f) {
  Clip c;
  c.lr.resize(frames * 2);
  for (size_t i = 0; i < frames; ++i) c.lr[2 * i] = c.lr[2 * i + 1] = amp * std::sin(0.05f * static_cast<float>(i));
  return c;
}
float peak(const std::vector<float>& v) {
  float p = 0;
  for (float x : v) p = std::fmax(p, std::fabs(x));
  return p;
}
}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "gridline_test_ui_sounds";
  fs::create_directories(tmp);

  // ---- event table
  expect(std::string(eventId(Event::Startup)) == "startup" && std::string(eventId(Event::RecordStop)) == "record_stop", "event ids");
  expect(settingKey(Event::TaskDone) == "sounds.task_done", "setting key");
  expect(defaultOn(Event::Save) && defaultOn(Event::Error) && !defaultOn(Event::Warning) && !defaultOn(Event::RecordStart),
         "defaults: warning and recording off");

  // ---- loader: 44.1 kHz mono-compatible stereo WAV, resampled to 48 kHz
  {
    AudioBuffer b(2, 4410);
    for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = 0.25f;
    const std::string p = (tmp / "t.wav").string();
    expect(writeWav(p, b, 44100, 24), "write test wav");
    Clip c;
    expect(loadClip(p, 48000, c), "loadClip decodes");
    expect(std::abs(static_cast<long>(c.frames()) - 4800) <= 1, "loadClip resamples 0.1 s to 48 kHz");
    expect(std::fabs(c.lr[100] - 0.25f) < 1e-3f && std::fabs(c.lr[101] - 0.25f) < 1e-3f, "loadClip keeps the level");
    Clip keep = tone(10);
    expect(!loadClip((tmp / "missing.wav").string(), 48000, keep) && keep.frames() == 10, "missing file: false, clip untouched");
  }

  // ---- shipped placeholders load and are sane
  {
    Player pl;
    const int n = pl.loadAll("assets/sounds", 48000);
    expect(n == kEventCount, "every event has a shipped sound");
    bool ok = true;
    for (int i = 0; i < kEventCount; ++i) {
      const Clip& c = pl.clip(static_cast<Event>(i));
      const double secs = static_cast<double>(c.frames()) / 48000.0;
      const float pk = peak(c.lr);
      ok = ok && secs > 0.1 && secs <= (i == 0 ? 3.0 : 1.0) && pk > 0.01f && pk < 0.9f;
      ok = ok && std::fabs(c.lr[0]) < 1e-3f && std::fabs(c.lr[c.lr.size() - 1]) < 1e-3f;  // clean edges
    }
    expect(ok, "shipped sounds: length, level under -1 dBFS, silent edges");
  }

  // ---- mixing: adds on top of the output, ends, no allocation on the audio path
  {
    Player pl;
    pl.setClip(Event::Save, tone(1000));
    pl.setClip(Event::Error, tone(300));
    pl.setVolume(1.0f);
    std::vector<float> buf(512 * 2, 0.1f);
    gAllocs = 0;
    gCount = true;
    pl.mix(buf.data(), 512);  // nothing queued: unchanged
    gCount = false;
    expect(std::fabs(buf[10] - 0.1f) < 1e-7f, "idle mix leaves the output alone");
    expect(pl.trigger(Event::Save) && pl.trigger(Event::Error), "trigger queues");
    expect(!pl.trigger(Event::Open), "event without a clip is not queued");
    gAllocs = 0;
    gCount = true;
    for (int k = 0; k < 4; ++k) pl.mix(buf.data(), 512);
    gCount = false;
    expect(gAllocs.load() == 0, "mix does not allocate");
    expect(pl.activeVoices() == 0, "voices end at the clip end");
    // level: a single voice at volume 0.5 adds half the clip
    std::vector<float> out(64 * 2, 0.0f);
    pl.setVolume(0.5f);
    pl.trigger(Event::Save);
    pl.mix(out.data(), 64);
    const Clip& c = pl.clip(Event::Save);
    expect(std::fabs(out[40] - 0.5f * c.lr[40]) < 1e-6f, "volume scales the sound");
    // overload: more requests than voices, still no allocation and bounded output
    pl.setVolume(1.0f);
    for (int k = 0; k < Player::kQueue + 4; ++k) pl.trigger(Event::Save);
    std::vector<float> o2(256 * 2, 0.0f);
    gAllocs = 0;
    gCount = true;
    for (int k = 0; k < 8; ++k) pl.mix(o2.data(), 256);
    gCount = false;
    expect(gAllocs.load() == 0 && pl.activeVoices() <= Player::kVoices, "burst: no allocation, voice cap kept");
    // muted volume: sounds advance silently and do not resume later
    Player m;
    m.setClip(Event::Save, tone(200));
    m.setVolume(0.0f);
    m.trigger(Event::Save);
    std::vector<float> z(256 * 2, 0.0f);
    m.mix(z.data(), 256);
    expect(peak(z) == 0.0f && m.activeVoices() == 0, "volume 0: silent and consumed");
  }

  // ---- muting rules
  {
    Context idle;
    expect(shouldPlay(Event::Save, idle), "idle: save plays");
    Context off = idle;
    off.enabled = false;
    expect(!shouldPlay(Event::Error, off), "master switch off: nothing, not even errors");
    Context evOff = idle;
    evOff.eventEnabled = false;
    expect(!shouldPlay(Event::Export, evOff), "per-event switch");
    Context playing = idle;
    playing.transportPlaying = true;
    expect(!shouldPlay(Event::Save, playing) && !shouldPlay(Event::Export, playing) && !shouldPlay(Event::Startup, playing),
           "transport playing: quiet");
    expect(shouldPlay(Event::Error, playing), "transport playing: errors still play");
    Context rec = playing;
    rec.recording = true;
    expect(shouldPlay(Event::RecordStart, rec) && shouldPlay(Event::RecordStop, rec), "record start / stop pass while recording");
    rec.audioInputRecording = true;
    expect(!shouldPlay(Event::RecordStart, rec) && !shouldPlay(Event::RecordStop, rec), "never over a live audio input");
    expect(shouldPlay(Event::Error, rec), "errors even while an input records");
    Gate g(0.25);
    expect(g.allow(Event::Save, 10.0) && !g.allow(Event::Save, 10.1) && g.allow(Event::Error, 10.1) && g.allow(Event::Save, 10.3),
           "cool-down per event");
  }

  // ---- settings: defaults, save, load
  {
    Settings s;
    expect(s.getBool("sounds.enabled") && std::fabs(s.getNumber("sounds.volume") - 0.7) < 1e-9, "settings defaults");
    bool match = true;
    for (int i = 0; i < kEventCount; ++i) match = match && s.getBool(settingKey(static_cast<Event>(i))) == defaultOn(static_cast<Event>(i));
    expect(match, "per-event setting defaults match the event table");
    expect(s.set("sounds.enabled", false) && s.set("sounds.volume", 0.35) && s.set("sounds.save", false) && s.set("sounds.warning", true),
           "settings accept the keys");
    const std::string p = (tmp / "settings.json").string();
    expect(s.save(p), "settings save");
    Settings t;
    expect(t.load(p), "settings load");
    expect(!t.getBool("sounds.enabled") && std::fabs(t.getNumber("sounds.volume") - 0.35) < 1e-9 && !t.getBool("sounds.save") &&
               t.getBool("sounds.warning") && t.getBool("sounds.export"),
           "settings round trip");
  }

  std::error_code ec;
  fs::remove_all(tmp, ec);
  return finish();
}
