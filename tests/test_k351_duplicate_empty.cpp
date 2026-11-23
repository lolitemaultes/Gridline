// Gridline - K-351: "Duplicate without notes" for Clips' and Smooth's tracks
// (trackmodes::duplicateTracks with withContent false).
//  * an instrument track: the copy comes right after it (after its automation lane), an instrument
//    track of a new channel of the same instrument with its own mixer track and the same effects,
//    and no clips; no pattern gains notes; the original keeps its clips and notes; one undo step;
//  * an audio track: a copy with its own mixer track and no clips;
//  * Duplicate (with the content) is as before: the copy has the clips;
//  * the edit reaches a collaborator, and one undo takes it back for both.
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/TrackModes.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

int clipsOn(const Song& s, int track) {
  int n = 0;
  for (const Clip& c : s.playlist) n += c.track == track ? 1 : 0;
  return n;
}

size_t notesOf(const Song& s, ObjectId chan) {
  size_t n = 0;
  for (const Pattern& p : s.patterns)
    if (auto it = p.notes.find(chan); it != p.notes.end()) n += it->second.size();
  return n;
}

struct Rig {
  ObjectId lead = kNoId, pat = kNoId, mix = kNoId;
  void build(Document& d) {
    d.reset(makeEmptySong());
    lead = d.addChannel("Lead", "gl.synth");
    pat = d.song().patterns[0].id;
    d.editPattern(pat, "notes", [&](Pattern& p) {
      for (int i = 0; i < 4; ++i) {
        Note n;
        n.start = i * 96, n.length = 96, n.pitch = 60 + i;
        p.notes[lead].push_back(n);
      }
    });
    std::string why;
    trackmodes::makeInstrumentTrack(d, 0, lead, &why);
    trackmodes::makeAudioTrack(d, 1, kNoId, &why);
    d.addClip(pat, 0, 0);
    d.addClip(pat, 0, 384);
    mix = trackmodes::linkedMixer(d.song(), 0) ? trackmodes::linkedMixer(d.song(), 0)->id : kNoId;
    if (mix != kNoId && mix != d.song().masterId())
      d.editMixerTrack(mix, "fx", [](MixerTrack& m) { m.slots[0].plugin = defaultPluginState("gl.reverb"); });
  }
};

void testInstrumentTrack() {
  Document d;
  Rig r;
  r.build(d);
  const Song s0 = d.song();
  const size_t undo0 = d.history().position();
  std::string why;
  const int t = trackmodes::duplicateTracks(d, {0}, &why, false);
  const Song& s = d.song();
  expect(t == 1, ("the copy comes right after the track: " + why).c_str());
  const Channel* c = trackmodes::linkedChannel(s, t);
  expect(c && c->id != r.lead && c->plugin.type == "gl.synth", "the copy plays a new channel of the same instrument");
  expect(s.channels.size() == s0.channels.size() + 1, "one more channel");
  expect(clipsOn(s, t) == 0, "the copy has no clips");
  expect(clipsOn(s, 0) == 2, "the original keeps its clips");
  expect(s.patterns.size() == s0.patterns.size(), "no pattern is added");
  expect(c && notesOf(s, c->id) == 0, "the copy has no notes in any pattern");
  expect(notesOf(s, r.lead) == 4, "the original keeps its notes");
  const MixerTrack* m0 = trackmodes::linkedMixer(s, 0);
  const MixerTrack* m1 = trackmodes::linkedMixer(s, t);
  expect(m0 && m1 && m0->id != m1->id, "the copy has its own mixer track");
  expect(m1 && m1->slots[0].plugin.type == "gl.reverb", "with the same effects");
  expect(trackmodes::modeOf(s, 2) == TrackMode::Audio && clipsOn(s, 2) == 0, "the audio track moved down after the copy");
  expect(d.history().position() == undo0 + 1, "one undo step");
  d.undo();
  expect(d.song().channels.size() == s0.channels.size() && d.song().tracks[1].mode == TrackMode::Audio && clipsOn(d.song(), 0) == 2,
         "undo takes the copy away");
  // Duplicate with the content, as before.
  const int t2 = trackmodes::duplicateTracks(d, {0}, &why);
  expect(t2 == 1 && clipsOn(d.song(), t2) == 2, "Duplicate (with the content) still copies the clips");
}

void testAudioTrack() {
  Document d;
  Rig r;
  r.build(d);
  std::string why;
  const ObjectId m1 = trackmodes::linkedMixer(d.song(), 1)->id;
  const int t = trackmodes::duplicateTracks(d, {1}, &why, false);
  const Song& s = d.song();
  expect(t == 2 && trackmodes::modeOf(s, t) == TrackMode::Audio, ("an audio track's copy is an audio track: " + why).c_str());
  const MixerTrack* m = trackmodes::linkedMixer(s, t);
  expect(m && m->id != m1 && clipsOn(s, t) == 0, "with its own mixer track and no clips");
}

void testCollab() {
  Clock clock;
  Net net(7851);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  Rig r;
  r.build(a.doc);
  a.folder = tmpDir("k351_dup_a"), b.folder = tmpDir("k351_dup_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const size_t chans = b.doc.song().channels.size();
  std::string why;
  expect(trackmodes::duplicateTracks(a.doc, {0}, &why, false) == 1, "duplicated without notes");
  settle(net, {&a, &b}, clock);
  const Channel* c = trackmodes::linkedChannel(b.doc.song(), 1);
  expect(c && c->id != r.lead && b.doc.song().channels.size() == chans + 1 && clipsOn(b.doc.song(), 1) == 0,
         "the collaborator gets the copy, with no clips");
  expect(songText(a) == songText(b), "both have the same song");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().channels.size() == chans && songText(a) == songText(b), "one undo takes it back for both");
}

}  // namespace

int main() {
  testInstrumentTrack();
  testAudioTrack();
  testCollab();
  return finish();
}
