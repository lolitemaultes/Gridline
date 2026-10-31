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
    // J-203i: no active clip, but the player names a keyboard-span clip in mono span mode that loops:
    // the keys play it (measured over the library); poly, one-shot and static span clips play through
    auto span = [&](const char* mode, const char* playback) {
      Json mc = obj({{"clip", obj({{"notes", notes}, {"regionEndBeats", num(4)}})}, {"plainParams", obj({{"kParamSpanKeyboardMode", str(mode)}})}});
      if (playback) mc.object["plainParams"].object["kParamPlaybackMode"] = str(playback);
      Json b = obj({{"ClipPlayer0", plain({{"kParamEnabled", num(1)}, {"kParamSpanKeyboardClip", num(0)}})}, {"MidiClip0", mc}});
      return import(b, st, rep, cache) && P(st, pl::G_ClipOn) > 0.5f;
    };
    expect(span("Mono", nullptr) && span("Mono", "Pendulum"), "a mono keyboard-span clip plays from the keys");
    expect(!span("Poly", nullptr) && !span("Mono", "Static") && !span("Mono", "OneShot") && !span("Off", nullptr),
           "poly, static, one-shot and unspanned clips play the keys through");
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

  // ---- LFO law (J-203i, measured on the source with LFO 1 on oscillator A's level, key 60):
  // free rise / delay are seconds, synced ones snap to power-of-two beats (1 beat at 2.1, 1/4 at
  // 1.4, 16 at 3.1), the rise is a linear fade, smoothing is a one-pole of
  // 3.39 ms / (1 - smooth)^1.8 whatever the rate (50 %: 11.8 ms, 90 %: 227 ms)
  {
    auto lfoBody = [](double sync, double delay, double rise, double smooth) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(0)}})},
                  {"LFO0", plain({{"kParamBeatSync", num(sync)}, {"kParamRate", num(2)}, {"kParamDelay", num(delay)}, {"kParamRise", num(rise)}, {"kParamSmooth", num(smooth)}})},
                  {"ModSlot0", obj({{"destModuleTypeString", str("Oscillator")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamVolume")},
                                    {"destModuleParamID", num(1)}, {"source", arr({num(6), num(0)})}, {"plainParams", obj({{"kParamAmount", num(100)}})}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto t16 = [&](int p) { return pl::lfoLawTime(static_cast<double>(P(st, pl::lfoP(0, p)))); };
    expect(import(lfoBody(0, 0.5, 0.25, 50), st, rep, cache) && P(st, pl::tailP(pl::T_LfoLaw)) > 0.5f, "imports use the LFO law");
    expect(std::fabs(t16(pl::L_Delay) - 0.5) < 1e-4 && std::fabs(t16(pl::L_Rise) - 0.25) < 1e-4, "free rise and delay import as seconds");
    expect(import(lfoBody(1, 2.1, 1.4, 0), st, rep, cache) && std::fabs(t16(pl::L_Delay) - 1.0) < 1e-4 && std::fabs(t16(pl::L_Rise) - 0.25) < 1e-4,
           "synced delay 2.1 is 1 beat, rise 1.4 is 1/4 beat");
    expect(import(lfoBody(1, 3.1, 2.6, 0), st, rep, cache) && std::fabs(t16(pl::L_Delay) - 16.0) < 1e-3 && std::fabs(t16(pl::L_Rise) - 4.0) < 1e-4,
           "synced delay 3.1 is 16 beats, rise 2.6 is 4 beats");
    // a free 0.5 s delay: silent before it (the LFO holds oscillator A's level at 0), sounding after
    expect(import(lfoBody(0, 0.5, 0, 0), st, rep, cache), "delayed LFO preset imports");
    const auto d = render(st, 60, 1.0, 1.0);
    expect(finite(d) && rmsDb(d, 0.6, 0.9) - rmsDb(d, 0.05, 0.4) > 30, "a free 0.5 s delay holds the LFO back for half a second");
    // a 1 s rise is a linear fade (source, LFO at 180 deg: the 0.40..0.45 s window sits 18.9 dB under
    // the same LFO point at 1.00..1.05 s; a squared fade gave 13 dB more)
    {
      Json body = lfoBody(0, 0, 1, 0);
      body.object["LFO0"].object["plainParams"].object["kParamPhase"] = num(180);
      expect(import(body, st, rep, cache), "rising LFO preset imports");
      const auto r = render(st, 60, 1.2, 1.2);
      const double drop = rmsDb(r, 1.0, 1.05) - rmsDb(r, 0.40, 0.45);
      expect(std::fabs(drop - 18.9) < 1.5, std::string("the LFO rise is a linear fade (" + std::to_string(drop) + " dB at 0.4 s)").c_str());
    }
    // smoothing is a fixed time: at 50 % a 2 Hz LFO keeps its swing (the classic law smooths over
    // half a cycle and flattens it)
    auto swing = [&](const std::vector<float>& x) {
      double lo = 1e9, hi = -1e9;
      for (double t = 0.5; t < 1.5; t += 0.02) lo = std::min(lo, rmsDb(x, t, t + 0.02)), hi = std::max(hi, rmsDb(x, t, t + 0.02));
      return hi - lo;
    };
    expect(import(lfoBody(0, 0, 0, 0), st, rep, cache), "plain LFO preset imports");
    const double s0 = swing(render(st, 60, 1.6, 1.6));
    expect(import(lfoBody(0, 0, 0, 50), st, rep, cache), "smoothed LFO preset imports");
    const double s50 = swing(render(st, 60, 1.6, 1.6));
    st.params[static_cast<size_t>(pl::tailP(pl::T_LfoLaw))] = 0.f;
    const double s50c = swing(render(st, 60, 1.6, 1.6));
    expect(s0 > 20 && s0 - s50 < 3, std::string("smooth 50 % keeps a 2 Hz LFO's swing (" + std::to_string(s0) + " / " + std::to_string(s50) + " dB)").c_str());
    expect(s50 - s50c > 3, std::string("the classic law smooths more (" + std::to_string(s50c) + " dB)").c_str());
  }

  // ---- a macro moved by another route passes that on whatever the slot order (J-203i, measured on
  // the source: macro 8 at 100 -> macro 1 by 25 %, macro 1 -> oscillator A level 50 %, level 0.1:
  // H1 -30.1 dBFS with either slot first, -35.9 without the macro 8 route)
  {
    auto chainBody = [](bool readerFirst, double amt) {
      const Json feeder = obj({{"destModuleTypeString", str("Macro")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamValue")}, {"destModuleParamID", num(0)},
                               {"source", arr({num(32), num(0)})}, {"plainParams", obj({{"kParamAmount", num(amt)}})}});
      const Json reader = obj({{"destModuleTypeString", str("Oscillator")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamVolume")}, {"destModuleParamID", num(1)},
                               {"source", arr({num(25), num(0)})}, {"plainParams", obj({{"kParamAmount", num(50)}})}});
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(0.1)}})},
                  {"Macro7", plain({{"kParamValue", num(100)}})},
                  {"ModSlot0", readerFirst ? reader : feeder},
                  {"ModSlot1", readerFirst ? feeder : reader}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto lev = [&](bool readerFirst, double amt) { return import(chainBody(readerFirst, amt), st, rep, cache) ? rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.5) : 99.0; };
    const double off = lev(true, 0), fwd = lev(false, 25), rev = lev(true, 25);
    expect(fwd - off > 4.5 && std::fabs(rev - fwd) < 0.2,
           std::string("the macro chain lifts the level by the same 5.8 dB in either slot order (" + std::to_string(fwd - off) + " / " + std::to_string(rev - off) + ")").c_str());
  }

  // ---- compressor makeup under modulation reads the source's knob, gain = 1 + 30.62 knob^2 (J-203i,
  // measured on the source with the threshold at the top: macro routes of 25 / 50 / 75 % lift the
  // output by 9.4 / 18.8 / 25.3 dB; a 50 % route equals a static makeup of 8.66)
  {
    auto compBody = [](double makeup, double amt) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(0.1)}})},
                  {"FXRack0", obj({{"FX", arr({obj({{"type", num(5)}, {"FXComp", plain({{"kParamEnable", num(1)}, {"kParamThresh", num(1)}, {"kParamMakeup", num(makeup)}})}})})}})},
                  {"Macro7", plain({{"kParamValue", num(100)}})},
                  {"ModSlot0", obj({{"destModuleTypeString", str("FXComp")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamMakeup")},
                                    {"destModuleParamID", num(6)}, {"source", arr({num(32), num(0)})}, {"plainParams", obj({{"kParamAmount", num(amt)}})}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto lev = [&](double makeup, double amt) { return import(compBody(makeup, amt), st, rep, cache) ? rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.5) : 99.0; };
    const double l0 = lev(1, 0), l25 = lev(1, 25), l50 = lev(1, 50), st50 = lev(8.6551, 0);
    expect(std::fabs(l25 - l0 - 9.4) < 0.6 && std::fabs(l50 - l0 - 18.8) < 0.6 && std::fabs(l50 - st50) < 0.3,
           std::string("compressor makeup routes follow 1 + 30.62 knob^2 (" + std::to_string(l25 - l0) + " / " + std::to_string(l50 - l0) + " dB)").c_str());
  }

  // ---- bus output 2 feeds the other bus (J-203i, measured on the source: oscillator A only into bus 1,
  // H1 -14.9 dBFS to the main output, -8.9 through bus 2 at full volumes, -14.9 with either
  // volume at half: both bus volumes apply)
  {
    auto busBody = [](double dest, double v1, double v2) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                  {"RoutingSlot0", plain({{"kParamRoutingDest", str("kRoutingDestNone")}, {"kParamFXBus1Level", num(100)}})},
                  {"Global0", plain({{"kParamFXBus1Dest", num(dest)}, {"kParamFXBus1Vol", num(v1)}, {"kParamFXBus2Vol", num(v2)}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto lev = [&](double dest, double v1, double v2) { return import(busBody(dest, v1, v2), st, rep, cache) ? rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.5) : 99.0; };
    const double mainOut = lev(0, 1, 1), chained = lev(2, 1, 1);
    expect(std::lround(P(st, pl::tailP(pl::T_BusChain))) == 1, "bus 1 output 2 imports as the bus 1 into bus 2 chain");
    const double half1 = lev(2, 0.5, 1), half2 = lev(2, 1, 0.5);
    expect(std::fabs(chained - mainOut - 6.0) < 0.5 && std::fabs(half1 - mainOut) < 0.5 && std::fabs(half2 - mainOut) < 0.5,
           std::string("the chained bus takes both bus volumes (" + std::to_string(chained - mainOut) + " / " + std::to_string(half1 - mainOut) + " / " +
                       std::to_string(half2 - mainOut) + " dB)").c_str());
    expect(import(obj({{"Global0", plain({{"kParamFXBus2Dest", num(2)}})}}), st, rep, cache) && std::lround(P(st, pl::tailP(pl::T_BusChain))) == 2,
           "bus 2 output 2 imports as the bus 2 into bus 1 chain");
    // both buses naming each other: bus 2 feeds bus 1, whose return is dropped (a source on bus 1 alone is silent)
    const bool both = import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                                  {"RoutingSlot0", plain({{"kParamRoutingDest", str("kRoutingDestNone")}, {"kParamFXBus1Level", num(100)}})},
                                  {"Global0", plain({{"kParamFXBus1Dest", num(2)}, {"kParamFXBus2Dest", num(2)}})}}),
                             st, rep, cache) &&
                      std::lround(P(st, pl::tailP(pl::T_BusChain))) == 3;
    expect(both && rmsDb(render(st, 60, 0.6, 0.6), 0.2, 0.5) < -100, "buses feeding each other drop bus 1's return");
  }

  // ---- imported bus, direct and master gains glide over about 113 ms:
  // a master step from 0 dB to -12 dB is about two thirds of the way there after 0.11 s
  {
    PluginState st;
    pl::ImportReport rep;
    expect(import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})}}), st, rep, cache), "a glide test preset imports");
    auto g = packs::createGenerator(st.type);
    ChannelAux aux;
    packs::prepareAux(st, aux);
    g->prepareHost(st, kSr);
    ProcessContext ctx;
    ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96, ctx.offline = true;
    PluginState q = st;
    q.params[static_cast<size_t>(pl::G_MasterVol)] *= 0.5f;  // -12 dB
    std::vector<float> L;
    std::vector<float> l(256), r(256);
    const int total = static_cast<int>(kSr);
    for (int pos = 0; pos < total; pos += 256) {
      if (pos == 0) g->noteOn(1, 60, 0.8f, 0);
      std::fill(l.begin(), l.end(), 0.f);
      std::fill(r.begin(), r.end(), 0.f);
      g->process(pos < static_cast<int>(0.2 * kSr) ? st : q, aux, ctx, l.data(), r.data(), 256);
      L.insert(L.end(), l.begin(), l.end());
    }
    const double before = rmsDb(L, 0.1, 0.2);
    const double early = rmsDb(L, 0.2, 0.21) - before, mid = rmsDb(L, 0.30, 0.32) - before, late = rmsDb(L, 0.8, 1.0) - before;
    expect(early > -3 && mid < -5 && mid > -10 && std::fabs(late + 12) < 0.5,
           std::string("the master gain glides (" + std::to_string(early) + " / " + std::to_string(mid) + " / " + std::to_string(late) + " dB)").c_str());
  }

  // ---- a route on the amp attack time already sets the first attack of a note (J-203m, measured on
  // the source: macro 1 at 50 -> Env 1 attack by 50 %, straight attack: the first 25 ms sit 24.6 dB under the held
  // level, the level is reached after about 0.23 s)
  {
    auto atkBody = [](double amt) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                  {"Env0", plain({{"kParamCurve1", num(50)}})},
                  {"Macro0", plain({{"kParamValue", num(50)}})},
                  {"ModSlot0", obj({{"destModuleTypeString", str("Env")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamAttack")},
                                    {"destModuleParamID", num(0)}, {"source", arr({num(25), num(0)})}, {"plainParams", obj({{"kParamAmount", num(amt)}})}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    expect(import(atkBody(50), st, rep, cache), "attack route preset imports");
    const auto r = render(st, 60, 0.6, 0.6);
    const double dip = rmsDb(r, 0.0, 0.025) - rmsDb(r, 0.3, 0.5), mid = rmsDb(r, 0.1, 0.125) - rmsDb(r, 0.3, 0.5);
    expect(std::fabs(dip + 24.6) < 2.0 && std::fabs(mid + 6.8) < 1.5,
           std::string("a modulated attack time shapes the first attack (" + std::to_string(dip) + " / " + std::to_string(mid) + " dB)").c_str());
  }

  // ---- shape segments bend on the source's segment law (J-203m, measured on the source: a 1 Hz
  // triangle LFO 1 on oscillator A's level with its rising segment bent 75 % sits 31.7 dB under the
  // straight one at 0.25..0.30 s; macro 1 at 50 through a drawn route curve bent 75 % sits 36.1 dB
  // under the straight curve; Env 1 with a 0.3 s attack at curve 80 and a 0.3 s decay to zero at
  // curve 95 is 28.9 dB under its peak 0.35..0.40 s after the start)
  {
    auto lfoBody = [](double cv) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(0)}})},
                  {"LFO0", obj({{"plainParams", obj({{"kParamBeatSync", num(0)}, {"kParamRate", num(1)}})},
                                {"curveData", obj({{"curveVals", arr({num(cv), num(0.5), num(0.5)})}, {"numPoints", num(2)}, {"loopbackPointNum", num(481)},
                                                   {"xVals", arr({num(0), num(0.5), num(1)})}, {"yVals", arr({num(1), num(0), num(1)})}})}})},
                  {"ModSlot0", obj({{"destModuleTypeString", str("Oscillator")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamVolume")},
                                    {"destModuleParamID", num(1)}, {"source", arr({num(6), num(0)})}, {"plainParams", obj({{"kParamAmount", num(100)}})}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    auto lfoLev = [&](double cv) { return import(lfoBody(cv), st, rep, cache) ? rmsDb(render(st, 60, 0.4, 0.4), 0.25, 0.30) : 99.0; };
    const double lfoDrop = lfoLev(0.75) - lfoLev(0.5);
    expect(std::fabs(lfoDrop + 31.7) < 1.5, std::string("a 75 % LFO segment bend follows the source's law (" + std::to_string(lfoDrop) + " dB)").c_str());
    auto curveBody = [](double cv) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamVolume", num(0)}})},
                  {"Macro0", plain({{"kParamValue", num(50)}})},
                  {"ModSlot0", obj({{"destModuleTypeString", str("Oscillator")}, {"destModuleID", num(0)}, {"destModuleParamName", str("kParamVolume")},
                                    {"destModuleParamID", num(1)}, {"source", arr({num(25), num(0)})},
                                    {"plainParams", obj({{"kParamAmount", num(100)}, {"kParamMainCurveData", num(1)}})},
                                    {"flex", arr({obj({{"curveVals", arr({num(cv), num(0.5)})}, {"numPoints", num(1)}, {"xVals", arr({num(0), num(1)})},
                                                       {"yVals", arr({num(1), num(0)})}}),
                                                  obj({})})}})}});
    };
    auto curveLev = [&](double cv) { return import(curveBody(cv), st, rep, cache) ? rmsDb(render(st, 60, 0.5, 0.5), 0.3, 0.45) : 99.0; };
    const double curveDrop = curveLev(0.75) - curveLev(0.5);
    expect(std::fabs(curveDrop + 36.1) < 1.5, std::string("a 75 % drawn route curve follows the source's law (" + std::to_string(curveDrop) + " dB)").c_str());
    expect(import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                       {"Env0", plain({{"kParamAttack", num(0.3)}, {"kParamDecay", num(0.3)}, {"kParamSustain", num(0)}, {"kParamCurve1", num(80)}, {"kParamCurve2", num(95)}})}}),
                  st, rep, cache),
           "curved envelope preset imports");
    const auto e = render(st, 60, 0.6, 0.6);
    const double envDrop = rmsDb(e, 0.35, 0.40) - rmsDb(e, 0.20, 0.25);
    expect(std::fabs(envDrop + 28.9) < 1.0, std::string("a curve-95 decay follows the source's stage law (" + std::to_string(envDrop) + " dB)").c_str());
  }

  // ---- a route with "via Env 1" off keeps its level through a decaying amp envelope (J-203m, measured
  // on the source: Env 1 decay 0.1 s to sustain 0; oscillator A to the main output holds -18.8 dBFS
  // from 0 to 0.55 s with it off, and is silent after 0.15 s with it on)
  {
    auto viaBody = [](double via) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                  {"Env0", plain({{"kParamSustain", num(0)}, {"kParamDecay", num(0.1)}})},
                  {"RoutingSlot0", plain({{"kParamRoutingDest", str("kRoutingDestMaster")}, {"kParamViaEnv1", num(via)}})}});
    };
    PluginState st;
    pl::ImportReport rep;
    expect(import(viaBody(0), st, rep, cache) && P(st, pl::tailP(pl::T_RawA)) > 0.5f, "via Env 1 off imports as skipping Env 1");
    const auto off = render(st, 60, 0.6, 0.6);
    expect(import(viaBody(1), st, rep, cache) && P(st, pl::tailP(pl::T_RawA)) < 0.5f, "via Env 1 on imports as through Env 1");
    const auto on = render(st, 60, 0.6, 0.6);
    expect(std::fabs(rmsDb(off, 0.3, 0.5) - rmsDb(off, 0.05, 0.1)) < 1.0 && rmsDb(on, 0.3, 0.5) < rmsDb(off, 0.3, 0.5) - 40,
           "a route that skips Env 1 holds its level, one through it decays");
  }

  // ---- granular density and length import on the source's knob laws (1000 v^4 grains / s, 10 v^4 s);
  // unset values: density 0.25, length 0.5, window amount 100 %
  {
    PluginState st;
    pl::ImportReport rep;
    auto gran = [](Json sp) {
      return obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_Granular")}})},
                                       {"GranularOsc0", obj({{"plainParams", std::move(sp)}})}})}});
    };
    const bool ok = import(gran(obj({{"kParamDensity", num(62.5)}, {"kParamGrainLength", num(0.16)}})), st, rep, cache) &&
                    std::lround(P(st, pl::oscP(0, pl::O_Type))) == pl::OscGranular;
    expect(ok, "a granular oscillator imports");
    if (ok) {
      expect(P(st, pl::tailP(pl::T_GranLaw)) > 0.5f && std::fabs(P(st, pl::oscP(0, pl::O_GDensity)) - 0.5f) < 1e-3 && std::fabs(P(st, pl::oscP(0, pl::O_GLength)) - 0.355656f) < 1e-3,
             "granular density 62.5 / s and length 0.16 s import on the source's knob laws");
      expect(import(gran(obj({{"kParamRandomPitch", num(0)}})), st, rep, cache) && std::fabs(P(st, pl::oscP(0, pl::O_GDensity)) - 0.25f) < 1e-4 &&
                 std::fabs(P(st, pl::oscP(0, pl::O_GLength)) - 0.5f) < 1e-4 && P(st, pl::oscP(0, pl::O_GWinParam)) > 0.999f,
             "unset granular density, length and window amount take the source's defaults");
    }
    expect(std::fabs(pl::grainDensityImp(0.25) - 3.90625) < 1e-6 && std::fabs(pl::grainLengthMsImp(0.5) - 625.0) < 1e-6, "imported granular laws");
  }

  // ---- the sub plays its shapes as the source does (J-203m, measured: the init saw one octave down
  // plus the sine sub at the same pitch sums to -13.0 dBFS on the source; the sub's sine starts
  // downward, in phase with the saw's fundamental, where an upward sine cancels it to -22 dBFS)
  {
    PluginState st;
    pl::ImportReport rep;
    auto subBody = [](const char* shape) {
      return obj({{"Oscillator0", plain({{"kParamEnable", num(1)}, {"kParamOctave", num(-1)}})},
                  {"Oscillator4", obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamOctave", num(-1)}})},
                                       {"SubOsc4", obj({{"plainParams", obj({{"kParamShape", str(shape)}})}})}})}});
    };
    expect(import(subBody("kSine"), st, rep, cache), "a sub preset imports");
    const double mix = rmsDb(render(st, 48, 0.6, 0.6), 0.2, 0.5);
    expect(std::fabs(mix + 13.0) < 1.0, std::string("the sine sub sums in phase with the saw (" + std::to_string(mix) + " dB)").c_str());
    // each shape at the source's loudness against its sine (RMS): square -0.35 dB, saw (with its offset) -0.5 dB
    auto subOnly = [&](const char* shape) {
      return import(obj({{"Oscillator0", plain({{"kParamEnable", num(0)}})},
                         {"Oscillator4", obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamOctave", num(-1)}})},
                                              {"SubOsc4", obj({{"plainParams", obj({{"kParamShape", str(shape)}})}})}})}}),
                    st, rep, cache)
                 ? rmsDb(render(st, 48, 0.6, 0.6), 0.2, 0.5)
                 : 99.0;
    };
    const double sine = subOnly("kSine"), square = subOnly("kSquare"), saw = subOnly("kSaw");
    expect(std::fabs(square - sine + 0.35) < 0.3 && std::fabs(saw - sine + 0.5) < 0.3,
           std::string("sub shape levels follow the source (square " + std::to_string(square - sine) + ", saw " + std::to_string(saw - sine) + " dB)").c_str());
  }

  // ---- voice panel time scaling: 100 % is neutral, the times scale in proportion (measured: 50 %
  // halves an envelope decay, 25 % quarters it); a newer file's named same-note mode imports as on
  {
    PluginState st;
    pl::ImportReport rep;
    expect(import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})},
                       {"VoicePanel0", plain({{"kParamGlobalScalingEnvTime", num(50)}, {"kParamGlobalScalingLfoTime", num(100)}})},
                       {"Global0", plain({{"kParamLimitSameNotePolyphony", str("Sustain")}})}}),
                  st, rep, cache),
           "a time-scaled preset imports");
    expect(std::fabs(P(st, pl::G_EnvScale) - 0.5f) < 1e-4 && std::fabs(P(st, pl::G_LfoScale) - 1.f) < 1e-4, "time scaling 50 % halves, 100 % is neutral");
    expect(P(st, pl::G_SameNote) > 0.5f, "a named same-note mode imports as on");
    // a spectral oscillator's stored base note sets where its sample plays at its own pitch
    expect(import(obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}, {"kParamType", str("kOsc_Spectral")}})},
                                            {"SpectralOsc0", obj({{"baseNote", num(72)}, {"plainParams", obj({})}})}})}}),
                  st, rep, cache) &&
               std::fabs(P(st, pl::oscP(0, pl::O_Coarse)) + 12.f) < 1e-4f,
           "a spectral base note of 72 plays the sample an octave down at key 60");
    // a synced LFO's stored rate picks its division the way the source does (120 BPM: 1.11 and
    // 1.4 stay at 2 bars, 1.5 and 3 are 1 bar, 16 is 1/16)
    auto syncBeats = [&](double rate) {
      if (!import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})}, {"LFO0", plain({{"kParamBeatSync", num(1)}, {"kParamRate", num(rate)}})}}), st, rep,
                  cache))
        return -1.0;
      const int top = pl::lfoRateDivisionCount() - 1;
      return pl::lfoRateDivisionBeats(static_cast<int>(std::lround(P(st, pl::lfoP(0, pl::L_Rate)) * top)));
    };
    expect(syncBeats(1.1097) == 8.0 && syncBeats(1.4) == 8.0 && syncBeats(1.5) == 4.0 && syncBeats(3.0) == 4.0 && syncBeats(16.0) == 0.25,
           "synced LFO rates pick the source's divisions");
  }

  // ---- routes to the main tuning move the source's -64..+64 semitone knob and stop at its ends
  // (measured with the fixed source at key 60: -18.75 % -> 65.4 Hz, 9.375 % -> 523.3 Hz, bipolar
  // 30 % -> 793.1 Hz, 30 % on a knob at +32 semitones -> 10.55 kHz)
  {
    PluginState st;
    pl::ImportReport rep;
    auto tuned = [&](double amt, bool bip, double knob) {
      Json pp = bip ? obj({{"kParamAmount", num(amt)}, {"kParamBipolar", num(1)}}) : obj({{"kParamAmount", num(amt)}});
      Json slot = obj({{"destModuleID", num(0)}, {"destModuleParamID", num(1)}, {"destModuleParamName", str("kParamMasterTuning")},
                       {"destModuleTypeString", str("Global")}, {"plainParams", pp}, {"source", arr({num(38), num(0)})}});
      if (!import(obj({{"Oscillator0", plain({{"kParamEnable", num(1)}})}, {"Global0", plain({{"kParamMasterTuning", num(knob)}})}, {"ModSlot0", slot}}), st,
                  rep, cache))
        return -1.0;
      return frequency(render(st, 60, 0.5, 0.5), 0.1, 0.45);
    };
    const double down = tuned(-18.75, false, 0.5), up = tuned(9.375, false, 0.5), bip = tuned(30, true, 0.5), top = tuned(30, false, 0.75);
    std::printf("# main tuning routes: %.1f %.1f %.1f %.1f Hz\n", down, up, bip, top);
    expect(std::fabs(down / 65.41 - 1) < 0.01 && std::fabs(up / 523.25 - 1) < 0.01 && std::fabs(bip / 793.1 - 1) < 0.01,
           "a route to the main tuning moves 128 semitones per 100 %");
    expect(std::fabs(top / 10548 - 1) < 0.02, "the main tuning stops at +64 semitones");
  }

  // ---- random phase spreads around the start phase over the amount squared (two copies of one
  // oscillator stay within 0.5 dB of in phase at 25 %: 1/32 cycle each way, where the classic law spreads a quarter cycle)
  {
    PluginState st;
    pl::ImportReport rep;
    auto pair = [&](double rnd) {
      Json wt = obj({{"plainParams", obj({{"kParamRandomPhase", num(rnd)}})}});
      if (!import(obj({{"Oscillator0", obj({{"plainParams", obj({{"kParamEnable", num(1)}})}, {"WTOsc0", wt}})},
                       {"Oscillator1", obj({{"plainParams", obj({{"kParamEnable", num(1)}})}, {"WTOsc1", wt}})}}),
                  st, rep, cache))
        return 99.0;
      return rmsDb(render(st, 48, 0.4, 0.4), 0.1, 0.35);
    };
    const double fixed = pair(0), spread = pair(25);
    expect(std::fabs(spread - fixed) < 0.5, std::string("random phase 25 % keeps two copies close to in phase (" + std::to_string(spread - fixed) + " dB)").c_str());
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
