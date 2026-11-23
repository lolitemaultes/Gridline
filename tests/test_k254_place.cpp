// Gridline: K-254, render MIDI to audio: where a render lands (engine/SourceRender.h place()).
//  * every destination that edits the song is one undo step, and undo gives the song back exactly;
//  * the written file holds the render sample for sample, tagged as a loop or a one-shot (with
//    its tempo), and a second render of the same thing is the same file;
//  * slicer (Kerf): one slice per hit at the hit's frame, notes at the hits' ticks in the source
//    pattern, slices capped at 64, beats and transients; a clip's chops go in a pattern of their own;
//  * one-shot sampler: the root key is the first note's; the channel sits after the source row;
//  * through the channel's own effects the new channel takes the row's volume, pan and mixer track;
//  * replace in place: a clip keeps its track, start, length and mute; a row's steps give way to
//    one sampler note; every clip of a pattern becomes a looping audio clip;
//  * an audio clip lands on a free track at the source's start, long enough for the tail;
//  * drum pad: the next free pad of the drum pad channel takes the sample and the name;
//  * mute the source: rows, notes and clips fall silent.
#include <cmath>
#include <filesystem>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "engine/LoopFile.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "engine/Slicing.h"
#include "engine/SourceRender.h"

using namespace gl;
namespace sr = gl::srcrender;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;
std::string gDir;

struct Fixture {
  Song s;
  ObjectId kick = kNoId, lead = kNoId, pattern = kNoId, clipA = kNoId, clipB = kNoId;
};

Fixture fixture(int bars = 2, int every = 0) {
  Fixture f;
  Song& s = f.s;
  s = makeEmptySong();
  s.tempo = 128;
  Channel k;
  k.id = s.allocateId();
  k.name = "Kick";
  k.plugin = defaultPluginState("gl.synth");
  k.mixerTrack = s.mixer.size() > 2 ? s.mixer[2].id : s.masterId();
  k.gain = 0.5f;
  k.pan = 0.25f;
  s.channels.push_back(k);
  f.kick = k.id;
  Channel l;
  l.id = s.allocateId();
  l.name = "Lead";
  l.plugin = defaultPluginState("gl.synth");
  s.channels.push_back(l);
  f.lead = l.id;
  Pattern p;
  p.id = s.allocateId();
  p.name = "Break A";
  p.bars = bars;
  std::vector<uint8_t> steps(static_cast<size_t>(16 * bars), 0);
  if (every > 0)
    for (size_t i = 0; i < steps.size(); i += static_cast<size_t>(every)) steps[i] = 100;
  else
    for (int i : {0, 6, 10, 16, 22, 26, 30}) steps[static_cast<size_t>(i)] = 100;
  p.steps[k.id] = steps;
  for (int n = 0; n < 4; ++n) p.notes[l.id].push_back(Note(n * s.beatTicks(), s.beatTicks() / 2, 38 + n, 90));
  s.patterns.push_back(p);
  f.pattern = p.id;
  Clip a;
  a.id = s.allocateId();
  a.pattern = p.id;
  a.track = 1;
  a.start = s.barTicks();
  s.playlist.push_back(a);
  f.clipA = a.id;
  Clip b = a;
  b.id = s.allocateId();
  b.start = 5 * s.barTicks();
  b.length = 3 * s.barTicks();
  b.muted = true;
  s.playlist.push_back(b);
  f.clipB = b.id;
  return f;
}

sr::Output renderOf(const Song& s, const sr::Source& src) {
  sr::Options o = sr::defaultOptions(src);
  o.sampleRate = kRate;
  return sr::render(s, src, o);
}

// Places in one undo step; checks the step and that undo gives the song back.
sr::Placed placeChecked(Document& doc, const sr::Source& src, const sr::Output& out, const std::string& path, const sr::PlaceOptions& o,
                        const char* what) {
  const Song before = doc.song();
  const size_t steps = doc.history().labels().size();
  sr::Placed r;
  doc.editSong("render to audio", [&](Song& x) { r = sr::place(x, src, out, path, o); });
  expect(doc.history().labels().size() == steps + 1, (std::string(what) + ": one undo step").c_str());
  const Song after = doc.song();
  doc.undo();
  expect(doc.song() == before, (std::string(what) + ": undo gives the song back").c_str());
  doc.redo();
  expect(doc.song() == after, (std::string(what) + ": redo places it again").c_str());
  return r;
}

void testFile() {
  Fixture f = fixture();
  const sr::Source src = sr::channelSource(f.pattern, f.kick);
  sr::Output out = renderOf(f.s, src);
  std::string err;
  const std::string path = sr::writeRender(out, gDir, &err);
  expect(!path.empty() && fs::exists(path), "the render is written");
  SamplePtr sp = loadSample(path);
  bool same = sp && sp->frames() == out.audio.frames;
  for (size_t i = 0; same && i < out.audio.frames; ++i) same = sp->left[i] == out.audio.at(0, i) && sp->right[i] == out.audio.at(1, i);
  expect(same, "the file holds the render sample for sample (32-bit float)");
  loopfile::Info li;
  expect(loopfile::read(path, li) && li.loop && std::fabs(li.bpm - 128) < 0.01, "a sequence is tagged as a loop with its tempo");
  expect(sr::writeRender(renderOf(f.s, src), gDir) == path, "the same render twice is the same file");
  sr::Output one = renderOf(f.s, sr::notesSource(f.pattern, f.lead, {0}));
  const std::string op = sr::writeRender(one, gDir);
  loopfile::Info oi;
  loopfile::read(op, oi);
  expect(!op.empty() && !oi.loop, "a one-shot is tagged as a one-shot");
}

void testSlicer() {
  Fixture f = fixture();
  Document doc(f.s);
  const sr::Source src = sr::channelSource(f.pattern, f.kick);
  sr::Output out = renderOf(doc.song(), src);
  const std::string path = sr::writeRender(out, gDir);
  sr::PlaceOptions o;
  o.dest = sr::Dest::Slicer;
  o.muteSource = true;
  sr::Placed r = placeChecked(doc, src, out, path, o, "slicer");
  const Song& s = doc.song();
  const Channel* c = s.channel(r.channel);
  expect(c && c->plugin.type == "gl.slicer" && c->plugin.data == path, "a new slicer channel plays the render");
  expect(r.slices == 7, "one slice per step hit");
  expect(s.channelIndex(r.channel) == s.channelIndex(f.kick) + 1, "the slicer row sits below the Kick row");
  bool marks = c != nullptr;
  for (int k = 0; marks && k < 7; ++k)
    marks = std::fabs(c->plugin.params[static_cast<size_t>(pp::slMarker(k))] - static_cast<double>(out.hits[static_cast<size_t>(k)].frame) / static_cast<double>(out.audio.frames)) < 1e-6;
  expect(marks && c->plugin.params[static_cast<size_t>(pp::slMarker(7))] < 0, "the markers sit at the hits");
  const auto& ns = s.pattern(f.pattern)->notes.at(r.channel);
  bool ticks = ns.size() == 7;
  for (size_t k = 0; ticks && k < 7; ++k) ticks = ns[k].start == out.hits[k].tick || std::llabs(ns[k].start - out.hits[k].tick) <= 1;
  expect(ticks, "the slicer's notes play the slices at the hits, in the same pattern");
  expect(s.channel(f.kick)->muted, "the source row is muted");
  // Beats; and more hits than a slicer has slices.
  sr::PlaceOptions b = o;
  b.sliceAt = sr::SliceAt::Beats;
  b.muteSource = false;
  sr::Placed rb = placeChecked(doc, src, out, path, b, "slicer at beats");
  expect(rb.slices == 8, "two bars: eight beats");
  Fixture g = fixture(8, 1);
  Document d2(g.s);
  const sr::Source many = sr::channelSource(g.pattern, g.kick);
  sr::Output mo = renderOf(d2.song(), many);
  sr::Placed rm = placeChecked(d2, many, mo, sr::writeRender(mo, gDir), o, "slicer with many hits");
  expect(rm.slices == pp::kMaxSlices, "at most 64 slices");
  // Transients of the audio itself.
  b.sliceAt = sr::SliceAt::Transients;
  const std::vector<int64_t> tr = sr::slicePoints(out, sr::SliceAt::Transients, 0.5);
  expect(tr.size() >= 2 && tr.front() == 0, "transients cut the render");
  // A clip's chops: a pattern of their own, on a free track at the clip's start.
  const sr::Source cs = sr::clipSource(f.clipA);
  sr::Output co = renderOf(doc.song(), cs);
  sr::Placed rc = placeChecked(doc, cs, co, sr::writeRender(co, gDir), o, "slicer from a clip");
  const Clip* pc = doc.song().clip(rc.clip);
  expect(rc.pattern != kNoId && pc && pc->pattern == rc.pattern && pc->start == doc.song().clip(f.clipA)->start && pc->track != 1,
         "a clip's chops play from the clip's start on a free track");
  expect(doc.song().clip(f.clipA)->muted, "the clip is muted");
}

void testOneShotAndStrip() {
  Fixture f = fixture();
  Document doc(f.s);
  const sr::Source src = sr::notesSource(f.pattern, f.lead, {1});
  sr::Output out = renderOf(doc.song(), src);
  sr::PlaceOptions o;
  o.dest = sr::Dest::OneShot;
  sr::Placed r = placeChecked(doc, src, out, sr::writeRender(out, gDir), o, "one-shot sampler");
  const Channel* c = doc.song().channel(r.channel);
  expect(c && c->plugin.type == "gl.sampler", "a new sampler channel");
  expect(c && static_cast<int>(c->plugin.params[pp::SmpRoot]) == 39, "its root key is the note's");
  expect(c && c->plugin.params[pp::SmpOneShot] > 0.5f, "it plays as a one-shot");
  expect(doc.song().channelIndex(r.channel) == doc.song().channelIndex(f.lead) + 1, "below the source row");
  // The Kick through its own effects: the new channel plays through the Kick's strip.
  const sr::Source ks = sr::channelSource(f.pattern, f.kick);
  sr::Output ko = renderOf(doc.song(), ks);
  sr::Placed rk = placeChecked(doc, ks, ko, sr::writeRender(ko, gDir), o, "one-shot from a row");
  const Channel* kc = doc.song().channel(rk.channel);
  const Channel* kick = doc.song().channel(f.kick);
  expect(kc && kick && kc->mixerTrack == kick->mixerTrack && kc->gain == kick->gain && kc->pan == kick->pan,
         "through its own effects: the row's volume, pan and mixer track");
  // Through the mixer: straight to the master at unity.
  sr::Options mo = sr::defaultOptions(ks);
  mo.through = sr::Through::Mixer;
  mo.sampleRate = kRate;
  sr::Output mx = sr::render(doc.song(), ks, mo);
  sr::Placed rm = placeChecked(doc, ks, mx, sr::writeRender(mx, gDir), o, "one-shot through the mixer");
  const Channel* mc = doc.song().channel(rm.channel);
  expect(mc && mc->mixerTrack == doc.song().masterId() && mc->gain == Channel{}.gain, "through the mixer: the master at unity");
}

void testReplace() {
  Fixture f = fixture();
  Document doc(f.s);
  // A clip: an audio clip in its place.
  const sr::Source cs = sr::clipSource(f.clipB);
  sr::Output co = renderOf(doc.song(), cs);
  sr::PlaceOptions o;
  o.dest = sr::Dest::Replace;
  const Clip old = *doc.song().clip(f.clipB);
  sr::Placed r = placeChecked(doc, cs, co, sr::writeRender(co, gDir), o, "replace a clip");
  const Clip* c = doc.song().clip(r.clip);
  expect(!doc.song().clip(f.clipB), "the pattern clip is gone");
  expect(c && c->kind == ClipKind::Audio && c->track == old.track && c->start == old.start && c->length == doc.song().clipLength(old) && c->muted,
         "the audio clip keeps the clip's track, start, length and mute");
  // A row: one sampler note where its steps were.
  const sr::Source rs = sr::channelSource(f.pattern, f.kick);
  sr::Output ro = renderOf(doc.song(), rs);
  sr::Placed rr = placeChecked(doc, rs, ro, sr::writeRender(ro, gDir), o, "replace a row");
  const Pattern* p = doc.song().pattern(f.pattern);
  expect(!p->steps.count(f.kick) && p->notes.count(rr.channel) && p->notes.at(rr.channel).size() == 1 && p->notes.at(rr.channel)[0].start == 0,
         "the row's steps give way to one note of the rendered sampler");
  // A pattern: every clip becomes an audio clip of the render.
  const sr::Source ps = sr::patternSource(f.pattern);
  sr::Output po = renderOf(doc.song(), ps);
  int clipsBefore = 0;
  for (const auto& k : doc.song().playlist) clipsBefore += k.kind == ClipKind::Pattern && k.pattern == f.pattern;
  sr::Placed rp = placeChecked(doc, ps, po, sr::writeRender(po, gDir), o, "replace a pattern");
  int left = 0;
  for (const auto& k : doc.song().playlist) left += k.kind == ClipKind::Pattern && k.pattern == f.pattern;
  expect(left == 0 && static_cast<int>(rp.clips.size()) == clipsBefore, "every clip of the pattern becomes an audio clip");
}

void testAudioClipAndPads() {
  Fixture f = fixture();
  Document doc(f.s);
  const sr::Source src = sr::channelSource(f.pattern, f.lead);
  sr::Options lo = sr::defaultOptions(src);
  lo.sampleRate = kRate;
  lo.tail = TailMode::Leave;
  sr::Output out = sr::render(doc.song(), src, lo);
  sr::PlaceOptions o;
  o.dest = sr::Dest::AudioClip;
  sr::Placed r = placeChecked(doc, src, out, sr::writeRender(out, gDir), o, "audio clip");
  const Clip* c = doc.song().clip(r.clip);
  expect(c && c->kind == ClipKind::Audio && c->start == doc.song().clip(f.clipA)->start, "at the pattern's first clip");
  bool free = c != nullptr;
  for (const auto& k : doc.song().playlist)
    if (c && k.id != c->id && k.track == c->track) free = free && (k.start >= c->start + c->length || k.start + doc.song().clipLength(k) <= c->start);
  expect(free, "on a free track");
  RenderJob j;
  j.scope = RenderScope::Pattern;
  expect(c && scopeFrames(doc.song(), j, 0, c->length, kRate) >= static_cast<int64_t>(out.audio.frames) - 2, "long enough for the tail");
  // Drum pads: no channel, then a channel with a free pad.
  o.dest = sr::Dest::DrumPad;
  sr::Placed none;
  Song tmp = doc.song();
  none = sr::place(tmp, src, out, "x.wav", o);
  expect(!none.error.empty(), "no drum pad channel: a reason, nothing placed");
  Channel pads;
  pads.id = doc.song().peekId();
  doc.editSong("add pads", [&](Song& x) {
    Channel d;
    d.id = x.allocateId();
    d.name = "Drum Kit";
    d.plugin = defaultPluginState("gl.pads");
    x.channels.push_back(d);
    pads.id = d.id;
  });
  const std::string path = sr::writeRender(out, gDir);
  const int pad = sr::freePad(doc.song(), pads.id);
  sr::Placed rp = placeChecked(doc, src, out, path, o, "drum pad");
  const Channel* pc = doc.song().channel(pads.id);
  expect(pad >= 0 && rp.pad == pad && pc && padSamplePath(pc->plugin.data, pad, 0) == path, "the next free pad takes the render");
  expect(pc && padDataGet(pc->plugin.data, "N" + std::to_string(pad + 1)) == out.name, "the pad takes the render's name");
  expect(sr::freePad(doc.song(), pads.id) != pad, "that pad is no longer free");
}

void testMuteNotes() {
  Fixture f = fixture();
  Document doc(f.s);
  const sr::Source src = sr::notesSource(f.pattern, f.lead, {0, 2});
  sr::Output out = renderOf(doc.song(), src);
  sr::PlaceOptions o;
  o.dest = sr::Dest::OneShot;
  o.muteSource = true;
  placeChecked(doc, src, out, sr::writeRender(out, gDir), o, "one-shot, notes muted");
  const auto& ns = doc.song().pattern(f.pattern)->notes.at(f.lead);
  expect(ns[0].muted && !ns[1].muted && ns[2].muted && !ns[3].muted, "only the selected notes are muted");
  sr::PlaceOptions l;
  l.dest = sr::Dest::Library;
  Song x = doc.song();
  sr::place(x, src, out, "y.wav", l);
  expect(x == doc.song(), "library, Loupe and file leave the song alone");
}

}  // namespace

int main() {
  const std::string& tmp = gl::test::hermeticTestEnv("k254place");
  gDir = tmp + "/Rendered";
  fs::create_directories(gDir);
  testFile();
  testSlicer();
  testOneShotAndStrip();
  testReplace();
  testAudioClipAndPads();
  testMuteNotes();
  return finish();
}
