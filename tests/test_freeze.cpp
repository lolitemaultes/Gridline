// Gridline: K-87, freezing and hibernating channels and mixer tracks.
//  * null tests: the song with a frozen channel / mixer track (its render playing in place of its
//    plugins) is the song played live, sample for sample: effects with tails past the song end,
//    plugin delay compensation, a bus fed by channels and a send, a sidechain from another track,
//    automation of a frozen parameter and of a live fader, a pre-fader send from a frozen track;
//  * the plugins really go: no instrument or effect runtime for frozen and covered units, fewer
//    sounding voices, less work per block (counted, never timed);
//  * unfreeze gives back the song exactly as it was, and the live sound;
//  * what makes a freeze out of date (and what does not), the reasons a unit cannot be frozen;
//  * save and load, older projects, a missing render, zipped projects;
//  * hibernate: unloaded, woken, the same sound once awake, left out of project-load preloading;
//  * undo and redo of a freeze.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/Json.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/ProjectInterop.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Freeze.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/engine/Sample.h"
#include "Check.h"
#include "collab/CollabSim.h"

namespace fs = std::filesystem;
using namespace gl;
using freeze::channelUnit;
using freeze::mixerUnit;

namespace {

constexpr int kSr = 44100;
std::string scratch;

ObjectId addChannel(Song& s, const std::string& name, const std::string& type, ObjectId mixer) {
  Channel c;
  c.id = s.allocateId();
  c.name = name;
  c.plugin = defaultPluginState(type);
  c.mixerTrack = mixer == kNoId ? s.masterId() : mixer;
  s.channels.push_back(c);
  return c.id;
}

ObjectId addInsert(Song& s, const std::string& name) {
  MixerTrack m;
  m.id = s.allocateId();
  m.name = name;
  m.sends = {{s.masterId(), 1.0f, false, false}};
  s.mixer.push_back(m);
  return m.id;
}

void setFx(Song& s, ObjectId track, int slot, const std::string& type) {
  s.mixerTrack(track)->slots[static_cast<size_t>(slot)].plugin = defaultPluginState(type);
}

void addNotes(Song& s, ObjectId pat, ObjectId ch, int first, int count, Tick every, Tick len) {
  for (int k = 0; k < count; ++k) s.pattern(pat)->notes[ch].push_back(Note(every * k, len, first + (k * 5) % 12, 100));
}

void addClip(Song& s, ObjectId pat, Tick start, int track) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = track;
  c.start = start;
  s.playlist.push_back(c);
}

// 120 BPM, 96 PPQ, two bars of a synth on insert "Bus" (delay, look-ahead compressor), a pluck
// also into the bus, a kick on its own insert keying the bus compressor, a lead straight to master.
struct Fixture {
  Song s;
  ObjectId bus = kNoId, kickTrack = kNoId, synth = kNoId, pluck = kNoId, kick = kNoId, lead = kNoId, pat = kNoId;
};

Fixture fixture() {
  Fixture f;
  f.s = makeEmptySong();
  Song& s = f.s;
  s.tempo = 120.0;
  s.ppq = 96;
  f.pat = s.patterns[0].id;
  s.pattern(f.pat)->bars = 2;
  f.bus = addInsert(s, "Bus");
  f.kickTrack = addInsert(s, "Kick track");
  setFx(s, f.bus, 0, "gl.delay");  // a tail past the song end
  setFx(s, f.bus, 1, "gl.comp");
  {
    PluginState& c = s.mixerTrack(f.bus)->slots[1].plugin;
    c.params[pp::CmpLookahead] = 5.0f;  // 5 ms of latency: delay compensation
    c.params[pp::CmpSidechain] = 1.0f;   // keyed from the kick track
    c.params[pp::CmpThreshold] = -24.0f;
    c.params[pp::CmpRatio] = 4.0f;
  }
  s.mixerTrack(f.kickTrack)->sends.push_back({f.bus, 1.0f, true, false});  // the key
  MixerTrack& bus = *s.mixerTrack(f.bus);
  bus.volume = 0.6f;  // live strip
  bus.pan = -0.3f;
  bus.eqGain = {3.0f, 0.0f, -2.0f};
  f.synth = addChannel(s, "Synth", "gl.synth", f.bus);
  f.pluck = addChannel(s, "Pluck", "gl.pluck", f.bus);
  f.kick = addChannel(s, "Kick", "gl.kick", f.kickTrack);
  f.lead = addChannel(s, "Lead", "gl.fm", kNoId);
  s.channel(f.synth)->gain = 0.55f;
  s.channel(f.synth)->pan = 0.4f;
  s.channel(f.lead)->gain = 0.6f;
  addNotes(s, f.pat, f.synth, 48, 8, 48, 40);
  addNotes(s, f.pat, f.pluck, 60, 6, 64, 20);
  addNotes(s, f.pat, f.kick, 36, 8, 96, 24);
  addNotes(s, f.pat, f.lead, 72, 4, 96, 80);
  addClip(s, f.pat, 0, 0);
  return f;
}

AudioBuffer renderPlain(const Song& s) {
  RenderOptions o;
  o.sampleRate = kSr;
  o.tail = 96 * 8;  // two bars of tail
  return renderSong(s, o);
}

// The song as the app plays it: up-to-date frozen units play their renders.
AudioBuffer renderAsPlayed(const Song& s, std::set<freeze::Unit>* live = nullptr) {
  RenderOptions o;
  o.sampleRate = kSr;
  o.tail = 96 * 8;
  Song v = freeze::exportView(s, o.frozen, live);
  return renderSong(v, o);
}

float maxDiff(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.frames != b.frames || a.channels != b.channels) return 1e9f;
  float d = 0;
  size_t at = 0;
  for (size_t i = 0; i < a.data.size(); ++i)
    if (std::fabs(a.data[i] - b.data[i]) > d) d = std::fabs(a.data[i] - b.data[i]), at = i;
  if (d > 1e-6f)
    std::printf("#   largest difference at frame %zu of %zu (%.4g vs %.4g)\n", at / static_cast<size_t>(a.channels), a.frames,
                static_cast<double>(a.data[at]), static_cast<double>(b.data[at]));
  return d;
}

float peak(const AudioBuffer& a) {
  float p = 0;
  for (float v : a.data) p = std::max(p, std::fabs(v));
  return p;
}

bool freezeUnit(Song& s, freeze::Unit u, std::string* why = nullptr) {
  FreezeRecord r;
  if (!freeze::render(s, u, kSr, (fs::path(scratch) / "Frozen").string(), r, why)) return false;
  return freeze::setRecord(s, u, r);
}

// ---------------------------------------------------------------- null tests

void channelFreezeNulls() {
  Fixture f = fixture();
  const AudioBuffer live = renderPlain(f.s);
  expect(peak(live) > 0.01f, "the fixture makes sound");
  Song s = f.s;
  std::string why;
  expect(freezeUnit(s, channelUnit(f.synth), &why), "a channel freezes");
  expect(fs::exists(s.channel(f.synth)->freeze.render.data), "its render is a file");
  expect(s.channel(f.synth)->plugin == f.s.channel(f.synth)->plugin, "the instrument's state is untouched");
  std::set<freeze::Unit> left;
  const AudioBuffer played = renderAsPlayed(s, &left);
  expect(left.empty(), "an up-to-date freeze plays its render");
  const float d = maxDiff(live, played);
  std::printf("# channel freeze null: max difference %.3g\n", static_cast<double>(d));
  expect(d < 1e-6f, "null test: a frozen channel sounds the same (instrument, notes, then its live strip and insert)");
  // The channel's volume and pan stay live.
  Song a = f.s, b = s;
  a.channel(f.synth)->gain = b.channel(f.synth)->gain = 0.3f;
  a.channel(f.synth)->pan = b.channel(f.synth)->pan = -0.7f;
  expect(maxDiff(renderPlain(a), renderAsPlayed(b)) < 1e-6f, "null test: the frozen channel's volume and pan stay live");
  expect(freeze::upToDate(b, channelUnit(f.synth)), "volume and pan leave the freeze up to date");
}

void mixerFreezeNulls() {
  Fixture f = fixture();
  const AudioBuffer live = renderPlain(f.s);
  Song s = f.s;
  std::string why;
  expect(freezeUnit(s, mixerUnit(f.bus), &why), "a mixer track freezes");
  if (!why.empty()) std::printf("# %s\n", why.c_str());
  const AudioBuffer played = renderAsPlayed(s);
  const float d = maxDiff(live, played);
  std::printf("# mixer freeze null: max difference %.3g\n", static_cast<double>(d));
  expect(d < 1e-6f, "null test: a frozen bus (two channels, delay tail, look-ahead compressor keyed by another track)");
  // Its strip stays live: fader, pan, EQ, a new send.
  Song a = f.s, b = s;
  for (Song* x : {&a, &b}) {
    const ObjectId rev = addInsert(*x, "Reverb");
    setFx(*x, rev, 0, "gl.reverb");
    MixerTrack& m = *x->mixerTrack(f.bus);
    m.volume = 0.9f;
    m.pan = 0.5f;
    m.eqGain = {-4.0f, 2.0f, 0.0f};
    m.sends.push_back({rev, 0.5f, false, true});  // pre-fader send from the frozen track
  }
  expect(freeze::upToDate(b, mixerUnit(f.bus)), "fader, pan, EQ and sends leave the freeze up to date");
  const float d2 = maxDiff(renderPlain(a), renderAsPlayed(b));
  std::printf("# mixer freeze live strip: max difference %.3g\n", static_cast<double>(d2));
  expect(d2 < 1e-6f, "null test: the frozen track's fader, pan, EQ and a pre-fader send stay live");
}

void automationNulls() {
  Fixture f = fixture();
  Song& s0 = f.s;
  auto addAuto = [&](ParamId target, float a, float b) {
    AutomationClip ac;
    ac.id = s0.allocateId();
    ac.owner = target.owner;
    ac.index = target.index;
    ac.points = {AutoPoint{0, a}, AutoPoint{96 * 8, b}};
    s0.automations.push_back(ac);
    Clip c;
    c.id = s0.allocateId();
    c.kind = ClipKind::Automation;
    c.source = ac.id;
    c.track = 1;
    c.start = 0;
    c.length = 96 * 8;
    s0.playlist.push_back(c);
  };
  addAuto(channelPluginParam(f.synth, pp::SynCutoff), 0.2f, 0.9f);       // baked into the render
  addAuto(channelParam(f.synth, ChannelParam::Volume), 0.9f, 0.3f);      // live
  addAuto(mixerParam(f.bus, MixerParam::Volume), 0.4f, 0.8f);            // live on the frozen track
  const AudioBuffer live = renderPlain(s0);
  Song c = s0;
  expect(freezeUnit(c, channelUnit(f.synth)), "a channel with automation freezes");
  const float d = maxDiff(live, renderAsPlayed(c));
  std::printf("# automation (channel freeze): max difference %.3g\n", static_cast<double>(d));
  expect(d < 1e-6f, "null test: automation of a frozen parameter is in the render, of the channel volume live");
  Song m = s0;
  expect(freezeUnit(m, mixerUnit(f.bus)), "a mixer track with automation freezes");
  const float d2 = maxDiff(live, renderAsPlayed(m));
  std::printf("# automation (mixer freeze): max difference %.3g\n", static_cast<double>(d2));
  expect(d2 < 1e-6f, "null test: automation inside a frozen track is in the render, of its fader live");
}

// ---------------------------------------------------------------- the plugins go

void pluginsUnload() {
  Fixture f = fixture();
  Song s = f.s;
  expect(freezeUnit(s, mixerUnit(f.bus)), "bus frozen");
  const freeze::Plan plan = freeze::makePlan(s);
  expect(plan.unloadMixers.count(f.bus) == 1, "the frozen track's effects are unloaded");
  expect(plan.unloadChannels.count(f.synth) == 1 && plan.unloadChannels.count(f.pluck) == 1,
         "channels that play only into it are unloaded too");
  expect(plan.unloadChannels.count(f.kick) == 0 && plan.unloadMixers.count(f.kickTrack) == 0,
         "the track keying it also plays to the master: it stays");
  expect(plan.unloadChannels.count(f.lead) == 0, "other channels stay");

  FrozenAudio fa;
  Song v = freeze::exportView(s, fa);
  ProgramBuilder live, frozen;
  live.setSampleRate(kSr);
  frozen.setSampleRate(kSr);
  frozen.setFrozenAudio(fa.channels, fa.mixers);
  auto pl = live.build(f.s);
  auto pf = frozen.build(v);
  expect(live.effectRuntime(f.bus, 0) != nullptr && live.effectRuntime(f.bus, 1) != nullptr, "live: the bus effects run");
  expect(!frozen.effectRuntime(f.bus, 0) && !frozen.effectRuntime(f.bus, 1), "frozen: no effect runtime on the bus");
  expect(frozen.channelRuntime(f.synth) && frozen.channelRuntime(f.synth)->type == "gl.sampler",
         "frozen: the synth's instrument is gone (a silent placeholder)");
  expect(frozen.channelRuntime(f.lead) && frozen.channelRuntime(f.lead)->type == "gl.fm", "frozen: the lead keeps its instrument");

  // Work per block: voices that sound, counted over the same playback.
  auto voices = [](const Program& p) {
    int n = 0;
    for (const auto& c : p.channels)
      if (c.rt && c.rt->gen && c.plugin.type != "gl.sampler") ++n;
    return n;
  };
  expect(voices(*pf) < voices(*pl), "fewer instruments run");
  auto sounding = [](const Song& song, const FrozenAudio& fz) {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    b.setFrozenAudio(fz.channels, fz.mixers);
    auto prog = b.build(song);
    AudioEngine eng(kSr);
    eng.setProgram(prog);
    eng.setLooping(false);
    eng.setOffline(true);
    eng.setFixedBlock(256);
    eng.setMode(PlayMode::Song);
    eng.play();
    std::vector<float> L(256), R(256);
    int busy = 0;
    for (int blk = 0; blk < 300; ++blk) {
      eng.process(L.data(), R.data(), 256);
      for (const auto& c : prog->channels)
        if (c.rt && c.rt->sounding.load()) ++busy;
    }
    return busy;
  };
  const int busyLive = sounding(f.s, FrozenAudio{});
  const int busyFrozen = sounding(v, fa);
  std::printf("# channel-blocks with a sounding instrument: live %d, frozen %d\n", busyLive, busyFrozen);
  expect(busyFrozen < busyLive, "fewer instrument voices sound while the bus is frozen");
  expect(ProgramBuilder::preloadItems(s).size() < ProgramBuilder::preloadItems(f.s).size(), "a project load prepares fewer plugins");
}

// ---------------------------------------------------------------- unfreeze

void unfreezeRestores() {
  Fixture f = fixture();
  Song s = f.s;
  expect(freezeUnit(s, mixerUnit(f.bus)) && freezeUnit(s, channelUnit(f.lead)), "two units frozen");
  expect(freeze::unfreeze(s, mixerUnit(f.bus)) && freeze::unfreeze(s, channelUnit(f.lead)), "both unfreeze");
  expect(s == f.s, "unfreeze gives back exactly the song before the freeze");
  expect(freeze::makePlan(s).empty(), "nothing is unloaded any more");
  expect(maxDiff(renderPlain(f.s), renderAsPlayed(s)) == 0.0f, "and it plays live again, the same");
  expect(!freeze::unfreeze(s, channelUnit(f.lead)), "unfreezing twice changes nothing");
}

// ---------------------------------------------------------------- out of date

void staleness() {
  Fixture f = fixture();
  Song s = f.s;
  expect(freezeUnit(s, channelUnit(f.synth)) && freezeUnit(s, mixerUnit(f.bus)), "frozen");
  auto still = [&](const char* name, const std::function<void(Song&)>& edit, bool stays) {
    Song x = s;
    edit(x);
    const bool a = freeze::upToDate(x, channelUnit(f.synth));
    const bool b = freeze::upToDate(x, mixerUnit(f.bus));
    expect(stays ? (a && b) : (!a && !b), name);
  };
  still("a note edit makes both out of date", [&](Song& x) { x.pattern(f.pat)->notes[f.synth][0].pitch += 1; }, false);
  still("an instrument parameter makes both out of date", [&](Song& x) { x.channel(f.synth)->plugin.params[pp::SynCutoff] *= 0.5f; }, false);
  still("the tempo makes both out of date", [&](Song& x) { x.tempo = 128; }, false);
  still("a moved clip makes both out of date", [&](Song& x) { x.playlist[0].start += 96; }, false);
  still("names, colours and icons do not", [&](Song& x) {
    x.channel(f.synth)->name = "Other";
    x.channel(f.synth)->color = 0x123456;
    x.mixerTrack(f.bus)->name = "Renamed";
    x.mixerTrack(f.bus)->icon = 3;
    x.info.title = "New title";
  }, true);
  still("the lead's notes do not (it plays elsewhere)", [&](Song& x) { x.pattern(f.pat)->notes[f.lead][0].pitch += 2; }, true);
  {
    Song x = s;
    x.mixerTrack(f.bus)->slots[0].plugin.params[0] *= 0.5f;
    expect(freeze::upToDate(x, channelUnit(f.synth)) && !freeze::upToDate(x, mixerUnit(f.bus)),
           "a bus effect parameter stales the bus, not the channel");
    x = s;
    x.mixerTrack(f.kickTrack)->slots[0].plugin = defaultPluginState("gl.dist");
    expect(!freeze::upToDate(x, mixerUnit(f.bus)), "a change on the sidechain source stales the bus");
  }
  std::set<freeze::Unit> live;
  Song x = s;
  x.tempo = 100;
  renderAsPlayed(x, &live);
  expect(live.count(channelUnit(f.synth)) && live.count(mixerUnit(f.bus)), "out-of-date units play live");
}

void reasons() {
  Fixture f = fixture();
  Song s = f.s;
  expect(!freeze::whyNotFreeze(s, mixerUnit(s.masterId())).empty(), "the master cannot be frozen");
  s.mixerTrack(f.kickTrack)->input = 0;
  expect(!freeze::whyNotFreeze(s, mixerUnit(f.kickTrack)).empty(), "a track with an audio input cannot be frozen");
  s = f.s;
  s.channel(f.lead)->layer = {f.synth};
  expect(!freeze::whyNotFreeze(s, channelUnit(f.lead)).empty(), "a layer cannot be frozen");
  s = f.s;
  expect(freezeUnit(s, mixerUnit(f.bus)), "bus frozen");
  expect(!freeze::whyNotFreeze(s, channelUnit(f.synth)).empty(), "a channel inside a frozen track cannot be frozen on its own");
  expect(freeze::coveringTrack(s, channelUnit(f.pluck)) == f.bus, "it knows the track that covers it");
  Song e = makeEmptySong();
  ObjectId ch = addChannel(e, "Empty", "gl.synth", kNoId);
  expect(!freeze::whyNotFreeze(e, channelUnit(ch)).empty(), "nothing to render: refused");
}

// ---------------------------------------------------------------- saving

void saveLoadZip() {
  Fixture f = fixture();
  Song s = f.s;
  expect(freezeUnit(s, channelUnit(f.lead)) && freezeUnit(s, mixerUnit(f.bus)), "frozen");
  s.channel(f.kick)->hibernate = true;
  Json j = songToJson(s);
  Song back;
  expect(songFromJson(j, back), "the project loads");
  expect(back.channel(f.lead)->freeze == s.channel(f.lead)->freeze && back.mixerTrack(f.bus)->freeze == s.mixerTrack(f.bus)->freeze,
         "freeze records round-trip");
  expect(back.channel(f.kick)->hibernate, "hibernate round-trips");
  Json old = songToJson(f.s);
  Song o;
  expect(songFromJson(old, o) && !o.channel(f.lead)->freeze.frozen() && !o.channel(f.kick)->hibernate,
         "projects saved before keep everything live");
  // A missing render: the unit plays live, its state intact.
  {
    Song m = s;
    m.channel(f.lead)->freeze.render.data = (fs::path(scratch) / "gone.wav").string();
    std::set<freeze::Unit> live;
    renderAsPlayed(m, &live);
    expect(live.count(channelUnit(f.lead)) == 1, "a missing render plays live");
  }
  // Zipped project: the renders go in, come back, and play.
  const std::string zip = (fs::path(scratch) / "frozen.gridlinez").string();
  CollectReport rep = exportZippedProject(s, zip, "frozen");
  expect(rep.error.empty(), "zipped project written");
  const std::string dir = (fs::path(scratch) / "unzipped").string();
  std::string projectFile;
  expect(unpackZippedProject(zip, dir, projectFile), "zipped project unpacked");
  Song z;
  expect(loadProject(projectFile, z), "unpacked project loads");
  const std::string lp = z.channel(f.lead)->freeze.render.data, bp = z.mixerTrack(f.bus)->freeze.render.data;
  std::error_code ec;
  const bool inside = fs::equivalent(fs::path(lp).parent_path().parent_path(), fs::path(dir), ec) ||
                      fs::equivalent(fs::path(lp).parent_path(), fs::path(dir), ec);
  if (!inside) std::printf("# render path %s, unpacked into %s\n", lp.c_str(), dir.c_str());
  expect(inside && fs::exists(lp) && fs::exists(bp), "the renders were packed and point into the unpacked folder");
  expect(freeze::upToDate(z, channelUnit(f.lead)) && freeze::upToDate(z, mixerUnit(f.bus)), "still up to date after the zip");
  std::set<freeze::Unit> live;
  z.channel(f.kick)->hibernate = false;
  const float d = maxDiff(renderPlain(f.s), renderAsPlayed(z, &live));
  expect(live.empty() && d < 1e-6f, "null test: the unzipped project plays its renders");
}

// ---------------------------------------------------------------- hibernate

void hibernate() {
  Fixture f = fixture();
  Song s = f.s;
  expect(freeze::setHibernate(s, channelUnit(f.lead), true), "a channel hibernates");
  expect(freeze::setHibernate(s, mixerUnit(f.kickTrack), true), "a mixer track hibernates");
  expect(!freeze::setHibernate(s, mixerUnit(s.masterId()), true), "the master never does");
  freeze::Plan asleep = freeze::makePlan(s);
  expect(asleep.unloadChannels.count(f.lead) && asleep.unloadMixers.count(f.kickTrack) && asleep.asleepMixers.count(f.kickTrack),
         "asleep: their plugins are unloaded");
  ProgramBuilder b;
  b.setSampleRate(kSr);
  b.build(freeze::playView(s, asleep));
  expect(b.channelRuntime(f.lead)->type == "gl.sampler", "asleep: the instrument is gone");
  const std::set<freeze::Unit> awake{channelUnit(f.lead), mixerUnit(f.kickTrack)};
  freeze::Plan woken = freeze::makePlan(s, {}, awake);
  expect(woken.empty(), "awake: nothing is unloaded");
  // Wake through a side builder (the app's path): the next build takes the prepared instance.
  ProgramBuilder side;
  side.setSampleRate(kSr);
  for (const auto& item : freeze::wakeItems(s, channelUnit(f.lead))) side.preload(s, item);
  b.adoptPreloaded(std::move(side));
  auto ready = b.channelRuntime(f.lead);
  b.build(freeze::playView(s, woken));
  expect(b.channelRuntime(f.lead) == ready && ready->type == "gl.fm", "the woken instrument is the one prepared ahead");
  expect(maxDiff(renderPlain(f.s), renderAsPlayed(s)) < 1e-6f, "renders wake everything: the same sound");
  auto items = ProgramBuilder::preloadItems(s);
  bool any = false;
  for (const auto& it : items) any = any || it.owner == f.lead || it.owner == f.kickTrack;
  expect(!any, "a project load prepares no hibernating plugin");
}

// ---------------------------------------------------------------- processing time

// A heavy bus (three synths into reverb, chorus and delay) frozen: the render plays from memory.
void cpuDrop() {
  Fixture f = fixture();
  Song s = f.s;
  for (int k = 0; k < 3; ++k) {
    const ObjectId c = addChannel(s, "Pad " + std::to_string(k), "gl.synth", f.bus);
    for (auto& p : s.patterns) p.notes[c] = s.patterns[0].notes[f.synth];
  }
  setFx(s, f.bus, 2, "gl.reverb");
  setFx(s, f.bus, 3, "gl.chorus");
  auto timeIt = [](const std::function<void()>& fn) {
    double best = 1e9;
    for (int k = 0; k < 3; ++k) {
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
  };
  AudioBuffer live, frozen;
  const double tl = timeIt([&] { live = renderPlain(s); });
  Song fz = s;
  expect(freezeUnit(fz, mixerUnit(f.bus)), "cpu: the heavy bus freezes");
  FrozenAudio fa;
  const Song view = freeze::exportView(fz, fa);
  RenderOptions o;
  o.sampleRate = kSr;
  o.tail = 96 * 8;
  o.frozen = fa;
  const double tf = timeIt([&] { frozen = renderSong(view, o); });
  std::printf("# render time live %.3f s, frozen %.3f s (%.0f%% less)\n", tl, tf, 100.0 * (1.0 - tf / std::max(1e-9, tl)));
  expect(maxDiff(live, frozen) < 1e-6f, "cpu: the frozen bus sounds the same");
  // Shared CI machines time noisily (an Intel runner measured 25% less once), so they only need a
  // clear saving; a local run keeps the 30% bar.
  const double bar = std::getenv("CI") ? 0.9 : 0.7;
  expect(tf < tl * bar, "cpu: the song takes far less processing with the bus frozen");
}

// ---------------------------------------------------------------- collaboration

void collaboration() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(87);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  Fixture f = fixture();
  a.doc.reset(f.s);
  a.folder = tmpDir("k87_a");
  b.folder = tmpDir("k87_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "collab: a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  // Ann freezes the bus: the record and its render reach Ben.
  FreezeRecord r;
  expect(freeze::render(a.doc.song(), mixerUnit(f.bus), kSr, (fs::path(a.folder) / "Frozen").string(), r), "collab: rendered");
  a.doc.editSong("Freeze Bus", [&](Song& x) { freeze::setRecord(x, mixerUnit(f.bus), r); });
  settle(net, {&a, &b}, clock);
  const FreezeRecord* rb = freeze::record(b.doc.song(), mixerUnit(f.bus));
  expect(rb && rb->frozen() && rb->fingerprint == r.fingerprint, "collab: the freeze arrives");
  std::error_code ec;
  const bool fileHere = rb && fs::is_regular_file(rb->render.data, ec);
  expect(fileHere, "collab: its render arrives as a file of the collaborator's own");
  expect(freeze::upToDate(b.doc.song(), mixerUnit(f.bus)), "collab: on the other computer it is up to date too (same fingerprint)");
  if (fileHere) {
    std::set<freeze::Unit> liveB;
    const AudioBuffer ab = renderAsPlayed(b.doc.song(), &liveB);
    expect(liveB.empty() && maxDiff(renderPlain(f.s), ab) < 1e-6f, "collab: it plays the render there, with the same sound");
  }
  // Hibernation stays on each computer.
  a.doc.patchSong([&](Song& x) { freeze::setHibernate(x, channelUnit(f.lead), true); });
  b.doc.patchSong([&](Song& x) { freeze::setHibernate(x, channelUnit(f.kick), true); });
  a.doc.editChannel(f.lead, "rename", [](Channel& c) { c.name = "Lead 2"; });  // something shared happens after
  settle(net, {&a, &b}, clock);
  expect(freeze::hibernating(a.doc.song(), channelUnit(f.lead)) && !freeze::hibernating(b.doc.song(), channelUnit(f.lead)),
         "collab: a channel hibernating here does not on the other computer");
  expect(freeze::hibernating(b.doc.song(), channelUnit(f.kick)) && !freeze::hibernating(a.doc.song(), channelUnit(f.kick)),
         "collab: and the other way round");
  expect(b.doc.song().channel(f.lead) && b.doc.song().channel(f.lead)->name == "Lead 2", "collab: other edits still arrive");
  // Ben unfreezes: Ann's copy unfreezes too.
  b.doc.editSong("Unfreeze Bus", [&](Song& x) { freeze::unfreeze(x, mixerUnit(f.bus)); });
  settle(net, {&a, &b}, clock);
  expect(!freeze::frozen(a.doc.song(), mixerUnit(f.bus)), "collab: an unfreeze arrives");
}

// ---------------------------------------------------------------- notes wait for a waking instrument

// Peak of `blocks` blocks of 256 frames.
float runBlocks(AudioEngine& eng, int blocks) {
  std::vector<float> L(256), R(256);
  float pk = 0;
  for (int b = 0; b < blocks; ++b) {
    eng.process(L.data(), R.data(), 256);
    for (int i = 0; i < 256; ++i) pk = std::max(pk, std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)])));
  }
  return pk;
}

// The app's builds: the plan's plugins left out, the unloaded instruments marked.
std::shared_ptr<const Program> buildAs(ProgramBuilder& b, const Song& s, const std::set<freeze::Unit>& live, const std::set<freeze::Unit>& awake) {
  const freeze::Plan plan = freeze::makePlan(s, live, awake);
  FrozenAudio fa;
  if (!plan.channelFiles.empty() || !plan.mixerFiles.empty()) freeze::exportView(s, fa, nullptr);
  std::map<ObjectId, SamplePtr> ch, mx;
  for (const auto& [id, f] : plan.channelFiles) ch[id] = fa.channels[id];
  for (const auto& [id, f] : plan.mixerFiles) mx[id] = fa.mixers[id];
  b.setFrozenAudio(ch, mx);
  std::set<ObjectId> asleep;
  for (ObjectId id : plan.unloadChannels)
    if (const Channel* c = s.channel(id); c && c->hibernate && !c->freeze.frozen()) asleep.insert(id);
  b.setUnloaded(plan.unloadChannels, asleep);
  return b.build(freeze::playView(s, plan));
}

void heldNotes() {
  Fixture f = fixture();
  Song base = f.s;
  for (auto& c : base.channels) c.muted = c.id != f.lead;  // only the lead sounds
  auto handOnly = [&](Song& x) {  // and only played by hand
    for (auto& p : x.patterns) p.notes.erase(f.lead), p.steps.erase(f.lead);
  };
  for (int kind = 0; kind < 2; ++kind) {  // 0: frozen, 1: hibernating
    const char* what = kind == 0 ? "frozen" : "hibernating";
    for (int tap = 0; tap < 2; ++tap) {  // 1: the key comes up before the instrument has loaded
      Song s = base;
      if (kind == 0) expect(freezeUnit(s, channelUnit(f.lead)), "held: frozen");
      else freeze::setHibernate(s, channelUnit(f.lead), true);
      handOnly(s);
      ProgramBuilder b;
      b.setSampleRate(kSr);
      AudioEngine eng(kSr);
      eng.setProgram(buildAs(b, s, {}, {}));
      eng.setOffline(true);
      eng.setFixedBlock(256);
      eng.setMode(PlayMode::Song);
      eng.noteOn(f.lead, 60, 0.9f);
      if (tap) eng.noteOff(f.lead, 60);
      const float before = runBlocks(eng, 4);
      expect(before < 1e-6f, (std::string("held (") + what + "): silent while the instrument is unloaded").c_str());
      // The app wakes it: the next build loads the instrument.
      const std::set<freeze::Unit> u{channelUnit(f.lead)};
      eng.setProgram(kind == 0 ? buildAs(b, s, u, {}) : buildAs(b, s, {}, u));
      const float after = runBlocks(eng, 40);
      std::printf("# held note (%s%s): peak %.3f, delivered %llu\n", what, tap ? ", tapped" : "", static_cast<double>(after),
                  static_cast<unsigned long long>(eng.heldNotesDelivered()));
      expect(after > 1e-3f && eng.heldNotesDelivered() == 1,
             (std::string("held (") + what + (tap ? ", key already up" : "") + "): the note played by hand sounds once the instrument loads").c_str());
      if (!tap) {
        eng.noteOff(f.lead, 60);
        runBlocks(eng, 200);
        expect(eng.liveHeld() == 0, (std::string("held (") + what + "): its key-up arrives and releases it").c_str());
      } else {
        expect(runBlocks(eng, 400) < after, (std::string("held (") + what + ", tapped): it ends by itself").c_str());
      }
    }
  }
  // A hibernating instrument's song note that starts before it has loaded plays once it loads.
  {
    Song s = f.s;
    for (auto& c : s.channels) c.muted = c.id != f.lead;
    freeze::setHibernate(s, channelUnit(f.lead), true);
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(buildAs(b, s, {}, {}));
    eng.setLooping(false);
    eng.setOffline(true);
    eng.setFixedBlock(256);
    eng.setMode(PlayMode::Song);
    eng.play();
    const float before = runBlocks(eng, 2);
    eng.setProgram(buildAs(b, s, {}, {channelUnit(f.lead)}));
    const float after = runBlocks(eng, 40);
    std::printf("# held song note: before %.3g, after %.3f, delivered %llu\n", static_cast<double>(before), static_cast<double>(after),
                static_cast<unsigned long long>(eng.heldNotesDelivered()));
    expect(before < 1e-6f && after > 1e-3f && eng.heldNotesDelivered() >= 1,
           "held: a song note of a hibernating instrument that starts before it loads still plays");
  }
}

// K-144: slide notes. A frozen channel renders its slides as it plays them live, and a song note
// waiting for a hibernating instrument that a slide moves meanwhile sounds at the slide's pitch.
int crossingsOf(AudioEngine& eng, int blocks) {
  std::vector<float> L(256), R(256);
  int c = 0;
  float last = 0;
  for (int b = 0; b < blocks; ++b) {
    eng.process(L.data(), R.data(), 256);
    for (float v : L) c += (v < 0) != (last < 0), last = v;
  }
  return c;
}

void slides() {
  Fixture f = fixture();
  Song s = f.s;
  for (auto& c : s.channels) c.muted = c.id != f.lead;
  Note held;
  held.start = 0, held.length = 384, held.pitch = 60, held.velocity = 100;
  Note sl = held;
  sl.start = 24, sl.length = 24, sl.pitch = 72, sl.slide = true;
  s.pattern(f.pat)->notes[f.lead] = {held, sl};
  {
    Song fz = s;
    expect(freezeUnit(fz, channelUnit(f.lead)), "slides: frozen");
    expect(maxDiff(renderPlain(s), renderAsPlayed(fz)) < 1e-6f, "slides: a frozen channel plays its slides as it did live");
  }
  int counts[2] = {0, 0};
  for (int withSlide = 0; withSlide < 2; ++withSlide) {
    Song h = s;
    if (!withSlide) h.pattern(f.pat)->notes[f.lead] = {held};
    freeze::setHibernate(h, channelUnit(f.lead), true);
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine eng(kSr);
    eng.setProgram(buildAs(b, h, {}, {}));
    eng.setLooping(false);
    eng.setOffline(true);
    eng.setFixedBlock(256);
    eng.setMode(PlayMode::Song);
    eng.play();
    runBlocks(eng, 60);  // past the slide's end, still asleep
    eng.setProgram(buildAs(b, h, {}, {channelUnit(f.lead)}));
    runBlocks(eng, 8);
    counts[withSlide] = crossingsOf(eng, 40);
  }
  std::printf("# hibernating slide: %d crossings without, %d with the slide\n", counts[0], counts[1]);
  expect(counts[0] > 0 && counts[1] > counts[0] * 18 / 10 && counts[1] < counts[0] * 22 / 10,
         "slides: a waiting note a slide moved sounds at the slide's pitch once its instrument loads");
}

// ---------------------------------------------------------------- undo

void undoRedo() {
  Fixture f = fixture();
  Document d;
  d.reset(f.s);
  FreezeRecord r;
  expect(freeze::render(d.song(), mixerUnit(f.bus), kSr, (fs::path(scratch) / "Frozen").string(), r), "rendered");
  d.editSong("Freeze Bus", [&](Song& x) { freeze::setRecord(x, mixerUnit(f.bus), r); });
  expect(freeze::frozen(d.song(), mixerUnit(f.bus)), "frozen in one step");
  d.undo();
  expect(d.song() == f.s, "undo: the song as it was");
  d.redo();
  expect(freeze::upToDate(d.song(), mixerUnit(f.bus)) && fs::exists(d.song().mixerTrack(f.bus)->freeze.render.data),
         "redo: frozen again, its render still there");
  FreezeRecord again;
  expect(freeze::render(d.song(), mixerUnit(f.bus), kSr, (fs::path(scratch) / "Frozen").string(), again) &&
             again.render.data == r.render.data,
         "a render of the same sound reuses its file");
}

}  // namespace

int main() {
  scratch = (fs::temp_directory_path() / "gridline_k87_freeze").string();
  fs::remove_all(scratch);
  fs::create_directories(scratch);
  channelFreezeNulls();
  mixerFreezeNulls();
  automationNulls();
  pluginsUnload();
  unfreezeRestores();
  staleness();
  reasons();
  saveLoadZip();
  hibernate();
  undoRedo();
  heldNotes();
  slides();
  cpuDrop();
  collaboration();
  fs::remove_all(scratch);
  return finish();
}
