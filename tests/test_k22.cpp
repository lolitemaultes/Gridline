// Gridline - K-22: channel rack steps visible and movable in the piano roll.
// The piano roll's view of a channel (its notes and its rack steps as step notes, every step
// parameter mapped), the first edit moving the steps into the notes in one undo step (a drag's
// frames merged, undo restoring the steps exactly), step notes playing exactly as the steps did
// (rendered audio identical with swing, swing truncation, shift, step parameters, step loops, echo,
// either swingNotes setting), a resized step note playing as an ordinary note, save / load, FL
// export / import round trip (step notes as FL step notes), notes back to steps, cancel snapshots.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/NoteTools.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/StepNotes.h"
#include "core/TrackPlus.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

constexpr int kSr = 44100;

std::string toneWav(const std::string& name) {
  AudioBuffer b(2, static_cast<size_t>(kSr / 2));
  for (size_t i = 0; i < b.frames; ++i) {
    const double t = static_cast<double>(i) / kSr;
    const float v = static_cast<float>(0.4 * std::sin(2 * 3.14159265358979323846 * 330.0 * t) * std::exp(-t * 6.0));
    b.at(0, i) = v, b.at(1, i) = v * 0.8f;
  }
  const std::string path = "test_k22_" + name + ".wav";
  writeWav(path, b, kSr, 16);
  return path;
}

ObjectId addChannel(Song& s, const std::string& type, const std::string& data = "") {
  Channel c;
  c.id = s.allocateId();
  c.name = type + std::to_string(s.channels.size());
  c.plugin = defaultPluginState(type);
  c.plugin.data = data;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return c.id;
}

// 2 bars of 16 steps at 120 BPM, swung, placed once. Three sampler channels with steps:
//   a: velocities, per-step pitch / pan / fine / release / mod X / mod Y / shift;
//   b: swing truncation, a shifted step, echo;
//   c: a 6-step loop; plus ordinary notes on c (they stay as they are).
struct Fixture {
  Song s;
  ObjectId pat = kNoId, a = kNoId, b = kNoId, c = kNoId;
};

Fixture makeSong(const std::string& wav, float swing = 0.45f) {
  Fixture f;
  f.s = makeEmptySong();
  Song& s = f.s;
  s.tempo = 120;
  s.swing = swing;
  Pattern& p = s.patterns[0];
  p.bars = 2;
  p.stepsPerBar = 16;
  f.pat = p.id;
  Clip clip;
  clip.id = s.allocateId();
  clip.pattern = p.id;
  s.playlist.push_back(clip);
  f.a = addChannel(s, "gl.sampler", wav);
  f.b = addChannel(s, "gl.sampler", wav);
  f.c = addChannel(s, "gl.sampler", wav);
  Pattern& q = s.patterns[0];
  std::vector<uint8_t> ra(32, 0), rb(32, 0), rc(32, 0);
  for (int k : {0, 3, 4, 7, 10, 13, 18, 25, 31}) ra[static_cast<size_t>(k)] = static_cast<uint8_t>(40 + 9 * k % 87);
  for (int k : {1, 2, 5, 9, 14, 21, 22}) rb[static_cast<size_t>(k)] = 100;
  for (int k : {0, 2, 5}) rc[static_cast<size_t>(k)] = 90;
  q.steps[f.a] = ra;
  q.steps[f.b] = rb;
  q.steps[f.c] = rc;
  std::vector<StepProps> pa(32);
  pa[3].pitch = 67, pa[3].pan = -30, pa[3].fine = 40;
  pa[4].release = 20, pa[4].modX = 200, pa[4].modY = 30;
  pa[7].shift = 25;
  pa[13].shift = 60, pa[13].pitch = 55;
  pa[25].pan = 50, pa[25].fine = -120;
  q.stepProps[f.a] = pa;
  std::vector<StepProps> pb(32);
  pb[9].shift = 40;
  q.stepProps[f.b] = pb;
  q.stepLoop[f.c] = 6;
  q.notes[f.c] = {Note(24, 48, 62, 80), Note(96 * 5, 96, 64, 110)};
  s.channel(f.b)->swingTruncate = true;
  s.channel(f.b)->echo.count = 2;
  s.channel(f.b)->echo.feedback = 0.5f;
  s.channel(f.b)->echo.time = 0.375f;
  return f;
}

AudioBuffer render(const Song& s, int block = 512) {
  RenderOptions o;
  o.blockSize = block;
  return renderSong(s, o);
}

double maxDiff(const AudioBuffer& x, const AudioBuffer& y) {
  if (x.frames != y.frames || x.channels != y.channels) return 1e9;
  double m = 0;
  for (size_t c = 0; c < x.channels; ++c)
    for (size_t i = 0; i < x.frames; ++i) m = std::max(m, static_cast<double>(std::fabs(x.at(c, i) - y.at(c, i))));
  return m;
}

double peak(const AudioBuffer& x) {
  double m = 0;
  for (size_t c = 0; c < x.channels; ++c)
    for (size_t i = 0; i < x.frames; ++i) m = std::max(m, static_cast<double>(std::fabs(x.at(c, i))));
  return m;
}

bool sameEvents(const std::vector<NoteEv>& x, const std::vector<NoteEv>& y) {
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i) {
    const NoteEv &a = x[i], &b = y[i];
    if (a.on != b.on || a.onFrac != b.onFrac || a.off != b.off || a.offFrac != b.offFrac || a.pitch != b.pitch || a.velocity != b.velocity ||
        a.props.pan != b.props.pan || a.props.release != b.props.release || a.props.modX != b.props.modX || a.props.modY != b.props.modY ||
        a.props.detune != b.props.detune || a.props.color != b.props.color || a.flags != b.flags)
      return false;
  }
  return true;
}

// Every channel's steps moved into its notes (the piano roll's first edit, without the edit).
Song converted(const Song& s, ObjectId pat) {
  Document d(s);
  for (const auto& c : s.channels) ops::convertStepsToNotes(d, pat, c.id);
  return d.song();
}

}  // namespace

int main() {
  const std::string wav = toneWav("tone");

  // ------------------------------------------------ the piano roll's view
  {
    Fixture f = makeSong(wav);
    const Pattern& p = *f.s.pattern(f.pat);
    const Tick tps = f.s.ticksPerStep(p);
    const std::vector<Note> va = stepnotes::view(f.s, p, f.a);
    expect(va.size() == 9 && std::all_of(va.begin(), va.end(), [&](const Note& n) { return n.step && n.length == tps; }),
           "a channel's steps show as step notes, one step long");
    const Note* n3 = nullptr;
    const Note* n13 = nullptr;
    for (const Note& n : va) {
      if (n.start == 3 * tps) n3 = &n;
      if (n.start == 13 * tps + tps * 60 / 100) n13 = &n;
    }
    expect(n3 && n3->pitch == 67 && n3->pan == -30 && n3->fine == 40 && n3->velocity == p.steps.at(f.a)[3],
           "step pitch, pan, fine pitch and velocity carried over");
    expect(n13 && n13->pitch == 55, "a shifted step starts inside its step, with its own key");
    const Note& n4 = *std::find_if(va.begin(), va.end(), [&](const Note& n) { return n.start == 4 * tps; });
    expect(n4.release == 20 && n4.modX == 200 && n4.modY == 30 && n4.pitch == 60, "release, mod X / Y kept; default key C5");
    const std::vector<Note> vc = stepnotes::view(f.s, p, f.c);
    expect(vc.size() == 5 && vc[1].start == 24 && !vc[1].step && vc[0].step, "notes and steps together, in order");
    expect(stepnotes::hasSteps(f.s, p, f.a) && !stepnotes::hasSteps(f.s, p, kNoId), "hasSteps");
    // Steps past the pattern's end (a shortened pattern) do not play and do not show.
    Fixture g = makeSong(wav);
    g.s.patterns[0].bars = 1;
    expect(stepnotes::view(g.s, g.s.patterns[0], g.a).size() == 6, "steps past the pattern end stay hidden (they do not play)");
  }

  // ------------------------------------------------ playback identical before / after the conversion
  {
    for (bool swingNotes : {true, false}) {
      Fixture f = makeSong(wav);
      f.s.swingNotes = swingNotes;
      const Song after = converted(f.s, f.pat);
      const Pattern& p0 = *f.s.pattern(f.pat);
      const Pattern& p1 = *after.pattern(f.pat);
      expect(p1.steps.empty() && p1.stepProps.empty() && p1.notes.at(f.a).size() == 9, "conversion moves every step into the notes");
      bool same = true;
      for (const auto& c : f.s.channels) same = same && sameEvents(compilePatternChannel(f.s, p0, c.id), compilePatternChannel(after, p1, c.id));
      expect(same, swingNotes ? "compiled events identical (swing on notes)" : "compiled events identical (swing on steps only)");
      const AudioBuffer r0 = render(f.s), r1 = render(after);
      std::printf("# render peak %.4f, max diff %.3g\n", peak(r0), maxDiff(r0, r1));
      expect(peak(r0) > 0.05 && maxDiff(r0, r1) == 0.0, swingNotes ? "rendered audio identical (swing on notes)" : "rendered audio identical (steps only)");
      expect(maxDiff(render(f.s, 64), render(after, 64)) == 0.0, "identical at block size 64 too");
    }
    // A step note given another length plays as an ordinary note (written position, the note swing).
    Fixture f = makeSong(wav);
    Song after = converted(f.s, f.pat);
    Pattern& p1 = *after.pattern(f.pat);
    const Tick tps = after.ticksPerStep(p1);
    auto& na = p1.notes.at(f.a);
    na[1].length = 2 * tps;  // the step at 3 (odd: swung)
    const auto ev = compilePatternChannel(after, p1, f.a);
    const NoteEv* e = nullptr;
    for (const auto& x : ev)
      if (std::lround(x.pitch) == 67) e = &x;
    const double on = e ? static_cast<double>(e->on) + e->onFrac : -1;
    const double want = swingWarp(static_cast<double>(3 * tps), 0.45, tps, 16);
    expect(e && std::fabs(on - want) < 1e-3 && !stepnotes::playsAsStep(after, p1, na[1]), "a resized step note is an ordinary note");
    after.swingNotes = false;
    const auto ev2 = compilePatternChannel(after, p1, f.a);
    bool straight = false;
    for (const auto& x : ev2)
      if (std::lround(x.pitch) == 67) straight = x.on == 3 * tps && x.onFrac == 0.f;
    expect(straight, "resized: no note swing when swing moves steps only");
  }

  // ------------------------------------------------ the first edit: one undo step, undo restores the steps
  {
    Fixture f = makeSong(wav);
    Document d(f.s);
    const Pattern before = *d.song().pattern(f.pat);
    const Tick tps = d.song().ticksPerStep(before);
    const size_t h0 = d.history().position();
    std::vector<Note> v = stepnotes::view(d.song(), before, f.a);
    expect(stepnotes::write(d, f.pat, f.a, v, "move notes") && d.history().position() == h0, "an unchanged list writes nothing (steps stay)");
    v[0].start += tps;  // a drag: frame 1
    expect(stepnotes::write(d, f.pat, f.a, v, "move notes", 0x22AA), "first edit");
    const Pattern& mid = *d.song().pattern(f.pat);
    expect(!mid.steps.count(f.a) && !mid.stepProps.count(f.a) && mid.notes.at(f.a).size() == 9 && mid.steps.count(f.b),
           "the edited channel's steps are notes now (other channels keep theirs)");
    v[0].start += tps;  // frame 2, same drag
    stepnotes::write(d, f.pat, f.a, v, "move notes", 0x22AA);
    d.endGesture();
    expect(d.history().position() == h0 + 1, "the drag with its conversion is one undo step");
    const Note* moved = nullptr;
    for (const Note& n : d.song().pattern(f.pat)->notes.at(f.a))
      if (n.start == 2 * tps) moved = &n;
    expect(moved && moved->step, "a moved step note stays a step note");
    d.undo();
    expect(*d.song().pattern(f.pat) == before, "undo restores the steps exactly");
    d.redo();
    expect(d.song().pattern(f.pat)->notes.at(f.a).size() == 9 && !d.song().pattern(f.pat)->steps.count(f.a), "redo");
    // Deleting a step note: the others stay (as notes), nothing doubles.
    Document d2(f.s);
    std::vector<Note> w = stepnotes::view(d2.song(), *d2.song().pattern(f.pat), f.c);
    w.erase(w.begin());
    stepnotes::write(d2, f.pat, f.c, w, "delete notes");
    const Pattern& p2 = *d2.song().pattern(f.pat);
    expect(p2.notes.at(f.c).size() == 4 && !p2.steps.count(f.c) && p2.stepLoop.at(f.c) == 6, "delete one: the rest stay, the loop stays");
    // Notes back to steps (the rack menu) gives the original row and parameters.
    Document d3(f.s);
    ops::convertStepsToNotes(d3, f.pat, f.a);
    ops::convertNotesToSteps(d3, f.pat, f.a);
    const Pattern& p3 = *d3.song().pattern(f.pat);
    tools::StepRow r0, r1;
    tools::StepPropRow q0, q1;
    ops::readStepRow(before, f.a, r0, q0);
    ops::readStepRow(p3, f.a, r1, q1);
    bool propsPlay = r0 == r1 && q0.size() == q1.size();
    for (size_t k = 0; propsPlay && k < q0.size(); ++k) {
      StepProps x = q0[k], y = q1[k];
      propsPlay = 24 * x.shift / kStepShiftMax == 24 * y.shift / kStepShiftMax;  // a shift is kept to the tick it plays on
      x.shift = y.shift = 0;
      propsPlay = propsPlay && x == y;
    }
    expect(propsPlay && !p3.notes.count(f.a), "piano roll to steps restores the row and its parameters");
    expect(sameEvents(compilePatternChannel(f.s, before, f.a), compilePatternChannel(d3.song(), p3, f.a)), "and it plays the same");
    // A tool's cancel puts the steps back.
    Document d4(f.s);
    const stepnotes::Snapshot snap = stepnotes::capture(d4.song(), f.pat, f.a);
    std::vector<Note> x = stepnotes::view(d4.song(), *d4.song().pattern(f.pat), f.a);
    for (auto& n : x) n.velocity = 20;
    stepnotes::write(d4, f.pat, f.a, x, "tool", 0x5151);
    stepnotes::restore(d4, snap, "cancel", 0x5151);
    expect(*d4.song().pattern(f.pat) == before, "cancel restores the steps");
    // Moving notes to another channel takes the steps along (none left behind).
    Document d5(f.s);
    std::vector<Note> all = stepnotes::view(d5.song(), *d5.song().pattern(f.pat), f.a);
    std::vector<Note> send(all.begin(), all.begin() + 3), keep(all.begin() + 3, all.end());
    ops::sendNotesToChannel(d5, f.pat, f.a, f.c, send, true, keep);
    const Pattern& p5 = *d5.song().pattern(f.pat);
    expect(!p5.steps.count(f.a) && p5.notes.at(f.a).size() == 6 && p5.notes.at(f.c).size() == 5 && p5.steps.count(f.c),
           "move to channel: no doubled steps");
  }

  // ------------------------------------------------ save / load
  {
    Fixture f = makeSong(wav);
    const Song after = converted(f.s, f.pat);
    Song back;
    std::string err;
    expect(songFromJson(songToJson(after), back, &err) && back.pattern(f.pat)->notes == after.pattern(f.pat)->notes,
           "step notes save and load");
    Json j = songToJson(after);
    const std::string text = jsonCompact(j);
    expect(text.find("\"step\":true") != std::string::npos || text.find("\"step\": true") != std::string::npos, "saved as \"step\"");
    Song plain = makeEmptySong();
    const ObjectId ch = addChannel(plain, "gl.synth");
    plain.patterns[0].notes[ch] = {Note(0, 24, 60, 100)};
    Song pb;
    songFromJson(songToJson(plain), pb, &err);
    expect(!pb.patterns[0].notes[ch][0].step, "ordinary notes load as ordinary notes");
  }

  // ------------------------------------------------ FL export / import round trip
  {
    Fixture f = makeSong(wav, 0.3f);
    f.s.patterns[0].stepLoop.clear();
    const Song after = converted(f.s, f.pat);
    flp::ExportOptions opt;
    opt.renderInstruments = false;
    auto roundTrip = [&](const Song& s, Song& out) {
      std::vector<uint8_t> bytes;
      flp::ExportReport rep;
      if (!flp::exportFlp(s, bytes, rep, opt)) return false;
      flp::Report r;
      std::string err;
      return importFlpBytes(bytes, out, r, &err);
    };
    Song fromSteps, fromNotes;
    expect(roundTrip(f.s, fromSteps) && roundTrip(after, fromNotes), "FL export and import");
    auto notesOf = [](const Song& s, size_t channel) {
      std::vector<Note> v;
      if (s.patterns.empty() || channel >= s.channels.size()) return v;
      auto it = s.patterns[0].notes.find(s.channels[channel].id);
      if (it != s.patterns[0].notes.end()) v = it->second;
      return v;
    };
    bool exact = true;
    size_t stepNotes = 0;
    for (ObjectId id : {f.a, f.b, f.c}) {
      const size_t k = static_cast<size_t>(after.channelIndex(id));
      const std::vector<Note> want = notesOf(after, k);
      const std::vector<Note> gotS = notesOf(fromSteps, k), gotN = notesOf(fromNotes, k);
      exact = exact && gotS == want && gotN == want;
      for (const Note& n : gotN) stepNotes += n.step;
      if (gotS != want || gotN != want)
        std::printf("# channel %zu: want %zu, from steps %zu, from notes %zu\n", k, want.size(), gotS.size(), gotN.size());
    }
    expect(exact, "steps and step notes both come back from FL as the same step notes (every parameter)");
    expect(stepNotes == 9 + 7 + 3, "every step is an FL step note (length 0)");
    // An ordinary one-step note stays ordinary through FL.
    Song plain = after;
    for (auto& [ch, ns] : plain.patterns[0].notes)
      for (auto& n : ns) n.step = false;
    Song fromPlain;
    roundTrip(plain, fromPlain);
    size_t st = 0;
    for (const auto& [ch, ns] : fromPlain.patterns[0].notes)
      for (const Note& n : ns) st += n.step;
    expect(st == 0, "ordinary notes one step long stay ordinary notes");
  }

  // ------------------------------------------------ a pattern the playlist track "+" started (K-12)
  {
    Fixture f = makeSong(wav);
    Document d(f.s);
    const trackplus::Started st = trackplus::assignChannel(d, 3, f.b, true, 0);
    expect(st.pattern != kNoId, "track + starts a pattern for the instrument");
    std::vector<uint8_t> row(16, 0);
    row[0] = row[6] = 99;
    d.setSteps(st.pattern, f.b, row);
    const size_t h = d.history().position();
    std::vector<Note> v = stepnotes::view(d.song(), *d.song().pattern(st.pattern), f.b);
    expect(v.size() == 2 && v[1].start == 6 * 24 && v[1].step, "its steps show in the piano roll");
    v[1].pitch = 64;
    stepnotes::write(d, st.pattern, f.b, v, "transpose notes");
    const Pattern& q = *d.song().pattern(st.pattern);
    expect(d.history().position() == h + 1 && !q.steps.count(f.b) && q.notes.at(f.b).size() == 2, "edited there: notes, one undo step");
    d.undo();
    expect(d.song().pattern(st.pattern)->steps.at(f.b) == row, "undo: the steps again");
  }

  std::remove(wav.c_str());
  return finish();
}
