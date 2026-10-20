// Gridline — FLP export tests: writer, sections, round trips (FlpWriter / FlpExport / FlpOrigin).
#pragma once
#include <cstdio>
#include <filesystem>
#include "enc_common.h"
#include "core/Demo.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/PluginParams.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpFx.h"
#include "core/flp/FlpWriter.h"

namespace coretest {

// Exports `s` (no FL origin) and imports the result.
inline bool exportImport(const gl::Song& s, gl::Song& back, gl::flp::ExportReport& rep, gl::flp::Bytes* bytes = nullptr) {
  gl::flp::ExportOptions o;
  o.useOrigin = false;
  gl::flp::Bytes out;
  if (!gl::flp::exportFlp(s, out, rep, o)) return false;
  if (bytes) *bytes = out;
  gl::flp::Report r;
  std::string err;
  return gl::importFlpBytes(out, back, r, &err, "", "", false);
}

inline bool has(const std::vector<std::string>& diffs, const std::string& prefix) {
  for (const auto& d : diffs)
    if (d.rfind(prefix, 0) == 0) return true;
  return false;
}

// A Gridline-native song using most of what the FL exporter writes.
inline gl::Song featureSong() {
  using namespace gl;
  Song s = makeEmptySong();
  s.info.title = "Export test";
  s.info.author = "Gridline";
  s.info.comments = "Line one\nLine two";
  s.tempo = 128.5;
  s.timeSigNum = 3;
  s.timeSigDen = 4;
  s.swing = 0.25f;
  s.masterPitch = -1.0f;
  // Mixer: inserts 1..3, insert 1 → 2 (send level), 2 → master, 3 sidechain into 2.
  while (s.mixer.size() < 4) {
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.mixer[0].id, 1.0f, false});
    s.mixer.push_back(t);
  }
  s.mixer[1].name = "Drums";
  s.mixer[1].color = 0x3366CC;
  // Values on FL's grids (fader word 9000, send word 6400) so they can be exact.
  s.mixer[1].volume = static_cast<float>(gl::flp::fx::glFaderFromGain(gl::flp::fx::flMixerGain(9000)));
  s.mixer[1].pan = -0.25f;
  s.mixer[1].stereoSep = 0.5f;
  s.mixer[1].invertPolarity = true;
  s.mixer[1].eqGain = {3.0f, -2.5f, 1.0f};
  s.mixer[1].sends = {{s.mixer[2].id, static_cast<float>(std::clamp(gl::flp::fx::flMixerGain(6400), 0.0, 1.25)), false}};
  s.mixer[3].sends = {{s.mixer[0].id, 1.0f, false}, {s.mixer[2].id, 1.0f, true}};
  s.mixer[2].muted = true;
  s.mixer[2].slots[0].plugin = defaultPluginState("gl.comp");  // a sidechain target needs an effect
  // Channels: a sampler with settings, routed to insert 1.
  Channel kick;
  kick.id = s.allocateId();
  kick.name = "Kick";
  kick.color = 0xCC3344;
  kick.plugin = defaultPluginState("gl.sampler");
  kick.plugin.data = "/tmp/kick.wav";
  kick.gain = 0.7f;
  kick.pan = 0.25f;
  kick.pitch = 2;
  kick.mixerTrack = s.mixer[1].id;
  kick.swingMix = 0.5f;
  kick.polyphony = 1;
  kick.portaTime = 0.125f;  // FL's porta word 512
  kick.plugin.params[pp::SmpDeclick] = 1;  // FL has no "declick off"
  kick.mono = true;
  s.channels.push_back(kick);
  Channel hat = kick;
  hat.id = s.allocateId();
  hat.name = "Hat";
  hat.muted = true;
  hat.cutGroup = 1;
  hat.cutBy = 1;
  hat.mixerTrack = s.mixer[3].id;
  s.channels.push_back(hat);
  // Pattern with notes and per-note properties.
  Pattern& p = s.patterns[0];
  p.name = "Beat";
  p.color = 0x228844;
  Note n(0, 96, 48, 100);
  n.pan = -32;
  n.fine = 50;
  n.modX = 200;
  n.color = 3;
  p.notes[kick.id] = {n, Note(192, 48, 50, 64)};
  Note sl(96, 24, 55, 90);
  sl.slide = true;
  p.notes[hat.id] = {sl};
  // Playlist: two pattern clips, one muted, named tracks.
  Clip c1;
  c1.id = s.allocateId();
  c1.pattern = p.id;
  c1.track = 0;
  c1.start = 0;
  c1.length = s.barTicks();
  s.playlist.push_back(c1);
  Clip c2 = c1;
  c2.id = s.allocateId();
  c2.track = 2;
  c2.start = s.barTicks() * 2;
  c2.muted = true;
  s.playlist.push_back(c2);
  s.tracks[0].name = "Main";
  s.tracks[0].color = 0x884422;
  s.tracks[2].height = 1.5f;
  // Markers (one time signature change).
  Marker m1;
  m1.pos = 0;
  m1.name = "Intro";
  Marker m2;
  m2.pos = s.barTicks() * 4;
  m2.name = "Four";
  m2.timeSigNum = 4;
  m2.timeSigDen = 4;
  s.markers = {m1, m2};
  // Automation clip: insert 1 volume.
  AutomationClip a;
  a.id = s.allocateId();
  a.name = "Drums vol";
  a.owner = s.mixer[1].id;
  a.index = static_cast<uint32_t>(MixerParam::Volume);
  a.min = 0;
  a.max = 0.8f;
  a.points = {AutoPoint{0, 0.25f, 0, AutoShape::Curve}, AutoPoint{192, 1.0f, 0.5f, AutoShape::Curve}};
  s.automations.push_back(a);
  Clip ca;
  ca.id = s.allocateId();
  ca.kind = ClipKind::Automation;
  ca.source = a.id;
  ca.track = 3;
  ca.start = 0;
  ca.length = 192;
  s.playlist.push_back(ca);
  s.channelGroups = {"Unsorted"};
  for (auto& ch : s.channels) ch.group = 0;
  return s;
}

}  // namespace coretest

inline void testCore() {
  using namespace gl::flp;
  // Event encoding: sizes by id class, LEB128 lengths, UTF-16 text.
  EventWriter w;
  w.byte(17, 4);
  w.word(64, 3);
  w.dword(156, 140000);
  w.text(194, "Song");
  Bytes big(300, 7);
  w.data(213, big);
  w.word(11, 64);  // a BYTE id written through word(): the size still follows the id class
  Bytes f = w.file(0, 1, 96);
  File back;
  std::string err;
  expect(readBytes(f, back, &err), "writer: file reads back");
  expect(back.ppq == 96 && back.events.size() == 6, "writer: header + event count");
  expect(back.events.size() == 6 && back.events[2].value() == 140000 && back.events[3].text() == "Song" && back.events[4].size == 300 &&
             back.events[5].size == 1 && back.events[5].value() == 64,
         "writer: values, text, long data, size by id class");
  EventList ev = decodeEvents(encodeEvents(w.events()));
  expect(ev == w.events(), "writer: event list round trip");
  expect(utf16z("é𝄞").size() == 8, "writer: UTF-16 with a surrogate pair + NUL");

  // --- Gridline-native songs → FLP → Gridline.
  {
    gl::Song s = coretest::featureSong();
    gl::Song b;
    ExportReport rep;
    Bytes bytes;
    const bool ok = coretest::exportImport(s, b, rep, &bytes);
    expect(ok, "native song: exports and imports back");
    if (ok) {
      const auto d = compareSongs(s, b);
      for (const auto& x : d) std::printf("  # native diff: %s\n", x.c_str());
      expect(!coretest::has(d, "tempo") && !coretest::has(d, "timeSig") && !coretest::has(d, "swing") && !coretest::has(d, "masterPitch") &&
                 !coretest::has(d, "info."),
             "native song: project settings");
      expect(!coretest::has(d, "channels"), "native song: channels (levels, routing, pitch, mono, cut groups, sampler)");
      expect(!coretest::has(d, "patterns"), "native song: pattern, notes and note properties");
      expect(!coretest::has(d, "mixer"), "native song: mixer (levels, EQ, flags, sends, sidechain)");
      expect(!coretest::has(d, "playlist") && !coretest::has(d, "markers"), "native song: playlist clips and markers");
      expect(!coretest::has(d, "automations"), "native song: automation clip (target, points, tension)");
      expect(!coretest::has(d, "tracks"), "native song: playlist tracks");
      // Exporting the re-imported song again gives the same bytes (stable output).
      gl::Song b2;
      ExportReport rep2;
      Bytes bytes2;
      expect(coretest::exportImport(b, b2, rep2, &bytes2) && compareSongs(b, b2).empty(), "native song: second round trip is identical");
    }
  }
  {
    gl::Song s = gl::makeDemoSong();
    gl::Song b;
    ExportReport rep;
    expect(coretest::exportImport(s, b, rep), "demo song: exports and imports back");
    expect(b.patterns.size() == s.patterns.size() && b.channels.size() == s.channels.size(), "demo song: patterns and channels kept");
    size_t na = 0, nb = 0;
    for (const auto& p : s.patterns) {
      for (const auto& [c, v] : p.notes) na += v.size();
      for (const auto& [c, row] : p.steps)  // steps are written as FL step notes
        for (uint8_t x : row) na += x != 0;
    }
    for (const auto& p : b.patterns)
      for (const auto& [c, v] : p.notes) nb += v.size();
    if (na != nb) std::printf("  # demo song notes %zu -> %zu\n", na, nb);
    expect(na == nb, "demo song: every note and step kept (steps as step notes)");
  }
  // Project JSON keeps the FL origin (export-flp from a saved project).
  {
    gl::Song s = coretest::featureSong();
    s.flOrigin.blobs["proj"] = std::string("\x0b\x40\x00", 3);
    gl::Song l;
    std::string e;
    const std::string path = (std::filesystem::temp_directory_path() / "gl_j34_origin.json").string();
    expect(gl::saveProject(path, s, &e) && gl::loadProject(path, l, &e) && l.flOrigin == s.flOrigin, "project file keeps the FL origin data");
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  if (!enctest::flInstalled()) return;
  // --- Corpus: every shipped project round-trips exactly with its FL origin;
  // without it (encoders only), every project still exports and reads back.
  const std::string root = gl::flp::defaultFactoryData() + "/Data";
  int files = 0, originSame = 0, encoderOk = 0, encoderSame = 0, eventsSame = 0, eventsAll = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".flp") continue;
    ++files;
    ExportOptions o;
    o.factoryData = gl::flp::defaultFactoryData();
    RoundTrip rt;
    std::string err;
    if (roundTrip(it->path().string(), o, rt, &err) && rt.songDiffs.empty()) ++originSame;
    else std::printf("  # origin round trip differs: %s (%zu)\n", it->path().filename().string().c_str(), rt.songDiffs.size());
    eventsSame += rt.identicalEvents;
    eventsAll += rt.originalEvents;
    if (it->path().string().find("Render tests") != std::string::npos) {
      o.useOrigin = false;
      RoundTrip re;
      if (roundTrip(it->path().string(), o, re, &err)) {
        ++encoderOk;
        encoderSame += re.songDiffs.empty();
      }
    }
  }
  std::printf("  # corpus: %d projects, origin round trip identical %d, events identical %d / %d; encoder mode (render tests) %d exported, %d identical\n",
              files, originSame, eventsSame, eventsAll, encoderOk, encoderSame);
  expect(files >= 160 && originSame == files, "corpus: every shipped project round-trips exactly (origin mode)");
  expect(eventsSame * 100 >= eventsAll * 99, "corpus: >= 99% of the original events written back identically");
  expect(encoderOk >= 80, "corpus: render tests export and read back from Gridline values only");
}
