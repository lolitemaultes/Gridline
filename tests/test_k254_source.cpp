// Gridline: K-254, render MIDI to audio: the engine call (engine/SourceRender.h).
// On a song with a step-sequenced "Kick" row (through mixer track 1 at half volume) and a "Lead"
// row of notes (on the master) in the pattern "Break A", played by two playlist clips:
//  * a rack sequence through the mixer is sample for sample a normal export of the pattern with
//    the other row muted by hand (the master neutral);
//  * through the channel's own effects it is sample for sample the freeze render of the channel,
//    and a normal export of the pattern with the row at unity, centre, on the master;
//  * the other row's notes never sound in it; Dry leaves the channel echo out;
//  * a pattern clip renders exactly as K-250's clip render (its offset and length kept);
//  * the whole pattern is a normal pattern export;
//  * selected notes render as a one-shot: it starts at the first sound and ends where the sound
//    falls under -60 dB; the untrimmed render is the selected notes alone;
//  * tail modes (Cut ends at the span, Leave rings on, Wrap folds back), loops, hits;
//  * the same render for any block size.
#include <cmath>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/ClipRender.h"
#include "engine/Freeze.h"
#include "engine/Render.h"
#include "engine/SourceRender.h"

using namespace gl;
namespace sr = gl::srcrender;

namespace {

constexpr int kRate = 44100;

struct Fixture {
  Song s;
  ObjectId kick = kNoId, lead = kNoId, pattern = kNoId, clipA = kNoId, clipB = kNoId;
};

Fixture fixture() {
  Fixture f;
  Song& s = f.s;
  s = makeEmptySong();
  s.tempo = 140;
  Channel k;
  k.id = s.allocateId();
  k.name = "Kick";
  k.plugin = defaultPluginState("gl.synth");
  k.mixerTrack = s.mixer.size() > 1 ? s.mixer[1].id : s.masterId();
  k.gain = 0.6f;
  k.pan = -0.3f;
  s.channels.push_back(k);
  if (s.mixer.size() > 1) s.mixer[1].volume = 0.5f;
  f.kick = k.id;
  Channel l;
  l.id = s.allocateId();
  l.name = "Lead";
  l.plugin = defaultPluginState("gl.synth");
  l.mixerTrack = s.masterId();
  s.channels.push_back(l);
  f.lead = l.id;
  Pattern p;
  p.id = s.allocateId();
  p.name = "Break A";
  p.bars = 2;
  std::vector<uint8_t> steps(32, 0);
  for (int i : {0, 6, 10, 16, 22, 26, 30}) steps[static_cast<size_t>(i)] = 100;
  p.steps[k.id] = steps;
  for (int n = 0; n < 6; ++n) p.notes[l.id].push_back(Note(n * s.beatTicks() + 7, s.beatTicks() / 2, 57 + 2 * n, 90));
  s.patterns.push_back(p);
  f.pattern = p.id;
  Clip a;
  a.id = s.allocateId();
  a.pattern = p.id;
  a.track = 0;
  a.start = s.barTicks();
  s.playlist.push_back(a);
  f.clipA = a.id;
  Clip b = a;
  b.id = s.allocateId();
  b.track = 1;
  b.start = 4 * s.barTicks();
  b.offset = s.beatTicks();
  b.length = s.barTicks() + s.beatTicks() / 2;
  s.playlist.push_back(b);
  f.clipB = b.id;
  return f;
}

double maxDiff(const AudioBuffer& a, const AudioBuffer& b) {
  if (a.frames != b.frames || a.channels != b.channels) return 1e9;
  double d = 0;
  for (size_t i = 0; i < a.data.size(); ++i) d = std::max(d, static_cast<double>(std::fabs(a.data[i] - b.data[i])));
  return d;
}

AudioBuffer patternExport(const Song& song, ObjectId pattern, TailMode tail = TailMode::Cut) {
  RenderJob j;
  j.sampleRate = kRate;
  j.scope = RenderScope::Pattern;
  j.pattern = pattern;
  j.tail = tail;
  j.maxTailSeconds = 4;
  return renderAudio(song, j).audio;
}

sr::Options opts(sr::Through t) {
  sr::Options o;
  o.through = t;
  o.sampleRate = kRate;
  return o;
}

void testChannelThroughMixer() {
  Fixture f = fixture();
  sr::Output out = sr::render(f.s, sr::channelSource(f.pattern, f.kick), opts(sr::Through::Mixer));
  expect(out.error.empty() && out.audio.frames > 0, "the Kick row renders through the mixer");
  // A normal export of the pattern, the Lead row muted by hand, the master neutral.
  Song ref = stemSong(f.s, f.s.masterId(), false);
  ref.channel(f.lead)->muted = true;
  const AudioBuffer want = patternExport(ref, f.pattern);
  expect(maxDiff(out.audio, want) == 0.0, "through the mixer: sample for sample the pattern export with the other row muted");
  expect(out.audio.peak() > 1e-3f, "the render holds sound");
  expect(!out.keepsStrip, "through the mixer the render already holds the strip");
  // The other row never sounds in it: the same render with the Lead's notes gone.
  Fixture g = fixture();
  g.s.pattern(g.pattern)->notes.erase(g.lead);
  sr::Output alone = sr::render(g.s, sr::channelSource(g.pattern, g.kick), opts(sr::Through::Mixer));
  expect(maxDiff(out.audio, alone.audio) == 0.0, "the other row's notes are not in the render");
}

void testChannelOwnEffects() {
  Fixture f = fixture();
  sr::Output out = sr::render(f.s, sr::channelSource(f.pattern, f.kick), opts(sr::Through::ChannelFx));
  expect(out.error.empty() && out.audio.frames > 0, "the Kick row renders through its own effects");
  RenderJob j;
  j.sampleRate = kRate;
  j.scope = RenderScope::Pattern;
  j.pattern = f.pattern;
  const AudioBuffer frz = renderAudio(freeze::renderSong(f.s, freeze::channelUnit(f.kick)), j).audio;
  expect(maxDiff(out.audio, frz) == 0.0, "through its own effects: sample for sample the freeze render of the channel");
  // A normal export with the row at unity and centre on the master (the master neutral).
  Song ref = stemSong(f.s, f.s.masterId(), false);
  ref.channel(f.lead)->muted = true;
  Channel* k = ref.channel(f.kick);
  k->gain = 0.78f, k->pan = 0, k->mixerTrack = ref.masterId();
  expect(maxDiff(out.audio, patternExport(ref, f.pattern)) == 0.0, "through its own effects: the pattern export of the row at unity on the master");
  expect(out.keepsStrip && out.stripOf == f.kick, "one row through its own effects plays back through the row's strip");
  expect(out.hits.size() == 7, "seven step hits");
  bool frames = out.hits.size() == 7;
  RenderJob pj;
  pj.scope = RenderScope::Pattern;
  pj.pattern = f.pattern;
  const Tick step = f.s.ticksPerStep(*f.s.pattern(f.pattern));
  const int idx[] = {0, 6, 10, 16, 22, 26, 30};
  for (size_t i = 0; frames && i < 7; ++i)
    frames = out.hits[i].tick == idx[i] * step && out.hits[i].frame == scopeFrames(f.s, pj, 0, idx[i] * step, kRate);
  expect(frames, "each hit at its step, in the engine's own frames");
}

void testDry() {
  Fixture f = fixture();
  f.s.channel(f.lead)->echo.count = 3;
  f.s.channel(f.lead)->echo.time = 0.25f;
  sr::Output fx = sr::render(f.s, sr::channelSource(f.pattern, f.lead), opts(sr::Through::ChannelFx));
  sr::Output dry = sr::render(f.s, sr::channelSource(f.pattern, f.lead), opts(sr::Through::Dry));
  expect(maxDiff(fx.audio, dry.audio) > 1e-4, "the channel echo is in the channel effects render, not the dry one");
  Fixture g = fixture();
  sr::Output plain = sr::render(g.s, sr::channelSource(g.pattern, g.lead), opts(sr::Through::ChannelFx));
  expect(maxDiff(dry.audio, plain.audio) == 0.0, "dry: the instrument alone (as without the echo)");
}

void testClipAndPattern() {
  Fixture f = fixture();
  for (ObjectId id : {f.clipA, f.clipB}) {
    sr::Output out = sr::render(f.s, sr::clipSource(id), opts(sr::Through::Mixer));
    const RenderResult k = renderClipAlone(f.s, id, kRate);
    expect(out.error.empty() && maxDiff(out.audio, k.audio) == 0.0, "a pattern clip renders exactly as the clip render");
    const Clip* c = f.s.clip(id);
    RenderJob j;
    j.scope = RenderScope::Range;
    expect(static_cast<int64_t>(out.audio.frames) == scopeFrames(f.s, j, c->start, c->start + f.s.clipLength(*c), kRate),
           "the clip's own length (offset clip too)");
    expect(out.songTicks && out.start == c->start, "the clip's start on the song's timeline");
  }
  sr::Output pa = sr::render(f.s, sr::clipSource(f.clipA), opts(sr::Through::Mixer));
  sr::Output pb = sr::render(f.s, sr::clipSource(f.clipB), opts(sr::Through::Mixer));
  expect(maxDiff(pa.audio, pb.audio) != 0.0, "the offset clip plays other content");
  sr::Output whole = sr::render(f.s, sr::patternSource(f.pattern), opts(sr::Through::Mixer));
  expect(maxDiff(whole.audio, patternExport(stemSong(f.s, f.s.masterId(), false), f.pattern)) == 0.0,
         "the whole pattern is the normal pattern export (master neutral)");
  expect(sr::sourceName(f.s, sr::clipSource(f.clipB)) == "Break A clip 2", "the second clip of Break A");
  expect(sr::sourceName(f.s, sr::channelSource(f.pattern, f.kick)) == "Kick sequence (Break A)", "a sequence's name");
}

void testOneShot() {
  Fixture f = fixture();
  const sr::Source src = sr::notesSource(f.pattern, f.lead, {2, 3});
  sr::Options o = sr::defaultOptions(src);
  o.sampleRate = kRate;
  expect(o.oneShotTrim && o.tail == TailMode::Leave && o.through == sr::Through::ChannelFx, "notes render as one-shots by default");
  sr::Output out = sr::render(f.s, src, o);
  expect(out.error.empty() && out.audio.frames > 0, "selected notes render");
  const float thr = static_cast<float>(std::pow(10.0, -90.0 / 20.0));
  expect(std::fabs(out.audio.at(0, 0)) > thr || std::fabs(out.audio.at(1, 0)) > thr, "the one-shot starts at the first sound");
  sr::Options raw = o;
  raw.oneShotTrim = false;
  sr::Output full = sr::render(f.s, src, raw);
  size_t first = 0;
  while (first < full.audio.frames && std::fabs(full.audio.at(0, first)) <= thr && std::fabs(full.audio.at(1, first)) <= thr) ++first;
  expect(static_cast<size_t>(out.trimmedFrom) == first, "the trim cuts exactly the silence before the first sound");
  bool same = true;
  for (size_t i = 0; same && i + 300 < out.audio.frames; ++i) same = out.audio.at(0, i) == full.audio.at(0, i + first);
  expect(same, "the trimmed one-shot is the render from its first sound");
  const float endThr = static_cast<float>(std::pow(10.0, -60.0 / 20.0));
  bool quietAfter = true;
  for (size_t i = first + out.audio.frames; i < full.audio.frames; ++i)
    quietAfter = quietAfter && std::fabs(full.audio.at(0, i)) <= endThr && std::fabs(full.audio.at(1, i)) <= endThr;
  expect(quietAfter, "the one-shot ends where the sound falls under -60 dB");
  expect(out.rootNote == f.s.pattern(f.pattern)->notes.at(f.lead)[2].pitch, "the root key is the first note's");
  expect(sr::sourceName(f.s, src).rfind("Lead one-shot ", 0) == 0, "a one-shot's name");
  // The untrimmed render is those notes alone: the same notes as a pattern of their own export the same.
  Song ref = freeze::renderSong(f.s, freeze::channelUnit(f.lead));
  Pattern* p = ref.pattern(f.pattern);
  std::vector<Note> two{p->notes[f.lead][2], p->notes[f.lead][3]};
  const Tick first0 = two[0].start;
  for (Note& n : two) n.start -= first0;
  p->notes[f.lead] = two;
  p->steps.clear();
  RenderJob j;
  j.sampleRate = kRate;
  j.scope = RenderScope::Range;
  j.start = 0;
  j.end = two[1].start + two[1].length;
  j.tail = TailMode::Leave;
  j.maxTailSeconds = 4;
  j.arrangementOnly = true;
  ref.playlist.clear();
  Clip c;
  c.id = ref.allocateId();
  c.pattern = p->id;
  c.length = j.end;
  ref.playlist.push_back(c);
  ref.live.areaEnd = 0;
  expect(maxDiff(full.audio, renderAudio(ref, j).audio) == 0.0, "the untrimmed one-shot: a normal export of just those notes");
}

void testTailsLoopsBlocks() {
  Fixture f = fixture();
  const sr::Source src = sr::channelSource(f.pattern, f.lead);
  sr::Options o = opts(sr::Through::ChannelFx);
  sr::Output cut = sr::render(f.s, src, o);
  expect(static_cast<int64_t>(cut.audio.frames) == cut.bodyFrames, "Cut: the file ends at the span");
  o.tail = TailMode::Leave;
  sr::Output leave = sr::render(f.s, src, o);
  expect(leave.bodyFrames == cut.bodyFrames && leave.audio.frames >= cut.audio.frames, "Leave: the tail rings on after the span");
  o.tail = TailMode::Wrap;
  sr::Output wrap = sr::render(f.s, src, o);
  expect(static_cast<int64_t>(wrap.audio.frames) == cut.bodyFrames, "Wrap: the span's length, the tail folded into the start");
  o.tail = TailMode::Cut;
  o.loops = 2;
  sr::Output two = sr::render(f.s, src, o);
  expect(two.bodyFrames == 2 * cut.bodyFrames || std::llabs(two.bodyFrames - 2 * cut.bodyFrames) <= 1, "two loops: twice the span");
  expect(two.notes.size() == 2 * cut.notes.size(), "two loops: every onset twice");
  o.loops = 1;
  o.blockSize = 64;
  sr::Output small = sr::render(f.s, src, o);
  o.blockSize = 1024;
  sr::Output big = sr::render(f.s, src, o);
  expect(maxDiff(small.audio, cut.audio) == 0.0 && maxDiff(big.audio, cut.audio) == 0.0, "the same render for any block size");
}

void testNotRenderable() {
  Fixture f = fixture();
  std::string why;
  Pattern empty;
  empty.id = f.s.allocateId();
  f.s.patterns.push_back(empty);
  expect(!sr::renderable(f.s, sr::channelSource(empty.id, f.kick), &why) && !why.empty(), "an empty row does not render");
  expect(!sr::renderable(f.s, sr::notesSource(f.pattern, f.lead, {}), &why), "no notes, no one-shot");
  sr::Output out = sr::render(f.s, sr::patternSource(empty.id), opts(sr::Through::Mixer));
  expect(!out.error.empty() && out.audio.frames == 0, "an empty pattern renders nothing, with a reason");
  expect(sr::sourceHitCount(f.s, sr::channelSource(f.pattern, f.kick)) == 7, "the Kick row has seven hits");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k254src");
  testChannelThroughMixer();
  testChannelOwnEffects();
  testDry();
  testClipAndPattern();
  testOneShot();
  testTailsLoopsBlocks();
  testNotRenderable();
  return finish();
}
