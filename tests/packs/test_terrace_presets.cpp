// Gridline — Terrace's factory presets, measured. Every preset runs over a fixed source made with
// Gridline's own instruments (drums, a bass line, a held pad, the three mixed) and must stay
// finite, free of denormals and clicks (no high-frequency burst at a step edge beyond the
// source's own), and within its loudness bounds against the bypassed source. Old projects that
// use Terrace render exactly as before (a saved project and the six first factory states, against
// their recorded levels).
//
// `gridline_test_terrace_presets --harness <dir> [--only <name part>]` is the long listening
// harness: every preset at 90, 120 and 140 BPM over each source; it writes the renders (WAV), a
// picture per preset (waveforms and each band's level and pan against the beat grid, PNG) and a
// table of the measurements (clicks, gate edge timing against the grid, loudness against bypass,
// DC, band activity, CPU per block).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/Effects.h"
#include "engine/Render.h"
#include "engine/ShaperSteps.h"
#include "packs/terrace/terrace.h"

extern "C" {
typedef void stbi_write_func(void* context, void* data, int size);
int stbi_write_png_to_func(stbi_write_func* func, void* context, int w, int h, int comp, const void* data, int stride_in_bytes);
}

using namespace gl;
namespace tr = gl::packs::terrace;

namespace {

constexpr int kSr = 48000;
constexpr int kBars = 4;

struct Stereo {
  std::vector<float> L, R;
  size_t size() const { return L.size(); }
};

// ------------------------------------------------------------------ sources (Gridline's own instruments)

enum SrcKind { SkDrums = 0, SkBass, SkPad, SkMix, SkCount };
const char* const kSrcName[SkCount] = {"drums", "bass", "pad", "mix"};

Stereo renderSongPart(int kind, double tempo) {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), tempo, false);
  const Song& s = d.song();
  const int ppq = s.ppq;
  const Tick step = ppq / 4;
  const ObjectId pat = d.addPattern("Source", kBars);
  auto steps = [&](ObjectId ch, const std::function<int(int)>& vel) {
    std::vector<uint8_t> row(static_cast<size_t>(16 * kBars), 0);
    for (int k = 0; k < 16 * kBars; ++k) row[static_cast<size_t>(k)] = static_cast<uint8_t>(vel(k));
    d.setSteps(pat, ch, row);
  };
  using namespace pp;
  if (kind == SkDrums) {
    const ObjectId kick = d.addChannel("Kick", "gl.kick"), clap = d.addChannel("Clap", "gl.clap"), hat = d.addChannel("Hat", "gl.hat"),
                   open = d.addChannel("Open hat", "gl.hat"), snare = d.addChannel("Snare", "gl.snare");
    d.setParam(channelPluginParam(open, HatOpen), 1);
    d.setParam(channelPluginParam(open, HatDecay), 0.14);
    steps(kick, [](int k) { return k % 4 == 0 || (k % 32 == 30) ? 118 : 0; });
    steps(clap, [](int k) { return k % 8 == 4 ? 104 : 0; });
    steps(snare, [](int k) { return k % 64 >= 60 ? 70 + (k % 4) * 8 : (k % 16 == 15 ? 45 : 0); });
    steps(hat, [](int k) { return k % 2 == 0 ? (k % 4 == 0 ? 60 : 78) : (k % 16 > 8 ? 38 : 0); });
    steps(open, [](int k) { return k % 4 == 2 ? 72 : 0; });
  } else if (kind == SkBass) {
    const ObjectId b = d.addChannel("Bass", "gl.synth");
    d.setParam(channelPluginParam(b, SynOsc2Coarse), 0);
    d.setParam(channelPluginParam(b, SynOsc2Level), 0.5);
    d.setParam(channelPluginParam(b, SynCutoff), 800);
    d.setParam(channelPluginParam(b, SynFilterEnv), 0.55);
    d.setParam(channelPluginParam(b, SynFilterDecay), 0.2);
    d.setParam(channelPluginParam(b, SynVoices), 1);
    d.setParam(channelPluginParam(b, SynRelease), 0.06);
    const int line[16] = {33, 33, 45, 33, 36, 36, 48, 31, 29, 29, 41, 29, 31, 31, 43, 38};
    std::vector<Note> ns;
    for (int i = 0; i < 8 * kBars; ++i) {
      Note n;
      n.start = static_cast<Tick>(i) * 2 * step;
      n.length = i % 2 ? step : step + step / 2;
      n.pitch = line[(i / 2) % 16];
      n.velocity = i % 4 == 0 ? 112 : 92;
      ns.push_back(n);
    }
    d.setNotes(pat, b, ns);
  } else {  // the pad: two detuned saws, slow attack, a chord a bar
    const ObjectId p = d.addChannel("Pad", "gl.synth");
    d.setParam(channelPluginParam(p, SynOsc2Coarse), 0);
    d.setParam(channelPluginParam(p, SynOsc2Fine), 9);
    d.setParam(channelPluginParam(p, SynOsc2Level), 0.8);
    d.setParam(channelPluginParam(p, SynAttack), 0.25);
    d.setParam(channelPluginParam(p, SynSustain), 0.9);
    d.setParam(channelPluginParam(p, SynRelease), 0.6);
    d.setParam(channelPluginParam(p, SynCutoff), 2600);
    d.setParam(channelPluginParam(p, SynFilterEnv), 0.1);
    d.setParam(channelPluginParam(p, SynVoices), 8);
    const int chords[4][4] = {{57, 60, 64, 69}, {53, 57, 60, 65}, {48, 55, 60, 64}, {55, 59, 62, 67}};
    std::vector<Note> ns;
    for (int c = 0; c < kBars; ++c)
      for (int k = 0; k < 4; ++k) {
        Note n;
        n.start = static_cast<Tick>(c) * s.barTicks();
        n.length = s.barTicks() - step / 2;
        n.pitch = chords[c % 4][k];
        n.velocity = 90;
        ns.push_back(n);
      }
    d.setNotes(pat, p, ns);
  }
  d.addClip(pat, 0, 0);
  RenderOptions o;
  o.sampleRate = kSr;
  o.tail = static_cast<Tick>(ppq) * 4;
  const AudioBuffer b = renderSong(d.song(), o);
  const size_t n = static_cast<size_t>(std::llround(kBars * 4 * 60.0 / tempo * kSr));
  Stereo out;
  out.L.assign(n, 0.f), out.R.assign(n, 0.f);
  for (size_t i = 0; i < n && i < b.frames; ++i) out.L[i] = b.data[i], out.R[i] = b.channels > 1 ? b.data[b.frames + i] : b.data[i];
  return out;
}

void normalize(Stereo& s, double peak) {
  double p = 1e-9;
  for (size_t i = 0; i < s.size(); ++i) p = std::max({p, static_cast<double>(std::fabs(s.L[i])), static_cast<double>(std::fabs(s.R[i]))});
  const float g = static_cast<float>(peak / p);
  for (size_t i = 0; i < s.size(); ++i) s.L[i] *= g, s.R[i] *= g;
}

Stereo makeSource(int kind, double tempo) {
  Stereo s;
  if (kind == SkMix) {
    Stereo a = makeSource(SkDrums, tempo), b = makeSource(SkBass, tempo), c = makeSource(SkPad, tempo);
    s = a;
    for (size_t i = 0; i < s.size(); ++i) s.L[i] += 0.75f * b.L[i] + 0.45f * c.L[i], s.R[i] += 0.75f * b.R[i] + 0.45f * c.R[i];
  } else {
    s = renderSongPart(kind, tempo);
  }
  normalize(s, 0.5);  // -6 dBFS peaks
  return s;
}

// ------------------------------------------------------------------ running Terrace

Stereo run(const PluginState& st, const Stereo& in, double tempo, int block = 256, double* nsPerBlock = nullptr) {
  auto fx = createEffect(tr::kType);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.tempo = tempo;
  ctx.playing = true;
  Stereo out = in;
  double ns = 0;
  int blocks = 0;
  for (size_t at = 0; at < in.size(); at += static_cast<size_t>(block)) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), in.size() - at));
    ctx.tick = static_cast<double>(at) / (kSr * 60.0 / tempo) * ctx.ppq;
    ctx.jumped = at == 0;
    const auto t0 = std::chrono::steady_clock::now();
    fx->process(st, ctx, out.L.data() + at, out.R.data() + at, n);
    ns += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    ++blocks;
  }
  if (nsPerBlock) *nsPerBlock = ns / std::max(1, blocks);
  return out;
}

// ------------------------------------------------------------------ measurements

struct Biquad {
  double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  double p(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    return y;
  }
};
Biquad highPass(double hz, double q) {
  const double w = 2 * 3.14159265358979323846 * hz / kSr, cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
  return Biquad{(1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0};
}

// Integrated loudness (K-weighted mean square, ungated), in dB.
double loudness(const Stereo& s) {
  double sum = 0;
  for (int c = 0; c < 2; ++c) {
    Biquad sh{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
    Biquad hp{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    const std::vector<float>& v = c ? s.R : s.L;
    for (float x : v) {
      const double y = hp.p(sh.p(x));
      sum += y * y;
    }
  }
  return -0.691 + 10 * std::log10(std::max(1e-20, sum / static_cast<double>(s.size())));
}

// High-frequency (above about 4 kHz) RMS in 1 ms windows, both channels.
std::vector<double> hfWindows(const Stereo& s) {
  const size_t w = kSr / 1000, nw = s.size() / w;
  std::vector<double> e(nw, 0);
  for (int c = 0; c < 2; ++c) {
    Biquad a = highPass(4000, 0.5412), b = highPass(4000, 1.3066);
    const std::vector<float>& v = c ? s.R : s.L;
    for (size_t i = 0; i < nw * w; ++i) {
      const double y = b.p(a.p(v[i]));
      e[i / w] += y * y;
    }
  }
  for (double& x : e) x = std::sqrt(x / (2.0 * static_cast<double>(w)));
  return e;
}

struct Clicks {
  double worstDb = -99;   // the worst 1 ms window: high frequencies of the output over the source's
  int over = 0;           // windows more than 6 dB over
  double jumpRatio = 0;   // largest sample-to-sample step of the output over the source's
  double worstAtMs = 0;
};

// `gain`: the most the preset can raise any band (output, band gains), linear.
Clicks clicks(const Stereo& dry, const Stereo& wet, double gain) {
  Clicks r;
  const std::vector<double> hd = hfWindows(dry), hw = hfWindows(wet);
  const double floor = std::pow(10.0, -66.0 / 20);
  for (size_t i = 2; i + 2 < hd.size(); ++i) {
    double ref = 0;
    for (size_t j = i - 2; j <= i + 2; ++j) ref = std::max(ref, hd[j]);
    const double db = 20 * std::log10((hw[i] + 1e-12) / (ref * gain + floor));
    if (db > r.worstDb) r.worstDb = db, r.worstAtMs = static_cast<double>(i);
    if (db > 6) ++r.over;
  }
  double jd = 1e-9, jw = 0;
  for (size_t i = 1; i < dry.size(); ++i) {
    jd = std::max({jd, static_cast<double>(std::fabs(dry.L[i] - dry.L[i - 1])), static_cast<double>(std::fabs(dry.R[i] - dry.R[i - 1]))});
    jw = std::max({jw, static_cast<double>(std::fabs(wet.L[i] - wet.L[i - 1])), static_cast<double>(std::fabs(wet.R[i] - wet.R[i - 1]))});
  }
  r.jumpRatio = jw / (jd * gain);
  return r;
}

struct Health {
  int nonFinite = 0, denormal = 0;
  double dc = 0, peak = 0;
};
Health health(const Stereo& dry, const Stereo& wet) {
  Health h;
  double md = 0, mw = 0;
  for (size_t i = 0; i < wet.size(); ++i)
    for (int c = 0; c < 2; ++c) {
      const float x = c ? wet.R[i] : wet.L[i];
      if (!std::isfinite(x)) ++h.nonFinite;
      else if (x != 0 && std::fabs(x) < 1.17549435e-38f) ++h.denormal;
      mw += x, md += c ? dry.R[i] : dry.L[i];
      h.peak = std::max(h.peak, static_cast<double>(std::fabs(x)));
    }
  h.dc = std::fabs(mw - md) / (2.0 * static_cast<double>(wet.size()));
  return h;
}

double maxStaticGain(const PluginState& st) {
  double g = st.params[tr::TrOutput];
  double bandMax = 0;
  for (int b = 0; b < tr::kBands; ++b) bandMax = std::max(bandMax, static_cast<double>(st.params[static_cast<size_t>(tr::bandParam(b, tr::BpGain))]));
  return std::pow(10.0, (g + bandMax) / 20);
}

// ------------------------------------------------------------------ per-band probes (white noise)

bool routeOnBand(const PluginState& st, int b) {
  for (int r = 0; r < tr::kRoutes; ++r) {
    const int src = static_cast<int>(std::lround(st.params[static_cast<size_t>(tr::routeParam(r, tr::RtSource))]));
    const int tg = static_cast<int>(std::lround(st.params[static_cast<size_t>(tr::routeParam(r, tr::RtTarget))]));
    const double amt = st.params[static_cast<size_t>(tr::routeParam(r, tr::RtAmount))];
    if (src != tr::SrcOff && amt != 0 && tg >= tr::bandTarget(b, 0) && tg < tr::bandTarget(b + 1, 0)) return true;
  }
  return false;
}

// The preset with band b alone, and the same with b's lanes and routes at rest (Mix 100 %).
void probeStates(const PluginState& st, int b, PluginState& on, PluginState& rest) {
  on = st;
  on.params[tr::TrMix] = 1;
  for (int k = 0; k < tr::kBands; ++k) on.params[static_cast<size_t>(tr::bandParam(k, tr::BpSolo))] = k == b ? 1.f : 0.f, on.params[static_cast<size_t>(tr::bandParam(k, tr::BpMute))] = 0;
  for (int r = 0; r < tr::kRoutes; ++r) {  // routes onto the output or mix would hide the band's own movement
    const int tg = static_cast<int>(std::lround(on.params[static_cast<size_t>(tr::routeParam(r, tr::RtTarget))]));
    if (tg == tr::TgOutput || tg == tr::TgMix) on.params[static_cast<size_t>(tr::routeParam(r, tr::RtSource))] = tr::SrcOff;
  }
  rest = on;
  for (int l = 0; l < tr::kLanes; ++l) rest.params[static_cast<size_t>(tr::laneParam(b, l, tr::LpDepth))] = 0;
  for (int r = 0; r < tr::kRoutes; ++r) {
    const int tg = static_cast<int>(std::lround(rest.params[static_cast<size_t>(tr::routeParam(r, tr::RtTarget))]));
    if (tg >= tr::bandTarget(b, 0) && tg < tr::bandTarget(b + 1, 0)) rest.params[static_cast<size_t>(tr::routeParam(r, tr::RtSource))] = tr::SrcOff;
  }
  rest.params[static_cast<size_t>(tr::bandParam(b, tr::BpGain))] = 0;
}

Stereo whiteNoise(size_t n) {
  Stereo s;
  s.L.resize(n), s.R.resize(n);
  uint32_t r = 12345;
  for (size_t i = 0; i < n; ++i) {
    r = r * 1664525u + 1013904223u;
    s.L[i] = s.R[i] = 0.2f + static_cast<float>(static_cast<int32_t>(r)) / 8.6e9f;  // the offset keeps the low band away from zero
  }
  return s;
}

struct BandTrace {
  std::vector<float> level, pan;  // per 1 ms: band level against its rest (linear), balance -1..1
  double rangeDb = 0, panRange = 0;
};

BandTrace bandTrace(const Stereo& on, const Stereo& rest) {
  BandTrace t;
  const size_t w = kSr / 1000, nw = on.size() / w;
  for (size_t k = 0; k < nw; ++k) {
    double l = 0, r = 0, ref = 0;
    for (size_t i = k * w; i < (k + 1) * w; ++i) l += on.L[i] * on.L[i], r += on.R[i] * on.R[i], ref += rest.L[i] * rest.L[i] + rest.R[i] * rest.R[i];
    t.level.push_back(static_cast<float>(std::sqrt((l + r) / std::max(ref, 1e-20))));
    t.pan.push_back(static_cast<float>((r - l) / std::max(l + r, 1e-20)));
  }
  // range over 10 ms averages (p5 to p95)
  std::vector<double> lv, pv;
  for (size_t k = 0; k + 10 <= t.level.size(); k += 10) {
    double a = 0, p = 0;
    for (size_t j = k; j < k + 10; ++j) a += t.level[j] * t.level[j], p += t.pan[j];
    lv.push_back(10 * std::log10(std::max(1e-4, a / 10))), pv.push_back(p / 10);
  }
  if (!lv.empty()) {
    std::sort(lv.begin(), lv.end()), std::sort(pv.begin(), pv.end());
    const size_t lo = lv.size() / 20, hi = lv.size() - 1 - lv.size() / 20;
    t.rangeDb = lv[hi] - lv[lo], t.panRange = pv[hi] - pv[lo];
  }
  return t;
}

// Gate edge timing: where the band's volume lane changes level, against where the grid says it
// should. Only bands whose volume lane is hard-stepped and nothing else moves the band.
struct Timing {
  int edges = 0;
  double worstMs = 0;  // the largest distance from the grid edge to where the gain starts to move
};

int patternAt(const PluginState& st, const tr::Bank& bank, double beat) {
  const int pat = std::clamp(static_cast<int>(std::lround(st.params[tr::TrPattern])), 0, 7);
  if (st.params[tr::TrChain] >= 0.5f && bank.chainLen > 0) {
    const double len = tr::kLengthBeats[std::clamp(static_cast<int>(std::lround(st.params[tr::TrLength])), 0, 3)];
    const int64_t slot = static_cast<int64_t>(std::floor(beat / len + 1e-9)) % bank.chainLen;
    if (bank.chain[slot] >= 0) return bank.chain[slot];
  }
  return pat;
}

Timing timing(const PluginState& st, int b, const Stereo& on, const Stereo& rest, double tempo) {
  Timing t;
  const tr::Bank* bank = nullptr;
  auto bk = std::make_unique<tr::Bank>();
  tr::parse(st.data.c_str(), *bk);
  bank = bk.get();
  auto P = [&](int k) { return static_cast<double>(st.params[static_cast<size_t>(k)]); };
  if (std::fabs(P(tr::laneParam(b, tr::LnVol, tr::LpCurve))) > 1e-4 || P(tr::laneParam(b, tr::LnVol, tr::LpDepth)) < 0.05 || routeOnBand(st, b)) return t;
  if (P(tr::laneParam(b, tr::LnCut, tr::LpDepth)) > 0) {  // a moving filter hides the gain
    for (int p = 0; p < tr::kPatterns; ++p)
      for (int k = 0; k < tr::kSteps; ++k)
        if (bank->val[p][b][tr::LnCut][k] < 1.f) return t;
  }
  const int steps = static_cast<int>(std::lround(P(tr::laneParam(b, tr::LnVol, tr::LpSteps)))) + 1;
  const int size = static_cast<int>(std::lround(P(tr::laneParam(b, tr::LnVol, tr::LpSize))));
  const double len = tr::kLengthBeats[std::clamp(static_cast<int>(std::lround(P(tr::TrLength))), 0, 3)];
  const double sb = size == 0 ? len / pp::kShaperPoints : pp::kShaperRateBeats[size];
  const double swing = P(tr::laneParam(b, tr::LnVol, tr::LpSwing)), depth = P(tr::laneParam(b, tr::LnVol, tr::LpDepth));
  const double spb = 60.0 / tempo * kSr;
  // the per-sample gain of the band (the two runs differ only by it)
  std::vector<double> g(on.size(), -1);
  double last = 1;
  for (size_t i = 0; i < on.size(); ++i) {
    const double l = std::fabs(rest.L[i]), r = std::fabs(rest.R[i]);
    if (std::max(l, r) > 2e-3) last = std::max(l > 2e-3 ? std::fabs(on.L[i]) / l : 0.0, r > 2e-3 ? std::fabs(on.R[i]) / r : 0.0);
    g[i] = last;
  }
  // the segments the grid plays: steps (swing on the second of each pair), repeats inside them
  struct Seg {
    double beat, level;
  };
  std::vector<Seg> segs;
  const double total = static_cast<double>(on.size()) / spb;
  const double m = 1 + 0.5 * swing;
  for (int64_t n = 0;; ++n) {
    const double start = (n % 2 == 0 ? static_cast<double>(n) : static_cast<double>(n - 1) + m) * sb;
    const double end = (n % 2 == 0 ? static_cast<double>(n) + (m - 1) : static_cast<double>(n + 1)) * sb + (n % 2 == 0 ? sb : 0);
    if (start >= total) break;
    const int pat = patternAt(st, *bank, start);
    const int k = static_cast<int>(n % steps);
    const shsteps::LaneRef ref{bank->chance[pat][b][tr::LnVol], bank->hits[pat][b][tr::LnVol], bank->anyChance[pat][b][tr::LnVol], bank->anyHits[pat][b][tr::LnVol],
                               b * tr::kLanes + tr::LnVol, pat, tr::laneRest(tr::LnVol)};
    const float* vals = bank->val[pat][b][tr::LnVol];
    auto lev = [&](double fr) { return shsteps::laneValue(ref, k, fr, n, steps, 0.0, [vals](int kk) { return static_cast<double>(vals[kk]); }); };
    const int hits = ref.anyHits ? std::max(1, static_cast<int>(ref.hits[k])) : 1;
    for (int h = 0; h < hits; ++h)
      for (int half = 0; half < (hits > 1 ? 2 : 1); ++half) {
        const double fr = (h + 0.5 * half) / hits;
        segs.push_back({start + (end - start) * fr, 1 - depth * (1 - lev(fr + 0.25 / hits))});
      }
  }
  // each change of level: the gain must start to move within 1 ms of the edge
  const double bandG = std::pow(10.0, P(tr::bandParam(b, tr::BpGain)) / 20) * std::pow(10.0, P(tr::TrOutput) / 20);
  for (size_t s = 1; s < segs.size(); ++s) {
    const double d = (segs[s].level - segs[s - 1].level) * bandG;
    if (std::fabs(d) < 0.1) continue;
    const double te = segs[s].beat * spb;
    const long i0 = static_cast<long>(std::floor(te));
    if (i0 < kSr / 50 || i0 + kSr / 100 >= static_cast<long>(g.size())) continue;
    // where the gain stood just before (it may still be settling from the edge before: take the trend)
    const double before = g[static_cast<size_t>(i0 - 1)], trend = g[static_cast<size_t>(i0 - 1)] - g[static_cast<size_t>(i0 - 2)];
    long onset = -1;
    for (long i = i0 - kSr / 200; i < i0 + kSr / 100; ++i) {
      const double expectNo = i < i0 ? g[static_cast<size_t>(i)] : before + trend * static_cast<double>(i - i0 + 1);
      if (i >= i0 - 1 && std::fabs(g[static_cast<size_t>(i)] - expectNo) > 0.02 * std::fabs(d) && (g[static_cast<size_t>(i)] - before) * d > 0) {
        onset = i;
        break;
      }
    }
    ++t.edges;
    const double err = onset < 0 ? 10.0 : std::fabs(static_cast<double>(onset) - te) * 1000.0 / kSr;
    t.worstMs = std::max(t.worstMs, err);
    if (err > 1 && std::getenv("TP_DEBUG")) std::printf("  band %d edge at beat %.4f (sample %.1f): onset %ld, err %.3f ms, levels %.3f -> %.3f\n", b, segs[s].beat, te, onset, err, segs[s - 1].level, segs[s].level);
  }
  return t;
}

// ------------------------------------------------------------------ pictures

struct Img {
  int w, h;
  std::vector<uint8_t> px;
  Img(int W, int H) : w(W), h(H), px(static_cast<size_t>(W * H * 3), 0) {}
  void put(int x, int y, uint32_t c, double a = 1) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = &px[static_cast<size_t>((y * w + x) * 3)];
    for (int k = 0; k < 3; ++k) {
      const double v = static_cast<double>((c >> (16 - 8 * k)) & 0xFF);
      p[k] = static_cast<uint8_t>(p[k] + (v - p[k]) * a);
    }
  }
  void fill(int x0, int y0, int x1, int y1, uint32_t c, double a = 1) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
      for (int x = std::max(0, x0); x < std::min(w, x1); ++x) put(x, y, c, a);
  }
  void vline(int x, int y0, int y1, uint32_t c, double a = 1) {
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) put(x, y, c, a);
  }
  bool save(const std::string& path) {
    std::vector<uint8_t> out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { auto* o = static_cast<std::vector<uint8_t>*>(ctx); o->insert(o->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size); },
                           &out, w, h, 3, px.data(), w * 3);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
  }
};

const uint32_t kBandCol[4] = {0xE07A5F, 0xE9C46A, 0x5FB3A8, 0x7AA2E0};

void grid(Img& im, int y0, int y1, double samplesPerBeat, size_t n, double xs) {
  for (int beat = 0; beat * samplesPerBeat < static_cast<double>(n); ++beat)
    for (int q = 0; q < 4; ++q) {
      const int x = static_cast<int>((beat + q * 0.25) * samplesPerBeat / xs);
      const double a = q ? 0.10 : beat % 4 == 0 ? 0.55 : 0.28;
      im.vline(x, y0, y1, 0xFFFFFF, a);
    }
}

void waveRow(Img& im, int y0, int hgt, const Stereo& dry, const Stereo& wet, double spb, uint32_t col) {
  const double xs = static_cast<double>(dry.size()) / im.w;
  im.fill(0, y0, im.w, y0 + hgt, 0x16181D);
  grid(im, y0, y0 + hgt - 1, spb, dry.size(), xs);
  const int mid = y0 + hgt / 2;
  for (int x = 0; x < im.w; ++x) {
    const size_t a = static_cast<size_t>(x * xs), b = std::min(dry.size(), static_cast<size_t>((x + 1) * xs));
    double dmx = 0, wmx = 0;
    for (size_t i = a; i < b; ++i) dmx = std::max({dmx, static_cast<double>(std::fabs(dry.L[i])), static_cast<double>(std::fabs(dry.R[i]))}),
                                 wmx = std::max({wmx, static_cast<double>(std::fabs(wet.L[i])), static_cast<double>(std::fabs(wet.R[i]))});
    const int dh = static_cast<int>(std::min(1.0, dmx) * (hgt / 2 - 2)), wh = static_cast<int>(std::min(1.0, wmx) * (hgt / 2 - 2));
    im.vline(x, mid - dh, mid + dh, 0x5A5F6B, 0.9);
    im.vline(x, mid - wh, mid + wh, col, 0.85);
  }
}

void bandRow(Img& im, int y0, int hgt, const BandTrace& t, double spb, size_t n, uint32_t col) {
  const double xs = static_cast<double>(n) / im.w;
  im.fill(0, y0, im.w, y0 + hgt, 0x121418);
  grid(im, y0, y0 + hgt - 1, spb, n, xs);
  const double msPerPx = xs / (kSr / 1000.0);
  int prevPan = -1;
  for (int x = 0; x < im.w; ++x) {
    const size_t k = std::min(t.level.size() - 1, static_cast<size_t>(x * msPerPx));
    const double lv = std::min(1.25, static_cast<double>(t.level[k]));
    const int top = y0 + hgt - 1 - static_cast<int>(lv / 1.25 * (hgt - 4));
    im.vline(x, top, y0 + hgt - 1, col, 0.75);
    const int py = y0 + hgt / 2 - static_cast<int>(t.pan[k] * (hgt / 2 - 3));
    if (prevPan >= 0) im.vline(x, prevPan, py, 0xFFFFFF, 0.9);
    prevPan = py;
  }
  im.fill(0, y0 + hgt - 1 - static_cast<int>(1 / 1.25 * (hgt - 4)), im.w, y0 + hgt - static_cast<int>(1 / 1.25 * (hgt - 4)), 0xFFFFFF, 0.25);  // unity
}

// ------------------------------------------------------------------ the presets

PluginState presetAt(size_t i) { return presetState(*findPlugin(tr::kType), i); }

struct Bounds {
  double lo, hi;
};
// Loudness against the bypassed source on the fixed mix: within about 1 dB, except the presets
// whose point is to take sound away (named here with how far they may go).
Bounds boundsFor(const std::string& name) {
  (void)name;  // every factory preset is level matched (the ducking ones cut only with a sidechain)
  return {-1.0, 1.0};
}

// ------------------------------------------------------------------ old projects

// A source made in code (independent of the instruments, so only Terrace can change its render):
// a kick each beat, noise hats on the eighths, a held chord of saws, 4 bars at 120.
Stereo plainSource() {
  const size_t n = static_cast<size_t>(kBars * 4 * kSr / 2);
  Stereo s;
  s.L.assign(n, 0.f), s.R.assign(n, 0.f);
  uint32_t r = 77;
  const double f[4] = {110, 130.81, 164.81, 220};
  double ph[4] = {0, 0.3, 0.6, 0.1};
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSr, beat = t * 2, inBeat = (beat - std::floor(beat)) / 2;
    const double kick = std::sin(2 * 3.14159265358979 * (50 * inBeat + 60 * (1 - std::exp(-inBeat * 30)) / 30)) * std::exp(-inBeat * 12);
    const double e8 = (beat * 2 - std::floor(beat * 2)) / 4;
    r = r * 1664525u + 1013904223u;
    const double hat = static_cast<double>(static_cast<int32_t>(r)) / 2147483648.0 * std::exp(-e8 * 120) * 0.3;
    double pad = 0;
    for (int k = 0; k < 4; ++k) {
      ph[k] += f[k] / kSr;
      ph[k] -= std::floor(ph[k]);
      pad += (2 * ph[k] - 1) * 0.08;
    }
    s.L[i] = static_cast<float>(kick * 0.5 + hat + pad), s.R[i] = static_cast<float>(kick * 0.5 + hat * 0.7 + pad);
  }
  return s;
}

// RMS of each half second of each side: what an old project's render must still give.
std::vector<double> fingerprint(const Stereo& s) {
  std::vector<double> v;
  const size_t w = kSr / 2;
  for (int c = 0; c < 2; ++c)
    for (size_t a = 0; a + w <= s.size(); a += w) {
      double e = 0;
      for (size_t i = a; i < a + w; ++i) e += static_cast<double>(c ? s.R[i] : s.L[i]) * (c ? s.R[i] : s.L[i]);
      v.push_back(std::sqrt(e / static_cast<double>(w)));
    }
  return v;
}

int harness(const std::string& dir, const std::string& only) {
  namespace fs = std::filesystem;
  fs::create_directories(dir + "/renders");
  fs::create_directories(dir + "/pictures");
  const PluginInfo* in = findPlugin(tr::kType);
  const double tempos[3] = {90, 120, 140};
  Stereo src[3][SkCount];
  for (int t = 0; t < 3; ++t)
    for (int k = 0; k < SkCount; ++k) src[t][k] = makeSource(k, tempos[t]);
  for (int k = 0; k < SkCount; ++k) {
    AudioBuffer b(2, src[1][k].size());
    std::copy(src[1][k].L.begin(), src[1][k].L.end(), b.plane(0)), std::copy(src[1][k].R.begin(), src[1][k].R.end(), b.plane(1));
    writeWav(dir + "/renders/_dry_" + kSrcName[k] + ".wav", b, kSr);
  }
  const Stereo nz120 = whiteNoise(src[1][0].size());
  FILE* tsv = std::fopen((dir + "/measurements.tsv").c_str(), "w");
  std::fprintf(tsv, "preset\tloud_drums\tloud_bass\tloud_pad\tloud_mix\tloud_min\tloud_max\tclick_worst_db\tclick_over\tjump_ratio\ttiming_edges\ttiming_worst_ms\tdc\tpeak\tnonfinite\tdenormal\tband_range_db\tband_pan\tus_per_block\n");
  std::printf("%-26s %6s %6s %6s %6s | %6s %4s %5s | %4s %6s | %s | %s\n", "preset", "drums", "bass", "pad", "mix", "click", "n>6", "jump", "edg", "tms", "band dB range (pan)", "us/blk");
  for (size_t p = 0; p < in->presets.size(); ++p) {
    const std::string name = in->presets[p].name;
    if (!only.empty() && name.find(only) == std::string::npos) continue;
    const PluginState st = presetAt(p);
    const double gmax = maxStaticGain(st);
    double loud120[SkCount] = {}, lmin = 99, lmax = -99, cw = -99, jr = 0, dcw = 0, pk = 0, tworst = 0;
    int cover = 0, nonf = 0, den = 0, edges = 0;
    Stereo wet120[SkCount];
    double usBlock = 0;
    for (int t = 0; t < 3; ++t)
      for (int k = 0; k < SkCount; ++k) {
        double ns = 0;
        const Stereo w = run(st, src[t][k], tempos[t], 256, &ns);
        const double dl = loudness(w) - loudness(src[t][k]);
        lmin = std::min(lmin, dl), lmax = std::max(lmax, dl);
        const Clicks c = clicks(src[t][k], w, gmax);
        if (c.worstDb > cw) cw = c.worstDb;
        cover += c.over, jr = std::max(jr, c.jumpRatio);
        const Health h = health(src[t][k], w);
        nonf += h.nonFinite, den += h.denormal, dcw = std::max(dcw, h.dc), pk = std::max(pk, h.peak);
        if (t == 1) {
          loud120[k] = dl, wet120[k] = w;
          if (k == SkMix) usBlock = ns / 1000.0;
        }
      }
    // per band: traces at 120 (noise), timing at each tempo
    const int bands = static_cast<int>(std::lround(st.params[tr::TrBands])) == 0 ? 3 : 4;
    BandTrace traces[tr::kBands];
    std::string rangeTxt;
    for (int b = 0; b < bands; ++b) {
      PluginState on, rest;
      probeStates(st, b, on, rest);
      for (int t = 0; t < 3; ++t) {
        const Stereo nz = whiteNoise(src[t][0].size());
        const Stereo a = run(on, nz, tempos[t]), r = run(rest, nz, tempos[t]);
        const Timing tm = timing(st, b, a, r, tempos[t]);
        edges += tm.edges, tworst = std::max(tworst, tm.worstMs);
        if (t == 1) traces[b] = bandTrace(a, r);
      }
      char buf[48];
      std::snprintf(buf, sizeof buf, "%s%4.1f(%3.1f)", b ? " " : "", traces[b].rangeDb, traces[b].panRange);
      rangeTxt += buf;
    }
    std::printf("%-26s %+6.2f %+6.2f %+6.2f %+6.2f | %+6.1f %4d %5.2f | %4d %6.3f | %s | %.1f\n", name.c_str(), loud120[0], loud120[1], loud120[2], loud120[3], cw, cover, jr, edges,
                tworst, rangeTxt.c_str(), usBlock);
    std::fprintf(tsv, "%s\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.1f\t%d\t%.2f\t%d\t%.3f\t%.2g\t%.3f\t%d\t%d\t%s\t\t%.1f\n", name.c_str(), loud120[0], loud120[1], loud120[2], loud120[3], lmin, lmax, cw,
                 cover, jr, edges, tworst, dcw, pk, nonf, den, rangeTxt.c_str(), usBlock);
    // renders and the picture
    std::string file = name;
    for (char& ch : file)
      if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    char idx[8];
    std::snprintf(idx, sizeof idx, "%02zu_", p);
    file = idx + file;
    for (int k = 0; k < SkCount; ++k) {
      AudioBuffer b(2, wet120[k].size());
      std::copy(wet120[k].L.begin(), wet120[k].L.end(), b.plane(0)), std::copy(wet120[k].R.begin(), wet120[k].R.end(), b.plane(1));
      writeWav(dir + "/renders/" + file + "_" + kSrcName[k] + ".wav", b, kSr);
    }
    const double spb = 60.0 / 120 * kSr;
    const int wh = 90, bh = 64;
    Img im(1600, 3 * (wh + 4) + bands * (bh + 4) + 4);
    int y = 2;
    for (int k : {SkDrums, SkMix, SkPad}) waveRow(im, y, wh, src[1][k], wet120[k], spb, 0x9FB4D8), y += wh + 4;
    for (int b = bands - 1; b >= 0; --b) bandRow(im, y, bh, traces[b], spb, nz120.size(), kBandCol[b]), y += bh + 4;
    im.save(dir + "/pictures/" + file + ".png");
  }
  std::fclose(tsv);
  return 0;
}


// A project saved by 0.7.7 with the six first factory presets on mixer tracks 1 to 6: it loads,
// and each Terrace renders the plain source as it did then (RMS of each half second).
const double kOldLevels[6][32] = {
  {0.143353, 0.142511, 0.142189, 0.140171, 0.140007, 0.143210, 0.142312, 0.142271, 0.140458, 0.139760, 0.142638, 0.143323, 0.142379, 0.140516, 0.140298, 0.141080, 0.142512, 0.141459, 0.141259, 0.139228, 0.139181, 0.142258, 0.141486, 0.141286, 0.139564, 0.138940, 0.141643, 0.142286, 0.141442, 0.139692, 0.139327, 0.140308, },  // Clean slate
  {0.121235, 0.111408, 0.104195, 0.118313, 0.100861, 0.118157, 0.111682, 0.102045, 0.118843, 0.099223, 0.120390, 0.112946, 0.103954, 0.117582, 0.101442, 0.117086, 0.120844, 0.110757, 0.104052, 0.117741, 0.100713, 0.117475, 0.111622, 0.101386, 0.118626, 0.098791, 0.120072, 0.112395, 0.103757, 0.117099, 0.101238, 0.116623, },  // Four floors
  {0.087275, 0.085289, 0.084765, 0.083209, 0.082896, 0.086361, 0.085968, 0.084618, 0.083785, 0.082626, 0.085501, 0.086790, 0.084725, 0.084208, 0.082696, 0.084099, 0.085840, 0.083886, 0.083258, 0.081724, 0.081470, 0.084859, 0.084537, 0.083100, 0.082299, 0.081151, 0.084039, 0.085197, 0.083264, 0.082754, 0.081166, 0.082771, },  // Low pump
  {0.141637, 0.140798, 0.140343, 0.138717, 0.137426, 0.141966, 0.140075, 0.140864, 0.138668, 0.137488, 0.139882, 0.141959, 0.140653, 0.138173, 0.138157, 0.140241, 0.140608, 0.139959, 0.139614, 0.138019, 0.136992, 0.141052, 0.139350, 0.140254, 0.137475, 0.137130, 0.139443, 0.141240, 0.139739, 0.137778, 0.137444, 0.139578, },  // Triplet tops
  {0.129384, 0.132713, 0.134978, 0.129567, 0.127019, 0.135586, 0.130455, 0.131375, 0.129777, 0.129460, 0.132132, 0.134564, 0.127760, 0.130767, 0.133441, 0.130020, 0.132377, 0.133815, 0.127336, 0.131711, 0.128934, 0.128033, 0.134483, 0.132603, 0.122856, 0.132549, 0.134313, 0.127595, 0.131018, 0.132064, 0.125960, 0.132164, },  // Rolling mids
  {0.142689, 0.141723, 0.141447, 0.139284, 0.139303, 0.142300, 0.141605, 0.141406, 0.139764, 0.138873, 0.141949, 0.142407, 0.141722, 0.139637, 0.139581, 0.140290, 0.142011, 0.140979, 0.140715, 0.138697, 0.138653, 0.141720, 0.140957, 0.140769, 0.139045, 0.138410, 0.141127, 0.141746, 0.140944, 0.139164, 0.138798, 0.139825, },  // Ducked lows
};

void testOldProjects() {
  Song song;
  std::string err;
  expect(loadProject("tests/fixtures/terrace_v077.gridline.json", song, &err), ("the 0.7.7 project with Terrace loads " + err).c_str());
  const Stereo src = plainSource();
  for (int i = 0; i < 6; ++i) {
    const MixerTrack& t = song.mixer[static_cast<size_t>(i + 1)];
    const bool there = !t.slots.empty() && t.slots[0].plugin.type == tr::kType;
    expect(there, "the old project's Terrace is in its slot");
    if (!there) continue;
    PluginState st = t.slots[0].plugin;
    normalizePluginState(st);
    const std::vector<double> v = fingerprint(run(st, src, 120, 333));
    double worst = 0;
    for (size_t k = 0; k < v.size() && k < 32; ++k) worst = std::max(worst, std::fabs(v[k] - kOldLevels[i][k]) / std::max(1e-6, kOldLevels[i][k]));
    char msg[128];
    std::snprintf(msg, sizeof msg, "old project, track %d: renders as in 0.7.7 (worst level difference %.2g)", i + 1, worst);
    expect(v.size() == 32 && worst < 2e-5, msg);
  }
}

// ------------------------------------------------------------------ the test

void testPresets() {
  const PluginInfo* in = findPlugin(tr::kType);
  expect(in && in->presets.size() >= 24, "Terrace has at least 24 factory presets");
  if (!in) return;
  // the first six keep their names (old projects and sessions name them)
  const char* const keep[] = {"Clean slate", "Four floors", "Low pump", "Triplet tops", "Rolling mids", "Ducked lows"};
  for (const char* k : keep) {
    bool found = false;
    for (const auto& p : in->presets) found = found || p.name == k;
    expect(found, (std::string("the preset ") + k + " is still there").c_str());
  }
  // names are unique
  bool unique = true;
  for (size_t i = 0; i < in->presets.size(); ++i)
    for (size_t j = i + 1; j < in->presets.size(); ++j) unique = unique && in->presets[i].name != in->presets[j].name;
  expect(unique, "preset names are unique");
  const Stereo mix = makeSource(SkMix, 120);
  const double dryLoud = loudness(mix);
  for (size_t p = 0; p < in->presets.size(); ++p) {
    const std::string name = in->presets[p].name;
    const PluginState st = presetAt(p);
    const Stereo w = run(st, mix, 120, 333);
    const Health h = health(mix, w);
    const Clicks c = clicks(mix, w, maxStaticGain(st));
    const double dl = loudness(w) - dryLoud;
    const Bounds bd = boundsFor(name);
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: finite, no denormals, DC %.2g", name.c_str(), h.dc);
    expect(h.nonFinite == 0 && h.denormal == 0 && h.dc < 1e-3, msg);
    std::snprintf(msg, sizeof msg, "%s: no clicks (worst 1 ms window %+.1f dB over the source at %.0f ms, %d over 6 dB)", name.c_str(), c.worstDb, c.worstAtMs, c.over);
    expect(c.worstDb < 6 && c.over == 0, msg);
    std::snprintf(msg, sizeof msg, "%s: loudness %+.2f dB against bypass (allowed %+.1f to %+.1f)", name.c_str(), dl, bd.lo, bd.hi);
    expect(dl >= bd.lo && dl <= bd.hi, msg);
    std::snprintf(msg, sizeof msg, "%s: peaks stay under full scale (%.2f)", name.c_str(), h.peak);
    expect(h.peak < 1.0, msg);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir, only;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--harness") == 0) dir = argv[i + 1];
    if (std::strcmp(argv[i], "--only") == 0) only = argv[i + 1];
  }
  if (!dir.empty()) return harness(dir, only);
  testOldProjects();
  testPresets();
  return finish();
}
