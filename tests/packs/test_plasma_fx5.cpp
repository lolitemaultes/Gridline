// Gridline — "plasma" pack tests: the imported laws of the phaser, flanger, frequency shifter,
// dimension and multiband compressor (J-203s). Synthetic .SerumPreset files written by this test
// check each law against plain numbers measured on the hosted source synth with the same settings
// (white noise or a held saw / sine in, the modulation stopped where noted); no preset content is used
// or shipped.
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
std::vector<float> render(const PluginState& st, int key, double seconds, double holdSec, bool playing = false, float vel = 0.8f) {
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
    if (pos == 0) g->noteOn(1, key, vel, 0);
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
// gain (dB) of slot 0's effect for a sine at hz on one side (0 left, 1 right), measured after 0.25 s
double sineGainCh(const PluginState& st, double hz, int ch, double amp = 0.1) {
  auto bank = pl::makeFxBank(st, nullptr, kSr);
  const int t = static_cast<int>(std::lround(P(st, pl::fxP(0, pl::X_Type))));
  if (!bank || !bank->unit[0]) return -999;
  pl::FxContext c;
  c.sr = kSr;
  const int n = static_cast<int>(0.5 * kSr);
  std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(6.283185307179586 * hz * i / kSr));
  for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], t, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
  return rmsDb(ch ? R : L, 0.25, 0.5) - 20 * std::log10(amp / std::sqrt(2.0));
}
// energy (dB) of x[a, b) at hz (Hann-windowed single-bin transform)
double binDb(const std::vector<float>& x, int a, int b, double hz) {
  double re = 0, im = 0;
  for (int i = a; i < b; ++i) {
    const double w = 0.5 - 0.5 * std::cos(6.283185307179586 * (i - a) / (b - a)), ph = 6.283185307179586 * hz * i / kSr;
    re += x[static_cast<size_t>(i)] * w * std::cos(ph), im -= x[static_cast<size_t>(i)] * w * std::sin(ph);
  }
  return 10 * std::log10(re * re + im * im + 1e-30);
}
}  // namespace

int main() {
  const fs::path cache =
      fs::temp_directory_path() / ("gl-plasma-fx5-test-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
  fs::create_directories(cache);
#ifdef _WIN32
  _putenv_s("GRIDLINE_CACHE_DIR", cache.string().c_str());
#else
  setenv("GRIDLINE_CACHE_DIR", cache.string().c_str(), 1);
#endif
  PluginState st;
  pl::ImportReport rep;
  auto check = [&](double got, double want, double tol, const std::string& what) {
    expect(std::fabs(got - want) <= tol, (what + ": " + std::to_string(got) + " dB (source " + std::to_string(want) + ")").c_str());
  };

  // ---- phaser, sweep stopped (RATE 0, DEPTH 0). Source, white noise in, transfer per frequency:
  // one pole at FREQ 566 (DEPTH 2 0): +1.56 dB at 200 Hz, -11.49 at 1000, -2.63 at 2000;
  // four poles at FREQ 600, DEPTH 2 0.5: -10.35 dB at 800, +1.72 at 1200, -10.53 at 2000, -7.29 at 6000
  {
    expect(import(fxBody("FXPhaser", pl::FxPhaser - 1, {{"kParamEnable", num(1)}, {"kParamRate", num(0)}, {"kParamDepth", num(0)}, {"kParamNumPoles", num(1)},
                                                        {"kParamDepth2", num(0)}, {"kParamFreq", num(566)}, {"kParamFeedback", num(0)}, {"kParamWet", num(100)}}), st, rep, cache), "phaser imports");
    expect(slot0Plain(st, "glLaw") == 1, "an imported phaser plays on the imported law");
    check(sineGainCh(st, 200, 0), 1.56, 0.5, "phaser one pole at 200 Hz");
    check(sineGainCh(st, 1000, 0), -11.49, 0.8, "phaser one pole at 1 kHz");
    check(sineGainCh(st, 2000, 0), -2.63, 0.5, "phaser one pole at 2 kHz");
    expect(import(fxBody("FXPhaser", pl::FxPhaser - 1, {{"kParamEnable", num(1)}, {"kParamRate", num(0)}, {"kParamDepth", num(0)}, {"kParamNumPoles", num(4)},
                                                        {"kParamDepth2", num(0.5)}, {"kParamFreq", num(600)}, {"kParamFeedback", num(0)}, {"kParamWet", num(100)}}), st, rep, cache), "phaser imports");
    check(sineGainCh(st, 800, 0), -10.35, 2.0, "phaser four poles at 800 Hz (a notch: the noise reading smears its bottom)");
    check(sineGainCh(st, 1200, 0), 1.72, 0.5, "phaser four poles at 1.2 kHz");
    check(sineGainCh(st, 2000, 0), -10.53, 2.0, "phaser four poles at 2 kHz (a notch)");
    check(sineGainCh(st, 6000, 0), -7.29, 1.0, "phaser four poles at 6 kHz");
    // Plasma's own effects keep their Classic law (LAW defaults to Classic, so older projects sound the same)
    for (int t : {pl::FxPhaser, pl::FxFlanger, pl::FxHyper, pl::FxBode, pl::FxComp})
      expect(pl::fxDefaultNorm(t, pl::fxParamIndex(t, "glLaw")) == 0.f, "a new or older effect plays on the Classic law");
  }

  // ---- flanger, RATE 0 holds the sweep at DEPTH: left at depth 50 %, right (PHASE 180) at the shortest delay.
  // Source, white noise in: left -5.27 dB at 200 Hz, -11.54 at 270, +1.84 at 540, -9.69 at 800;
  // right -7.00 dB at 3 kHz, +2.20 at 7.4 kHz. Feedback 60: left +5.25 dB at 540, -6.82 at 800.
  {
    auto fl = [&](double fb) {
      expect(import(fxBody("FXFlanger", pl::FxFlanger - 1, {{"kParamEnable", num(1)}, {"kParamRate", num(0)}, {"kParamDepth", num(50)}, {"kParamFeedback", num(fb)}, {"kParamWet", num(100)}}),
                    st, rep, cache), "flanger imports");
    };
    fl(0);
    expect(slot0Plain(st, "glLaw") == 1, "an imported flanger plays on the imported law");
    check(sineGainCh(st, 200, 0), -5.27, 0.5, "flanger left at 200 Hz");
    check(sineGainCh(st, 270, 0), -11.54, 1.0, "flanger left at 270 Hz");
    check(sineGainCh(st, 540, 0), 1.84, 0.5, "flanger left at 540 Hz");
    check(sineGainCh(st, 800, 0), -9.69, 3.0, "flanger left at 800 Hz (a notch)");
    check(sineGainCh(st, 3000, 1), -7.00, 0.8, "flanger right at 3 kHz");
    check(sineGainCh(st, 7400, 1), 2.20, 0.5, "flanger right at 7.4 kHz");
    fl(60);
    check(sineGainCh(st, 540, 0), 5.25, 0.8, "flanger feedback 60 at 540 Hz");
    check(sineGainCh(st, 800, 0), -6.82, 1.0, "flanger feedback 60 at 800 Hz");
  }

  // ---- convolution IR GAIN. Source (cab impulse, mix 100): the unset value equals -18 dB; 0 / +6 dB play
  // 18 / 24 dB louder than unset, -5 dB 13 dB louder
  {
    expect(import(fxBody("FXConv", pl::FxConvolve - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamIr", num(3)}}), st, rep, cache), "convolution imports");
    expect(std::fabs(slot0Plain(st, "kParamIpTrim") + 18.0) < 0.01, "an unset IR GAIN imports as -18 dB");
    const double g0 = sineGainCh(st, 440, 0);
    expect(import(fxBody("FXConv", pl::FxConvolve - 1, {{"kParamEnable", num(1)}, {"kParamWet", num(100)}, {"kParamIr", num(3)}, {"kParamIpTrim", num(-5)}}), st, rep, cache),
           "convolution imports");
    const double g1 = sineGainCh(st, 440, 0);
    check(g1 - g0, 13.0, 0.3, "IR GAIN -5 dB over unset");
  }

  // ---- frequency shifter. Source, a 440 Hz sine: SHIFT 100 %, RANGE 100 Hz, DIR -100 % gives 340 Hz on both
  // sides (the other sideband below -60 dB); SHIFT 50 %, DIR unset: left 390 Hz, right 490 Hz
  {
    auto bode = [&](std::initializer_list<std::pair<const char*, Json>> kv, double rightHz = 440) {
      expect(import(fxBody("FXBode", pl::FxBode - 1, kv), st, rep, cache), "frequency shifter imports");
      auto bank = pl::makeFxBank(st, nullptr, kSr);
      pl::FxContext c;
      c.sr = kSr;
      const int n = static_cast<int>(0.6 * kSr);
      std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        L[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(6.283185307179586 * 440 * i / kSr));
        R[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(6.283185307179586 * rightHz * i / kSr));
      }
      for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxBode, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
      return std::make_pair(L, R);
    };
    const int a = static_cast<int>(0.2 * kSr), b = static_cast<int>(0.6 * kSr);
    auto lr = bode({{"kParamEnable", num(1)}, {"kParamShift", num(100)}, {"kParamRange", num(100)}, {"kParamOutputMix", num(-100)}, {"kParamWet", num(100)}});
    expect(slot0Plain(st, "glLaw") == 1, "an imported frequency shifter plays on the imported law");
    expect(binDb(lr.first, a, b, 340) - binDb(lr.first, a, b, 540) > 45 && binDb(lr.second, a, b, 340) - binDb(lr.second, a, b, 540) > 45,
           "shift +100 % of 100 Hz, DIR -100 %: 340 Hz on both sides, the other sideband 45 dB down");
    lr = bode({{"kParamEnable", num(1)}, {"kParamShift", num(50)}, {"kParamRange", num(100)}});
    expect(binDb(lr.first, a, b, 390) - binDb(lr.first, a, b, 490) > 45 && binDb(lr.second, a, b, 490) - binDb(lr.second, a, b, 390) > 45,
           "shift +50 Hz, DIR unset: down on the left (390 Hz), up on the right (490 Hz)");
    expect(binDb(lr.first, a, b, 390) - binDb(lr.first, a, b, 440) > 40, "the unset mix is fully wet (no 440 Hz left)");
    // Source, a different signal per side: with no shift each side keeps its own input; DIR -100 % sends the same
    // (mono) signal to both sides; BLUR 74 % alone parts the sides (correlation 0.91 on a saw)
    lr = bode({{"kParamEnable", num(1)}, {"kParamShift", num(0)}, {"kParamWet", num(100)}}, 660);
    expect(binDb(lr.first, a, b, 440) - binDb(lr.first, a, b, 660) > 40 && binDb(lr.second, a, b, 660) - binDb(lr.second, a, b, 440) > 40,
           "no shift: each side keeps its own input");
    lr = bode({{"kParamEnable", num(1)}, {"kParamShift", num(20)}, {"kParamRange", num(100)}, {"kParamOutputMix", num(-100)}, {"kParamWet", num(100)}}, 660);
    double dmax = 0;
    for (int i = a; i < b; ++i) dmax = std::max(dmax, static_cast<double>(std::fabs(lr.first[static_cast<size_t>(i)] - lr.second[static_cast<size_t>(i)])));
    expect(dmax < 1e-6, "DIR -100 %: the same signal on both sides");
    lr = bode({{"kParamEnable", num(1)}, {"kParamShift", num(0)}, {"kParamWet", num(100)}, {"kParamBlur", num(74)}});
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = a; i < b; ++i) {
      const double x = lr.first[static_cast<size_t>(i)], y = lr.second[static_cast<size_t>(i)];
      sxy += x * y, sxx += x * x, syy += y * y;
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    expect(corr > 0.6 && corr < 0.97, ("blur 74 %, no shift: the sides part (correlation " + std::to_string(corr) + ")").c_str());
  }

  // ---- dimension (inside the hyper module, UNISON 1 so the voices pass the input): the mid is untouched and
  // the side is four echoes of the mono sum, the first 15 ms + 100 ms x SIZE late
  {
    expect(import(fxBody("FXHyperD", pl::FxHyper - 1, {{"kParamEnable", num(1)}, {"kParamUnison", num(1)}, {"kParamDimEWet", num(100)}, {"kParamDimESize", num(0)}}), st, rep, cache),
           "hyper imports");
    expect(slot0Plain(st, "glLaw") == 1, "an imported hyper plays on the imported law");
    auto bank = pl::makeFxBank(st, nullptr, kSr);
    pl::FxContext c;
    c.sr = kSr;
    const int n = 12000;
    std::vector<float> L(n, 0.f), R(n, 0.f);
    L[0] = R[0] = 1.f;
    for (int pos = 0; pos < n; pos += 256) pl::fxProcess(*bank->unit[0], pl::FxHyper, slot0(st), L.data() + pos, R.data() + pos, std::min(256, n - pos), c, nullptr, 0);
    double mid = 0, side = 0;
    int first = -1;
    for (int i = 1; i < n; ++i) {
      mid += std::fabs(L[static_cast<size_t>(i)] + R[static_cast<size_t>(i)]);
      const double s = std::fabs(L[static_cast<size_t>(i)] - R[static_cast<size_t>(i)]);
      side += s;
      if (first < 0 && s > 1e-3) first = i;
    }
    expect(std::fabs(L[0] + R[0] - 2.f) < 1e-4f && mid < 1e-3, "dimension leaves the mid untouched");
    expect(side > 0.1 && std::abs(first - 720) <= 2, ("dimension's first side echo at 15 ms: sample " + std::to_string(first)).c_str());
  }

  // ---- multiband compressor. Source, the default saw held at key 48 (velocity 1, attack 0), held level change against the unset
  // multiband (threshold 0.5, ratio 4): THRESH 0.7 -22.7 dB; THRESH 0.6, RATIO 10, BELOW 0.6, THRESH L 150 %,
  // THRESH H 50 %, M +6 dB -3.6 dB; THRESH 0.8, RATIO 100, MAKEUP 4, MIX 50 % +2.0 dB; THRESH 0.3,
  // RATIO 1e6, RELEASE 300 ms +7.4 dB (this patch sits 0.4 dB below the measured one, which moves the two
  // limiter-end cases by about 1 dB)
  {
    auto lev = [&](std::initializer_list<std::pair<const char*, Json>> kv) {
      Json b = fxBody("FXComp", pl::FxComp - 1, kv);
      b.object["Env0"] = plain({{"kParamAttack", num(0)}, {"kParamRelease", num(0.01)}});
      expect(import(b, st, rep, cache), "multiband imports");
      const auto x = render(st, 48, 1.0, 1.0, false, 1.0f);
      expect(finite(x), "multiband output is finite");
      return rmsDb(x, 0.4, 0.95);
    };
    const double base = lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}});
    expect(slot0Plain(st, "glLaw") == 1, "an imported compressor plays on the imported law");
    check(lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamThresh", num(0.7)}, {"kParamRatio", num(4)}}) - base, -22.7, 0.5, "multiband threshold 0.7");
    check(lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamRatioBelow", num(0.6)}, {"kParamThresh", num(0.6)}, {"kParamRatio", num(10)},
               {"kParamThreshUD0", num(150)}, {"kParamThreshUD2", num(50)}, {"kParamGain1", num(6)}}) - base, -3.6, 0.5, "multiband bands and below ratio");
    check(lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamWet", num(50)}, {"kParamThresh", num(0.8)}, {"kParamRatio", num(100)}, {"kParamMakeup", num(4)}}) - base,
          2.0, 1.5, "multiband limiter end, makeup and mix");
    check(lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamThresh", num(0.3)}, {"kParamRatio", num(1e6)}, {"kParamRelease", num(300)}}) - base, 7.4, 1.5,
          "multiband threshold 0.3 at the limiter end");
    // the per-band switches arrive packed in LAW and turn the bands' downward part off
    const double down = lev({{"kParamEnable", num(1)}, {"kParamMultiband", num(1)}, {"kParamThresh", num(0.7)}, {"kParamRatio", num(4)},
                             {"kParamBypassDown1", num(1)}, {"kParamBypassDown2", num(1)}, {"kParamBypassDown3", num(1)}});
    expect(slot0Plain(st, "glLaw") == 1, "LAW still reads Imported with the switches packed");
    expect(down - base > 0.5, ("downward parts off: no compression at threshold 0.7 (" + std::to_string(down - base) + " dB over the unset)").c_str());
  }

  // ---- extremes stay finite (phaser 18 poles, full depth and feedback; flanger full feedback; shifter blur
  // and feedback; multiband full upward)
  {
    const struct {
      const char* name;
      int type;
      std::vector<std::pair<const char*, double>> kv;
    } cases[] = {
        {"FXPhaser", pl::FxPhaser - 1, {{"kParamNumPoles", 18}, {"kParamDepth", 100}, {"kParamDepth2", 1}, {"kParamFeedback", 100}, {"kParamRate", 20}, {"kParamFreq", 18000}}},
        {"FXFlanger", pl::FxFlanger - 1, {{"kParamFeedback", 100}, {"kParamDepth", 100}, {"kParamRate", 20}}},
        {"FXBode", pl::FxBode - 1, {{"kParamFeedback", 100}, {"kParamBlur", 100}, {"kParamShift", -100}, {"kParamRange", 5000}, {"kParamDelayTime", 0.0005}}},
        {"FXComp", pl::FxComp - 1, {{"kParamMultiband", 1}, {"kParamRatioBelow", 1}, {"kParamThresh", 1}, {"kParamGain0", 24}, {"kParamGain1", 24}, {"kParamGain2", 24}}},
    };
    for (const auto& cs : cases) {
      Json pp = Json::makeObject();
      pp.object["kParamEnable"] = num(1);
      for (const auto& kv : cs.kv) pp.object[kv.first] = num(kv.second);
      Json fx = arr({obj({{cs.name, obj({{"plainParams", pp}})}, {"type", num(cs.type)}})});
      expect(import(obj({{"FXRack0", obj({{"FX", fx}})}}), st, rep, cache), "extreme preset imports");
      expect(finite(render(st, 48, 0.8, 0.5)), (std::string(cs.name) + " at its extremes stays finite").c_str());
    }
  }

  std::error_code ec;
  fs::remove_all(cache, ec);
  return finish();
}
