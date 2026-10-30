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
double sineGainDb(const PluginState& st, double hz) {
  auto bank = pl::makeFxBank(st, nullptr, kSr);
  const int t = static_cast<int>(std::lround(P(st, pl::fxP(0, pl::X_Type))));
  if (!bank || !bank->unit[0]) return -999;
  pl::FxContext c;
  c.sr = kSr;
  const int n = static_cast<int>(0.5 * kSr);
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(6.283185307179586 * hz * i / kSr));
  for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], t, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
  return rmsDb(L, 0.25, 0.5) - 20 * std::log10(0.1 / std::sqrt(2.0));
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

  std::error_code ec;
  fs::remove_all(cache, ec);
  return finish();
}
