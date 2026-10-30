// Gridline — "plasma" pack tests: Serum 2 import fidelity (J-201). Synthetic
// .SerumPreset files written by this test check the laws measured against the
// hosted source synth: noise pitch and key tracking, built-in noise tone and
// levels, main tuning, Hz offset, the delay effect's times / mix / feedback,
// arpeggiator rates and pattern timing, and clip players without an active
// clip. Expected numbers are plain values measured on the source (each check
// says how); no Xfer content is used or shipped.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
std::vector<uint8_t> container(const Json& body) {
  Cbor c;
  cborJson(c, body);
  const std::string meta = "{\"fileType\":\"SerumPreset\",\"presetName\":\"Fidelity\",\"product\":\"Serum2\",\"version\":1}";
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
Json plain(std::initializer_list<std::pair<const char*, Json>> kv) { return obj({{"plainParams", obj(kv)}}); }
Json floats(const std::vector<float>& v) {
  Json j = Json::makeArray();
  for (float f : v) j.array.push_back(num(static_cast<double>(f)));
  return j;
}

bool import(const Json& body, PluginState& st, pl::ImportReport& rep, const fs::path& cache) {
  return pl::importSerumBytes(container(body), st, rep, {cache.string()});
}
float P(const PluginState& st, int k) { return st.params[static_cast<size_t>(k)]; }

// Renders `seconds` of one note (key, velocity 0.8) held for `holdSec`; returns the left channel.
std::vector<float> render(const PluginState& st, int key, double seconds, double holdSec, bool playing = false) {
  std::vector<float> out;
  auto g = packs::createGenerator(st.type);
  if (!g) return out;
  ChannelAux aux;
  packs::prepareAux(st, aux);
  g->prepareHost(st, kSr);
  ProcessContext ctx;
  ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96, ctx.playing = playing, ctx.offline = true;
  const int total = static_cast<int>(seconds * kSr), hold = static_cast<int>(holdSec * kSr);
  std::vector<float> L(256), R(256);
  for (int pos = 0; pos < total; pos += 256) {
    if (pos == 0) g->noteOn(1, key, 0.8f, 0);
    if (pos <= hold && hold < pos + 256) g->noteOff(1, static_cast<int>(hold - pos));
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    g->process(st, aux, ctx, L.data(), R.data(), 256);  // process adds into the buffers
    out.insert(out.end(), L.begin(), L.end());
    ctx.tick += 256.0 / kSr * 2.0 * ctx.ppq;  // 120 BPM: 2 beats per second
  }
  return out;
}
double frequency(const std::vector<float>& x, double from, double to) {  // positive zero crossings per second
  const size_t a = static_cast<size_t>(from * kSr), b = std::min(x.size(), static_cast<size_t>(to * kSr));
  int n = 0;
  double first = -1, last = -1;
  for (size_t i = a + 1; i < b; ++i)
    if (x[i - 1] <= 0 && x[i] > 0) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = t;
      last = t, ++n;
    }
  return n > 1 ? (n - 1) * kSr / (last - first) : 0;
}
double rmsDb(const std::vector<float>& x, double from, double to) {
  const size_t a = static_cast<size_t>(from * kSr), b = std::min(x.size(), static_cast<size_t>(to * kSr));
  double e = 0;
  for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
  return 10 * std::log10(e / std::max<double>(1, static_cast<double>(b - a)) + 1e-30);
}
bool finite(const std::vector<float>& x) {
  for (float v : x)
    if (!std::isfinite(v)) return false;
  return !x.empty();
}
int onsets(const std::vector<float>& x, double seconds) {  // 5 ms windows rising 12 dB above the last 15 ms
  const size_t w = static_cast<size_t>(0.005 * kSr), end = std::min(x.size(), static_cast<size_t>(seconds * kSr));
  std::vector<double> e;
  for (size_t i = 0; i + w <= end; i += w) e.push_back(rmsDb(x, static_cast<double>(i) / kSr, static_cast<double>(i + w) / kSr));
  int n = 0;
  size_t lastOn = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    double lo = 1e9;
    for (size_t k = i >= 3 ? i - 3 : 0; k < i; ++k) lo = std::min(lo, e[k]);
    if (i == 0) lo = -300;
    if (e[i] > -60 && e[i] - lo > 12 && (n == 0 || i - lastOn > 6)) ++n, lastOn = i;
  }
  return n;
}
std::vector<float> sine(size_t n, double cycles, float a) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = a * static_cast<float>(std::sin(6.283185307179586 * cycles * static_cast<double>(i) / static_cast<double>(n)));
  return v;
}
// noise only: the oscillator off, the noise a 1 kHz sine at 44.1 kHz (the source plays noises at 44.1 kHz)
Json noiseBody(double color, bool track, double detune) {
  Json n3 = obj({{"embeddedNoiseData", arr({floats(sine(44100, 1000, 0.5f))})},
                 {"numFrames", num(44100)},
                 {"detuneFactor", num(detune)},
                 {"plainParams", obj({{"kParamColor", num(color)}})}});
  return obj({{"Oscillator0", plain({{"kParamEnable", num(0)}})},
              {"Oscillator3", obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamVolume", num(1)}, {"kParamPitchTrack", num(track ? 1 : 0)}})},
                                   {"NoiseOsc3", n3}})}});
}
}  // namespace

int main() {
  const fs::path cache =
      fs::temp_directory_path() / ("gl-plasma-fidelity-test-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
  fs::create_directories(cache);
#ifdef _WIN32
  _putenv_s("GRIDLINE_CACHE_DIR", cache.string().c_str());
#else
  setenv("GRIDLINE_CACHE_DIR", cache.string().c_str(), 1);
#endif
  constexpr double kRef = 0.4758218478413068;  // the usual stored noise detuneFactor

  // ---- noise pitch (source measured with a 1 kHz sine as the noise, peak of a 16k FFT)
  {
    PluginState st;
    pl::ImportReport rep;
    // untracked, knob 0.6: 1741.1 Hz on the source (+9.6 semitones)
    expect(import(noiseBody(0.6, false, kRef), st, rep, cache), "noise preset imports");
    expect(std::fabs(P(st, pl::noiseP(pl::N_Pitch)) - 9.6f) < 1e-3f && P(st, pl::noiseP(pl::N_PitchTrack)) < 0.5f, "untracked noise pitch: +96 st per unit above the centre");
    const double f = frequency(render(st, 60, 0.6, 0.6), 0.1, 0.6);
    expect(std::fabs(f - 1741.1) < 1741.1 * 0.005, std::string("untracked noise at 0.6 plays 1741 Hz like the source (" + std::to_string(f) + ")").c_str());
    // untracked below the centre: 384 st per unit (0.45 -> 329.8 Hz on the source)
    expect(import(noiseBody(0.45, false, kRef), st, rep, cache) && std::fabs(P(st, pl::noiseP(pl::N_Pitch)) + 19.2f) < 1e-3f, "untracked noise pitch below the centre");
    // tracked, detuneFactor 1, key 60: 1050.8 Hz on the source
    expect(import(noiseBody(0.5, true, 1.0), st, rep, cache), "tracked noise preset imports");
    const double f2 = frequency(render(st, 60, 0.6, 0.6), 0.1, 0.6);
    expect(P(st, pl::noiseP(pl::N_PitchTrack)) > 0.5f && std::fabs(f2 - 1050.8) < 1050.8 * 0.005, std::string("tracked noise follows detuneFactor (" + std::to_string(f2) + " Hz)").c_str());
    // tracked, usual detuneFactor, key 60: 500 Hz on the source (the sample's rate at key 72)
    expect(import(noiseBody(0.5, true, kRef), st, rep, cache), "tracked noise preset imports (usual detune)");
    const double f3 = frequency(render(st, 60, 0.6, 0.6), 0.1, 0.6);
    expect(std::fabs(f3 - 500.0) < 2.5, std::string("tracked noise plays its own rate at key 72 (" + std::to_string(f3) + " Hz at key 60)").c_str());
    // a matrix route to the knob scales with the knob's span (untracked upper half: 96 of Plasma's 128 st)
    Json body = noiseBody(0.6, false, kRef);
    body.object["ModSlot0"] = obj({{"destModuleTypeString", str("NoiseOsc")}, {"destModuleID", num(3)}, {"destModuleParamName", str("kParamColor")},
                                   {"source", arr({num(25), num(0)})}, {"plainParams", obj({{"kParamAmount", num(50)}})}});
    expect(import(body, st, rep, cache), "noise pitch route imports");
    bool routed = false;
    for (int s = 0; s < pl::kModSlots; ++s)
      if (static_cast<int>(P(st, pl::modP(s, pl::M_Dest))) == pl::noiseP(pl::N_Pitch) + 1) routed = std::fabs(P(st, pl::modP(s, pl::M_Amount)) - 0.375f) < 1e-4f;
    expect(routed, "noise pitch route amount follows the knob span");
  }

  // ---- built-in noises: the knob is a tone filter, level trims (measured on held notes, key 60)
  {
    PluginState st;
    pl::ImportReport rep;
    Json body = noiseBody(0.3, true, kRef);
    body.object["Oscillator3"].object["NoiseOsc3"] = plain({{"kParamColor", num(0.3)}, {"kParamNoiseType", str("White")}});
    body.object["Oscillator3"].object["plainParams"].object["kParamVolume"] = num(0.25);
    expect(import(body, st, rep, cache), "white noise preset imports");
    expect(std::fabs(P(st, pl::tailP(pl::T_NoiseTone)) + 0.4f) < 1e-4f && P(st, pl::noiseP(pl::N_Pitch)) == 0.f && P(st, pl::noiseP(pl::N_PitchTrack)) < 0.5f,
           "white noise: tone from the knob, no pitch, no key tracking (the source's tone does not follow the key)");
    expect(std::fabs(P(st, pl::noiseP(pl::N_Level)) - 0.5f * std::pow(10.f, -3.9f / 40.f)) < 1e-4f, "white noise level trimmed by 3.9 dB");
    // the tone filter darkens: a closed low-pass (tone -0.4: about 1.1 kHz) loses most of a white noise's level
    const auto dark = render(st, 60, 0.6, 0.6);
    st.params[static_cast<size_t>(pl::tailP(pl::T_NoiseTone))] = 0;
    const auto open = render(st, 60, 0.6, 0.6);
    expect(finite(dark) && rmsDb(open, 0.1, 0.5) - rmsDb(dark, 0.1, 0.5) > 6, "noise tone below 0 is a low-pass");
    st.params[static_cast<size_t>(pl::tailP(pl::T_NoiseTone))] = 0.9f;
    const auto bright = render(st, 60, 0.6, 0.6);
    expect(finite(bright) && rmsDb(open, 0.1, 0.5) - rmsDb(bright, 0.1, 0.5) > 6, "noise tone above 0 is a high-pass");
  }

  // ---- sample noise level: stored knob^2, no extra trim (measured flat within 0.1 dB, 30 Hz..10 kHz)
  {
    PluginState st;
    pl::ImportReport rep;
    Json body = noiseBody(0.5, false, kRef);
    body.object["Oscillator3"].object["plainParams"].object["kParamVolume"] = num(0.49);
    expect(import(body, st, rep, cache) && std::fabs(P(st, pl::noiseP(pl::N_Level)) - 0.7f) < 1e-4f, "sample noise level follows the knob law");
  }

  // ---- main tuning: 0..1 = -64..+64 st (source: 0.25 -> -32 st, 0.75 -> +32 st, 0.6 -> 548 Hz from 261.7 Hz)
  {
    PluginState st;
    pl::ImportReport rep;
    expect(import(obj({{"Global0", plain({{"kParamMasterTuning", num(0.6)}, {"kParamTranspose", num(-7)}})}}), st, rep, cache), "tuning preset imports");
    // 0.6 = +12.8 st: +13 st into Transpose (with the stored -7), -20 cents left on the fine tune
    expect(std::fabs(P(st, pl::G_Transpose) - 6.f) < 1e-4f && std::fabs(P(st, pl::G_MasterTune) + 20.f) < 1e-3f, "main tuning folds into transpose and cents");
    expect(import(obj({{"Global0", plain({{"kParamMasterTuning", num(0.5)}})}}), st, rep, cache) && P(st, pl::G_Transpose) == 0.f && P(st, pl::G_MasterTune) == 0.f,
           "the default main tuning (0.5) is no change");
  }

  // ---- Hz offset only in Ratio mode (source: 100 Hz moves a key-36 tone 65.4 -> 165.4 Hz in Ratio mode only)
  {
    PluginState st;
    pl::ImportReport rep;
    expect(import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamHzOffset", num(100)}})}}), st, rep, cache) && P(st, pl::oscP(0, pl::O_HzOffset)) == 0.f,
           "Hz offset is dropped outside Ratio mode");
    expect(import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamHzOffset", num(100)}, {"kParamPitchMode", str("Ratio")}})}}), st, rep, cache) &&
               std::fabs(P(st, pl::oscP(0, pl::O_HzOffset)) - 100.f) < 1e-3f,
           "Hz offset kept in Ratio mode");
  }

  // ---- clip player on without an active clip: the keys play through (as the source does)
  {
    PluginState st;
    pl::ImportReport rep;
    Json notes = arr({obj({{"noteNum", num(60)}, {"timeStamp", num(0)}, {"length", num(4)}})});
    Json body = obj({{"ClipPlayer0", plain({{"kParamEnabled", num(1)}})}, {"MidiClip0", obj({{"clip", obj({{"notes", notes}, {"regionEndBeats", num(4)}})}})}});
    expect(import(body, st, rep, cache) && P(st, pl::G_ClipOn) < 0.5f, "a clip player without an active clip is off");
    body.object["ClipPlayer0"].object["activeClips"] = arr({num(0)});
    expect(import(body, st, rep, cache) && P(st, pl::G_ClipOn) > 0.5f, "a clip player with an active clip stays on");
  }

  // ---- delay: free times in seconds, mix law, unity feedback per 100 %, stable at high feedback
  {
    auto delayBody = [](double wet, double fb, double time) {
      Json fx = arr({obj({{"FXDelay", plain({{"kParamWet", num(wet)}, {"kParamFeedback", num(fb)}, {"kParamBeatSync", num(0)}, {"kParamTimeL", num(time)}, {"kParamTimeR", num(time)}})},
                          {"type", num(pl::FxDelay - 1)}})});
      return obj({{"Env0", plain({{"kParamDecay", num(0.015)}, {"kParamSustain", num(0)}, {"kParamRelease", num(0.01)}})}, {"FXRack0", obj({{"FX", fx}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    expect(import(delayBody(100, 0, 0.2), st, rep, cache), "delay preset imports");
    const auto x = render(st, 60, 0.6, 0.05);
    // fully wet: nothing before the echo, the echo at 0.2 s (source: echo at 200 ms)
    expect(finite(x) && rmsDb(x, 0.19, 0.25) - rmsDb(x, 0.01, 0.15) > 30, "a 0.2 s free delay echoes at 0.2 s, fully wet");
    expect(import(delayBody(50, 80, 0.1), st, rep, cache), "feedback delay imports");
    const auto y = render(st, 60, 4.0, 0.05);
    // 80 % feedback (source, 5 ms peaks of a pluck at key 60: first echo -31 dB, second -34 dB,
    // then about 1.9 dB per repeat out to 2 s)
    expect(finite(y) && rmsDb(y, 3.4, 4.0) < rmsDb(y, 0.1, 0.7) - 20, "80 % feedback decays");
    const double r1 = rmsDb(y, 0.09, 0.13), r2 = rmsDb(y, 0.19, 0.23);
    // J-203c: with the echo filter at the source's own unset values (848.5 Hz, 6.75) the source drops
    // 2.0 dB per repeat from the first echo on (100 ms windows: -38.2, -40.2, -42.1 dB)
    expect(std::fabs((r1 - r2) - 2.0) < 0.7, std::string("the second echo 2 dB below the first at 80 % feedback (" + std::to_string(r1 - r2) + ")").c_str());
  }

  // ---- arpeggiator: rate law, dotted, pattern timing and rows past the held notes
  {
    auto arpBody = [](const char* shape, double rate, bool dotted, const Json& clip) {
      Json pp = obj({{"kParamShape", str(shape)}, {"kParamRate", num(rate)}, {"kParamGate", num(30)}});
      if (dotted) pp.object["kParamDotted"] = num(1);
      return obj({{"Arp0", plain({{"kParamEnabled", num(1)}})}, {"ArpClip0", obj({{"plainParams", pp}, {"clip", clip}})},
                  {"Env0", plain({{"kParamDecay", num(0.02)}, {"kParamSustain", num(0)}, {"kParamRelease", num(0.005)}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    pl::Model m;
    // measured at 120 BPM: 0.5 -> 1/16, 0.6 -> 1/32, 0.4 -> 1/8; dotted 0.5 -> 0.1875 beats
    expect(import(arpBody("Up", 0.6, false, Json::makeObject()), st, rep, cache) && pl::modelFromData(st.data, m) && m.arp[0].rate == 8 && !m.arp[0].dot, "arp rate 0.6 is 1/32");
    expect(import(arpBody("Up", 0.4, false, Json::makeObject()), st, rep, cache) && pl::modelFromData(st.data, m) && m.arp[0].rate == 6, "arp rate 0.4 is 1/8");
    expect(import(arpBody("Up", 0.5, true, Json::makeObject()), st, rep, cache) && pl::modelFromData(st.data, m) && m.arp[0].rate == 8 && m.arp[0].dot,
           "dotted plays the next faster division dotted");
    // pattern: steps at 0, 0.5, 1, 1.5 over 2 beats play every 0.5 beats at 1/16 (source: 0.5 beats), dotted ignored
    Json notes = Json::makeArray();
    for (double t : {0.0, 0.5, 1.0, 1.5}) notes.array.push_back(obj({{"noteNum", num(0)}, {"timeStamp", num(t)}, {"length", num(0.25)}}));
    const Json clip = obj({{"notes", notes}, {"regionEndBeats", num(2)}});
    expect(import(arpBody("Pattern", 0.5, true, clip), st, rep, cache) && pl::modelFromData(st.data, m) && m.arp[0].rate == 7 && !m.arp[0].dot,
           "a pattern keeps straight steps when dotted is on");
    const auto x = render(st, 60, 1.05, 1.05);
    const int n = onsets(x, 1.0);
    expect(finite(x) && n >= 3 && n <= 5, std::string("pattern gaps are rests: 4 steps in a second, not 8 (" + std::to_string(n) + ")").c_str());
    expect(m.arp[0].wrap == 2, "pattern rows past the held notes repeat them");
  }

  // ---- unison mix: imports select Alternating; levels per channel against one voice
  // (source, held key 60, detune 1, left channel: 3 voices at blend 0 -7.2 dB, 2 voices -2.0 dB
  // at any blend, 4 voices at blend 0 +0.6 dB for the left one of the middle pair)
  {
    auto uniBody = [](int voices, double blend) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamUnison", num(voices)}, {"kParamDetune", num(1)}, {"kParamDetuneWid", num(blend)}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    expect(import(uniBody(1, 0), st, rep, cache) && P(st, pl::tailP(pl::T_UniMix)) > 0.5f, "imports use the alternating unison mix");
    const double one = rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.6);
    auto rel = [&](int voices, double blend) {
      if (!import(uniBody(voices, blend), st, rep, cache)) return 99.0;
      return rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.6) - one;
    };
    const double u3 = rel(3, 0), u2 = rel(2, 70), u4 = rel(4, 0);
    expect(std::fabs(u3 + 7.2) < 0.5, std::string("3 voices at blend 0: the centre voice alone at -7.2 dB (" + std::to_string(u3) + ")").c_str());
    expect(std::fabs(u2 + 2.0) < 0.5, std::string("2 voices: one per side at -2 dB (" + std::to_string(u2) + ")").c_str());
    expect(std::fabs(u4 - 0.6) < 0.5, std::string("4 voices at blend 0: the middle pair at +0.6 dB each (" + std::to_string(u4) + ")").c_str());
    // the Even mix keeps its own law: about one voice in total (4 voices, blend 0: -1.4 dB on the left)
    st.params[static_cast<size_t>(pl::tailP(pl::T_UniMix))] = 0.f;
    const double even = rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.6) - one;
    expect(std::fabs(even + 1.4) < 0.3, std::string("the even mix keeps its level (" + std::to_string(even) + ")").c_str());
  }

  // ---- filter drive: imports use Boost; source (saw at key 60, filter at 0.6, oscillator level 0.2):
  // drive 50 % +14.0 dB, 100 % +24.0 dB over drive 0; at full oscillator level drive 100 % ends near
  // -2 dBFS RMS (Plasma's leveled drive stays near -10.5)
  {
    auto drvBody = [](double drive, double level) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(level)}})},
                  {"VoiceFilter0", plain({{"kParamEnable", num(1)}, {"kParamType", str("MgL12")}, {"kParamFreq", num(0.6)}, {"kParamDrive", num(drive)}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto lev = [&](double drive, double level) {
      if (!import(drvBody(drive, level), st, rep, cache)) return 99.0;
      return rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.6);
    };
    const double d0 = lev(0, 0.04), d50 = lev(50, 0.04), d100 = lev(100, 0.04);
    expect(P(st, pl::tailP(pl::T_DriveMode)) > 0.5f, "imports use the boost drive mode");
    expect(std::fabs((d50 - d0) - 14.0) < 1.0, std::string("drive 50 %: +14 dB (" + std::to_string(d50 - d0) + ")").c_str());
    expect(std::fabs((d100 - d0) - 24.0) < 1.0, std::string("drive 100 %: +24 dB (" + std::to_string(d100 - d0) + ")").c_str());
    const double hot = lev(100, 1.0);
    st.params[static_cast<size_t>(pl::tailP(pl::T_DriveMode))] = 0.f;
    const double leveled = rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.6);
    expect(finite(render(st, 60, 0.2, 0.2)) && hot - leveled > 6.0, std::string("boost clips 8 dB higher than leveled (" + std::to_string(hot - leveled) + ")").c_str());
  }

  // ---- old states pad the new parameters with their defaults (noise tone off, even unison mix)
  {
    PluginState st = defaultPluginState(pl::kType);
    st.params.resize(static_cast<size_t>(pl::kTailBase));
    normalizePluginState(st);
    expect(st.params.size() == static_cast<size_t>(pl::kParamCount) && P(st, pl::tailP(pl::T_NoiseTone)) == 0.f && P(st, pl::tailP(pl::T_UniMix)) == 0.f && P(st, pl::tailP(pl::T_DriveMode)) == 0.f,
           "an old state gets the noise tone off, the even unison mix and the leveled drive");
  }

  std::error_code ec;
  fs::remove_all(cache, ec);
  return finish();
}
