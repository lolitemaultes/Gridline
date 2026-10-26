// J-48: interface sounds — loader, real-time mixing (no allocation), muting
// rules, cool-down, settings round trip, shipped assets. J-133: sound themes
// (fallback chain, every theme complete, level and loudness per theme), bank
// hand-over while the audio thread runs, previews.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <memory>
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
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
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
// Loudness of the loudest 100 ms (ITU-R BS.1770 K-weighting at 48 kHz, LUFS
// scale): the felt-loudness measure the themes are levelled to (J-133).
double hitLoudness(const Clip& c) {
  const double b1[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285}, a1[2] = {-1.69065929318241, 0.73248077421585};
  const double b2[3] = {1.0, -2.0, 1.0}, a2[2] = {-1.99004745483398, 0.99007225036621};
  const size_t n = c.frames() + 19200;  // + 0.4 s so the filter tail counts
  std::vector<double> pw(n, 0.0);
  for (int ch = 0; ch < 2; ++ch) {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, v1 = 0, v2 = 0;
    for (size_t i = 0; i < n; ++i) {
      const double x = i < c.frames() ? c.lr[2 * i + static_cast<size_t>(ch)] : 0.0;
      const double y = b1[0] * x + b1[1] * x1 + b1[2] * x2 - a1[0] * y1 - a1[1] * y2;
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      const double v = b2[0] * y + b2[1] * u1 + b2[2] * u2 - a2[0] * v1 - a2[1] * v2;
      u2 = u1, u1 = y, v2 = v1, v1 = v;
      pw[i] += v * v;
    }
  }
  const size_t w = 4800;
  double acc = 0, best = 0;
  for (size_t i = 0; i < n; ++i) {
    acc += pw[i];
    if (i >= w) acc -= pw[i - w];
    best = std::max(best, acc / static_cast<double>(w));
  }
  return -0.691 + 10.0 * std::log10(std::max(best, 1e-20));
}
void touchWav(const fs::path& p, float amp) {
  fs::create_directories(p.parent_path());
  AudioBuffer b(2, 4800);
  for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = amp * std::sin(0.05f * static_cast<float>(i));
  writeWav(p.string(), b, 48000, 24);
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

  // ---- J-133: theme table and fallback chain
  {
    expect(kThemeCount == 5 && std::string(themeId(0)) == "graphite" && std::string(kDefaultTheme) == "graphite", "graphite first, default");
    expect(themeIndex("prism") == 1 && themeIndex("classic") == 4 && themeIndex("nope") == -1, "theme index");
    expect(resolveTheme("tine") == "tine" && resolveTheme("") == "graphite" && resolveTheme("Glass") == "graphite",
           "unknown or missing theme resolves to graphite");
    expect(fallbackChain("prism") == std::vector<std::string>{"prism", "graphite", "classic"}, "chain: theme, graphite, classic");
    expect(fallbackChain("bogus") == std::vector<std::string>{"graphite", "classic"}, "chain for an unknown theme");
    expect(fallbackChain("classic") == std::vector<std::string>{"classic", "graphite"}, "chain for classic");
    // A sparse sounds folder: each event comes from the first place that has it, none goes silent by accident.
    const fs::path dir = tmp / "sounds";
    touchWav(dir / "prism" / "error.wav", 0.3f);
    touchWav(dir / "graphite" / "startup.wav", 0.3f);
    touchWav(dir / "graphite" / "error.wav", 0.1f);
    touchWav(dir / "classic" / "save.wav", 0.3f);
    touchWav(dir / "open.wav", 0.3f);  // older flat layout
    auto b = loadTheme(dir.string(), "prism", 48000);
    expect(b && b->theme == "prism" && b->rate == 48000, "bank names its theme and rate");
    expect(b->from[static_cast<size_t>(Event::Error)] == "prism" && b->from[static_cast<size_t>(Event::Startup)] == "graphite" &&
               b->from[static_cast<size_t>(Event::Save)] == "classic" && b->from[static_cast<size_t>(Event::Open)] == "default",
           "per-event fallback: theme, graphite, classic, flat folder");
    expect(b->clips[static_cast<size_t>(Event::Export)].lr.empty() && b->from[static_cast<size_t>(Event::Export)].empty(),
           "nothing anywhere: that event alone has no sound");
    auto u = loadTheme(dir.string(), "unknown-theme", 48000);
    expect(u->theme == "graphite" && u->from[static_cast<size_t>(Event::Error)] == "graphite", "unknown theme loads graphite");
    Clip pc;
    expect(loadThemeClip(dir.string(), "tine", Event::Save, 48000, pc) && pc.frames() == 4800, "preview clip with fallback");
  }

  // ---- J-133: every shipped theme is complete, clean, under -1 dBFS and on the loudness ladder
  {
    std::array<std::array<double, kEventCount>, kThemeCount> loud{};
    bool complete = true, clean = true;
    for (int t = 0; t < kThemeCount; ++t) {
      auto b = loadTheme("assets/sounds", themeId(t), 48000);
      for (int i = 0; i < kEventCount; ++i) {
        const Clip& c = b->clips[static_cast<size_t>(i)];
        complete = complete && b->from[static_cast<size_t>(i)] == themeId(t);
        if (c.lr.empty()) {
          clean = false;
          continue;
        }
        const double secs = static_cast<double>(c.frames()) / 48000.0;
        const float pk = peak(c.lr);
        const bool ok = secs > 0.1 && secs <= (i == 0 ? 3.0 : 1.0) && pk > 0.01f && pk <= 0.8913f &&  // -1 dBFS
                        std::fabs(c.lr[0]) < 1e-3f && std::fabs(c.lr[c.lr.size() - 1]) < 1e-3f;
        if (!ok) std::printf("  %s/%s: %.2f s, peak %.3f\n", themeId(t), eventId(static_cast<Event>(i)), secs, static_cast<double>(pk));
        clean = clean && ok;
        loud[static_cast<size_t>(t)][static_cast<size_t>(i)] = hitLoudness(c);
      }
    }
    expect(complete, "every theme has all ten events in its own folder");
    expect(clean, "shipped sounds: length, peak at or under -1 dBFS, silent edges");
    // Felt loudness (loudest 100 ms) per event agrees across themes within 1 LU (targets +-0.5),
    // and save stays the quietest event of every theme.
    double spread = 0;
    bool saveQuietest = true;
    for (int i = 0; i < kEventCount; ++i) {
      double lo = 1e9, hi = -1e9;
      for (int t = 0; t < kThemeCount; ++t) {
        lo = std::min(lo, loud[static_cast<size_t>(t)][static_cast<size_t>(i)]);
        hi = std::max(hi, loud[static_cast<size_t>(t)][static_cast<size_t>(i)]);
        if (i != static_cast<int>(Event::Save))
          saveQuietest = saveQuietest && loud[static_cast<size_t>(t)][static_cast<size_t>(Event::Save)] < loud[static_cast<size_t>(t)][static_cast<size_t>(i)];
      }
      spread = std::max(spread, hi - lo);
    }
    std::printf("  loudness spread between themes: %.3f LU\n", spread);
    expect(spread <= 1.0, "same felt loudness per event across themes (within 1 LU)");
    expect(saveQuietest, "save is the quietest event in every theme");
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
    // J-133: sound.theme
    Settings d;
    expect(d.getString(kThemeKey) == "graphite", "sound.theme defaults to graphite");
    expect(d.set(kThemeKey, std::string("tine")), "sound.theme accepts a theme");
    const std::string q = (tmp / "settings2.json").string();
    expect(d.save(q), "settings save (theme)");
    Settings e;
    expect(e.load(q) && e.getString(kThemeKey) == "tine" && resolveTheme(e.getString(kThemeKey)) == "tine", "sound.theme round trip");
    expect(e.set(kThemeKey, std::string("retired-theme")) && resolveTheme(e.getString(kThemeKey)) == "graphite",
           "a stale theme name falls back to graphite");
  }

  // ---- J-133: switching banks while sounds play (audio thread keeps running, no allocation in mix)
  {
    Player pl;
    pl.setVolume(1.0f);
    auto a = std::make_unique<Bank>();
    a->clips[static_cast<size_t>(Event::Save)] = tone(2000, 0.5f);
    pl.install(std::move(a));
    std::vector<float> buf(256 * 2, 0.0f);
    expect(pl.trigger(Event::Save), "bank A: trigger");
    pl.mix(buf.data(), 256);
    expect(pl.activeVoices() == 1, "bank A: playing");
    auto b = std::make_unique<Bank>();
    b->clips[static_cast<size_t>(Event::Save)] = tone(300, 0.25f);
    pl.install(std::move(b));  // while A's sound is still playing
    expect(pl.clip(Event::Save).frames() == 300, "UI sees bank B at once");
    expect(pl.trigger(Event::Save), "bank B: trigger");
    gAllocs = 0;
    gCount = true;
    pl.mix(buf.data(), 256);
    gCount = false;
    expect(gAllocs.load() == 0, "adopting a bank does not allocate");
    expect(pl.activeVoices() == 2, "A finishes while B starts");
    expect(pl.collect() == 0, "bank A is not handed back while it sounds");
    for (int k = 0; k < 10; ++k) pl.mix(buf.data(), 256);
    expect(pl.activeVoices() == 0 && pl.collect() == 1, "bank A handed back and freed once its sound ended");
    // rapid switches: banks never adopted are freed by the UI thread itself
    for (int k = 0; k < 20; ++k) pl.setClip(Event::Open, tone(100 + static_cast<size_t>(k)));
    pl.mix(buf.data(), 256);
    pl.collect();
    expect(pl.clip(Event::Open).frames() == 119, "latest of rapid switches wins");
    // preview of another theme's clip
    std::vector<float> o(512 * 2, 0.0f);
    expect(pl.preview(tone(400, 0.5f)), "preview queued");
    pl.mix(o.data(), 512);
    expect(peak(o) > 0.4f, "preview plays");
    for (int k = 0; k < 4; ++k) pl.mix(o.data(), 512);
    expect(pl.activeVoices() == 0, "preview ends");
  }

  std::error_code ec;
  fs::remove_all(tmp, ec);
  return finish();
}
