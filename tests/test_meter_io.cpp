// Gridline — meter changes across the song (K-179), engine and file side: the metronome's
// accents, the song loop end, the position display, MIDI export (the leading meter of a range
// that starts after a change) and import (markers, or one pattern per meter section), the FL
// project round trip of pattern meters and meter changes, WAV cue names and the 'acid' meter,
// and the render: a meter is notation, so meter changes never change the sound.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/Log.h"
#include "collab/Session.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MenuParity.h"
#include "core/Meter.h"
#include "core/MeterEdit.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ExportJob.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

Marker meterMarker(Tick pos, int num, int den, const std::string& name = "") {
  Marker m;
  m.pos = pos;
  m.timeSigNum = num;
  m.timeSigDen = den;
  m.name = name;
  return m;
}

// 4/4 for 2 bars, 7/8 for 2 bars, then 6/8 (96 PPQ: 7/8 = 336 ticks, 6/8 = 288).
Song sevenThenSix() {
  Song s = makeEmptySong();
  s.markers.push_back(meterMarker(2 * 384, 7, 8));
  s.markers.push_back(meterMarker(2 * 384 + 2 * 336, 6, 8));
  return s;
}

// A song with one synth channel, a pattern of notes and two clips of it.
Song notesSong() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Lead", "gl.synth");
  const ObjectId pat = d.song().patterns.front().id;
  std::vector<Note> notes;
  for (int i = 0; i < 8; ++i) {
    Note n;
    n.start = i * 96;
    n.length = 80;
    n.pitch = 60 + (i * 5) % 12;
    notes.push_back(n);
  }
  d.setNotes(pat, ch, notes);
  d.addClip(pat, 0, 0);
  d.addClip(pat, 0, 4 * 384);
  return d.song();
}

void metronome() {
  Song s = sevenThenSix();
  ProgramBuilder b;
  auto prog = b.build(s);
  expect(prog && !prog->meter.constant(), "the program carries the song's meter map");
  std::vector<MetronomeBeat> beats;
  for (Tick t = 0;;) {
    const MetronomeBeat mb = metronomeBeat(prog.get(), nullptr, true, t);
    if (mb.tick >= 2 * 384 + 2 * 336 + 288) break;
    beats.push_back(mb);
    t = mb.tick + 1;
  }
  // 4/4: 8 quarter beats; 7/8: 14 eighth beats; 6/8: 6 eighth beats.
  expect(beats.size() == 8u + 14u + 6u, "one click per beat in each section (" + std::to_string(beats.size()) + ")");
  if (beats.size() < 8u + 14u + 6u) return;
  expect(beats[0].accent == 2 && beats[1].accent == 0 && beats[4].accent == 2, "4/4: an accent on each bar start");
  expect(beats[8].tick == 768 && beats[8].accent == 2 && beats[9].tick == 768 + 48 && beats[15].accent == 2,
         "7/8: clicks every eighth, accents on bar starts");
  const size_t six = 8 + 14;
  expect(beats[six].tick == 768 + 672 && beats[six].accent == 2 && beats[six + 3].accent == 1 && beats[six + 1].accent == 0,
         "6/8: a bar accent, then a group accent on the fourth eighth");
  // Pattern mode: a pattern with its own 6/8 meter.
  Song p = makeEmptySong();
  p.patterns.front().meterNum = 6;
  p.patterns.front().meterDen = 8;
  auto pp = b.build(p);
  const ProgPattern* pat = pp->pattern(p.patterns.front().id);
  expect(pat && pat->bar == 288 && pat->beat == 48 && pat->group == 3, "a pattern's own meter reaches the program");
  if (pat) {
    expect(metronomeBeat(pp.get(), pat, false, 1).tick == 48 && metronomeBeat(pp.get(), pat, false, 144).accent == 1 &&
               metronomeBeat(pp.get(), pat, false, 288).accent == 2,
           "pattern mode clicks in the pattern's meter");
  }
  // A plain 4/4 song: the old click (a beat every quarter, accent on bar starts).
  Song plain = makeEmptySong();
  auto pl = b.build(plain);
  bool same = true;
  for (Tick t = 0; t < 4 * 384; t += 7) {
    const MetronomeBeat mb = metronomeBeat(pl.get(), nullptr, true, t);
    const Tick want = (t + 95) / 96 * 96;
    same = same && mb.tick == want && mb.accent == (want % 384 == 0 ? 2 : 0);
  }
  expect(same, "a song without meter changes clicks as before");
}

void loopEndAndTime() {
  Song s = sevenThenSix();
  Document d(s);
  d.addClip(d.song().patterns.front().id, 0, 2 * 384 + 100);  // ends inside the first 7/8 bar
  ProgramBuilder b;
  auto prog = b.build(d.song());
  expect(prog->songLoopEnd == 2 * 384 + 336 || prog->songLoopEnd == prog->meter.roundUpToBar(prog->songEnd),
         "the song loop end is a bar line of the meter map");
  expect(prog->meter.roundUpToBar(prog->songLoopEnd) == prog->songLoopEnd, "the loop end sits on a bar line");
  // Position display: bar 4 starts at the second 7/8 bar.
  const std::string at = ops::formatSongTime(s, 2 * 384 + 336, ops::TimeFormat::BarBeatTick);
  expect(at.find("4:01") != std::string::npos || at.find("004:01") != std::string::npos,
         "the position display counts bars of the meter map (" + at + ")");
  const std::string six = ops::formatSongTime(s, 2 * 384 + 2 * 336 + 48 * 3, ops::TimeFormat::BarBeatTick);
  expect(six.find("5:04") != std::string::npos, "beats in 6/8 are eighths (" + six + ")");
  // A plain song shows exactly what it showed before.
  Song plain = makeEmptySong();
  expect(ops::formatSongTime(plain, 8 * 384, ops::TimeFormat::BarBeatTick) == ops::formatSongTime(plain, 8.0 * 384, ops::TimeFormat::BarBeatTick),
         "plain songs format as before");
}

void midiExport() {
  Song s = notesSong();
  s.markers.push_back(meterMarker(2 * 384, 7, 8));
  s.markers.push_back(meterMarker(2 * 384 + 2 * 336, 6, 8));
  // The bug: a range starting after the 7/8 change wrote 4/4 as its leading meter.
  MidiFileData r = songRangeToMidi(s, 2 * 384 + 336, 6 * 384);
  expect(r.timeSigNum == 7 && r.timeSigDen == 8, "a range export starting in 7/8 begins in 7/8");
  expect(!r.timeSigs.empty() && r.timeSigs[0].tick == 0 && r.timeSigs[0].num == 7, "the leading 7/8 sits at tick 0");
  MidiFileData back;
  std::string err;
  expect(parseMidi(encodeMidi(r), back, &err) && back.timeSigNum == 7 && back.timeSigDen == 8, "the file reads back as 7/8 (" + err + ")");
  // A range starting in 6/8 has one meter.
  MidiFileData r6 = songRangeToMidi(s, 2 * 384 + 2 * 336, 6 * 384);
  expect(r6.timeSigNum == 6 && r6.timeSigDen == 8, "a range export starting in 6/8 begins in 6/8");
  // Whole song: 4/4 then the changes.
  MidiFileData whole = songToMidi(s);
  expect(whole.timeSigNum == 4 && whole.timeSigs.size() >= 2, "the whole song exports its meter changes");
  // A pattern with its own meter exports it.
  Song p = notesSong();
  p.patterns.front().meterNum = 5;
  p.patterns.front().meterDen = 4;
  MidiFileData pm = patternToMidi(p, p.patterns.front().id);
  expect(pm.timeSigNum == 5 && pm.timeSigDen == 4, "a pattern exports its own meter");
}

void midiImport() {
  // A file in 4/4, a change to 7/8 inside bar 2 (a short bar), then 6/8.
  MidiFileData m;
  m.division = 96;
  m.timeSigNum = 4;
  m.timeSigDen = 4;
  m.timeSigs = {{0, 4, 4}, {384 + 192, 7, 8}, {384 + 192 + 336, 6, 8}};
  MidiTrack t;
  t.name = "Keys";
  for (int i = 0; i < 16; ++i) t.notes.push_back({static_cast<int64_t>(i) * 96, 90, 60 + i % 7, 100, 0});
  m.tracks.push_back(t);
  Song s = makeEmptySong();
  const MidiImportResult res = importMidiIntoSong(s, m, "Song");
  expect(res.pattern != kNoId && res.meterChanges == 2, "import: two meter changes placed as markers");
  int meters = 0;
  bool shortKept = false;
  for (const auto& mk : s.markers)
    if (mk.isMeter()) ++meters, shortKept = shortKept || (mk.pos == 384 + 192 && mk.timeSigNum == 7);
  expect(meters == 2 && shortKept, "import: a mid-bar change stays where it is (a short bar)");
  const MeterMap map(s);
  expect(map.shortBar(384) && map.barIndex(384 + 192) == 2, "import: the short bar keeps its number, the next bar starts at the change");
  // The round trip through a file keeps the three meters.
  MidiFileData out = songToMidi(s);
  MidiFileData back;
  expect(parseMidi(encodeMidi(out), back) && back.timeSigs.size() >= 3, "import then export keeps the three meters");
  // Split per meter section.
  Song sp = makeEmptySong();
  MidiImportOptions o;
  o.splitAtMeterChanges = true;
  const MidiImportResult rs = importMidiIntoSong(sp, m, "Song", o);
  expect(rs.sections.size() == 3, "split import: one pattern per meter section (" + std::to_string(rs.sections.size()) + ")");
  if (rs.sections.size() == 3) {
    const Pattern* p2 = sp.pattern(rs.sections[1]);
    const Pattern* p3 = sp.pattern(rs.sections[2]);
    expect(p2 && sp.patternMeterNum(*p2) == 7 && sp.patternMeterDen(*p2) == 8 && p2->stepsPerBar == 14, "split import: the 7/8 section's pattern is in 7/8");
    expect(p3 && sp.patternMeterNum(*p3) == 6 && p3->stepsPerBar == 12, "split import: the 6/8 section's pattern is in 6/8");
    int notes = 0;
    for (ObjectId id : rs.sections)
      if (const Pattern* p = sp.pattern(id))
        for (const auto& kv : p->notes) notes += static_cast<int>(kv.second.size());
    expect(notes == 16, "split import: every note lands in one section");
    bool clipsAtSections = true;
    for (const auto& c : sp.playlist)
      if (c.kind == ClipKind::Pattern && c.pattern == rs.sections[1]) clipsAtSections = clipsAtSections && c.start == 384 + 192;
    expect(clipsAtSections, "split import: each section's clip starts at its meter change");
  }
}

void flpRoundTrip() {
  Song s = notesSong();
  s.markers.push_back(meterMarker(2 * 384, 7, 8));
  s.markers.push_back(meterMarker(2 * 384 + 2 * 336, 6, 8, "Coda"));
  Document d(s);
  const ObjectId seven = d.addPattern("Seven", 2, 14, -1, 7, 8);
  Song src = d.song();
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(src, bytes, rep, eo), "flp: exported");
  Song imp;
  flp::Report ir;
  std::string err;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported (" + err + ")");
  int meters = 0;
  bool coda = false;
  for (const auto& m : imp.markers) {
    if (!m.isMeter()) continue;
    ++meters;
    coda = coda || (m.pos == 2 * 384 + 2 * 336 && m.timeSigNum == 6 && m.timeSigDen == 8);
  }
  expect(meters == 2 && coda, "flp: meter changes come back");
  bool own = false, plainFollows = true;
  for (const auto& p : imp.patterns) {
    if (p.name == "Seven") own = imp.patternMeterNum(p) == 7 && imp.patternMeterDen(p) == 8 && p.ownMeter();
    else plainFollows = plainFollows && !p.ownMeter();
  }
  expect(own, "flp: a pattern's own meter comes back");
  expect(plainFollows, "flp: patterns in the song's meter still follow the song");
  (void)seven;
}

void wavCues() {
  Song s = notesSong();
  s.markers.push_back(meterMarker(2 * 384, 7, 8));
  s.markers.push_back(meterMarker(2 * 384 + 2 * 336, 6, 8, "Coda"));
  RenderJob job;
  job.sampleRate = 48000;
  const auto cues = scopeMarkers(s, job, 0, 8 * 384);
  bool seven = false, coda = false;
  for (const auto& c : cues) seven = seven || c.name == "7/8", coda = coda || c.name == "Coda (6/8)";
  expect(cues.size() == 2 && seven && coda, "wav: meter changes are cue points named by their meter");
}

double maxDiff(const AudioBuffer& a, const AudioBuffer& b, size_t& frames) {
  frames = std::min(a.frames, b.frames);
  if (a.channels != b.channels) return 1.0;
  double d = 0;
  for (size_t c = 0; c < a.channels; ++c)
    for (size_t i = 0; i < frames; ++i)
      d = std::max(d, std::fabs(static_cast<double>(a.data[c * a.frames + i]) - static_cast<double>(b.data[c * b.frames + i])));
  return d;
}

void renderIdentity() {
  const Song plain = notesSong();
  RenderOptions ro;
  ro.sampleRate = 44100;
  const AudioBuffer a = renderSong(plain, ro);
  // A meter is notation: meter changes leave every sample as it was.
  Song metered = plain;
  metered.markers.push_back(meterMarker(2 * 384, 7, 8));
  metered.markers.push_back(meterMarker(2 * 384 + 2 * 336, 6, 8));
  const AudioBuffer b = renderSong(metered, ro);
  size_t frames = 0;
  expect(a.frames > 0 && maxDiff(a, b, frames) == 0.0 && frames > 44100, "render: meter changes do not change the sound");
  // A change to the meter already in force is merged away: the render and the map are the old ones.
  Song same = plain;
  same.markers.push_back(meterMarker(4 * 384, 4, 4));
  const AudioBuffer c = renderSong(same, ro);
  expect(c.frames == a.frames && maxDiff(a, c, frames) == 0.0, "render: a same-meter marker changes nothing");
  expect(MeterMap(same).constant(), "a same-meter marker leaves the map constant");
  // Pattern meters are notation too.
  Song pm = plain;
  pm.patterns.front().meterNum = 2;  // half the bar, twice the bars: the same length in ticks
  pm.patterns.front().meterDen = 4;
  pm.patterns.front().bars = plain.patterns.front().bars * 2;
  const AudioBuffer e = renderSong(pm, ro);
  expect(maxDiff(a, e, frames) == 0.0, "render: a pattern's own meter with the same length sounds the same");
}

void plainProjectUnchanged() {
  // A plain song saves byte for byte as before: the same JSON as a song that never saw the meter code
  // paths (no meter keys), and loading and saving again gives the same bytes.
  const Song s = notesSong();
  const std::string a = jsonCompact(songToJson(s));
  Song back;
  expect(songFromJson(songToJson(s), back) && jsonCompact(songToJson(back)) == a, "a plain project saves the same after a load");
  expect(a.find("meter") == std::string::npos, "a plain project writes no meter keys");
  // The old fixtures load with a constant map and save without meter keys, the same after a reload.
  for (const char* path : {"tests/fixtures/v1_song.json", "tests/fixtures/test_song.gridline.json"}) {
    Song old;
    std::string err;
    if (!loadProject(path, old, &err)) {
      expect(false, std::string("fixture loads: ") + path + " (" + err + ")");
      continue;
    }
    bool follows = true;
    for (const auto& p : old.patterns) follows = follows && !p.ownMeter();
    const std::string once = jsonCompact(songToJson(old));
    Song again;
    expect(follows && songFromJson(songToJson(old), again) && jsonCompact(songToJson(again)) == once &&
               once.find("meterNum") == std::string::npos && once.find("meterForPatterns") == std::string::npos,
           std::string("an old project saves as before: ") + path);
  }
}

void collabRepair() {
  expect(collab::kProtocolVersion >= 4, "collab: the protocol version is raised (older peers are told on join)");
  // A peer's project with a broken pattern meter is repaired before it loads.
  Song s = makeEmptySong();
  s.patterns.front().meterNum = 7;
  s.patterns.front().meterDen = 8;
  Json j = songToJson(s);
  for (auto& jp : j.object["patterns"].array) {
    jp.object["meterNum"].number = 1e9;
    jp.object["meterDen"].number = -5;
  }
  collab::repairProjectJson(j);
  Song back;
  expect(songFromJson(j, back), "collab: a repaired project loads");
  const Pattern& p = back.patterns.front();
  expect(p.meterNum >= 0 && p.meterNum <= 32 && p.meterDen >= 0 && p.meterDen <= 32 && back.patternBarTicks(p) > 0,
         "collab: a broken pattern meter is clamped to something playable");
}

}  // namespace

int main() {
  test::hermeticTestEnv("meter_io");
  metronome();
  loopEndAndTime();
  midiExport();
  midiImport();
  flpRoundTrip();
  wavCues();
  renderIdentity();
  plainProjectUnchanged();
  collabRepair();
  return finish();
}
