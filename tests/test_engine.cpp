// Gridline — engine tests: rendering, determinism, block-size invariance,
// pattern/song modes, looping, mixer routing, effects, live notes, WAV.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MidiFile.h"
#include "core/PluginParams.h"
#include "engine/EqDesign.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Render.h"

namespace {

// kick (steps) + lead (notes), one bar, 120 BPM, routed through inserts 1 and 2.
gl::Document makeDoc() {
  gl::Document d(gl::makeEmptySong());
  d.setParam(gl::songParam(gl::SongParam::Tempo), 120.0, false);
  const gl::Song& s = d.song();
  gl::ObjectId pat = s.patterns[0].id;
  gl::ObjectId kick = d.addChannel("kick", "gl.kick");
  gl::ObjectId lead = d.addChannel("lead", "gl.synth");
  d.routeChannel(kick, s.mixer[1].id);
  d.routeChannel(lead, s.mixer[2].id);
  for (int st = 0; st < 16; st += 4) d.setStep(pat, kick, st, 127);
  d.addNote(pat, lead, {0, 96, 60, 110});
  d.addNote(pat, lead, {96, 96, 64, 110});
  d.addClip(pat, 0, 0);
  return d;
}

gl::AudioBuffer render(const gl::Song& s, int block = 512, gl::Tick tail = 0) {
  gl::RenderOptions o;
  o.blockSize = block;
  o.tail = tail;
  return gl::renderSong(s, o);
}

float rms(const gl::AudioBuffer& b, size_t from, size_t to) {
  double acc = 0;
  size_t n = 0;
  for (size_t c = 0; c < b.channels; ++c)
    for (size_t i = from; i < to && i < b.frames; ++i, ++n) acc += b.at(c, i) * b.at(c, i);
  return n ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.f;
}

}  // namespace

int main() {
  gl::Document doc = makeDoc();
  const gl::Song& s = doc.song();

  // 1. JSON round-trip of the full model.
  std::string text = gl::jsonStringify(gl::songToJson(s));
  gl::Json back;
  expect(gl::jsonParse(text, back), "json round-trip parses");
  gl::Song s2;
  expect(gl::songFromJson(back, s2) && s2 == s, "song (with mixer, plugins, tracks) survives round-trip");

  // 2. Timeline math: 1 bar of 4/4 at PPQ 96 = 384 ticks; 120 BPM → 2.0 s.
  expect(s.lengthTicks() == 4 * s.ppq, "one-bar clip length = 384 ticks");

  // 3. Render.
  gl::AudioBuffer buf = render(s);
  expect(buf.channels == 2, "stereo buffer");
  expect(buf.frames == 88201, "sample-accurate render length (88201 frames)");
  expect(buf.peak() > 0.05f, "render is not silent");
  expect(rms(buf, 0, 2000) > 0.01f, "kick on beat 1 sounds at the start");

  // 4. Determinism and block-size invariance.
  expect(render(s).data == buf.data, "render is deterministic (bit-identical)");
  expect(render(s, 64).data == buf.data && render(s, 1000).data == buf.data && render(s, 1).data == buf.data,
         "output is identical for block sizes 1, 64, 512, 1000");

  // 5. Muting.
  {
    gl::Document d = makeDoc();
    for (const auto& c : d.song().channels) d.setParam(gl::channelParam(c.id, gl::ChannelParam::Mute), 1);
    expect(render(d.song()).peak() == 0.0f, "muted channels render silence");
  }
  {
    gl::Document d = makeDoc();
    d.setParam(gl::mixerParam(d.song().mixer[0].id, gl::MixerParam::Mute), 1);
    expect(render(d.song()).peak() == 0.0f, "muted master renders silence");
  }

  // 6. Solo: soloing insert 2 (lead) silences insert 1 (kick) at the start.
  {
    gl::Document d = makeDoc();
    d.editMixerTrack(d.song().mixer[2].id, "solo", [](gl::MixerTrack& m) { m.solo = true; });
    gl::AudioBuffer b = render(d.song());
    gl::Document k = makeDoc();
    k.setParam(gl::channelParam(k.song().channels[1].id, gl::ChannelParam::Mute), 1);  // kick only
    gl::AudioBuffer kickOnly = render(k.song());
    expect(b.peak() > 0.01f && b.data != buf.data, "solo keeps the soloed track");
    gl::Document l = makeDoc();
    l.setParam(gl::channelParam(l.song().channels[0].id, gl::ChannelParam::Mute), 1);  // lead only
    expect(b.data == render(l.song()).data, "solo of the lead insert equals muting the kick channel");
    (void)kickOnly;
  }

  // 7. Routing: insert 1 → insert 3 only; unrouting from master is respected.
  {
    gl::Document d = makeDoc();
    const auto& m = d.song().mixer;
    expect(d.setSend(m[1].id, m[3].id, 1.0f), "route insert 1 to insert 3");
    expect(d.setSend(m[1].id, m[0].id, 0.0f), "unroute insert 1 from master");
    expect(!d.setSend(m[3].id, m[1].id, 1.0f), "cycle insert 3 → insert 1 is refused");
    expect(render(d.song()).data == buf.data, "routing through an empty insert keeps the sound");
    d.setSend(m[3].id, m[0].id, 0.0f);
    gl::Document l = makeDoc();
    l.setParam(gl::channelParam(l.song().channels[0].id, gl::ChannelParam::Mute), 1);
    expect(render(d.song()).data == render(l.song()).data, "a track with no path to the master is silent");
  }

  // 8. Every effect: finite output; limiter holds its ceiling.
  {
    for (const auto& info : gl::pluginCatalog()) {
      if (info.kind != gl::PluginKind::Effect) continue;
      gl::Document d = makeDoc();
      d.setSlotPlugin(d.song().mixer[0].id, 0, info.type);
      gl::AudioBuffer b = render(d.song(), 512, d.song().ppq * 4);
      bool finite = true;
      for (float v : b.data) finite = finite && std::isfinite(v);
      expect(finite && b.peak() > 0.0f, ("effect produces finite, non-silent output: " + info.type).c_str());
    }
    gl::Document d = makeDoc();
    d.setSlotPlugin(d.song().mixer[0].id, 0, "gl.utility");
    d.setParam(gl::slotParam(d.song().mixer[0].id, 0, 0), 24.0);  // +24 dB into the limiter
    d.setSlotPlugin(d.song().mixer[0].id, 1, "gl.limiter");
    d.setParam(gl::slotParam(d.song().mixer[0].id, 1, 1), -6.0);  // ceiling -6 dB
    float pk = render(d.song()).peak();
    expect(pk <= 0.5013f && pk > 0.3f, "limiter holds a -6 dB ceiling on a hot signal");
  }

  // 9. Every instrument sounds.
  for (const auto& info : gl::pluginCatalog()) {
    if (info.kind != gl::PluginKind::Generator || info.type == "gl.midiout" || !info.outputs.empty()) continue;  // controllers are silent
    gl::Document d(gl::makeEmptySong());
    gl::ObjectId c = d.addChannel("x", info.type);
    // Sample-based instruments use a built-in sample (drum pads: the built-in kit).
    if (info.usesSample && info.type != "gl.pads") d.editChannel(c, "", [](gl::Channel& ch) { ch.plugin.data = "builtin:snare"; });
    d.addNote(d.song().patterns[0].id, c, {0, 96, 60, 100});
    d.addClip(d.song().patterns[0].id, 0, 0);
    expect(render(d.song(), 512, 96).peak() > 0.01f, ("instrument is audible: " + info.type).c_str());
  }

  // 9b. Song-mode time selection: playback wraps from the selection end to its start.
  {
    gl::ProgramBuilder builder;
    gl::AudioEngine eng(44100);
    gl::Song ls = s;
    ls.tempo = 120;  // one 4/4 bar = 2 s
    eng.setProgram(builder.build(ls));
    eng.setMode(gl::PlayMode::Song);
    const gl::Tick bar = ls.barTicks();
    eng.setLoopRange(bar, 2 * bar);  // loop bar 2
    eng.setPosition(bar);
    eng.play();
    std::vector<float> L(44100 * 3), R(44100 * 3);
    eng.process(L.data(), R.data(), 44100 * 3);  // 1.5 bars: wraps once, half-way into the loop again
    double pos = eng.position();
    expect(pos >= bar + 0.45 * bar && pos < bar + 0.55 * bar, "time selection loops playback inside the range");
    eng.setLoopRange(0, 0);  // cleared: the song loops over its full length again
    eng.process(L.data(), R.data(), 44100);
    expect(eng.position() > 2 * bar - 1 || eng.position() < bar, "cleared selection no longer wraps at its end");
  }

  // 9c. J-40: jumps outside the time selection play on from there (no bounce
  // back to the selection start); playback re-enters the loop when it reaches
  // it (before the selection) or after wrapping at the song end (after it).
  {
    gl::Document ld = makeDoc();
    ld.addClip(ld.song().patterns[0].id, 0, 7 * ld.song().barTicks());  // the song is 8 bars long
    gl::ProgramBuilder builder;
    gl::AudioEngine eng(44100);
    eng.setProgram(builder.build(ld.song()));
    eng.setMode(gl::PlayMode::Song);
    const double bar = static_cast<double>(ld.song().barTicks());  // 2 s at 120 BPM
    const int64_t barSmp = 88200;
    std::vector<float> L(barSmp * 4), R(barSmp * 4);
    auto run = [&](double bars) { eng.process(L.data(), R.data(), static_cast<int>(bars * static_cast<double>(barSmp))); };
    auto near = [&](double bars) { return std::fabs(eng.position() - bars * bar) < 0.02 * bar; };
    eng.setLoopRange(static_cast<gl::Tick>(bar), static_cast<gl::Tick>(2 * bar));  // loop bar 2
    eng.setPosition(static_cast<gl::Tick>(bar));
    eng.play();
    run(0.25);
    expect(near(1.25), "loop: playing inside the selection");
    eng.jumpTo(static_cast<gl::Tick>(5 * bar), 0);  // ruler click after the selection
    run(0.5);
    expect(near(5.5), "loop: a jump past the selection plays on from there (no bounce back)");
    run(3.0);  // reaches the song end (8 bars) and wraps into the selection
    expect(near(1.5), "loop: after the song end playback wraps to the selection start");
    run(1.0);
    expect(near(1.5), "loop: the selection loops again once re-entered");
    eng.jumpTo(0, 0);  // before the selection: plays into it, then loops
    run(0.5);
    expect(near(0.5), "loop: a jump before the selection plays from there");
    run(2.0);
    expect(near(1.5), "loop: ... and loops once playback reaches the selection");
    eng.jumpTo(static_cast<gl::Tick>(1.75 * bar), 0);  // inside: loops as before
    run(0.5);
    expect(near(1.25), "loop: a jump inside the selection keeps looping");
    eng.jumpTo(static_cast<gl::Tick>(4 * bar), static_cast<gl::Tick>(bar));  // quantized (J-31): lands on the next bar line
    run(1.25);  // 0.75 bar to the bar line (= the selection end), then 0.5 bar from bar 4
    expect(near(4.5), "loop: a quantized jump past the selection lands there and plays on");
    eng.pause();
    eng.setPosition(static_cast<gl::Tick>(6 * bar));  // stopped: position set past the selection, then play
    eng.play();
    run(0.5);
    expect(near(6.5), "loop: play from a position past the selection plays on");
    eng.setLoopRange(static_cast<gl::Tick>(5 * bar), static_cast<gl::Tick>(6 * bar));  // a new selection behind the position
    run(0.25);
    expect(near(6.75), "loop: moving the selection keeps playing on past it");
    eng.stop();
    eng.setPosition(static_cast<gl::Tick>(7 * bar));
    eng.setLoopRange(0, 0);  // no selection: the song wraps to its start at the end
    eng.play();
    run(1.5);
    expect(near(0.5), "loop: without a selection the song wraps to its start");
  }

  // 10. Realtime engine: pattern mode loops; live notes; stop.
  {
    gl::ProgramBuilder builder;
    gl::AudioEngine eng(44100);
    eng.setProgram(builder.build(s));
    eng.setPattern(s.patterns[0].id);
    eng.setMode(gl::PlayMode::Pattern);
    eng.play();
    std::vector<float> L(44100 * 3), R(44100 * 3);
    eng.process(L.data(), R.data(), 44100 * 3);  // 3 s = one and a half loops of 2 s
    expect(eng.position() > 0 && eng.position() < 384, "pattern mode wraps at the pattern end");
    float second = 0;
    for (size_t i = 88200; i < 88200 + 2000; ++i) second = std::max(second, std::fabs(L[i]));
    expect(second > 0.05f, "the loop retriggers beat 1");
    eng.stop();
    eng.process(L.data(), R.data(), 4096);
    expect(!eng.playing() && eng.position() == 0, "stop returns to the start");
    std::vector<float> L2(8192), R2(8192);
    eng.process(L2.data(), R2.data(), 8192);  // let tails die
    eng.noteOn(s.channels[1].id, 69, 0.9f);
    eng.process(L2.data(), R2.data(), 4096);
    float live = 0;
    for (size_t i = 0; i < 4096; ++i) live = std::max(live, std::fabs(L2[i]));
    expect(live > 0.01f, "live note-on is audible while stopped");
    eng.noteOff(s.channels[1].id, 69);
    for (int i = 0; i < 40; ++i) eng.process(L2.data(), R2.data(), 4096);
    float after = 0;
    for (size_t i = 0; i < 4096; ++i) after = std::max(after, std::fabs(L2[i]));
    expect(after < 1e-3f, "live note-off releases the voice");
  }

  // 11. Program swap mid-playback keeps the sequencer position.
  {
    gl::Document d = makeDoc();
    gl::ProgramBuilder builder;
    gl::AudioEngine eng(44100);
    eng.setProgram(builder.build(d.song()));
    eng.setMode(gl::PlayMode::Song);
    eng.play();
    std::vector<float> L(22050), R(22050);
    eng.process(L.data(), R.data(), 22050);
    double before = eng.position();
    d.setParam(gl::channelParam(d.song().channels[0].id, gl::ChannelParam::Pan), 0.5);
    eng.setProgram(builder.build(d.song()));
    eng.process(L.data(), R.data(), 512);
    expect(eng.position() > before, "editing during playback keeps the position moving");
    expect(builder.channelRuntime(d.song().channels[0].id) != nullptr, "channel runtimes persist across edits");
  }

  // 12. MIDI file round-trip (pattern → .mid → read back).
  {
    gl::Document d = makeDoc();
    gl::MidiFileData m = gl::patternToMidi(d.song(), d.song().patterns[0].id);
    const char* mid = "gridline_test.mid";
    std::string err;
    expect(gl::writeMidiFile(mid, m, &err), "MIDI file writes");
    gl::MidiFileData back;
    expect(gl::readMidiFile(mid, back, &err), "MIDI file reads back");
    bool same = back.tracks.size() == m.tracks.size() && back.division == m.division && std::fabs(back.tempo - 120.0) < 0.01;
    for (size_t i = 0; same && i < m.tracks.size(); ++i) {
      same = back.tracks[i].notes.size() == m.tracks[i].notes.size() && back.tracks[i].name == m.tracks[i].name;
      for (size_t k = 0; same && k < m.tracks[i].notes.size(); ++k) {
        const auto &x = m.tracks[i].notes[k], &y = back.tracks[i].notes[k];
        same = x.start == y.start && x.length == y.length && x.pitch == y.pitch && x.velocity == y.velocity;
      }
    }
    expect(same, "MIDI round-trip keeps tracks, notes, timing, tempo");
    std::remove(mid);
  }

  // 13. MIDI input reaches the engine; the sustain pedal holds notes.
  {
    gl::Document d = makeDoc();
    gl::ProgramBuilder b;
    gl::AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    eng.setMidiTarget(d.song().channels[1].id);  // synth
    std::vector<float> L(4096), R(4096);
    auto peak = [&]() { float p = 0; for (float v : L) p = std::max(p, std::fabs(v)); return p; };
    eng.midiEvent(0xB0, 64, 127);  // pedal down
    eng.midiEvent(0x90, 60, 100);
    eng.process(L.data(), R.data(), 4096);
    expect(peak() > 0.01f, "MIDI note-on plays the target channel");
    eng.midiEvent(0x80, 60, 0);
    for (int i = 0; i < 20; ++i) eng.process(L.data(), R.data(), 4096);
    expect(peak() > 0.01f, "sustain pedal holds the note after note-off");
    eng.midiEvent(0xB0, 64, 0);  // pedal up
    for (int i = 0; i < 30; ++i) eng.process(L.data(), R.data(), 4096);
    expect(peak() < 1e-3f, "releasing the pedal releases the note");
  }

  // 14. Controller links survive save/load; EQ band types shape the response.
  {
    gl::Document d = makeDoc();
    d.editSong("link", [&](gl::Song& s) {
      gl::ControllerLink l;
      l.cc = 74;
      l.owner = s.channels[0].id;
      l.index = 1;
      s.links.push_back(l);
    });
    gl::Song back2;
    expect(gl::songFromJson(gl::songToJson(d.song()), back2) && back2.links.size() == 1 && back2.links[0].cc == 74,
           "controller links are saved and loaded");
    gl::PluginState eq = gl::defaultPluginState("gl.eq");
    eq.params[gl::pp::EqModel] = 0;  // legacy band model (slope parameter)
    eq.params[gl::pp::eqParam(3, gl::pp::EqFreq)] = 1000;
    eq.params[gl::pp::eqParam(0, gl::pp::EqType)] = gl::pp::EqLowCut;
    eq.params[gl::pp::eqParam(0, gl::pp::EqFreq)] = 200;
    eq.params[gl::pp::eqParam(0, gl::pp::EqSlope)] = 3;
    gl::EqBandFilters f;
    gl::designEqBand(eq, 0, 44100, f);
    double at50 = gl::dsp::gainToDb(gl::eqBandMagnitude(f, 50, 44100)), at2k = gl::dsp::gainToDb(gl::eqBandMagnitude(f, 2000, 44100));
    expect(f.count == 4 && at50 < -40 && std::fabs(at2k) < 0.5, "48 dB/oct low cut: -40 dB two octaves below, flat above");
    eq.params[gl::pp::eqParam(3, gl::pp::EqGain)] = 6;
    gl::designEqBand(eq, 3, 44100, f);
    expect(std::fabs(gl::dsp::gainToDb(gl::eqBandMagnitude(f, 1000, 44100)) - 6.0) < 0.05, "peak band reaches its gain at its frequency");
  }

  // 15. MIDI Out channels send their notes to the MIDI ring, not to audio.
  {
    gl::Document d(gl::makeEmptySong());
    gl::ObjectId c = d.addChannel("out", "gl.midiout");
    gl::ProgramBuilder b;
    gl::AudioEngine eng(44100);
    eng.setProgram(b.build(d.song()));
    std::vector<float> L(512), R(512);
    eng.noteOn(c, 64, 0.5f);
    eng.process(L.data(), R.data(), 512);
    eng.noteOff(c, 64);
    eng.process(L.data(), R.data(), 512);
    gl::MidiOutRing::Msg m1{}, m2{};
    auto rt = b.channelRuntime(c);
    bool got = rt && rt->midiOut.pop(m1) && rt->midiOut.pop(m2);
    expect(got && m1.s == 0x90 && m1.a == 64 && m2.s == 0x80 && m2.a == 64, "MIDI Out sends note-on and note-off");
  }

  // 16. WAV writer: header fields + data size.
  const char* tmp = "gridline_test_out.wav";
  expect(gl::writeWav(tmp, buf, 44100, 16), "wav write succeeds");
  std::ifstream f(tmp, std::ios::binary);
  char hdr[44] = {0};
  f.read(hdr, 44);
  expect(std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "WAVE", 4) == 0, "wav RIFF/WAVE magic");
  uint32_t dataSize;
  std::memcpy(&dataSize, hdr + 40, 4);
  expect(dataSize == 88201u * 2u * 2u, "wav data chunk size matches frames");
  f.close();
  std::remove(tmp);

  return finish();
}
