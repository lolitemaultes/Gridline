// Gridline — J-72 tests: channel envelopes / LFOs / filter (engine/ChannelIns),
// one system with two timing behaviours (FL timing = classic, smooth).
//   * laws: per-tick gains and pitch of the classic timing against small
//     numeric captures taken from the reference voice running locally
//     (chunk-end gains and pitch offsets, see chanins_fixtures.h);
//   * smooth timing has no steps (gain and pitch move per sample / 8 samples);
//   * timing switch: text format, default for old text, voices follow it;
//   * project JSON and FLP round trips of a channel's envelopes;
//   * block-size invariance of an instrument running them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../../src/core/Demo.h"
#include "../../src/core/Document.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/Project.h"
#include "../../src/core/flp/Flp.h"
#include "../../src/core/flp/FlpExport.h"
#include "../../src/engine/ChannelIns.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Program.h"
#include "../Check.h"

using namespace gl;

namespace {

struct Cap {
  int pos, len;
  float gl, gr, pitch;
};
#include "chanins_fixtures.h"

InsTarget target(std::initializer_list<int> v) {  // the 17 stored fields in FL's record order
  int f[17];
  int i = 0;
  for (int x : v) f[i++] = x;
  InsTarget t;
  t.flags = f[0], t.on = f[1] != 0, t.predelay = f[2], t.attack = f[3], t.hold = f[4], t.decay = f[5], t.sustain = f[6];
  t.release = f[7], t.amount = f[8], t.lfoDelay = f[9], t.lfoAttack = f[10], t.lfoAmount = f[11], t.lfoSpeed = f[12];
  t.lfoShape = f[13], t.tensionA = f[14], t.tensionD = f[15], t.tensionR = f[16];
  return t;
}

// The settings of the three captured cases (the reference voice ran a sine at full level, 120 BPM, PPQ 96, 44.1 kHz).
InsSettings caseSettings(int which) {
  InsSettings s;
  if (which == 0) {
    s.t[InsVol] = target({4, -1, 3000, 12000, 6000, 25000, 40, 15000, 0, 100, 20000, 0, 32950, 0, 40, -60, -101});
  } else if (which == 1) {
    s.t[InsPan] = target({0x20, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 5000, 9000, 50, 15000, 1, 0, 0, 0});
    s.t[InsVol] = target({6, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, -70, 9000, 2, 0, 0, -101});
  } else {
    s.t[InsPitch] = target({0, -1, 100, 4000, 20000, 20000, 64, 20000, -1200, 100, 20000, 300, 12000, 0, 0, 0, 0});
  }
  return s;
}

// Classic timing on a constant input: the gain that starts chunk j + 1 is the
// (slope-limited) target chunk j ended on; the pitch is read after each tick.
void checkCase(int which, const Cap* caps, int n, int onTick, int offTick, float pan, const char* name) {
  const InsSettings s = caseSettings(which);
  InsVoice v;
  v.start(&s, 44100, 120, 96, 0, 0, onTick);
  v.setHostGains(pan, 1.0f);
  const int start = caps[0].pos, off = static_cast<int>(std::floor(offTick * 229.6875 + 0.5));
  int gainOk = 0, gainN = 0, pitchOk = 0, pitchN = 0;
  int c = 0;
  for (int pos = start; c < n; ++pos) {
    if (pos == off) v.release();
    v.sync();
    if (pos == caps[c].pos) {
      ++pitchN;
      pitchOk += v.pitchCents() == caps[c].pitch;
    }
    float l = 1.0f, r = 1.0f;
    v.processWith(l, r, 1.0f);
    if (c > 0 && pos == caps[c].pos) {
      gainN += 2;
      gainOk += (l == caps[c - 1].gl) + (r == caps[c - 1].gr);
    }
    if (pos + 1 == caps[c].pos + caps[c].len) ++c;
  }
  char b[160];
  std::snprintf(b, sizeof b, "laws (%s): chunk gains match the reference captures exactly (%d / %d)", name, gainOk, gainN);
  expect(gainOk == gainN && gainN > 40, b);
  std::snprintf(b, sizeof b, "laws (%s): pitch offsets match the reference captures exactly (%d / %d)", name, pitchOk, pitchN);
  expect(pitchOk == pitchN, b);
}

// Per-sample gain of a voice on a constant input.
std::vector<float> gains(const InsSettings& s, int frames, int releaseAt) {
  InsVoice v;
  v.start(&s, 44100, 120, 96, 0, 0, 0);
  std::vector<float> g;
  for (int i = 0; i < frames; ++i) {
    if (i == releaseAt) v.release();
    float l = 1.0f, r = 1.0f;
    v.process(l, r);
    g.push_back(l);
  }
  return g;
}
std::vector<double> pitches(const InsSettings& s, int frames) {
  InsVoice v;
  v.start(&s, 44100, 120, 96, 0, 0, 0);
  std::vector<double> p;
  for (int i = 0; i < frames; ++i) {
    v.sync();
    p.push_back(v.pitchSemis() * 100.0);
    float l = 1.0f, r = 1.0f;
    v.process(l, r);
  }
  return p;
}
double maxStep(const std::vector<float>& v) {
  double m = 0;
  for (size_t i = 1; i < v.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
  return m;
}
double maxStepD(const std::vector<double>& v) {
  double m = 0;
  for (size_t i = 1; i < v.size(); ++i) m = std::max(m, std::fabs(v[i] - v[i - 1]));
  return m;
}

// A Trio Synth (reference oscillators) with channel envelopes, one note, rendered in blocks of `block`.
std::vector<float> renderSynth(const PluginState& st, int block, int total) {
  std::vector<float> L(static_cast<size_t>(total), 0.f), R(static_cast<size_t>(total), 0.f);
  auto g = createGenerator(st.type);
  ChannelAux aux;
  prepareChannelAux(st, aux);
  auto ins = std::make_shared<InsSettings>();
  if (parseIns(st.ins, *ins) && ins->active()) aux.ins = ins;
  ProcessContext ctx;
  ctx.sampleRate = 44100, ctx.tempo = 120, ctx.ppq = 96;
  const int on = 0, off = 13092;
  for (int pos = 0; pos < total; pos += block) {
    const int k = std::min(block, total - pos);
    if (on >= pos && on < pos + k) g->noteOn(1, 64.0, 100.0f / 128.0f, on - pos);
    if (off >= pos && off < pos + k) g->noteOff(1, off - pos);
    ctx.tick = pos * ctx.tempo * ctx.ppq / (60.0 * ctx.sampleRate);
    g->process(st, aux, ctx, L.data() + pos, R.data() + pos, k);
  }
  std::vector<float> out;
  for (int i = 0; i < total; ++i) out.push_back(L[static_cast<size_t>(i)]), out.push_back(R[static_cast<size_t>(i)]);
  return out;
}

PluginState trioWithIns(const InsSettings& s) {
  PluginState st = defaultPluginState("gl.synth");
  if (st.params.size() > pp::SynModel) st.params[pp::SynModel] = 1.0f;  // reference oscillators (what FL imports play)
  st.ins = formatIns(s);
  return st;
}

InsSettings richSettings(InsTiming timing) {
  InsSettings s;
  s.timing = timing;
  s.t[InsVol] = target({4, -1, 100, 9000, 3000, 26000, 60, 14000, 0, 100, 20000, 30, 11000, 0, 20, -40, -101});
  s.t[InsModX] = target({0, -1, 100, 100, 100, 22000, 20, 16000, 90, 100, 20000, 20, 16000, 1, 0, -60, 0});
  s.t[InsModY] = target({0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 2000, 6000, 40, 13000, 2, 0, 0, 0});
  s.t[InsPitch] = target({0, -1, 100, 6000, 100, 18000, 64, 20000, 400, 100, 20000, 25, 10000, 0, 0, 0, 0});
  s.t[InsPan] = target({0x20, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 100, 30, 14000, 1, 0, 0, 0});
  s.cutoff = 150, s.reso = 90;
  const int vel[4] = {90, 10, 34, -20}, key[4] = {60, -15, 42, 8};  // velocity / key tracking
  for (int j = 0; j < 4; ++j) s.track[0][j] = vel[j], s.track[1][j] = key[j];
  return s;
}

}  // namespace

int main() {
  // ---- Laws (classic timing) vs the reference captures.
  checkCase(0, kVol, static_cast<int>(sizeof kVol / sizeof kVol[0]), 0, 40, 0.0f, "volume envelope, delay / tensions / release");
  checkCase(1, kLfo, static_cast<int>(sizeof kLfo / sizeof kLfo[0]), 0, 40, 0.25f, "global pan LFO with delay + attack, synced pulse tremolo");
  checkCase(2, kPitch, static_cast<int>(sizeof kPitch / sizeof kPitch[0]), 5, 40, 0.0f, "pitch envelope + vibrato, note on tick 5");
  expect(std::fabs(insKnobTime(32950, false) - 2.0) < 0.002 && insKnobTime(100, false) == 0 && std::fabs(insKnobTime(65536, false) - 64.0) < 0.01 &&
             std::fabs(insKnobTime(32950, true) - 4.0) < 0.004,
         "laws: time knob 0.064·(1001^(v/65536) − 1) s (2 s / 4 beats at 32950, 64 s at the top)");
  expect(insCurve(0, 0.3) == 0.3 && insCurve(60, 0.5) > 0.5 && insCurve(-60, 0.5) < 0.5 && std::fabs(insCurve(-101, 1.0) - 1.0) < 1e-12,
         "laws: tension curves (concave above 0, convex below, full level at the end)");

  // ---- Timing switch: text, defaults, voices.
  {
    InsSettings a;
    expect(a.classic(), "timing: settings default to FL timing (classic)");
    InsSettings p;
    parseIns(formatIns(a), p);
    expect(formatIns(a).find("\nM ") == std::string::npos && p.classic(), "timing: classic text has no timing line (pre-J-72 text reads as FL timing)");
    a.timing = InsTiming::Smooth;
    expect(formatIns(a).find("M 0") != std::string::npos && parseIns(formatIns(a), p) && !p.classic() && p == a, "timing: smooth is written and read back");
    a.timing = InsTiming::Classic;
    expect(parseIns(formatIns(a), p) && p.classic() && p == a, "timing: switching back to FL timing round-trips");
    InsSettings t = richSettings(InsTiming::Classic);
    expect(parseIns(formatIns(t), p) && p == t && t.tracking(), "text: envelopes, filter and velocity / key tracking round-trip");
  }
  // ---- Velocity / key tracking moves the filter base like the reference host.
  {
    InsSettings s;
    s.cutoff = 128;
    s.track[0][2] = 64;  // velocity → mod X, middle 100
    InsVoice lo, mid, hi;
    for (InsVoice* v : {&lo, &mid, &hi}) v->start(&s, 44100, 120, 96, 0, 0, 0);
    lo.setNote(60, 40.0f / 128.0f), mid.setNote(60, 100.0f / 128.0f), hi.setNote(60, 127.0f / 128.0f);
    auto rms = [](InsVoice& v) {  // a bright input through the fast low-pass: more cutoff, more level
      double e = 0;
      for (int i = 0; i < 4000; ++i) {
        float l = (i & 1) ? 1.0f : -1.0f, r = l;
        v.process(l, r);
        if (i > 1000) e += static_cast<double>(l) * l;
      }
      return e;
    };
    const double a = rms(lo), b = rms(mid), c = rms(hi);
    expect(a < b && b < c, "tracking: higher velocity opens the channel filter (velocity → mod X)");
  }

  // ---- Smooth timing: no steps; classic: tick steps.
  {
    InsSettings s;  // instant attack, pulse tremolo, instant-ish release
    s.t[InsVol] = target({4, -1, 100, 100, 100, 30000, 128, 1500, 0, 100, 100, 100, 12000, 2, 0, 0, 0});
    s.timing = InsTiming::Smooth;
    const double smooth = maxStep(gains(s, 44100, 30000));
    s.timing = InsTiming::Classic;
    const double classic = maxStep(gains(s, 44100, 30000));
    char b[160];
    std::snprintf(b, sizeof b, "smooth: gain moves without steps (largest step %.4f per sample, classic %.4f)", smooth, classic);
    expect(smooth < 0.025 && classic <= 0.0011, b);
    InsSettings q;
    q.t[InsPitch] = target({0, -1, 100, 12000, 100, 20000, 0, 20000, 1200, 100, 100, 100, 9000, 0, 0, 0, 0});
    q.timing = InsTiming::Smooth;
    const double ps = maxStepD(pitches(q, 22050));
    q.timing = InsTiming::Classic;
    const double pc = maxStepD(pitches(q, 22050));
    std::snprintf(b, sizeof b, "smooth: pitch envelope moves in small steps (%.2f ct, classic ticks %.2f ct)", ps, pc);
    expect(ps < 3.0 && pc > 4.0 * ps, b);
    // Smooth timing follows the same knob laws: a 2 s attack reaches full level after 2 s.
    InsSettings t2;
    t2.timing = InsTiming::Smooth;
    t2.t[InsVol] = target({4, -1, 100, 32950, 100, 30000, 128, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0});
    const std::vector<float> g = gains(t2, 44100 * 3, -1);
    const float full = g.back();
    expect(g[44100] < 0.56f * full && g[44100] > 0.44f * full && g[88300] > 0.99f * full && g[88300] <= full,
           "smooth: the same time law (a 2 s linear attack is half way at 1 s, done at 2 s)");
  }

  // ---- Project JSON round trip.
  {
    Document d(makeDemoSong());
    const ObjectId a = d.addChannel("Trio classic", "gl.synth");
    const ObjectId b = d.addChannel("Trio smooth", "gl.synth");
    d.editChannel(a, "ins", [](Channel& c) { c.plugin.ins = formatIns(richSettings(InsTiming::Classic)); });
    d.editChannel(b, "ins", [](Channel& c) { c.plugin.ins = formatIns(richSettings(InsTiming::Smooth)); });
    Song back;
    std::string err;
    const bool ok = songFromJson(songToJson(d.song()), back, &err);
    expect(ok && back.channel(a) && back.channel(b) && back.channel(a)->plugin.ins == d.song().channel(a)->plugin.ins &&
               back.channel(b)->plugin.ins == d.song().channel(b)->plugin.ins,
           "project: channel envelopes (both timings) survive save / load");

    // ---- FLP round trip.
    flp::Bytes bytes;
    flp::ExportReport rep;
    flp::ExportOptions eo;
    eo.useOrigin = false;
    expect(flp::exportFlp(d.song(), bytes, rep, eo), "flp: exported");
    Song imp;
    flp::Report ir;
    const bool ri = importFlpBytes(bytes, imp, ir, &err);
    const Channel* ca = nullptr;
    const Channel* cb = nullptr;
    for (const auto& c : imp.channels) {
      if (c.name == "Trio classic") ca = &c;
      if (c.name == "Trio smooth") cb = &c;
    }
    InsSettings ia, ib;
    const bool pa = ca && parseIns(ca->plugin.ins, ia), pb = cb && parseIns(cb->plugin.ins, ib);
    expect(ri && pa && ia == richSettings(InsTiming::Classic), "flp: FL-timing envelopes export field for field and import unchanged");
    InsSettings want = richSettings(InsTiming::Smooth);
    want.timing = InsTiming::Classic;  // FL has no smooth timing: the same fields import with FL timing
    expect(pb && ib == want, "flp: smooth envelopes export the same FL fields (imported with FL timing)");
  }

  // ---- Block-size invariance (classic and smooth, reference oscillators).
  for (InsTiming tm : {InsTiming::Classic, InsTiming::Smooth}) {
    const PluginState st = trioWithIns(richSettings(tm));
    const int total = 30000;
    const std::vector<float> ref = renderSynth(st, 256, total);
    bool same = true, finite = true;
    double peak = 0;
    for (float x : ref) finite = finite && std::isfinite(x), peak = std::max(peak, static_cast<double>(std::fabs(x)));
    for (int blk : {1, 7, 64, 100, 1000, 4096}) {
      const std::vector<float> o = renderSynth(st, blk, total);
      same = same && o == ref;
    }
    expect(finite && peak > 0.01, tm == InsTiming::Classic ? "classic: sound out, finite" : "smooth: sound out, finite");
    expect(same, tm == InsTiming::Classic ? "classic: identical output for block sizes 1..4096" : "smooth: identical output for block sizes 1..4096");
  }
  return finish();
}
