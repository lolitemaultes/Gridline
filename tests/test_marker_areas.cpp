// Gridline tests: Export marker areas (engine/MarkerAreas.h). The areas of an arrangement
// (no markers, one marker, content before the first marker, empty areas, markers at one
// position, meter and key markers, the song end, the Clips layout's slot clips), their file
// names (pattern, unsafe characters, reserved names, repeats), and an export of a small
// project with three areas: each file's length, no sound of one area in another's file, tails,
// stems in a folder per area, Track Receipt sidecars per file, Cancel keeping finished areas.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Project.h"
#include "core/TrackReceipt.h"
#include "engine/Export.h"
#include "engine/ExportJob.h"
#include "engine/MarkerAreas.h"
#include "engine/Sample.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

std::string scratch;

std::string impulseWav() {
  static std::string p;
  if (p.empty()) {
    AudioBuffer b(2, 2000);
    b.plane(0)[0] = 1.0f;
    b.plane(1)[0] = 1.0f;
    p = (fs::path(scratch) / "impulse.wav").string();
    writeWavFile(p, b, 44100, 32);
  }
  return p;
}

// 120 BPM, 96 PPQ: one beat is 0.5 s (22050 frames at 44.1 kHz).
Song baseSong() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  s.ppq = 96;
  return s;
}

ObjectId addSampler(Song& s, ObjectId mixer = kNoId) {
  Channel c;
  c.id = s.allocateId();
  c.name = "Click";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = impulseWav();
  c.mixerTrack = mixer == kNoId ? s.masterId() : mixer;
  s.channels.push_back(c);
  return c.id;
}

ObjectId addPattern(Song& s, const std::string& name) {
  Pattern p;
  p.id = s.allocateId();
  p.name = name;
  s.patterns.push_back(p);
  return p.id;
}

void addClip(Song& s, ObjectId pat, Tick start, Tick len, int track = 0) {
  Clip c;
  c.id = s.allocateId();
  c.pattern = pat;
  c.track = track;
  c.start = start;
  c.length = len;
  s.playlist.push_back(c);
}

void addMarker(Song& s, Tick pos, const std::string& name) {
  Marker m;
  m.pos = pos;
  m.name = name;
  s.markers.push_back(m);
  std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
}

std::vector<int64_t> onsets(const Sample& x, float thr = 0.01f) {
  std::vector<int64_t> o;
  bool on = false;
  for (size_t i = 0; i < x.frames(); ++i) {
    const bool loud = std::fabs(x.left[i]) > thr;
    if (loud && !on) o.push_back(static_cast<int64_t>(i));
    on = loud;
  }
  return o;
}

// A song with a clip of one pattern at each of [0, 384), [384, 768), [768, 960) (one bar, one
// bar, half a bar) and the markers given.
Song threeSongs(ObjectId* chOut = nullptr) {
  Song s = baseSong();
  const ObjectId ch = addSampler(s);
  if (chOut) *chOut = ch;
  const ObjectId p1 = addPattern(s, "One"), p2 = addPattern(s, "Two"), p3 = addPattern(s, "Three");
  s.pattern(p1)->notes[ch].push_back(Note(0, 24, 60, 100));
  s.pattern(p2)->notes[ch].push_back(Note(0, 24, 60, 100));
  s.pattern(p3)->notes[ch].push_back(Note(0, 24, 60, 100));
  s.pattern(p3)->notes[ch].push_back(Note(48, 24, 60, 100));
  addClip(s, p1, 0, 384);
  addClip(s, p2, 384, 384);
  addClip(s, p3, 768, 192);
  return s;
}

void testAreas() {
  {
    const Song s = threeSongs();
    expect(markerAreas(s).empty(), "areas: no markers, no areas");
  }
  {
    Song s = threeSongs();
    addMarker(s, 0, "All");
    const auto a = markerAreas(s);
    expect(a.size() == 1 && a[0].start == 0 && a[0].end == 960 && a[0].name == "All" && a[0].marker == 0 && a[0].hasContent,
           "areas: one marker at the start, one area to the song end");
  }
  {
    Song s = threeSongs();
    addMarker(s, 384, "Second");
    const auto a = markerAreas(s);
    expect(a.size() == 2 && a[0].start == 0 && a[0].end == 384 && a[0].marker == -1 && a[0].name == "Start" && a[0].hasContent && a[1].start == 384 &&
               a[1].end == 960 && a[1].name == "Second",
           "areas: content before the first marker is an area of its own");
  }
  {
    Song s = threeSongs();
    s.playlist.erase(s.playlist.begin());  // nothing before bar 2 now
    addMarker(s, 384, "Second");
    const auto a = markerAreas(s);
    expect(a.size() == 1 && a[0].start == 384 && a[0].end == 960, "areas: an empty stretch before the first marker is no area");
  }
  {
    Song s = threeSongs();
    s.playlist[0].muted = true;  // a muted clip does not count as content
    addMarker(s, 384, "Second");
    expect(markerAreas(s).size() == 1, "areas: a muted clip before the first marker is no content");
  }
  {
    Song s = threeSongs();
    addMarker(s, 0, "One");
    addMarker(s, 384, "Two");
    addMarker(s, 768, "Three");
    const auto a = markerAreas(s);
    expect(a.size() == 3 && a[0].end == 384 && a[1].start == 384 && a[1].end == 768 && a[2].start == 768 && a[2].end == 960 && a[2].name == "Three",
           "areas: three markers, three areas, the last to the song end");
  }
  {
    Song s = threeSongs();
    s.playlist.erase(s.playlist.begin() + 1);  // bar 2 is silent
    addMarker(s, 0, "One");
    addMarker(s, 384, "Gap");
    addMarker(s, 768, "Three");
    const auto a = markerAreas(s);
    expect(a.size() == 3 && a[0].hasContent && !a[1].hasContent && a[1].name == "Gap" && a[2].hasContent, "areas: an empty area is listed, marked empty");
  }
  {
    Song s = threeSongs();
    addMarker(s, 0, "");
    addMarker(s, 384, "Two");
    addMarker(s, 384, "");
    addMarker(s, 384, "Two again");
    const auto a = markerAreas(s);
    expect(a.size() == 2 && a[0].name == "Marker 1" && a[1].start == 384 && a[1].end == 960 && a[1].name == "Two" && a[1].marker == 1,
           "areas: markers at one position open one area (the first named one names it); an unnamed marker is Marker N");
  }
  {
    Song s = threeSongs();
    addMarker(s, 0, "One");
    Marker meter;
    meter.pos = 192;
    meter.timeSigNum = 3;
    meter.timeSigDen = 4;
    s.markers.push_back(meter);
    Marker key;
    key.pos = 480;
    key.name = "D minor";
    key.keyRoot = 2;
    s.markers.push_back(key);
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
    const auto a = markerAreas(s);
    expect(a.size() == 1 && a[0].start == 0 && a[0].end == 960, "areas: meter and key markers do not split areas");
  }
  {
    Song s = threeSongs();
    addMarker(s, 0, "One");
    addMarker(s, 960, "End");   // at the song end: no area
    addMarker(s, 1500, "Far");  // after it: no area
    const auto a = markerAreas(s);
    expect(a.size() == 1 && a[0].end == 960, "areas: markers at or after the song end open no area");
  }
  {
    Song s = threeSongs();
    Marker loop;
    loop.pos = 384;
    loop.name = "Loop me";
    loop.action = MarkerAction::Loop;
    s.markers.push_back(loop);
    addMarker(s, 0, "One");
    expect(markerAreas(s).size() == 2, "areas: a marker with a playback action is an area marker");
  }
  {
    // Clips layout: slot clips before the live area's end are the Session's, not the arrangement's.
    Song s = threeSongs();
    s.live.areaEnd = 384;
    addMarker(s, 768, "Three");
    const auto a = markerAreas(s);
    expect(a.size() == 2 && a[0].start == 384 && a[0].end == 768 && a[0].marker == -1 && a[1].start == 768,
           "areas: the Session's slot clips are no content; the arrangement starts after them");
  }
  {
    // multiple arrangements: the current one's markers and clips (Song::markers / playlist)
    Song s = threeSongs();
    addMarker(s, 0, "One");
    Arrangement other;
    other.name = "Other";
    Marker m;
    m.pos = 384;
    m.name = "Other's marker";
    other.markers.push_back(m);
    s.arrangements = {Arrangement{}, other};
    s.currentArrangement = 0;
    expect(markerAreas(s).size() == 1, "areas: another arrangement's markers do not count");
  }
}

void testNames() {
  std::vector<MarkerArea> a(5);
  a[0].name = "Intro";
  a[1].name = "Verse/Chorus: 2?";
  a[2].name = "intro";
  a[3].name = "CON";
  a[4].name = "";
  const auto n = markerAreaFileNames(a, kMarkerAreaPattern, "My Album");
  expect(n.size() == 5 && n[0] == "My Album - Intro", "names: project - marker by default");
  expect(n[1] == "My Album - Verse_Chorus_ 2_", "names: separators and reserved characters become _");
  expect(n[2] == "My Album - intro 2", "names: a repeated name (ignoring case) is numbered");
  expect(n[4] == "My Album -", "names: trailing spaces go (an empty marker name)");
  const auto r = markerAreaFileNames({a[3]}, "%M", "x");
  expect(r.size() == 1 && r[0] == "CON_", "names: a reserved Windows name gets an underscore");
  const auto num = markerAreaFileNames({a[0], a[2]}, "%N %M (%R)", "x", 48000);
  expect(num.size() == 2 && num[0] == "01 Intro (48000)" && num[1] == "02 intro (48000)", "names: area number and sample rate tokens");
  const auto e = markerAreaFileNames({a[4], a[4]}, "%M", "x");
  expect(e.size() == 2 && e[0] == "untitled" && e[1] == "untitled 2", "names: an empty name is never empty on disk");
  MarkerArea longName;
  longName.name = std::string(400, 'a');
  expect(markerAreaFileNames({longName}, "%M", "x")[0].size() <= 180, "names: a very long name is shortened");
}

void testExport() {
  Song s = threeSongs();
  addMarker(s, 0, "Song One");
  addMarker(s, 384, "Song Two");
  addMarker(s, 768, "Song Three");
  const auto areas = markerAreas(s);
  const auto names = markerAreaFileNames(areas, kMarkerAreaPattern, "Album");
  ExportSettings es;
  es.folder = (fs::path(scratch) / "areas_cut").string();
  es.formats = {ExportFormat::Wav};
  es.wavBits = 32;
  es.render.tail = TailMode::Cut;
  es.projectTitle = "Album";
  double last = 0;
  std::vector<MarkerAreaResult> per;
  ExportOutcome o = runMarkerAreaExport(s, es, areas, names, [&](double p, const std::string&) { last = std::max(last, p); }, nullptr, &per);
  expect(o.errors.empty() && o.files.size() == 3 && per.size() == 3 && per[0].done && per[2].done, "export: one file per area");
  expect(std::fabs(last - 1.0) < 1e-9, "export: progress runs to 1 over the areas");
  const fs::path d(es.folder);
  const SamplePtr a1 = loadSample((d / "Album - Song One.wav").string()), a2 = loadSample((d / "Album - Song Two.wav").string()),
                  a3 = loadSample((d / "Album - Song Three.wav").string());
  expect(a1 && a2 && a3, "export: files named project - marker");
  if (!a1 || !a2 || !a3) return;
  expect(a1->frames() == 88200 && a2->frames() == 88200 && a3->frames() == 44100, "export: each file is its area's length (time 0 = area start)");
  const auto o1 = onsets(*a1), o2 = onsets(*a2), o3 = onsets(*a3);
  expect(o1.size() == 1 && o1[0] < 64, "export: area 1 holds only its own note");
  expect(o2.size() == 1 && o2[0] < 64, "export: area 2 holds only its own note, at its start");
  expect(o3.size() == 2 && o3[0] < 64 && std::llabs(o3[1] - 11025) < 64, "export: area 3 holds its two notes at their times");
  expect(fs::exists(receipt::originPath((d / "Album - Song Two.wav").string())), "export: a Track Receipt sidecar per area file");

  // Tails ring on, but never take the next area's notes.
  ExportSettings tl = es;
  tl.folder = (fs::path(scratch) / "areas_tail").string();
  tl.render.tail = TailMode::Leave;
  o = runMarkerAreaExport(s, tl, areas, names);
  const SamplePtr t1 = loadSample((fs::path(tl.folder) / "Album - Song One.wav").string());
  expect(o.errors.empty() && o.files.size() == 3 && t1 && t1->frames() >= 88200, "export: a Leave tail keeps at least the area");
  if (t1) {
    float after = 0;
    for (size_t i = 88200; i < t1->frames(); ++i) after = std::max(after, std::fabs(t1->left[i]));
    expect(onsets(*t1).size() == 1 && after < 0.01f, "export: the tail of area 1 has no note of area 2 in it");
  }

  // Only the ticked areas; MIDI per area too.
  ExportSettings some = es;
  some.folder = (fs::path(scratch) / "areas_some").string();
  some.midi = true;
  o = runMarkerAreaExport(s, some, {areas[1]}, {names[1]});
  expect(o.files.size() == 2 && fs::exists(fs::path(some.folder) / "Album - Song Two.wav") && fs::exists(fs::path(some.folder) / "Album - Song Two.mid") &&
             !fs::exists(fs::path(some.folder) / "Album - Song One.wav"),
         "export: only the chosen areas, with their MIDI");

  // Stems: a folder per area holding its mix and its parts.
  {
    Song st = s;
    st.channels[0].mixerTrack = st.mixer[1].id;
    st.mixer[1].name = "Clicks";
    ExportSettings sp = es;
    sp.folder = (fs::path(scratch) / "areas_stems").string();
    sp.splitMixer = true;
    o = runMarkerAreaExport(st, sp, areas, names);
    const fs::path f(sp.folder);
    expect(o.errors.empty() && fs::is_directory(f / "Album - Song One") && fs::exists(f / "Album - Song One" / "Album - Song One.wav") &&
               fs::exists(f / "Album - Song Three" / "Album - 01 Clicks.wav"),
           "export: with stems each area gets a folder of its own");
    const SamplePtr stem = loadSample((f / "Album - Song Three" / "Album - 01 Clicks.wav").string());
    expect(stem && stem->frames() == 44100 && onsets(*stem).size() == 2, "export: an area's stem is the area's length, its notes only");
  }

  // Cancel in the second area: the first stays, the second goes, the third never starts.
  {
    ExportSettings cn = es;
    cn.folder = (fs::path(scratch) / "areas_cancel").string();
    std::atomic<bool> cancel{false};
    o = runMarkerAreaExport(s, cn, areas, names, [&](double p, const std::string&) {
      if (p > 0.4) cancel = true;
    }, &cancel, &per);
    const fs::path f(cn.folder);
    expect(o.cancelled && fs::exists(f / "Album - Song One.wav") && !fs::exists(f / "Album - Song Two.wav") && !fs::exists(f / "Album - Song Three.wav"),
           "export: Cancel keeps the finished areas and stops");
    expect(o.files.size() == 1 && per.size() == 2 && per[0].done && !per[1].done, "export: the outcome lists the kept files only");
  }

  // The song itself is left as it was (exporting is no edit).
  Song before = s;
  (void)runMarkerAreaExport(s, es, areas, names);
  expect(before.playlist == s.playlist && before.markers == s.markers, "export: the project is not changed");
}

}  // namespace

int main() {
  scratch = (fs::path(gl::test::hermeticTestEnv("marker_areas")) / "work").lexically_normal().string();
  fs::create_directories(scratch);
  testAreas();
  testNames();
  testExport();
  return finish();
}
