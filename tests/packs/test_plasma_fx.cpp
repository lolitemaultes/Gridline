// Gridline — "plasma" pack tests: Serum 2 effect laws (J-203c). Synthetic
// .SerumPreset files written by this test check the effect laws measured against
// the hosted source synth: the EQ's Q laws, the source's own values for effect
// parameters a preset leaves unset, and the compressor's threshold, limiter and
// makeup laws. Expected numbers are plain values measured on the source (each
// check says how); no Xfer content is used or shipped.
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
#include "../../src/packs/plasma/plasma_dsp.h"
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

// one module in rack 0 on the default patch (saw, random phase off), given values only
Json fxBody(const char* name, int type, std::initializer_list<std::pair<const char*, Json>> kv) {
  Json fx = arr({obj({{name, plain(kv)}, {"type", num(type)}})});
  return obj({{"Oscillator0", obj({{"WTOsc0", plain({{"kParamRandomPhase", num(0)}})}})}, {"FXRack0", obj({{"FX", fx}})}});
}
// the normalized parameters of rack slot 0
const float* slot0(const PluginState& st) { return st.params.data() + pl::fxP(0, pl::X_P0); }
double slot0Plain(const PluginState& st, const char* key) {
  const int t = static_cast<int>(std::lround(P(st, pl::fxP(0, pl::X_Type))));
  const int k = pl::fxParamIndex(t, key);
  return k < 0 ? -1e9 : pl::fxPlain(t, k, static_cast<double>(slot0(st)[k]));
}
// gain (dB) of slot 0's effect for a sine at hz, measured after 0.25 s
double sineGainDb(const PluginState& st, double hz, double amp = 0.1) {
  auto bank = pl::makeFxBank(st, nullptr, kSr);
  const int t = static_cast<int>(std::lround(P(st, pl::fxP(0, pl::X_Type))));
  if (!bank || !bank->unit[0]) return -999;
  pl::FxContext c;
  c.sr = kSr;
  const int n = static_cast<int>(0.5 * kSr);
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * i / kSr));
  for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], t, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
  return rmsDb(L, 0.25, 0.5) - 20 * std::log10(amp / std::sqrt(2.0));
}
}  // namespace

int main() {
  const fs::path cache =
      fs::temp_directory_path() / ("gl-plasma-fx-test-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
  fs::create_directories(cache);
#ifdef _WIN32
  _putenv_s("GRIDLINE_CACHE_DIR", cache.string().c_str());
#else
  setenv("GRIDLINE_CACHE_DIR", cache.string().c_str(), 1);
#endif
  PluginState st;
  pl::ImportReport rep;

  // ---- EQ: peak / pass Q = 0.1 + 18.9 r^4 (r = reso / 100). Source: the low-pass gain at its own
  // cutoff (1046.5 Hz) is -20.0 / -4.7 / 8.1 / 25.6 dB at reso 0 / 40 / 60 / 100.
  {
    const double reso[4] = {0, 40, 60, 100}, want[4] = {-20.0, -4.7, 8.1, 25.6};
    for (int i = 0; i < 4; ++i) {
      expect(import(fxBody("FXEQ", pl::FxEq - 1, {{"kParamType2", num(2)}, {"kParamFreq2", num(1046.5)}, {"kParamReso2", num(reso[i])}, {"kParamGain1", num(0)}}), st, rep, cache), "EQ preset imports");
      const double g = sineGainDb(st, 1046.5);
      expect(std::fabs(g - want[i]) < 0.5, ("EQ low-pass gain at the cutoff, reso " + std::to_string(reso[i]) + ": " + std::to_string(g) + " dB").c_str());
    }
    // the high-pass mirrors it (source: -20.0 dB at its cutoff with reso 0)
    expect(import(fxBody("FXEQ", pl::FxEq - 1, {{"kParamType1", num(2)}, {"kParamFreq1", num(523.25)}, {"kParamReso1", num(0)}, {"kParamGain2", num(0)}}), st, rep, cache), "EQ high-pass imports");
    expect(std::fabs(sineGainDb(st, 523.25) + 20.0) < 0.5, "EQ high-pass gain at the cutoff, reso 0");
    // shelf Q = 0.4 + 1.73 r^3: a +12 dB low shelf at 200 Hz with reso 0 is half way (+6 dB) at the
    // corner and within 1 dB of flat at 1 kHz
    expect(import(fxBody("FXEQ", pl::FxEq - 1, {{"kParamType1", num(0)}, {"kParamFreq1", num(200)}, {"kParamReso1", num(0)}, {"kParamGain1", num(12)}, {"kParamGain2", num(0)}}), st, rep, cache), "EQ shelf imports");
    const double s0 = sineGainDb(st, 200), s1 = sineGainDb(st, 1000);
    expect(std::fabs(s0 - 6.0) < 0.5 && s1 < 1.0 && s1 > -0.1, ("EQ low shelf at the corner " + std::to_string(s0) + " dB, at 1 kHz " + std::to_string(s1) + " dB").c_str());
  }

  // ---- unset effect values take the source's own defaults (found by rendering a stored value
  // against the unset one until both were identical)
  {
    expect(import(fxBody("FXReverb", pl::FxReverb - 1, {{"kParamEnable", num(1)}}), st, rep, cache), "reverb preset imports");
    expect(std::fabs(slot0Plain(st, "kParamWet") - 20) < 0.01 && std::fabs(slot0Plain(st, "kParamSize") - 35) < 0.01 && std::fabs(slot0Plain(st, "kParamWidth") - 20) < 0.01,
           "an unset reverb mixes 20 %, size 35 %, width 20 %");
    expect(import(fxBody("FXChorus", pl::FxChorus - 1, {{"kParamEnable", num(1)}}), st, rep, cache), "chorus preset imports");
    expect(std::fabs(slot0Plain(st, "kParamWet") - 50) < 0.01, "an unset chorus mixes 50 %");
    expect(import(fxBody("FXComp", pl::FxComp - 1, {{"kParamEnable", num(1)}}), st, rep, cache), "compressor preset imports");
    expect(std::fabs(slot0Plain(st, "kParamThresh") - 0.5) < 1e-3 && std::fabs(slot0Plain(st, "kParamRatio") - 4) < 0.01, "an unset compressor sits at threshold 0.5, ratio 4");
    expect(import(fxBody("FXDelay", pl::FxDelay - 1, {{"kParamEnable", num(1)}}), st, rep, cache), "delay preset imports");
    expect(std::fabs(slot0Plain(st, "kParamWet") - 30) < 0.01 && std::fabs(slot0Plain(st, "kParamFeedback") - 40) < 0.01, "an unset delay mixes 30 % with 40 % feedback");
  }

  // ---- compressor. Source, the default saw held at key 36 (-18.8 dB RMS dry): with attack 0.1 ms and
  // release 10 ms the limited level is 60 log10(1 - t) - 2.1 dB RMS (-38.1 at t 0.75, -62.0 at t 0.9);
  // with the default times it is -40.9 at t 0.75; ratios from 100 up limit too; makeup 2 / 4 lift the
  // limited level to -31.1 / -23.7 dB (default times); makeup below 1 is unity.
  {
    auto level = [&](double t, double ratio, double makeup, bool on, bool fast = false) {
      Json b = fxBody("FXComp", pl::FxComp - 1, {{"kParamEnable", num(on ? 1 : 0)}, {"kParamThresh", num(t)}, {"kParamRatio", num(ratio)}, {"kParamMakeup", num(makeup)}});
      if (fast) {
        Json& pp = b.object["FXRack0"].object["FX"].array[0].object["FXComp"].object["plainParams"];
        pp.object["kParamAttack"] = num(0.1), pp.object["kParamRelease"] = num(10);
      }
      expect(import(b, st, rep, cache), "compressor imports");
      const auto x = render(st, 36, 1.0, 1.0);
      expect(finite(x), "compressor output is finite");
      return rmsDb(x, 0.4, 0.95);
    };
    const double a = level(0.75, 1e6, 1, true), c = level(0.75, 100, 1, true);
    const double fa = level(0.75, 1e6, 1, true, true), fb = level(0.9, 1e6, 1, true, true);
    expect(std::fabs(a + 40.9) < 1.5, ("limiter at t 0.75: " + std::to_string(a) + " dB (source -40.9)").c_str());
    expect(std::fabs(fa + 38.1) < 1.5 && std::fabs(fb + 62.0) < 1.5, ("fast limiter at t 0.75 / 0.9: " + std::to_string(fa) + " / " + std::to_string(fb) + " dB (source -38.1 / -62.0)").c_str());
    expect(std::fabs(c - a) < 0.5, "ratio 100 limits like the limiter");
    const double m2 = level(0.75, 1e6, 2, true), m4 = level(0.75, 1e6, 4, true), mh = level(0.75, 1e6, 0.5, true);
    expect(std::fabs(m2 + 31.1) < 1.5 && std::fabs(m4 + 23.7) < 1.5, ("limiter makeup 2 / 4: " + std::to_string(m2) + " / " + std::to_string(m4) + " dB (source -31.1 / -23.7)").c_str());
    expect(std::fabs(mh - a) < 0.3, "makeup below 1 is unity");
    const double open = level(0, 4, 1, true), dry = level(0, 4, 1, false);
    expect(std::fabs(open - dry) < 0.3, "threshold 0 leaves the level alone");
  }

  // ---- reverb. Source, the default saw at key 60 held 0.3 s, wet 100: the default hall's tail falls
  // with an RT60 of 3.1 s over the whole band (Schroeder, 4 s tail), about 19 dB per second; with the
  // delay knob at 0 it is 1.4 s; the space type is mono; at wet 50 the dry drops 2.6 dB (dry 1 - m^2).
  {
    auto rvBody = [](const char* type, double wet, double delay) {
      Json fx = arr({obj({{"FXReverb", plain({{"kParamWet", num(wet)}, {"kParamType", str(type)}, {"kParamDelay", num(delay)}})}, {"type", num(pl::FxReverb - 1)}})});
      return obj({{"Oscillator0", obj({{"WTOsc0", plain({{"kParamRandomPhase", num(0)}})}})},
                  {"Env0", plain({{"kParamRelease", num(0.01)}})},
                  {"FXRack0", obj({{"FX", fx}})}});
    };
    expect(import(rvBody("kHall", 100, 31.25), st, rep, cache), "hall imports");
    const auto h = render(st, 60, 2.5, 0.3);
    const double slope = rmsDb(h, 0.8, 1.0) - rmsDb(h, 1.8, 2.0);
    expect(finite(h) && std::fabs(slope - 19.1) < 5.0, ("the default hall decays about 19 dB/s (" + std::to_string(slope) + ")").c_str());
    // J-203h: the vintage wet ends at 16 kHz (source, 1/3-octave transfer at mix 100: 12.8 kHz -6.2 dB,
    // 20 kHz -43 dB); the hall stays open (-1.9 / -2.5 dB)
    {
      expect(import(rvBody("kVintage", 100, 31.25), st, rep, cache), "vintage imports");
      const double v12 = sineGainDb(st, 12000), v20 = sineGainDb(st, 20000);
      expect(import(rvBody("kHall", 100, 31.25), st, rep, cache), "hall imports");
      const double h12 = sineGainDb(st, 12000), h20 = sineGainDb(st, 20000);
      std::printf("# vintage 12k %.1f 20k %.1f, hall 12k %.1f 20k %.1f\n", v12, v20, h12, h20);
      expect(v12 - v20 > 25.0 && h12 - h20 < 6.0, ("the vintage reverb ends at 16 kHz, the hall does not (" + std::to_string(v12 - v20) + " / " + std::to_string(h12 - h20) + " dB)").c_str());
    }
    expect(import(rvBody("kHall", 100, 0), st, rep, cache), "short hall imports");
    const auto h0 = render(st, 60, 2.5, 0.3);
    const double slope0 = (rmsDb(h0, 0.5, 0.6) - rmsDb(h0, 1.0, 1.1)) / 0.5;
    // source: 3.14 s -> 1.36 s, 2.3 times faster
    expect(slope0 / slope > 1.7 && slope0 / slope < 3.2, ("the delay knob at 0 makes the hall decay about 2.3 times faster (" + std::to_string(slope0 / slope) + ")").c_str());
    expect(import(rvBody("kSpace", 100, 31.25), st, rep, cache), "space imports");
    {
      auto g = packs::createGenerator(st.type);
      ChannelAux aux;
      packs::prepareAux(st, aux);
      g->prepareHost(st, kSr);
      ProcessContext ctx;
      ctx.sampleRate = kSr, ctx.tempo = 120, ctx.ppq = 96, ctx.offline = true;
      std::vector<float> L(256), R(256);
      double diff = 0, sum = 0;
      for (int pos = 0; pos < static_cast<int>(kSr); pos += 256) {
        if (pos == 0) g->noteOn(1, 60, 0.8f, 0);
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        g->process(st, aux, ctx, L.data(), R.data(), 256);
        for (size_t i = 0; i < 256; ++i) diff += std::fabs(L[i] - R[i]), sum += std::fabs(L[i]);
      }
      expect(sum > 0 && diff < sum * 1e-4, "the space reverb is mono");
    }
    // mix law, measured on the dry-only start of a plate (its tail starts later)
    expect(import(rvBody("kPlate", 50, 31.25), st, rep, cache), "plate imports");
    const double m50 = rmsDb(render(st, 60, 0.1, 0.3), 0.005, 0.02);
    expect(import(rvBody("kPlate", 0, 31.25), st, rep, cache), "dry plate imports");
    const double m0 = rmsDb(render(st, 60, 0.1, 0.3), 0.005, 0.02);
    expect(std::fabs((m50 - m0) + 2.6) < 0.6, ("wet 50 keeps the dry 2.6 dB down (" + std::to_string(m50 - m0) + ")").c_str());
  }

  // ---- reverb tone and per-octave decay (J-203f). Source: white noise through each type at mix 100,
  // 1/3-octave transfer of a held note, and Schroeder RT60 per octave after a 1 s noise burst.
  {
    auto rv = [&](const char* type, std::initializer_list<std::pair<const char*, Json>> extra) {
      Json pp = plain({{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamType", str(type)}});
      for (const auto& kv : extra) pp.object["plainParams"].object[kv.first] = kv.second;
      Json fx = arr({obj({{"FXReverb", pp}, {"type", num(pl::FxReverb - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "reverb imports");
    };
    // white noise burst (1 s) through slot 0, read in one octave around hz: the gain while it plays (dB) and the
    // tail's fall from 0.2-0.4 s to 1.0-1.2 s after it (dB)
    auto burst = [&](double hz, double& gain, double& fall) {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      const int on = static_cast<int>(1.0 * kSr), n = static_cast<int>(2.4 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      uint32_t r = 12345u;
      for (int i = 0; i < on; ++i) r = r * 1664525u + 1013904223u, L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>((r >> 8) / 8388608.0 - 1.0) * 0.1f;
      const std::vector<float> in = L;
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxReverb, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      auto band = [&](const std::vector<float>& x) {  // RBJ band-pass, one octave (two passes)
        const double w = 6.283185307179586 * hz / kSr, al = std::sin(w) / (2 * 1.41), a0 = 1 + al;
        const double b0 = al / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - al) / a0;
        std::vector<float> y = x;
        for (int pass = 0; pass < 2; ++pass) {
          double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
          for (auto& v : y) {
            const double o = b0 * v - b0 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = o, v = static_cast<float>(o);
          }
        }
        return y;
      };
      const auto bo = band(L), bi = band(in);
      gain = rmsDb(bo, 0.6, 1.0) - rmsDb(bi, 0.6, 1.0);
      fall = rmsDb(bo, 1.2, 1.4) - rmsDb(bo, 2.0, 2.2);
    };
    double g1, f1, g2, f2;
    // plate low cut 50: -28.2 dB at 200 Hz, -2.7 dB at 3.2 kHz (25.5 dB apart)
    rv("kPlate", {{"kParamFreq", num(50)}});
    burst(200, g1, f1), burst(3200, g2, f2);
    expect(std::fabs((g2 - g1) - 25.5) < 5.0, ("plate low cut 50: 3.2 kHz over 200 Hz " + std::to_string(g2 - g1) + " dB (source 25.5)").c_str());
    // hall high cut 100: +1.4 dB at 200 Hz, -16.6 dB at 5 kHz
    rv("kHall", {{"kParamFreqB", num(100)}});
    burst(200, g1, f1), burst(5000, g2, f2);
    expect(std::fabs((g1 - g2) - 18.0) < 5.0, ("hall high cut 100: 200 Hz over 5 kHz " + std::to_string(g1 - g2) + " dB (source 18.0)").c_str());
    // small hall, delay 100: the 125 Hz octave rings 13.8 s, the 1 kHz one 3.9 s
    rv("kHall", {{"kParamSize", num(0)}, {"kParamDelay", num(100)}});
    burst(125, g1, f1), burst(1000, g2, f2);
    expect(f1 > 0 && f2 / f1 > 2.0, ("small hall with delay 100: the lows decay slower (" + std::to_string(f1) + " vs " + std::to_string(f2) + " dB per 0.8 s)").c_str());
    // plate size 70: 1 kHz 10.6 s, 8 kHz 5.9 s
    rv("kPlate", {{"kParamSize", num(70)}});
    burst(1000, g1, f1), burst(8000, g2, f2);
    expect(f1 > 0 && f2 / f1 > 1.3 && f2 / f1 < 2.6, ("plate size 70: 8 kHz decays about 1.8 times faster than 1 kHz (" + std::to_string(f2 / f1) + ")").c_str());
  }

  // ---- distortion (J-203d). Source, held sines through each mode: the module halves its output; hard
  // clip has a small-signal gain of 1 + 4 d^3 and clips at the source's full scale (1.657 in Plasma's
  // units); downsample holds 1 + 96 d^2 samples at 48 kHz and keeps the level; mix m gives the wet
  // sin^2(pi m / 2) and the dry 1 - wet^2 (dry -2.5 dB at 50 %); the filter is a state-variable filter
  // with Q = 0.71 e^(0.46 BW) (low-pass gain at its cutoff = Q: +4.7 dB at the default BW 1.95); an
  // unset filter type is the low-pass.
  {
    auto dist = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
      expect(import(fxBody("FXDistortion", pl::FxDistortion - 1, kv), st, rep, cache), "distortion preset imports");
    };
    dist({{"kParamEnable", num(1)}});
    expect(std::fabs(slot0Plain(st, "kParamLPHP")) < 1e-3, "an unset distortion filter is the low-pass");
    dist({{"kParamMode", str("kHardClip")}, {"kParamDrive", num(50)}});
    const double hc50 = sineGainDb(st, 300);
    expect(std::fabs(hc50 - 20 * std::log10(0.5 * 1.5)) < 0.2, ("hard clip at drive 50: " + std::to_string(hc50) + " dB (source -2.5)").c_str());
    dist({{"kParamMode", str("kHardClip")}, {"kParamDrive", num(100)}});
    const double hc100 = sineGainDb(st, 300);
    expect(std::fabs(hc100 - 20 * std::log10(0.5 * 5.0)) < 0.2, ("hard clip at drive 100: " + std::to_string(hc100) + " dB (source +8.0)").c_str());
    {  // a loud sine clips at half of full scale
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      std::vector<float> L(4800), R(4800);
      for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(4.0 * std::sin(6.283185307179586 * 200 * static_cast<double>(i) / kSr));
      for (int pos = 0; pos < 4800; pos += 240) pl::fxProcess(*bank->unit[0], pl::FxDistortion, slot0(st), L.data() + pos, R.data() + pos, 240, c, nullptr, 0);
      float pk = 0;
      for (float v : L) pk = std::max(pk, std::fabs(v));
      // (the 4x oversampling filters ring a few percent past the clip on the way down)
      expect(std::fabs(pk - 0.5f * 1.657f) < 0.04f, ("hard clip ceiling " + std::to_string(pk) + " (source 0.83)").c_str());
    }
    // downsample at drive 100: a new value every 97 samples (495 Hz), level kept
    dist({{"kParamMode", str("kDownsample")}, {"kParamDrive", num(100)}});
    {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      std::vector<float> L(48000), R(48000);
      for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.1 * std::sin(6.283185307179586 * 1234 * static_cast<double>(i) / kSr));
      for (int pos = 0; pos < 48000; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxDistortion, slot0(st), L.data() + pos, R.data() + pos, std::min(256, 48000 - pos), c, nullptr, 0);
      int steps = 0;
      for (size_t i = 1; i < L.size(); ++i) steps += L[i] != L[i - 1];
      expect(steps > 480 && steps < 510, ("downsample at drive 100 changes " + std::to_string(steps) + " times a second (source 495)").c_str());
    }
    dist({{"kParamMode", str("kDownsample")}, {"kParamDrive", num(0)}});
    expect(std::fabs(sineGainDb(st, 300)) < 0.1, "downsample at drive 0 keeps the level");
    dist({{"kParamMode", str("kDownsample")}, {"kParamDrive", num(0)}, {"kParamWet", num(50)}});
    const double mix50 = sineGainDb(st, 300);  // in phase: dry 0.75 + wet 0.5
    expect(std::fabs(mix50 - 20 * std::log10(1.25)) < 0.1, ("mix 50 sums dry 0.75 and wet 0.5: " + std::to_string(mix50) + " dB").c_str());
    dist({{"kParamMode", str("kDownsample")}, {"kParamDrive", num(0)}, {"kParamPrePost", num(1)}, {"kParamFreq", num(0.5)}, {"kParamBW", num(1.95)}});
    const double lp = sineGainDb(st, pl::cutoffHz(0.5));
    expect(std::fabs(lp - 20 * std::log10(0.71 * std::exp(0.46 * 1.95))) < 0.3, ("pre filter gain at its cutoff " + std::to_string(lp) + " dB (source +4.7)").c_str());
    dist({{"kParamMode", str("kDownsample")}, {"kParamDrive", num(0)}, {"kParamPrePost", num(1)}, {"kParamFreq", num(0.5)}, {"kParamLPHP", num(100)}});
    expect(sineGainDb(st, 60) < -20, "the high-pass end of the filter type removes lows");
    for (int m = 0; m < 18; ++m) {
      dist({{"kParamMode", num(m)}, {"kParamDrive", num(100)}});
      expect(std::isfinite(sineGainDb(st, 300)), ("mode " + std::to_string(m) + " at drive 100 is finite").c_str());
    }
  }

  // ---- multiband compressor (J-203d). Source, a held 32.7 Hz sine (deep in the low band); levels are
  // sine RMS in dB of the source's output (Plasma's units are 9.04 dB higher); each point below is one
  // measured (input, output) pair with the named values, the rest unset. The module's low cut takes
  // 0.5 dB at this frequency.
  {
    auto mb = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
      expect(import(fxBody("FXComp", pl::FxComp - 1, kv), st, rep, cache), "multiband preset imports");
    };
    auto outDb = [&](double inDb) {
      const double amp = std::sqrt(2.0) * std::pow(10.0, (inDb + 9.04) / 20.0);
      return inDb + sineGainDb(st, 32.7, amp);
    };
    struct Pt {
      double t, below, r0, ratio, ud, rb, dz, in, out;
    };
    const Pt pts[] = {
        {0.5, 0, 1, 4, 100, 1, 7, -27.7, -34.7},    {0.5, 0, 1, 4, 100, 1, 7, -15.7, -31.6},     {0.5, 0, 1, 4, 100, 1, 7, -3.6, -28.2},  // threshold 0.5
        {0.6, 0, 1, 4, 100, 1, 7, -27.7, -45.7},    {0.6, 0, 1, 4, 100, 1, 7, -3.6, -38.0},                                                // threshold 0.6
        {0.75, 0, 1, 4, 100, 1, 7, -27.7, -64.0},   {0.75, 0, 1, 4, 100, 1, 7, -3.6, -43.6},                                               // 40 dB ceiling
        {0.9, 0, 1, 4, 100, 1, 7, -27.7, -67.9},    {0.9, 0, 1, 4, 100, 1, 7, -3.6, -44.1},      {0, 0, 1, 4, 100, 1, 7, -3.6, -4.1},
        {0.5, 0, 0.5, 4, 100, 1, 7, -15.7, -23.9},  {0.5, 0, 0.5, 4, 100, 1, 7, -3.6, -16.3},                                              // band ratio 0.5
        {0.5, 0, 1, 1.5, 100, 1, 7, -3.6, -15.0},   {0.5, 0, 1, 10, 100, 1, 7, -3.6, -32.7},     {0.5, 0, 1, 1e6, 100, 1, 7, -3.6, -35.5}, // global ratio
        {0.5, 0, 1, 4, 50, 1, 7, -15.7, -40.2},     {0.5, 0, 1, 4, 150, 1, 7, -3.6, -19.5},                                                // band threshold
        {0.5, 0.75, 1, 4, 100, 1, 7, -63.8, -48.7}, {0.5, 0.75, 1, 4, 100, 1, 7, -51.8, -45.0},  {0.5, 0.25, 1, 4, 100, 1, 7, -63.8, -59.2},  // upward
        {0.5, 1, 1, 4, 100, 1, 7, -63.8, -43.7},    {0.5, 0.75, 1, 4, 100, 1, 0, -51.8, -42.3},  {0.5, 0.75, 1, 4, 100, 1, 14, -51.8, -49.5}, // deadband
        {0.5, 0.75, 1, 4, 100, 0, 7, -51.8, -54.9}, {0.5, 0.75, 1, 4, 100, 0.5, 7, -51.8, -50.4},                                          // band BELOW
    };
    for (const Pt& p : pts) {
      mb({{"kParamMultiband", num(1)}, {"kParamThresh", num(p.t)}, {"kParamRatioBelow", num(p.below)}, {"kParamRatio0", num(p.r0)}, {"kParamRatio", num(p.ratio)},
          {"kParamThreshUD0", num(p.ud)}, {"kParamRatioBelow0", num(p.rb)}, {"kParamDeadband0", num(p.dz)}});
      const double o = outDb(p.in);
      char msg[200];
      std::snprintf(msg, sizeof msg, "multiband t %.2f below %.2f ratio %.1f x %g thresh %.0f below %.1f dead %.0f: %.1f dB in -> %.1f out (source %.1f)", p.t, p.below, p.r0, p.ratio,
                    p.ud, p.rb, p.dz, p.in, o, p.out);
      expect(std::isfinite(o) && std::fabs(o - p.out) < 1.5, msg);
    }
    // a band gain is a plain gain (source: +6 dB on the low band lifts the sine by 6.0 dB)
    mb({{"kParamMultiband", num(1)}, {"kParamThresh", num(0)}, {"kParamGain0", num(6)}});
    expect(std::fabs(outDb(-27.7) - (-27.7 - 0.5 + 6.0)) < 0.5, "a +6 dB low band gain is a plain gain");
    mb({{"kParamMultiband", num(1)}});
    expect(std::fabs(slot0Plain(st, "kParamRatio1") - 1) < 1e-3 && std::fabs(slot0Plain(st, "kParamRatioBelow2") - 1) < 1e-3 && std::fabs(slot0Plain(st, "kParamRatioBelow")) < 1e-3,
           "an unset multiband has its band ratios at 1 and no upward compression");
  }

  // ---- frequency shifter (J-203d). Source, a held 262 Hz sine, shift 50 % of the unset 22.4 Hz range:
  // unset, the left side carries only the down shift and the right only the up shift, each at the
  // sine's level; MIX 25 keeps the dry at unity with the shift 6 dB down, MIX 50 has both at unity.
  {
    auto bode = [&](double wet, double width) {
      expect(import(fxBody("FXBode", pl::FxBode - 1, {{"kParamShift", num(50)}, {"kParamWet", num(wet)}, {"kParamOutputWidth", num(width)}}), st, rep, cache),
             "frequency shifter imports");
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      const int n = static_cast<int>(kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(6.283185307179586 * 262.0 * i / kSr));
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxBode, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      // the down / up lines of each side (single-bin DFTs over 0.5 s)
      auto line = [&](const std::vector<float>& x, double hz) {
        double re = 0, im = 0;
        for (int i = n / 2; i < n; ++i) re += x[static_cast<size_t>(i)] * std::cos(6.283185307179586 * hz * i / kSr), im += x[static_cast<size_t>(i)] * std::sin(6.283185307179586 * hz * i / kSr);
        return 20 * std::log10(2 * std::sqrt(re * re + im * im) / (n / 2) / 0.1 + 1e-12);
      };
      const double sh = 0.5 * 22.3607;
      return std::vector<double>{line(L, 262 - sh), line(L, 262 + sh), line(R, 262 - sh), line(R, 262 + sh), line(L, 262)};
    };
    const auto d = bode(100, 100);
    expect(std::fabs(d[0]) < 1.0 && d[1] < -20 && d[2] < -20 && std::fabs(d[3]) < 1.0,
           ("unset shifter: left down " + std::to_string(d[0]) + " / up " + std::to_string(d[1]) + ", right down " + std::to_string(d[2]) + " / up " + std::to_string(d[3]) + " dB").c_str());
    const auto m = bode(100, 0);
    expect(std::fabs(m[0] + 6) < 1.0 && std::fabs(m[1] + 6) < 1.0, "width 0 puts half of each shift on both sides");
    const auto w25 = bode(25, 100), w50 = bode(50, 100);
    expect(std::fabs(w25[4]) < 0.5 && std::fabs(w25[0] + 6) < 1.0, ("mix 25: dry " + std::to_string(w25[4]) + " dB, shift " + std::to_string(w25[0]) + " dB (source 0 / -6)").c_str());
    expect(std::fabs(w50[4]) < 0.5 && std::fabs(w50[0]) < 1.0, ("mix 50: dry " + std::to_string(w50[4]) + " dB, shift " + std::to_string(w50[0]) + " dB (source 0 / 0)").c_str());
    expect(std::fabs(slot0Plain(st, "kParamOutputMix")) < 1e-3, "an unset shifter direction is both ways");
  }

  // ---- chorus / flanger / phaser with the sweep stopped (depth 0, feedback 0 unless given). Source gains (dB)
  // at harmonics of a held saw, key 36, against the dry input.
  {
    struct Case {
      const char* fx;
      int type;
      double wet, fb, poles, hz, want, tol;
    };
    const Case cases[] = {
        {"FXChorus", pl::FxChorus, 100, 0, 0, 65.4, -5.73, 0.5},   {"FXChorus", pl::FxChorus, 100, 0, 0, 1046.5, -2.55, 0.5},
        {"FXChorus", pl::FxChorus, 50, 0, 0, 261.6, -0.91, 0.3},   {"FXChorus", pl::FxChorus, 25, 0, 0, 261.6, 0.12, 0.3},
        {"FXFlanger", pl::FxFlanger, 100, 0, 0, 65.4, 1.93, 0.3},  {"FXFlanger", pl::FxFlanger, 100, 0, 0, 1046.5, 1.12, 0.5},
        {"FXFlanger", pl::FxFlanger, 50, 0, 0, 65.4, 1.02, 0.3},   {"FXFlanger", pl::FxFlanger, 100, 50, 0, 65.4, 4.78, 0.5},
        {"FXFlanger", pl::FxFlanger, 100, 50, 0, 523.2, 3.49, 0.7}, {"FXPhaser", pl::FxPhaser, 100, 0, 4, 65.4, 1.02, 0.5},
        {"FXPhaser", pl::FxPhaser, 100, 0, 4, 261.6, -8.59, 1.5},  {"FXPhaser", pl::FxPhaser, 100, 0, 4, 1308.1, 1.93, 0.5},
        {"FXPhaser", pl::FxPhaser, 100, 0, 2, 65.4, 1.87, 0.5},    {"FXPhaser", pl::FxPhaser, 100, 0, 2, 1046.5, -8.18, 1.5},
    };
    for (const Case& k : cases) {
      const Json body =
          k.type == pl::FxChorus
              ? fxBody(k.fx, k.type - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(k.wet)}, {"kParamDepth", num(0)}, {"kParamFeedback", num(k.fb)}, {"kParamFilt", num(20000)}})
          : k.type == pl::FxPhaser
              ? fxBody(k.fx, k.type - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(k.wet)}, {"kParamDepth", num(0)}, {"kParamFeedback", num(k.fb)}, {"kParamNumPoles", num(k.poles)}})
              : fxBody(k.fx, k.type - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(k.wet)}, {"kParamDepth", num(0)}, {"kParamFeedback", num(k.fb)}});
      expect(import(body, st, rep, cache), "modulation preset imports");
      const double g = sineGainDb(st, k.hz);
      expect(std::fabs(g - k.want) < k.tol, (std::string(k.fx) + " mix " + std::to_string(k.wet) + " feedback " + std::to_string(k.fb) + " at " + std::to_string(k.hz) + " Hz: " +
                                             std::to_string(g) + " dB (source " + std::to_string(k.want) + ")")
                                                .c_str());
    }
  }

  // ---- flanger sweep (J-203k). Source, held saw at key 24, feedback 0, the wet tap's delay tracked by
  // cross-correlation every 50 ms: left and right sweep in opposite phase between 0.136 ms and 0.136 + 3.5 ms at
  // full depth (left + right = 181 samples at 48 kHz, 99 at depth 50). Rate 0 holds the sweep at its start.
  for (double depth : {100.0, 50.0}) {
    expect(import(fxBody("FXFlanger", pl::FxFlanger - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamDepth", num(depth)}, {"kParamFeedback", num(0)},
                                                         {"kParamRate", num(0)}}),
                  st, rep, cache),
           "flanger preset imports");
    auto bank = pl::makeFxBank(st, nullptr, kSr);
    pl::FxContext c;
    c.sr = kSr;
    const int n = 512;
    std::vector<float> L(static_cast<size_t>(n), 0.f), R(static_cast<size_t>(n), 0.f);
    L[0] = R[0] = 1.f;
    pl::fxProcess(*bank->unit[0], pl::FxFlanger, slot0(st), L.data(), R.data(), n, c, nullptr, 0);
    auto centroid = [](const std::vector<float>& x) {  // where the wet tap sits (linear tap: weighted position)
      double w = 0, m = 0;
      for (size_t i = 1; i < x.size(); ++i) w += std::fabs(x[i]), m += std::fabs(x[i]) * static_cast<double>(i);
      return w > 0 ? m / w : -1.0;
    };
    const double sum = centroid(L) + centroid(R), want = depth > 75 ? 181.0 : 99.0;
    expect(std::fabs(sum - want) < 4.0, ("flanger depth " + std::to_string(depth) + ": left + right delay " + std::to_string(sum) + " samples (source " + std::to_string(want) + ")").c_str());
  }

  // ---- convolution: the wet path has no latency (the source's identity impulse comes out in phase, so mix 50
  // sums to +1.94 dB); the direct first partition plus the FFT partitions must equal one plain convolution
  {
    expect(import(fxBody("FXConv", pl::FxConvolve - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamIr", num(3)}}), st, rep, cache), "convolution preset imports");
    const int t = pl::FxConvolve;
    auto run = [&](const std::vector<float>& in) {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      std::vector<float> L(in), R(in);
      const int n = static_cast<int>(in.size());
      for (int pos = 0; pos < n; pos += 100) pl::fxProcess(*bank->unit[0], t, slot0(st), L.data() + pos, R.data() + pos, std::min(100, n - pos), c, nullptr, 0);
      return L;
    };
    const int n = 6000;
    std::vector<float> imp(static_cast<size_t>(n), 0.f), noise(static_cast<size_t>(n));
    imp[0] = 1;
    uint32_t seed = 1;
    for (auto& v : noise) seed = seed * 1664525u + 1013904223u, v = static_cast<float>(seed >> 8) / 16777216.f - 0.5f;
    const auto h = run(imp), y = run(noise);
    double err = 0, ref = 0;
    for (int i = 0; i < n; ++i) {
      double s = 0;
      for (int k = 0; k <= i; ++k) s += static_cast<double>(h[static_cast<size_t>(k)]) * noise[static_cast<size_t>(i - k)];
      err += (s - y[static_cast<size_t>(i)]) * (s - y[static_cast<size_t>(i)]), ref += s * s;
    }
    expect(ref > 0 && err / ref < 1e-6, ("convolution matches a plain convolution with its own impulse response: rel err " + std::to_string(err / std::max(ref, 1e-30))).c_str());
  }

  // ---- dimension: mid untouched, a slowly rotating side signal from four delays (15 ms + 100 ms x size apart by
  // 1.18). Source, a held sine at key 60, size 50, mix 100, 0.3 .. 0.6 s after the note: left +4.6 dB, right -5.9 dB
  {
    expect(import(fxBody("FXHyperD", pl::FxHyper - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(0)}, {"kParamDimEWet", num(100)}, {"kParamDimESize", num(50)}}), st, rep, cache),
           "dimension preset imports");
    auto bank = pl::makeFxBank(st, nullptr, kSr);
    pl::FxContext c;
    c.sr = kSr;
    const int n = static_cast<int>(0.6 * kSr);
    std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(6.283185307179586 * 261.63 * i / kSr));
    for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxHyper, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
    const double ref = 20 * std::log10(0.1 / std::sqrt(2.0)), gl = rmsDb(L, 0.3, 0.6) - ref, gr = rmsDb(R, 0.3, 0.6) - ref;
    expect(std::fabs(gl - 4.6) < 0.6 && std::fabs(gr + 5.9) < 0.6, ("dimension size 50: left " + std::to_string(gl) + " dB, right " + std::to_string(gr) + " dB (source +4.6 / -5.9)").c_str());
  }

  // ---- X-Shaper (J-203k). Source, held saws through drawn curves: the drive crossfades from the first curve
  // (drive 0) to the second (drive 100, an empty one is the straight line); the symmetric mode reads the curve
  // over the input's magnitude as C(sqrt|x|)^2, so a straight rising line passes the input (times the module's
  // wet gain: on the imported law the same 0.5 as the hard clip at drive 0) and a falling one keeps only the
  // quiet part of the wave.
  {
    auto line = [&](double y0, double y1, double bend) {
      return obj({{"curveVals", arr({num(bend), num(0.5)})}, {"numPoints", num(1)}, {"xVals", arr({num(0), num(1)})}, {"yVals", arr({num(y0), num(y1)})}});
    };
    auto xs = [&](double drive, const Json& a, const Json& b) {
      Json body = fxBody("FXDistortion", pl::FxDistortion - 1, {{"kParamEnable", num(1)}, {"kParamMode", str("kXShaper")}, {"kParamDrive", num(drive)}});
      body.object["FXRack0"].object["FX"].array[0].object["flex"] = arr({a, b});
      return import(body, st, rep, cache);
    };
    auto gainDb = [&](double amp) {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      auto aux = pl::buildAux(st);
      pl::FxContext c;
      c.sr = kSr;
      const int n = static_cast<int>(0.25 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(6.283185307179586 * 300 * i / kSr));
      const std::vector<float> in = L;
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxDistortion, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, aux.get(), 0);
      return rmsDb(L, 0.1, 0.25) - rmsDb(in, 0.1, 0.25);
    };
    const Json empty = Json::makeObject();
    expect(xs(0, line(1, 0, 0.5), empty), "x-shaper preset imports");
    const double straight = gainDb(0.5);
    std::printf("# x-shaper straight line: %.2f dB\n", straight);
    expect(std::fabs(straight - 20 * std::log10(0.5)) < 0.2, "x-shaper: a straight rising line passes the input");
    expect(xs(0, line(0, 1, 0.5), empty), "x-shaper preset imports");
    const double falling = gainDb(0.5);
    expect(xs(100, line(0, 1, 0.5), empty), "x-shaper preset imports");
    const double fallingD100 = gainDb(0.5);
    std::printf("# x-shaper falling line: %.2f dB at drive 0, %.2f at 100\n", falling, fallingD100);
    expect(std::fabs(fallingD100 - straight) < 0.3, "x-shaper: drive 100 fades to the second (empty: straight) curve");
    expect(std::fabs(falling - straight) > 1.0, "x-shaper: drive 0 plays the first curve");
  }

  // ---- hyper (J-203h). Source, deterministic white noise through the module, impulse responses per 21 ms window:
  // unison 1 passes the input untouched; the dry part is 1 - 5/9 x (0.444 at mix 100; 0.721 at mix 50, x = 0.5);
  // each side carries 0 / 1 / 2 / 3 / 5 / 5 / 7 voices (unison 1..7) at gain 0.97 whose sum passes a 2-pole low cut
  // (253 Hz, Q 0.75: the whole wet path has no DC); voice delays stay between 1 and 10 + 9.6 * detune samples
  // (detune 0: under 10 samples; rate 0 holds them). Unison 4, detune 25: the source comes out 4 to 7 dB louder in the
  // mids (3 incoherent voices per side + the dry part).
  {
    auto hyper = [&](double uni, double det, double rate, double mix) {
      return import(fxBody("FXHyperD", pl::FxHyper - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(mix)}, {"kParamUnison", num(uni)},
                                                         {"kParamDetune", num(det)}, {"kParamRate", num(rate)}}), st, rep, cache);
    };
    auto runIr = [&](int n) {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      std::vector<float> L(static_cast<size_t>(n), 0.f), R(static_cast<size_t>(n), 0.f);
      L[0] = R[0] = 1.f;
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxHyper, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      return std::make_pair(L, R);
    };
    expect(hyper(1, 50, 40, 100), "hyper preset imports");
    expect(std::fabs(sineGainDb(st, 261.6)) < 0.05, "hyper unison 1 passes the input untouched");
    for (double mix : {100.0, 50.0}) {
      expect(hyper(2, 0, 0, mix), "hyper preset imports");
      const auto ir = runIr(8000);
      const double dry = mix > 75 ? 4.0 / 9.0 : 0.7222;
      double sum = 0, early = 0, peak = 0;
      for (int i = 0; i < 8000; ++i) sum += ir.first[static_cast<size_t>(i)];
      for (int i = 1; i <= 11; ++i) early += ir.first[static_cast<size_t>(i)], peak = std::max(peak, static_cast<double>(ir.first[static_cast<size_t>(i)]));
      expect(std::fabs(ir.first[0] - dry) < 0.01, ("hyper mix " + std::to_string(mix) + ": dry tap " + std::to_string(ir.first[0]) + " (source " + std::to_string(dry) + ")").c_str());
      expect(std::fabs(sum - dry) < 0.01, ("hyper wet path has no DC (IR sum " + std::to_string(sum) + ")").c_str());
      expect(early > 0.4 * (mix > 75 ? 1.0 : 0.5) && peak > 0.2, ("hyper detune 0: the voice sits within 11 samples (" + std::to_string(early) + ")").c_str());
    }
    // J-203k: each voice starts at its own fixed sweep phase, different on the two sides (source, unison 2,
    // detune 0, rate 0, voice delays found by cross-correlation: left about 9.5, right about 4.9 samples at 48 kHz)
    {
      expect(hyper(2, 0, 0, 100), "hyper preset imports");
      const auto ir = runIr(64);
      int atL = 1, atR = 1;
      for (int i = 1; i < 20; ++i) {
        if (ir.first[static_cast<size_t>(i)] > ir.first[static_cast<size_t>(atL)]) atL = i;
        if (ir.second[static_cast<size_t>(i)] > ir.second[static_cast<size_t>(atR)]) atR = i;
      }
      const double k = kSr / 48000.0;
      expect(std::fabs(atL - 9.5 * k) <= 1.0 * k + 0.5 && std::fabs(atR - 4.9 * k) <= 1.0 * k + 0.5,
             ("hyper start delays per side: " + std::to_string(atL) + " / " + std::to_string(atR) + " samples").c_str());
    }
    expect(hyper(4, 25, 40, 100), "hyper preset imports");
    {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      const int n = static_cast<int>(2 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      uint32_t seed = 7;
      for (int i = 0; i < n; ++i) seed = seed * 1664525u + 1013904223u, L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(seed >> 8) / 16777216.f - 0.5f;
      const std::vector<float> in = L;
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxHyper, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      const double g = rmsDb(L, 0.5, 2.0) - rmsDb(in, 0.5, 2.0), gr = rmsDb(R, 0.5, 2.0) - rmsDb(in, 0.5, 2.0);
      // broadband: the linear taps keep (1 - f)^2 + f^2 of white noise's power at fraction f (2/3 on average,
      // as the source's taps do), so 0.198 + 3 * 0.94 * 2/3 = 2.08: +3.2 dB
      expect(g > 2.5 && g < 3.9 && gr > 2.5 && gr < 3.9, ("hyper unison 4 detune 25 on noise: " + std::to_string(g) + " / " + std::to_string(gr) + " dB (expected +3.2)").c_str());
    }
  }

  // ---- utility, filter effect, multiband mix (J-203f). Source: noise and stereo saws through each module, 1/3-octave
  // transfer against the dry render. The utility's low cut is two 2-pole sections (-6 dB at the knob); the utility
  // and the filter effect mix as the distortion does (dry 1 - wet^2, wet sin^2(pi m / 2): dry -2.5 dB at 50 %);
  // a multiband compressor at ratio 1 stays flat at mix 50 (the dry part follows the crossovers' phase).
  {
    expect(import(fxBody("FXUtils", pl::FxUtility - 1, {{"kParamEnable", num(1)}, {"kParamHPF", num(100)}}), st, rep, cache), "utility imports");
    const double h50 = sineGainDb(st, 50), h100 = sineGainDb(st, 100), h400 = sineGainDb(st, 400);
    expect(std::fabs(h50 + 24.6) < 1.0 && std::fabs(h100 + 6.0) < 0.5 && h400 > -0.2,
           ("utility low cut 100: " + std::to_string(h50) + " / " + std::to_string(h100) + " / " + std::to_string(h400) + " dB at 50 / 100 / 400 Hz (24 dB per octave)").c_str());
    expect(import(fxBody("FXUtils", pl::FxUtility - 1, {{"kParamEnable", num(1)}, {"kParamHPF", num(2000)}, {"kParamWet", num(50)}}), st, rep, cache), "utility imports");
    const double um = sineGainDb(st, 60);
    expect(std::fabs(um + 2.5) < 0.3, ("utility mix 50, the dry part alone: " + std::to_string(um) + " dB (source -2.5)").c_str());
    auto filt = [&](std::initializer_list<std::pair<const char*, Json>> extra) {
      Json pp = plain({{"kParamEnable", num(1)}});
      for (const auto& kv : extra) pp.object["plainParams"].object[kv.first] = kv.second;
      Json fx = arr({obj({{"FXFilter", pp}, {"type", num(pl::FxFilter - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "filter effect imports");
    };
    for (const auto& [m, want] : {std::pair<double, double>{25, -0.2}, {50, -2.5}, {75, -11.3}}) {
      filt({{"kParamType", str("MgL12")}, {"kParamFreq", num(0.3)}, {"kParamReso", num(0)}, {"kParamWet", num(m)}});
      const double g = sineGainDb(st, 5000);
      expect(std::fabs(g - want) < 0.4, ("filter effect mix " + std::to_string(m) + ", the dry part alone: " + std::to_string(g) + " dB (source " + std::to_string(want) + ")").c_str());
    }
    // runs slot 0's filter on a sine with the imported drive law (boost), returns the gain (dB)
    auto boosted = [&](double hz, double amp) {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr, c.driveBoost = true;
      const int n = static_cast<int>(0.5 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * i / kSr));
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxFilter, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      return rmsDb(L, 0.25, 0.5) - 20 * std::log10(amp / std::sqrt(2.0));
    };
    // PAD: drive 60 adds its small-signal gain without clipping (source +16.3 dB over drive 0 with a loud saw)
    filt({{"kParamType", str("MgL12")}, {"kParamFreq", num(0.5)}, {"kParamReso", num(30)}, {"kParamDrive", num(0)}, {"kParamPad", num(1)}});
    const double p0 = boosted(100, 0.5);
    filt({{"kParamType", str("MgL12")}, {"kParamFreq", num(0.5)}, {"kParamReso", num(30)}, {"kParamDrive", num(60)}, {"kParamPad", num(1)}});
    const double p60 = boosted(100, 0.5);
    expect(std::fabs(p60 - p0 - 16.3) < 1.0, ("filter PAD, drive 60 over drive 0: " + std::to_string(p60 - p0) + " dB (source +16.3)").c_str());
    // J-203h: the Mg ladders clip their driven input before the feedback (the loop clips far above), so the
    // resonance keeps its level and the ceiling sits higher. Source, filter effect MgL12 at cutoff 0.5,
    // drive 100, held saw at key 36: -0.6 dB RMS (1.9 dB above the loop-clipped ladder's -2.5) at res 0.
    // Here: a loud sine below the cutoff peaks at the input clip, 1.4 x the drive head of 2.66 (+2.9 dB).
    auto loudPeak = [&](double reso) {
      filt({{"kParamType", str("MgL12")}, {"kParamFreq", num(0.5)}, {"kParamReso", num(reso)}, {"kParamDrive", num(100)}});
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr, c.driveBoost = true;
      const int n = static_cast<int>(kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(std::sin(6.283185307179586 * 40.0 * i / kSr));
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxFilter, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      float pk = 0;
      for (int i = n / 2; i < n; ++i) pk = std::max(pk, std::fabs(L[static_cast<size_t>(i)]));
      return static_cast<double>(pk);
    };
    {
      const double p0 = loudPeak(0);
      std::printf("# driven MgL12, loud 40 Hz sine: peak %.2f\n", p0);
      expect(p0 > 3.3 && p0 < 4.3, ("driven Mg ladder clips its input at 1.4 x the head: peak " + std::to_string(p0) + " (3.72)").c_str());
    }
    // comb: echoes every 1.0028 periods of the cutoff (source, cutoff 426.6 Hz at 48 kHz: 112.87 samples)
    filt({{"kParamType", str("CombP")}, {"kParamFreq", num(0.5)}, {"kParamReso", num(90)}});
    {
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr, c.driveBoost = true;  // imports run the matched voicing
      std::vector<float> L(1024, 0.f), R(1024, 0.f);
      L[0] = R[0] = 1;
      for (int pos = 0; pos < 1024; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxFilter, slot0(st), L.data() + pos, R.data() + pos, 256, c, nullptr, 0);
      auto cen = [&](int a, int b) {
        double s = 0, w = 0;
        for (int i = a; i < b; ++i) s += L[static_cast<size_t>(i)], w += i * static_cast<double>(L[static_cast<size_t>(i)]);
        return w / s;
      };
      const double e1 = cen(108, 119), e2 = cen(221, 232);
      expect(std::fabs(e2 - e1 - 112.87) < 0.3, ("comb echo spacing at cutoff 426.6 Hz: " + std::to_string(e2 - e1) + " samples (source 112.87)").c_str());
    }
    expect(import(fxBody("FXComp", pl::FxComp - 1, {{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamRatio", num(1)}, {"kParamWet", num(50)}}), st, rep, cache),
           "multiband imports");
    double worst = 0;
    for (double hz : {133.75, 600.0, 2480.8}) worst = std::max(worst, std::fabs(sineGainDb(st, hz)));
    expect(worst < 0.5, ("multiband ratio 1, mix 50: flat through the crossovers (worst " + std::to_string(worst) + " dB)").c_str());
  }

  // ---- phaser sweep (J-203h). Source, the lowest notch tracked over time (feedback 0, FREQ 600, 4 poles): the
  // sweep is linear in Hz and only upward from the resting notches (about 870 Hz), up to 14 kHz at depth 100
  // and 7.5 kHz at depth 50. So a tone below the resting notch keeps its level while the sweep runs, and a
  // tone at 6 kHz is notched once per cycle at depth 100.
  {
    auto swing = [&](double hz) {
      expect(import(fxBody("FXPhaser", pl::FxPhaser - 1, {{"kParamEnable", num(1)}, {"kParamDepth", num(100)}, {"kParamFeedback", num(0)}, {"kParamRate", num(0.25)}}), st, rep, cache),
             "phaser imports");
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      const int n = static_cast<int>(8 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(6.283185307179586 * hz * i / kSr));
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxPhaser, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      double lo = 1e9, hi = -1e9;
      for (double t = 0.5; t < 7.9; t += 0.05) lo = std::min(lo, rmsDb(L, t, t + 0.05)), hi = std::max(hi, rmsDb(L, t, t + 0.05));
      return hi - lo;
    };
    const double s300 = swing(100), s6k = swing(6000);
    std::printf("# phaser depth 100 level swing: 100 Hz %.1f dB, 6 kHz %.1f dB\n", s300, s6k);
    expect(s300 < 3.0 && s6k > 10.0, ("the phaser sweeps upward only (swing 100 Hz " + std::to_string(s300) + " dB, 6 kHz " + std::to_string(s6k) + " dB)").c_str());
  }

  // ---- bus outputs (J-203h). Source, a bus carrying the oscillator with a clipper on the main rack: output 0
  // comes out clipped (-11.5 dB, through the main rack), 1 dry (-18.8 dB, direct), 2 clipped (into the other bus)
  {
    const double want[3] = {0, 1, 0};
    for (int d = 0; d < 3; ++d) {
      expect(import(obj({{"Global0", plain({{"kParamFXBus1Dest", num(d)}, {"kParamFXBus2Dest", num(d)}})}}), st, rep, cache), "bus output imports");
      expect(P(st, pl::G_Bus1Dest) == want[d] && P(st, pl::G_Bus2Dest) == want[d], ("bus output " + std::to_string(d) + " maps to " + std::to_string(want[d])).c_str());
    }
  }

  // ---- J-203q: delay, compressor and reverb pre-delay on the imported law. Expected numbers were measured on
  // the hosted source with deterministic white noise (impulse responses by cross-spectrum) and held sines.
  auto runSlot = [&](const PluginState& s, int type, std::vector<float>& L, std::vector<float>& R, double tempo = 120) {
    auto bank = pl::makeFxBank(s, nullptr, kSr);
    pl::FxContext c;
    c.sr = kSr, c.tempo = tempo;
    const int n = static_cast<int>(L.size());
    for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], type, slot0(s), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
  };
  auto peakAt = [](const std::vector<float>& x, int from, int to) {
    int best = from;
    for (int i = from; i < to && i < static_cast<int>(x.size()); ++i)
      if (std::fabs(x[static_cast<size_t>(i)]) > std::fabs(x[static_cast<size_t>(best)])) best = i;
    return best;
  };
  auto dly = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
    Json pp = plain({{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamBW", num(8.25)}, {"kParamBeatSync", num(0)}});
    for (const auto& e : kv) pp.object["plainParams"].object[e.first] = e.second;
    Json fx = arr({obj({{"FXDelay", pp}, {"type", num(pl::FxDelay - 1)}})});
    expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "delay imports");
  };
  {
    // free times: TIME 0.021 s (stored) echoes after 958 samples at 48 kHz, 0.1 s after 4750 (the stored value is
    // 0.001 + 0.5 t^4 and the echo comes 2 samples early)
    dly({{"kParamTimeL", num(0.021)}, {"kParamTimeR", num(0.1)}, {"kParamFeedback", num(0)}});
    expect(slot0Plain(st, "glLaw") == 1, "an imported delay plays on the imported law");
    std::vector<float> L(12000, 0.f), R(12000, 0.f);
    L[0] = R[0] = 1.f;
    runSlot(st, pl::FxDelay, L, R);
    const int eL = peakAt(L, 10, 12000), eR = peakAt(R, 10, 12000);
    expect(std::abs(eL - 958) <= 1 && std::abs(eR - 4750) <= 1, ("delay free echoes " + std::to_string(eL) + " / " + std::to_string(eR) + " (source 958 / 4750)").c_str());
    // synced: stored 0.12 is two beats (47998 at 120 BPM), 0.03 a quarter beat (5998)
    dly({{"kParamBeatSync", num(1)}, {"kParamTimeL", num(0.12)}, {"kParamTimeR", num(0.03)}, {"kParamFeedback", num(0)}});
    std::vector<float> L2(52000, 0.f), R2(52000, 0.f);
    L2[0] = R2[0] = 1.f;
    runSlot(st, pl::FxDelay, L2, R2);
    const int sL = peakAt(L2, 10, 52000), sR = peakAt(R2, 10, 52000);
    expect(std::abs(sL - 47998) <= 1 && std::abs(sR - 5998) <= 1, ("delay synced echoes " + std::to_string(sL) + " / " + std::to_string(sR) + " (source 47998 / 5998)").c_str());
    // feedback 50 %, L 0.021 / R 0.041 s: the left repeats every 959 samples at 0.395 / 0.884 (-7.0 dB), the
    // right (twice as long) at 0.197 / 0.883 (fb^2: the same decay per second)
    dly({{"kParamTimeL", num(0.021)}, {"kParamTimeR", num(0.041)}, {"kParamFeedback", num(50)}});
    std::vector<float> L3(8000, 0.f), R3(8000, 0.f);
    L3[0] = R3[0] = 1.f;
    runSlot(st, pl::FxDelay, L3, R3);
    const int a1 = peakAt(L3, 900, 1000), a2 = peakAt(L3, 1850, 1980), b1 = peakAt(R3, 1850, 1980), b2 = peakAt(R3, 3780, 3900);
    const double rl = std::fabs(L3[static_cast<size_t>(a2)] / L3[static_cast<size_t>(a1)]), rr = std::fabs(R3[static_cast<size_t>(b2)] / R3[static_cast<size_t>(b1)]);
    expect(a2 - a1 == 959 && std::fabs(rl - 0.447) < 0.05 && std::fabs(rr - 0.223) < 0.04,
           ("delay feedback: loop " + std::to_string(a2 - a1) + " samples, repeat ratios " + std::to_string(rl) + " / " + std::to_string(rr) + " (source 959, 0.447 / 0.223)").c_str());
  }
  {
    // echo band-pass: FREQ 1000 Hz, BW 0.75: the source is -6.2 dB one octave below, -4.6 dB one octave above
    // (the filter is designed for 44.1 kHz, so its centre sits at 1.09 kHz at 48 kHz) and -25.7 dB at 62.5 Hz
    dly({{"kParamTimeL", num(0.011)}, {"kParamTimeR", num(0.011)}, {"kParamFeedback", num(0)}, {"kParamBW", num(0.75)}, {"kParamFreq", num(1000)}});
    auto gainAt = [&](double hz) {
      const int n = 24000;
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(6.283185307179586 * hz * i / kSr));
      runSlot(st, pl::FxDelay, L, R);
      return rmsDb(L, 0.25, 0.5) - 20 * std::log10(0.1 / std::sqrt(2.0));
    };
    const double g500 = gainAt(500), g2k = gainAt(2000), g62 = gainAt(62.5);
    expect(std::fabs(g500 + 6.2) < 0.5 && std::fabs(g2k + 4.6) < 0.5 && std::fabs(g62 + 25.7) < 1.0,
           ("delay band-pass 500 Hz / 2 kHz / 62.5 Hz: " + std::to_string(g500) + " / " + std::to_string(g2k) + " / " + std::to_string(g62) + " dB (source -6.2 / -4.6 / -25.7)").c_str());
  }
  {
    // compressor on the imported law: a held sine whose peak is -6.1 dB in Plasma's units (-18.2 dB RMS at the
    // source's output), attack 0.1 ms, release 1000 ms: the source reduces it by 1.2 / 14.7 / 17.6 / 9.8 / 32.6 dB
    // at threshold 0.5 ratio 4, 0.75 ratio 4, 0.75 ratio 10, 0.75 ratio 2, 0.9 ratio 4
    auto red = [&](double t, double ratio, double makeup) {
      Json pp = plain({{"kParamEnable", num(1)}, {"kParamThresh", num(t)}, {"kParamRatio", num(ratio)}, {"kParamAttack", num(0.1)},
                       {"kParamRelease", num(1000)}, {"kParamMakeup", num(makeup)}});
      Json fx = arr({obj({{"FXComp", pp}, {"type", num(pl::FxComp - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "compressor imports");
      const int n = 48000;
      const double amp = std::pow(10.0, -6.1 / 20.0);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(6.283185307179586 * 65.4 * i / kSr));
      runSlot(st, pl::FxComp, L, R);
      return 20 * std::log10(amp / std::sqrt(2.0)) - rmsDb(L, 0.5, 0.95);
    };
    const double r1 = red(0.5, 4, 1), r2 = red(0.75, 4, 1), r3 = red(0.75, 10, 1), r4 = red(0.75, 2, 1), r6 = red(0.9, 4, 1);
    expect(std::fabs(r1 - 1.2) < 0.4 && std::fabs(r2 - 14.7) < 0.4 && std::fabs(r3 - 17.6) < 0.4 && std::fabs(r4 - 9.8) < 0.4 && std::fabs(r6 - 32.6) < 0.5,
           ("compressor reductions " + std::to_string(r1) + " / " + std::to_string(r2) + " / " + std::to_string(r3) + " / " + std::to_string(r4) + " / " +
            std::to_string(r6) + " dB (source 1.2 / 14.7 / 17.6 / 9.8 / 32.6)").c_str());
    // makeup 4 at threshold 0.25 ratio 4: the stored makeup is 1 + 30 M^2 and the gain 1 + 31 M^2 (+12.3 dB)
    const double rm = red(0.25, 4, 4), r0 = red(0.25, 4, 1);
    expect(std::fabs((r0 - rm) - 12.25) < 0.2, ("compressor makeup 4: +" + std::to_string(r0 - rm) + " dB (source +12.25)").c_str());
  }
  {
    // reverb pre-delay: stored 0.078125 (knob 0.5) waits 78 ms free on every type, a quarter beat synced
    auto onset = [&](const char* type, bool sync) {
      Json pp = plain({{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamType", str(type)}, {"kParamPreDelay", num(0.078125)},
                       {"kParamPreDelayBeatSync", num(sync ? 1 : 0)}});
      Json fx = arr({obj({{"FXReverb", pp}, {"type", num(pl::FxReverb - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "reverb imports");
      std::vector<float> L(24000, 0.f), R(24000, 0.f);
      for (int i = 0; i < 48; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = 0.5f;
      runSlot(st, pl::FxReverb, L, R);
      for (int i = 0; i < 24000; ++i)
        if (std::fabs(L[static_cast<size_t>(i)]) > 1e-5f || std::fabs(R[static_cast<size_t>(i)]) > 1e-5f) return i / kSr;
      return 1.0;
    };
    const double h = onset("kHall", false), hs = onset("kHall", true);
    expect(h > 0.074 && h < 0.085, ("hall free pre-delay " + std::to_string(h) + " s (source 0.078)").c_str());
    expect(hs > 0.12 && hs < 0.135, ("hall synced pre-delay " + std::to_string(hs) + " s (source a quarter beat, 0.125)").c_str());
  }
  {
    // distortion on the imported law: hard clip at drive 0 and the X-Shaper's straight line both give the wet 0.5
    // (the source's wet 0.25 m' on its input doubled); downsample at drive 100 holds round(0.002 sr) + 1 samples
    auto dist = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
      expect(import(fxBody("FXDistortion", pl::FxDistortion - 1, kv), st, rep, cache), "distortion imports");
    };
    dist({{"kParamEnable", num(1)}, {"kParamMode", str("kHardClip")}, {"kParamDrive", num(0)}});
    expect(slot0Plain(st, "glLaw") == 1, "an imported distortion plays on the imported law");
    std::vector<float> L(9600), R(9600);
    for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = static_cast<float>(0.3 * std::sin(6.283185307179586 * 200 * static_cast<double>(i) / kSr));
    const std::vector<float> in = L;
    runSlot(st, pl::FxDistortion, L, R);
    const double g = rmsDb(L, 0.05, 0.2) - rmsDb(in, 0.05, 0.2);
    expect(std::fabs(g + 6.02) < 0.1, ("imported hard clip at drive 0: " + std::to_string(g) + " dB (source -6.02)").c_str());
    dist({{"kParamEnable", num(1)}, {"kParamMode", str("kDownsample")}, {"kParamDrive", num(100)}});
    std::vector<float> L2(4800), R2(4800);
    for (size_t i = 0; i < L2.size(); ++i) L2[i] = R2[i] = static_cast<float>(0.3 * std::sin(6.283185307179586 * 50 * static_cast<double>(i) / kSr));
    runSlot(st, pl::FxDistortion, L2, R2);
    int changes = 0;
    for (size_t i = 1001; i < 3001; ++i) changes += L2[i] != L2[i - 1];
    expect(std::abs(changes - 2000 / 97) <= 1, ("imported downsample at drive 100: " + std::to_string(changes) + " steps in 2000 samples (source 20 or 21)").c_str());
  }
  {
    // convolution on the imported law: the impulse is peak-normalised to 0.125 (a unit impulse comes out at -18 dB)
    // and DECAY (stored 0.1 s) fades it by e^-6 over 0.1 s
    auto conv = [&](double decay) {
      Json body = fxBody("FXConv", pl::FxConvolve - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamIr", num(3)}, {"kParamDecay", num(decay)}});
      expect(import(body, st, rep, cache), "convolution imports");
      std::vector<float> L(24000, 0.f), R(24000, 0.f);
      L[0] = R[0] = 1.f;
      runSlot(st, pl::FxConvolve, L, R);
      return L;
    };
    const auto off = conv(40), dec = conv(0.1);
    expect(slot0Plain(st, "glLaw") == 1, "an imported convolution plays on the imported law");
    float pk = 0;
    for (float v : off) pk = std::max(pk, std::fabs(v));
    expect(std::fabs(pk - 0.125f) < 0.01f, ("imported impulse peak " + std::to_string(pk) + " (source 0.125)").c_str());
    auto win = [](const std::vector<float>& x, int a, int b) {
      double e = 0;
      for (int i = a; i < b; ++i) e += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
      return 10 * std::log10(e + 1e-30);
    };
    // the ratio of the late window to the early one drops by e^-12 (52 dB) in energy between 0.0 and 0.1 s
    const double drop = (win(off, 4800, 5280) - win(off, 0, 480)) - (win(dec, 4800, 5280) - win(dec, 0, 480));
    expect(std::fabs(drop - 52.1) < 2.0, ("imported convolution decay 0.1 s: " + std::to_string(drop) + " dB less at 0.1 s (source 52.1)").c_str());
  }
  {
    // chorus on the imported law, LFO stopped: DELAY 5 / DELAY 2 10 ms give taps at 246.5 and 486.5 samples (48 kHz),
    // half the input each; DEPTH 20 moves the right side's first tap a quarter of the swing (to 1400 samples)
    auto ch = [&](double depth) {
      Json pp = plain({{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamRate", num(0)}, {"kParamDepth", num(depth)}, {"kParamDelay", num(5)},
                       {"kParamDelay2", num(10)}, {"kParamFeedback", num(0)}, {"kParamFilt", num(15000)}});
      Json fx = arr({obj({{"FXChorus", pp}, {"type", num(pl::FxChorus - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "chorus imports");
    };
    ch(0);
    expect(slot0Plain(st, "glLaw") == 1, "an imported chorus plays on the imported law");
    std::vector<float> L(6000, 0.f), R(6000, 0.f);
    L[0] = R[0] = 1.f;
    runSlot(st, pl::FxChorus, L, R);
    auto sum2 = [&](const std::vector<float>& x, int i) { return x[static_cast<size_t>(i)] + x[static_cast<size_t>(i + 1)]; };
    const int t1 = peakAt(L, 200, 300), t2 = peakAt(L, 400, 600);
    expect((t1 == 246 || t1 == 247) && (t2 == 486 || t2 == 487) && std::fabs(sum2(L, 246) - 0.5f) < 0.12f,
           ("imported chorus taps " + std::to_string(t1) + " / " + std::to_string(t2) + " (source 246.5 / 486.5)").c_str());
    ch(20);
    std::vector<float> L2(6000, 0.f), R2(6000, 0.f);
    L2[0] = R2[0] = 1.f;
    runSlot(st, pl::FxChorus, L2, R2);
    const int r1 = peakAt(R2, 1000, 1500), l2 = peakAt(L2, 2500, 3000);
    expect(std::abs(r1 - 1400) <= 2 && std::abs(l2 - 2794) <= 2, ("imported chorus depth 20: taps " + std::to_string(r1) + " / " + std::to_string(l2) + " (source 1400 / 2794)").c_str());
  }
  {
    // reverb models on the imported law: the space is mono (a hard-left click gives the same wet on both sides),
    // the plate nearly vanishes at LO CUT 100 (its input high-pass removes almost everything), the hall's LO CUT
    // 100 cuts the lows by about 20 dB, and every type rings out (finite, decaying)
    auto rv = [&](const char* type, std::initializer_list<std::pair<const char*, Json>> kv) {
      Json pp = plain({{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamType", str(type)}});
      for (const auto& e : kv) pp.object["plainParams"].object[e.first] = e.second;
      Json fx = arr({obj({{"FXReverb", pp}, {"type", num(pl::FxReverb - 1)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "reverb imports");
    };
    rv("kSpace", {});
    std::vector<float> L(48000, 0.f), R(48000, 0.f);
    for (int i = 0; i < 48; ++i) L[static_cast<size_t>(i)] = 0.5f;
    runSlot(st, pl::FxReverb, L, R);
    double diff = 0, e = 0;
    for (size_t i = 0; i < L.size(); ++i) diff += std::fabs(L[i] - R[i]), e += std::fabs(L[i]);
    expect(e > 1.0 && diff < 1e-3 * e, ("imported space: mono wet (" + std::to_string(diff) + " of " + std::to_string(e) + ")").c_str());
    auto noiseDb = [&](double lpHz) {
      const int n = 96000;
      std::vector<float> a(static_cast<size_t>(n)), b2(static_cast<size_t>(n));
      uint32_t seed = 7;
      double z = 0;
      const double c1 = 1 - std::exp(-6.283185307179586 * lpHz / kSr);
      for (auto& v : a) seed = seed * 1664525u + 1013904223u, z += c1 * ((static_cast<float>(seed >> 8) / 16777216.f - 0.5f) - z), v = static_cast<float>(z);
      b2 = a;
      const double in = rmsDb(a, 0.5, 2.0);
      runSlot(st, pl::FxReverb, a, b2);
      return rmsDb(a, 0.5, 2.0) - in;
    };
    rv("kPlate", {{"kParamFreq", num(100)}});
    const double plate = noiseDb(2000);
    expect(plate < -25, ("imported plate at LO CUT 100: " + std::to_string(plate) + " dB (source -44 dB at 1 kHz, -28 at 8 kHz)").c_str());
    rv("kHall", {{"kParamFreq", num(0)}});
    const double h0 = noiseDb(60);
    rv("kHall", {{"kParamFreq", num(100)}});
    const double h1 = noiseDb(60);
    expect(h0 - h1 > 13 && h0 - h1 < 24, ("imported hall LO CUT 100 on lows: -" + std::to_string(h0 - h1) + " dB (source -20 at 60 Hz)").c_str());
    for (const char* t : {"kPlate", "kHall", "kVintage", "kAbyss", "kSpace"}) {
      rv(t, {});
      std::vector<float> a(96000, 0.f), b2(96000, 0.f);
      for (int i = 0; i < 480; ++i) a[static_cast<size_t>(i)] = b2[static_cast<size_t>(i)] = 0.5f * std::sin(0.1f * static_cast<float>(i));
      runSlot(st, pl::FxReverb, a, b2);
      bool fin = true;
      for (float v : a) fin = fin && std::isfinite(v);
      expect(fin && rmsDb(a, 0.05, 0.3) > rmsDb(a, 1.6, 2.0) + 10, (std::string("imported reverb rings out: ") + t).c_str());
    }
  }

  std::error_code ec;
  fs::remove_all(cache, ec);
  return finish();
}
