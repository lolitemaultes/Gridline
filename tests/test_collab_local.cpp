// Gridline - J-111: sound and playback stay on each computer. Two
// collaborators with real audio engines (rendered offline):
//  * A plays (Seltmann's own pattern, held keys, the transport), solos, arms,
//    sets input monitoring, launches a live clip, folds a track and turns a
//    plugin's listening aid on: no op is made, B's song and B's engine stay
//    as they were (silent, stopped, no plugin playback), and A keeps all of it
//    when B's changes come in;
//  * a finished recording reaches B as a placeholder, then the clip;
//  * A adds notes, turns a knob, loads a sample while B is stopped: B is silent;
//  * A deletes the channel B is auditioning or playing its own pattern on, or
//    replaces the sample of B's running pattern: B releases cleanly.
#include <cmath>
#include <cstdio>
#include <memory>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/LocalState.h"
#include "core/AudioBuffer.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "packs/dynamics/dynamics.h"
#include "packs/seltmann/seltmann.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;
namespace sm = gl::packs::seltmann;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

struct Rig {
  ProgramBuilder builder;
  AudioEngine eng{kSr};
  uint64_t builtRev = ~0ull;
  float bl[kBlock], br[kBlock];
  Rig() { builder.setSampleRate(kSr); }
  // what App::syncEngine does
  void sync(const Document& d) {
    if (d.revision() == builtRev) return;
    builtRev = d.revision();
    eng.setProgram(builder.build(d.song()));
  }
  float run(double seconds) {
    float peak = 0;
    for (int done = 0; done < static_cast<int>(seconds * kSr); done += kBlock) {
      eng.process(bl, br, kBlock);
      for (int i = 0; i < kBlock; ++i) peak = std::max(peak, std::max(std::fabs(bl[i]), std::fabs(br[i])));
    }
    return peak;
  }
};

std::string writeBreak(const std::string& path, float freq) {
  const int sr = 44100, n = sr * 4;
  AudioBuffer b(2, static_cast<size_t>(n));
  for (int k = 0; k < 16; ++k) {
    const int at = k * sr / 4;
    for (int i = 0; i < sr / 5 && at + i < n; ++i) {
      const float v = 0.6f * std::exp(-static_cast<float>(i) / (sr * 0.03f)) * std::sin(static_cast<float>(i) * freq);
      b.data[static_cast<size_t>(at + i)] = v;
      b.data[static_cast<size_t>(n + at + i)] = v;
    }
  }
  writeWav(path, b, sr);
  return path;
}

struct Pair {
  Clock clock;
  Net net{77};
  Peer a, b;
  Rig ra, rb;
  ObjectId kick = kNoId, selt = kNoId, selt2 = kNoId, ins = kNoId, clip = kNoId;
  Pair() {
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    const std::string media = tmpDir("local_media");
    a.doc.reset(makeEmptySong());
    kick = a.doc.addChannel("Kick", "gl.kick");
    selt = a.doc.addChannel("Break", sm::kType);
    selt2 = a.doc.addChannel("Break 2", sm::kType);
    for (ObjectId ch : {selt, selt2})
      a.doc.editChannel(ch, "", [&](Channel& c) {
        c.plugin.data = writeBreak(media + (ch == selt ? "/break.wav" : "/break2.wav"), ch == selt ? 0.05f : 0.07f);
        c.plugin.params[sm::SmSync] = sm::SyncOriginal;
        c.plugin.params[sm::SmLock] = 0;
      });
    ins = a.doc.song().mixer[1].id;
    a.doc.setSlotPlugin(ins, 0, "gl.emphasis");
    clip = a.doc.addClip(a.doc.song().patterns[0].id, 0, 0);
    a.folder = tmpDir("local_a");
    b.folder = tmpDir("local_b");
    a.s = Session::create(a.doc, config(a.folder, a.id, "Alex", &clock));
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, "Sam", &clock), ticketOf(*a.s));
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    step(5.0);
  }
  // one frame of each app: network, collaborators' edits, own playback guard, engine rebuild
  void frame(Peer& p, Rig& r) {
    const OwnPlaySig sig = ownPlaySignature(r.eng, p.doc.song());
    const uint64_t before = p.s->incrementalApplies() + p.s->fullApplies();
    p.s->frame(net.now);
    if (p.s->incrementalApplies() + p.s->fullApplies() != before) stopOwnPlaybackIfChanged(r.eng, p.doc.song(), sig);
    r.sync(p.doc);
  }
  void step(double seconds) {
    for (double t = 0; t < seconds; t += 0.02) {
      net.now += 0.02;
      clock.ms += 20;
      net.deliver();
      frame(a, ra);
      frame(b, rb);
      ra.run(0.02);
      rb.run(0.02);
    }
  }
};

void testLocalPlayback(Pair& p) {
  const uint64_t opsA = p.a.s->opCount(), opsB = p.b.s->opCount();
  const VersionVector vvA = p.a.s->versionVector(), vvB = p.b.s->versionVector();
  // A plays: Seltmann's own pattern, a held key, the transport
  p.ra.eng.ownNoteOn(p.selt, 48, 0.9f);
  p.ra.eng.noteOn(p.kick, 60, 1.0f);
  p.ra.eng.play();
  // A's listening and recording set-up
  p.a.doc.editMixerTrack(p.ins, "solo", [](MixerTrack& m) { m.solo = true; });
  p.a.doc.editMixerTrack(p.ins, "arm", [](MixerTrack& m) {
    m.armed = true;
    m.input = 0;
    m.monitor = 1;
    m.recPoint = 1;
  });
  p.a.doc.editClip(p.clip, "launch", [](Clip& c) { c.launched = true; });
  p.a.doc.editPlaylistTrack(0, "fold", [](PlaylistTrack& t) {
    t.folded = true;
    t.height = 2.0f;
  });
  p.a.doc.setParam(slotParam(p.ins, 0, gl::packs::dynamics::EmDelta), 1.0, false);
  p.step(1.0);
  expect(p.ra.run(0.2) > 0.01f && p.ra.eng.playing() && p.ra.eng.pluginPlaybackActive(), "A is playing (transport, its own pattern, a held key)");
  expect(p.a.s->opCount() == opsA && p.b.s->opCount() == opsB && p.a.s->versionVector() == vvA && p.b.s->versionVector() == vvB,
         "none of it makes an op (playback, solo, arm, monitoring, input, launch, fold, a plugin's Delta listen)");
  const MixerTrack* mb = p.b.doc.song().mixerTrack(p.ins);
  expect(mb && !mb->solo && !mb->armed && mb->input < 0 && mb->monitor == 0 && !p.b.doc.song().clip(p.clip)->launched &&
             !p.b.doc.song().tracks[0].folded && p.b.doc.song().tracks[0].height == 1.0f &&
             mb->slots[0].plugin.params[gl::packs::dynamics::EmDelta] == 0.0f,
         "B's song is unchanged by A's session state");
  expect(p.rb.run(0.5) == 0.0f && !p.rb.eng.playing() && !p.rb.eng.pluginPlaybackActive() && p.rb.eng.liveHeld() == 0,
         "B stays silent and stopped, nothing plays on its own there");
  // a collaborator's change does not reset A's session state
  p.b.doc.renameChannel(p.kick, "Kick (Sam)");
  p.b.doc.editMixerTrack(p.ins, "volume", [](MixerTrack& m) { m.volume = 0.5f; });
  p.step(1.0);
  const MixerTrack* ma = p.a.doc.song().mixerTrack(p.ins);
  expect(p.a.doc.song().channel(p.kick)->name == "Kick (Sam)" && ma->volume == 0.5f && ma->solo && ma->armed && ma->input == 0 &&
             p.a.doc.song().clip(p.clip)->launched && p.a.doc.song().tracks[0].folded &&
             ma->slots[0].plugin.params[gl::packs::dynamics::EmDelta] == 1.0f,
         "A keeps its solo, arm, monitoring, launch, fold and Delta when B's edits arrive");
  expect(p.ra.eng.playing() && p.ra.eng.pluginPlaybackActive(), "... and keeps playing");
  p.ra.eng.stop();
  p.ra.eng.stopPluginPlayback();
  p.ra.eng.releaseAllNotes();
  p.step(0.5);
  expect(isSessionParam("gl.emphasis", gl::packs::dynamics::EmDelta) && isSessionParam("gl.tideline", 11) && !isSessionParam("gl.emphasis", 0) &&
             sessionParamList().size() > 40,
         "listening aids and gestures of the plugins are marked local (Delta, Band listen, solos, holds...)");
}

void testRemoteEditsSilent(Pair& p) {
  // B is stopped; A adds notes, turns knobs, loads a sample: nothing sounds on B
  const ObjectId pat = p.a.doc.song().patterns[0].id;
  for (int k = 0; k < 8; ++k) p.a.doc.addNote(pat, p.kick, Note(k * 24, 24, 60, 120));
  p.a.doc.setParam(channelParam(p.kick, ChannelParam::Volume), 1.0, false);
  p.a.doc.setParam(mixerParam(p.ins, MixerParam::Volume), 1.2, false);
  const ObjectId smp = p.a.doc.addChannel("Sampler", "gl.sampler");
  p.a.doc.editChannel(smp, "sample", [&](Channel& c) { c.plugin.data = writeBreak(tmpDir("local_media2") + "/one.wav", 0.09f); });
  float peak = 0;
  for (int i = 0; i < 100; ++i) {
    p.step(0.02);
    peak = std::max(peak, p.rb.run(0.0));
  }
  peak = std::max(peak, p.rb.run(0.5));
  expect(p.b.doc.song().pattern(pat)->notes.at(p.kick).size() >= 8 && p.b.doc.song().channel(smp), "B received the notes and the sample");
  expect(peak == 0.0f && !p.rb.eng.playing(), "added notes, knob moves and a loaded sample make no sound on B (no audition)");
}

void testRecordingArrives(Pair& p) {
  // a finished take: a new audio channel + clip referring to the recorded file
  const std::string take = writeBreak(tmpDir("local_take") + "/take.wav", 0.11f);
  ObjectId ch = kNoId;
  p.a.doc.editSong("Record audio", [&](Song& s) {
    Channel c;
    c.id = s.allocateId();
    c.name = "Take 1";
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = take;
    c.audioClip = true;
    ch = c.id;
    s.channels.push_back(c);
    Clip cl;
    cl.id = s.allocateId();
    cl.kind = ClipKind::Audio;
    cl.source = c.id;
    cl.track = 3;
    cl.start = 0;
    cl.length = s.barTicks() * 2;
    s.playlist.push_back(cl);
  });
  p.a.s->fileBudget = 8 * 1024;
  bool placeholder = false, arrived = false;
  for (int i = 0; i < 600 && !arrived; ++i) {
    p.step(0.02);
    const Channel* cb = p.b.doc.song().channel(ch);
    if (cb && p.b.s->fileLoading(cb->plugin.data)) placeholder = true;
    arrived = cb && !p.b.s->fileLoading(cb->plugin.data) && std::filesystem::exists(cb->plugin.data);
  }
  expect(placeholder && arrived, "a finished recording reaches B as a placeholder, then the playable clip");
}

void testRemoteDeleteWhileSounding(Pair& p) {
  // B auditions the kick and runs Seltmann's pattern on "Break"
  p.rb.eng.noteOn(p.kick, 60, 1.0f);
  p.rb.eng.ownNoteOn(p.selt, 48, 0.9f);
  p.step(1.0);
  expect(p.rb.eng.pluginPlaybackActive() && p.rb.run(0.2) > 0.01f, "B auditions and runs its own pattern");
  const uint32_t ep = p.rb.eng.stopEpoch();
  p.a.doc.removeChannel(p.selt);
  p.a.doc.removeChannel(p.kick);
  p.step(1.0);
  p.rb.eng.releaseLiveNotes();  // (B's key-up arrives; the channel is gone)
  p.step(2.0);
  expect(!p.b.doc.song().channel(p.selt) && !p.rb.eng.pluginPlaybackActive() && p.rb.eng.stopEpoch() != ep,
         "a collaborator deleting the channel B plays on its own: B stops it (its Play button goes off)");
  expect(p.rb.run(0.5) < 1e-4f, "... and B goes quiet with a normal release, no stuck note");

  // B runs the pattern of "Break 2"; A replaces its sample
  p.rb.eng.ownNoteOn(p.selt2, 48, 0.9f);
  p.step(1.0);
  expect(p.rb.eng.pluginPlaybackActive(), "B runs Break 2's pattern");
  const std::string other = writeBreak(tmpDir("local_media3") + "/other.wav", 0.03f);
  p.a.doc.editChannel(p.selt2, "Load audio", [&](Channel& c) { c.plugin.data = other; });
  p.step(2.0);
  expect(!p.rb.eng.pluginPlaybackActive(), "A replacing the sample of B's running pattern stops it on B");
  p.rb.eng.releaseLiveNotes();
  p.step(2.0);
  expect(p.rb.run(0.5) < 1e-4f, "... cleanly (silent afterwards)");
}

// The digest that tells whether two computers converged: each refers to its own
// Audio folder, spelt the way its system spells paths.
void testSongDigest() {
  const std::string hash(64, 'a');
  auto withSample = [&](const std::string& path) {
    Document d;
    d.reset(makeEmptySong());
    const ObjectId ch = d.addChannel("Loop", "gl.sampler");
    d.editChannel(ch, "sample", [&](Channel& c) { c.plugin.data = path; });
    return sharedSongDigest(d.song());
  };
  const std::string alex = withSample("/srv/alex/Song/Audio/" + hash + ".wav");
  expect(alex == withSample("/Volumes/Music/Song (2)/Audio/" + hash + ".wav") && alex == withSample("Audio/" + hash + ".wav"),
         "the song digest does not depend on where each computer keeps the project");
  expect(alex != withSample("/srv/alex/Song/Audio/" + std::string(64, 'b') + ".wav"), "...but on the file it refers to");
#ifdef _WIN32
  expect(alex == withSample("D:\\Projects\\Song\\Audio\\" + hash + ".wav") && alex == withSample("C:/Music/Sam/Song/Audio/" + hash + ".wav"),
         "Windows paths (either separator) give the same digest");
#endif
}

}  // namespace

int main() {
  testSongDigest();
  Pair p;
  expect(p.b.s->ready(), "two collaborators with their own audio engines");
  testLocalPlayback(p);
  testRemoteEditsSilent(p);
  testRecordingArrives(p);
  testRemoteDeleteWhileSounding(p);
  return finish();
}
