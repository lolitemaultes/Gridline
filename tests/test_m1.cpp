// Gridline — M1 foundations tests: per-project PPQ, object IDs, parameters,
// command/undo, project schema v2 + migration, settings, backups.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Backup.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "engine/Render.h"
#include "core/Platform.h"

namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
  fs::path d = fs::temp_directory_path() / ("gridline_m1_" + std::to_string(std::rand()));
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::string readFile(const fs::path& p) {
  std::ifstream is(p, std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

// A small song built only through Document, so it is itself undoable.
void buildSong(gl::Document& d) {
  d.setParam(gl::songParam(gl::SongParam::Tempo), 120.0, false);
  gl::ObjectId kick = d.addChannel("kick", "gl.kick");
  gl::ObjectId lead = d.addChannel("lead", "gl.synth");
  gl::ObjectId p = d.addPattern("p1", 1, 16);
  for (int s = 0; s < 16; s += 4) d.setStep(p, kick, s, 127);
  d.addNote(p, lead, {0, 96, 60, 110});
  d.addNote(p, lead, {96, 96, 64, 110});
  d.addClip(p, 0, 0);
  d.addClip(p, 1, 384);
}

gl::AudioBuffer render(const gl::Song& s) {
  gl::RenderOptions opt;
  opt.sampleRate = 44100;
  opt.tail = s.ppq;
  return gl::renderSong(s, opt);
}

// Deterministic pseudo-random generator for the property test.
struct Lcg {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  uint32_t next() {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(x >> 33);
  }
  int below(int n) { return n > 0 ? static_cast<int>(next() % static_cast<uint32_t>(n)) : 0; }
};

// Applies one random edit; every Document operation is covered.
void randomEdit(gl::Document& d, Lcg& r) {
  const gl::Song& s = d.song();
  auto anyChannel = [&]() { return s.channels.empty() ? gl::kNoId : s.channels[static_cast<size_t>(r.below(static_cast<int>(s.channels.size())))].id; };
  auto anyPattern = [&]() { return s.patterns.empty() ? gl::kNoId : s.patterns[static_cast<size_t>(r.below(static_cast<int>(s.patterns.size())))].id; };
  auto anyClip = [&]() { return s.playlist.empty() ? gl::kNoId : s.playlist[static_cast<size_t>(r.below(static_cast<int>(s.playlist.size())))].id; };
  switch (r.below(18)) {
    case 0: {
      static const char* types[] = {"gl.synth", "gl.kick", "gl.hat", "gl.snare"};
      d.addChannel("ch" + std::to_string(r.below(100)), types[r.below(4)], r.below(4) - 1);
      break;
    }
    case 1: if (s.channels.size() > 1) d.removeChannel(anyChannel()); break;
    case 2: d.moveChannel(anyChannel(), r.below(static_cast<int>(s.channels.size()))); break;
    case 3: d.renameChannel(anyChannel(), "renamed" + std::to_string(r.below(9))); break;
    case 4: d.setParam(gl::channelParam(anyChannel(), static_cast<gl::ChannelParam>(r.below(3))), r.below(130) / 100.0 - 0.2, r.below(2)); break;
    case 5: d.addPattern("pat", 1 + r.below(4), r.below(2) ? 16 : 12); break;
    case 6: if (s.patterns.size() > 1) d.removePattern(anyPattern()); break;
    case 7: d.setStep(anyPattern(), anyChannel(), r.below(32), static_cast<uint8_t>(r.below(128))); break;
    case 8: d.addNote(anyPattern(), anyChannel(), {r.below(4) * 48, 24 + r.below(96), 36 + r.below(48), 1 + r.below(127)}); break;
    case 9: d.addClip(anyPattern(), r.below(8), r.below(16) * 96, r.below(2) ? 0 : 96 * (1 + r.below(8))); break;
    case 10: d.removeClip(anyClip()); break;
    case 11: d.moveClip(anyClip(), r.below(8), r.below(32) * 48); break;
    case 12: d.setClipLength(anyClip(), r.below(3) * 96); break;
    case 13: d.setPpq(gl::kPpqChoices[static_cast<size_t>(r.below(static_cast<int>(gl::kPpqChoices.size())))]); break;
    case 14: d.setTimeSignature(2 + r.below(6), r.below(2) ? 4 : 8); break;
    case 15: { gl::SongInfo i = s.info; i.author = "a" + std::to_string(r.below(9)); d.setInfo(i); break; }
    case 16: d.setPatternLength(anyPattern(), 1 + r.below(3), 16); break;
    case 17:
      d.beginGroup("group");
      d.renamePattern(anyPattern(), "g");
      d.addClip(anyPattern(), 0, 0);
      d.endGroup();
      break;
  }
  if (r.below(3) == 0) d.endGesture();
}

}  // namespace

int main() {
  std::srand(12345);
  fs::path tmp = tempDir();

  // ---- CORE-01: per-project PPQ ------------------------------------------------
  expect(gl::isValidPpq(96) && gl::isValidPpq(960) && gl::isValidPpq(24), "PPQ choices include 24, 96, 960");
  expect(!gl::isValidPpq(100) && !gl::isValidPpq(0), "PPQ outside the choice list is rejected");
  {
    gl::Document d;
    buildSong(d);
    gl::Song at96 = d.song();
    d.setPpq(192);
    expect(d.song().ppq == 192 && d.song().patterns[0].notes.begin()->second[1].start == 192,
           "raising PPQ rescales note positions");
    expect(d.song().lengthTicks() == 2 * at96.lengthTicks(), "raising PPQ rescales song length");
    expect(render(d.song()).data == render(at96).data, "same song renders identically at PPQ 96 and 192");
    d.undo();
    expect(d.song() == at96, "PPQ change is undoable (exact snapshot)");
    gl::Document lossy(at96);
    lossy.addNote(at96.patterns[0].id, at96.channels[1].id, {5, 7, 72, 100});  // off any coarse grid
    int rounded = 0;
    lossy.setPpq(24, &rounded);
    expect(rounded > 0, "reducing PPQ reports values that had to be rounded");
    expect(lossy.song().ppq == 24, "PPQ reduced to 24");
    bool allPositive = true;
    for (const auto& [ch, ns] : lossy.song().patterns[0].notes)
      for (const auto& n : ns) allPositive = allPositive && n.length >= 1;
    expect(allPositive, "notes keep a length of at least one tick after reducing PPQ");
    expect(!d.setPpq(100), "setPpq rejects unsupported values");
  }

  // ---- CORE-02: stable object IDs ----------------------------------------------
  {
    gl::Document d;
    buildSong(d);
    const gl::Song& s = d.song();
    std::vector<gl::ObjectId> ids;
    for (const auto& c : s.channels) ids.push_back(c.id);
    for (const auto& p : s.patterns) ids.push_back(p.id);
    for (const auto& c : s.playlist) ids.push_back(c.id);
    bool unique = true;
    for (size_t i = 0; i < ids.size(); ++i)
      for (size_t j = i + 1; j < ids.size(); ++j) unique = unique && ids[i] != ids[j];
    expect(unique, "object IDs are unique across kinds");
    bool belowNext = true;
    for (auto id : ids) belowNext = belowNext && id >= gl::kFirstObjectId && id < s.nextId;
    expect(belowNext, "IDs are below nextId");
    gl::ObjectId lead = s.channels[1].id;
    d.moveChannel(lead, 0);
    expect(d.song().channels[0].id == lead && d.song().channel(lead)->name == "lead", "IDs survive reordering");
    gl::ObjectId first = d.song().channels[0].id;
    d.removeChannel(first);
    expect(d.song().channel(first) == nullptr && d.song().patterns[0].notes.count(first) == 0,
           "deleting a channel removes its pattern rows");
    d.undo();
    expect(d.song().channel(first) != nullptr && d.song().patterns[0].notes.count(first) == 1,
           "undo restores the channel and its pattern rows");
  }

  // ---- AU-01: parameters --------------------------------------------------------
  {
    gl::Document d;
    buildSong(d);
    const gl::Song& s = d.song();
    gl::ParamId vol = gl::channelParam(s.channels[0].id, gl::ChannelParam::Volume);
    expect(gl::ParamId::unpack(vol.packed()) == vol, "ParamId packs and unpacks");
    size_t expected = static_cast<size_t>(gl::SongParam::Count);
    for (const auto& c : s.channels)
      expected += static_cast<size_t>(gl::ChannelParam::Count) + gl::findPlugin(c.plugin.type)->params.size();
    expected += s.mixer.size() * static_cast<size_t>(gl::MixerParam::Count);
    expect(gl::listParams(s).size() == expected, "listParams covers song, channel, instrument and mixer params");
    expect(gl::describeParam(s, vol)->name == "Volume", "describeParam names the parameter");
    expect(!gl::describeParam(s, {999, 0}), "unknown owner does not resolve");
    d.setParam(vol, 9.0);
    expect(*gl::getParam(s, vol) == 1.25, "setParam clamps to the maximum");
    gl::ParamId mute = gl::channelParam(s.channels[0].id, gl::ChannelParam::Mute);
    d.setParam(mute, 0.7);
    expect(s.channels[0].muted && *gl::getParam(s, mute) == 1.0, "toggle params snap to 0/1");
    auto desc = *gl::describeParam(s, gl::songParam(gl::SongParam::Tempo));
    expect(desc.fromNormalized(desc.toNormalized(133.0)) == 133.0, "normalized mapping round-trips");

    gl::Smoother sm;
    sm.reset(0.0);
    sm.setRampSamples(4);
    sm.setTarget(1.0);
    double a = sm.next(), b = sm.next(), c = sm.next(), e = sm.next();
    expect(a == 0.25 && b == 0.5 && c == 0.75 && e == 1.0 && !sm.ramping(), "smoother ramps linearly to target");

    gl::ParamBus bus(4);
    bool pushed = bus.push({vol, 1}) && bus.push({vol, 2}) && bus.push({vol, 3}) && bus.push({vol, 4});
    expect(pushed && !bus.push({vol, 5}), "param bus accepts up to capacity");
    gl::ParamChange out;
    bool fifo = bus.pop(out) && out.value == 1 && bus.pop(out) && out.value == 2;
    expect(fifo && bus.push({vol, 5}), "param bus is FIFO and frees space");
  }

  // ---- UN-01 / UN-02: command & undo ---------------------------------------------
  {
    gl::Document d;
    gl::Song empty = d.song();
    buildSong(d);
    gl::Song built = d.song();
    size_t steps = d.history().size();
    expect(steps > 5, "every edit records a history step");
    while (d.undo()) {}
    expect(d.song() == empty, "undo everything returns to the empty song");
    while (d.redo()) {}
    expect(d.song() == built, "redo everything returns to the built song");

    gl::ParamId pan = gl::channelParam(d.song().channels[0].id, gl::ChannelParam::Pan);
    size_t before = d.history().size();
    for (int i = 1; i <= 10; ++i) d.setParam(pan, i / 10.0);
    expect(d.history().size() == before + 1, "a knob drag (same param) merges into one step");
    d.endGesture();
    d.setParam(pan, -0.5);
    expect(d.history().size() == before + 2, "endGesture starts a new step");
    d.undo();
    d.undo();
    expect(d.song() == built, "merged drag undoes in one step");

    d.beginGroup("duplicate");
    gl::ObjectId np = d.addPattern("copy");
    d.addClip(np, 3, 0);
    d.endGroup();
    expect(d.history().undoLabel() == "duplicate", "group records one labelled step");
    d.undo();
    expect(d.song() == built, "group undoes as a whole");

    d.setMaxUndoLevels(3);
    for (int i = 0; i < 6; ++i) d.renameChannel(d.song().channels[0].id, "n" + std::to_string(i));
    expect(d.history().size() == 3, "history is trimmed to the level limit");
    d.setMaxUndoLevels(100);

    gl::Document m(built);
    expect(!m.modified(), "fresh document is unmodified");
    m.renamePattern(built.patterns[0].id, "x");
    expect(m.modified(), "edit marks the document modified");
    m.undo();
    expect(!m.modified(), "undoing back to the saved state clears modified");
    m.renamePattern(built.patterns[0].id, "y");
    m.markSaved();
    m.undo();
    expect(m.modified(), "undoing past the save point is modified");
    m.jumpTo(1);
    expect(!m.modified() && m.song().patterns[0].name == "y", "jumpTo moves through history");

    expect(!d.removeChannel(424242) && !d.addClip(424242, 0, 0) && !d.setStep(424242, 1, 0, 1),
           "operations on unknown IDs are rejected without recording");
  }

  // ---- CORE-03: undo/redo property test --------------------------------------------
  {
    gl::Document d;
    buildSong(d);
    d.setMaxUndoLevels(100000);
    std::vector<gl::Song> states{d.song()};
    std::vector<size_t> positions{d.history().position()};
    Lcg r;
    for (int i = 0; i < 600; ++i) {
      randomEdit(d, r);
      if (d.history().position() != positions.back()) {
        states.push_back(d.song());
        positions.push_back(d.history().position());
      } else {
        states.back() = d.song();  // merged into the previous step
      }
    }
    gl::Song finalSong = d.song();
    bool ok = true;
    for (size_t k = states.size(); k-- > 0;) {
      d.jumpTo(positions[k]);
      ok = ok && d.song() == states[k];
    }
    expect(ok, "property: every intermediate state is reproduced by undo (600 random edits)");
    d.jumpTo(d.history().size());
    expect(d.song() == finalSong, "property: redo to the end reproduces the final state");
    gl::Song reloaded;
    expect(gl::songFromJson(gl::songToJson(finalSong), reloaded) && reloaded == finalSong,
           "property: final random song survives save/load");
    expect(render(reloaded).data == render(finalSong).data, "property: render is bit-identical after save/load");
  }

  // ---- PJ-01: schema v2, migration, validation -----------------------------------------
  {
    gl::Song v1;
    std::string err;
    expect(gl::loadProject("demo/song.json", v1, &err), "v1 demo project loads (migrated)");
    expect(v1.ppq == 96 && v1.channels.size() == 3 && v1.channels[0].id >= gl::kFirstObjectId,
           "migration assigns IDs and default PPQ");
    bool refsOk = !v1.playlist.empty();
    for (const auto& c : v1.playlist) refsOk = refsOk && v1.pattern(c.pattern) != nullptr;
    expect(refsOk, "migration maps clip pattern indices to IDs");
    bool rowsOk = true;
    for (const auto& p : v1.patterns)
      for (const auto& [ch, row] : p.steps) rowsOk = rowsOk && v1.channel(ch) != nullptr;
    expect(rowsOk, "migration maps step rows to channel IDs");
    expect(v1.info.title.find("Demo") != std::string::npos, "migration moves the title into song info");

    fs::path f = tmp / "demo.gridline.json";
    expect(gl::saveProject(f.string(), v1, &err), "atomic save succeeds");
    expect(!fs::exists(f.string() + ".tmp"), "atomic save leaves no temp file");
    gl::Song back;
    expect(gl::loadProject(f.string(), back, &err) && back == v1, "v2 save → load is lossless");
    expect(render(back).data == render(v1).data, "render is bit-identical across save → load");
    std::string once = readFile(f);
    gl::saveProject(f.string(), back);
    expect(readFile(f) == once, "saving the same song twice produces identical text");

    gl::Json j = gl::songToJson(v1);
    j.object["version"] = gl::Json::makeNumber(99);
    expect(!gl::songFromJson(j, back, &err) && err.find("newer") != std::string::npos, "newer schema is refused");
    j = gl::songToJson(v1);
    j.object["channels"].array[1].object["id"] = j.object["channels"].array[0].object["id"];
    expect(!gl::songFromJson(j, back, &err), "duplicate IDs are refused");
    j = gl::songToJson(v1);
    j.object["playlist"].array[0].object["pattern"] = gl::Json::makeNumber(999999);
    expect(!gl::songFromJson(j, back, &err), "dangling clip pattern reference is refused");
    j = gl::songToJson(v1);
    j.object["ppq"] = gl::Json::makeNumber(100);
    expect(!gl::songFromJson(j, back, &err), "unsupported PPQ is refused");
    j = gl::songToJson(v1);
    j.object["nextId"] = gl::Json::makeNumber(3);
    expect(gl::songFromJson(j, back, &err) && back.nextId > back.playlist.back().id,
           "a stale nextId is repaired past the highest ID");
  }

  // ---- ST-01: settings ---------------------------------------------------------------
  {
    gl::Settings st;
    expect(st.getNumber("undo.maxLevels") == 100 && st.getNumber("backup.maxCount") == 100, "settings defaults");
    expect(st.set("project.defaultPpq", 192) && !st.set("project.defaultPpq", "x") && !st.set("no.such.key", 1),
           "settings reject unknown keys and wrong types");
    fs::path f = tmp / "cfg" / "settings.json";
    expect(st.save(f.string()), "settings save creates folders");
    gl::Settings st2;
    expect(st2.load(f.string()) && st2.getNumber("project.defaultPpq") == 192, "settings round-trip");
    gl::Settings st3;
    expect(st3.load((tmp / "missing.json").string()) && st3.getNumber("project.defaultPpq") == 96,
           "missing settings file keeps defaults");
    gl::platform::unsetEnv("GRIDLINE_CONFIG_HOME");
    gl::platform::setEnv("XDG_CONFIG_HOME", "/xdg");
    expect(gl::Settings::defaultPath() == (std::filesystem::path("/xdg") / "gridline" / "settings.json").string(),
           "settings path follows XDG_CONFIG_HOME");
  }

  // ---- PJ-02: backups -------------------------------------------------------------------
  {
    gl::BackupPolicy pol;
    expect(pol.intervalSeconds() == 300, "default backup interval is 5 minutes");
    pol.frequency = gl::BackupFrequency::VeryFrequently;
    expect(pol.intervalSeconds() == 60 && pol.backupBeforeRiskyEdit(), "very frequent: every minute + risky edits");
    pol.frequency = gl::BackupFrequency::Never;
    expect(pol.intervalSeconds() < 0, "never: no timed backups");

    gl::BackupPolicy p2;
    p2.maxCount = 3;
    p2.globalFolder = (tmp / "backups").string();
    gl::BackupManager bm(p2);
    gl::Song s;
    gl::Document d;
    buildSong(d);
    s = d.song();
    int64_t t0 = 1'700'000'000;
    expect(bm.due(t0), "first backup is due immediately");
    bool wrote = true;
    for (int i = 0; i < 5; ++i) wrote = wrote && !bm.write(s, "/songs/My Song.gridline.json", t0 + i * 300).empty();
    expect(wrote, "backups written");
    auto list = bm.list("/songs/My Song.gridline.json");
    expect(list.size() == 3, "backups beyond maxCount are pruned");
    expect(!bm.due(t0 + 4 * 300 + 10) && bm.due(t0 + 5 * 300), "next backup due after the interval");
    gl::Song restored;
    expect(gl::loadProject(list.back(), restored) && restored == s, "a backup loads as the project");
    expect(fs::path(list.back()).filename().string().rfind("My Song (backup ", 0) == 0, "backup file is named after the project");

    gl::BackupPolicy p3;
    p3.inProjectFolder = true;
    gl::BackupManager local(p3);
    expect(fs::path(local.folderFor("/songs/a/x.gridline.json")) == fs::path("/songs/a/Backup"), "project-folder backups go next to the project");
  }

  fs::remove_all(tmp);
  {  // v5 → v6: MIDI links to mixer-slot params move to the 1000-wide slot stride
    gl::Song sg = gl::makeEmptySong();
    gl::Document d(sg);
    gl::ObjectId tr = d.song().mixer.size() > 1 ? d.song().mixer[1].id : d.song().mixer[0].id;
    d.setSlotPlugin(tr, 2, "gl.eq");
    gl::Json j = gl::songToJson(d.song());
    gl::Json links = gl::Json::makeArray();
    auto add = [&](double idx) {
      gl::Json l = gl::Json::makeObject();
      l.object["cc"] = gl::Json::makeNumber(7);
      l.object["midiChannel"] = gl::Json::makeNumber(-1);
      l.object["owner"] = gl::Json::makeNumber(tr);
      l.object["index"] = gl::Json::makeNumber(idx);
      l.object["min"] = gl::Json::makeNumber(0);
      l.object["max"] = gl::Json::makeNumber(1);
      links.array.push_back(l);
    };
    add(1000 + 2 * 100 + 3);   // slot 2, param 3 (old layout)
    add(1000 + 2 * 100 + 99);  // slot 2, mix (old layout)
    j.object["links"] = links;
    j.object["version"] = gl::Json::makeNumber(5);
    gl::Song back;
    std::string err;
    bool ok = gl::songFromJson(j, back, &err);
    bool moved = ok && back.links.size() == 2 && back.links[0].index == gl::slotParam(tr, 2, 3).index &&
                 back.links[1].index == gl::slotMixParam(tr, 2).index;
    expect(moved, "v5 projects: slot parameter links migrate to the v6 layout");
  }
  {  // mixer automation targets: strip EQ, send levels, slot on/off
    gl::Song sg = gl::makeEmptySong();
    gl::Document d(sg);
    gl::ObjectId tr = d.song().mixer[1].id;
    d.setSlotPlugin(tr, 0, "gl.eq");
    bool eq = d.setParam(gl::mixerParam(tr, gl::MixerParam::EqMidGain), 6.0, false) &&
              std::fabs(d.song().mixer[1].eqGain[1] - 6.0f) < 1e-6f &&
              d.setParam(gl::mixerParam(tr, gl::MixerParam::EqHighFreq), 8000.0, false) &&
              std::fabs(d.song().mixer[1].eqFreq[2] - 8000.0f) < 1e-3f;
    expect(eq, "strip EQ bands are parameters");
    bool send = d.setParam(gl::sendParam(tr, 0), 0.5, false) && std::fabs(d.song().mixer[1].sends[0].level - 0.5f) < 1e-6f;
    expect(send, "send level to the master is a parameter");
    bool on = d.setParam(gl::slotEnableParam(tr, 0), 0.0, false) && !d.song().mixer[1].slots[0].enabled;
    expect(on, "effect slot on/off is a parameter");
  }
  return finish();
}
