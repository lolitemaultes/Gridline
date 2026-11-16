// Gridline — K-218: sound library scanning. Tempo, key and sound kind heard in the audio; the
// browser index caching them and filtering on them; the known library folders, offered once.
// Every folder here is a fixture under the temporary folder: nothing personal is looked at.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/SoundAnalysis.h"
#include "core/SoundLibraries.h"
#include "core/WavWriter.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

fs::path dir() {
  static const fs::path d = [] {
    fs::path p = fs::temp_directory_path() / "gridline_test_k218";
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
  }();
  return d;
}

std::vector<float> silence(double sec) { return std::vector<float>(static_cast<size_t>(sec * kSr), 0.0f); }

void addKick(std::vector<float>& v, size_t at, float gain = 0.9f) {
  double ph = 0;
  for (size_t i = 0; i < static_cast<size_t>(0.35 * kSr) && at + i < v.size(); ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double f = 50 + 100 * std::exp(-t / 0.03);
    ph += 2 * kPi * f / kSr;
    v[at + i] += gain * static_cast<float>(std::sin(ph) * std::exp(-t / 0.08));
  }
}
void addNoise(std::vector<float>& v, size_t at, double decay, double lp, float gain, unsigned seed, bool hp = false) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1, 1);
  float y = 0, prev = 0;
  const float k = static_cast<float>(1 - std::exp(-2 * kPi * lp / kSr));
  for (size_t i = 0; i < static_cast<size_t>(6 * decay * kSr) && at + i < v.size(); ++i) {
    const float n = u(rng);
    y += k * (n - y);
    const float s = hp ? n - prev : y;
    prev = n;
    v[at + i] += gain * s * static_cast<float>(std::exp(-static_cast<double>(i) / kSr / decay));
  }
}
void addTone(std::vector<float>& v, size_t at, double sec, double f, float gain, double vibrato = 0) {
  double ph = 0;
  for (size_t i = 0; i < static_cast<size_t>(sec * kSr) && at + i < v.size(); ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double fi = f * std::pow(2.0, vibrato * std::sin(2 * kPi * 5.5 * t) / 12.0);
    ph += 2 * kPi * fi / kSr;
    const double env = std::min(1.0, t / 0.01) * std::min(1.0, (sec - t) / 0.02);
    v[at + i] += gain * static_cast<float>(env * (std::sin(ph) + 0.3 * std::sin(2 * ph)));
  }
}
double mtof(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

// Two bars of a beat at `bpm`: kicks on the beats, a snare on two and four, hats on the eighths.
std::vector<float> drumLoop(double bpm, int bars = 2) {
  const double beat = 60.0 / bpm;
  std::vector<float> v = silence(bars * 4 * beat);
  for (int b = 0; b < bars * 4; ++b) {
    const size_t at = static_cast<size_t>(b * beat * kSr);
    addKick(v, at);
    if (b % 2) addNoise(v, at, 0.06, 2500, 0.5f, 7u + static_cast<unsigned>(b));
    addNoise(v, at + static_cast<size_t>(beat / 2 * kSr), 0.02, 0, 0.25f, 100u + static_cast<unsigned>(b), true);
  }
  return v;
}

// A chord progression in a key: each chord a bar of `beat` long notes.
std::vector<float> chords(const std::vector<std::vector<int>>& prog, double bpm = 100) {
  const double beat = 60.0 / bpm;
  std::vector<float> v = silence(static_cast<double>(prog.size()) * 4 * beat);
  for (size_t c = 0; c < prog.size(); ++c)
    for (int b = 0; b < 4; ++b)
      for (int m : prog[c]) addTone(v, static_cast<size_t>((static_cast<double>(c) * 4 + b) * beat * kSr), beat * 0.9, mtof(m), 0.12f);
  return v;
}

std::string wav(const std::string& name, const std::vector<float>& v) {
  AudioBuffer b(2, v.size());
  for (size_t i = 0; i < v.size(); ++i) b.at(0, i) = v[i], b.at(1, i) = v[i];
  const fs::path p = dir() / "lib" / name;
  fs::create_directories(p.parent_path());
  writeWav(p.string(), b, static_cast<int>(kSr), 16);
  return p.string();
}

void testTempo() {
  for (double bpm : {90.0, 120.0, 128.0, 140.0, 174.0}) {
    const auto v = drumLoop(bpm, 4);
    double conf = 0;
    const double got = sound::detectTempo(v, kSr, 0, &conf);
    std::printf("# tempo %.0f -> %.2f (confidence %.2f)\n", bpm, got, conf);
    expect(std::fabs(got - bpm) < 1.0, "tempo: a beat's tempo is heard");
  }
  std::vector<float> one = silence(2.5);
  addKick(one, 0);
  expect(sound::detectTempo(one, kSr, 0) == 0, "tempo: a single hit has none");
  expect(sound::detectTempo(silence(4), kSr, 0) == 0, "tempo: silence has none");
}

void testKey() {
  int pc = -1;
  bool minor = false;
  // C major: C, F, G, C.
  expect(sound::detectKey(chords({{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}}), kSr, pc, minor) && pc == 0 && !minor,
         "key: C major");
  // A minor: Am, Dm, E, Am.
  expect(sound::detectKey(chords({{57, 60, 64}, {62, 65, 69}, {64, 68, 71}, {57, 60, 64}}), kSr, pc, minor) && pc == 9 && minor, "key: A minor");
  // F# major, to check the transposition.
  expect(sound::detectKey(chords({{66, 70, 73}, {71, 75, 78}, {73, 77, 80}, {66, 70, 73}}), kSr, pc, minor) && pc == 6 && !minor,
         "key: F sharp major");
  std::vector<float> noise = silence(3);
  addNoise(noise, 0, 10, 0, 0.3f, 3, true);
  expect(!sound::detectKey(noise, kSr, pc, minor), "key: noise has none");
}

void testKind() {
  std::vector<float> k = silence(0.6);
  addKick(k, 0);
  std::vector<float> h = silence(0.3);
  addNoise(h, 0, 0.02, 0, 0.6f, 5, true);
  std::vector<float> cy = silence(2.5);
  addNoise(cy, 0, 0.6, 0, 0.6f, 6, true);
  std::vector<float> sn = silence(0.6);
  addNoise(sn, 0, 0.07, 7000, 0.8f, 9);  // a snare: bright wires over a short low ring
  addTone(sn, 0, 0.04, 190, 0.15f);
  std::vector<float> bass = silence(1.2);
  addTone(bass, 0, 1.0, 55, 0.5f);
  std::vector<float> pad = silence(2.0);
  addTone(pad, 0, 1.9, 440, 0.4f);
  std::vector<float> voice = silence(4.0);
  for (int s = 0; s < 6; ++s) addTone(voice, static_cast<size_t>((0.1 + s * 0.62) * kSr), 0.45, mtof(57 + (s % 3) * 2), 0.4f, 0.4);
  const std::pair<const char*, const std::vector<float>*> cases[] = {{"kick", &k},  {"hat", &h},   {"cymbal", &cy}, {"snare", &sn},
                                                                    {"bass", &bass}, {"tonal", &pad}, {"vocal", &voice}};
  for (const auto& [want, sig] : cases) {
    const std::string got = sound::detectKind(*sig, kSr);
    std::printf("# kind %s -> %s\n", want, got.c_str());
    expect(got == want, (std::string("sound kind: ") + want).c_str());
  }
  expect(sound::detectKind(drumLoop(120), kSr) == "drums", "sound kind: a beat is drums");
  expect(sound::detectKind(silence(1), kSr).empty(), "sound kind: silence is nothing");
  const sound::Analysis a = sound::analyse(drumLoop(120), kSr);
  expect(a.kind == "drums" && std::fabs(a.bpm - 120) < 0.5 && a.key == -1, "analyse: a beat has a tempo and no key");
}

void testIndex() {
  wav("untitled 1.wav", drumLoop(128));  // nothing in the name
  wav("take 7.wav", chords({{57, 60, 64}, {62, 65, 69}, {64, 68, 71}, {57, 60, 64}}));
  std::vector<float> k = silence(0.6);
  addKick(k, 0);
  wav("hit 3.wav", k);
  wav("groove 95bpm.wav", drumLoop(128));  // the name wins over the audio
  const std::string cache = (dir() / "index.json").string();
  {
    browse::Index ix;
    ix.setCacheFile(cache);
    ix.setRoots({{(dir() / "lib").string(), true}});
    ix.start();
    ix.waitIdle();
    browse::FileMeta m;
    expect(ix.lookup((dir() / "lib" / "untitled 1.wav").string(), m) && m.sound == "drums" && std::fabs(m.bpm - 128) < 0.5 && m.bpmHeard,
           "index: an untagged loop's tempo and kind, heard");
    expect(std::fabs(m.beats() - 8) < 0.05, "index: its length in beats");
    expect(ix.lookup((dir() / "lib" / "take 7.wav").string(), m) && m.key == 9 && m.minor && m.keyHeard, "index: a key heard");
    expect(ix.lookup((dir() / "lib" / "hit 3.wav").string(), m) && m.sound == "kick" && m.bpm == 0, "index: a one-shot kick, no tempo");
    expect(ix.lookup((dir() / "lib" / "groove 95bpm.wav").string(), m) && m.bpm == 95 && !m.bpmHeard, "index: a tempo in the name wins");
    expect(ix.search(browse::parseQuery("sound:kick")).size() == 1, "filter: sound kind");
    expect(ix.search(browse::parseQuery("tag:drums")).size() == 2, "filter: the sound kind is a tag too");
    expect(ix.search(browse::parseQuery("scale:minor")).size() == 1, "filter: scale without a key");
    expect(ix.search(browse::parseQuery("beats:8")).size() == 1, "filter: length in beats");
    expect(ix.search(browse::parseQuery("bpm:120-130")).size() == 1, "filter: tempo heard");
    bool chip = false;
    for (const auto& [t, n] : ix.tagCounts()) chip = chip || (t == "drums" && n == 2);
    expect(chip, "filter chips: the sound kinds are offered");
    ix.stop();
  }
  // The cache keeps it: nothing is decoded again.
  browse::Index again;
  again.setCacheFile(cache);
  expect(again.load(), "cache: loads");
  browse::FileMeta m;
  expect(again.lookup((dir() / "lib" / "untitled 1.wav").string(), m) && m.sound == "drums" && m.bpmHeard && m.analysed, "cache: kind and tempo kept");
}

void testLibraries() {
  const fs::path sys = dir() / "sys", home = dir() / "home", cfg = dir() / "cfg";
#if defined(__APPLE__)
  const fs::path lib = sys / "Library/Audio/Apple Loops";
  const fs::path mine = home / "Library/Audio/Apple Loops";
#elif defined(_WIN32)
  const fs::path lib = sys / "Users/Public/Music/Samples";
  const fs::path mine = sys / "Users/Public/Documents/Samples";
#else
  const fs::path lib = sys / "usr/share/sounds/samples";
  const fs::path mine = home / ".local/share/gridline/sounds";
#endif
  fs::create_directories(home);
  expect(soundlib::candidates(home.string(), sys.string()).empty(), "libraries: none where none exist");
  fs::create_directories(lib);
  fs::create_directories(mine);
  const auto c = soundlib::candidates(home.string(), sys.string());
  expect(c.size() == 2, "libraries: the known folders that exist");
  for (const auto& l : c) {
    std::printf("# library %s (%s)\n", l.path.c_str(), l.name.c_str());
    expect(l.path.find("gridline_test_k218") != std::string::npos && !l.name.empty(), "libraries: only under the fixture roots");
  }
  // Offered once.
  auto offer = soundlib::toOffer(cfg.string(), home.string(), sys.string());
  expect(offer.size() == 2, "libraries: offered");
  soundlib::answer(cfg.string(), offer, false);
  expect(soundlib::toOffer(cfg.string(), home.string(), sys.string()).empty(), "libraries: a no is not asked again");
  fs::remove(cfg / "sound-libraries.json");
  {
    FILE* f = std::fopen((cfg / "browser-folders.txt").string().c_str(), "wb");
    std::fprintf(f, "%s\n", c[0].path.c_str());
    std::fclose(f);
  }
  offer = soundlib::toOffer(cfg.string(), home.string(), sys.string());
  expect(offer.size() == 1 && offer[0].path == c[1].path, "libraries: a folder in the browser already is not offered");
  soundlib::answer(cfg.string(), offer, true);
  const soundlib::Answer a = soundlib::loadAnswer(cfg.string());
  expect(a.asked && a.added.size() == 1 && a.added[0] == c[1].path, "libraries: a yes is remembered");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k218_library");
  testTempo();
  testKey();
  testKind();
  testIndex();
  testLibraries();
  return finish();
}
