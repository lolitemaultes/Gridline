// Gridline - K-129 tests: the Tracker Sampler's instrument and sample settings.
//   * an empty instrument is silent; old states keep the sound they were saved with;
//   * the MIDI settings (mute, wheel range, notes out to the MIDI output), 8-bit samples, the
//     envelope slots, copying a sample: each changes the output as it should, extremes stay finite,
//     and texts without them read and write back as before;
//   * the sample edits on known buffers (cut, copy, paste, crop, silence, reverse, ramps,
//     normalise, resample, loop crossfade, DC offset, mix, echo, trimming after the loop), a
//     committed edit plays the new audio, the project finds its file, and undo brings the old back;
//   * hearing one sample: the asked-for slot plays once at its own rate, then stops; a second
//     request replaces it, a note off stops it, a plain note afterwards plays the key map again.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/app/KeyLightState.h"
#include "../../src/core/Document.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/ProjectInterop.h"
#include "../../src/core/TrackerData.h"
#include "../../src/core/WavWriter.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Sample.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/tracker/tracker.h"
#include "../Check.h"

using namespace gl;
namespace trk = gl::tracker;
namespace tp = gl::packs::trackerpack;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

struct Out {
  std::vector<float> L, R;
};

struct Run {
  int offAt = -1;
  int bendAt = -1, bendValue = 8192;
  MidiOutRing* ring = nullptr;
};

Out play(const PluginState& st, double pitch, int total, int block, const Run& r = {}) {
  Out o;
  o.L.assign(static_cast<size_t>(total), 0.0f);
  o.R.assign(static_cast<size_t>(total), 0.0f);
  auto g = packs::createGenerator(st.type);
  if (r.ring) g->setMidiOut(r.ring);
  ChannelAux aux;
  packs::prepareAux(st, aux);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  double tick = 0;
  for (int pos = 0; pos < total; pos += block) {
    const int n = std::min(block, total - pos);
    if (pos == 0) g->noteOn(1, pitch, 100.0f / 127.0f, 0);
    if (r.offAt >= pos && r.offAt < pos + n) g->noteOff(1, r.offAt - pos);
    if (r.bendAt >= pos && r.bendAt < pos + n)
      g->midi(0xE0, static_cast<uint8_t>(r.bendValue & 0x7F), static_cast<uint8_t>((r.bendValue >> 7) & 0x7F), r.bendAt - pos);
    ctx.tick = tick;
    g->process(st, aux, ctx, o.L.data() + pos, o.R.data() + pos, n);
    tick += n * ctx.tempo * ctx.ppq / (60.0 * ctx.sampleRate);
  }
  return o;
}

double peak(const std::vector<float>& v, size_t a, size_t b) {
  double m = 0;
  for (size_t i = a; i < std::min(b, v.size()); ++i) m = std::max(m, static_cast<double>(std::abs(v[i])));
  return m;
}
bool finite(const Out& o) {
  for (size_t i = 0; i < o.L.size(); ++i)
    if (!std::isfinite(o.L[i]) || !std::isfinite(o.R[i])) return false;
  return true;
}
// Zero crossings per second (rising), a rough pitch.
double crossingsHz(const std::vector<float>& v, size_t from, size_t len) {
  int n = 0;
  for (size_t i = from + 1; i < std::min(v.size(), from + len); ++i) n += (v[i - 1] < 0 && v[i] >= 0) ? 1 : 0;
  return n * 44100.0 / static_cast<double>(len);
}

std::string tempDir(const std::string& tag) {
  const fs::path d = fs::temp_directory_path() / ("gridline-k129-" + tag);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d.u8string();
}

std::string writeFile(const std::string& dir, const std::string& name, const std::vector<float>& left, int rate = 44100) {
  AudioBuffer b(1, left.size());
  for (size_t i = 0; i < left.size(); ++i) b.data[i] = left[i];
  const std::string p = (fs::u8path(dir) / name).u8string();
  writeWav(p, b, rate, 24);
  return p;
}

std::vector<float> sine(int frames, double hz, double amp, int rate = 44100) {
  std::vector<float> v(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) v[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2 * 3.14159265358979 * hz * i / rate));
  return v;
}

PluginState withData(const std::string& data, int sound = 0) {
  PluginState st = defaultPluginState(trk::kInstrumentType);
  st.params[trk::PSound] = static_cast<float>(sound);
  st.params[trk::PInterp] = trk::InterpNone;
  st.params[trk::PRamp] = 0;
  st.data = data;
  return st;
}

// An instrument with one plain sample (no envelope: the sample as it is).
tp::Edit oneSample(const std::string& path) {
  tp::Edit e = tp::readEdit("", 0);
  e.text.def.vol.flags = 0;
  SamplePtr sp = sampleCache().get(path);
  tp::addSample(e, path, sp ? static_cast<int32_t>(sp->frames()) : 0, sp ? sp->sampleRate : 44100, "s");
  return e;
}

void testSilentAndOld() {
  const PluginState fresh = defaultPluginState(trk::kInstrumentType);
  ChannelAux aux;
  expect(packs::prepareAux(fresh, aux), "empty: prepares");
  const Out o = play(fresh, 60, 20000, 256, {10000});
  expect(finite(o) && peak(o.L, 0, 20000) == 0.0 && peak(o.R, 0, 20000) == 0.0, "empty: a new instance plays nothing");
  const PluginInfo* info = findPlugin(trk::kInstrumentType);
  expect(info && info->params[trk::PSound].def == 0.0, "empty: no built-in sound by default");
  // A state saved with a built-in sound (all seven parameters stored) keeps it.
  PluginState saved = defaultPluginState(trk::kInstrumentType);
  saved.params[trk::PSound] = 3;
  PluginState loaded = saved;
  normalizePluginState(loaded);
  expect(loaded.params[trk::PSound] == 3.0f, "old state: a saved built-in sound stays");
  expect(peak(play(loaded, 60, 20000, 256).L, 0, 20000) > 0.1, "old state: and plays");
  // A state saved before the sounds (six parameters) has none, empty or not.
  PluginState six = defaultPluginState(trk::kInstrumentType);
  six.params.resize(6);
  normalizePluginState(six);
  expect(six.params[trk::PSound] == 0.0f && peak(play(six, 60, 8000, 256).L, 0, 8000) == 0.0, "old state: six parameters, no sound");
  // Presets still choose sounds.
  if (info)
    for (size_t i = 0; i < info->presets.size(); ++i)
      expect(peak(play(presetState(*info, i), 60, 20000, 256, {12000}).L, 0, 20000) > 0.05, "preset " + info->presets[i].name + ": sounds");
}

void testSettings(const std::string& dir) {
  const std::string path = writeFile(dir, "tone.wav", sine(30000, 441.0, 0.5));
  tp::Edit e = oneSample(path);
  const Out plain = play(withData(tp::writeEdit(e)), 60, 8000, 256);
  expect(peak(plain.L, 0, 8000) > 0.2, "settings: the plain sample sounds");

  // Text: defaults write nothing new; the new lines read back.
  const std::string before = tp::writeEdit(e);
  expect(before.find("midi ") == std::string::npos && before.find("bits ") == std::string::npos, "text: default settings add no lines");
  tp::Edit m = e;
  m.midi.on = true;
  m.midi.channel = 9;
  m.midi.program = 33;
  m.midi.bend = 12;
  m.midi.mute = true;
  m.eightBit[0] = true;
  const tp::Edit back = tp::readEdit(tp::writeEdit(m), 0);
  expect(back.midi.on && back.midi.channel == 9 && back.midi.program == 33 && back.midi.bend == 12 && back.midi.mute && back.eightBit[0] && back.text.def.mute,
         "text: MIDI settings and bit depth read back");
  expect(tp::writeEdit(back) == tp::writeEdit(m), "text: and write back the same");
  const tp::Edit junk = tp::readEdit(before + "midi 7 99 999 -5 2\nbits 40 8\nbits x\n", 0);
  expect(junk.midi.on && junk.midi.channel == 15 && junk.midi.program == 127 && junk.midi.bend == 0 && junk.midi.mute, "text: out-of-range values are clamped");

  // Mute: silent; the MIDI notes still go out.
  tp::Edit mu = e;
  mu.midi.mute = true;
  expect(peak(play(withData(tp::writeEdit(mu)), 60, 8000, 256).L, 0, 8000) == 0.0, "mute: the instrument's own sound is silent");

  // MIDI output: a program change, the note on its channel, the note off.
  {
    tp::Edit mo = e;
    mo.midi.on = true;
    mo.midi.channel = 3;
    mo.midi.program = 20;
    MidiOutRing ring;
    Run r;
    r.offAt = 3000;
    r.ring = &ring;
    const Out o = play(withData(tp::writeEdit(mo)), 64, 6000, 256, r);
    std::vector<MidiOutRing::Msg> msgs;
    MidiOutRing::Msg msg;
    while (ring.pop(msg)) msgs.push_back(msg);
    const bool prog = !msgs.empty() && msgs[0].s == 0xC3 && msgs[0].a == 20;
    bool on = false, off = false;
    for (const auto& x : msgs) {
      on = on || (x.s == 0x93 && x.a == 64 && x.b > 0);
      off = off || (x.s == 0x83 && x.a == 64);
    }
    expect(prog && on && off, "MIDI out: program change, note on and note off on channel 4");
    expect(peak(o.L, 0, 2000) > 0.2, "MIDI out: the own sound still plays when not muted");
    MidiOutRing quiet;
    Run q;
    q.ring = &quiet;
    play(withData(tp::writeEdit(e)), 64, 2000, 256, q);
    expect(!quiet.pop(msg), "MIDI out: off by default, nothing is sent");
  }

  // Wheel range: twelve semitones bend an octave.
  {
    tp::Edit br = e;
    br.midi.bend = 12;
    Run up;
    up.bendAt = 0;
    up.bendValue = 16383;
    const Out a = play(withData(tp::writeEdit(e)), 60, 20000, 256);
    const Out b = play(withData(tp::writeEdit(br)), 60, 20000, 256, up);
    const double ratio = crossingsHz(b.L, 2000, 8000) / crossingsHz(a.L, 2000, 8000);
    expect(std::abs(ratio - 2.0) < 0.05, "wheel range: 12 semitones bend an octave (" + std::to_string(ratio) + ")");
    tp::Edit wide = e;
    wide.midi.bend = 36;
    expect(finite(play(withData(tp::writeEdit(wide)), 127, 8000, 256, up)), "wheel range: the widest bend on the top key stays finite");
  }

  // 8 bits: a quiet tone below one 8-bit step disappears; louder values land on 1/128 steps.
  {
    const std::string qp = writeFile(dir, "quiet.wav", sine(20000, 441.0, 0.003));
    tp::Edit q = oneSample(qp);
    const double full = peak(play(withData(tp::writeEdit(q)), 60, 8000, 256).L, 0, 8000);
    q.eightBit[0] = true;
    const double crushed = peak(play(withData(tp::writeEdit(q)), 60, 8000, 256).L, 0, 8000);
    expect(full > 0.001 && crushed < 1e-6, "8-bit: values below one step become silence");
    tp::Edit l = e;
    l.eightBit[0] = true;
    auto inst = tp::instrumentFor(withData(tp::writeEdit(l)));
    bool steps = inst->data[0] != nullptr;
    for (int i = 0; steps && i < 2000; ++i) {
      const double v = inst->data[0][i] * 128.0;
      steps = std::abs(v - std::round(v)) < 1e-6;
    }
    expect(steps, "8-bit: the played values sit on 256 steps");
    expect(tp::eightBitValue(1.0f) == 127.0f / 128.0f && tp::eightBitValue(-1.0f) == -1.0f && tp::eightBitValue(0.003f) == 0.0f && tp::eightBitValue(0.005f) == 1.0f / 128.0f, "8-bit: rounding and its ends");
  }

  // Copying a sample: a second slot with the same file and settings, no keys.
  {
    tp::Edit c = e;
    c.text.def.samples[0].volume = 40;
    c.eightBit[0] = true;
    const int k = tp::duplicateSample(c, 0);
    expect(k == 1 && c.text.files.size() == 1 && c.text.fileOf[1] == 0 && c.text.def.samples[1].volume == 40 && c.eightBit[1] && c.text.def.keymap[60] == 0,
           "copy sample: the next free slot, same file and settings, keys unchanged");
    expect(tp::duplicateSample(c, 5) == -1, "copy sample: an empty slot copies nothing");
  }

  // Envelope slots: a slot changes the sound; the file round trip; a broken file keeps the factory.
  {
    const tp::EnvSlots f = tp::factoryEnvSlots();
    bool valid = true;
    for (int k = 0; k < tp::kEnvSlots; ++k)
      for (const auto* s : {&f.vol[static_cast<size_t>(k)], &f.pan[static_cast<size_t>(k)]}) {
        valid = valid && s->env.count >= 2 && s->env.on() && s->env.x[0] == 0;
        for (int i = 1; i < s->env.count; ++i) valid = valid && s->env.x[static_cast<size_t>(i)] > s->env.x[static_cast<size_t>(i - 1)];
      }
    expect(valid, "envelope slots: twelve starting shapes, each a valid envelope");
    tp::Edit st = e;
    tp::applyEnvSlot(st.text.def, f.vol[5], false);  // the short stab
    const Out stab = play(withData(tp::writeEdit(st)), 60, 20000, 256);
    expect(peak(stab.L, 0, 2000) > 0.2 && peak(stab.L, 12000, 20000) < 1e-6, "envelope slots: the stab ends while the plain sample goes on");
    tp::applyEnvSlot(st.text.def, f.pan[1], true);
    expect(st.text.def.pan.on() && st.text.def.pan.y[0] == 8 && st.text.def.vol.x[2] == f.vol[5].env.x[2], "envelope slots: the panning slot leaves the volume envelope");
    tp::EnvSlots mine = f;
    mine.vol[2] = tp::takeEnvSlot(st.text.def, false);
    mine.vol[2].fadeout = 777;
    const std::string sp = (fs::u8path(dir) / "cfg" / "tracker-envelopes.txt").u8string();
    expect(tp::saveEnvSlots(sp, mine), "envelope slots: saved");
    const tp::EnvSlots read = tp::loadEnvSlots(sp);
    expect(read.vol[2].fadeout == 777 && read.vol[2].env.count == mine.vol[2].env.count && read.vol[2].env.x[2] == mine.vol[2].env.x[2] && read.pan[1].env.y[0] == 8,
           "envelope slots: read back");
    writeFile(dir, "dummy.wav", {0.0f});
    const tp::EnvSlots none = tp::loadEnvSlots((fs::u8path(dir) / "dummy.wav").u8string());
    expect(none.vol[0].env.count == f.vol[0].env.count && none.pan[2].env.loopEnd == f.pan[2].env.loopEnd, "envelope slots: an unreadable file keeps the starting shapes");
  }
}

tp::Wave ramp(int n, bool stereo = false) {
  tp::Wave w;
  w.rate = 44100;
  w.stereo = stereo;
  for (int i = 0; i < n; ++i) {
    w.L.push_back(static_cast<float>(i));
    if (stereo) w.R.push_back(static_cast<float>(-i));
  }
  return w;
}
bool same(const std::vector<float>& v, std::initializer_list<float> want) {
  if (v.size() != want.size()) return false;
  size_t i = 0;
  for (float x : want)
    if (std::abs(v[i++] - x) > 1e-5f) return false;
  return true;
}

void testWaveEdits() {
  // Delete / cut: the range goes, the points after it move back.
  {
    tp::Wave w = ramp(10, true);
    w.loop = trk::kLoopForward;
    w.loopStart = 6;
    w.loopLength = 3;
    w.cutA = 1;
    w.cutB = 9;
    const tp::Wave clip = tp::waveCopy(w, {2, 5});
    expect(same(clip.L, {2, 3, 4}) && same(clip.R, {-2, -3, -4}), "copy: the range, both sides");
    expect(tp::waveDelete(w, {2, 5}) && same(w.L, {0, 1, 5, 6, 7, 8, 9}) && same(w.R, {0, -1, -5, -6, -7, -8, -9}), "cut: the range goes");
    expect(w.loopStart == 3 && w.loopLength == 3 && w.cutA == 1 && w.cutB == 6, "cut: loop and end marks move with the audio");
    expect(!tp::waveDelete(w, {0, 7}) && !tp::waveDelete(w, {3, 3}), "cut: never everything, never an empty range");
    // Paste the clip back where it was: the original again.
    expect(tp::wavePaste(w, {2, 2}, clip) && same(w.L, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) && w.loopStart == 6 && w.cutB == 9, "paste: inserted, points move on");
    // Paste over a range.
    tp::Wave one;
    one.L = {50};
    expect(tp::wavePaste(w, {1, 4}, one) && same(w.L, {0, 50, 4, 5, 6, 7, 8, 9}) && same(w.R, {0, 50, -4, -5, -6, -7, -8, -9}), "paste: over a range (a mono clip on both sides)");
    expect(!tp::wavePaste(w, {0, 0}, tp::Wave{}), "paste: nothing copied, nothing pasted");
  }
  {
    tp::Wave w = ramp(10);
    w.loop = trk::kLoopPingPong;
    w.loopStart = 2;
    w.loopLength = 6;
    expect(tp::waveCrop(w, {3, 7}) && same(w.L, {3, 4, 5, 6}) && w.loopStart == 0 && w.loopLength == 4, "crop: only the range stays, the loop inside it");
    tp::Wave s = ramp(6);
    expect(tp::waveSilence(s, {1, 3}) && same(s.L, {0, 0, 0, 3, 4, 5}) && !tp::waveSilence(s, {2, 2}), "silence: the range only");
    tp::Wave r = ramp(6);
    expect(tp::waveReverse(r, {1, 4}) && same(r.L, {0, 3, 2, 1, 4, 5}), "reverse: the range");
    expect(tp::waveReverse(r, {}) && same(r.L, {5, 4, 1, 2, 3, 0}), "reverse: no range, the whole sample");
  }
  {
    tp::Wave w;
    w.L = {1, 1, 1, 1, 1};
    expect(tp::waveRamp(w, {}, 0.0, 1.0) && same(w.L, {0, 0.25f, 0.5f, 0.75f, 1}), "fade in: silent first frame, full last");
    w.L = {1, 1, 1, 1, 1};
    expect(tp::waveRamp(w, {1, 4}, 2.0, 4.0) && same(w.L, {1, 2, 3, 4, 1}), "volume ramp: start to end across the range");
    w.L = {0.25f, -0.5f, 0.1f};
    expect(std::abs(tp::waveMaxScale(w, {}) - 2.0) < 1e-9 && tp::waveNormalise(w, {}) && same(w.L, {0.5f, -1.0f, 0.2f}), "normalise: the loudest point to full scale");
    expect(!tp::waveNormalise(w, {}), "normalise: already there, no change");
    w.L = {1.5f, 0.5f, 1.0f, 1.0f};
    expect(tp::waveFixDc(w, {}) && same(w.L, {0.5f, -0.5f, 0, 0}) && !tp::waveFixDc(w, {}), "DC offset: the mean goes, once");
  }
  {
    tp::Wave w;
    w.rate = 44100;
    w.L = sine(4410, 441.0, 0.5);
    w.loop = trk::kLoopForward;
    w.loopStart = 1000;
    w.loopLength = 2000;
    w.relNote = 3;
    expect(tp::waveResample(w, 12) && std::abs(w.frames() - 8820) <= 2 && w.relNote == 15, "resample: an octave up doubles the frames, the transpose follows");
    expect(std::abs(w.loopStart - 2000) <= 1 && std::abs(w.loopLength - 4000) <= 2, "resample: the loop scales with it");
    expect(std::abs(crossingsHz(w.L, 0, 8000) - 220.5) < 8, "resample: the same sound at half the speed (" + std::to_string(crossingsHz(w.L, 0, 8000)) + ")");
    expect(!tp::waveResample(w, 0), "resample: zero semitones change nothing");
  }
  {
    tp::Wave w;
    w.L.assign(100, 0.0f);
    for (int i = 0; i < 50; ++i) w.L[static_cast<size_t>(i)] = 1.0f;
    w.loop = trk::kLoopForward;
    w.loopStart = 50;
    w.loopLength = 50;
    expect(tp::waveCrossfadeLoop(w, 10) && w.L[99] > 0.9f && w.L[89] < 0.2f && w.L[60] == 0.0f, "crossfade loop: the loop's end fades into what precedes its start");
    tp::Wave n = w;
    n.loopStart = 0;
    expect(!tp::waveCrossfadeLoop(n, 10), "crossfade loop: nothing before the loop, no change");
    expect(tp::waveMinimise(w) == false, "trim after loop: a loop to the end is already trimmed");
    w.loopLength = 30;
    expect(tp::waveMinimise(w) && w.frames() == 80, "trim after loop: the frames after the loop go");
  }
  {
    tp::Wave w;
    w.L = {1, 1};
    tp::Wave o;
    o.L = {0, 0, 4};
    expect(tp::waveMix(w, o, 0.25) && same(w.L, {0.75f, 0.75f, 1.0f}), "mix: balance, the longer length");
    expect(!tp::waveMix(w, o, 0.0), "mix: no balance, no change");
    tp::Wave e;
    e.L = {1, 0, 0, 0};
    expect(tp::waveEcho(e, 2, 1, 0.5, false) && same(e.L, {1, 0.5f, 0.25f, 0}), "echo: repeats each quieter");
    tp::Wave x;
    x.L = {1, 0};
    expect(tp::waveEcho(x, 3, 2, 1.0, true) && x.frames() == 8 && same(x.L, {1, 0, 1, 0, 1, 0, 1, 0}), "echo: lengthened to hold the repeats");
    tp::Wave big;
    big.L = sine(2000, 300, 1.0);
    tp::waveEcho(big, 32, 7, 1.0, true);
    tp::waveRamp(big, {}, 4.0, 4.0);
    bool fin = true;
    for (float v : big.L) fin = fin && std::isfinite(v);
    expect(fin, "echo and ramp at their extremes: finite");
  }
}

void testCommitAndUndo(const std::string& dir) {
  const std::string path = writeFile(dir, "orig.wav", sine(8000, 441.0, 0.5));
  tp::Edit e = oneSample(path);
  const PluginState st = withData(tp::writeEdit(e));
  auto inst = tp::instrumentFor(st);
  tp::Wave w;
  expect(tp::waveOf(e, 0, *inst, w) && w.frames() == 8000, "commit: the slot's wave");
  tp::waveReverse(w, {});
  tp::waveRamp(w, {}, 0.5, 0.5);
  const std::string out = tp::commitWave(e, 0, w, (fs::u8path(dir) / "Edited audio").u8string(), "orig");
  expect(!out.empty() && fs::is_regular_file(fs::u8path(out)) && out != path, "commit: a new file");
  PluginState edited = st;
  edited.data = tp::writeEdit(e);
  auto inst2 = tp::instrumentFor(edited);
  SamplePtr src = sampleCache().get(path);
  bool rev = inst2->data[0] && src;
  for (int i = 0; rev && i < 8000; ++i) rev = std::abs(inst2->data[0][i] - 0.5f * src->left[static_cast<size_t>(7999 - i)]) < 1e-6f;
  expect(rev, "commit: the instrument plays the edited audio");
  // The project finds the new file (save, zip, collaboration carry it).
  bool found = false;
  PluginState copy = edited;
  forEachStateFileRef(copy, [&](std::string& p) { found = found || p == out; });
  expect(found, "commit: the project's file list has the edited file");
  // Undo in a document brings the old audio back.
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("trk", trk::kInstrumentType);
  d.editChannel(ch, "set", [&](Channel& c) { c.plugin = st; });
  d.editChannel(ch, "reverse sample", [&](Channel& c) { c.plugin = edited; });
  expect(d.song().channel(ch)->plugin.data == edited.data, "undo: the edit is in the document");
  expect(d.undo() && d.song().channel(ch)->plugin.data == st.data, "undo: the old text is back");
  auto back = tp::instrumentFor(d.song().channel(ch)->plugin);
  bool orig = back->data[0] && src;
  for (int i = 0; orig && i < 8000; ++i) orig = back->data[0][i] == src->left[static_cast<size_t>(i)];
  expect(orig, "undo: and the old audio plays");
}

void testPreview(const std::string& dir) {
  // Two samples: slot 0 a low tone on every key, slot 1 a short high one.
  const std::string lowP = writeFile(dir, "low.wav", sine(40000, 220.5, 0.5));
  const std::string highP = writeFile(dir, "high.wav", sine(4410, 1764.0, 0.5));
  tp::Edit e = oneSample(lowP);
  SamplePtr hs = sampleCache().get(highP);
  tp::addSample(e, highP, static_cast<int32_t>(hs->frames()), hs->sampleRate, "high");
  e.text.def.samples[1].loop = trk::kLoopForward;  // a loop the preview ignores
  e.text.def.samples[1].loopStart = 0;
  e.text.def.samples[1].loopLength = 4410;
  const PluginState st = withData(tp::writeEdit(e));
  ChannelAux aux;
  packs::prepareAux(st, aux);
  const auto* inst = dynamic_cast<const tp::InstrumentAux*>(aux.custom.get());
  expect(inst != nullptr, "preview: the instrument");
  if (!inst) return;
  const int pitch = tp::auditionPitch(e, 1, hs->sampleRate);
  expect(pitch == 60, "preview: a file plays at its own rate on middle C");
  auto g = packs::createGenerator(st.type);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  std::vector<float> L(20000, 0.0f), R(20000, 0.0f);
  const uint32_t id = tp::requestAudition(inst, 1, pitch);
  expect(tp::auditionSounding(id) && !tp::auditionPlaying(id), "preview: asked for, not yet playing");
  g->noteOn(7, pitch, 100.0f / 127.0f, 0);
  g->process(st, aux, ctx, L.data(), R.data(), 512);
  expect(tp::auditionPlaying(id), "preview: playing");
  for (int pos = 512; pos < 20000; pos += 512) g->process(st, aux, ctx, L.data() + pos, R.data() + pos, std::min(512, 20000 - pos));
  const double hz = crossingsHz(L, 200, 4000);
  expect(std::abs(hz - 1764.0) < 30, "preview: the asked-for sample, not the key map's (" + std::to_string(hz) + " Hz)");
  expect(peak(L, 5000, 20000) == 0.0 && !tp::auditionSounding(id), "preview: once through (no loop), then it ends");
  g->noteOff(7, 0);
  // A note off stops a preview; a new request replaces the old.
  const uint32_t id2 = tp::requestAudition(inst, 0, 60);
  const uint32_t id3 = tp::requestAudition(inst, 1, pitch);
  expect(!tp::auditionSounding(id2) && tp::auditionSounding(id3), "preview: a new request replaces the old");
  std::fill(L.begin(), L.end(), 0.0f);
  g->noteOn(8, pitch, 100.0f / 127.0f, 0);
  g->process(st, aux, ctx, L.data(), R.data(), 512);
  g->noteOff(8, 0);
  for (int pos = 512; pos < 4096; pos += 512) g->process(st, aux, ctx, L.data() + pos, R.data() + pos, 512);
  expect(peak(L, 0, 512) > 0.2 && peak(L, 2048, 4096) == 0.0 && !tp::auditionSounding(id3), "preview: a note off stops it");
  // Afterwards the same key plays the key map again (slot 0, the low tone).
  std::fill(L.begin(), L.end(), 0.0f);
  g->noteOn(9, pitch, 100.0f / 127.0f, 0);
  for (int pos = 0; pos < 8192; pos += 512) g->process(st, aux, ctx, L.data() + pos, R.data() + pos, 512);
  expect(std::abs(crossingsHz(L, 1000, 6000) - 220.5) < 10, "preview: used once; a plain note plays the key map");
  // A request no note takes can be dropped.
  const uint32_t id4 = tp::requestAudition(inst, 1, 61);
  tp::cancelAudition(id4);
  expect(!tp::auditionSounding(id4), "preview: an unused request is dropped");
}

// The key strip lights the keys that sound: its keys map to the pitches notes arrive with.
void testKeyLights() {
  expect(tp::stripKeyPitch(48) == 60 && tp::stripKeyPitch(0) == 12 && tp::stripKeyPitch(95) == 107, "key strip: C4 is middle C, C0 to B7");
  NoteActivity na;
  keylight::ChannelLights lights;
  lights.update(na, 0.016f);
  na.on(1, 60.0f, 0.8f);  // a chord
  na.on(2, 64.0f, 0.8f);
  na.on(3, 67.0f, 0.8f);
  lights.update(na, 0.016f);
  expect(lights.key(tp::stripKeyPitch(48)) == 1.0f && lights.key(tp::stripKeyPitch(52)) == 1.0f && lights.key(tp::stripKeyPitch(55)) == 1.0f &&
             lights.key(tp::stripKeyPitch(50)) == 0.0f,
         "key strip: every held key of a chord lights, the others stay dark");
  na.off(1);
  lights.update(na, 0.016f);
  const float fading = lights.key(tp::stripKeyPitch(48));
  for (int i = 0; i < 20; ++i) lights.update(na, 0.016f);
  expect(fading > 0.0f && fading < 1.0f && lights.key(tp::stripKeyPitch(48)) == 0.0f && lights.key(tp::stripKeyPitch(52)) == 1.0f,
         "key strip: a released key fades out promptly, held ones stay lit");
}

// "Add file" and a drop share addSampleFiles: files become samples in free slots, named after the
// file, the first taking every key; a file that does not load is skipped; it stops when full.
void testAddFiles(const std::string& dir) {
  const std::string a = writeFile(dir, "kick one.wav", sine(6000, 441.0, 0.5));
  const std::string b = writeFile(dir, "snare.wav", sine(3000, 882.0, 0.5), 22050);
  const std::string bad = (fs::u8path(dir) / "nothing.wav").u8string();
  tp::Edit e = tp::readEdit("", 0);
  int added = -1;
  bool full = true;
  const int first = tp::addSampleFiles(e, {a, bad, b}, &added, &full);
  expect(first == 0 && added == 2 && !full, "add file: two files added, the missing one skipped");
  expect(e.text.sampleNames[0] == "kick one" && e.text.sampleNames[1] == "snare", "add file: named after the files");
  bool allFirst = true;
  for (int k = 0; k < 96; ++k) allFirst = allFirst && e.text.def.keymap[static_cast<size_t>(k)] == 0;
  expect(allFirst, "add file: the first sample takes every key");
  expect(e.text.def.samples[1].length == 3000, "add file: the file's length");
  // The same text as the drop path builds by hand (addSample with the file's own frames and rate).
  tp::Edit hand = tp::readEdit("", 0);
  hand.text.def = tp::defaultDefinition();
  SamplePtr sa = sampleCache().get(a), sb = sampleCache().get(b);
  tp::addSample(hand, a, static_cast<int32_t>(sa->frames()), sa->sampleRate, "kick one");
  tp::addSample(hand, b, static_cast<int32_t>(sb->frames()), sb->sampleRate, "snare");
  expect(tp::writeEdit(hand) == tp::writeEdit(e), "add file: the same instrument a drop makes");
  // It plays: a fresh instance with the added file sounds.
  expect(peak(play(withData(tp::writeEdit(e)), 60, 4000, 256).L, 0, 4000) > 0.1, "add file: the instrument plays the file");
  // A full instrument takes no more and says so.
  tp::Edit many = tp::readEdit("", 0);
  std::vector<std::string> lots(trk::kMaxSamples + 2, a);
  const int f2 = tp::addSampleFiles(many, lots, &added, &full);
  expect(f2 == 0 && added == trk::kMaxSamples && full, "add file: stops at 16 samples and reports full");
  expect(tp::addSampleFiles(many, {b}, &added, &full) == -1 && added == 0 && full, "add file: nothing added when full");
}

// A strip key held while the editor closes: the next frame's sweep sends its note off and the
// instrument falls silent; a preview is released the same way (its audition dropped too).
void testHeldRelease(const std::string& dir) {
  const std::string path = writeFile(dir, "held.wav", sine(4410, 441.0, 0.5));
  tp::Edit e = oneSample(path);
  e.text.def.samples[0].loop = trk::kLoopForward;  // keeps sounding while held
  e.text.def.samples[0].loopStart = 0;
  e.text.def.samples[0].loopLength = 4410;
  const PluginState st = withData(tp::writeEdit(e));
  ChannelAux aux;
  packs::prepareAux(st, aux);
  auto g = packs::createGenerator(st.type);
  ProcessContext ctx;
  ctx.sampleRate = 44100;
  ctx.tempo = 125;
  ctx.ppq = 96;
  std::vector<float> L(512), R(512);
  auto block = [&]() {
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    g->process(st, aux, ctx, L.data(), R.data(), 512);
    return peak(L, 0, 512);
  };
  tp::NoteGuard guard;
  const uint64_t editor = 77;
  const int pitch = tp::stripKeyPitch(48);
  // The press (frame 10): the note starts and the guard holds it.
  guard.hold(tp::NoteGuard::kStrip, editor, 3, pitch, 0, 10);
  const uint32_t noteId = 7;  // the engine's id for the note the press started
  g->noteOn(noteId, pitch, 100.0f / 127.0f, 0);
  int offs = 0;
  auto off = [&](const tp::HeldNote& h) {
    ++offs;
    expect(h.channel == 3 && h.pitch == pitch && h.serial == 0, "held key: the guard gives back the pressed note");
    g->noteOff(noteId, 0);
  };
  // Frames 11 and 12: the editor shows the held key; the sweeps keep it.
  for (int f = 11; f <= 12; ++f) {
    guard.sweep(f, off);
    guard.seen(tp::NoteGuard::kStrip, f);
    block();
  }
  expect(offs == 0 && guard.get(tp::NoteGuard::kStrip).held() && block() > 0.2, "held key: sounds while the editor shows it");
  // Frame 13: the editor is closed (nothing marks the key); frame 14's sweep releases it.
  guard.sweep(13, off);
  expect(offs == 0, "held key: one frame without the editor is not yet a release");
  guard.sweep(14, off);
  expect(offs == 1 && !guard.get(tp::NoteGuard::kStrip).held(), "held key: closing the editor sends the note off");
  block();
  block();
  double after = 0;
  for (int i = 0; i < 8; ++i) after = std::max(after, block());
  expect(after == 0.0, "held key: no note sounds after the editor closed");
  // A preview held by one editor ends with releaseAll (its audition serial goes along).
  guard.hold(tp::NoteGuard::kPreview, editor, 3, 60, 42, 20);
  uint32_t gotSerial = 0;
  guard.releaseAll(editor, [&](const tp::HeldNote& h) { gotSerial = h.serial; });
  expect(gotSerial == 42 && !guard.get(tp::NoteGuard::kPreview).held(), "preview: released with its audition");
  // Another editor's notes stay held.
  guard.hold(tp::NoteGuard::kStrip, 5, 1, 60, 0, 30);
  int other = 0;
  guard.releaseAll(editor, [&](const tp::HeldNote&) { ++other; });
  expect(other == 0 && guard.get(tp::NoteGuard::kStrip).held(), "held key: another editor's note stays");
}

}  // namespace

int main() {
  testKeyLights();
  const std::string dir = tempDir("editor");
  testSilentAndOld();
  testSettings(dir);
  testWaveEdits();
  testCommitAndUndo(dir);
  testPreview(dir);
  testAddFiles(dir);
  testHeldRelease(dir);
  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
  return finish();
}
