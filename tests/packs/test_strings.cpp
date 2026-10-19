// Gridline — J-22 "strings" tests: Twin String (gl.twinstring), Plucker
// (gl.plucker), Step Bass (gl.stepbass) and the decode of FL Studio's shipped
// Sakura / Plucked! / Transistor Bass states.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/core/PluginInfo.h"
#include "../../src/core/flp/FlpJ22Strings.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/models/strings.h"
#include "../Check.h"
#include "fl_corpus.h"

using namespace gl;
using namespace gl::packs::models::strs;

namespace {

struct Ev {
  int at;
  bool on;
  uint32_t id;
  double pitch;
  float vel;
};
struct Out {
  std::vector<float> L, R;
  bool finite = true;
  float peak = 0;
};
Out render(const PluginState& st, const std::vector<Ev>& evs, int total, int block = 256, double sr = 44100.0,
           const std::vector<std::pair<int, std::array<uint8_t, 3>>>& midi = {}) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.f);
  o.R.assign(static_cast<size_t>(total), 0.f);
  auto g = packs::createGenerator(st.type);
  if (!g) {
    o.finite = false;
    return o;
  }
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = sr;
  ctx.tempo = 120;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    for (const auto& m : midi)
      if (m.first >= pos && m.first < pos + n) g->midi(m.second[0], m.second[1], m.second[2], m.first - pos);
    for (const auto& e : evs)
      if (e.at >= pos && e.at < pos + n) {
        if (e.on)
          g->noteOn(e.id, e.pitch, e.vel, e.at - pos);
        else
          g->noteOff(e.id, e.at - pos);
      }
    ctx.tick = pos;
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
  }
  for (size_t i = 0; i < o.L.size(); ++i) {
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) o.finite = false;
    o.peak = std::max({o.peak, std::fabs(o.L[i]), std::fabs(o.R[i])});
  }
  return o;
}
double rms(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
  return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0;
}
// High-frequency share: energy of the first difference over the energy.
double brightness(const std::vector<float>& v, size_t a, size_t b) {
  double e = 0, d = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; ++i) {
    e += static_cast<double>(v[i]) * v[i];
    double x = v[i] - v[i - 1];
    d += x * x;
  }
  return e > 0 ? d / e : 0;
}
// Goertzel magnitude at hz over [a, b).
double tone(const std::vector<float>& v, size_t a, size_t b, double hz, double sr = 44100) {
  double w = 2 * 3.14159265358979 * hz / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) {
    double s0 = v[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(b - a);
}
// Strongest partial between 40 Hz and 2.5 kHz (1/32-semitone scan), Hz.
double pitchHz(const std::vector<float>& v, size_t a, size_t b, double sr = 44100) {
  double best = 0, bestM = -1;
  for (double n = 28; n < 100; n += 1.0 / 32) {
    double hz = 440.0 * std::pow(2.0, (n - 69) / 12.0), m = tone(v, a, b, hz, sr);
    if (m > bestM) bestM = m, best = hz;
  }
  return best;
}
double noteToHzTest(double n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }
void debugTones(const char* what, const std::vector<float>& v) {
  if (!std::getenv("GL_STR_DEBUG")) return;
  std::printf("# %s:", what);
  for (double hz : {55.0, 110.0, 146.8, 220.0, 293.7, 440.0, 660.0, 880.0, 1320.0}) std::printf(" %.0f=%.2e", hz, tone(v, 4000, 12000, hz));
  std::printf("\n");
}

PluginState fresh(const char* type) {
  PluginState s;
  s.type = type;
  normalizePluginState(s);
  return s;
}
const std::vector<Ev> kNote = {{0, true, 1, 57, 0.9f}, {22050, false, 1, 57, 0}};

// Raw state (event 213) of a preset file.
std::vector<uint8_t> rawState(const std::string& path) {
  flp::File f;
  if (!flp::readFile(path, f, nullptr)) return {};
  for (const auto& e : f.events)
    if (e.id == 213) return std::vector<uint8_t>(e.data, e.data + e.size);
  return {};
}

// ---------------------------------------------------------------- tests
void testCatalog() {
  const PluginInfo* ts = findPlugin("gl.twinstring");
  const PluginInfo* pk = findPlugin("gl.plucker");
  const PluginInfo* sb = findPlugin("gl.stepbass");
  expect(ts && ts->params.size() == static_cast<size_t>(TsCount), "Twin String registered with 114 parameters (tag layout)");
  expect(pk && pk->params.size() == static_cast<size_t>(PkCount), "Plucker registered with 5 parameters");
  expect(sb && sb->params.size() == static_cast<size_t>(SbCount), "Step Bass registered");
  expect(ts && ts->presets.size() >= 2 && ts->presets.size() <= 6, "Twin String has 2-6 factory presets");
  expect(pk && pk->presets.size() >= 2 && pk->presets.size() <= 6, "Plucker has 2-6 factory presets");
  expect(sb && sb->presets.size() >= 2 && sb->presets.size() <= 6, "Step Bass has 2-6 factory presets");
  // Every factory preset renders finite sound.
  for (const char* type : {"gl.twinstring", "gl.plucker"}) {
    const PluginInfo* info = findPlugin(type);
    int ok = 0;
    for (const auto& pr : info->presets) {
      PluginState s = fresh(type);
      for (auto [k, v] : pr.values) s.params[static_cast<size_t>(k)] = v;
      Out o = render(s, kNote, 44100);
      ok += o.finite && o.peak > 1e-3f && o.peak < 4.0f;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s: %d/%zu factory presets sound (finite, < +12 dB)", type, ok, info->presets.size());
    expect(ok == static_cast<int>(info->presets.size()), buf);
  }
}

void testTwinStringBehaviour() {
  PluginState base = fresh("gl.twinstring");
  Out o = render(base, kNote, 44100);
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Twin String: default patch sounds (peak %.3f)", static_cast<double>(o.peak));
    expect(o.finite && o.peak > 0.05f && o.peak < 2.0f, buf);
  }
  // Pitch follows the note (A3 = 220 Hz).
  {
    PluginState s = base;
    s.params[TsHiCut] = 0.8f;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 22050);
    double hz = pitchHz(a.L, 4000, 12000);
    char buf[80];
    std::snprintf(buf, sizeof buf, "Twin String: A3 plays at %.1f Hz (220)", hz);
    expect(std::fabs(hz - 220.0) < 4.0, buf);
    // Transpose knob: +1 octave at 0.5 + 12/72.
    s.params[TsTranspose] = 4;
    Out b = render(s, {{0, true, 1, 57, 0.9f}}, 22050);
    double hz2 = pitchHz(b.L, 4000, 12000);
    debugTones("transpose +12", b.L);
    std::snprintf(buf, sizeof buf, "Twin String: transpose +12 st plays %.1f Hz (440)", hz2);
    expect(std::fabs(hz2 - 440.0) < 8.0, buf);
  }
  // String decay: longer ring time keeps more energy late.
  {
    PluginState s = base, t = base;
    s.params[TsS1Decay] = 0.3f;
    t.params[TsS1Decay] = 0.9f;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 44100), b = render(t, {{0, true, 1, 57, 0.9f}}, 44100);
    expect(rms(b.L, 30000, 44100) > 3 * rms(a.L, 30000, 44100), "Twin String: string decay lengthens the ring");
  }
  // Damping darkens the string.
  {
    PluginState s = base, t = base;
    s.params[TsHiCut] = t.params[TsHiCut] = 0.9f;
    t.params[TsS1Damp] = 0.8f;
    Out a = render(s, kNote, 22050), b = render(t, kNote, 22050);
    expect(brightness(b.L, 4000, 20000) < 0.7 * brightness(a.L, 4000, 20000), "Twin String: damping darkens the tone");
  }
  // Exciter high cut: higher cutoff → brighter attack.
  {
    PluginState s = base, t = base;
    s.params[TsHiCut] = 0.3f;
    t.params[TsHiCut] = 0.95f;
    Out a = render(s, kNote, 8000), b = render(t, kNote, 8000);
    expect(brightness(b.L, 0, 4000) > 1.5 * brightness(a.L, 0, 4000), "Twin String: high cut opens the exciter");
  }
  // Sustained excitation (high-cut sustain) keeps the note going (bowed).
  {
    PluginState s = base, t = base;
    s.params[TsS1Decay] = t.params[TsS1Decay] = 0.4f;
    t.params[TsHiSus] = 0.7f;
    t.params[TsHiAtk] = 0.3f;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 44100), b = render(t, {{0, true, 1, 57, 0.9f}}, 44100);
    expect(rms(b.L, 33000, 44100) > 5 * rms(a.L, 33000, 44100) && b.finite, "Twin String: exciter sustain bows the string");
  }
  // String B: mix toward B with ratio 2/1 raises the pitch an octave.
  {
    PluginState s = base;
    s.params[TsHiCut] = 0.8f;
    s.params[TsMix] = 1.0f;
    s.params[TsS2Ratio] = 4;
    Out a = render(s, {{0, true, 1, 45, 0.9f}}, 22050);
    double hz = pitchHz(a.L, 4000, 12000);
    char buf[80];
    std::snprintf(buf, sizeof buf, "Twin String: string B ratio 2/1 at A2 plays %.1f Hz (220)", hz);
    expect(std::fabs(hz - 220.0) < 5.0, buf);
  }
  // Inverted loop keeps the pitch and removes even harmonics (hollow tone).
  {
    PluginState s = base;
    s.params[TsHiCut] = 0.8f;
    s.params[TsS1Inv] = 1;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 22050);
    double hz = pitchHz(a.L, 4000, 12000);
    char buf[80];
    std::snprintf(buf, sizeof buf, "Twin String: inverted loop keeps A3 (%.1f Hz)", hz);
    expect(std::fabs(hz - 220.0) < 5.0, buf);
  }
  // Stereo spread: A left / B right differ when spread is full.
  {
    PluginState s = base;
    s.params[TsMix] = 0.5f;
    s.params[TsS2Ratio] = 4;
    s.params[TsStereo] = 1;
    Out a = render(s, kNote, 22050);
    double diff = 0;
    for (size_t i = 0; i < a.L.size(); ++i) diff += std::fabs(a.L[i] - a.R[i]);
    s.params[TsStereo] = 0;
    Out b = render(s, kNote, 22050);
    double diff0 = 0;
    for (size_t i = 0; i < b.L.size(); ++i) diff0 += std::fabs(b.L[i] - b.R[i]);
    expect(diff > 100 * (diff0 + 1e-9) && diff > 1.0, "Twin String: stereo spread separates the strings");
  }
  // Velocity → level (default slot 1).
  {
    Out a = render(base, {{0, true, 1, 57, 1.0f}}, 22050), b = render(base, {{0, true, 1, 57, 0.25f}}, 22050);
    expect(rms(b.L, 0, 22050) < 0.4 * rms(a.L, 0, 22050), "Twin String: velocity scales the level (mod slot 1)");
  }
  // Pitch bend through the matrix (default slot 2: pitch wheel → pitch).
  {
    PluginState s = base;
    s.params[TsHiCut] = 0.8f;
    std::vector<std::pair<int, std::array<uint8_t, 3>>> bend = {{0, {0xE0, 0x7F, 0x7F}}};
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 22050, 256, 44100, bend);
    double hz = pitchHz(a.L, 4000, 12000);
    expect(hz > 300, "Twin String: pitch bend raises the pitch via the matrix");
  }
  // Comb filter and resonators change the sound.
  {
    PluginState s = base, t = base, u = base;
    t.params[TsCombLevel] = 0.8f;
    t.params[TsCombFb] = 0.6f;
    u.params[TsResOn1] = 1;
    u.params[TsResFreq1] = 0.4f;
    u.params[TsResFb1] = 0.8f;
    Out a = render(s, kNote, 22050), b = render(t, kNote, 22050), c = render(u, kNote, 22050);
    double d1 = 0, d2 = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d1 += std::fabs(a.L[i] - b.L[i]), d2 += std::fabs(a.L[i] - c.L[i]);
    expect(d1 > 1.0 && b.finite, "Twin String: comb filter colours the strings");
    expect(d2 > 1.0 && c.finite, "Twin String: resonator band colours the body");
  }
  // Effects add a tail after the note stops ringing.
  {
    PluginState s = base, t = base;
    s.params[TsS1Decay] = t.params[TsS1Decay] = 0.2f;
    s.params[TsAmpR] = t.params[TsAmpR] = 0.0f;
    t.params[TsDlyOn] = 1;
    t.params[TsDlyWet] = 0.8f;
    t.params[TsDlyFb] = 0.7f;
    t.params[TsRevOn] = 1;
    std::vector<Ev> e = {{0, true, 1, 57, 0.9f}, {4410, false, 1, 57, 0}};
    Out a = render(s, e, 66150), b = render(t, e, 66150);
    expect(rms(b.L, 44100, 66150) > 10 * rms(a.L, 44100, 66150) + 1e-6, "Twin String: delay + reverb add a tail");
  }
  // LFO → pitch vibrato moves the pitch over time.
  {
    PluginState s = base;
    s.params[TsHiCut] = 0.8f;
    s.params[TsHiSus] = 0.6f;
    s.params[tsMod(2, TmSource)] = MsLfo;
    s.params[tsMod(2, TmDest)] = MdPitch;
    s.params[tsMod(2, TmAmount)] = 0.1f;
    s.params[TsLfoRate] = 0.3f;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 88200);
    double h1 = pitchHz(a.L, 10000, 14000), h2 = pitchHz(a.L, 30000, 34000), h3 = pitchHz(a.L, 50000, 54000);
    expect(std::max({h1, h2, h3}) - std::min({h1, h2, h3}) > 5.0 && a.finite, "Twin String: LFO vibrato through the matrix");
  }
  // Release: amp release lets the note fade after note-off.
  {
    PluginState s = base;
    s.params[TsS1Decay] = 1;
    s.params[TsHiSus] = 0.5f;
    s.params[TsAmpR] = 0.05f;
    Out a = render(s, {{0, true, 1, 57, 0.9f}, {11025, false, 1, 57, 0}}, 44100);
    expect(rms(a.L, 33000, 44100) < 0.01 * rms(a.L, 5000, 11025), "Twin String: amp release ends the note");
  }
}

void testTwinStringRobust() {
  const PluginInfo* info = findPlugin("gl.twinstring");
  // Extremes: all minimum / all maximum.
  for (int ext = 0; ext < 2; ++ext) {
    PluginState s = fresh("gl.twinstring");
    for (size_t i = 0; i < s.params.size(); ++i) s.params[i] = static_cast<float>(ext ? info->params[i].max : info->params[i].min);
    std::vector<Ev> e;
    for (int k = 0; k < 20; ++k) e.push_back({k * 500, true, static_cast<uint32_t>(k + 1), 20.0 + 5 * k, 1.0f});
    Out o = render(s, e, 44100);
    expect(o.finite && o.peak < 100.f, ext ? "Twin String: all-maximum settings stay finite" : "Twin String: all-minimum settings stay finite");
  }
  // Extreme notes and sample rates.
  for (double sr : {22050.0, 96000.0, 192000.0}) {
    PluginState s = fresh("gl.twinstring");
    Out o = render(s, {{0, true, 1, 0, 1}, {0, true, 2, 127, 1}, {10, true, 3, 60, 1}}, 20000, 256, sr);
    char buf[80];
    std::snprintf(buf, sizeof buf, "Twin String: notes 0 / 127 at %.0f Hz finite", sr);
    expect(o.finite, buf);
  }
  // Block-size invariance.
  {
    PluginState s = fresh("gl.twinstring");
    s.params[tsMod(2, TmSource)] = MsLfo;
    s.params[tsMod(2, TmDest)] = MdHiCut;
    s.params[tsMod(2, TmAmount)] = 0.4f;
    s.params[TsChoOn] = 1;
    Out a = render(s, kNote, 30000, 64), b = render(s, kNote, 30000, 1000);
    double d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(d < 1e-4, "Twin String: block-size invariant");
  }
  // Short parameter vectors normalise to the defaults.
  {
    PluginState s;
    s.type = "gl.twinstring";
    s.params = {0.5f, 0.5f};
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(TsCount) && s.params[TsS1Decay] == static_cast<float>(info->params[TsS1Decay].def),
           "Twin String: short state normalises to defaults");
  }
  // Polyphony overflow (voice stealing) stays finite.
  {
    PluginState s = fresh("gl.twinstring");
    std::vector<Ev> e;
    for (int k = 0; k < 40; ++k) e.push_back({k * 100, true, static_cast<uint32_t>(k + 1), 40.0 + k, 0.8f});
    Out o = render(s, e, 20000);
    expect(o.finite && o.peak > 0.01f, "Twin String: 40 overlapping notes (stealing) finite");
  }
}

void testTwinStringParse() {
  // Malformed states.
  std::vector<uint8_t> good(596, 0);
  std::memcpy(good.data(), "SSLF", 4);
  uint32_t sz = 588;
  std::memcpy(good.data() + 4, &sz, 4);
  std::memcpy(good.data() + 8, "1rkS", 4);
  flp::j22::TwinStringState st;
  expect(flp::j22::parseTwinString(good.data(), good.size(), st) && st.consumed == 596, "Sakura parser: synthetic state walks 596 bytes");
  for (size_t cut : {0, 3, 11, 12, 100, 467}) {
    std::vector<uint8_t> t(good.begin(), good.begin() + static_cast<long>(cut));
    expect(!flp::j22::parseTwinString(t.data(), t.size(), st), ("Sakura parser rejects a state truncated to " + std::to_string(cut)).c_str());
  }
  std::vector<uint8_t> bad = good;
  bad[8] = 'X';
  expect(!flp::j22::parseTwinString(bad.data(), bad.size(), st), "Sakura parser rejects a wrong id");
  bad = good;
  uint32_t wrong = 1000;
  std::memcpy(bad.data() + 4, &wrong, 4);
  expect(!flp::j22::parseTwinString(bad.data(), bad.size(), st), "Sakura parser rejects a wrong size field");
  bad = good;
  uint32_t nan = 0x7FC00000u;
  std::memcpy(bad.data() + 12 + 4 * 6, &nan, 4);
  expect(!flp::j22::parseTwinString(bad.data(), bad.size(), st), "Sakura parser rejects NaN values");
  flp::j22::PluckerState ps;
  int32_t pv[5] = {128, 64, 1, 0, 1};
  expect(flp::j22::parsePlucker(reinterpret_cast<uint8_t*>(pv), 20, ps) && ps.consumed == 20, "Plucked! parser: 20-byte state");
  expect(!flp::j22::parsePlucker(reinterpret_cast<uint8_t*>(pv), 16, ps), "Plucked! parser rejects a short state");
  pv[1] = 999;
  expect(!flp::j22::parsePlucker(reinterpret_cast<uint8_t*>(pv), 20, ps), "Plucked! parser rejects out-of-range values");
}

void testSakuraCorpus() {
  if (!testfl::flInstalled()) {
    expect(true, "FL Studio not installed: Sakura corpus skipped");
    return;
  }
  // Walk every shipped preset state to its end.
  auto files = testfl::presetFiles("Sakura");
  size_t walked = 0;
  for (const auto& f : files) {
    auto raw = rawState(f);
    flp::j22::TwinStringState st;
    if (flp::j22::parseTwinString(raw.data(), raw.size(), st) && st.consumed == raw.size()) ++walked;
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "Sakura: %zu/%zu shipped preset states walk to their exact end", walked, files.size());
  expect(walked == files.size() && files.size() >= 500, buf);
  auto pres = testfl::decodePresets("Sakura");
  size_t ok = 0, exact = 0, finite = 0, audible = 0;
  for (const auto& d : pres) {
    testfl::maybeDump(d);
    if (!d.ok || d.state.type != "gl.twinstring") continue;
    ++ok;
    exact += d.exact;
    Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {13230, false, 1, 60, 0}}, 22050);
    finite += o.finite && o.peak < 8.f;
    audible += o.finite && o.peak > 1e-3f;
    if (std::getenv("GL_STR_PEAKS")) std::printf("#  %-40s peak %.4f\n", d.where.c_str(), static_cast<double>(o.peak));
  }
  std::snprintf(buf, sizeof buf, "Sakura presets: %zu/%zu decode → gl.twinstring (%zu exact), %zu finite, %zu audible in 0.5 s", ok,
                pres.size(), exact, finite, audible);
  expect(ok == pres.size() && exact == ok && finite == ok && audible >= ok - ok / 50, buf);
  auto proj = testfl::decodeProjects("Sakura");
  size_t pok = 0, pexact = 0, paud = 0;
  for (const auto& d : proj) {
    if (!d.ok || d.state.type != "gl.twinstring") continue;
    ++pok;
    pexact += d.exact;
    Out o = render(d.state, {{0, true, 1, 60, 0.8f}, {13230, false, 1, 60, 0}}, 22050);
    paud += o.finite && o.peak > 1e-3f;
  }
  std::snprintf(buf, sizeof buf, "Sakura projects: %zu/%zu channel states decode (%zu exact), %zu audible", pok, proj.size(), pexact, paud);
  expect(pok == proj.size() && proj.size() >= 4 && pexact == pok && paud == pok, buf);
}

void testPlucker() {
  PluginState base = fresh("gl.plucker");
  Out o = render(base, kNote, 44100);
  expect(o.finite && o.peak > 0.01f, "Plucker: default sounds");
  char buf[96];
  {
    // Harmonic series on A3: partials at 220 / 440 Hz, nothing between.
    double h1 = tone(o.L, 2000, 10000, 220), h2 = tone(o.L, 2000, 10000, 440), x1 = tone(o.L, 2000, 10000, 233), x2 = tone(o.L, 2000, 10000, 466);
    std::snprintf(buf, sizeof buf, "Plucker: A3 rings on its harmonic series (%.1e / %.1e)", h1, x1);
    expect(h1 > 5 * x1 && h2 > 5 * x2, buf);
  }
  {
    PluginState s = base, t = base;
    s.params[PkDecay] = 60;
    t.params[PkDecay] = 220;
    Out a = render(s, {{0, true, 1, 57, 0.9f}}, 44100), b = render(t, {{0, true, 1, 57, 0.9f}}, 44100);
    expect(rms(b.L, 30000, 44100) > 4 * rms(a.L, 30000, 44100), "Plucker: decay lengthens the ring");
  }
  {
    PluginState s = base, t = base;
    s.params[PkColor] = 10;
    t.params[PkColor] = 120;
    Out a = render(s, kNote, 22050), b = render(t, kNote, 22050);
    expect(brightness(b.L, 1000, 10000) > 1.5 * brightness(a.L, 1000, 10000), "Plucker: colour brightens");
  }
  {
    PluginState s = base, t = base;
    s.params[PkDecay] = t.params[PkDecay] = 230;
    t.params[PkGate] = 1;
    std::vector<Ev> e = {{0, true, 1, 57, 0.9f}, {4410, false, 1, 57, 0}};
    Out a = render(s, e, 22050), b = render(t, e, 22050);
    expect(rms(b.L, 11025, 22050) < 0.01 * rms(a.L, 11025, 22050), "Plucker: gate cuts the note at release");
  }
  {
    PluginState s = base, t = base;
    s.params[PkWiden] = 0;
    t.params[PkWiden] = 1;
    Out a = render(s, kNote, 22050), b = render(t, kNote, 22050);
    double d0 = 0, d1 = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d0 += std::fabs(a.L[i] - a.R[i]), d1 += std::fabs(b.L[i] - b.R[i]);
    expect(d0 < 1e-6 && d1 > 1.0, "Plucker: widen makes it stereo");
  }
  {
    // Normalized decay: a high note rings about as long as a low one; off → shorter.
    PluginState s = base, t = base;
    s.params[PkDecay] = t.params[PkDecay] = 200;
    t.params[PkNormalize] = 0;
    auto ratio = [](const PluginState& st) {
      Out lo = render(st, {{0, true, 1, 57, 0.9f}}, 44100), hi = render(st, {{0, true, 1, 93, 0.9f}}, 44100);
      if (std::getenv("GL_STR_DEBUG"))
        std::printf("# norm %g: hi %.2e %.2e lo %.2e %.2e\n", static_cast<double>(st.params[PkNormalize]), rms(hi.L, 30000, 44100), rms(hi.L, 0, 4000),
                    rms(lo.L, 30000, 44100), rms(lo.L, 0, 4000));
      return (rms(hi.L, 30000, 44100) / rms(hi.L, 0, 4000)) / (rms(lo.L, 30000, 44100) / rms(lo.L, 0, 4000));
    };
    double rs = ratio(s), rt = ratio(t);
    char b2[96];
    std::snprintf(b2, sizeof b2, "Plucker: even decay keeps high notes ringing (%.3f vs %.3f)", rs, rt);
    expect(rs > 2.5 * rt, b2);
  }
  for (int ext = 0; ext < 2; ++ext) {
    PluginState s = base;
    s.params = ext ? std::vector<float>{256, 128, 1, 1, 1} : std::vector<float>{0, 0, 0, 0, 0};
    Out a = render(s, {{0, true, 1, 0, 1}, {0, true, 2, 127, 1}, {0, true, 3, 60, 1}}, 22050);
    expect(a.finite, ext ? "Plucker: maximum settings finite" : "Plucker: minimum settings finite");
  }
  {
    Out a = render(base, kNote, 20000, 32), b = render(base, kNote, 20000, 777);
    double d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(d < 1e-6, "Plucker: block-size invariant");
  }
  if (testfl::flInstalled()) {
    auto files = testfl::presetFiles("Plucked!");
    size_t walked = 0;
    for (const auto& f : files) {
      auto raw = rawState(f);
      flp::j22::PluckerState st;
      if (flp::j22::parsePlucker(raw.data(), raw.size(), st) && st.consumed == raw.size()) ++walked;
    }
    auto pres = testfl::decodePresets("Plucked!");
    size_t ok = 0, exact = 0, aud = 0;
    for (const auto& d : pres) {
      if (!d.ok || d.state.type != "gl.plucker") continue;
      ++ok;
      exact += d.exact;
      Out a = render(d.state, kNote, 22050);
      aud += a.finite && a.peak > 1e-3f;
    }
    std::snprintf(buf, sizeof buf, "Plucked!: %zu/%zu walk to end, %zu/%zu decode (%zu exact), %zu audible", walked, files.size(), ok,
                  pres.size(), exact, aud);
    expect(walked == files.size() && ok == pres.size() && exact == ok && aud == ok && !files.empty(), buf);
  }
}


// Onsets: count of rises of the short-term level above a threshold.
int onsets(const std::vector<float>& v, size_t win = 256) {
  int n = 0;
  bool up = false;
  double peak = 0;
  for (float x : v) peak = std::max(peak, static_cast<double>(std::fabs(x)));
  for (size_t i = 0; i + win <= v.size(); i += win) {
    double r = rms(v, i, i + win);
    if (!up && r > 0.3 * peak) ++n, up = true;
    if (up && r < 0.1 * peak) up = false;
  }
  return n;
}

void testStepBass() {
  PluginState base = fresh("gl.stepbass");
  Out o = render(base, {{0, true, 1, 36, 0.7f}, {11025, false, 1, 36, 0}}, 22050);
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Step Bass: default note sounds (peak %.3f)", static_cast<double>(o.peak));
    expect(o.finite && o.peak > 0.02f && o.peak < 4.f, buf);
  }
  expect(rms(o.L, 13000, 22050) < 0.05 * rms(o.L, 2000, 11000), "Step Bass: note ends at release (gate VCA)");
  {
    PluginState s = base;
    s.params[SbCutoff] = 0.8f;
    s.params[SbEnvMod] = 0;
    Out a = render(s, {{0, true, 1, 45, 0.7f}}, 22050);
    double h1 = tone(a.L, 4000, 20000, 110), x1 = tone(a.L, 4000, 20000, 116.5);
    expect(h1 > 5 * x1, "Step Bass: A2 rings on 110 Hz");
  }
  {
    PluginState s = base, t = base;
    s.params[SbCutoff] = 0.2f;
    t.params[SbCutoff] = 0.8f;
    s.params[SbEnvMod] = t.params[SbEnvMod] = 0;
    Out a = render(s, {{0, true, 1, 40, 0.7f}}, 11025), b = render(t, {{0, true, 1, 40, 0.7f}}, 11025);
    expect(brightness(b.L, 1000, 11025) > 2 * brightness(a.L, 1000, 11025), "Step Bass: cutoff opens the filter");
  }
  {
    PluginState s = base, t = base;
    s.params[SbCutoff] = t.params[SbCutoff] = 0.3f;
    s.params[SbReso] = 0;
    t.params[SbReso] = 0.95f;
    Out a = render(s, {{0, true, 1, 40, 0.7f}}, 11025), b = render(t, {{0, true, 1, 40, 0.7f}}, 11025);
    double fc = 40.0 * std::pow(9000.0 / 40.0, 0.3);
    expect(tone(b.L, 2000, 11025, fc) > 1.5 * tone(a.L, 2000, 11025, fc) && b.finite, "Step Bass: resonance peaks at the cutoff");
  }
  {
    PluginState s = base, t = base;
    s.params[SbCutoff] = t.params[SbCutoff] = 0.15f;
    s.params[SbEnvMod] = 0;
    t.params[SbEnvMod] = 1;
    Out a = render(s, {{0, true, 1, 40, 0.7f}}, 6000), b = render(t, {{0, true, 1, 40, 0.7f}}, 6000);
    expect(brightness(b.L, 0, 2000) > 1.5 * brightness(a.L, 0, 2000), "Step Bass: env mod sweeps the filter open");
  }
  {
    // Legato: an overlapping second note slides (no new attack) to the new pitch.
    PluginState s = base;
    s.params[SbCutoff] = 0.7f;
    Out a = render(s, {{0, true, 1, 45, 0.7f}, {8000, true, 2, 57, 0.7f}, {9000, false, 1, 45, 0}}, 22050);
    expect(tone(a.L, 14000, 22050, 220) > 3 * tone(a.L, 14000, 22050, 110), "Step Bass: overlapping note slides to the new pitch");
  }
  {
    // Sequencer: holding one key plays the program's 16 steps (default: all C, gate on).
    PluginState s = base;
    s.params[SbSeqOn] = 1;
    s.params[SbTempoMode] = 1;
    s.params[SbBpm] = 120;  // 16th = 5512 samples
    Out a = render(s, {{0, true, 1, 60, 0.7f}}, 44100 * 2);
    int n = onsets(a.L);
    char buf[80];
    std::snprintf(buf, sizeof buf, "Step Bass: sequencer plays 16ths while a key is held (%d onsets in 2 s)", n);
    expect(n >= 12 && n <= 20 && a.finite, buf);
    // Gate off steps rest; key transposes; data edits are picked up.
    s.data = "stepbass 1\nseq 0 4 125 60:1:0:0 60:0:0:0 72:1:1:0 60:0:0:0 | test\n";
    Out b = render(s, {{0, true, 1, 60, 0.7f}}, 44100 * 2);
    int nb = onsets(b.L);
    std::snprintf(buf, sizeof buf, "Step Bass: rest steps leave gaps (%d onsets for 2 gates x 8 bars)", nb);
    expect(nb >= 6 && nb <= 10, buf);
    Out c = render(s, {{0, true, 1, 72, 0.7f}}, 44100);
    double up = tone(c.L, 0, 44100, noteToHzTest(72)), dn = tone(c.L, 0, 44100, noteToHzTest(60));
    expect(up > dn, "Step Bass: the held key transposes the sequence");
  }
  for (int ext = 0; ext < 2; ++ext) {
    PluginState s = base;
    const PluginInfo* info = findPlugin("gl.stepbass");
    for (size_t i = 0; i < s.params.size(); ++i) s.params[i] = static_cast<float>(ext ? info->params[i].max : info->params[i].min);
    s.params[SbProgram] = 0;
    Out a = render(s, {{0, true, 1, 0, 1}, {300, true, 2, 127, 1}, {600, true, 3, 60, 1}, {9000, false, 2, 127, 0}}, 30000);
    expect(a.finite && a.peak < 50.f, ext ? "Step Bass: maximum settings finite" : "Step Bass: minimum settings finite");
  }
  {
    PluginState s = base;
    s.params[SbDlyOn] = s.params[SbRevOn] = s.params[SbDistOn] = 1;
    Out a = render(s, kNote, 30000, 64), b = render(s, kNote, 30000, 999);
    double d = 0;
    for (size_t i = 0; i < a.L.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.L[i] - b.L[i])));
    expect(d < 1e-5, "Step Bass: block-size invariant (with effects)");
  }
  {
    PluginState s;
    s.type = "gl.stepbass";
    s.params = {0.5f};
    normalizePluginState(s);
    expect(s.params.size() == static_cast<size_t>(SbCount), "Step Bass: short state normalises");
    s.data = "garbage\nseq x y\nseq 0 99 nan 1:2:3\nprog -4 9 9\n";
    Out a = render(s, kNote, 8000);
    expect(a.finite, "Step Bass: malformed bank text is ignored safely");
  }
  // Malformed container states.
  {
    std::vector<uint8_t> junk(300, 0);
    flp::j22::StepBassState st;
    expect(!flp::j22::parseStepBass(junk.data(), junk.size(), st), "Transistor Bass parser rejects junk");
    expect(!flp::j22::parseStepBass(nullptr, 0, st), "Transistor Bass parser rejects empty");
  }
  if (testfl::flInstalled()) {
    auto files = testfl::presetFiles("Transistor Bass");
    size_t walked = 0, trunc = 0;
    for (const auto& f : files) {
      auto raw = rawState(f);
      flp::j22::StepBassState st;
      if (flp::j22::parseStepBass(raw.data(), raw.size(), st) && st.consumed == raw.size()) ++walked;
      // Truncations of a real state are rejected.
      bool rej = true;
      for (size_t cut : {raw.size() / 2, raw.size() - 1, static_cast<size_t>(100)}) {
        flp::j22::StepBassState t2;
        rej = rej && !flp::j22::parseStepBass(raw.data(), cut, t2);
      }
      trunc += rej;
    }
    auto pres = testfl::decodePresets("Transistor Bass");
    size_t ok = 0, aud = 0, seqd = 0;
    for (const auto& d : pres) {
      if (!d.ok || d.state.type != "gl.stepbass") continue;
      ++ok;
      seqd += d.state.data.find("seq 127 ") != std::string::npos;
      Out a = render(d.state, {{0, true, 1, 48, 0.8f}, {20000, false, 1, 48, 0}}, 30000);
      aud += a.finite && a.peak > 1e-3f;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "Transistor Bass: %zu/%zu walk to end (%zu reject truncation), %zu/%zu decode with all 128 sequences (%zu), %zu audible",
                  walked, files.size(), trunc, ok, pres.size(), seqd, aud);
    expect(walked == files.size() && trunc == files.size() && ok == pres.size() && seqd == ok && aud == ok && files.size() >= 3, buf);
  }
}

}  // namespace

int main() {
  testCatalog();
  testTwinStringBehaviour();
  testTwinStringRobust();
  testTwinStringParse();
  testSakuraCorpus();
  testPlucker();
  testStepBass();
  return finish();
}
