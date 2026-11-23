// Gridline: the Patch Player in a collaboration session. Two in-process collaborators (the
// collab_local pattern, messages delayed and reordered in memory):
//  * a Patch Player on an SF2 file reaches the other computer, the file with it;
//  * a patch chosen in the window (one edit), the Bank and Patch parameters and a note's program
//    change (the Tracker's I effect) each reach the other side;
//  * both computers render the same audio once the edits arrive;
//  * each undo takes back only its own person's change, for both.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Params.h"
#include "engine/Render.h"
#include "packs/samplers/patchplayer.h"
#include "packs/sf2_bank.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;
using namespace gl::packs::samplers;

namespace {

// Programs 0, 1, 40 in bank 0 and a kit (128:0), each a DC level of its own.
std::string makeFont(const std::string& dir) {
  using namespace sf2t;
  Bank b;
  b.name = "Levels";
  for (int i = 0; i < 4; ++i)
    b.samples.push_back(sf2t::Sample{"L" + std::to_string(i), dc(4410, static_cast<int16_t>(4096 * (i + 1))), {}, {}, 44100, 60, 0, 0, 4410, 1, 0});
  for (int i = 0; i < 4; ++i)
    b.insts.push_back(Inst{"I" + std::to_string(i), {sf2t::Zone{}.g(AttackVolEnv, -12000).g(ReleaseVolEnv, -12000).g(SampleModes, 1).g(SampleId, i)}});
  b.presets = {Preset{"One", 0, 0, {sf2t::Zone{}.g(Instrument, 0)}}, Preset{"Two", 1, 0, {sf2t::Zone{}.g(Instrument, 1)}},
               Preset{"Strings", 40, 0, {sf2t::Zone{}.g(Instrument, 2)}}, Preset{"Kit", 0, 128, {sf2t::Zone{}.g(Instrument, 3)}}};
  const std::string p = dir + "/levels.sf2";
  sf2t::writeFile(p, b.build());
  return p;
}

struct Pair {
  Clock clock;
  Net net{348};
  Peer a, b;
  Pair() {
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    a.doc.reset(makeEmptySong());
    a.folder = tmpDir("k348_a");
    b.folder = tmpDir("k348_b");
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock));
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s));
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle(net, {&a, &b}, clock);
  }
  void sync() { settle(net, {&a, &b}, clock); }
};

const Channel* chan(const Peer& p, ObjectId id) { return p.doc.song().channel(id); }

// The song as text with each Patch Player's file named by its file name only (each computer keeps
// the shared file in its own project folder).
std::string comparable(const Peer& p) {
  Song s = p.doc.song();
  s.nextId = 0;
  for (auto& c : s.channels)
    if (c.plugin.type == kPatchPlayerType) {
      PatchRef r = parsePatchRef(c.plugin.data);
      r.path = std::filesystem::path(r.path).filename().string();
      c.plugin.data = serializePatchRef(r);
    }
  return jsonStringify(songToJson(s), 0);
}

bool same(Pair& t) {
  const bool ok = comparable(t.a) == comparable(t.b) && t.a.s->versionVector() == t.b.s->versionVector();
  if (!ok) {
    std::ofstream("collab_test_tmp/k348_a.json") << comparable(t.a);
    std::ofstream("collab_test_tmp/k348_b.json") << comparable(t.b);
  }
  return ok;
}

AudioBuffer render(const Peer& p) {
  RenderJob job;
  job.scope = RenderScope::Pattern;
  job.pattern = p.doc.song().patterns[0].id;
  job.tail = TailMode::Cut;
  return renderAudio(p.doc.song(), job).audio;
}

float diff(const AudioBuffer& x, const AudioBuffer& y) {
  if (x.frames != y.frames || x.data.size() != y.data.size()) return 1e9f;
  float d = 0;
  for (size_t i = 0; i < x.data.size(); ++i) d = std::max(d, std::fabs(x.data[i] - y.data[i]));
  return d;
}

float peakOf(const AudioBuffer& x) {
  float p = 0;
  for (float v : x.data) p = std::max(p, std::fabs(v));
  return p;
}

}  // namespace

int main() {
  Pair t;
  expect(t.b.s->ready(), "the collaborator has the project");
  const std::string font = makeFont(tmpDir("k348_media"));

  // A adds a Patch Player and chooses its patch (the window's one edit).
  const ObjectId id = t.a.doc.addChannel("Keys", kPatchPlayerType);
  t.a.doc.editChannel(id, "change patch", [&](Channel& c) { setPatch(c.plugin, font, 0, 1, "Two"); });
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  {
    std::vector<Note> v;
    for (int k = 0; k < 3; ++k) v.push_back(Note(k * t.a.doc.song().ppq, t.a.doc.song().ppq - 6, 60, 100));
    t.a.doc.setNotes(pat, id, v, "notes");
  }
  t.sync();
  std::string fileB;
  for (int i = 0; i < 2000; ++i) {
    const Channel* cb = chan(t.b, id);
    fileB = cb ? parsePatchRef(cb->plugin.data).path : std::string();
    if (!fileB.empty() && std::filesystem::exists(fileB) && !t.b.s->fileLoading(fileB)) break;
    run(t.net, {&t.a, &t.b}, t.clock, 0.02);
  }
  const Channel* cb = chan(t.b, id);
  expect(cb && cb->plugin.type == kPatchPlayerType && parsePatchRef(cb->plugin.data).program == 1, "the Patch Player and its patch reach the collaborator");
  expect(!fileB.empty() && std::filesystem::exists(fileB) && samePath(std::filesystem::path(fileB).parent_path().string(), t.b.folder + "/Audio"),
         "the SF2 file reaches the collaborator's project");
  expect(same(t), "both have the same project");
  const AudioBuffer first = render(t.a);
  expect(peakOf(first) > 1e-4f && diff(first, render(t.b)) < 1e-6f, "both computers play the same patch");

  // A chooses another patch (preset): one edit, synced.
  t.a.doc.editChannel(id, "change patch", [&](Channel& c) { setPatch(c.plugin, parsePatchRef(c.plugin.data).path, 0, 40, "Strings"); });
  t.sync();
  expect(parsePatchRef(chan(t.b, id)->plugin.data).program == 40 && same(t), "a patch chosen in the window reaches the collaborator");

  // B moves the Bank parameter to the drum bank, A the Patch parameter (program 1): both arrive.
  t.b.doc.setParam(channelPluginParam(id, PpBank), 129.0, false);
  t.b.doc.endGesture();
  t.a.doc.setParam(channelPluginParam(id, PpPatch), 2.0, false);
  t.a.doc.endGesture();
  t.sync();
  expect(chan(t.a, id)->plugin.params[PpBank] == 129.f && chan(t.b, id)->plugin.params[PpPatch] == 2.f && same(t),
         "the Bank and Patch parameters reach the other side");
  t.b.doc.setParam(channelPluginParam(id, PpBank), 0.0, false);  // back to the chosen bank
  t.b.doc.endGesture();
  t.sync();

  // B gives the second note a program change (the Tracker's I effect on it).
  {
    std::vector<Note> v = t.b.doc.song().pattern(pat)->notes.at(id);
    v[1].program = 40;
    t.b.doc.setNotes(pat, id, v, "effect");
  }
  t.sync();
  expect(t.a.doc.song().pattern(pat)->notes.at(id)[1].program == 40 && same(t), "a note's program change reaches the collaborator");
  const AudioBuffer ra = render(t.a), rb = render(t.b);
  expect(diff(ra, rb) < 1e-6f && diff(ra, first) > 1e-3f, "both computers play the program changes the same way");

  // Undo: each takes back only their own change, for both.
  t.b.doc.undo();  // B: the note's program change
  t.sync();
  expect(t.a.doc.song().pattern(pat)->notes.at(id)[1].program == -1 && chan(t.a, id)->plugin.params[PpPatch] == 2.f && same(t),
         "B's undo takes back the program change only, for both");
  t.a.doc.undo();  // A: the Patch parameter
  t.sync();
  expect(chan(t.b, id)->plugin.params[PpPatch] == 0.f && chan(t.b, id)->plugin.params[PpBank] == 0.f &&
             parsePatchRef(chan(t.b, id)->plugin.data).program == 40 && same(t),
         "A's undo takes back the Patch parameter, for both");
  t.a.doc.undo();  // A: the patch chosen in the window
  t.sync();
  expect(parsePatchRef(chan(t.b, id)->plugin.data).program == 1 && same(t), "undo of a patch choice reaches the collaborator");
  expect(diff(render(t.a), first) < 1e-6f && diff(render(t.b), first) < 1e-6f, "after the undos both play as at the start");
  return finish();
}
