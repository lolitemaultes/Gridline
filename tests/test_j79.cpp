// Gridline — J-79: playlist editing fixes and FL's audio clip editing toggles.
//  * right-click / delete-tool sweeps remove only the topmost clip at a point
//    (overlapping pattern, audio and automation clips), one undo step;
//  * the volume line: dB per pixel, the 0 dB detent, fine / free modifiers,
//    range, reset, labels, FLP @52 and project round trips;
//  * zero crossings: trim, resize, slip and slice of audio on a synthetic sine;
//  * stretch mode: the length changes as asked, the pitch stays (rendered);
//  * the toggles persist as global settings and have keymap commands.
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include "../src/core/ClipGain.h"
#include "../src/core/Document.h"
#include "../src/core/Keymap.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Project.h"
#include "../src/core/Settings.h"
#include "../src/core/Song.h"
#include "../src/core/WavWriter.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/ClipOps.h"
#include "../src/engine/PlaylistTools.h"
#include "../src/engine/Render.h"
#include "Check.h"
#include "flp_export/core.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;
constexpr double kHz = 441.0;  // period 100 samples: zero crossings at every 50th sample

std::string writeSine(const char* name, double seconds) {
  const std::string path = (fs::temp_directory_path() / name).string();
  AudioBuffer b(2, static_cast<size_t>(seconds * kRate));
  for (size_t i = 0; i < b.frames; ++i)
    b.at(0, i) = b.at(1, i) = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * kHz * static_cast<double>(i) / kRate));
  writeWav(path, b, kRate, 16);
  return path;
}

ObjectId addSampler(Song& s, const std::string& wav) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "sine";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}

ObjectId addAudio(Song& s, ObjectId chan, int track, Tick start) {
  Clip a;
  a.id = s.allocateId(), a.kind = ClipKind::Audio, a.source = chan, a.track = track, a.start = start;
  a.length = audioClipNaturalLength(s, a);
  s.playlist.push_back(a);
  return a.id;
}

// True when `sec` is a zero crossing of the synthetic sine: a whole sample at
// a multiple of 50 samples, or the sample right after it (16-bit rounding can
// leave the sample on the crossing a hair below zero, so the sign changes one
// sample later).
bool onCrossing(double sec) {
  const double smp = sec * kRate;
  if (std::fabs(smp - std::round(smp)) > 1e-6) return false;
  const long k = std::lround(smp) % 50;
  return k == 0 || k == 1;
}

const Clip* clipIn(const std::vector<Clip>& v, ObjectId id) {
  for (const auto& c : v)
    if (c.id == id) return &c;
  return nullptr;
}

// Frequency of a (roughly) sinusoidal span from its sign changes.
double frequency(const AudioBuffer& b, size_t i0, size_t i1) {
  int n = 0;
  for (size_t i = i0 + 1; i < i1; ++i) n += (b.at(0, i - 1) < 0) != (b.at(0, i) < 0);
  return n * 0.5 * kRate / static_cast<double>(i1 - i0);
}

void testTopmostDelete() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const std::string wav = writeSine("gridline_j79_sine.wav", 2.0);
  const ObjectId chan = addSampler(s, wav);
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.points = {{0, 0.0f, 0.0f, AutoShape::Curve}, {384, 1.0f, 0.0f, AutoShape::Curve}};
  s.automations.push_back(ac);
  // Track 0: a pattern clip [0, 384), an audio clip [192, 576) on it, an automation clip [288, 672) on top.
  Clip p;
  p.id = s.allocateId(), p.kind = ClipKind::Pattern, p.pattern = s.patterns[0].id, p.track = 0, p.start = 0, p.length = 384;
  s.playlist.push_back(p);
  const ObjectId A = addAudio(s, chan, 0, 192);
  Clip m;
  m.id = s.allocateId(), m.kind = ClipKind::Automation, m.source = ac.id, m.track = 0, m.start = 288, m.length = 384;
  s.playlist.push_back(m);
  Clip other = p;  // another track: never touched
  other.id = s.allocateId(), other.track = 1;
  s.playlist.push_back(other);
  expect(s.clip(A)->length == 384, "fixture: 2 s of audio = 384 ticks");

  expect(pltools::topClipAt(s, 0, 300) == m.id && pltools::topClipAt(s, 0, 250) == A && pltools::topClipAt(s, 0, 100) == p.id,
         "stacking: the clip that starts later lies on top (J-38 order)");
  expect(pltools::topClipAt(s, 0, 700) == kNoId && pltools::topClipAt(s, 1, 300) == other.id, "stacking: empty place / other track");
  {
    Song e = s;  // equal starts: the later one in the playlist is on top
    Clip twin = p;
    twin.id = e.allocateId();
    e.playlist.push_back(twin);
    expect(pltools::topClipAt(e, 0, 100) == twin.id, "stacking: equal starts -> later in the playlist on top");
  }

  // A right-click held still over three stacked clips (the view runs the sweep every frame).
  Document d(s);
  d.beginGroup("delete clips");
  pltools::EraseSweep sweep;
  int removed = 0;
  for (int frame = 0; frame < 30; ++frame) {
    const ObjectId id = sweep.pick(d.song(), 0, 300);
    if (id == kNoId) continue;
    sweep.add(d.song(), *d.song().clip(id));
    d.removeClip(id);
    ++removed;
  }
  d.endGroup();
  expect(removed == 1 && !d.song().clip(m.id) && d.song().clip(A) && d.song().clip(p.id),
         "right-click on overlapping clips deletes only the one on top (the clips beneath stay)");
  d.undo();
  expect(d.song().playlist.size() == 4, "undo brings it back in one step");

  // The view's hit test names the top clip; the sweep refuses the clip it uncovered at the same point.
  pltools::EraseSweep sw2;
  sw2.add(s, m);
  expect(sw2.pick(s, 0, 300, A) == kNoId && sw2.pick(s, 0, 250, A) == A, "hit-tested clip: refused under the deleted one, taken outside it");

  // A right-drag from 300 to 100 enters each clip in turn: every clip goes, one undo step.
  d.beginGroup("delete clips");
  pltools::EraseSweep sweep2;
  for (Tick t = 300; t >= 100; t -= 10) {
    const ObjectId id = sweep2.pick(d.song(), 0, t);
    if (id == kNoId) continue;
    sweep2.add(d.song(), *d.song().clip(id));
    d.removeClip(id);
  }
  d.endGroup();
  expect(d.song().playlist.size() == 1 && d.song().clip(other.id), "right-drag across: each clip it passes over goes (other tracks stay)");
  d.undo();
  expect(d.song().playlist.size() == 4, "the drag undoes in one step");

  // Hold still over the audio clip alone (outside the automation clip): it goes, the pattern beneath stays.
  pltools::EraseSweep sweep3;
  Song t = s;
  for (int frame = 0; frame < 5; ++frame)
    if (const ObjectId id = sweep3.pick(t, 0, 200); id != kNoId) {
      sweep3.add(t, *t.clip(id));
      t.playlist.erase(t.playlist.begin() + t.clipIndex(id));
    }
  expect(!t.clip(A) && t.clip(p.id) && t.clip(m.id), "audio clip over a pattern clip: only the audio clip goes");
}

void testGainLine() {
  using namespace clipgain;
  expect(fromDb(0) == 1.0f && toDb(1.0f) == 0.0, "0 dB is unity gain, exactly");
  expect(std::fabs(fromDb(36) - kMaxGain) < 1e-6f && fromDb(80) == kMaxGain, "top of the range: +36 dB (FL's 63.0957)");
  expect(fromDb(kMinDb - 1) == kSilentGain && silent(kSilentGain) && playGain(kSilentGain) == 0.0f && !silent(fromDb(-95.9)),
         "bottom: below -96 dB the clip is silent (FL's stored value, plays as 0)");
  expect(label(fromDb(-3.2)) == "-3.2 dB" && label(1.0f) == "0.0 dB" && label(fromDb(1.5)) == "+1.5 dB" && label(kSilentGain) == "-inf dB",
         "readout labels");
  bool rt = true;
  for (double db = -96; db <= 36; db += 0.7) rt = rt && std::fabs(toDb(fromDb(db)) - db) < 1e-4;
  expect(rt, "dB -> gain -> dB round trip over the range");

  // A drag, pixel by pixel like a pointer (returns the value at the end).
  auto steps = [](Drag& d, double& up, int dir, int n) {
    double db = d.db();
    for (int i = 0; i < n; ++i) db = d.move(up += dir);
    return db;
  };
  {
    Drag f;
    f.begin(0, false, true);
    double up = 0;
    expect(std::fabs(steps(f, up, 1, 10) - 2.0) < 1e-12, "drag: 0.2 dB per pixel, up = louder");
    f.begin(0, false, true);
    up = 0;
    expect(std::fabs(steps(f, up, -1, 16) + 3.2) < 1e-12, "drag down: quieter (16 px = -3.2 dB)");
    Drag fn;
    fn.begin(0, true, true);
    up = 0;
    expect(std::fabs(steps(fn, up, 1, 10) - 0.2) < 1e-12, "drag with the fine modifier: 0.02 dB per pixel");
  }
  {  // FL's own playlist self-test sequence ("Gain should pause at 0dB (1.0)").
    Drag d;
    double up = 0;
    d.begin(0);
    const double a1 = steps(d, up, -1, 10);
    expect(a1 < 0, "FL: 10 px down from 0 dB: below unity");
    d.begin(a1), up = 0;
    expect(steps(d, up, 1, 10) == 0.0, "FL: a new drag 10 px back up: exactly unity");
    d.begin(0), up = 0;
    const double a3 = steps(d, up, 1, 10);
    d.begin(a3), up = 0;
    expect(a3 > 0 && steps(d, up, -1, 10) == 0.0, "FL: up 10 then a new drag down 10: exactly unity");
    d.begin(0), up = 0;
    const double a5 = steps(d, up, 1, 1);
    expect(a5 > 0, "FL: one pixel from 0 dB leaves it (no pause at the start of a drag)");
    d.begin(a5), up = 0;
    expect(steps(d, up, -1, 3) == 0.0, "FL: crossing 0 dB within a drag pauses there");
    expect(steps(d, up, -1, 40) < 0, "FL: the pause ends (below unity)");
    expect(steps(d, up, 1, 40) == 0.0, "FL: coming back up pauses at 0 dB again");
    expect(steps(d, up, 1, 1) == 0.0 && steps(d, up, 1, 1) == 0.0, "FL: still paused two pixels later");
    expect(steps(d, up, 1, 40) > 0, "FL: then the drag passes unity");
    Drag fr;  // the free modifier: no pause
    fr.begin(0.2, false, true), up = 0;
    expect(steps(fr, up, -1, 2) < 0, "free modifier: no pause at 0 dB");
  }
  {
    Drag d;
    double up = 0;
    d.begin(0);
    expect(steps(d, up, 1, 10000) == kMaxDb, "drag clamps at +36 dB");
    expect(steps(d, up, -1, 5) < kMaxDb, "and comes straight back down (no hidden overshoot)");
    d.begin(0), up = 0;
    steps(d, up, -1, 10000);
    expect(d.gain() == kSilentGain && silent(d.gain()), "drag to the bottom: silent");
    expect(!silent(fromDb(steps(d, up, 1, 2))), "from silence a drag up brings the clip back at once");
  }
  Drag d0;
  d0.begin(0);

  // One undo step per drag (the view merges every frame of a drag under one key).
  Song s = makeEmptySong();
  const ObjectId chan = addSampler(s, writeSine("gridline_j79_sine.wav", 2.0));
  const ObjectId A = addAudio(s, chan, 0, 0);
  Document doc(s);
  for (int px = 1; px <= 30; ++px) {
    const float g = fromDb(d0.move(-px));
    doc.editClip(A, "clip volume", [g](Clip& x) { x.gain = g; }, 0x4741494Eull);
  }
  doc.endGesture();
  expect(std::fabs(toDb(doc.song().clip(A)->gain) + 6.0) < 1e-4, "a 30 px drag down from 0 dB lands on -6.0 dB");
  doc.undo();
  expect(doc.song().clip(A)->gain == 1.0f, "the whole drag undoes in one step");

  // FLP @52: the linear gain is written and read back exactly.
  Song f = coretest::featureSong();
  ObjectId kick = kNoId;
  for (const auto& ch : f.channels)
    if (ch.plugin.type == "gl.sampler") kick = ch.id;
  const float gains[] = {fromDb(-3.2), kMaxGain, kSilentGain, fromDb(7.3)};
  std::vector<ObjectId> ids;
  for (int i = 0; i < 4; ++i) {
    Clip c;
    c.id = f.allocateId(), c.kind = ClipKind::Audio, c.source = kick, c.track = 8 + i, c.start = 0, c.length = 384;
    c.gain = gains[i];
    f.playlist.push_back(c);
    ids.push_back(c.id);
  }
  Song b;
  flp::ExportReport rep;
  flp::Bytes bytes;
  const bool ok = kick != kNoId && coretest::exportImport(f, b, rep, &bytes);
  expect(ok, "FLP: song with clip gains exports and imports back");
  if (ok) {
    std::vector<float> back;
    for (const auto& c : b.playlist)
      if (c.kind == ClipKind::Audio && c.track >= 8 && c.track < 12) back.push_back(c.gain);
    bool same = back.size() == 4;
    for (size_t i = 0; same && i < 4; ++i) same = std::memcmp(&back[i], &gains[i], 4) == 0;
    expect(same, "FLP @52: -3.2 dB, +36 dB, silent and +7.3 dB come back bit-exact");
    // The raw records carry the same floats at @52.
    flp::File file;
    std::string err;
    int found = 0;
    if (flp::readBytes(bytes, file, &err))
      for (const auto& e : file.events)
        if (e.id == 233 && e.size % 80 == 0)
          for (uint32_t o = 0; o + 80 <= e.size; o += 80) {
            float v;
            std::memcpy(&v, e.data + o + 52, 4);
            for (float g : gains) found += std::memcmp(&v, &g, 4) == 0 && g != 1.0f;
          }
    expect(found >= 4, "FLP @52: the playlist records hold the gains as FL's linear floats");
    Song b2;
    flp::ExportReport rep2;
    expect(coretest::exportImport(b, b2, rep2) && flp::compareSongs(b, b2).empty(), "FLP: a second round trip is identical");
  }
  // Project files keep the whole range (+36 dB was clamped to +24 dB before).
  const std::string proj = (fs::temp_directory_path() / "gridline_j79_gain.gridline").string();
  Song pj = s;
  pj.clip(A)->gain = kMaxGain;
  Song pb;
  expect(saveProject(proj, pj) && loadProject(proj, pb) && pb.clip(A) && pb.clip(A)->gain == kMaxGain, "project file: +36 dB kept");
}

void testZeroCrossings() {
  Song s = makeEmptySong();
  s.tempo = 120.0;  // one tick = 1/192 s = 229.6875 samples: tick edges rarely hit a crossing
  const ObjectId chan = addSampler(s, writeSine("gridline_j79_sine.wav", 2.0));
  const ObjectId A = addAudio(s, chan, 0, 0);
  TempoMap tm;
  tm.build(s);
  const Clip a = *s.clip(A);

  auto trim = pltools::trimStarts(s, tm, s.playlist, {A}, 7, false, true);
  expect(onCrossing(clipIn(trim, A)->audioStart) && std::fabs(clipIn(trim, A)->audioStart - 7.0 / 192) <= pltools::kZeroCrossReach,
         "trim (left edge): the audio starts on the nearest zero crossing");
  auto plain = pltools::trimStarts(s, tm, s.playlist, {A}, 7, false, false);
  expect(!onCrossing(clipIn(plain, A)->audioStart), "trim without the toggle: the exact tick position");

  auto res = pltools::resizeEnds(s, s.playlist, {A}, -5, false, &tm, true);
  const Clip* r = clipIn(res, A);
  const double endSrc = (384.0 - 5) / 192;
  expect(r->length == 379 && onCrossing(r->audioEnd) && r->audioEnd <= endSrc + 1e-12 && endSrc - r->audioEnd <= pltools::kZeroCrossReach,
         "resize (right edge): the audio ends on the zero crossing at or before the new end");
  Song grown = s;
  grown.playlist = res;
  auto res2 = pltools::resizeEnds(grown, grown.playlist, {A}, 5, false, &tm, false);
  expect(clipIn(res2, A)->length == 384 && clipIn(res2, A)->audioEnd == 0, "growing back past the cut end opens the audio up again");
  auto res3 = pltools::resizeEnds(s, s.playlist, {A}, -5, false, &tm, false);
  expect(clipIn(res3, A)->audioEnd == a.audioEnd, "resize without the toggle leaves the audio end alone");

  Clip slipped = pltools::slipClip(s, tm, *clipIn(trim, A), 3, true);
  expect(onCrossing(slipped.audioStart), "slip: the new audio start is a zero crossing");
  Clip slipPlain = pltools::slipClip(s, tm, *clipIn(plain, A), 3, false);
  expect(!onCrossing(slipPlain.audioStart), "slip without the toggle: exact");

  Song cut = s;
  auto right = pltools::sliceClips(cut, {A}, 101, true);
  const Clip *L = cut.clip(A), *R = right.empty() ? nullptr : cut.clip(right[0]);
  expect(R && onCrossing(R->audioStart) && L->audioEnd == R->audioStart && R->audioStart <= 101.0 / 192 + 1e-12 &&
             101.0 / 192 - R->audioStart <= pltools::kZeroCrossReach,
         "slice: both parts meet on the zero crossing before the cut");
  Song cut2 = s;
  auto right2 = pltools::sliceClips(cut2, {A}, 101, false);
  expect(!right2.empty() && std::fabs(cut2.clip(right2[0])->audioStart - 101.0 / 192) < 1e-9, "slice without the toggle: at the tick");
  // Rendered: a slice on a crossing joins without a step.
  RenderOptions o;
  AudioBuffer out = renderSong(cut, o);
  float step = 0;
  for (size_t i = 22000; i < 24500 && i < out.frames; ++i) step = std::max(step, std::fabs(out.at(0, i) - out.at(0, i - 1)));
  expect(step < 0.05f, "render: the sliced sine plays on without a click at the cut");
}

void testStretch() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const ObjectId chan = addSampler(s, writeSine("gridline_j79_sine.wav", 2.0));
  const ObjectId A = addAudio(s, chan, 0, 0);
  const auto chans = pltools::resamplingChannels(s, s.playlist, {A});
  expect(chans.size() == 1 && *chans.begin() == chan, "a resampling channel is found for the stretched clip");
  Song st = s;
  pltools::keepPitch(st, chans);
  expect(st.channel(chan)->plugin.params[pp::SmpStretchMode] == 1.0f && pltools::resamplingChannels(st, st.playlist, {A}).empty(),
         "stretch mode switches the channel to its pitch-keeping time stretch");
  st.playlist = pltools::resizeEnds(st, st.playlist, {A}, 384, true);
  expect(st.clip(A)->length == 768 && std::fabs(st.clip(A)->stretch - 2.0) < 1e-12, "right edge +4 beats: twice as long, stretch x2");
  TempoMap tm;
  tm.build(st);
  auto lt = pltools::trimStarts(st, tm, st.playlist, {A}, 192, true);
  expect(clipIn(lt, A)->length == 576 && std::fabs(clipIn(lt, A)->stretch - 1.5) < 1e-12 && clipIn(lt, A)->start == 192,
         "left edge in stretch mode: shorter and faster, content kept whole");

  RenderOptions o;
  AudioBuffer kept = renderSong(st, o);
  const double fk = frequency(kept, kRate / 2, 3 * kRate);  // inside the 4 s clip
  double tail = 0;
  for (size_t i = 3 * kRate; i < static_cast<size_t>(3.9 * kRate) && i < kept.frames; ++i) tail = std::max(tail, static_cast<double>(std::fabs(kept.at(0, i))));
  expect(std::fabs(fk - kHz) < kHz * 0.03, "render: the stretched clip keeps its pitch (441 Hz)");
  expect(kept.frames >= static_cast<size_t>(3.9 * kRate) && tail > 0.1, "render: it plays for twice as long (sound at 3-3.9 s)");
  Song rs = s;  // resample (pitch follows speed): an octave down
  rs.playlist = pltools::resizeEnds(rs, rs.playlist, {A}, 384, true);
  AudioBuffer low = renderSong(rs, o);
  expect(std::fabs(frequency(low, kRate / 2, 3 * kRate) - kHz / 2) < kHz * 0.03, "control: without the switch the pitch would drop an octave");
}

void testToggles() {
  Settings a;
  expect(!a.getBool("playlist.stretchResize") && a.getBool("playlist.showFades") && !a.getBool("playlist.zeroCross"),
         "toggle defaults: stretch off, fade + volume handles on, zero crossings off");
  expect(a.set("playlist.stretchResize", true) && a.set("playlist.showFades", false) && a.set("playlist.zeroCross", true), "toggles set");
  const std::string path = (fs::temp_directory_path() / "gridline_j79_settings.json").string();
  expect(a.save(path), "settings saved");
  Settings b;
  expect(b.load(path) && b.getBool("playlist.stretchResize") && !b.getBool("playlist.showFades") && b.getBool("playlist.zeroCross"),
         "toggles persist (global settings, like FL's toolbar switches)");
  expect(keys::findCommand("playlist.stretchResize") && keys::findCommand("playlist.fades") && keys::findCommand("playlist.zeroCross"),
         "every toggle has a keymap command (tooltips name its key)");
  const keys::Command* fades = keys::findCommand("playlist.fades");
  expect(fades && !fades->defaults.empty(), "fade handles keep Shift+F");
}

}  // namespace

// GRIDLINE_J79_SHOWCASE=<dir>: writes <dir>/j79.gridline (overlapping audio
// clips of a decaying "drum" loop over a pattern clip) for screenshots.
void writeShowcase(const char* dir) {
  const std::string wav = std::string(dir) + "/j79_loop.wav";
  AudioBuffer b(2, static_cast<size_t>(2.0 * kRate));
  for (size_t i = 0; i < b.frames; ++i) {
    const double t = static_cast<double>(i) / kRate, beat = std::fmod(t, 0.5);
    const double hit = std::exp(-beat * 9.0) * std::sin(2 * M_PI * (60 + 120 * std::exp(-beat * 30)) * beat);
    const double hat = (std::fmod(t + 0.25, 0.5) < 0.04 ? 0.25 : 0.0) * std::sin(t * 9000.0) * std::exp(-std::fmod(t + 0.25, 0.5) * 60);
    b.at(0, i) = b.at(1, i) = static_cast<float>(0.8 * hit + hat);
  }
  writeWav(wav, b, kRate, 16);
  Song s = makeEmptySong();
  s.tempo = 120.0;
  const ObjectId chan = addSampler(s, wav);
  s.channels.back().name = "Drum loop";
  addAudio(s, chan, 0, 0);
  addAudio(s, chan, 0, 288);  // overlaps the first by a beat
  const ObjectId q = addAudio(s, chan, 1, 96);
  s.clip(q)->gain = clipgain::fromDb(-3.2);
  addAudio(s, chan, 2, 0);
  addAudio(s, chan, 2, 384);
  saveProject(std::string(dir) + "/j79.gridline", s);
}

int main() {
  if (const char* dir = std::getenv("GRIDLINE_J79_SHOWCASE")) writeShowcase(dir);
  testTopmostDelete();
  testGainLine();
  testZeroCrossings();
  testStretch();
  testToggles();
  return finish();
}
