// Gridline — modulation pack tests: expression language, internal controller
// links (mapping, smoothing, undo, save/load), controllers, note processors
// (timing exact across block sizes), sidechain and sends, patch host.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/Document.h"
#include "core/Expr.h"
#include "core/Json.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/NoteFx.h"
#include "engine/Render.h"
#include "packs/modulation/modulation.h"
#include "packs/modulation/modulation_dsp.h"

using namespace gl;
namespace mod = gl::packs::modulation;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

double evalExpr(const std::string& text, std::vector<double> vars = {}, std::vector<std::string> names = {}) {
  Expr e;
  std::string err;
  if (!e.compile(text, names, &err)) return std::nan("");
  return e.eval(vars.empty() ? nullptr : vars.data());
}

// Runs the engine over a document's song for `frames` samples with a block size.
struct Rig {
  ProgramBuilder builder;
  AudioEngine eng{44100.0};
  std::vector<float> L, R;
  explicit Rig(const Song& s, bool play = true, PlayMode mode = PlayMode::Song) {
    eng.setProgram(builder.build(s));
    eng.setMode(mode);
    if (mode == PlayMode::Pattern && !s.patterns.empty()) eng.setPattern(s.patterns[0].id);
    if (play) eng.play();
  }
  void update(const Song& s) { eng.setProgram(builder.build(s)); }
  // Renders `frames` more samples in blocks of `block`.
  void run(int frames, int block = 512) {
    size_t at = L.size();
    L.resize(at + static_cast<size_t>(frames));
    R.resize(at + static_cast<size_t>(frames));
    for (int pos = 0; pos < frames; pos += block) {
      int n = std::min(block, frames - pos);
      eng.process(L.data() + at + pos, R.data() + at + pos, n);
    }
  }
  double rms(size_t from, size_t to) const {
    double acc = 0;
    size_t n = 0;
    for (size_t i = from; i < to && i < L.size(); ++i, ++n) acc += L[i] * L[i] + R[i] * R[i];
    return n ? std::sqrt(acc / (2.0 * static_cast<double>(n))) : 0.0;
  }
};

// Drives one note processor directly: inputs at absolute sample positions,
// processed in blocks of `block`; returns outputs with absolute positions.
struct FxEvent {
  int64_t at;
  NoteMsg m;
};
std::vector<FxEvent> runFx(const PluginState& st, const NoteFxAux& aux, std::vector<FxEvent> inputs, int64_t total,
                           int block, double tempo = 120, int ppq = 96, double sr = 44100) {
  auto fx = createNoteFx(st.type);
  std::vector<FxEvent> out;
  if (!fx) return out;
  uint32_t ids = 1;
  NoteBuffer in, res;
  for (int64_t pos = 0; pos < total; pos += block) {
    int n = static_cast<int>(std::min<int64_t>(block, total - pos));
    in.clear();
    res.clear();
    for (const auto& e : inputs)
      if (e.at >= pos && e.at < pos + n) {
        NoteMsg m = e.m;
        m.offset = static_cast<int>(e.at - pos);
        in.push(m);
      }
    in.sort();
    NoteFxContext ctx;
    ctx.sampleRate = sr;
    ctx.tempo = tempo;
    ctx.ppq = ppq;
    ctx.from = 0;
    ctx.len = n;
    ctx.anchorTick = 0;
    ctx.clock = pos;
    ctx.spt = sr * 60.0 / (tempo * ppq);
    ctx.playing = true;
    ctx.idCounter = &ids;
    fx->process(st, aux, ctx, in, res);
    for (int i = 0; i < res.count; ++i) out.push_back({pos + res.ev[i].offset, res.ev[i]});
  }
  return out;
}
FxEvent on(int64_t at, uint32_t id, float pitch, float vel = 0.8f) {
  NoteMsg m;
  m.kind = NoteMsg::On;
  m.id = id;
  m.pitch = pitch;
  m.velocity = vel;
  return {at, m};
}
FxEvent off(int64_t at, uint32_t id) {
  NoteMsg m;
  m.kind = NoteMsg::Off;
  m.id = id;
  return {at, m};
}
bool sameEvents(const std::vector<FxEvent>& a, const std::vector<FxEvent>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].at != b[i].at || a[i].m.kind != b[i].m.kind || a[i].m.pitch != b[i].m.pitch || a[i].m.velocity != b[i].m.velocity ||
        a[i].m.target != b[i].m.target)
      return false;
  return true;
}
std::vector<FxEvent> ons(const std::vector<FxEvent>& v) {
  std::vector<FxEvent> r;
  for (const auto& e : v)
    if (e.m.kind == NoteMsg::On) r.push_back(e);
  return r;
}

// Empty song at 120 BPM with one pattern clip of `bars` bars.
Document baseDoc(int bars = 1) {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  d.setPatternLength(d.song().patterns[0].id, bars, 16);
  d.addClip(d.song().patterns[0].id, 0, 0);
  return d;
}

ModLink link(ObjectId srcOwner, int srcSlot, int output, ParamId target) {
  ModLink l;
  l.sourceOwner = srcOwner;
  l.sourceSlot = srcSlot;
  l.sourceOutput = output;
  l.owner = target.owner;
  l.index = target.index;
  return l;
}

}  // namespace

int main() {
  // ------------------------------------------------------------ 1. Expr
  {
    expect(near(evalExpr("1 + 2 * 3"), 7), "expr: precedence of * over +");
    expect(near(evalExpr("(1 + 2) * 3"), 9), "expr: brackets");
    expect(near(evalExpr("2 ^ 3 ^ 2"), 512), "expr: ^ is right-associative");
    expect(near(evalExpr("-2 ^ 2"), -4), "expr: unary minus binds looser than ^");
    expect(near(evalExpr("10 % 4 + 7 / 2"), 5.5), "expr: % and /");
    expect(near(evalExpr("1 < 2 && 3 >= 3"), 1) && near(evalExpr("1 > 2 || 0"), 0), "expr: comparisons and logic");
    expect(near(evalExpr("1 ? 5 : 6"), 5) && near(evalExpr("0 ? 5 : 6"), 6), "expr: ternary");
    expect(near(evalExpr("if(0, 1, 2)"), 2), "expr: if()");
    expect(near(evalExpr("sin(pi / 2)"), 1, 1e-12) && near(evalExpr("cos(0)"), 1), "expr: sin / cos / pi");
    expect(near(evalExpr("min(3, 1, 2) + max(4, 9)"), 10), "expr: variadic min / max");
    expect(near(evalExpr("clamp(5, 0, 1)"), 1) && near(evalExpr("clamp(-1, 0, 1)"), 0), "expr: clamp");
    expect(near(evalExpr("saw(1.25)"), 0.25) && near(evalExpr("tri(0.5)"), 1) && near(evalExpr("sqr(0.75)"), 0),
           "expr: saw / tri / sqr take a phase in cycles");
    expect(near(evalExpr("frac(2.75) + floor(1.5) + ceil(1.2) + round(2.5)"), 0.75 + 1 + 2 + 3), "expr: rounding family");
    expect(near(evalExpr("lerp(10, 20, 0.25)"), 12.5) && near(evalExpr("step(0.5, 0.7)"), 1), "expr: lerp / step");
    expect(near(evalExpr("tension(0.5, 0)"), 0.5) && evalExpr("tension(0.5, 1)") < 0.1, "expr: tension()");
    expect(near(evalExpr("a * 2 + B", {0.25, 1.0}, {"a", "b"}), 1.5), "expr: variables (case-insensitive)");
    expect(near(evalExpr("1 / 0"), 0) && near(evalExpr("sqrt(-1)"), 0) && near(evalExpr("log(0)"), -1e9), "expr: no NaN / inf");
    Expr e;
    std::string err;
    expect(!e.compile("1 +", {}, &err) && !err.empty(), "expr: incomplete formula is an error");
    expect(!e.compile("foo(1)", {}, &err) && err.find("foo") != std::string::npos, "expr: unknown function named in the error");
    expect(!e.compile("x + 1", {"a"}, &err) && err.find("x") != std::string::npos, "expr: unknown variable is an error");
    expect(!e.compile("sin(1, 2)", {}, &err), "expr: wrong argument count is an error");
    expect(!e.compile("(1 + 2", {}, &err), "expr: unbalanced bracket is an error");
    expect(e.empty() && e.eval(nullptr) == 0.0, "expr: failed compile evaluates to 0");
    Expr r;
    r.compile("rand()", {});
    uint32_t s1 = 7, s2 = 7;
    double x1 = r.eval(nullptr, &s1), x2 = r.eval(nullptr, &s2);
    expect(x1 == x2 && x1 >= 0 && x1 <= 1 && r.eval(nullptr, &s1) != x1, "expr: rand() is seeded and advances");
    expect(near(evalExpr("rand(5)"), evalExpr("rand(5)")) && !near(evalExpr("rand(5)"), evalExpr("rand(6)")),
           "expr: rand(seed) is a fixed hash");
    std::string deep(300, '(');
    expect(!e.compile(deep + "1" + std::string(300, ')'), {}, &err), "expr: absurd nesting is refused");
  }

  // ------------------------------------------------- 2. mapping curves
  {
    expect(near(applyModCurve(ModCurve::Linear, 0.3), 0.3) && near(applyModCurve(ModCurve::Inverted, 0.3), 0.7),
           "curve: linear / inverted");
    expect(near(applyModCurve(ModCurve::Log, 0), 0) && near(applyModCurve(ModCurve::Log, 1), 1, 1e-12) &&
               applyModCurve(ModCurve::Log, 0.2) > 0.2,
           "curve: fast rise spans 0..1 and rises early");
    expect(near(applyModCurve(ModCurve::InvLog, 1), 1, 1e-12) && applyModCurve(ModCurve::InvLog, 0.2) < 0.2,
           "curve: slow rise");
    expect(applyModCurve(ModCurve::Switch, 0.49) == 0 && applyModCurve(ModCurve::Switch, 0.5) == 1, "curve: switch");
    expect(near(applyModCurve(ModCurve::FirstHalf, 0.25), 0.5) && near(applyModCurve(ModCurve::LastHalf, 0.75), 0.5),
           "curve: first / second half");
    expect(near(applyModCurve(ModCurve::DeadZone, 0.52), 0.5) && near(applyModCurve(ModCurve::DeadZone, 1), 1),
           "curve: centre dead zone");
    int distinct = 0;
    double prev = -1;
    for (int i = 0; i <= 100; ++i) {
      double v = applyModCurve(ModCurve::Steps, i / 100.0);
      if (v != prev) ++distinct;
      prev = v;
    }
    expect(distinct == 8, "curve: 8 steps");
  }

  // ------------------- 3. links: X-Y pad drives a channel's volume (mapping math)
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("lead", "gl.synth");
    ObjectId ins = d.song().mixer[1].id;
    d.setSlotPlugin(ins, 0, "gl.xyctl");
    d.setParam(slotParam(ins, 0, mod::XyX), 0.25);
    d.setParam(slotParam(ins, 0, mod::XySmooth), 0.001);
    ModLink l = link(ins, 0, 0, channelParam(ch, ChannelParam::Volume));
    l.min = 0.2f;
    l.max = 0.6f;
    expect(d.setModLink(l), "link X output to channel volume");
    expect(isModulated(d.song(), channelParam(ch, ChannelParam::Volume)), "isModulated() reports the linked knob");
    expect(!isModulated(d.song(), channelParam(ch, ChannelParam::Pan)), "unlinked knob is not modulated");
    expect(listControllerOutputs(d.song()).size() == 2, "controller outputs list the pad's X and Y");
    Rig rig(d.song());
    rig.run(4096, 256);
    double v = 0;
    const ParamDesc vol = *describeParam(d.song(), channelParam(ch, ChannelParam::Volume));
    expect(rig.builder.modulatedValue(channelParam(ch, ChannelParam::Volume), v) &&
               near(v, vol.fromNormalized(0.2 + 0.25 * 0.4), 1e-4),
           "linked value = range min + X * (max - min), in target units");
    // invert + formula
    d.editModLink(channelParam(ch, ChannelParam::Volume), "edit", [](ModLink& x) {
      x.curve = ModCurve::Formula;
      x.formula = "1 - input";
      x.min = 0;
      x.max = 1;
    });
    rig.update(d.song());
    rig.run(4096, 256);
    rig.builder.modulatedValue(channelParam(ch, ChannelParam::Volume), v);
    expect(near(v, vol.fromNormalized(0.75), 1e-4), "formula mapping (1 - input) inverts the controller");
    d.editModLink(channelParam(ch, ChannelParam::Volume), "edit", [](ModLink& x) {
      x.curve = ModCurve::Formula;
      x.formula = "base";
    });
    rig.update(d.song());
    rig.run(1024, 256);
    rig.builder.modulatedValue(channelParam(ch, ChannelParam::Volume), v);
    expect(near(v, 0.78, 1e-4), "formula variable `base` is the knob's own value");
    // smoothing: a jump of the controller is approached gradually
    d.editModLink(channelParam(ch, ChannelParam::Volume), "edit", [](ModLink& x) {
      x.curve = ModCurve::Linear;
      x.smoothMs = 200;
    });
    rig.update(d.song());
    rig.run(4096, 256);
    d.setParam(slotParam(ins, 0, mod::XyX), 1.0);
    rig.update(d.song());
    rig.run(2205, 256);  // 50 ms
    double mid = 0, end = 0;
    rig.builder.modulatedValue(channelParam(ch, ChannelParam::Volume), mid);
    rig.run(88200, 256);
    rig.builder.modulatedValue(channelParam(ch, ChannelParam::Volume), end);
    double n0 = vol.toNormalized(mid);
    expect(n0 > 0.3 && n0 < 0.9, "smoothing: halfway after a fraction of the smoothing time");
    expect(near(vol.toNormalized(end), 1.0, 1e-3), "smoothing: settles on the target");
    // undo / redo of link edits
    size_t before = d.song().modLinks.size();
    expect(d.removeModLink(channelParam(ch, ChannelParam::Volume)) && d.song().modLinks.size() == before - 1, "remove link");
    expect(d.undo() && d.song().modLinks.size() == before, "undo restores the link");
    expect(d.redo() && d.song().modLinks.empty(), "redo removes it again");
    d.undo();
    // refused links
    expect(!d.setModLink(link(ch, -1, 0, channelParam(ch, ChannelParam::Pan))), "a synth is not a controller source");
    expect(!d.setModLink(link(ins, 0, 0, {ch, 99999})), "unresolved targets are refused");
    // replacing the controller plugin drops its links (undoably)
    d.setSlotPlugin(ins, 0, "gl.filter");
    expect(d.song().modLinks.empty(), "replacing the controller drops its links");
    d.undo();
    expect(d.song().modLinks.size() == 1, "undo brings the controller and its link back");
    // moving the slot keeps the link attached
    d.moveSlot(ins, 0, 3);
    expect(d.song().modLinks.size() == 1 && d.song().modLinks[0].sourceSlot == 3, "links follow a moved controller slot");
    d.undo();
    // deleting the target channel removes the link; undo restores both
    d.removeChannel(ch);
    expect(d.song().modLinks.empty(), "deleting the target channel removes the link");
    d.undo();
    expect(d.song().modLinks.size() == 1 && d.song().channel(ch), "undo restores channel and link");
  }

  // ----------------------------- 4. links: every kind of target resolves and applies
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("lead", "gl.synth");
    ObjectId ins = d.song().mixer[1].id, ins2 = d.song().mixer[2].id;
    d.setSlotPlugin(ins, 0, "gl.surface");
    d.setSlotPlugin(ins2, 1, "gl.filter");
    d.addNoteFx(ch, "gl.arp");
    for (int k = 0; k < 8; ++k) d.setParam(slotParam(ins, 0, k), 0.1 * (k + 1));
    d.setParam(slotParam(ins, 0, mod::SfSmooth), 0.001);
    std::vector<ParamId> targets = {channelPluginParam(ch, pp::SynCutoff), channelParam(ch, ChannelParam::Pan),
                                    mixerParam(ins2, MixerParam::Volume), mixerParam(ins2, MixerParam::Pan),
                                    slotParam(ins2, 1, pp::FltCutoff), slotMixParam(ins2, 1),
                                    noteFxParam(ch, 0, mod::ArGate), channelParam(ch, ChannelParam::Pitch)};
    for (size_t i = 0; i < targets.size(); ++i) d.setModLink(link(ins, 0, static_cast<int>(i), targets[i]));
    expect(d.song().modLinks.size() == targets.size(), "links to plugin, strip, mixer, slot, mix and note-FX targets");
    Rig rig(d.song());
    rig.run(8192, 128);
    bool all = true;
    for (size_t i = 0; i < targets.size(); ++i) {
      double v = 0;
      auto desc = describeParam(d.song(), targets[i]);
      bool ok = rig.builder.modulatedValue(targets[i], v) && near(v, desc->fromNormalized(0.1 * static_cast<double>(i + 1)), 1e-3);
      if (!ok) std::printf("# target %zu: %f\n", i, v);
      all = all && ok;
    }
    expect(all, "each target follows its control (live values)");
    auto beforeVal = getParam(d.song(), channelPluginParam(ch, pp::SynCutoff));
    expect(beforeVal && near(*beforeVal, describeParam(d.song(), channelPluginParam(ch, pp::SynCutoff))->def, 1e-6),
           "stored parameter keeps its own value (modulation is non-destructive)");
  }

  // --------------------- 5. peak follower follows a burst and ducks another channel
  {
    Document d = baseDoc(2);
    ObjectId pat = d.song().patterns[0].id;
    ObjectId kick = d.addChannel("kick", "gl.kick");
    ObjectId pad = d.addChannel("pad", "gl.synth");
    ObjectId ins1 = d.song().mixer[1].id, ins2 = d.song().mixer[2].id;
    d.routeChannel(kick, ins1);
    d.routeChannel(pad, ins2);
    d.setStep(pat, kick, 0, 127);
    d.addNote(pat, pad, {0, 96 * 8, 60, 100});
    d.setParam(channelPluginParam(kick, pp::KickDecay), 0.08);  // short hit
    d.setSlotPlugin(ins1, 0, "gl.peakctl");
    d.setParam(slotParam(ins1, 0, mod::PkDecay), 0.1);
    d.setParam(slotParam(ins1, 0, mod::PkRamp), 0.0);
    Rig rig(d.song());
    rig.run(2048, 256);
    float hit = rig.builder.controllerOutput(ins1, 0, 0);
    rig.run(44100, 256);
    float later = rig.builder.controllerOutput(ins1, 0, 0);
    expect(hit > 0.2f, "peak output rises with the kick");
    expect(later < 0.02f, "peak output decays in silence");
    expect(rig.builder.controllerOutput(ins1, 0, 1) >= 0.0f, "LFO output exists");
    // duck: peak → pad volume, inverted
    ModLink l = link(ins1, 0, 0, channelParam(pad, ChannelParam::Volume));
    l.curve = ModCurve::Inverted;
    l.min = 0.0f;
    l.max = describeParam(d.song(), channelParam(pad, ChannelParam::Volume))->toNormalized(0.78);
    expect(d.setModLink(l), "link the peak to the pad's volume (inverted)");
    d.setParam(mixerParam(ins1, MixerParam::Volume), 0.0);  // hear only the pad; the follower still hears the kick
    Document plain(d.song());
    plain.removeModLink(channelParam(pad, ChannelParam::Volume));
    Rig a(d.song()), b(plain.song());
    a.run(22050, 256);
    b.run(22050, 256);
    double ducked = a.rms(1500, 4000), open = b.rms(1500, 4000), recovered = a.rms(15000, 22050), plainLate = b.rms(15000, 22050);
    expect(ducked < open * 0.6, "peak-linked volume ducks the pad under the kick");
    expect(recovered > plainLate * 0.8, "the pad recovers after the kick decays");
  }

  // ----------------------------------------------------- 6. formula controller
  {
    Document d = baseDoc();
    ObjectId ins = d.song().mixer[1].id;
    d.setSlotPlugin(ins, 0, "gl.formulactl");
    d.editMixerTrack(ins, "formula", [](MixerTrack& m) { m.slots[0].plugin.data = "a*0.5 + b*0.25 + c*0"; });
    d.setParam(slotParam(ins, 0, mod::FmA), 0.8);
    d.setParam(slotParam(ins, 0, mod::FmB), 0.4);
    d.setParam(slotParam(ins, 0, mod::FmSmooth), 0.001);
    Rig rig(d.song());
    rig.run(4096);
    expect(near(rig.builder.controllerOutput(ins, 0, 0), 0.5, 1e-3), "formula controller computes a*0.5 + b*0.25");
    d.editMixerTrack(ins, "formula", [](MixerTrack& m) { m.slots[0].plugin.data = "clamp(a * 3, 0, 1)"; });
    rig.update(d.song());
    rig.run(4096);
    expect(near(rig.builder.controllerOutput(ins, 0, 0), 1.0, 1e-3), "formula output is clamped to 0..1");
    d.editMixerTrack(ins, "formula", [](MixerTrack& m) { m.slots[0].plugin.data = "this is not maths"; });
    rig.update(d.song());
    rig.run(4096);
    expect(near(rig.builder.controllerOutput(ins, 0, 0), 0.8, 1e-3), "a broken formula falls back to `a`");
    d.editMixerTrack(ins, "formula", [](MixerTrack& m) { m.slots[0].plugin.data = "saw(beat)"; });
    rig.update(d.song());
    rig.run(11025);
    float sawv = rig.builder.controllerOutput(ins, 0, 0);
    double beatNow = (3 * 4096 + 11025) / 22050.0;  // 120 BPM: 22050 samples per beat
    expect(near(sawv, beatNow - std::floor(beatNow), 0.01), "time variables (beat) drive the output");
    Expr e;
    std::string err;
    expect(mod::compileFormula("sin(t*2*pi)*0.5+0.5 + in*0", e, &err), "formula controller variables compile");
  }

  // ----------------------------------- 7. envelope / keyboard / voltage / surface
  {
    Document d = baseDoc();
    ObjectId pat = d.song().patterns[0].id;
    ObjectId env = d.addChannel("env", "gl.envctl");
    d.addNote(pat, env, {0, 96, 60, 127});
    Rig rig(d.song());
    rig.run(441, 64);  // 10 ms: past the 5 ms attack point
    float peak = rig.builder.controllerOutput(env, -1, 0);
    rig.run(13230, 64);  // 0.31 s: sustain at 0.6
    float sus = rig.builder.controllerOutput(env, -1, 0);
    rig.run(44100, 64);
    float rel = rig.builder.controllerOutput(env, -1, 0);
    expect(peak > 0.9f, "envelope controller: attack reaches point 1");
    expect(near(sus, 0.6, 0.02), "envelope controller: holds the sustain point while the note is held");
    expect(rel < 0.05f, "envelope controller: releases after the note");
    expect(rig.rms(0, rig.L.size()) == 0.0, "envelope controller makes no sound");

    Document k = baseDoc();
    ObjectId kc = k.addChannel("keys", "gl.keyctl");
    k.editChannel(kc, "keys", [](Channel& c) {
      std::vector<float> v(128, 0.0f);
      v[64] = 0.9f;
      c.plugin.data = mod::formatKeyValues(v);
    });
    k.setParam(channelPluginParam(kc, mod::KcAttack), 0.001);
    k.addNote(k.song().patterns[0].id, kc, {0, 96, 64, 100});
    Rig kr(k.song());
    kr.run(4410);
    expect(near(kr.builder.controllerOutput(kc, -1, 0), 0.9, 0.01), "keyboard controller outputs the key's value");
    expect(near(kr.builder.controllerOutput(kc, -1, 2), 1.0) && near(kr.builder.controllerOutput(kc, -1, 3), 64 / 127.0, 1e-4),
           "keyboard controller gate and note outputs");
    std::vector<float> back = mod::parseKeyValues(mod::formatKeyValues(std::vector<float>(128, 0.25f)));
    expect(back.size() == 128 && near(back[100], 0.25, 1e-4), "key table data round-trips");

    Document c = baseDoc();
    ObjectId cv = c.addChannel("cv", "gl.cvctl");
    c.setParam(channelPluginParam(cv, mod::CvEnable), 1);
    c.addNote(c.song().patterns[0].id, cv, {0, 192, 72, 100});
    Rig cr(c.song());
    cr.run(4410);
    expect(near(cr.L[4000], 0.1, 1e-4), "voltage out: one octave above C4 = +1 V = 0.1 full scale (left = pitch)");
    expect(near(cr.R[4000], 0.5, 1e-4), "voltage out: gate high = 5 V (right = gate)");

    Document s = baseDoc();
    ObjectId ins = s.song().mixer[1].id;
    s.setSlotPlugin(ins, 0, "gl.surface");
    s.setParam(slotParam(ins, 0, 5), 0.7);
    s.setParam(slotParam(ins, 0, mod::SfSmooth), 0.001);
    Rig sr(s.song());
    sr.run(4096);
    expect(near(sr.builder.controllerOutput(ins, 0, 5), 0.7, 1e-3), "control surface outputs its controls");
  }

  // ------------------------------- 8. arpeggiator: exact timing, any block size
  {
    PluginState arp = defaultPluginState("gl.arp");
    NoteFxAux aux;
    std::vector<FxEvent> in = {on(1000, 1, 60), on(1000, 2, 64), on(1000, 3, 67), off(60000, 1), off(60000, 2), off(60000, 3)};
    auto ref = runFx(arp, aux, in, 70000, 512);
    auto o = ons(ref);
    const double step = 0.25 * 96 * (44100.0 * 60.0 / (120.0 * 96));  // 1/16 at 120 BPM = 5512.5 samples
    bool timing = !o.empty();
    for (size_t k = 0; k < o.size(); ++k)
      timing = timing && o[k].at == 1000 + static_cast<int64_t>(std::ceil(k * step - 1e-9));
    expect(timing, "arpeggiator steps land exactly on the 1/16 grid from the first note");
    expect(o.size() == 11, "arpeggiator plays every step until release (11 steps)");
    expect(o.size() >= 4 && o[0].m.pitch == 60 && o[1].m.pitch == 64 && o[2].m.pitch == 67 && o[3].m.pitch == 60,
           "arpeggiator up pattern");
    bool gate = true;
    for (const auto& e : ref)
      if (e.m.kind == NoteMsg::Off)
        for (const auto& x : o)
          if (x.m.id == e.m.id) gate = gate && (e.at - x.at == static_cast<int64_t>(std::ceil(step * 0.5 - 1e-9)) || e.at == 60000);
    expect(gate, "arpeggiator gate = 50% of a step");
    bool invariant = true;
    for (int bs : {1, 7, 64, 100, 333, 1024}) invariant = invariant && sameEvents(runFx(arp, aux, in, 70000, bs), ref);
    expect(invariant, "arpeggiator output identical for block sizes 1, 7, 64, 100, 333, 1024");
    int offs = 0, onsN = 0;
    for (const auto& e : ref) (e.m.kind == NoteMsg::On ? onsN : offs) += 1;
    expect(onsN == offs, "every arpeggiated note is released");
    // directions
    arp.params[mod::ArDirection] = mod::AdDown;
    auto down = ons(runFx(arp, aux, in, 20000, 256));
    expect(down.size() >= 3 && down[0].m.pitch == 67 && down[1].m.pitch == 64 && down[2].m.pitch == 60, "arpeggiator down");
    arp.params[mod::ArDirection] = mod::AdUpDown;
    arp.params[mod::ArRange] = 1;
    auto ud = ons(runFx(arp, aux, in, 60000, 256));
    std::vector<float> want = {60, 64, 67, 72, 76, 79, 76, 72, 67, 64};
    bool udOk = ud.size() >= want.size();
    for (size_t i = 0; udOk && i < want.size(); ++i) udOk = ud[i].m.pitch == want[i];
    expect(udOk, "arpeggiator up-down over two octaves");
    arp.params[mod::ArDirection] = mod::AdChord;
    auto chord = ons(runFx(arp, aux, in, 6000, 256));
    expect(chord.size() == 6 && chord[0].at == chord[5].at, "arpeggiator chord mode plays all held notes per step");
    // latch keeps running after release
    PluginState latch = defaultPluginState("gl.arp");
    latch.params[mod::ArLatch] = 1;
    auto lt = ons(runFx(latch, aux, {on(0, 1, 60), off(100, 1)}, 30000, 256));
    expect(lt.size() == 6, "latch keeps arpeggiating released notes");
    // swing delays odd steps
    PluginState sw = defaultPluginState("gl.arp");
    sw.params[mod::ArSwing] = 0.5f;
    auto so = ons(runFx(sw, aux, {on(0, 1, 60), off(30000, 1)}, 30000, 256));
    expect(so.size() >= 3 && so[1].at == static_cast<int64_t>(std::ceil(step * 1.25 - 1e-9)) &&
               so[2].at == static_cast<int64_t>(std::ceil(step * 2 - 1e-9)),
           "swing delays odd steps by swing x half a step");
  }

  // ------------------------- 9. arpeggiator in the engine: render is block-size invariant
  {
    Document d = baseDoc();
    ObjectId pat = d.song().patterns[0].id;
    ObjectId ch = d.addChannel("arp", "gl.synth");
    d.addNote(pat, ch, {0, 300, 60, 100});
    d.addNote(pat, ch, {0, 300, 63, 100});
    expect(d.addNoteFx(ch, "gl.arp"), "add arpeggiator to a channel");
    RenderOptions o;
    o.tail = 96;
    o.blockSize = 512;
    AudioBuffer ref = renderSong(d.song(), o);
    bool same = true;
    for (int bs : {1, 64, 100, 1000}) {
      o.blockSize = bs;
      same = same && renderSong(d.song(), o).data == ref.data;
    }
    expect(ref.peak() > 0.01f && same, "channel with an arpeggiator renders identically for block sizes 1, 64, 100, 512, 1000");
    Document plain(d.song());
    plain.removeNoteFx(ch, 0);
    o.blockSize = 512;
    expect(renderSong(plain.song(), o).data != ref.data, "the arpeggiator changes what plays");
  }

  // ------------------------------------------------------- 10. chord generator
  {
    PluginState c = defaultPluginState("gl.chord");
    NoteFxAux aux;
    auto ev = runFx(c, aux, {on(100, 1, 60, 1.0f), off(5000, 1)}, 8000, 256);
    auto o = ons(ev);
    expect(o.size() == 3 && o[0].m.pitch == 60 && o[1].m.pitch == 64 && o[2].m.pitch == 67 && o[0].at == 100 && o[2].at == 100,
           "major chord from one note");
    int offs = 0;
    for (const auto& e : ev) offs += e.m.kind == NoteMsg::Off && e.at == 5000;
    expect(offs == 3, "releasing the key releases the whole chord");
    c.params[mod::ChType] = 4;  // minor 7
    c.params[mod::ChInversion] = 1;
    auto inv = ons(runFx(c, aux, {on(0, 1, 60)}, 1000, 256));
    expect(inv.size() == 4 && inv[0].m.pitch == 63 && inv[3].m.pitch == 72, "minor 7, first inversion");
    PluginState s = defaultPluginState("gl.chord");
    s.params[mod::ChStrum] = 0.01f;
    auto st = ons(runFx(s, aux, {on(0, 1, 60), off(30000, 1)}, 40000, 128));
    expect(st.size() == 3 && st[0].at == 0 && st[1].at == 441 && st[2].at == 882, "strum spaces notes by 10 ms");
    expect(sameEvents(runFx(s, aux, {on(0, 1, 60), off(30000, 1)}, 40000, 37), runFx(s, aux, {on(0, 1, 60), off(30000, 1)}, 40000, 128)),
           "strum timing is block-size invariant");
    auto early = runFx(s, aux, {on(0, 1, 60), off(500, 1)}, 4000, 64);
    expect(ons(early).size() == 2, "notes not yet strummed when the key lifts never sound");
  }

  // ----------------------------------------------------- 11. note sequencer
  {
    PluginState q = defaultPluginState("gl.notesq");
    NoteFxAux aux;
    for (int k = 0; k < mod::kSeqSteps; ++k) {
      q.params[static_cast<size_t>(mod::seqParam(k, mod::SqNote))] = static_cast<float>(k % 3);
      q.params[static_cast<size_t>(mod::seqParam(k, mod::SqOn))] = k == 3 ? 0.f : 1.f;
    }
    q.params[static_cast<size_t>(mod::seqParam(4, mod::SqOctave))] = 1;
    auto o = ons(runFx(q, aux, {on(0, 1, 67), on(0, 2, 60), on(0, 3, 64), off(30000, 1), off(30000, 2), off(30000, 3)}, 30000, 200));
    expect(o.size() == 5 && o[0].m.pitch == 60 && o[1].m.pitch == 64 && o[2].m.pitch == 67 && o[3].m.pitch == 76,
           "sequencer: steps pick held notes by index (lowest first), rests skip, octave shift");
    expect(o.size() >= 4 && o[3].at == static_cast<int64_t>(std::ceil(4 * 5512.5 - 1e-9)), "sequencer: step 5 on the grid");
  }

  // ------------------------------------------------------------ 12. key mapper
  {
    PluginState k = defaultPluginState("gl.keymap");
    NoteFxAux aux;
    k.params[mod::KmScale] = 1;  // C major
    k.params[mod::KmSnap] = 1;
    k.params[mod::KmTranspose] = 12;
    auto o = ons(runFx(k, aux, {on(0, 1, 61), on(0, 2, 62)}, 256, 256));
    expect(o.size() == 2 && (o[0].m.pitch == 72 || o[0].m.pitch == 74) && o[1].m.pitch == 74, "key mapper: transpose + snap to scale");
    PluginState mute = defaultPluginState("gl.keymap");
    mute.params[mod::KmMap + 1] = -13;
    expect(ons(runFx(mute, aux, {on(0, 1, 61), on(0, 2, 60)}, 256, 256)).size() == 1, "key mapper: muted pitch class");
    PluginState both = defaultPluginState("gl.keymap");
    for (int i = 0; i < 12; ++i) both.params[static_cast<size_t>(mod::KmMap + i)] = 7;
    both.params[mod::KmMulti] = 1;
    auto h = runFx(both, aux, {on(0, 1, 60), off(100, 1)}, 256, 256);
    expect(ons(h).size() == 2 && ons(h)[1].m.pitch == 67 && h.size() == 4, "key mapper: mapped + original harmony, both released");
  }

  // ----------------------------------------------- 13. keyboard splitter routing
  {
    Document d = baseDoc();
    ObjectId pat = d.song().patterns[0].id;
    ObjectId a = d.addChannel("keys", "gl.synth");
    ObjectId b = d.addChannel("bass", "gl.synth");
    ObjectId ia = d.song().mixer[1].id, ib = d.song().mixer[2].id;
    d.routeChannel(a, ia);
    d.routeChannel(b, ib);
    d.addNoteFx(a, "gl.split");
    d.setNoteFxState(a, 0, [&] {
      PluginState s = d.song().channel(a)->noteFx[0];
      s.data = makeNoteFxData({b, a}, "");
      return s;
    }(), "targets");
    d.addNote(pat, a, {0, 96, 40, 100});  // low zone → bass channel
    Rig r1(d.song());
    r1.run(8192);
    float pa = r1.builder.mixerRuntime(ia)->peakL.load(), pb = r1.builder.mixerRuntime(ib)->peakL.load();
    expect(pb > 0.01f && pa == 0.0f, "low key is routed to the other channel only");
    Document d2(d.song());
    d2.setNotes(pat, a, {{0, 96, 72, 100}});  // high zone → this channel
    Rig r2(d2.song());
    r2.run(8192);
    expect(r2.builder.mixerRuntime(ia)->peakL.load() > 0.01f && r2.builder.mixerRuntime(ib)->peakL.load() == 0.0f,
           "high key stays on this channel");
    PluginState sp = defaultPluginState("gl.split");
    NoteFxAux aux;
    aux.targets[0] = 5;
    sp.params[static_cast<size_t>(mod::splitParam(0, mod::SpTranspose))] = -12;
    auto ev = runFx(sp, aux, {on(0, 1, 50), off(100, 1)}, 256, 256);
    expect(ev.size() == 2 && ev[0].m.target == 5 && ev[0].m.pitch == 38 && ev[1].m.target == 5, "splitter: zone target and transpose, off follows");
  }

  // ------------------------------------------------- 14. level scaler / colour / script
  {
    NoteFxAux aux;
    PluginState v = defaultPluginState("gl.velscale");
    v.params[mod::VsMultiply] = 0;
    v.params[mod::VsOffset] = 0.9f;
    auto o = ons(runFx(v, aux, {on(0, 1, 60, 0.2f)}, 64, 64));
    expect(o.size() == 1 && near(o[0].m.velocity, 0.9, 1e-6), "level scaler: fixed velocity");
    PluginState c = defaultPluginState("gl.velscale");
    c.params[mod::VsCenter] = 0.5f;
    c.params[mod::VsMultiply] = 0.5f;
    auto oc = ons(runFx(c, aux, {on(0, 1, 60, 1.0f), on(0, 2, 62, 0.0f)}, 64, 64));
    expect(oc.size() == 2 && near(oc[0].m.velocity, 0.75, 1e-6) && near(oc[1].m.velocity, 0.25, 1e-6), "level scaler: compress around the centre");
    PluginState h = defaultPluginState("gl.velscale");
    h.params[mod::VsHumanize] = 0.3f;
    std::vector<FxEvent> many;
    for (int i = 0; i < 20; ++i) many.push_back(on(i * 10, static_cast<uint32_t>(i + 1), 60, 0.5f));
    auto hv = ons(runFx(h, aux, many, 400, 64));
    bool varied = false, bounded = true;
    for (const auto& e : hv) {
      varied = varied || std::fabs(e.m.velocity - 0.5f) > 0.01f;
      bounded = bounded && std::fabs(e.m.velocity - 0.5f) <= 0.3f + 1e-6;
    }
    expect(varied && bounded && sameEvents(hv, ons(runFx(h, aux, many, 400, 64))), "level scaler: humanize is bounded and repeatable");

    PluginState cm = defaultPluginState("gl.colormap");
    cm.params[mod::CmSource] = mod::CsVelocity;
    for (int g = 8; g < 16; ++g) cm.params[static_cast<size_t>(mod::CmMap + g)] = 1;
    NoteFxAux caux;
    caux.targets[0] = 3;
    auto co = runFx(cm, caux, {on(0, 1, 60, 0.9f), on(0, 2, 60, 0.2f), off(50, 1)}, 64, 64);
    expect(co.size() == 3 && co[0].m.target == 3 && co[1].m.target == -1 && co[2].m.target == 3, "colour router: velocity layers to targets");
    PluginState cc = defaultPluginState("gl.colormap");
    cc.params[static_cast<size_t>(mod::CmMap + 2)] = 9;  // colour 3 muted
    NoteMsg m;
    m.kind = NoteMsg::On;
    m.id = 1;
    m.pitch = 60;
    m.color = 2;
    expect(runFx(cc, caux, {{0, m}}, 64, 64).empty(), "colour router: muted colour group");

    PluginState ns = defaultPluginState("gl.notescript");
    ns.data = "pitch = pitch + 12 * (a > 0.4)\nvelocity = vel * 0.5\nplay = pitch != 61\n";
    NoteFxAux saux;
    prepareNoteFxAux(ns, saux);
    auto so = runFx(ns, saux, {on(0, 1, 60, 0.8f), on(0, 2, 61, 0.8f), off(10, 2), off(10, 1)}, 64, 64);
    auto son = ons(so);
    expect(son.size() == 1 && son[0].m.pitch == 72 && near(son[0].m.velocity, 0.4, 1e-6), "note script: pitch / velocity / play formulas");
    int offs = 0;
    for (const auto& e : so) offs += e.m.kind == NoteMsg::Off;
    expect(offs == 1, "note script: offs of dropped notes are dropped too");
    mod::NoteScriptProgram prog;
    mod::compileNoteScript("pitch = nonsense(", prog);
    expect(!prog.error.empty(), "note script: compile errors are reported");
  }

  // ---------------------------------------- 15. sidechain ducker and sends
  {
    Document d = baseDoc();
    ObjectId pat = d.song().patterns[0].id;
    ObjectId kick = d.addChannel("kick", "gl.kick");
    ObjectId pad = d.addChannel("pad", "gl.synth");
    ObjectId ik = d.song().mixer[1].id, ip = d.song().mixer[2].id;
    d.routeChannel(kick, ik);
    d.routeChannel(pad, ip);
    for (int s = 0; s < 16; s += 4) d.setStep(pat, kick, s, 127);
    d.addNote(pat, pad, {0, 384, 48, 110});
    d.setParam(channelPluginParam(kick, pp::KickDecay), 0.08);
    d.setSlotPlugin(ip, 0, "gl.scduck");
    d.setSend(ik, ip, 1.0f);
    expect(d.setSendSidechain(ik, ip, true) && d.song().mixer[1].sends.size() == 2, "sidechain-only send from the kick to the pad");
    d.editMixerTrack(ik, "silence kick to master", [&](MixerTrack& m) {
      for (auto& sd : m.sends)
        if (!sd.sidechain) sd.level = 0;
    });
    Rig duck(d.song());
    duck.run(44100, 256);
    Document noKey(d.song());
    noKey.setSendSidechain(ik, ip, false);
    noKey.setSend(ik, ip, 0.0f);
    Rig open(noKey.song());
    open.run(44100, 256);
    double dk = duck.rms(500, 4000), op = open.rms(500, 4000);
    expect(dk < op * 0.5, "sidechain ducker reduces the pad while the kick plays");
    expect(duck.rms(18000, 21500) > open.rms(18000, 21500) * 0.6, "ducker releases between kicks");
    float gr = duck.builder.effectRuntime(ip, 0)->meter.load();
    expect(gr <= 0.0f, "ducker meter reports gain reduction");
    // the sidechain route adds no kick audio to the pad's track
    Document onlyKick(d.song());
    onlyKick.setParam(channelParam(pad, ChannelParam::Mute), 1);
    onlyKick.setSlotPlugin(ip, 0, "");
    Rig ok(onlyKick.song());
    ok.run(8192);
    expect(ok.builder.mixerRuntime(ip)->peakL.load() == 0.0f, "a sidechain-only send does not mix audio into the destination");

    // Mixer Send: from insert 1 to insert 3 at a point in the chain.
    Document s = baseDoc();
    ObjectId c = s.addChannel("lead", "gl.synth");
    ObjectId i1 = s.song().mixer[1].id, i3 = s.song().mixer[3].id;
    s.routeChannel(c, i1);
    s.addNote(s.song().patterns[0].id, c, {0, 192, 60, 100});
    s.setSlotPlugin(i1, 0, "gl.send");
    s.setParam(slotParam(i1, 0, mod::SnTarget), 3);
    s.setParam(slotParam(i1, 0, mod::SnDry), 0.0);
    Rig sr(s.song());
    sr.run(8192);
    expect(sr.builder.mixerRuntime(i3)->peakL.load() > 0.01f && sr.builder.mixerRuntime(i1)->peakL.load() == 0.0f,
           "mixer send moves the audio to the target track");
    expect(sr.rms(0, 8192) > 0.001, "sent audio reaches the master through the target");
    // Bus send / return between inserts 1 and 4.
    Document b(s.song());
    b.setSlotPlugin(i1, 0, "gl.bussend");
    b.setParam(slotParam(i1, 0, mod::BsBus), 2);
    b.setParam(slotParam(i1, 0, mod::BsDry), 0.0);
    ObjectId i4 = b.song().mixer[4].id;
    b.setSlotPlugin(i4, 0, "gl.busreturn");
    b.setParam(slotParam(i4, 0, mod::BrBus), 2);
    Rig br(b.song());
    br.run(8192);
    expect(br.builder.mixerRuntime(i4)->peakL.load() > 0.01f, "bus return receives what the bus send sends");
    Document b2(b.song());
    b2.setParam(slotParam(i4, 0, mod::BrBus), 3);
    Rig br2(b2.song());
    br2.run(8192);
    expect(br2.builder.mixerRuntime(i4)->peakL.load() == 0.0f, "a return on another bus hears nothing");
  }

  // ------------------------------------------------------------ 16. patch host
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("patch", "gl.patcher");
    d.addNote(d.song().patterns[0].id, ch, {0, 192, 60, 100});
    RenderOptions o;
    o.tail = 96;
    AudioBuffer a = renderSong(d.song(), o);
    expect(a.peak() > 0.01f, "patch host instrument plays its inner synth");
    d.setParam(channelPluginParam(ch, 0), 0.0);  // macro 1 → filter cutoff down
    AudioBuffer b = renderSong(d.song(), o);
    expect(b.data != a.data, "macro changes an inner parameter");
    o.blockSize = 100;
    expect(renderSong(d.song(), o).data == b.data, "patch host output is block-size invariant");
    mod::PatchGraph g = mod::patchGraphOf(d.song().channel(ch)->plugin);
    expect(g.nodes.size() == 3 && g.nodes[0].plugin.type == "gl.synth" && !g.macros[0].empty(), "default patch graph");
    g.nodes.push_back({defaultPluginState("gl.patcher"), false, 1.0f, false});  // nesting is refused
    PluginState st = d.song().channel(ch)->plugin;
    st.data = mod::serializePatchGraph(g);
    mod::PatchGraph back = mod::patchGraphOf(st);
    expect(back.nodes.size() == 3 && back.macros[0].size() == g.macros[0].size(), "graph data round-trips; nested hosts dropped");
    // effect version, parallel branches
    Document f = baseDoc();
    ObjectId lead = f.addChannel("lead", "gl.synth");
    f.addNote(f.song().patterns[0].id, lead, {0, 192, 60, 100});
    ObjectId ins = f.song().mixer[0].id;
    f.setSlotPlugin(ins, 0, "gl.patcherfx");
    AudioBuffer fx = renderSong(f.song(), o);
    bool finite = true;
    for (float v : fx.data) finite = finite && std::isfinite(v);
    expect(fx.peak() > 0.01f && finite, "patch host effect processes audio");
    f.setParam(slotParam(ins, 0, mod::PhMix), 0.0);
    Document dry(f.song());
    dry.setSlotPlugin(ins, 0, "");
    expect(renderSong(f.song(), o).data == renderSong(dry.song(), o).data, "patch host effect at 0% mix is transparent");
  }

  // --------------------------------- 17. links + note FX in projects (save / load / migrate)
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("lead", "gl.synth");
    ObjectId ins = d.song().mixer[1].id;
    d.setSlotPlugin(ins, 2, "gl.peakctl");
    d.addNoteFx(ch, "gl.chord");
    d.addNoteFx(ch, "gl.arp");
    d.setParam(noteFxParam(ch, 1, mod::ArRate), 3);
    ModLink l = link(ins, 2, 1, channelPluginParam(ch, pp::SynCutoff));
    l.curve = ModCurve::Formula;
    l.formula = "input * 0.5 + base * 0.5";
    l.smoothMs = 25;
    l.min = 0.1f;
    l.max = 0.9f;
    d.setModLink(l);
    d.setModLink(link(ins, 2, 0, noteFxParam(ch, 1, mod::ArGate)));
    d.setSend(ins, d.song().mixer[2].id, 1.0f);
    d.setSendSidechain(ins, d.song().mixer[2].id, true);
    Song back;
    std::string err;
    expect(songFromJson(songToJson(d.song()), back, &err) && back == d.song(), "links, note FX and sidechain sends survive save/load");
    expect(back.modLinks.size() == 2 && back.modLinks[0].formula == l.formula && back.channel(ch)->noteFx.size() == 2,
           "loaded link keeps its formula; channel keeps its note FX chain");
    Json j = songToJson(d.song());
    expect(static_cast<int>(j.num("version")) == kProjectSchemaVersion && kProjectSchemaVersion == 6, "projects save as schema v6");
    // v4 project (no modLinks / noteFx / sidechain) still loads
    Json old = songToJson(baseDoc().song());
    old.object["version"] = Json::makeNumber(4);
    old.object.erase("modLinks");
    Song o4;
    expect(songFromJson(old, o4, &err), "schema v4 projects load");
    // links to missing targets are dropped on load
    Json broken = songToJson(d.song());
    broken.object["modLinks"].array[0].object["owner"] = Json::makeNumber(999999);
    Song b2;
    expect(songFromJson(broken, b2, &err) && b2.modLinks.size() == 1, "links to missing targets are dropped on load");
    // note FX moves keep links attached; removal drops them
    d.moveNoteFx(ch, 1, 0);
    expect(findModLink(d.song(), ch, noteFxParam(ch, 0, mod::ArGate).index) != nullptr, "links follow a moved note processor");
    d.removeNoteFx(ch, 0);
    expect(d.song().modLinks.size() == 1, "removing the note processor drops its links");
    d.undo();
    expect(d.song().modLinks.size() == 2 && d.song().channel(ch)->noteFx.size() == 2, "undo restores processor and link");
    expect(!d.addNoteFx(ch, "gl.synth"), "only note processors go into the note FX chain");
    expect(paramDisplayName(d.song(), noteFxParam(ch, 0, mod::ArGate)).find("Arpeggiator") != std::string::npos,
           "note FX parameters have readable names");
  }

  // ---------------------------- 18. links render identically for any block size
  {
    Document d = baseDoc(2);
    ObjectId pat = d.song().patterns[0].id;
    ObjectId kick = d.addChannel("kick", "gl.kick");
    ObjectId lead = d.addChannel("lead", "gl.synth");
    ObjectId ik = d.song().mixer[1].id;
    d.routeChannel(kick, ik);
    for (int s = 0; s < 32; s += 4) d.setStep(pat, kick, s, 120);
    d.addNote(pat, lead, {0, 700, 55, 100});
    d.setSlotPlugin(ik, 0, "gl.peakctl");
    ModLink l = link(ik, 0, 2, channelPluginParam(lead, pp::SynCutoff));
    l.smoothMs = 10;
    d.setModLink(l);
    d.setModLink(link(ik, 0, 1, channelParam(lead, ChannelParam::Pan)));
    RenderOptions o;
    o.blockSize = 512;
    AudioBuffer ref = renderSong(d.song(), o);
    bool same = true;
    for (int bs : {1, 63, 64, 100, 1024}) {
      o.blockSize = bs;
      same = same && renderSong(d.song(), o).data == ref.data;
    }
    expect(same, "song with controller links renders identically for block sizes 1, 63, 64, 100, 512, 1024");
  }

  // --------------------------------------------- 19. every pack plugin is sane
  {
    int n = 0;
    bool ok = true;
    for (const auto& info : pluginCatalog()) {
      bool mine = info.type == "gl.peakctl" || info.type == "gl.xyctl" || info.type == "gl.xyzctl" || info.type == "gl.formulactl" ||
                  info.type == "gl.envctl" || info.type == "gl.keyctl" || info.type == "gl.cvctl" || info.type == "gl.surface" ||
                  info.kind == PluginKind::NoteFx || info.type == "gl.patcher" || info.type == "gl.patcherfx" ||
                  info.type == "gl.send" || info.type == "gl.bussend" || info.type == "gl.busreturn" || info.type == "gl.scduck";
      if (!mine) continue;
      ++n;
      bool good = info.presets.size() >= 2 && info.presets.size() <= 6 && info.editorWidth > 0;
      for (size_t p = 0; p < info.presets.size(); ++p) {
        PluginState ps = presetState(info, p);
        good = good && ps.params.size() == info.params.size();
      }
      PluginState shortSt;
      shortSt.type = info.type;
      shortSt.params = {0.5f};
      normalizePluginState(shortSt);
      good = good && shortSt.params.size() == info.params.size();
      if (info.kind == PluginKind::NoteFx) good = good && createNoteFx(info.type) != nullptr;
      else if (info.kind == PluginKind::Generator) good = good && createGenerator(info.type) != nullptr;
      else good = good && createEffect(info.type) != nullptr;
      if (!good) std::printf("# plugin %s failed sanity\n", info.type.c_str());
      ok = ok && good;
    }
    expect(n == 22 && ok, "22 pack plugins: runtime, 2-6 presets, editor width, normalize short states");
    // extremes: every effect / controller with all params at min and at max stays finite
    bool finite = true;
    for (const char* t : {"gl.peakctl", "gl.xyctl", "gl.xyzctl", "gl.formulactl", "gl.surface", "gl.send", "gl.bussend",
                          "gl.busreturn", "gl.scduck", "gl.patcherfx"}) {
      for (int side = 0; side < 2; ++side) {
        Document d = baseDoc();
        ObjectId c = d.addChannel("lead", "gl.synth");
        d.addNote(d.song().patterns[0].id, c, {0, 192, 60, 100});
        ObjectId ins = d.song().mixer[0].id;
        d.setSlotPlugin(ins, 0, t);
        const PluginInfo* info = findPlugin(t);
        for (size_t k = 0; k < info->params.size(); ++k)
          d.setParam(slotParam(ins, 0, static_cast<int>(k)), side ? info->params[k].max : info->params[k].min);
        RenderOptions o;
        AudioBuffer b = renderSong(d.song(), o);
        for (float v : b.data) finite = finite && std::isfinite(v);
      }
    }
    for (const char* t : {"gl.envctl", "gl.keyctl", "gl.cvctl", "gl.patcher"}) {
      for (int side = 0; side < 2; ++side) {
        Document d = baseDoc();
        ObjectId c = d.addChannel("x", t);
        d.addNote(d.song().patterns[0].id, c, {0, 192, 60, 100});
        const PluginInfo* info = findPlugin(t);
        for (size_t k = 0; k < info->params.size(); ++k)
          d.setParam(channelPluginParam(c, static_cast<int>(k)), side ? info->params[k].max : info->params[k].min);
        AudioBuffer b = renderSong(d.song(), RenderOptions{});
        for (float v : b.data) finite = finite && std::isfinite(v);
      }
    }
    for (const auto& info : pluginCatalog()) {
      if (info.kind != PluginKind::NoteFx) continue;
      for (int side = 0; side < 2; ++side) {
        Document d = baseDoc();
        ObjectId c = d.addChannel("x", "gl.pluck");
        d.addNote(d.song().patterns[0].id, c, {0, 192, 60, 100});
        d.addNote(d.song().patterns[0].id, c, {48, 96, 67, 80});
        d.addNoteFx(c, info.type);
        for (size_t k = 0; k < info.params.size(); ++k)
          d.setParam(noteFxParam(c, 0, static_cast<int>(k)), side ? info.params[k].max : info.params[k].min);
        RenderOptions o;
        o.tail = 192;
        AudioBuffer b = renderSong(d.song(), o);
        for (float v : b.data) finite = finite && std::isfinite(v);
      }
    }
    expect(finite, "every pack plugin stays finite with all parameters at their extremes");
  }

  // ------------------------------------------- 20. note FX with live input & panic
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("arp", "gl.pluck");
    d.addNoteFx(ch, "gl.arp");
    Rig rig(d.song(), false);
    rig.eng.noteOn(ch, 60, 0.9f);
    rig.run(22050, 256);
    double playing = rig.rms(5000, 22050);
    rig.eng.noteOff(ch, 60);
    rig.run(88200, 256);
    double after = rig.rms(22050 + 66150, 22050 + 88200);
    expect(playing > 0.001 && after < playing * 0.05, "live notes run through the arpeggiator while stopped and stop on release");
    d.removeNoteFx(ch, 0);
    rig.update(d.song());
    rig.eng.noteOn(ch, 60, 0.9f);
    rig.run(4096, 256);
    rig.eng.panic();
    rig.run(44100, 256);
    expect(rig.rms(rig.L.size() - 4410, rig.L.size()) < 1e-4, "panic silences a channel whose chain was removed");
  }

  // ------------------------- 21. pattern loop with an arpeggiator repeats exactly
  {
    Document d = baseDoc();
    ObjectId pat = d.song().patterns[0].id;
    // MIDI Out instrument: its outgoing message ring shows exactly which notes the arpeggiator made.
    ObjectId ch = d.addChannel("arp", "gl.midiout");
    d.addNote(pat, ch, {0, 384, 60, 100});
    d.addNote(pat, ch, {0, 384, 67, 100});
    d.addNoteFx(ch, "gl.arp");
    Rig rig(d.song(), true, PlayMode::Pattern);
    const int bar = 88200;  // one 4/4 bar at 120 BPM
    std::vector<std::vector<int>> passes;
    for (int pass = 0; pass < 3; ++pass) {
      rig.run(bar, 333);
      std::vector<int> evs;
      MidiOutRing::Msg msg;
      auto rt = rig.builder.channelRuntime(ch);
      while (rt && rt->midiOut.pop(msg))
        if ((msg.s & 0xF0) == 0x90 || (msg.s & 0xF0) == 0x80) evs.push_back(((msg.s & 0xF0) == 0x90 ? 1000 : 0) + msg.a);
      passes.push_back(evs);
    }
    int onsN = 0;
    for (int v : passes[1]) onsN += v >= 1000;
    expect(onsN == 16 && passes[1].size() == 32, "looping pattern: 16 arpeggiated notes per bar, all released");
    expect(passes[1] == passes[2], "looping pattern: every pass through the arpeggiator plays the same notes");
  }

  // ------------------------- 22. live MIDI channel = note colour for the colour router
  {
    Document d = baseDoc();
    ObjectId a = d.addChannel("keys", "gl.synth");
    ObjectId b = d.addChannel("pad", "gl.synth");
    ObjectId ia = d.song().mixer[1].id, ib = d.song().mixer[2].id;
    d.routeChannel(a, ia);
    d.routeChannel(b, ib);
    d.addNoteFx(a, "gl.colormap");
    PluginState st = d.song().channel(a)->noteFx[0];
    st.params[static_cast<size_t>(mod::CmMap + 1)] = 1;  // colour 2 (MIDI channel 2) → target 1
    st.data = makeNoteFxData({b}, "");
    d.setNoteFxState(a, 0, st, "route");
    Rig rig(d.song(), false);
    rig.eng.setMidiTarget(a);
    rig.eng.midiEvent(0x91, 64, 100);  // note on, MIDI channel 2
    rig.run(4096);
    float pa = rig.builder.mixerRuntime(ia)->peakL.load(), pb = rig.builder.mixerRuntime(ib)->peakL.load();
    rig.eng.midiEvent(0x81, 64, 0);
    rig.eng.midiEvent(0x90, 60, 100);  // MIDI channel 1 stays here
    rig.run(4096);
    expect(pb > 0.01f && pa == 0.0f, "MIDI channel 2 notes are routed to the target channel");
    expect(rig.builder.mixerRuntime(ia)->peakL.load() > 0.01f, "MIDI channel 1 notes stay on this channel");
  }

  // ------------------------------------------------------- 23. X-Y-Z pad outputs
  {
    Document d = baseDoc();
    ObjectId ins = d.song().mixer[1].id;
    d.setSlotPlugin(ins, 0, "gl.xyzctl");
    d.setParam(slotParam(ins, 0, mod::XzX), 0.2);
    d.setParam(slotParam(ins, 0, mod::XzY), 0.4);
    d.setParam(slotParam(ins, 0, mod::XzZ), 1.0);
    d.setParam(slotParam(ins, 0, mod::XzZMin), 0.5);
    Rig rig(d.song());
    rig.run(44100);
    expect(near(rig.builder.controllerOutput(ins, 0, 0), 0.2, 1e-3) && near(rig.builder.controllerOutput(ins, 0, 1), 0.4, 1e-3) &&
               near(rig.builder.controllerOutput(ins, 0, 2), 1.0, 1e-3),
           "X-Y-Z pad: three outputs follow the pad (smoothed)");
    d.setParam(slotParam(ins, 0, mod::XzZ), 0.0);
    rig.update(d.song());
    rig.run(44100);
    expect(near(rig.builder.controllerOutput(ins, 0, 2), 0.5, 1e-3), "X-Y-Z pad: output range (Z min 50%)");
  }

  // ---------------------------------------- 24. loading sanitises note FX and links
  {
    Document d = baseDoc();
    ObjectId ch = d.addChannel("lead", "gl.synth");
    d.addNoteFx(ch, "gl.arp");
    Json j = songToJson(d.song());
    Json& chans = j.object["channels"];
    Json bogus = Json::makeObject();
    bogus.object["type"] = Json::makeString("gl.reverb");  // an effect is not a note processor
    chans.array[0].object["noteFx"].array.push_back(bogus);
    Song s;
    std::string err;
    expect(songFromJson(j, s, &err) && s.channels[0].noteFx.size() == 1, "non-note-processor entries are dropped on load");
    PluginState shortArp;
    shortArp.type = "gl.arp";
    shortArp.params = {2};
    normalizePluginState(shortArp);
    expect(shortArp.params.size() == static_cast<size_t>(mod::ArCount) && shortArp.params[0] == 2.0f,
           "short note-processor states are padded with defaults");
  }

  return finish();
}
