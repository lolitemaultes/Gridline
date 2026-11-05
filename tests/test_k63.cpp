// Gridline: K-63, the level of notes on an FL sampler channel. FL plays them at (11^x - 1) / 10 for
// channel volume x, times 0.61 at velocity 100; the import sets the sampler's volume so Gridline's
// strip gives that, and keeps x on the channel. Audio clip channels and Gridline's own samplers keep
// their level. Every file here is synthetic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/core/flp/FlpExport.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kPpq = 96;
constexpr int kRate = 44100;

struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, kPpq, 0, 'F', 'L', 'd', 't'};
    const uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

std::vector<uint8_t> levels(uint32_t volume) {
  std::vector<uint8_t> v(24, 0);
  const uint32_t pan = 6400;
  for (int i = 0; i < 4; ++i) {
    v[static_cast<size_t>(i)] = static_cast<uint8_t>(pan >> (8 * i));
    v[static_cast<size_t>(4 + i)] = static_cast<uint8_t>(volume >> (8 * i));
  }
  v[13] = 1;  // the filter's mod X at 256: fully open
  return v;
}

void channel(Builder& B, int index, uint8_t type, const std::string& sample, uint32_t volume) {
  B.w(64, static_cast<uint16_t>(index)), B.b(21, type), B.text(203, "Channel");
  B.text(196, sample);
  B.data(219, levels(volume));
}

bool load(const std::string& sample, uint8_t type, uint32_t volume, Song& s) {
  Builder B;
  channel(B, 0, type, sample, volume);
  B.w(99, 0);
  for (int ins = 0; ins < 2; ++ins) {
    B.data(236, std::vector<uint8_t>(12, 0));
    for (int k = 0; k < 10; ++k) B.w(98, static_cast<uint16_t>(k));
    B.d(147, 0);
  }
  flp::Report rep;
  std::string err;
  const bool ok = importFlpBytes(B.file(), s, rep, &err);
  for (auto& t : s.mixer) t.muted = false;  // the synthetic inserts carry no enable flag
  return ok;
}

std::string writeTone() {
  const size_t n = static_cast<size_t>(1.0 * kRate);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i) b.at(0, i) = b.at(1, i) = 0.5f * static_cast<float>(std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / kRate));
  const std::string path = (fs::temp_directory_path() / "k63_tone.wav").string();
  writeWav(path, b, kRate, 24);
  sampleCache().clear();
  return path;
}

// Peak of one note (velocity 100, the sample's root key), away from its start and end.
double notePeak(Song& s) {
  Channel& c = s.channels[0];
  Pattern p;
  p.id = 1000;
  p.name = "k63";
  p.notes[c.id].push_back(Note(0, 2 * kPpq, 60, 100));
  s.patterns.push_back(p);
  Clip cl;
  cl.id = 2000;
  cl.pattern = p.id;
  cl.start = 0;
  cl.length = 4 * kPpq;
  s.playlist.push_back(cl);
  RenderOptions o;
  o.sampleRate = kRate;
  const AudioBuffer a = renderSong(s, o);
  double pk = 0;
  for (size_t i = kRate / 20; i < std::min<size_t>(a.frames, kRate / 4); ++i) pk = std::max(pk, static_cast<double>(std::fabs(a.at(0, i))));
  return pk;
}

double flLevel(double x) { return (std::pow(11.0, x) - 1.0) / 10.0 * (100.0 / 128.0) * (100.0 / 128.0); }

}  // namespace

int main() {
  const std::string tone = writeTone();
  // A low, a usual and a high volume, the default, and full.
  for (uint32_t raw : {3500u, 3650u, 9100u, 10000u, 10250u, 12100u, 12800u}) {
    const double x = raw / 12800.0;
    Song s;
    expect(load(tone, 0, raw, s) && !s.channels.empty(), ("sampler channel imports at " + std::to_string(raw)).c_str());
    if (s.channels.empty()) continue;
    expect(std::fabs(s.channels[0].gain - x) < 1e-5, ("the channel keeps FL's volume at " + std::to_string(raw)).c_str());
    const double want = 0.5 * flLevel(x), got = notePeak(s);
    char line[120];
    std::snprintf(line, sizeof line, "note level at volume %u: got %.4f, want %.4f", raw, got, want);
    expect(std::fabs(20 * std::log10(std::max(got, 1e-9) / want)) < 0.1, line);
  }
  // Very low volumes stay valid and never go louder than FL's level.
  expect(flp::flSamplerVolume(0.0) == 0.8, "a silent channel keeps the default volume");
  for (double x : {0.001, 0.01, 0.05, 0.1}) expect(std::isfinite(flp::flSamplerVolume(x)) && flp::flSamplerVolume(x) <= 1.0, "low volumes stay in range");
  // An audio clip channel plays its clips through the strip: its sampler volume stays.
  {
    Song s;
    expect(load(tone, 4, 10000, s) && !s.channels.empty(), "audio clip channel imports");
    if (!s.channels.empty()) expect(std::fabs(s.channels[0].plugin.params[pp::SmpVolume] - 0.8f) < 1e-5f, "an audio clip channel keeps the sampler volume");
  }
  // Gridline's own samplers keep their sound.
  {
    const PluginState st = defaultPluginState("gl.sampler");
    expect(std::fabs(st.params[pp::SmpVolume] - 0.8f) < 1e-5f, "a new sampler keeps its default volume");
  }
  // A project written from Gridline's own song comes back at Gridline's level.
  {
    Song s;
    Channel c;
    c.id = s.allocateId();
    c.name = "Native";
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = tone;
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    const flp::ExportOptions o;
    std::vector<uint8_t> bytes;
    flp::ExportReport er;
    expect(flp::exportFlp(s, bytes, er, o), "a native song exports");
    Song back;
    flp::Report rep;
    expect(importFlpBytes(bytes, back, rep, nullptr) && !back.channels.empty(), "the native export imports");
    if (!back.channels.empty()) expect(std::fabs(back.channels[0].plugin.params[pp::SmpVolume] - 0.8f) < 1e-5f, "a native export keeps Gridline's sampler volume");
    // A project that came from FL goes out as FL's, and comes back at FL's level.
    Song fl;
    expect(load(tone, 0, 10000, fl), "an FL project imports");
    std::vector<uint8_t> again;
    flp::ExportReport er2;
    expect(flp::exportFlp(fl, again, er2, o), "the FL project exports");
    Song back2;
    flp::Report rep2;
    expect(importFlpBytes(again, back2, rep2, nullptr) && !back2.channels.empty(), "the FL project re-imports");
    if (!back2.channels.empty()) expect(std::fabs(back2.channels[0].plugin.params[pp::SmpVolume] - static_cast<float>(flp::flSamplerVolume(10000.0 / 12800.0))) < 1e-4f, "an FL project keeps FL's level through Gridline");
  }
  // The mark needs a build number; FL's three part versions have none.
  expect(flp::flNativeExportBuild("25.2.5.5319") == 5318 && flp::flNativeExportBuild("20.0.1") == 0, "the mark reads the build number only");
  return finish();
}
