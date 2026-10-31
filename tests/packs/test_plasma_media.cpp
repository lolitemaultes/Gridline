// Gridline — "plasma" pack tests: audio embedded in Serum presets. Synthetic
// Serum 2 (.SerumPreset: CBOR in a zstd frame) and Serum 1 (.fxp: two zlib
// streams) files written by this test carry a wavetable, a sample, a
// multisample map, a noise, an impulse response and a microtuning; the import
// keeps them in the Plasma state (Model::media), extracts them for playback,
// survives a save / load round trip, reports a missing external file, maps the
// levels on the source's law and still reads old states. No Xfer content is
// used or shipped.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../../src/core/Json.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

using namespace gl;
namespace fs = std::filesystem;
namespace pl = gl::packs::plasma;

namespace {
constexpr double kSr = 48000;

// ------------------------------------------------------------------ Serum 2 container writer
struct Cbor {
  std::vector<uint8_t> b;
  void head(int mt, uint64_t v) {
    if (v < 24) b.push_back(static_cast<uint8_t>(mt << 5 | v));
    else if (v < 256) b.push_back(static_cast<uint8_t>(mt << 5 | 24)), b.push_back(static_cast<uint8_t>(v));
    else if (v < 65536) b.push_back(static_cast<uint8_t>(mt << 5 | 25)), b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v));
    else {
      b.push_back(static_cast<uint8_t>(mt << 5 | 26));
      for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
    }
  }
  void str(const std::string& s) {
    head(3, s.size());
    b.insert(b.end(), s.begin(), s.end());
  }
  void num(double d) {
    if (d == std::floor(d) && std::fabs(d) < 1e9) {
      if (d >= 0) head(0, static_cast<uint64_t>(d));
      else head(1, static_cast<uint64_t>(-d - 1));
      return;
    }
    // audio arrays are single-precision floats, like the presets store them
    const float f = static_cast<float>(d);
    if (static_cast<double>(f) == d) {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      b.push_back(0xFA);
      for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(u >> s));
      return;
    }
    b.push_back(0xFB);
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(u >> s));
  }
};
void cborJson(Cbor& c, const Json& j) {
  switch (j.type) {
    case Json::Type::Object:
      c.head(5, j.object.size());
      for (const auto& kv : j.object) {
        c.str(kv.first);
        cborJson(c, kv.second);
      }
      break;
    case Json::Type::Array:
      c.head(4, j.array.size());
      for (const auto& v : j.array) cborJson(c, v);
      break;
    case Json::Type::String: c.str(j.str); break;
    case Json::Type::Number: c.num(j.number); break;
    case Json::Type::Bool: c.b.push_back(j.boolean ? 0xF5 : 0xF4); break;
    default: c.b.push_back(0xF6); break;
  }
}
std::vector<uint8_t> zstdRaw(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> f = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};
  const uint32_t n = static_cast<uint32_t>(data.size());
  for (int s = 0; s < 32; s += 8) f.push_back(static_cast<uint8_t>(n >> s));
  size_t pos = 0;
  do {
    const size_t len = std::min<size_t>(data.size() - pos, 100000);
    const uint32_t hdr = (static_cast<uint32_t>(len) << 3) | (pos + len >= data.size() ? 1u : 0u);
    f.push_back(static_cast<uint8_t>(hdr));
    f.push_back(static_cast<uint8_t>(hdr >> 8));
    f.push_back(static_cast<uint8_t>(hdr >> 16));
    f.insert(f.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  } while (pos < data.size());
  return f;
}
std::vector<uint8_t> container(const Json& body, const std::string& name) {
  Cbor c;
  cborJson(c, body);
  const std::string meta = "{\"fileType\":\"SerumPreset\",\"presetName\":\"" + name + "\",\"product\":\"Serum2\",\"version\":1}";
  std::vector<uint8_t> out = {'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
  const uint64_t ml = meta.size();
  for (int s = 0; s < 64; s += 8) out.push_back(static_cast<uint8_t>(ml >> s));
  out.insert(out.end(), meta.begin(), meta.end());
  const uint32_t un = static_cast<uint32_t>(c.b.size());
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(un >> s));
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(2u >> s));
  const auto z = zstdRaw(c.b);
  out.insert(out.end(), z.begin(), z.end());
  return out;
}
Json obj(std::initializer_list<std::pair<const char*, Json>> kv) {
  Json j = Json::makeObject();
  for (const auto& p : kv) j.object[p.first] = p.second;
  return j;
}
Json num(double d) { return Json::makeNumber(d); }
Json str(const std::string& s) { return Json::makeString(s); }
Json arr(const std::vector<Json>& v) {
  Json j = Json::makeArray();
  j.array = v;
  return j;
}
Json floats(const std::vector<float>& v) {
  Json j = Json::makeArray();
  for (float f : v) j.array.push_back(num(static_cast<double>(f)));
  return j;
}
// Test signals: a sine of `cycles` periods over n samples at amplitude a.
std::vector<float> sine(size_t n, double cycles, float a) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = a * static_cast<float>(std::sin(6.283185307179586 * cycles * static_cast<double>(i) / static_cast<double>(n)));
  return v;
}

// ------------------------------------------------------------------ Serum 1 .fxp writer
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t len = std::min<size_t>(data.size() - pos, 65535);
    z.push_back(pos + len >= data.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(len));
    z.push_back(static_cast<uint8_t>(len >> 8));
    z.push_back(static_cast<uint8_t>(~len));
    z.push_back(static_cast<uint8_t>(~len >> 8));
    z.insert(z.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  } while (pos < data.size());
  uint32_t a = 1, b = 0;
  for (uint8_t v : data) a = (a + v) % 65521, b = (b + a) % 65521;
  const uint32_t ad = b << 16 | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(ad >> s));
  return z;
}
// State (parameters at 0x3460 / 0x4AE0, embedded byte sizes of tables A and B at
// 0x4968 / 0x496C), then the audio stream (A, B, noise), then the first stream's length.
std::vector<uint8_t> serum1Fxp(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>& noise) {
  std::vector<uint8_t> st(20704, 0);
  auto put = [&](int i, float v) {
    const size_t at = i < 228 ? 0x3460 + 4 * static_cast<size_t>(i) : 0x4AE0 + 4 * static_cast<size_t>(i - 228);
    std::memcpy(st.data() + at, &v, 4);
  };
  for (int i = 0; i < 299; ++i) put(i, 0.0f);
  put(1, 0.6f);
  for (int i = 0; i < 10; ++i) {
    const uint32_t p = i == 9 ? 0 : static_cast<uint32_t>(i + 1);
    std::memcpy(st.data() + 0x3BE0 + 4 * static_cast<size_t>(i), &p, 4);
  }
  const uint32_t na = static_cast<uint32_t>(a.size() * 4), nb = static_cast<uint32_t>(b.size() * 4);
  std::memcpy(st.data() + 0x4968, &na, 4);
  std::memcpy(st.data() + 0x496C, &nb, 4);
  std::memcpy(st.data() + 0x4972, "Embedded S1", 11);
  std::vector<uint8_t> audio;
  for (const auto* v : {&a, &b, &noise}) {
    const auto* p = reinterpret_cast<const uint8_t*>(v->data());
    audio.insert(audio.end(), p, p + v->size() * 4);
  }
  std::vector<uint8_t> chunk = zlibStored(st);
  const uint32_t first = static_cast<uint32_t>(chunk.size());
  const auto z2 = zlibStored(audio);
  chunk.insert(chunk.end(), z2.begin(), z2.end());
  for (int s = 0; s < 32; s += 8) chunk.push_back(static_cast<uint8_t>(first >> s));
  std::vector<uint8_t> f = {'C', 'c', 'n', 'K', 0, 0, 0, 0, 'F', 'P', 'C', 'h', 0, 0, 0, 1, 'X', 'f', 's', 'X', 0, 0, 0, 1, 0, 0, 0, 1};
  f.resize(0x38, 0);
  const uint32_t n = static_cast<uint32_t>(chunk.size());
  for (int s = 24; s >= 0; s -= 8) f.push_back(static_cast<uint8_t>(n >> s));
  f.insert(f.end(), chunk.begin(), chunk.end());
  const uint32_t total = static_cast<uint32_t>(f.size() - 8);
  for (int k = 0; k < 4; ++k) f[4 + static_cast<size_t>(k)] = static_cast<uint8_t>(total >> (24 - 8 * k));
  return f;
}

// ------------------------------------------------------------------ rendering
float renderPeak(const PluginState& st, int key = 60) {
  auto g = packs::createGenerator(st.type);
  if (!g) return -1;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96;
  std::vector<float> L(256), R(256);
  float peak = 0;
  for (int pos = 0; pos < 24000; pos += 256) {
    if (pos == 0) g->noteOn(1, key, 0.8f, 0);
    g->process(st, aux, ctx, L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) {
      if (!std::isfinite(L[static_cast<size_t>(i)]) || !std::isfinite(R[static_cast<size_t>(i)])) return -1;
      peak = std::max({peak, std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)])});
    }
  }
  return peak;
}
// RMS level in dB of each 10 ms window (480 frames): note on at 0, off at offAt.
std::vector<double> renderWindows(const PluginState& st, float vel, int offAt, int total, int key = 60) {
  std::vector<double> out;
  auto g = packs::createGenerator(st.type);
  if (!g) return out;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96;
  std::vector<float> L(480), R(480);
  for (int pos = 0; pos < total; pos += 480) {
    if (pos == 0) g->noteOn(1, key, vel, 0);
    if (pos == offAt) g->noteOff(1, 0);
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    g->process(st, aux, ctx, L.data(), R.data(), 480);
    double e = 0;
    for (size_t i = 0; i < 480; ++i) e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
    out.push_back(10.0 * std::log10(e / 480.0 + 1e-30));
  }
  return out;
}
// Key 60 held for 0.5 s: the stereo frames.
void renderLR(const PluginState& st, std::vector<float>& L, std::vector<float>& R, int key = 60) {
  const int total = 24000;
  L.assign(static_cast<size_t>(total), 0.f), R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) return;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96;
  for (int pos = 0; pos < total; pos += 480) {
    if (pos == 0) g->noteOn(1, key, 1.f, 0);
    g->process(st, aux, ctx, L.data() + pos, R.data() + pos, 480);
  }
}
// Level (dB) of harmonic h of key 60 (times `up`) in the left channel over 0.2..0.5 s (Hann window).
double harmDb(const std::vector<float>& x, int h, double up = 1.0) {
  const double f = 440.0 * std::pow(2.0, -9.0 / 12.0) * h * up, pi = 3.14159265358979;
  const size_t a = 9600, n = 14400;
  double re = 0, im = 0, wsum = 0;
  for (size_t i = 0; i < n && a + i < x.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * pi * static_cast<double>(i) / static_cast<double>(n));
    re += w * x[a + i] * std::cos(2 * pi * f * static_cast<double>(i) / kSr);
    im -= w * x[a + i] * std::sin(2 * pi * f * static_cast<double>(i) / kSr);
    wsum += w;
  }
  return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}
double meanDb(const std::vector<double>& w, size_t a, size_t b) {
  double e = 0;
  for (size_t i = a; i < b && i < w.size(); ++i) e += std::pow(10.0, w[i] / 10.0);
  return 10.0 * std::log10(e / static_cast<double>(b - a) + 1e-30);
}
bool readWav(const std::string& path, int& rate, int& channels, size_t& frames) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
  uint16_t ch;
  uint32_t sr, data;
  std::memcpy(&ch, b.data() + 22, 2);
  std::memcpy(&sr, b.data() + 24, 4);
  std::memcpy(&data, b.data() + 40, 4);
  rate = static_cast<int>(sr), channels = ch, frames = data / (4u * ch);
  return true;
}
}  // namespace

int main() {
  // extracted media go to a scratch folder of this test
  const fs::path cache =
      fs::temp_directory_path() / ("gl-plasma-media-test-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
  fs::create_directories(cache);
#ifdef _WIN32
  _putenv_s("GRIDLINE_CACHE_DIR", cache.string().c_str());
#else
  setenv("GRIDLINE_CACHE_DIR", cache.string().c_str(), 1);
#endif
  expect(pl::mediaCacheDir() == cache.string() + "/plasma-media", "media cache follows GRIDLINE_CACHE_DIR");

  // ---- media store: dedupe, 16-bit round trip with the peak kept, save / load
  {
    pl::Model m = pl::defaultModel();
    const auto a = sine(1000, 3, 1.6f);  // above full scale: kept through the peak factor
    const std::string r1 = pl::addMedia(m, "a.wav", 44100, {a});
    const std::string r2 = pl::addMedia(m, "again.wav", 44100, {a});
    const std::string r3 = pl::addMedia(m, "st.wav", 32000, {a, sine(1000, 5, 0.5f)});
    expect(pl::isMediaRef(r1) && r1 == r2 && r1 != r3 && m.media.size() == 2, "identical audio is stored once");
    const pl::Media* e = pl::findMedia(m, r3);
    std::vector<std::vector<float>> ch;
    expect(e && e->channels == 2 && e->rate == 32000 && e->frames == 1000 && pl::mediaAudio(*e, ch) && ch.size() == 2, "stereo entry keeps rate and layout");
    float err = 0;
    const pl::Media* e1 = pl::findMedia(m, r1);
    std::vector<std::vector<float>> c1;
    if (e1 && pl::mediaAudio(*e1, c1))
      for (size_t i = 0; i < a.size(); ++i) err = std::max(err, std::fabs(c1[0][i] - a[i]));
    expect(e1 && err < 1e-4f, "audio comes back within 16-bit precision, peak above 1 kept");
    m.osc[1].sample = r1;
    {
      pl::Model once;
      expect(pl::modelFromData(pl::modelToData(m), once) && once.media.size() == 1, "an entry nothing uses is not saved");
    }
    m.noiseFile = r3;
    pl::Model back;
    expect(pl::modelFromData(pl::modelToData(m), back) && back.media.size() == 2 && back.osc[1].sample == r1 && back.media[0].pcm == m.media[0].pcm,
           "media survive the state round trip");
    const std::string file = pl::mediaFile(back, r3);
    int rate = 0, chans = 0;
    size_t frames = 0;
    expect(!file.empty() && readWav(file, rate, chans, frames) && rate == 32000 && chans == 2 && frames == 1000, "an entry is extracted as a WAV file");
    expect(pl::mediaFile(back, "media:0123456789abcdef").empty() && pl::mediaFile(back, "/some/file.wav") == "/some/file.wav",
           "unknown entries give no file; plain paths pass through");
    // damaged entries are dropped, the rest of the state loads
    Json j;
    jsonParse(pl::modelToData(m), j);
    j.object["media"].array[0].object["id"] = str("../../etc");
    pl::Model dam;
    expect(pl::modelFromData(jsonStringify(j, 0), dam) && dam.media.size() == 1, "a damaged media entry is dropped");
  }

  // ---- old states: no media, short parameter vectors
  {
    pl::Model m = pl::defaultModel();
    m.osc[0].sample = "/old/path/sample.wav";
    std::string data = pl::modelToData(m);
    pl::Model back;
    expect(pl::modelFromData(data, back) && back.media.empty() && back.osc[0].sample == "/old/path/sample.wav", "an old state without media loads");
    PluginState st = defaultPluginState(pl::kType);
    st.params.resize(40);
    st.data = data;
    normalizePluginState(st);
    expect(st.params.size() == static_cast<size_t>(pl::kParamCount) && renderPeak(st) >= 0, "a short old state normalises and plays");
  }

  // ---- Serum 2: every kind of embedded audio
  const auto table = [] {
    std::vector<float> t;
    for (int f = 0; f < 3; ++f) {
      const auto s = sine(2048, 1 + f, 0.9f);
      t.insert(t.end(), s.begin(), s.end());
    }
    return t;
  }();
  const auto smp = sine(4800, 40, 0.7f);
  const auto kick = sine(2000, 20, 0.8f), snare = sine(1500, 30, 0.6f);
  const auto noiseL = sine(3000, 300, 0.5f), noiseR = sine(3000, 310, 0.5f);
  const auto ir = [] {
    std::vector<float> v(600, 0.f);
    v[0] = 1.f, v[100] = 0.4f, v[300] = 0.2f;
    return v;
  }();
  std::string tun = "[Tuning]\n";
  for (int k = 0; k < 128; ++k) tun += "note " + std::to_string(k) + "=" + std::to_string(k * 100 + (k % 12 == 4 ? -14 : 0)) + "\n";
  Json tunBytes = Json::makeArray();
  for (unsigned char c : tun) tunBytes.array.push_back(num(c));
  const std::string sfz =
      "// test map\n<control> default_path=Kit Samples/\n<group> ampeg_release=0.2\n"
      "<region> lokey=0 hikey=61 pitch_keycenter=60 sample=Kit Samples/Low Hit C3.flac\n"
      "<region> lokey=62 hikey=127 pitch_keycenter=64 sample=Kit Samples\\High Hit E3.flac volume=-3\n";
  Json osc0 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(0.25)}})},
                   {"WTOsc0", obj({{"relativePathToWT", str("Tables/Nowhere/Gone.wav")},
                                   {"tableDisplayName", str("Three Sines")},
                                   {"numFrames", num(2048)},  // stale: the data has three frames
                                   {"embeddedWTData", floats(table)},
                                   {"plainParams", obj({{"kParamTablePos", num(256)}})}})}});
  Json osc1 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_Sample")}})},
                   {"SampleOsc1", obj({{"samplePathRelative", str("Factory/Nowhere/Hit.wav")},
                                       {"embedded_audio", obj({{"data", arr({floats(smp)})}, {"sampleRate", num(48000)}, {"table_name", str("Hit")}})}})}});
  Json members = Json::makeObject();
  members.object["Kit Samples/Low Hit C3.flac"] = obj({{"data", arr({floats(kick), floats(kick)})}, {"sampleRate", num(44100)}, {"loopStart", num(0)}, {"loopEnd", num(0)}});
  members.object["Kit Samples/High Hit E3.flac"] = obj({{"data", arr({floats(snare)})}, {"sampleRate", num(44100)}, {"loopStart", num(100)}, {"loopEnd", num(1400)}});
  Json osc2 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_MultiSample")}})},
                   {"MultiSampleOsc2", obj({{"sfzPath", str("C:/Elsewhere/Kit.sfz")}, {"embedded_sfz", str(sfz)}, {"embedded_audio", members}})}});
  Json noise = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(0.49)}})},
                    {"NoiseOsc3", obj({{"pathToNoiseSample", str("not_installed_noise")}, {"embeddedNoiseData", arr({floats(noiseL), floats(noiseR)})}})}});
  Json fxlist = Json::makeArray();
  fxlist.array.push_back(obj({{"FXConv", obj({{"relativePathToIR", str("Impulses/Nowhere/Room.wav")}, {"sampleRate", num(48000)},
                                                {"embeddedIR", arr({floats(ir)})}, {"plainParams", obj({{"kParamWet", num(30)}})}})},
                              {"type", num(pl::FxConvolve - 1)}}));
  Json glob = obj({{"plainParams", obj({{"kParamMasterVolume", num(0.64)}, {"kParamVoiceAmp", num(0.5)}})}});
  Json body = obj({{"Oscillator0", osc0}, {"Oscillator1", osc1}, {"Oscillator2", osc2}, {"Oscillator3", noise}, {"FXRack0", obj({{"FX", fxlist}})},
                   {"Global0", glob}, {"tuningData", tunBytes}, {"tuningName", str("Test tuning")}});
  {
    PluginState st;
    pl::ImportReport rep;
    const bool ok = pl::importSerumBytes(container(body, "Embedded Everything"), st, rep, {cache.string()});
    expect(ok && rep.missingFiles.empty(), "embedded audio needs no files on disk");
    pl::Model m;
    pl::modelFromData(st.data, m);
    auto P = [&](int k) { return st.params[static_cast<size_t>(k)]; };
    // levels on the source's law: stored = knob^2 (osc, noise, main), voice amp stored = knob, gain 2x
    expect(std::fabs(P(pl::oscP(0, pl::O_Level)) - 0.5f) < 1e-4f && std::fabs(P(pl::noiseP(pl::N_Level)) - 0.7f) < 1e-4f &&
               std::fabs(P(pl::G_MasterVol) - 0.8f) < 1e-4f && std::fabs(P(pl::G_Amp) - 0.5f) < 1e-4f,
           "levels follow the source's volume law");
    // wavetable: the embedded frames win over the (missing) file, whatever numFrames says
    bool same = m.osc[0].table == "custom" && m.osc[0].frames.size() == 3 && m.osc[0].tableName == "Three Sines";
    for (size_t f = 0; same && f < 3; ++f)
      for (size_t i = 0; i < 2048; i += 97) same = same && std::fabs(m.osc[0].frames[f][i] - table[f * 2048 + i]) < 5e-5f;
    expect(same, "embedded wavetable frames imported (16-bit precision)");
    expect(std::fabs(P(pl::oscP(0, pl::O_WtPos)) - 1.f) < 1e-5f, "table position 256 is the last frame");
    // sample, multisample, noise, impulse: media references with their rates
    const pl::Media* s1 = pl::findMedia(m, m.osc[1].sample);
    expect(s1 && s1->rate == 48000 && s1->frames == 4800 && s1->name == "Hit", "embedded sample imported (rate, length, name)");
    const pl::Media* ms = pl::findMedia(m, m.osc[2].sfz);
    expect(ms && !ms->sfz.empty() && ms->sfz.find("Hit C3.flac") == std::string::npos && ms->sfz.find("default_path") == std::string::npos &&
               ms->sfz.find("loop_start=100 loop_end=1400") != std::string::npos && ms->sfz.find("volume=-3") != std::string::npos,
           "multisample map points at its embedded samples (spaces, backslashes, loops kept)");
    const pl::Media* nz = pl::findMedia(m, m.noiseFile);
    expect(nz && nz->channels == 2 && nz->rate == 44100 && std::lround(P(pl::noiseP(pl::N_Type))) == 0, "embedded stereo noise imported");
    std::string irRef;
    for (const auto& e : m.fxExtra)
      if (pl::isMediaRef(e.ir)) irRef = e.ir;
    const pl::Media* im = pl::findMedia(m, irRef);
    expect(im && im->rate == 48000 && im->frames == 600, "embedded impulse response imported");
    expect(m.tuning == tun && m.tuningName == "Test tuning", "embedded microtuning imported");
    // extracted for playback: the multisample map and its members, readable by the map reader
    const std::string sfzFile = pl::mediaFile(m, m.osc[2].sfz);
    std::ifstream sf(fs::u8path(sfzFile));
    const std::string sfzText((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
    sf.close();  // Windows cannot delete a file that is still open (the cache is removed below)
    int wavs = 0;
    for (const auto& de : fs::directory_iterator(fs::u8path(pl::mediaCacheDir())))
      if (de.path().extension() == ".wav") ++wavs;
    expect(!sfzFile.empty() && sfzText.find("media:") == std::string::npos && sfzText.find(".wav") != std::string::npos && wavs >= 2,
           "multisample map extracted next to its samples");
    // it plays: each source alone
    for (int o = 0; o < 3; ++o) {
      PluginState solo = st;
      for (int k = 0; k < 3; ++k) solo.params[static_cast<size_t>(pl::oscP(k, pl::O_Enable))] = k == o ? 1.f : 0.f;
      solo.params[static_cast<size_t>(pl::noiseP(pl::N_Enable))] = 0.f;
      const float pk = renderPeak(solo, o == 2 ? 64 : 60);
      std::printf("# source %d alone: peak %.3f\n", o, static_cast<double>(pk));
      expect(pk > 0.01f, o == 0 ? "embedded wavetable plays" : o == 1 ? "embedded sample plays" : "embedded multisample plays");
    }
    {
      PluginState solo = st;
      for (int k = 0; k < 3; ++k) solo.params[static_cast<size_t>(pl::oscP(k, pl::O_Enable))] = 0.f;
      expect(renderPeak(solo) > 0.005f, "embedded noise plays");
    }
    // a state written by the import loads again on a computer without the cache: entries are re-extracted
    std::error_code rmEc;
    fs::remove_all(fs::u8path(pl::mediaCacheDir()), rmEc);
    if (rmEc) std::printf("# removing the cache failed: %s\n", rmEc.message().c_str());
    expect(!rmEc && !fs::exists(fs::u8path(pl::mediaCacheDir())), "the media cache can be removed (no file left open)");
    expect(renderPeak(st) > 0.01f && fs::exists(fs::u8path(pl::mediaCacheDir())), "a saved state re-extracts its media");
    // link probes (files = false) read no audio
    std::vector<std::string> unread = rep.unread;
    expect(std::find_if(unread.begin(), unread.end(), [](const std::string& u) { return u.find("Volume") != std::string::npos; }) == unread.end(),
           "the mapped values are not reported as unread");
  }

  // ---- a referenced file that is neither embedded nor installed is reported
  {
    Json o0 = obj({{"plainParams", obj({{"kParamEnable", num(1)}})}, {"WTOsc0", obj({{"relativePathToWT", str("Tables/Not Here/Missing Table.wav")}})}});
    Json o1 = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_Sample")}})},
                   {"SampleOsc1", obj({{"samplePathRelative", str("Not Here/Missing Sample.wav")}})}});
    PluginState st;
    pl::ImportReport rep;
    const bool ok = pl::importSerumBytes(container(obj({{"Oscillator0", o0}, {"Oscillator1", o1}}), "Missing"), st, rep, {cache.string()});
    const bool listed = std::find(rep.missingFiles.begin(), rep.missingFiles.end(), "Tables/Not Here/Missing Table.wav") != rep.missingFiles.end() &&
                        std::find(rep.missingFiles.begin(), rep.missingFiles.end(), "Not Here/Missing Sample.wav") != rep.missingFiles.end();
    bool noted = false;
    for (const auto& n : rep.approximated) noted = noted || n.find("Wavetable not found") != std::string::npos;
    pl::Model m;
    pl::modelFromData(st.data, m);
    expect(ok && listed && noted && m.openMissing.size() == 2 && renderPeak(st) > 0.01f, "missing external files are named; a built-in table plays");
    // ...and found next to the preset (a pack's own folder) when it is there
    const fs::path pack = cache / "Pack" / "Presets";
    fs::create_directories(cache / "Pack" / "Samples");
    fs::create_directories(pack);
    {
      pl::Model tmp = pl::defaultModel();
      const std::string r = pl::addMedia(tmp, "x", 44100, {sine(2000, 10, 0.5f)});
      fs::copy_file(fs::u8path(pl::mediaFile(tmp, r)), cache / "Pack" / "Samples" / "Missing Sample.wav", fs::copy_options::overwrite_existing);
    }
    const auto bytes = container(obj({{"Oscillator1", o1}}), "Pack");
    std::ofstream(pack / "Pack.SerumPreset", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    PluginState ps;
    pl::ImportReport pr;
    pl::openPresetFile((pack / "Pack.SerumPreset").u8string(), ps, pr, {cache.string()});
    pl::Model pm;
    pl::modelFromData(ps.data, pm);
    expect(pr.ok && pr.missingFiles.empty() && pm.osc[1].sample.find("Missing Sample.wav") != std::string::npos, "a pack's sample is found next to its preset");
  }

  // ---- a mixed array under an audio key still reads as an array
  {
    Json odd = obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}})}})},
                    {"Extra", obj({{"data", arr({num(1.5), str("x")})}})}});
    PluginState st;
    pl::ImportReport rep;
    expect(pl::importSerumBytes(container(odd, "Odd"), st, rep, {cache.string()}), "a mixed array under an audio key does not break the import");
  }

  // ---- Serum 1: the second stream holds tables A and B and the noise
  {
    std::vector<float> a = sine(4096, 2, 0.8f), b = sine(2048, 3, 0.7f), nz = sine(5000, 400, 0.4f);
    const auto fxp = serum1Fxp(a, b, nz);
    PluginState st;
    pl::ImportReport rep;
    const bool ok = pl::importSerumBytes(fxp, st, rep, {cache.string()});
    pl::Model m;
    pl::modelFromData(st.data, m);
    const pl::Media* n = pl::findMedia(m, m.noiseFile);
    expect(ok && m.osc[0].table == "custom" && m.osc[0].frames.size() == 2 && m.osc[1].table == "custom" && m.osc[1].frames.size() == 1,
           "Serum 1 embedded tables A and B imported");
    expect(ok && std::fabs(m.osc[0].frames[1][100] - a[2048 + 100]) < 5e-5f && std::fabs(m.osc[1].frames[0][700] - b[700]) < 5e-5f,
           "Serum 1 table samples kept (16-bit precision)");
    expect(n && n->frames == 5000 && n->channels == 1, "Serum 1 embedded noise imported");
    expect(rep.missingFiles.empty() && std::fabs(st.params[static_cast<size_t>(pl::oscP(0, pl::O_Level))] - 0.6f) < 0.01f,
           "Serum 1: nothing missing, osc A level mapped");
    // a chunk cut before the audio stream still imports the parameters
    std::vector<uint8_t> cut(fxp.begin(), fxp.begin() + 60 + 20704 / 2);
    PluginState cs;
    pl::ImportReport cr;
    pl::importSerumBytes(cut, cs, cr, {cache.string()});
    expect(true, "a damaged Serum 1 chunk is handled without a crash");
  }

  // ---- J-203a: multisample maps the preset only references, and the source's multisample level law
  {
    // a library layout: <content>/Multisamples/Kit/{Disk Kit.sfz, Tones/Tone.wav}
    const fs::path content = cache / "Content";
    fs::create_directories(content / "Multisamples" / "Kit" / "Tones");
    const auto tone = sine(96000, 440, 0.5f);  // 2 s of 220 Hz at 48 kHz
    {
      pl::Model tmp = pl::defaultModel();
      const std::string r = pl::addMedia(tmp, "tone", 48000, {tone});
      fs::copy_file(fs::u8path(pl::mediaFile(tmp, r)), content / "Multisamples" / "Kit" / "Tones" / "Tone.wav", fs::copy_options::overwrite_existing);
    }
    // default_path without a trailing slash (the source reads it as a folder)
    std::ofstream(content / "Multisamples" / "Kit" / "Disk Kit.sfz")
        << "<control> default_path=Tones\n<region> sample=Tone.wav pitch_keycenter=60 amp_veltrack=50 ampeg_release=0.3\n";
    const std::string mapText = "<control> default_path=Tones\n<region> sample=Tone.wav pitch_keycenter=60 amp_veltrack=50\n";
    // ENV1 held flat with a long release, so the map's own envelope shapes the note
    Json env = obj({{"plainParams", obj({{"kParamAttack", num(0)}, {"kParamSustain", num(1)}, {"kParamRelease", num(4)}})}});
    auto msOsc = [&](const Json& sub) {
      return obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_MultiSample")}})}, {"MultiSampleOsc0", sub}});
    };
    Json smpOsc = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_Sample")}})},
                       {"SampleOsc0", obj({{"embedded_audio", obj({{"data", arr({floats(tone)})}, {"sampleRate", num(48000)}, {"table_name", str("Tone")}})}})}});
    auto load = [&](const Json& osc0, PluginState& st) {
      pl::ImportReport rep;
      return pl::importSerumBytes(container(obj({{"Oscillator0", osc0}, {"Env0", env}}), "Law"), st, rep, {content.string()});
    };
    // (a) the map text the preset carries, its samples found below the stored (stale) map folder
    PluginState ref, disk, one;
    const bool okRef = load(msOsc(obj({{"sfzPathRelative", str("Kit/Moved Away.sfz")}, {"embedded_sfz", str(mapText)}})), ref);
    // (b) a map file on disk, read through default_path without a slash
    const bool okDisk = load(msOsc(obj({{"sfzPathRelative", str("Kit/Disk Kit.sfz")}})), disk);
    // (c) the same audio in the single-sample oscillator
    const bool okOne = load(smpOsc, one);
    expect(okRef && okDisk && okOne, "multisample law presets import");
    auto P = [](const PluginState& st, int k) { return st.params[static_cast<size_t>(k)]; };
    expect(P(ref, pl::tailP(pl::T_MsLaw)) == 1.f && P(ref, pl::tailP(pl::T_KeyCentre)) == 1.f,
           "imports select the source's multisample law and key-tracking centre");
    const auto wRef = renderWindows(ref, 1.f, 48000, 96000), wDisk = renderWindows(disk, 1.f, 48000, 96000);
    const double lRef = meanDb(wRef, 20, 90), lDisk = meanDb(wDisk, 20, 90), lOne = meanDb(renderWindows(one, 1.f, 48000, 96000), 20, 90);
    std::printf("# multisample %.2f dB (map file %.2f dB), sample %.2f dB\n", lRef, lDisk, lOne);
    // measured on the source (J-203a): the multisample oscillator plays a file 9.06 dB louder than the
    // sample oscillator plays the same file at the same settings
    expect(lOne > -60 && std::fabs(lRef - lOne - 9.06) < 0.5, "a referenced map plays (samples below the stored map folder), 9.06 dB over the sample oscillator");
    expect(std::fabs(lDisk - lRef) < 0.5, "a map file whose default_path has no trailing slash plays");
    // velocity: dB = amp_veltrack * 40 * log10(velocity) (measured on the source: 35 % at velocity 0.5 -> -4.2 dB);
    // 50 % at velocity 0.5 -> -6.02 dB
    const double lHalf = meanDb(renderWindows(ref, 0.5f, 48000, 96000), 20, 90);
    std::printf("# velocity 0.5: %.2f dB\n", lHalf - lRef);
    expect(std::fabs(lHalf - lRef + 6.02) < 0.5, "multisample velocity follows amp_veltrack (40 log10 law)");
    // the map's release (0.5 s when it names none) falls about 80 dB linearly in dB, then cuts (measured);
    // ENV1's 4 s release barely moves in that time
    const double atOff = meanDb(wRef, 96, 100), mid = meanDb(wRef, 122, 128), after = meanDb(wRef, 156, 160);
    const double dmid = meanDb(wDisk, 122, 128);
    std::printf("# release: %.1f dB at note off, %.1f dB after 0.25 s (map file 0.3 s: %.1f), %.1f dB after 0.56 s\n", atOff, mid, dmid, after);
    expect(atOff - mid > 30 && atOff - mid < 50 && after < atOff - 90, "multisample release: default 0.5 s, 80 dB linear-in-dB fall, then a cut");
    expect(atOff - dmid > 55 && atOff - dmid < 75, "the map's ampeg_release sets the release");
    // Plasma's own law stays as it was for its own projects: unity with the sample oscillator
    PluginState own = ref;
    own.params[static_cast<size_t>(pl::tailP(pl::T_MsLaw))] = 0.f;
    const double lOwn = meanDb(renderWindows(own, 1.f, 48000, 96000), 20, 90);
    expect(std::fabs(lOwn - lOne) < 0.5, "Plasma's own multisample law is unchanged");
    // an old state (before these tail parameters) normalises to Plasma's own laws
    PluginState old = ref;
    old.params.resize(static_cast<size_t>(pl::tailP(pl::T_KeyCentre)));
    normalizePluginState(old);
    expect(old.params.size() == static_cast<size_t>(pl::kParamCount) && P(old, pl::tailP(pl::T_MsLaw)) == 0.f && P(old, pl::tailP(pl::T_KeyCentre)) == 0.f,
           "old states keep Plasma's own multisample law and key-tracking centre");
    // filter X / Y are not imported (no measurable effect on the source's filters, J-203b); key tracking is
    const PluginState def = defaultPluginState(pl::kType);
    Json filt = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamX", num(0.9)}, {"kParamY", num(0.9)}, {"kParamKeyTrack", num(1)}})}});
    PluginState xy;
    pl::ImportReport xr;
    pl::importSerumBytes(container(obj({{"Oscillator0", msOsc(obj({{"embedded_sfz", str(mapText)}}))}, {"VoiceFilter0", filt}}), "XY"), xy, xr, {content.string()});
    expect(P(xy, pl::filtP(0, pl::F_X)) == P(def, pl::filtP(0, pl::F_X)) && P(xy, pl::filtP(0, pl::F_Y)) == P(def, pl::filtP(0, pl::F_Y)) &&
               P(xy, pl::filtP(0, pl::F_KeyTrack)) == 1.f,
           "filter X / Y are not imported, key tracking is");
    // key tracking pivots on key 32 for imports (measured on the source, slope 1): at key 60 the cutoff sits
    // 28 semitones higher than with Plasma's own key-60 centre, so a low cutoff passes more of the 220 Hz tone
    xy.params[static_cast<size_t>(pl::filtP(0, pl::F_Freq))] = 0.15f;
    PluginState c60 = xy;
    c60.params[static_cast<size_t>(pl::tailP(pl::T_KeyCentre))] = 0.f;
    const double k32 = meanDb(renderWindows(xy, 1.f, 48000, 96000), 20, 90), k60 = meanDb(renderWindows(c60, 1.f, 48000, 96000), 20, 90);
    std::printf("# key tracking at key 60: centre 32 %.1f dB, centre 60 %.1f dB\n", k32, k60);
    expect(k32 > k60 + 3, "imported key tracking pivots on key 32");
  }

  // ---- J-203a: envelope curves. A preset leaves out values at the source's defaults (curves 60),
  // and the source bends a stage by 9.5 atanh(c). Measured on the source, 0.3 s decay to zero: the
  // level halfway through is 0.255 of the start at curve 60 (-11.9 dB) and 0.0041 at curve 90 (-47.7 dB).
  {
    auto decayDb = [&](const Json& envPlain) {
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(obj({{"Env0", obj({{"plainParams", envPlain}})}}), "Env"), st, rep, {cache.string()});
      const auto w = renderWindows(st, 1.f, 40000, 48000);
      return std::make_pair(st, w.size() > 16 ? w[15] - w[0] : 0.0);  // 0.15-0.16 s against 0-0.01 s
    };
    const auto d60 = decayDb(obj({{"kParamDecay", num(0.3)}, {"kParamSustain", num(0)}}));
    const auto d90 = decayDb(obj({{"kParamDecay", num(0.3)}, {"kParamSustain", num(0)}, {"kParamCurve2", num(90)}}));
    std::printf("# decay halfway: default curve %.1f dB, curve 90 %.1f dB\n", d60.second, d90.second);
    expect(d60.first.params[static_cast<size_t>(pl::envP(0, pl::E_CurveD))] == 60.f && d60.first.params[static_cast<size_t>(pl::envP(1, pl::E_CurveA))] == 40.f &&
               d60.first.params[static_cast<size_t>(pl::tailP(pl::T_EnvLaw))] == 1.f,
           "absent envelope curves import as the source's defaults, on its curve law");
    expect(std::fabs(d60.second + 12.1) < 2.0, "default decay curve follows the source");
    expect(d90.second < -42 && d90.second > -54, "a steep decay curve follows the source");
    PluginState gentle = d90.first;
    gentle.params[static_cast<size_t>(pl::tailP(pl::T_EnvLaw))] = 0.f;
    const auto wg = renderWindows(gentle, 1.f, 40000, 48000);
    expect(wg.size() > 16 && wg[15] - wg[0] > d90.second + 3, "Plasma's own curve law is gentler (existing projects unchanged)");
  }

  // ---- J-203g: wavetable warps on the imported law. Synthetic presets: oscillator A plays an embedded
  // one-frame saw at key 60 with one warp; the saw rises from 0 at the cycle start and wraps at the
  // half cycle, like the source's start-up table. The expected harmonic balances were measured on the
  // source (harmonics 1..3 in dBFS); the plain saw has H1 - H2 = 6.0 dB.
  {
    std::vector<float> saw(2048);
    for (size_t i = 0; i < saw.size(); ++i) {
      const float p = static_cast<float>(i) / 2048.f;
      saw[i] = p < 0.5f ? 2.f * p : 2.f * p - 2.f;
    }
    auto warped = [&](const char* warp, double amt, int law = -1) {
      Json wt = obj({{"plainParams", obj({{"kParamWarpMenu", str(warp)}, {"kParamWarp", num(amt)}})}, {"embeddedWTData", floats(saw)}});
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}})}, {"WTOsc0", wt}})}}), "Warp"), st, rep,
                           {cache.string()});
      if (law >= 0) st.params[static_cast<size_t>(pl::tailP(pl::T_OscLaw))] = static_cast<float>(law);
      std::vector<float> L, R;
      renderLR(st, L, R);
      return std::vector<double>{harmDb(L, 1), harmDb(L, 2), harmDb(L, 3), st.params[static_cast<size_t>(pl::tailP(pl::T_OscLaw))]};
    };
    const auto plain = warped("kNone", 0);
    const auto bend = warped("kBendPos", 1.0), bendOld = warped("kBendPos", 1.0, 0);
    const auto sync = warped("kSync", 0.5), lpf = warped("kFilterLPF", 0.25), eo = warped("kEvenOdd", 0.75), q = warped("kQuantize", 0.75);
    std::printf("# warps H1-H2: plain %.1f bend+ 1 %.1f (classic %.1f) lpf 0.25 %.1f even/odd 0.75 %.1f; sync 0.5 H3-H1 %.1f; quantize 0.75 H3-H1 %.1f\n",
                plain[0] - plain[1], bend[0] - bend[1], bendOld[0] - bendOld[1], lpf[0] - lpf[1], eo[0] - eo[1], sync[2] - sync[0], q[2] - q[0]);
    expect(bend[3] == 1.f && std::fabs(plain[0] - plain[1] - 6.0) < 1.0, "imports select the imported wavetable law; the plain saw");
    // bend+ at 1: H1 -32.1, H2 -32.9 (11.2 dB under the plain saw's H1)
    expect(std::fabs(bend[0] - bend[1] - 0.8) < 1.5 && std::fabs(bend[0] - plain[0] + 11.2) < 1.5, "bend+ follows the source");
    expect(std::fabs(bendOld[0] - bendOld[1] - 0.8) > 2.0, "the classic law keeps Plasma's own bend (existing projects unchanged)");
    expect(sync[2] - sync[0] > 22.0, "sync 0.5: the source's third harmonic leads by 29.4 dB");
    expect(std::fabs(lpf[0] - lpf[1] - 10.0) < 1.5, "the low-pass warp at 0.25: H1 - H2 = 10.0 dB");
    expect(std::fabs(eo[0] - eo[1] + 3.5) < 1.5, "even/odd at 0.75 lifts the even harmonics: H1 - H2 = -3.5 dB");
    expect(q[2] - q[0] < -40.0, "quantize at 0.75 is a three-step staircase (no third harmonic)");
    // tube at 0.5: H1 -18.9, H2 -27.6 (2.0 dB over the plain saw's H1); soft saturation at 0.5: H1 -15.3, H3 -27.4
    const auto tube = warped("kDistTube", 0.5), soft = warped("kDistSoftSat", 0.5);
    std::printf("# tube 0.5: H1-H2 %.1f, H1 %+.1f; soft sat 0.5: H1-H3 %.1f\n", tube[0] - tube[1], tube[0] - plain[0], soft[0] - soft[2]);
    expect(std::fabs(tube[0] - tube[1] - 8.7) < 1.5 && std::fabs(tube[0] - plain[0] - 2.0) < 1.0, "the tube warp is as gentle as the source's");
    expect(std::fabs(soft[0] - soft[2] - 12.1) < 1.5, "the soft saturation warp follows the source");

    // FM from oscillator B (both sines, B at volume 0) at 0.2: the source's deviation is a fixed number
    // of Hz, so the sidebands fall an octave up. Measured H2 - H1: -11.2 dB at key 60, -17.4 dB at key 72.
    std::vector<float> sn(2048);
    for (size_t i = 0; i < sn.size(); ++i) sn[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 2048.0));
    auto fm = [&](int key, const char* warp = "kFM_OSC", double amt = 0.2, const std::vector<float>* mod = nullptr, double* h1 = nullptr) {
      Json a = obj({{"plainParams", obj({{"kParamEnable", num(1)}})},
                    {"WTOsc0", obj({{"plainParams", obj({{"kParamWarpMenu", str(warp)}, {"kParamWarp", num(amt)}})}, {"embeddedWTData", floats(sn)}})}});
      Json b = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(0)}})}, {"WTOsc1", obj({{"embeddedWTData", floats(mod ? *mod : sn)}})}});
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(obj({{"Oscillator0", a}, {"Oscillator1", b}}), "FM"), st, rep, {cache.string()});
      std::vector<float> L, R;
      renderLR(st, L, R, key);
      const double up = std::pow(2.0, (key - 60) / 12.0);
      if (h1) *h1 = harmDb(L, 1, up);
      return harmDb(L, 2, up) - harmDb(L, 1, up);
    };
    const double fm60 = fm(60), fm72 = fm(72);
    std::printf("# FM 0.2 sine/sine H2-H1: key 60 %.1f dB, key 72 %.1f dB\n", fm60, fm72);
    expect(std::fabs(fm60 + 11.2) < 1.5 && std::fabs(fm72 + 17.4) < 1.5, "oscillator FM is a fixed deviation in Hz, as on the source");
    // ring / amplitude modulation of the sine by oscillator B's saw, measured: RM 1.0 H1 -26.9, H2 -24.4;
    // AM 0.5 H1 -16.9, H2 -30.5 (the plain sine's H1 is -17.0)
    double rmH1 = 0, amH1 = 0, sineH1 = 0;
    const double rm = fm(60, "kRM_OSC", 1.0, &saw, &rmH1), am = fm(60, "kAM_OSC", 0.5, &saw, &amH1);
    fm(60, "kNone", 0.0, &saw, &sineH1);
    std::printf("# RM 1.0 H2-H1 %.1f, H1 %+.1f; AM 0.5 H2-H1 %.1f, H1 %+.1f\n", rm, rmH1 - sineH1, am, amH1 - sineH1);
    expect(std::fabs(rm - 2.5) < 1.0 && std::fabs(rmH1 - sineH1 + 9.9) < 1.0, "ring modulation follows the source");
    expect(std::fabs(am + 13.6) < 1.0 && std::fabs(amH1 - sineH1 - 0.1) < 1.0, "amplitude modulation keeps the carrier, as on the source");
  }

  // ---- J-203g: a wavetable file whose 'clm ' flags start with 1 blends its frames between positions on
  // the source (measured: a two-frame step at 16 frames moves smoothly with the position), one flagged 0
  // steps. Synthetic two-frame tables (sine, then a sine with its third harmonic).
  {
    auto writeTable = [&](const fs::path& p, const char* flags) {
      std::vector<int16_t> pcm(4096);
      for (size_t i = 0; i < pcm.size(); ++i) {
        const double ph = 2.0 * 3.14159265358979 * static_cast<double>(i % 2048) / 2048.0;
        pcm[i] = static_cast<int16_t>(std::lround((i < 2048 ? std::sin(ph) : 0.5 * std::sin(ph) + 0.5 * std::sin(3 * ph)) * 20000));
      }
      const std::string clm = std::string("<!>2048 ") + flags + " wavetable";
      std::ofstream f(p, std::ios::binary);
      auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
      auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
      const uint32_t clmLen = static_cast<uint32_t>(clm.size());
      f.write("RIFF", 4), u32(4 + 24 + 8 + clmLen + (clmLen & 1) + 8 + 8192), f.write("WAVE", 4);
      f.write("fmt ", 4), u32(16), u16(1), u16(1), u32(44100), u32(88200), u16(2), u16(16);
      f.write("clm ", 4), u32(clmLen), f.write(clm.data(), static_cast<std::streamsize>(clm.size()));
      if (clmLen & 1) f.put('\0');
      f.write("data", 4), u32(8192), f.write(reinterpret_cast<const char*>(pcm.data()), 8192);
    };
    writeTable(cache / "blend.wav", "11000000");
    writeTable(cache / "step.wav", "01000000");
    const auto blend = pl::loadTableFile((cache / "blend.wav").string()), step = pl::loadTableFile((cache / "step.wav").string());
    expect(blend && step && blend->frames == 2 && step->frames == 2 && blend->morph && !step->morph, "a table file's clm flags say whether its frames blend");
  }

  // ---- J-203g: the voice filter's pad. Measured on the source (saw into filter 1, MgL12, cutoff 0.5,
  // resonance 30 %): drive 100 % with pad on plays 17.9 dB over drive 0 (-3.0 against -20.9 dB).
  {
    auto filtered = [&](double drive, double pad) {
      Json f = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("MgL12")}, {"kParamFreq", num(0.5)}, {"kParamReso", num(30)},
                                           {"kParamDrive", num(drive)}, {"kParamPad", num(pad)}})}});
      Json r = obj({{"plainParams", obj({{"kParamFilterBalance", num(0)}})}});
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(obj({{"VoiceFilter0", f}, {"RoutingSlot0", r}}), "Pad"), st, rep, {cache.string()});
      return std::make_pair(st.params[static_cast<size_t>(pl::tailP(pl::T_Pad1))], meanDb(renderWindows(st, 1.f, 48000, 48000), 20, 45));
    };
    const auto clean = filtered(0, 0), padded = filtered(100, 1);
    std::printf("# filter pad: drive 100 + pad %.1f dB over drive 0\n", padded.second - clean.second);
    expect(padded.first == 1.f && clean.first == 0.f && std::fabs(padded.second - clean.second - 17.9) < 1.5, "the voice filter pad follows the source");
  }

  // ---- J-203g: the imported matrix law. Synthetic presets route a macro to oscillator A's volume; the
  // expected levels were measured on the source against the oscillator at full volume.
  {
    auto level = [&](const Json& o0, std::vector<std::pair<const char*, Json>> extra, int law = -1) {
      std::vector<std::pair<const char*, Json>> kv = {{"Oscillator0", o0}};
      for (auto& e : extra) kv.push_back(e);
      Json body = Json::makeObject();
      for (auto& e : kv) body.object[e.first] = e.second;
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(body, "Matrix"), st, rep, {cache.string()});
      if (law >= 0) st.params[static_cast<size_t>(pl::tailP(pl::T_ModLaw))] = static_cast<float>(law);
      std::vector<float> L, R;
      renderLR(st, L, R);
      double e = 0, s = 0;
      for (size_t i = 9600; i < L.size(); ++i) e += 0.5 * (L[i] + R[i]) * 0.5 * (L[i] + R[i]), s += 0.25 * (L[i] - R[i]) * (L[i] - R[i]);
      return std::make_pair(harmDb(L, 1), 10.0 * std::log10(s / (e + 1e-30) + 1e-30));
    };
    auto osc = [&](double vol, std::vector<std::pair<const char*, Json>> more = {}) {
      Json p = obj({{"kParamEnable", num(1)}, {"kParamVolume", num(vol)}});
      for (auto& m : more) p.object[m.first] = m.second;
      return obj({{"plainParams", p}});
    };
    auto route = [&](int src, int aux, const char* type, int id, int pid, const char* param, std::vector<std::pair<const char*, Json>> pp) {
      Json p = Json::makeObject();
      for (auto& m : pp) p.object[m.first] = m.second;
      return obj({{"source", arr({num(src), num(aux)})}, {"destModuleTypeString", str(type)}, {"destModuleID", num(id)},
                  {"destModuleParamID", num(pid)}, {"destModuleParamName", str(param)}, {"plainParams", p}});
    };
    auto macro = [&](double v) { return obj({{"plainParams", obj({{"kParamValue", num(v)}})}}); };
    const double full = level(osc(1.0), {}).first;
    // input curve -50 at macro 50 %, volume 0 + 100 %: 47.9 dB under full volume
    const double curved = level(osc(0.0), {{"Macro7", macro(50)}, {"ModSlot0", route(32, 0, "Oscillator", 0, 1, "kParamVolume", {{"kParamAmount", num(100)}, {"kParamCurveIn", num(-50)}})}}).first;
    // bipolar route at macro 0, volume 0.5 - 40 %: 11.8 dB under full volume
    const auto bip = [&](int law) {
      return level(osc(0.5), {{"Macro7", macro(0)}, {"ModSlot0", route(32, 0, "Oscillator", 0, 1, "kParamVolume", {{"kParamAmount", num(40)}, {"kParamBipolar", num(1)}})}}, law).first;
    };
    // macro 8 (100 %) drives macro 1 by 25 %, macro 1 drives volume 0.1 by 50 %: 5.9 dB over the bare 0.1
    const double bare = level(osc(0.1), {}).first;
    const double chained = level(osc(0.1), {{"Macro7", macro(100)},
                                            {"Macro0", macro(0)},
                                            {"ModSlot0", route(32, 0, "Macro", 0, 0, "kParamValue", {{"kParamAmount", num(25)}})},
                                            {"ModSlot1", route(25, 0, "Oscillator", 0, 1, "kParamVolume", {{"kParamAmount", num(50)}})}})
                                 .first;
    const double bipNew = bip(-1), bipOld = bip(0);
    std::printf("# matrix: curve -50 %.1f dB, bipolar %.1f dB (classic %.1f), macro chain %+.1f dB\n", curved - full, bipNew - full, bipOld - full, chained - bare);
    expect(std::fabs(curved - full + 47.9) < 2.0, "matrix input curves follow the source");
    expect(std::fabs(bipNew - full + 11.8) < 1.0 && bipOld < bipNew - 3.0, "a bipolar route swings half its amount each way (classic: the whole)");
    expect(std::fabs(chained - bare - 5.9) < 1.0, "a modulated macro passes its modulation on");
    // utility width: width 0 + macro 8 at 25 % by 100 % is 12.5 % (the knob cubed): 18 dB less side
    // than the plain 100 % (measured)
    auto widthOf = [&](double w, double m, int law) {
      Json o = osc(0.5, {{"kParamUnison", num(2)}, {"kParamDetune", num(0.3)}, {"kParamUnisonStereo", num(100)}});
      Json fx = obj({{"FX", arr({obj({{"type", num(12)}, {"FXUtils", obj({{"plainParams", obj({{"kParamWidth", num(w)}})}})}})})}});
      std::vector<std::pair<const char*, Json>> ex = {{"FXRack0", fx}};
      if (m > 0) ex.push_back({"Macro7", macro(m)}), ex.push_back({"ModSlot0", route(32, 0, "FXUtils", 0, 3, "kParamWidth", {{"kParamAmount", num(100)}})});
      return level(o, ex, law).second;
    };
    const double w100 = widthOf(100, 0, -1), w25 = widthOf(0, 25, -1), w25old = widthOf(0, 25, 0);
    std::printf("# utility width by macro 25 %%: %.1f dB side against width 100 %% (classic %.1f)\n", w25 - w100, w25old - w100);
    expect(std::fabs(w25 - w100 + 18.0) < 2.0 && w25old > w25 + 10.0, "a matrix route moves the utility width on the source's cubed knob");
    // voice amp (0.5): a route adds to the linear gain, 2 per full amount, past the knob's top and through zero.
    // Measured: macro 50 % by 100 % is +6.0 dB; macro 75 % by -100 % is -6.0 dB (the gain flips sign)
    auto ampOf = [&](double m, double amt, int law) {
      return level(osc(1.0), {{"Global0", obj({{"plainParams", obj({{"kParamVoiceAmp", num(0.5)}})}})}, {"Macro7", macro(m)},
                              {"ModSlot0", route(32, 0, "Global", 0, 2, "kParamVoiceAmp", {{"kParamAmount", num(amt)}})}}, law).first;
    };
    const double ampUp = ampOf(50, 100, -1), ampDown = ampOf(75, -100, -1), ampDownOld = ampOf(75, -100, 0);
    std::printf("# voice amp by macro: +50 %% %+.1f dB, -75 %% %+.1f dB (classic %+.1f)\n", ampUp - full, ampDown - full, ampDownOld - full);
    expect(std::fabs(ampUp - full - 6.0) < 0.5, "a matrix route adds to the voice amp's linear gain");
    expect(std::fabs(ampDown - full + 6.0) < 0.5 && ampDownOld < ampDown - 20.0, "a matrix route takes the voice amp through zero (classic: clamped silent)");
  }

  // ---- J-203i: more wavetable warps on the imported law, the start phase, unmapped multisample keys.
  // The same synthetic saw as above; expected balances measured on the source at key 60 (dBFS):
  // mirror 0.25 H1 -29.1 H2 -23.1; self phase distortion 0.5 H1 -21.0 H2 -74.0 H3 -32.1;
  // quantize 0.7 H1 -20.9 H4 -41.3 H6 -35.2 (4.5 steps per cycle).
  {
    std::vector<float> saw(2048);
    for (size_t i = 0; i < saw.size(); ++i) {
      const float p = static_cast<float>(i) / 2048.f;
      saw[i] = p < 0.5f ? 2.f * p : 2.f * p - 2.f;
    }
    auto warped = [&](const char* warp, double amt, int law = -1, PluginState* out = nullptr) {
      Json wt = obj({{"plainParams", obj({{"kParamWarpMenu", str(warp)}, {"kParamWarp", num(amt)}})}, {"embeddedWTData", floats(saw)}});
      PluginState st;
      pl::ImportReport rep;
      pl::importSerumBytes(container(obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}})}, {"WTOsc0", wt}})}}), "Warp"), st, rep,
                           {cache.string()});
      if (law >= 0) st.params[static_cast<size_t>(pl::tailP(pl::T_OscLaw))] = static_cast<float>(law);
      std::vector<float> L, R;
      renderLR(st, L, R);
      if (out) *out = st;
      std::vector<double> h;
      for (int k = 1; k <= 6; ++k) h.push_back(harmDb(L, k));
      return h;
    };
    PluginState st;
    const auto mir = warped("kDLM", 0.25, -1, &st), mirOld = warped("kDLM", 0.25, 0);
    const auto spd = warped("kSelfPD", 0.5), q = warped("kQuantize", 0.7);
    std::printf("# mirror 0.25 H2-H1 %.1f (classic %.1f); self PD 0.5 H2-H1 %.1f H3-H1 %.1f; quantize 0.7 H4-H1 %.1f H6-H1 %.1f\n", mir[1] - mir[0],
                mirOld[1] - mirOld[0], spd[1] - spd[0], spd[2] - spd[0], q[3] - q[0], q[5] - q[0]);
    expect(std::fabs(mir[1] - mir[0] - 6.0) < 1.5 && std::fabs(mirOld[1] - mirOld[0] - 6.0) > 3.0, "the mirror warp follows the source (classic law unchanged)");
    expect(spd[1] - spd[0] < -35.0 && std::fabs(spd[2] - spd[0] + 11.1) < 1.5, "self phase distortion follows the source");
    expect(std::fabs(q[3] - q[0] + 20.4) < 2.0 && std::fabs(q[5] - q[0] + 14.3) < 2.0, "quantize takes partial steps, as on the source");
    // the waveshaper warps shape at every amount, 0 included (measured on the source's saw, dB re. the
    // plain saw's H1): Sine Shaper 0 H1 +1.7; Sine Shaper 0.5 H5 - H1 = +11.1; Tube 1 H3 - H1 = -9.9;
    // Stomp Box 0.5 H1 - H2 = 19.0; Soft Clip 0 H3 - H1 = -10.8 (plain -9.6)
    const auto plainSaw = warped("kNone", 0), ss0 = warped("kDistSineShaper", 0), ss5 = warped("kDistSineShaper", 0.5);
    const auto tube1 = warped("kDistTube", 1.0), stomp = warped("kDistStompBox", 0.5), soft0 = warped("kDistSoftClip", 0);
    const auto ss0Old = warped("kDistSineShaper", 0, 0);
    std::printf("# shapers: sine shaper 0 H1 %+.1f (classic %+.1f), 0.5 H5-H1 %.1f; tube 1 H3-H1 %.1f; stomp 0.5 H1-H2 %.1f; soft clip 0 H3-H1 %.1f\n",
                ss0[0] - plainSaw[0], ss0Old[0] - plainSaw[0], ss5[4] - ss5[0], tube1[2] - tube1[0], stomp[0] - stomp[1], soft0[2] - soft0[0]);
    expect(std::fabs(ss0[0] - plainSaw[0] - 1.7) < 0.5 && std::fabs(ss0Old[0] - plainSaw[0]) < 0.1, "the sine shaper shapes at 0 (classic law: off at 0)");
    expect(std::fabs(ss5[4] - ss5[0] - 11.1) < 1.5, "the sine shaper folds as the source's at 0.5");
    expect(std::fabs(tube1[2] - tube1[0] + 9.9) < 1.0, "the tube warp at 100 % follows the source");
    expect(std::fabs(stomp[0] - stomp[1] - 19.0) < 1.5, "the stomp box warp keeps the source's even harmonics");
    expect(std::fabs(soft0[2] - soft0[0] + 10.8) < 0.8, "the soft clip warp at 0 follows the source");
    // a wavetable starts at 180 degrees on the source unless the preset sets its phase
    expect(st.params[static_cast<size_t>(pl::oscP(0, pl::O_Phase))] == 0.5f, "an imported wavetable starts at the source's default phase");

    // a key no zone of the map covers is silent on the source (measured on kits), not played by the nearest zone
    const fs::path content = cache / "Content";
    const std::string one = "<control> default_path=Tones\n<region> sample=Tone.wav pitch_keycenter=60 lokey=58 hikey=62\n";
    Json ms = obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_MultiSample")}})},
                   {"MultiSampleOsc0", obj({{"sfzPathRelative", str("Kit/One.sfz")}, {"embedded_sfz", str(one)}})}});
    PluginState mk;
    pl::ImportReport mr;
    const bool okMk = pl::importSerumBytes(container(obj({{"Oscillator0", ms}}), "Zones"), mk, mr, {content.string()});
    const double in = meanDb(renderWindows(mk, 1.f, 24000, 24000, 60), 5, 40), out72 = meanDb(renderWindows(mk, 1.f, 24000, 24000, 72), 5, 40);
    PluginState mkOld = mk;
    mkOld.params[static_cast<size_t>(pl::tailP(pl::T_MsLaw))] = 0.f;
    const double old72 = meanDb(renderWindows(mkOld, 1.f, 24000, 24000, 72), 5, 40);
    std::printf("# zone 58..62: key 60 %.1f dB, key 72 %.1f dB (own law %.1f dB)\n", in, out72, old72);
    expect(okMk && in > -40 && out72 < -100 && old72 > -40, "keys outside every zone are silent on the mapped law (own law: nearest zone)");

    // a mapped multisample's unison skips the stack's level drop: on the source two voices play
    // 4.7 dB and four 6.3 dB over one voice (a wavetable stack: -2.0 and -0.9 dB)
    auto uni = [&](int u, bool law) {
      PluginState s = mk;
      s.params[static_cast<size_t>(pl::oscP(0, pl::O_Unison))] = static_cast<float>(u);
      if (!law) s.params[static_cast<size_t>(pl::tailP(pl::T_MsLaw))] = 0.f;
      return meanDb(renderWindows(s, 1.f, 24000, 24000, 60), 5, 40);
    };
    const double u1 = uni(1, true), u2 = uni(2, true), u4 = uni(4, true), u2Old = uni(2, false);
    std::printf("# multisample unison: 2 voices %+.1f dB, 4 voices %+.1f dB (own law, 2 voices %+.1f dB)\n", u2 - u1, u4 - u1, u2Old - u1);
    expect(std::fabs(u2 - u1 - 4.7) < 1.5 && std::fabs(u4 - u1 - 6.3) < 1.5 && u2Old - u1 < 1.0, "a mapped multisample's unison keeps its voices' level (own law unchanged)");
  }

  std::error_code ec;
  fs::remove_all(cache, ec);
  return finish();
}
