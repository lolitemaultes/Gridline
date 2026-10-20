// Gridline tests — J-59: loop starter (GEN-01) and render to sampled
// instrument (RN-08).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/ChordTools.h"
#include "core/Document.h"
#include "core/LoopStarter.h"
#include "core/NoteTools.h"
#include "core/PluginParams.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "engine/Render.h"
#include "engine/SampledInstrument.h"
#include "host/HostedPlugins.h"
#include "host/vst3/Vst3Scan.h"
#include "packs/samplers/dwave.h"

using namespace gl;
namespace fs = std::filesystem;
namespace ls = gl::loopstarter;

namespace {

double rmsDb(const AudioBuffer& b, double fromSec, double toSec, int sr = 44100) {
  const size_t a = std::min(b.frames, static_cast<size_t>(fromSec * sr)), e = std::min(b.frames, static_cast<size_t>(toSec * sr));
  double s = 0;
  size_t n = 0;
  for (size_t c = 0; c < b.channels; ++c)
    for (size_t i = a; i < e; ++i) s += static_cast<double>(b.at(c, i)) * b.at(c, i), ++n;
  return 10 * std::log10(std::max(n ? s / static_cast<double>(n) : 0.0, 1e-20));
}

// Frequency from rising zero crossings (first and last crossing in the window).
double freqOf(const AudioBuffer& b, double fromSec, double toSec, int sr = 44100) {
  const size_t a = static_cast<size_t>(fromSec * sr), e = std::min(b.frames, static_cast<size_t>(toSec * sr));
  double first = -1, last = -1;
  int cycles = -1;
  for (size_t i = a + 1; i < e; ++i) {
    const float x0 = b.at(0, i - 1), x1 = b.at(0, i);
    if (x0 < 0 && x1 >= 0) {
      const double t = static_cast<double>(i - 1) + x0 / (x0 - x1);
      if (first < 0) first = t;
      last = t;
      ++cycles;
    }
  }
  return cycles > 0 ? cycles * sr / (last - first) : 0.0;
}

bool finite(const AudioBuffer& b) {
  for (float v : b.data)
    if (!std::isfinite(v)) return false;
  return true;
}

const Channel* channelNamed(const Song& s, const std::string& n) {
  for (const auto& c : s.channels)
    if (c.name == n) return &c;
  return nullptr;
}

const Pattern* patternNamed(const Song& s, const std::string& n) {
  for (const auto& p : s.patterns)
    if (p.name == n) return &p;
  return nullptr;
}

// ------------------------------------------------------------ loop starter ----

void testStylesAndSettings() {
  expect(ls::styles().size() >= 8, "at least eight styles");
  std::set<std::string> names;
  for (const auto& st : ls::styles()) {
    names.insert(st.name);
    expect(st.tempoDefault >= st.tempoMin && st.tempoDefault <= st.tempoMax, (std::string(st.name) + ": default tempo in range").c_str());
  }
  for (const char* n : {"House", "Techno", "Hip hop", "Trap", "Lo-fi", "Drum and bass", "Pop", "Ambient"})
    expect(names.count(n) == 1, (std::string("style ") + n).c_str());
  for (int sc : ls::scaleChoices()) expect(tools::scales()[static_cast<size_t>(sc)].steps.size() == 7, "offered scales have seven notes");
  ls::Settings a = ls::styleDefaults(0, 1), b = ls::styleDefaults(0, 2);
  expect(a.seeds != b.seeds, "different seeds give different part seeds");
  ls::Settings r = a;
  r.locked[ls::kChords] = true;
  ls::reroll(r, -1, 77);
  expect(r.seeds[ls::kChords] == a.seeds[ls::kChords], "re-roll all keeps a locked part");
  expect(r.seeds[ls::kDrums] != a.seeds[ls::kDrums] && r.seeds[ls::kBass] != a.seeds[ls::kBass] && r.seeds[ls::kLead] != a.seeds[ls::kLead],
         "re-roll all changes the unlocked parts");
  ls::Settings r2 = a;
  r2.locked[ls::kChords] = true;
  ls::reroll(r2, -1, 77);
  expect(r2 == r, "re-roll is deterministic for the same entropy");
  ls::Settings one = a;
  ls::reroll(one, ls::kBass, 5);
  expect(one.seeds[ls::kBass] != a.seeds[ls::kBass] && one.seeds[ls::kDrums] == a.seeds[ls::kDrums] && one.seeds[ls::kLead] == a.seeds[ls::kLead],
         "re-roll one part only");
  ls::Settings lk = a;
  lk.locked[ls::kLead] = true;
  ls::reroll(lk, ls::kLead, 9);
  expect(lk.seeds[ls::kLead] == a.seeds[ls::kLead], "a locked part is not re-rolled");
  expect(ls::describe(a).find("House") == 0 && ls::describe(a).find("BPM") != std::string::npos, "summary line");
}

void testDeterminism() {
  for (int style = 0; style < static_cast<int>(ls::styles().size()); ++style) {
    ls::Settings s = ls::styleDefaults(style, 42);
    const Song x = ls::generate(s), y = ls::generate(s);
    expect(x == y && projectToText(x) == projectToText(y), (std::string(ls::styles()[static_cast<size_t>(style)].name) + ": same seed, same project").c_str());
  }
  // A different drum seed changes only the drums.
  ls::Settings s = ls::styleDefaults(0, 3);
  const Song base = ls::generate(s);
  ls::reroll(s, ls::kDrums, 11);
  const Song drums = ls::generate(s);
  expect(patternNamed(base, "Drums")->steps != patternNamed(drums, "Drums")->steps ||
             patternNamed(base, "Drums")->notes != patternNamed(drums, "Drums")->notes,
         "re-rolled drums differ");
  for (const char* p : {"Bass", "Chords", "Lead"})
    expect(patternNamed(base, p)->notes == patternNamed(drums, p)->notes, (std::string(p) + " unchanged by a drum re-roll").c_str());
  // Chords re-rolled: bass and lead follow the new chords, drums stay.
  ls::Settings c = ls::styleDefaults(6, 3);
  const Song before = ls::generate(c);
  bool changed = false;
  for (uint32_t e = 1; e < 6 && !changed; ++e) {
    ls::Settings t = c;
    ls::reroll(t, ls::kChords, e);
    const Song after = ls::generate(t);
    changed = patternNamed(before, "Chords")->notes != patternNamed(after, "Chords")->notes;
    if (changed) expect(patternNamed(before, "Drums")->steps == patternNamed(after, "Drums")->steps, "drums unchanged by a chord re-roll");
  }
  expect(changed, "re-rolled chords differ");
  // Energy: a busier groove at high energy.
  ls::Settings lo = ls::styleDefaults(1, 5), hi = lo;
  lo.energy = 0.1, hi.energy = 1.0;
  auto hits = [](const Song& song) {
    size_t n = 0;
    for (const auto& [id, row] : patternNamed(song, "Drums")->steps)
      for (uint8_t v : row) n += v > 0;
    for (const auto& [id, ns] : patternNamed(song, "Drums")->notes) n += ns.size();
    return n;
  };
  expect(hits(ls::generate(hi)) > hits(ls::generate(lo)), "higher energy, more drum hits");
}

// Pitched notes (bass, chords, lead) all inside the key's scale.
bool inKey(const Song& song, int key, int scale) {
  for (const char* part : {"Bass", "Chords", "Lead"}) {
    const Channel* ch = channelNamed(song, part);
    for (const auto& p : song.patterns) {
      auto it = p.notes.find(ch->id);
      if (it == p.notes.end()) continue;
      for (const Note& n : it->second)
        if (!tools::inScale(n.pitch, key, scale)) return false;
    }
  }
  return true;
}

void testEveryStyle() {
  for (int style = 0; style < static_cast<int>(ls::styles().size()); ++style) {
    const std::string sn = ls::styles()[static_cast<size_t>(style)].name;
    ls::Settings s = ls::styleDefaults(style, 1234);
    const Song song = ls::generate(s);
    // Valid project: round trip through the file format, references resolve.
    Song back;
    std::string err;
    expect(songFromJson(songToJson(song), back, &err) && back == song, (sn + ": project round trip " + err).c_str());
    expect(song.tempo == ls::effectiveTempo(s) && song.tempo == ls::styles()[static_cast<size_t>(style)].tempoDefault, (sn + ": tempo").c_str());
    expect(!song.markers.empty() && song.markers[0].keyRoot == s.key && song.markers[0].keyScale == s.scale, (sn + ": key marker").c_str());
    expect(inKey(song, s.key, s.scale), (sn + ": bass, chords and lead in key").c_str());
    bool builtIn = true;
    for (const auto& c : song.channels) builtIn = builtIn && findPlugin(c.plugin.type) && !isHostedType(c.plugin.type);
    for (const auto& m : song.mixer)
      for (const auto& sl : m.slots) builtIn = builtIn && (sl.plugin.empty() || (findPlugin(sl.plugin.type) && !isHostedType(sl.plugin.type)));
    expect(builtIn, (sn + ": Gridline's own instruments and effects only").c_str());
    bool routed = true;
    std::set<ObjectId> tracks;
    for (const auto& c : song.channels) routed = routed && c.mixerTrack != song.masterId() && song.mixerTrack(c.mixerTrack), tracks.insert(c.mixerTrack);
    expect(routed && tracks.size() >= 7, (sn + ": channels on their own mixer inserts").c_str());
    // Each part plays something.
    for (const char* p : {"Drums", "Bass", "Chords", "Lead"}) {
      const Pattern* pat = patternNamed(song, p);
      RenderOptions o;
      o.sampleRate = 22050;
      o.patternMode = true;
      o.pattern = pat ? pat->id : kNoId;
      AudioBuffer b = renderSong(song, o);
      expect(pat && finite(b) && rmsDb(b, 0, 1e9, 22050) > -45, (sn + ": " + p + " is not silent").c_str());
    }
    // The mix: not silent, not clipping.
    RenderOptions o;
    o.tail = song.barTicks();
    AudioBuffer mix = renderSong(song, o);
    const double rms = rmsDb(mix, 0, 1e9), peak = mix.peak();
    std::printf("# %-14s rms %.1f dB, peak %.2f\n", sn.c_str(), rms, static_cast<double>(peak));
    expect(finite(mix) && rms > -26 && rms < -8, (sn + ": mix level").c_str());
    expect(peak > 0.1f && peak < 0.999f, (sn + ": mix does not clip").c_str());
  }
}

void testScalesAndForms() {
  for (int sc : ls::scaleChoices())
    for (int key : {0, 4, 10}) {
      ls::Settings s = ls::styleDefaults(2, static_cast<uint32_t>(sc * 31 + key));
      s.key = key, s.scale = sc;
      for (ls::Mood m : {ls::Mood::Dark, ls::Mood::Dreamy}) {
        s.mood = m;
        expect(inKey(ls::generate(s), key, sc), "every offered scale and key stays in key");
      }
    }
  ls::Settings s = ls::styleDefaults(0, 9);
  s.form = ls::Form::Loop8;
  const Song l8 = ls::generate(s);
  expect(l8.lengthTicks() == 8 * l8.barTicks() && patternNamed(l8, "Chords")->bars == 8, "8-bar loop");
  s.form = ls::Form::Arrangement;
  const Song ar = ls::generate(s);
  std::set<std::string> secs;
  for (const auto& m : ar.markers) secs.insert(m.name);
  expect(ar.lengthTicks() == 36 * ar.barTicks() && secs.count("Intro") && secs.count("Break") && secs.count("Outro") && patternNamed(ar, "Drums light"),
         "short arrangement: 36 bars, sections, light drums");
  Song back;
  expect(songFromJson(songToJson(ar), back) && back == ar, "arrangement round trip");
  // Tempo override.
  s.tempo = 101;
  expect(ls::generate(s).tempo == 101, "tempo override");
}

// The window puts a generated project into the document with one whole-song edit.
void testOneUndoStep() {
  Document d(makeEmptySong());
  const Song empty = d.song();
  const Song s = ls::generate(ls::styleDefaults(4, 8));
  d.editSong("loop starter", [&](Song& x) { x = s; });
  expect(d.history().size() == 1 && d.song() == s, "a generated project is one undo step");
  d.undo();
  expect(d.song() == empty, "undo brings the previous project back");
  d.redo();
  expect(d.song() == s, "redo applies it again");
}

// ------------------------------------------------------- render to sampler ----

void testMapping() {
  sampled::Options o;
  o.noteLo = 36, o.noteHi = 50, o.step = 4;
  const auto n = sampled::sampledNotes(o);
  expect((n == std::vector<int>{36, 40, 44, 48, 50}), "sampled notes: range, step, top note");
  o.notes = {62, 50, 62, 200};
  expect((sampled::sampledNotes(o) == std::vector<int>{50, 62}), "sampled notes: a note selection");
  // Key ranges tile 0..127; velocity layers tile 1..127.
  int next = 0;
  bool ok = true;
  for (size_t i = 0; i < n.size(); ++i) {
    int lo, hi;
    sampled::keyRange(n, i, lo, hi);
    ok = ok && lo == next && hi >= n[i] && lo <= n[i];
    next = hi + 1;
  }
  expect(ok && next == 128, "key ranges cover every key once, each around its root");
  for (int layers = 1; layers <= 8; ++layers) {
    int v = 1;
    bool good = true;
    for (int k = 0; k < layers; ++k) {
      int lo, hi;
      sampled::velocityLayer(layers, k, lo, hi);
      good = good && lo == v && hi >= lo;
      v = hi + 1;
    }
    expect(good && v == 128, "velocity layers cover 1..127");
  }
  expect(sampled::noteLabel(60) == "C5" && sampled::noteLabel(61) == "C#5", "note labels (C5 = 60)");
}

void testTrimAndLoop() {
  AudioBuffer a(2, 6000);
  for (size_t i = 1000; i < 3000; ++i) a.at(0, i) = a.at(1, i) = 0.5f * static_cast<float>(std::sin(i * 0.1));
  const int64_t cut = sampled::trimSilence(a, -60);
  expect(cut == 984 && a.frames == 16 + 2000 + 256, "trim: leading and trailing silence");
  expect(std::fabs(a.at(0, a.frames - 1)) < 1e-6f, "trim: faded end");
  AudioBuffer s(2, 44100);
  for (size_t i = 0; i < s.frames; ++i) s.at(0, i) = s.at(1, i) = 0.4f * static_cast<float>(std::sin(2 * M_PI * 220.0 * i / 44100.0));
  int ls0 = 0, le0 = 0;
  expect(sampled::findLoop(s, 8000, 40000, 44100, ls0, le0) && le0 - ls0 > 2205, "loop found in a steady tone");
  const double cycles = (le0 - ls0) * 220.0 / 44100.0;
  expect(std::fabs(cycles - std::round(cycles)) < 0.05, "loop spans whole cycles");
  AudioBuffer z(2, 44100);
  expect(!sampled::findLoop(z, 8000, 40000, 44100, ls0, le0), "no loop in silence");
}

// One held note on `chan` (other channels silent), from 0.
AudioBuffer playNote(Song song, ObjectId chan, int pitch, int vel, double seconds) {
  song.tempo = 120;
  Pattern& p = song.patterns[0];
  p.notes.clear();
  p.steps.clear();
  const Tick len = static_cast<Tick>(seconds * 2 * song.ppq);
  p.notes[chan] = {Note(0, len, pitch, vel)};
  p.bars = static_cast<int>(len / song.barTicks()) + 2;
  for (auto& c : song.channels) c.muted = c.id != chan;
  RenderOptions o;
  o.patternMode = true;
  o.pattern = p.id;
  return renderSong(song, o);
}

void testRenderToSampler() {
  Document d(makeEmptySong());
  const ObjectId sine = d.addChannel("Sine", "gl.synth");
  d.editChannel(sine, "", [](Channel& c) {
    using namespace pp;
    c.plugin.params[SynOsc1Shape] = 0;  // sine
    c.plugin.params[SynOsc2Level] = 0;
    c.plugin.params[SynSustain] = 1;
    c.plugin.params[SynFilterEnv] = 0;
    c.plugin.params[SynCutoff] = 20000;
    c.plugin.params[SynRelease] = 0.1f;
  });
  const ObjectId rich = d.addChannel("Rich", "gl.synth");  // saw + square, filter envelope
  const std::string dir = (fs::path(gl::test::hermeticTestEnv("j59")) / "rendered").string();

  sampled::Options o;
  o.noteLo = 48, o.noteHi = 72, o.step = 6;
  o.velocityLayers = 2;
  o.noteSeconds = 1.0, o.tailSeconds = 0.4;
  o.findLoops = true;
  o.name = "Sine";
  const std::string sdir = sampled::freshFolder(dir, "Sine");
  sampled::Result r = sampled::renderToSampler(d.song(), sine, o, sdir);
  expect(r.error.empty() && r.takes.size() == 5 * 2, ("rendered 5 notes x 2 layers " + r.error).c_str());
  bool files = true, loops = true, trimmed = true;
  for (const auto& t : r.takes) {
    files = files && fs::exists(t.file);
    loops = loops && t.loopEnd > t.loopStart;
    trimmed = trimmed && t.audio.frames < static_cast<size_t>(1.4 * 44100) + 300 && t.audio.frames > 44100;
  }
  expect(files, "one WAV per take");
  expect(loops, "sustain loops found on held sine notes");
  expect(trimmed, "takes trimmed to the sound");
  expect(sampled::freshFolder(dir, "Sine") != sdir, "a second render goes to a new folder");
  // The preset maps the files.
  PluginState pst;
  expect(loadPreset(r.presetFile, "gl.waveprogram", pst) && pst == r.state, "preset file loads as the Wave Program state");
  packs::samplers::dw::DwProgram prog;
  expect(packs::samplers::dw::parseProgram(r.state.data, prog) && prog.zones.size() == 10, "program: one zone per take");
  bool mapped = prog.zones.size() == r.takes.size();
  for (size_t i = 0; mapped && i < prog.zones.size(); ++i) {
    const auto& z = prog.zones[i];
    const auto& t = r.takes[i];
    mapped = z.root == t.note && z.lo == t.keyLo && z.hi == t.keyHi && z.vlo == t.velLo && z.vhi == t.velHi && z.path == t.file &&
             z.loopMode == 3 && z.loopEnd == t.loopEnd;
  }
  expect(mapped, "zones: root, key range, velocity layer, file, loop");
  expect(prog.zones[0].lo == 0 && prog.zones.back().hi == 127 && prog.zones[0].vlo == 1 && prog.zones[1].vlo == 65, "zones cover the keyboard and velocity");

  // The new instrument matches the source on held notes.
  Document d2(d.song());
  const size_t before = d2.song().channels.size();
  const ObjectId smp = sampled::applyToProject(d2, sine, r.state, false, "Sine (sampled)");
  expect(d2.song().channels.size() == before + 1 && d2.song().channel(smp)->plugin.type == "gl.waveprogram" &&
             d2.song().channelIndex(smp) == d2.song().channelIndex(sine) + 1,
         "added as a new channel after the source");
  for (int pitch : {54, 72}) {
    for (int vel : {64, 127}) {
      AudioBuffer a = playNote(d2.song(), sine, pitch, vel, 0.8), b = playNote(d2.song(), smp, pitch, vel, 0.8);
      const double ra = rmsDb(a, 0.2, 0.7), rb = rmsDb(b, 0.2, 0.7);
      std::printf("# sampled note %d vel %d: source %.2f dB, sampler %.2f dB\n", pitch, vel, ra, rb);
      expect(std::fabs(ra - rb) < 0.5, "held note level matches at a sampled note");
      expect(std::fabs(freqOf(a, 0.2, 0.7) / freqOf(b, 0.2, 0.7) - 1) < 0.003, "held note pitch matches at a sampled note");
    }
  }
  {  // Between the samples: transposed by the sampler.
    AudioBuffer a = playNote(d2.song(), sine, 57, 127, 0.8), b = playNote(d2.song(), smp, 57, 127, 0.8);
    const double fa = freqOf(a, 0.2, 0.7), fb = freqOf(b, 0.2, 0.7);
    std::printf("# between samples: %.2f Hz vs %.2f Hz, %.2f vs %.2f dB\n", fa, fb, rmsDb(a, 0.2, 0.7), rmsDb(b, 0.2, 0.7));
    expect(std::fabs(fb / fa - 1) < 0.003 && std::fabs(fa - 220.0) < 1.0, "a note between samples plays at its pitch");
    expect(std::fabs(rmsDb(a, 0.2, 0.7) - rmsDb(b, 0.2, 0.7)) < 1.0, "a note between samples keeps the level");
  }
  {  // Held longer than the take: the sustain loop keeps it sounding.
    AudioBuffer b = playNote(d2.song(), smp, 60, 127, 3.0);
    expect(rmsDb(b, 2.2, 2.8) > rmsDb(b, 0.2, 0.7) - 1.0, "sustain loop holds a long note");
  }
  // Velocity layers: the soft layer answers soft notes.
  {
    AudioBuffer soft = playNote(d2.song(), smp, 60, 40, 0.8), loud = playNote(d2.song(), smp, 60, 127, 0.8);
    expect(rmsDb(loud, 0.2, 0.7) - rmsDb(soft, 0.2, 0.7) > 6.0, "soft notes play softer");
  }
  // Undo: one step.
  d2.undo();
  expect(d2.song().channels.size() == before, "adding the sampled channel is one undo step");
  // Replace.
  const ObjectId same = sampled::applyToProject(d2, sine, r.state, true, "");
  expect(same == sine && d2.song().channel(sine)->plugin.type == "gl.waveprogram" && d2.song().channels.size() == before, "replace the channel's instrument");
  d2.undo();
  expect(d2.song().channel(sine)->plugin.type == "gl.synth", "replacing is one undo step");

  // A richer sound with round robins, no loops, 16-bit.
  sampled::Options o2;
  o2.notes = {57};
  o2.roundRobins = 2;
  o2.noteSeconds = 0.8, o2.tailSeconds = 0.5;
  o2.bits = 16;
  o2.name = "Rich";
  sampled::Result r2 = sampled::renderToSampler(d.song(), rich, o2, sampled::freshFolder(dir, "Rich"));
  expect(r2.error.empty() && r2.takes.size() == 2 && fs::exists(r2.takes[1].file) && r2.takes[1].file.find("_rr2") != std::string::npos,
         "round robins: two takes per note");
  packs::samplers::dw::DwProgram p2;
  packs::samplers::dw::parseProgram(r2.state.data, p2);
  expect(p2.zones.size() == 2 && p2.zones[0].trigGroup == 1 && p2.trig[1].type == 1 && p2.zones[0].loopMode == 0, "round robins cycle in a trigger group");
  Document d3(d.song());
  const ObjectId rs = sampled::applyToProject(d3, rich, r2.state, false, "");
  AudioBuffer a = playNote(d3.song(), rich, 57, 127, 0.6), b = playNote(d3.song(), rs, 57, 127, 0.6);
  std::printf("# rich: source %.2f dB, sampler %.2f dB\n", rmsDb(a, 0.1, 0.5), rmsDb(b, 0.1, 0.5));
  expect(std::fabs(rmsDb(a, 0.1, 0.5) - rmsDb(b, 0.1, 0.5)) < 1.0, "rich sound: held level matches");
  expect(d3.song().channel(rs)->name == "Rich (sampled)", "default name of the new channel");

  // Cancel.
  std::atomic<bool> cancel{true};
  sampled::Result rc = sampled::renderToSampler(d.song(), sine, o, sampled::freshFolder(dir, "Cancelled"), {}, &cancel);
  expect(rc.cancelled && !rc.error.empty() && rc.takes.empty(), "cancel stops the render");
  expect(!sampled::renderToSampler(d.song(), 999999, o, dir).error.empty(), "unknown channel is an error");
  {
    Document e(makeEmptySong());
    const ObjectId mute = e.addChannel("Silent", "gl.synth");
    e.editChannel(mute, "", [](Channel& c) { c.plugin.params[pp::SynVolume] = 0; });
    sampled::Options q;
    q.notes = {60};
    q.noteSeconds = 0.1, q.tailSeconds = 0;
    const std::string sd = sampled::freshFolder(dir, "Silent");
    expect(!sampled::renderToSampler(e.song(), mute, q, sd).error.empty() && !fs::exists(sd), "a silent instrument is reported, nothing written");
  }
}

void testHostedSource() {
  // A hosted plugin renders as itself (the in-tree VST3 test synth: one sine voice).
  host::registerScanResult(vst3::scanBundleInProcess(GRIDLINE_TESTPLUG_BUNDLE));
  const std::string kSynth = "vst3:47524944" "4C494E45" "54535931" "00000001";
  if (!findPlugin(kSynth)) {
    expect(false, "test VST3 synth registered");
    return;
  }
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Hosted", kSynth);
  sampled::Options o;
  o.notes = {57, 69};
  o.noteSeconds = 0.5, o.tailSeconds = 0.1;
  o.name = "Hosted";
  const std::string dir = (fs::path(gl::test::hermeticTestEnv("j59")) / "hosted").string();
  sampled::Result r = sampled::renderToSampler(d.song(), ch, o, dir);
  expect(r.error.empty() && r.takes.size() == 2, ("hosted instrument rendered " + r.error).c_str());
  if (r.takes.size() == 2) {
    expect(r.takes[1].audio.peak() > 0.05f, "hosted take is not silent");
    const double f = freqOf(r.takes[1].audio, 0.05, 0.4);
    std::printf("# hosted A5 take: %.2f Hz\n", f);
    expect(std::fabs(f - 440.0) < 2.0, "hosted take plays the note's pitch");
  }
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("j59");
  testStylesAndSettings();
  testDeterminism();
  testScalesAndForms();
  testOneUndoStep();
  testEveryStyle();
  testMapping();
  testTrimAndLoop();
  testRenderToSampler();
  testHostedSource();
  return finish();
}
