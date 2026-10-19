// Gridline — mixer parity (J-12): routing operations, cycle detection with
// explanations, pre/post-fader send maths, mono / swap / polarity, disabled
// tracks, manual track latency, threaded-processing switch, RMS meters, stem
// renders, effect moves between tracks (with undo), track state files,
// groups, docking and project round trips of the new fields.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/MixerOps.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Effects.h"
#include "../src/engine/Render.h"
#include "Check.h"

using namespace gl;
namespace mo = gl::mixops;

namespace {

// Silences the right channel (makes a stereo signal out of a mono one).
class TestLeftOnly final : public Effect {
 public:
  void process(const PluginState&, const ProcessContext&, float*, float* R, int n) override {
    for (int i = 0; i < n; ++i) R[i] = 0.f;
  }
  void reset() override {}
};
class TestInvert final : public Effect {
 public:
  void process(const PluginState&, const ProcessContext&, float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i) L[i] = -L[i], R[i] = -R[i];
  }
  void reset() override {}
};
class TestGain final : public Effect {  // params[0] = gain
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const float g = s.params.empty() ? 1.f : s.params[0];
    for (int i = 0; i < n; ++i) L[i] *= g, R[i] *= g;
  }
  void reset() override {}
};

std::unique_ptr<Effect> testFactory(const std::string& type) {
  if (type == "test.leftonly") return std::make_unique<TestLeftOnly>();
  if (type == "test.invert") return std::make_unique<TestInvert>();
  if (type == "test.gain") return std::make_unique<TestGain>();
  return nullptr;
}

PluginState fx(const std::string& type, std::vector<float> params = {}) {
  PluginState s;
  s.type = type;
  s.params = std::move(params);
  return s;
}

// Two identical kick channels on inserts 1 and 2 (a kick on every step).
Song twinSong() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const Song& s = d.song();
  ObjectId pat = s.patterns[0].id;
  ObjectId a = d.addChannel("a", "gl.kick");
  ObjectId b = d.addChannel("b", "gl.kick");
  d.routeChannel(a, s.mixer[1].id);
  d.routeChannel(b, s.mixer[2].id);
  for (int st = 0; st < 16; ++st) {
    d.setStep(pat, a, st, 120);
    d.setStep(pat, b, st, 120);
  }
  d.addClip(pat, 0, 0);
  Song out = d.song();
  for (auto& c : out.channels)
    if (c.plugin.type == "gl.kick" && c.plugin.params.size() > pp::KickClick) c.plugin.params[pp::KickClick] = 0.f;
  return out;
}

struct Runner {
  ProgramBuilder builder;
  AudioEngine engine;
  Runner(const Song& s, int threads = 1) : engine(44100) {
    builder.setSampleRate(44100);
    engine.setThreads(threads);
    engine.setProgram(builder.build(s));
    engine.setMode(PlayMode::Pattern);
    engine.setPattern(s.patterns[0].id);
    engine.play();
  }
  void run(std::vector<float>& outL, std::vector<float>& outR, int frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    for (int done = 0; done < frames; done += block) {
      int n = std::min(block, frames - done);
      engine.process(L.data(), R.data(), n);
      outL.insert(outL.end(), L.begin(), L.begin() + n);
      outR.insert(outR.end(), R.begin(), R.begin() + n);
    }
  }
};

float peak(const std::vector<float>& x) {
  float p = 0;
  for (float v : x) p = std::max(p, std::fabs(v));
  return p;
}
float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  float p = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) p = std::max(p, std::fabs(a[i] - b[i]));
  return p;
}
bool sameBuffers(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.channels != b.channels || a.frames != b.frames) return false;
  for (size_t c = 0; c < a.channels; ++c)
    if (std::memcmp(a.plane(c), b.plane(c), sizeof(float) * a.frames) != 0) return false;
  return true;
}

void runPair(const Song& s, std::vector<float>& L, std::vector<float>& R, int threads = 1) {
  Runner r(s, threads);
  r.run(L, R, 22050);
}

}  // namespace

int main() {
  setTestEffectFactory(&testFactory);

  // ---- routing operations ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId m = s.masterId(), i1 = s.mixer[1].id, i2 = s.mixer[2].id, i3 = s.mixer[3].id, i4 = s.mixer[4].id;
    std::string err;
    expect(mo::route(d, {i1}, {i2}, mo::RouteMode::Send, &err) == 1, "route: add a send");
    expect(s.mixerTrack(i1)->sends.size() == 2, "route: send added beside the master route");
    expect(mo::route(d, {i1}, {i3}, mo::RouteMode::Only) == 1 && s.mixerTrack(i1)->sends.size() == 1 &&
               s.mixerTrack(i1)->sends[0].to == i3,
           "route only: the destination becomes the only route");
    expect(mo::route(d, {i2, i4}, {i3}, mo::RouteMode::Sidechain) == 2, "sidechain selected tracks to this track");
    const MixerTrack* t2 = s.mixerTrack(i2);
    expect(t2->sends.size() == 2 && t2->sends[1].sidechain && !t2->sends[0].sidechain, "sidechain route added, audio route kept");
    expect(mo::route(d, {i2}, {i4}, mo::RouteMode::SidechainOnly) == 1, "sidechain only");
    t2 = s.mixerTrack(i2);
    bool hasI3 = false, hasI4 = false, hasM = false;
    for (const auto& sd : t2->sends) hasI3 |= sd.to == i3, hasI4 |= sd.to == i4 && sd.sidechain, hasM |= sd.to == m;
    expect(!hasI3 && hasI4 && hasM, "sidechain only: other sidechain routes dropped, audio routes kept");
    expect(mo::routedTo(s, i3) == std::vector<ObjectId>{i1, i4}, "routedTo lists the senders in mixer order");
    expect(mo::routedFrom(s, i2).size() == 2, "routedFrom lists the destinations");
    // route this track to several selected tracks "only"
    const ObjectId i5 = s.mixer[5].id;
    expect(mo::route(d, {i4}, {i1, i5}, mo::RouteMode::Only) == 2 && s.mixerTrack(i4)->sends.size() == 2,
           "route only to several selected tracks keeps all of them");
    expect(mo::unrouteIncoming(d, i3) == 1 && mo::routedTo(s, i3).empty(), "unroute tracks routed to the track");
    expect(s.mixerTrack(i1)->sends.size() == 1 && s.mixerTrack(i1)->sends[0].to == m, "a source left unrouted falls back to the master");
    expect(mo::resetRouting(d, {i2, i4}) == 2 && s.mixerTrack(i2)->sends.size() == 1 && s.mixerTrack(i2)->sends[0].to == m,
           "reset routing: master only");
    // submix
    const size_t before = s.mixer.size();
    ObjectId sub = mo::createSubmix(d, {i1, i2}, "Drums bus");
    expect(sub != kNoId && s.mixer.size() == before + 1 && s.mixer[3].id == sub, "submix inserted after the last source");
    expect(mixerTrackName(s, 3) == "Drums bus" && s.mixerTrack(sub)->sends[0].to == m, "submix named, routed to the master");
    expect(s.mixerTrack(i1)->sends.size() == 1 && s.mixerTrack(i1)->sends[0].to == sub && s.mixerTrack(i2)->sends[0].to == sub,
           "sources route only to the submix");
    d.undo();
    expect(s.mixer.size() == before && s.mixerTrack(i1)->sends[0].to == m, "submix is one undo step");
    // routing overview
    mo::route(d, {i1}, {i2}, mo::RouteMode::Only);
    mo::route(d, {i2}, {i3}, mo::RouteMode::Only);
    mo::RouteLayout L = mo::routeLayout(s);
    expect(L.column[1] == 0 && L.column[2] == 1 && L.column[3] == 2 && L.column[0] == 3 && L.columns == 4,
           "overview: columns follow the longest route, master rightmost");
    size_t edges = 0;
    for (size_t i = 0; i < s.mixer.size(); ++i) edges += s.mixer[i].sends.size();
    expect(L.edges.size() == edges, "overview: every route is an edge");
  }

  // ---- cycle detection ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId m = s.masterId(), i1 = s.mixer[1].id, i2 = s.mixer[2].id, i3 = s.mixer[3].id;
    mo::route(d, {i1}, {i2}, mo::RouteMode::Only);
    mo::route(d, {i2}, {i3}, mo::RouteMode::Only);
    std::vector<ObjectId> loop = mo::cyclePath(s, i3, i1);
    expect(loop == std::vector<ObjectId>{i1, i2, i3}, "cycle path: the existing chain back to the source");
    std::string why = mo::routeProblem(s, i3, i1);
    expect(why.find("feedback loop") != std::string::npos && why.find("Insert 1") != std::string::npos &&
               why.find("Insert 2") != std::string::npos,
           "cycle message names the loop");
    std::string err;
    expect(mo::route(d, {i3}, {i1}, mo::RouteMode::Send, &err) == 0 && err == why, "cyclic route refused with the message");
    expect(!mo::routeProblem(s, i1, i1).empty() && !mo::routeProblem(s, m, i1).empty(), "self and master routes refused");
    expect(mo::routeProblem(s, i1, i3).empty() && mo::cyclePath(s, i1, i3).empty(), "forward route allowed");
    expect(mo::route(d, {i3}, {i2}, mo::RouteMode::Sidechain, &err) == 0, "sidechain routes are cycle-checked too");
  }

  // ---- pre / post-fader send maths ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId i1 = s.mixer[1].id, i2 = s.mixer[2].id;
    d.setParam(mixerParam(i1, MixerParam::Volume), 0.4, false);  // fader -12 dB (gain 0.25)
    mo::route(d, {i1}, {i2}, mo::RouteMode::Send);
    d.setSend(i1, i2, 0.5f);
    expect(std::fabs(mo::sendGain(s, i1, i2) - 0.125) < 1e-6, "post-fader send: level x fader gain");
    expect(mo::setSendPreFader(d, i1, i2, true) && std::fabs(mo::sendGain(s, i1, i2) - 0.5) < 1e-6,
           "pre-fader send: level only");
    expect(!mo::setSendPreFader(d, i1, i2, true), "pre-fader: no-op when unchanged");
    d.undo();
    expect(!s.mixerTrack(i1)->sends[1].preFader, "pre-fader switch undoes");
    // Engine: insert 1 (fader at -12 dB) pre-fader to the master at full level
    // plus an inverted unity copy on insert 2 cancel exactly; post-fader doesn't.
    Song t = twinSong();
    const ObjectId m = t.masterId();
    t.mixer[1].volume = 0.4f;
    t.mixer[1].sends = {{m, 1.0f, false, true}};
    t.mixer[2].invertPolarity = true;
    std::vector<float> L, R, pL, pR;
    runPair(t, L, R);
    expect(peak(L) == 0.f && peak(R) == 0.f, "pre-fader send bypasses the fader (exact cancellation)");
    t.mixer[1].sends[0].preFader = false;
    runPair(t, pL, pR);
    expect(peak(pL) > 0.05f, "post-fader send follows the fader");
    // pre-fader keeps effects and EQ: an inverting effect on insert 1 flips the copy
    t.mixer[1].sends[0].preFader = true;
    t.mixer[1].slots[0].plugin = fx("test.invert");
    t.mixer[2].invertPolarity = false;
    std::vector<float> eL, eR;
    runPair(t, eL, eR);
    expect(peak(eL) == 0.f, "pre-fader tap is after the effects");
    // sidechain-only route doesn't reach the master
    Song sc = twinSong();
    sc.mixer[1].sends = {{sc.mixer[3].id, 1.0f, true, false}};
    sc.mixer[2].muted = true;
    std::vector<float> sL, sR;
    runPair(sc, sL, sR);
    expect(peak(sL) == 0.f, "sidechain send: no audio to the master");
  }

  // ---- mono / swap / polarity ----
  {
    Document d(twinSong());
    const Song& s = d.song();
    const ObjectId i1 = s.mixer[1].id, i2 = s.mixer[2].id;
    d.editMixerTrack(i1, "fx", [](MixerTrack& m) { m.slots[0].plugin = fx("test.leftonly"); });
    d.editMixerTrack(i2, "mute", [](MixerTrack& m) { m.muted = true; });
    std::vector<float> L0, R0;
    runPair(s, L0, R0);
    expect(peak(L0) > 0.05f && peak(R0) == 0.f, "fixture: left-only signal");
    expect(mo::setMono(d, {i1}, true) && mo::isMono(*s.mixerTrack(i1)), "make mono");
    std::vector<float> L1, R1;
    runPair(s, L1, R1);
    expect(maxDiff(L1, R1) == 0.f && peak(L1) > 0.02f, "mono: both sides identical");
    expect(std::fabs(peak(L1) - 0.5f * peak(L0)) < 1e-3f, "mono: left-only signal merged at half level");
    mo::setMono(d, {i1}, false);
    expect(s.mixerTrack(i1)->stereoSep == 0.f, "make stereo: separation back to centre");
    expect(mo::setSwap(d, {i1}, true), "swap left/right");
    std::vector<float> L2, R2;
    runPair(s, L2, R2);
    expect(peak(L2) == 0.f && maxDiff(R2, L0) == 0.f, "swap: left signal moves to the right");
    mo::setSwap(d, {i1}, false);
    d.editMixerTrack(i1, "fx", [](MixerTrack& m) { m.slots[0] = EffectSlot{}; });
    d.editMixerTrack(i2, "unmute", [](MixerTrack& m) { m.muted = false; });
    expect(mo::setPolarity(d, {i2}, true) && s.mixerTrack(i2)->invertPolarity, "reverse polarity");
    std::vector<float> L3, R3;
    runPair(s, L3, R3);
    expect(peak(L3) == 0.f, "reversed polarity cancels its twin exactly");
  }

  // ---- disabled tracks ----
  {
    Song t = twinSong();
    t.mixer[1].slots[0].plugin = fx("test.gain", {0.1f});
    t.mixer[1].slots[1].plugin = fx("test.invert");
    t.mixer[1].volume = 0.3f;
    t.mixer[1].eqGain = {6.f, 0.f, 0.f};
    t.mixer[1].disabled = true;
    t.mixer[2].invertPolarity = true;  // unity, inverted
    std::vector<float> L, R;
    runPair(t, L, R);
    expect(peak(L) == 0.f && peak(R) == 0.f, "disabled track passes its input untouched (effects, EQ, fader bypassed)");
    ProgramBuilder b;
    b.setSampleRate(44100);
    auto p = b.build(t);
    expect(!p->mixer[1].slots[0].enabled && p->mixer[1].latOwn == 0, "disabled track runs no effects and adds no latency");
    t.mixer[1].disabled = false;
    std::vector<float> L2, R2;
    runPair(t, L2, R2);
    expect(peak(L2) > 0.05f, "enabled again: effects and fader apply");
    Document d(twinSong());
    expect(mo::setDisabled(d, {d.song().mixer[1].id}, true) && d.song().mixer[1].disabled, "disable op");
    d.undo();
    expect(!d.song().mixer[1].disabled, "disable undoes");
  }

  // ---- manual latency, threaded processing, RMS meter ----
  {
    Song t = twinSong();
    t.mixer[1].latencyOffset = 300;
    ProgramBuilder b;
    b.setSampleRate(44100);
    auto p = b.build(t);
    expect(p->mixer[1].latOwn == 300 && p->outputLatency == 300, "manual latency reported like an effect's");
    bool tap = false;
    for (const auto& in : p->mixer[static_cast<size_t>(p->master)].inputs)
      if (in.kind == ProgInput::Send && in.src == 2) tap = in.tap.delay == 300;
    expect(tap, "manual latency: other paths delayed by it");
    expect(mo::latencyFromMs(10, 48000) == 480 && mo::latencyFromBeats(1, 120, 48000) == 24000, "latency in ms / beats");
    Document d(t);
    expect(mo::setLatencyOffset(d, d.song().mixer[2].id, 50) && mo::resetManualLatency(d) == 2 &&
               d.song().mixer[1].latencyOffset == 0,
           "reset manual latency on all tracks");
    Song th = twinSong();
    th.mixer[1].threaded = false;
    th.mixer[2].threaded = false;
    auto p2 = b.build(th);
    expect(p2->graph.mainOnly.size() == static_cast<size_t>(p2->graph.tasks) &&
               p2->graph.mainOnly[static_cast<size_t>(p2->graph.channels) + 1] == 1 &&
               p2->graph.mainOnly[static_cast<size_t>(p2->graph.channels) + 3] == 0,
           "threaded off: the track's task is pinned to the audio thread");
    std::vector<float> L1, R1, L4, R4;
    runPair(th, L1, R1, 1);
    Runner r4(th, 4);
    r4.run(L4, R4, 22050);
    expect(maxDiff(L1, L4) == 0.f && peak(L1) > 0.05f, "pinned tracks: 4 threads bit-identical to 1");
#if defined(__APPLE__)
    // Workers run with real-time scheduling, so a busy machine can't preempt
    // one mid-block (that stalled the audio thread for whole 10 ms quanta and
    // showed as 200-300% audio load while nothing was playing).
    expect(r4.engine.workerThreads() == 3 && r4.engine.realtimeWorkers() == 3, "worker threads run with real-time scheduling");
#endif
    auto rt = r4.builder.mixerRuntime(th.mixer[1].id);
    expect(rt && rt->rmsL.load() > 0.f && rt->rmsL.load() <= rt->peakL.load() + 1e-6f, "RMS meter fills (<= peak)");
  }

  // ---- stems ----
  {
    Song t = twinSong();
    t.mixer[0].slots[0].plugin = fx("test.gain", {0.5f});  // master effect: not in the stems
    t.mixer[2].volume = 0.5f;
    Song st = mo::stemSong(t, t.mixer[2].id);
    expect(st.mixer[1].muted && !st.mixer[2].muted && st.mixer[0].slots[0].plugin.empty(), "stem song: others muted, master neutral");
    Song ref = t;
    ref.mixer[1].muted = true;
    ref.mixer[0].slots[0] = EffectSlot{};
    RenderOptions o;
    o.sampleRate = 44100;
    o.tail = 0;
    AudioBuffer a = renderSong(st, o), b = renderSong(ref, o);
    expect(a.frames > 0 && sameBuffers(a, b), "stem render = the track alone through a neutral master");
    // A track feeding the stem's track stays audible.
    Song chain = twinSong();
    chain.mixer[1].sends = {{chain.mixer[2].id, 1.0f, false, false}};
    Song cs = mo::stemSong(chain, chain.mixer[2].id);
    expect(!cs.mixer[1].muted && cs.mixer[3].muted, "stem keeps the tracks routed into it");
    std::vector<mo::StemJob> jobs = mo::stemJobs(t, {t.mixer[2].id, t.mixer[0].id, t.mixer[1].id}, "/tmp/x", "Song");
    expect(jobs.size() == 3 && jobs[0].track == t.mixer[0].id && jobs[0].file == "/tmp/x/Song_Master.wav" &&
               jobs[1].file == "/tmp/x/Song_01_Insert 1.wav" && jobs[2].file == "/tmp/x/Song_02_Insert 2.wav",
           "stem list: mixer order, numbered file names");
    t.mixer[1].name = "Kick/Sub?";
    t.mixer[2].name = "Kick/Sub?";
    jobs = mo::stemJobs(t, {t.mixer[1].id, t.mixer[2].id}, "", "");
    expect(jobs[0].file == "01_Kick_Sub_.wav" && jobs[1].file == "02_Kick_Sub_.wav", "stem names are filesystem-safe");
  }

  // ---- effect slots: reorder, move between tracks, undo ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId i1 = s.mixer[1].id, i2 = s.mixer[2].id;
    d.setSlotPlugin(i1, 0, "gl.delay");
    d.setSlotPlugin(i1, 1, "gl.reverb");
    d.setSlotPlugin(i2, 0, "gl.eq");
    d.setParam(slotMixParam(i1, 1), 0.4, false);
    const Song before = s;
    expect(mo::moveSlotTo(d, i1, 1, i1, 0), "reorder within a track");
    expect(s.mixerTrack(i1)->slots[0].plugin.type == "gl.reverb" && s.mixerTrack(i1)->slots[0].mix == 0.4f, "reorder keeps mix");
    d.undo();
    expect(s == before, "reorder undoes");
    ModLink ml;
    ml.sourceOwner = i2;
    ml.sourceSlot = 0;
    ml.owner = i1;
    ml.index = slotParam(i1, 1, 0).index;
    Song withLink = s;
    withLink.modLinks.push_back(ml);
    d.reset(withLink);
    expect(mo::moveSlotTo(d, i1, 1, i2, 0), "move to another track's taken slot (shifts down)");
    const MixerTrack* a = s.mixerTrack(i1);
    const MixerTrack* b = s.mixerTrack(i2);
    expect(a->slots[1].plugin.empty() && b->slots[0].plugin.type == "gl.reverb" && b->slots[1].plugin.type == "gl.eq",
           "moved between tracks; target slots shifted");
    expect(s.modLinks.size() == 1 && s.modLinks[0].owner == i2 && s.modLinks[0].index == slotParam(i2, 0, 0).index &&
               s.modLinks[0].sourceSlot == 1,
           "links follow the moved effect and the shifted source");
    d.undo();
    expect(s == withLink, "move between tracks undoes in one step");
    expect(mo::moveSlotTo(d, i1, 0, i2, 5, true) && s.mixerTrack(i1)->slots[0].plugin.type == "gl.delay" &&
               s.mixerTrack(i2)->slots[5].plugin.type == "gl.delay",
           "copy to another track");
    expect(!mo::moveSlotTo(d, i1, 4, i2, 0), "empty source slot refused");
  }

  // ---- track state, groups, docking, icons, insert, project round trip ----
  {
    Document d(makeEmptySong());
    const Song& s = d.song();
    const ObjectId i1 = s.mixer[1].id, i2 = s.mixer[2].id, i3 = s.mixer[3].id;
    d.editMixerTrack(i1, "setup", [](MixerTrack& m) {
      m.name = "Lead";
      m.color = 0x33AAFF;
      m.icon = 4;
      m.volume = 0.6f;
      m.invertPolarity = true;
      m.latencyOffset = 64;
      m.threaded = false;
      m.eqGain = {3.f, 0.f, -2.f};
      m.slots[2].plugin = defaultPluginState("gl.delay");
      m.slots[2].mix = 0.3f;
    });
    mo::route(d, {i2}, {i3}, mo::RouteMode::Only);
    std::string text = mo::trackStateText(*s.mixerTrack(i1));
    expect(mo::applyTrackState(d, i2, text), "load track state onto another track");
    const MixerTrack* t2 = s.mixerTrack(i2);
    expect(t2->name == "Lead" && t2->icon == 4 && t2->volume == 0.6f && t2->invertPolarity && t2->latencyOffset == 64 &&
               !t2->threaded && t2->eqGain[2] == -2.f && t2->slots[2].plugin.type == "gl.delay" && t2->slots[2].mix == 0.3f,
           "track state carries levels, switches, effects, EQ, latency");
    expect(t2->id == i2 && t2->sends.size() == 1 && t2->sends[0].to == i3, "track state keeps identity and routing");
    std::string err;
    expect(!mo::applyTrackState(d, i2, "{\"format\":\"x\"}", &err) && !err.empty(), "foreign file refused");
    const std::string path = (std::filesystem::temp_directory_path() / "gl_j12_state.gltrack").string();
    expect(mo::saveTrackState(path, *s.mixerTrack(i1)) && mo::loadTrackState(d, i3, path) && s.mixerTrack(i3)->name == "Lead",
           "track state file round trip");
    std::filesystem::remove(path);
    // groups
    expect(mo::setGroup(d, {i1, i3}, "Vox") && mo::groupMembers(s, "Vox") == std::vector<ObjectId>{i1, i3}, "group tracks");
    expect(mo::newGroupName(s) == "Group 1" && mo::groupNames(s) == std::vector<std::string>{"Vox"}, "group names");
    // docking
    expect(mo::setDock(d, {i3}, MixerDock::Left) && mo::setDock(d, {i1}, MixerDock::Right), "dock tracks");
    std::vector<size_t> order = mo::displayOrder(s);
    expect(order.size() == s.mixer.size() && order[0] == 0 && order[1] == 3 && order[2] == 2 && order.back() == 1,
           "display order: master, left dock, middle, right dock");
    expect(!mo::setDock(d, {s.masterId()}, MixerDock::Right), "the master doesn't dock");
    // icons / colours
    expect(mo::setIcon(d, {i2}, 7) && s.mixerTrack(i2)->icon == 7, "set icon");
    mo::setColor(d, {i1}, 0x000000);
    mo::setColor(d, {i3}, 0xFEFEFE);
    expect(mo::gradientColor(d, {i1, i2, i3}) && s.mixerTrack(i2)->color == 0x7F7F7F, "gradient colour");
    // insert / grow
    const size_t n0 = s.mixer.size();
    auto ins = mo::insertTracks(d, 2, 3);
    expect(ins.size() == 3 && s.mixer.size() == n0 + 3 && s.mixer[2].id == ins[0] && s.mixer[4].id == ins[2],
           "insert multiple tracks at a position");
    expect(mo::growTo(d, static_cast<int>(s.mixer.size()) + 1) == 2 && mo::growTo(d, 3) == 0, "increase the number of tracks");
    // reset
    expect(mo::resetTracks(d, {i1}) == 1 && !s.mixerTrack(i1)->invertPolarity && s.mixerTrack(i1)->latencyOffset == 0 &&
               s.mixerTrack(i1)->name == "Lead" && !s.mixerTrack(i1)->slots[2].plugin.empty(),
           "reset tracks: switches back to default, name and effects kept");
    // project round trip
    d.editMixerTrack(i1, "x", [](MixerTrack& m) { m.disabled = true; });
    mo::setSendPreFader(d, i2, i3, true);
    Song back;
    std::string perr;
    expect(songFromJson(songToJson(s), back, &perr) && back == s, "project round trip keeps the new mixer fields");
  }

  return finish();
}
