// Gridline — J-21 QA: undo/redo stress over random Document edits, save/load
// round trips, parameter-id layout limits, and regression tests for bugs
// found by the end-to-end sweep and the reader fuzzers.
//
//   gridline_tests_qa [project.json...]   extra projects get the undo stress too
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/Demo.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MenuOps.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "engine/WavPack.h"

using namespace gl;
using namespace gl::ops;

namespace {

std::string text(const Song& s) { return projectToText(s); }

// ------------------------------------------------------------ random edits
struct Editor {
  Document& d;
  std::mt19937_64 rng;
  std::vector<std::string> gens, fxs, notefx;
  int applied = 0;

  size_t pick(size_t n) { return n ? static_cast<size_t>(rng() % n) : 0; }
  int range(int lo, int hi) { return lo + static_cast<int>(pick(static_cast<size_t>(hi - lo + 1))); }
  const Song& s() const { return d.song(); }
  ObjectId anyChannel() { return s().channels.empty() ? kNoId : s().channels[pick(s().channels.size())].id; }
  ObjectId anyPattern() { return s().patterns.empty() ? kNoId : s().patterns[pick(s().patterns.size())].id; }
  ObjectId anyClip() { return s().playlist.empty() ? kNoId : s().playlist[pick(s().playlist.size())].id; }
  ObjectId anyMixer() { return s().mixer.empty() ? kNoId : s().mixer[pick(s().mixer.size())].id; }
  ObjectId anyInsert() { return s().mixer.size() < 2 ? kNoId : s().mixer[1 + pick(s().mixer.size() - 1)].id; }
  Tick anyTick() { return static_cast<Tick>(pick(static_cast<size_t>(s().ppq) * 64)); }

  ParamId anyParam() {
    switch (pick(6)) {
      case 0: return songParam(static_cast<SongParam>(pick(static_cast<size_t>(SongParam::Count))));
      case 1: return channelParam(anyChannel(), static_cast<ChannelParam>(pick(static_cast<size_t>(ChannelParam::Count))));
      case 2: {
        ObjectId c = anyChannel();
        for (const auto& ch : s().channels)
          if (ch.id == c && !ch.plugin.params.empty()) return channelPluginParam(c, static_cast<int>(pick(ch.plugin.params.size())));
        return channelParam(c, ChannelParam::Volume);
      }
      case 3: return mixerParam(anyMixer(), static_cast<MixerParam>(pick(static_cast<size_t>(MixerParam::Count))));
      case 4: {
        ObjectId t = anyMixer();
        for (const auto& m : s().mixer)
          if (m.id == t)
            for (int sl = 0; sl < static_cast<int>(m.slots.size()); ++sl)
              if (!m.slots[static_cast<size_t>(sl)].plugin.empty() && !m.slots[static_cast<size_t>(sl)].plugin.params.empty())
                return slotParam(t, sl, static_cast<int>(pick(m.slots[static_cast<size_t>(sl)].plugin.params.size())));
        return slotMixParam(t, static_cast<int>(pick(kEffectSlots)));
      }
      default: return sendParam(anyMixer(), static_cast<int>(pick(s().mixer.size())));
    }
  }

  // One random edit; returns a short name (for failure messages).
  const char* step() {
    std::uniform_real_distribution<double> u(-0.2, 1.2);
    switch (pick(46)) {
      case 0: d.addChannel("ch" + std::to_string(applied), gens[pick(gens.size())], range(-1, 5)); return "addChannel";
      case 1: if (s().channels.size() > 1) d.removeChannel(anyChannel()); return "removeChannel";
      case 2: d.moveChannel(anyChannel(), range(-2, static_cast<int>(s().channels.size()) + 2)); return "moveChannel";
      case 3: d.renameChannel(anyChannel(), "n" + std::to_string(applied)); return "renameChannel";
      case 4: d.setChannelPlugin(anyChannel(), gens[pick(gens.size())]); return "setChannelPlugin";
      case 5: d.cloneChannel(anyChannel()); return "cloneChannel";
      case 6: d.routeChannel(anyChannel(), anyMixer()); return "routeChannel";
      case 7: case 8: case 9: d.setParam(anyParam(), u(rng) * (pick(4) ? 1.0 : 400.0), pick(2) == 0); return "setParam";
      case 10: d.endGesture(); d.setParam(anyParam(), u(rng)); return "setParam-gesture";
      case 11: d.addPattern("p" + std::to_string(applied), range(1, 4), pick(2) ? 16 : 12, range(-1, 3)); return "addPattern";
      case 12: if (s().patterns.size() > 1) d.removePattern(anyPattern()); return "removePattern";
      case 13: d.clonePattern(anyPattern()); return "clonePattern";
      case 14: d.renamePattern(anyPattern(), "r" + std::to_string(applied)); return "renamePattern";
      case 15: d.setPatternLength(anyPattern(), range(0, 8), range(0, 32)); return "setPatternLength";
      case 16: d.setStep(anyPattern(), anyChannel(), range(-1, 70), static_cast<uint8_t>(pick(256))); return "setStep";
      case 17: case 18: {
        Note n;
        n.start = anyTick();
        n.length = range(0, s().ppq * 4);
        n.pitch = range(-5, 132);
        n.velocity = range(0, 127);
        d.addNote(anyPattern(), anyChannel(), n);
        return "addNote";
      }
      case 19: {
        std::vector<Note> v(pick(6));
        for (auto& n : v) n.start = anyTick(), n.pitch = range(0, 127);
        d.setNotes(anyPattern(), anyChannel(), v);
        return "setNotes";
      }
      case 20: case 21: d.addClip(anyPattern(), range(-1, 20), anyTick(), pick(2) ? 0 : anyTick()); return "addClip";
      case 22: d.removeClip(anyClip()); return "removeClip";
      case 23: d.moveClip(anyClip(), range(-1, 30), anyTick()); return "moveClip";
      case 24: d.setClipLength(anyClip(), range(-10, s().ppq * 16)); return "setClipLength";
      case 25: d.setPlaylistTrackCount(range(0, 200)); return "setPlaylistTrackCount";
      case 26: d.addMixerTrack(); return "addMixerTrack";
      case 27: if (s().mixer.size() > 3) d.removeMixerTrack(anyInsert()); return "removeMixerTrack";
      case 28: d.setSend(anyMixer(), anyMixer(), static_cast<float>(u(rng))); return "setSend";
      case 29: d.setSlotPlugin(anyMixer(), range(-1, kEffectSlots), pick(5) ? fxs[pick(fxs.size())] : std::string()); return "setSlotPlugin";
      case 30: d.moveSlot(anyMixer(), range(-1, kEffectSlots), range(-1, kEffectSlots)); return "moveSlot";
      case 31: d.setSendSidechain(anyMixer(), anyMixer(), pick(2)); return "setSendSidechain";
      case 32: if (!notefx.empty()) d.addNoteFx(anyChannel(), notefx[pick(notefx.size())], range(-1, 3)); return "addNoteFx";
      case 33: d.removeNoteFx(anyChannel(), range(-1, 3)); return "removeNoteFx";
      case 34: d.moveNoteFx(anyChannel(), range(-1, 3), range(-1, 3)); return "moveNoteFx";
      case 35: d.setPpq(pick(3) ? kPpqChoices[pick(kPpqChoices.size())] : range(1, 2000)); return "setPpq";
      case 36: d.setTimeSignature(range(0, 17), 1 << pick(6)); return "setTimeSignature";
      case 37: transposePattern(d, anyPattern(), range(-30, 30)); return "transposePattern";
      case 38: fillSteps(d, anyPattern(), anyChannel(), range(0, 8), range(-2, 5)); return "fillSteps";
      case 39: rotateChannel(d, anyPattern(), anyChannel(), pick(2) ? 1 : -1); return "rotateChannel";
      case 40: sortChannels(d, static_cast<ChannelSort>(pick(4))); return "sortChannels";
      case 41: insertPlaylistTrack(d, range(-1, 30)); return "insertPlaylistTrack";
      case 42: deletePlaylistTrack(d, range(-1, 30)); return "deletePlaylistTrack";
      case 43: {
        std::set<ObjectId> c;
        for (int k = 0; k < 3; ++k) c.insert(anyClip());
        c.erase(kNoId);
        switch (pick(3)) {
          case 0: duplicateClips(d, c); return "duplicateClips";
          case 1: mergeClips(d, c); return "mergeClips";
          default: removeClips(d, c); return "removeClips";
        }
      }
      case 44: makeClipUnique(d, anyClip()); return "makeClipUnique";
      default: {  // grouped burst
        d.beginGroup("burst");
        for (int k = 0; k < 3; ++k) {
          Note n;
          n.start = anyTick();
          d.addNote(anyPattern(), anyChannel(), n);
          d.setParam(anyParam(), u(rng));
        }
        d.endGroup();
        return "group";
      }
    }
  }
};

void undoStress(const Song& start, const char* tag, int edits, uint64_t seed) {
  const int checkEvery = std::getenv("GRIDLINE_QA_CHECK_EVERY") ? std::max(1, std::atoi(std::getenv("GRIDLINE_QA_CHECK_EVERY"))) : 25;
  Document d(start);
  d.setMaxUndoLevels(static_cast<size_t>(edits) + 10);
  Editor e{d, std::mt19937_64(seed), {}, {}, {}};
  for (const auto& p : pluginCatalog()) {
    if (p.kind == PluginKind::Generator) e.gens.push_back(p.type);
    else if (p.kind == PluginKind::Effect) e.fxs.push_back(p.type);
    else e.notefx.push_back(p.type);
  }
  const std::string original = text(d.song());
  // Hash of the state after each history position, to check every step back.
  std::vector<size_t> at{std::hash<std::string>()(original)};
  std::vector<std::string> what{"start"};
  bool valid = true;
  std::string firstInvalid;
  for (int i = 0; i < edits; ++i) {
    const char* op = e.step();
    ++e.applied;
    size_t pos = d.history().position();
    std::string t = text(d.song());
    at.resize(pos + 1);
    what.resize(pos + 1);
    at[pos] = std::hash<std::string>()(t);
    what[pos] = op;
    if (i % checkEvery == 0) {  // the edited song must still be a loadable project
      Json j;
      Song back;
      std::string err;
      if (!jsonParse(t, j) || !songFromJson(j, back, &err) || text(back) != t) {
        if (valid) {
          firstInvalid = std::string(op) + " (edit " + std::to_string(i) + "): " + err;
          std::string b = text(back);
          size_t k = 0;
          while (k < t.size() && k < b.size() && t[k] == b[k]) ++k;
          size_t from = k > 160 ? k - 160 : 0;
          firstInvalid += "\n# saved:  ..." + t.substr(from, 240) + "\n# reload: ..." + b.substr(from, 240);
        }
        valid = false;
      }
    }
  }
  const std::string final = text(d.song());
  const size_t steps = d.history().position();
  std::string firstBad;
  for (size_t p = steps; p > 0; --p) {
    d.undo();
    if (firstBad.empty() && std::hash<std::string>()(text(d.song())) != at[p - 1])
      firstBad = "undo of '" + what[p] + "' (step " + std::to_string(p) + ")";
  }
  const bool backToStart = text(d.song()) == original;
  for (size_t p = 1; p <= steps; ++p) {
    d.redo();
    if (firstBad.empty() && std::hash<std::string>()(text(d.song())) != at[p])
      firstBad = "redo of '" + what[p] + "' (step " + std::to_string(p) + ")";
  }
  const bool backToFinal = text(d.song()) == final;
  std::string n = std::string(tag) + ": ";
  if (!firstBad.empty()) std::printf("# first mismatch: %s\n", firstBad.c_str());
  if (!valid) std::printf("# first invalid project: %s\n", firstInvalid.c_str());
  std::printf("# %s: %d edits, %zu history steps\n", tag, edits, steps);
  expect(valid, (n + "every edited state saves and loads back identically").c_str());
  expect(firstBad.empty(), (n + "each undo / redo step restores the recorded state").c_str());
  expect(backToStart, (n + "undo all == original").c_str());
  expect(backToFinal, (n + "redo all == final").c_str());
}

// Parameter ids pack (owner, index); plugin params must not run into the next
// range (channel: note-processor base; slot: enable/mix indexes; note fx: stride).
void paramLayout() {
  size_t maxGen = 0, maxFx = 0, maxNote = 0;
  std::string gen, fx, note;
  for (const auto& p : pluginCatalog()) {
    size_t n = p.params.size();
    if (p.kind == PluginKind::Generator && n > maxGen) maxGen = n, gen = p.type;
    if (p.kind == PluginKind::Effect && n > maxFx) maxFx = n, fx = p.type;
    if (p.kind == PluginKind::NoteFx && n > maxNote) maxNote = n, note = p.type;
  }
  std::printf("# largest param counts: %s %zu, %s %zu, %s %zu\n", gen.c_str(), maxGen, fx.c_str(), maxFx, note.c_str(), maxNote);
  expect(kChannelPluginBase + maxGen <= kNoteFxBase, "generator params stay below the note-processor range");
  expect(maxFx <= kSlotEnable, "effect params stay below the slot enable / mix indexes");
  expect(maxNote <= kNoteFxStride, "note-processor params fit their stride");
}

void roundTrip(const Song& s, const char* tag) {
  std::string a = text(s);
  Json j;
  Song back;
  std::string err;
  bool ok = jsonParse(a, j) && songFromJson(j, back, &err);
  expect(ok && text(back) == a, (std::string(tag) + ": save -> load -> save is byte-identical").c_str());
}

// Bug: merging pattern clips far apart (or in a short time signature) made a
// pattern longer than a project file accepts; the saved project did not load.
void mergedPatternStaysLoadable() {
  Document d(makeDemoSong());
  ObjectId p = d.song().patterns[0].id;
  ObjectId a = d.addClip(p, 0, 0), b = d.addClip(p, 1, static_cast<Tick>(d.song().barTicks()) * 1500);
  ObjectId m = ops::mergeClips(d, {a, b});
  expect(m != kNoId, "merge clips 1500 bars apart");
  Json j;
  Song back;
  std::string err;
  expect(jsonParse(text(d.song()), j) && songFromJson(j, back, &err), "merged long pattern: project loads back");
  const Clip* c = d.song().clip(m);
  expect(c && c->length > static_cast<Tick>(d.song().barTicks()) * 1500, "merged clip keeps the whole span");
  expect(!d.setPatternLength(p, kMaxPatternBars + 1, 16) && !d.setPatternLength(p, 4, kMaxStepsPerBar + 1),
         "setPatternLength refuses lengths a project cannot store");
  expect(d.addPattern("x", kMaxPatternBars + 1) == kNoId, "addPattern refuses lengths a project cannot store");
}

// Bug (undo/save stress on converted FL projects): deleting a channel left its
// audio clips and layer references behind; the saved project silently lost
// them on load, so save -> load was no longer identical.
void removeChannelDropsReferences() {
  Document d(makeDemoSong());
  ObjectId ch = d.song().channels[0].id, layer = d.song().channels[1].id;

  d.editSong("fixture", [&](Song& s) {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch;
    c.start = 96;
    c.length = 96;
    s.playlist.push_back(c);
    s.channel(layer)->layer.push_back(ch);
  });
  const std::string withRefs = text(d.song());
  d.removeChannel(ch);
  Json j;
  Song back;
  std::string err;
  std::string t = text(d.song());
  bool same = jsonParse(t, j) && songFromJson(j, back, &err) && text(back) == t;
  bool gone = true;
  for (const auto& c : d.song().playlist) gone = gone && !(c.kind == ClipKind::Audio && c.source == ch);
  expect(same && gone && d.song().channel(layer)->layer.empty(), "removeChannel: audio clips and layer refs go with it (save/load identical)");
  d.undo();
  expect(text(d.song()) == withRefs, "removeChannel: undo restores clips and layer refs");

}

// Bugs from the reader fuzzers / sanitizer sweep: JSON numbers and casts.
void jsonNumbers() {
  // A subnormal value (e.g. a denormal parameter) was written fine but std::stod
  // threw ERANGE on reading it back, so the whole project failed to load.
  Json j;
  expect(jsonParse("[4.9406564584124654e-324, 1e-310, -2.5e-320]", j) && j.array.size() == 3,
         "JSON: subnormal numbers parse");
  expect(!jsonParse("[1e999]", j), "JSON: numbers that overflow a double are rejected");
  // Stringify cast |v| >= 2^63 to long long before the range check (UB) and
  // wrote NaN / inf as text no JSON reader accepts.
  Json arr = Json::makeArray();
  for (double v : {1e19, -1e19, 9.3e18, 1e300, std::nan(""), HUGE_VAL, 4.9406564584124654e-324, 42.0}) {
    Json n;
    n.type = Json::Type::Number;
    n.number = v;
    arr.array.push_back(n);
  }
  Json back;
  std::string t = jsonStringify(arr, 0);
  expect(jsonParse(t, back) && back.array.size() == 8, "JSON: every double serialises to parseable text");
  expect(back.array.size() == 8 && back.array[4].type == Json::Type::Null && back.array[0].number == 1e19 &&
             back.array[7].number == 42.0,
         "JSON: non-finite numbers become null, finite ones round-trip");
  expect(jsonInt<int>(1e300) == 2147483647 && jsonInt<int>(-1e300) == -2147483647 - 1 && jsonInt<uint64_t>(-1) == 0 &&
             jsonInt<int64_t>(9.3e18) == INT64_MAX && jsonInt<int>(std::nan("")) == 0 && jsonInt<int>(-7.9) == -7,
         "jsonInt saturates instead of overflowing");
  // Plugin parameters from a file: NaN / inf / huge choice indexes.
  ParamDesc c;
  c.kind = ParamKind::Choice;
  c.choices = {"a", "b", "c"};
  ParamDesc k;
  k.min = -1, k.max = 1, k.def = 0.25;
  expect(c.clamp(HUGE_VAL) == 2 && c.clamp(-HUGE_VAL) == 0 && c.clamp(std::nan("")) == 0 && c.clamp(1.4) == 1 &&
             c.clamp(1.6) == 2 && c.clamp(-0.4) == 0,
         "ParamDesc::clamp: choice indexes from any double");
  expect(k.clamp(std::nan("")) == 0.25 && k.clamp(HUGE_VAL) == 1 && k.clamp(-HUGE_VAL) == -1, "ParamDesc::clamp: NaN -> default");
  // Project with out-of-range numbers: loads (clamped / refused), no UB.
  Song s = makeDemoSong();
  Json pj = songToJson(s);
  for (auto& ch : pj.object["channels"].array) {
    ch.object["plugin"].object["params"].array.assign(3, Json());
    for (auto& v : ch.object["plugin"].object["params"].array) v.type = Json::Type::Number, v.number = 1e300;
    ch.object["mixer"].type = Json::Type::Number, ch.object["mixer"].number = 1e300;
  }
  Song out;
  std::string err;
  (void)songFromJson(pj, out, &err);
  expect(true, "project with huge numbers: no undefined behaviour (sanitizers)");
  // Browser index cache with hostile sizes / times / channel counts.
  std::string dir = gl::test::hermeticTestEnv("qa") + "/bi";
  std::string file = dir + "/index.json";
  std::filesystem::create_directories(dir);
  writeFileAtomic(file, R"({"format":"gridline-browser-index","version":99,"files":{"/x/a.wav":{"s":-1,"m":1e300,"ch":3e12,"key":-1e300}}})");
  browse::Index idx;
  idx.setCacheFile(file);
  browse::FileMeta m;
  expect(idx.load() && idx.lookup("/x/a.wav", m) && m.size == 0 && m.key == -1, "browser index: hostile cache values clamp");
}

// Every FL plugin decoder fed synthetic state payloads (no FL data): sizes
// around typical record boundaries, filled with extreme integers / floats.
// Under the sanitizers this pins the decoder overflow fixes from the fuzzers;
// in any build it checks every decoded state normalises to finite values.
void decoderSweep() {
  auto& all = flp::decoders();
  const size_t sizes[] = {0, 1, 3, 4, 7, 16, 33, 64, 128, 255, 400, 1024, 4099};
  const uint32_t words[] = {0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x7F7FFFFFu /* FLT_MAX */, 0x7FC00000u /* NaN */,
                            0xFF800000u /* -inf */, 0x00000001u, 0x40000000u};
  std::mt19937 rng(5);
  size_t calls = 0, mapped = 0;
  bool finite = true;
  std::string bad;
  for (const auto& [name, dec] : all) {
    for (size_t sz : sizes)
      for (size_t w = 0; w <= sizeof words / sizeof words[0]; ++w) {
        std::vector<uint8_t> payload(sz);
        for (size_t i = 0; i < sz; ++i) {
          uint32_t v = w < sizeof words / sizeof words[0] ? words[w] : static_cast<uint32_t>(rng());
          payload[i] = static_cast<uint8_t>(v >> (8 * (i % 4)));
        }
        flp::File f;
        flp::Event st;
        st.id = 213;
        st.data = payload.data();
        st.size = static_cast<uint32_t>(sz);
        f.events.push_back(st);
        Song song;
        flp::Report rep;
        flp::Context c(f, song, rep);
        c.factoryData = "/nonexistent-fl-data";
        flp::PluginMapping m;
        m.flName = name;
        ++calls;
        if (!dec(c, f.events, m)) continue;
        ++mapped;
        if (m.state.type.empty()) m.state.type = m.glType;
        normalizePluginState(m.state);
        for (float v : m.state.params)
          if (!std::isfinite(v)) {
            if (finite) bad = name + " (" + std::to_string(sz) + " bytes)";
            finite = false;
          }
        if (m.param && m.value)
          for (int k : {0, 1, 7, 100, -1, 65535, 0x7FFFFFFF}) {
            int gi = m.param(k);
            (void)gi;
            (void)m.value(k, 0.5);
          }
        if (m.rawNorm) (void)m.rawNorm(1, 0x7FFFFFFF), (void)m.rawNorm(1, INT32_MIN);
      }
  }
  std::printf("# decoder sweep: %zu decoders, %zu calls, %zu mapped%s%s\n", all.size(), calls, mapped,
              bad.empty() ? "" : ", first non-finite: ", bad.c_str());
  expect(calls > 0, "every FL plugin decoder accepts arbitrary payloads (sanitizer-clean)");
  expect(finite, "decoded states normalise to finite parameters");
}

// Bug (fuzzer, sample reader): an Ogg Vorbis comment header with a huge comment
// count made stb_vorbis compute sizeof(char*) * count in int (wrapping to a
// tiny array it then wrote past) or allocate per comment for minutes.
void oggCommentCount() {
  auto page = [](uint8_t flags, uint32_t seq, const std::vector<uint8_t>& packet) {
    std::vector<uint8_t> p = {'O', 'g', 'g', 'S', 0, flags};
    for (int i = 0; i < 8; ++i) p.push_back(0);  // granule
    for (uint32_t v : {0x1234u, seq, 0u})        // serial, sequence, CRC (not checked when reading from memory)
      for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i)));
    p.push_back(1);
    p.push_back(static_cast<uint8_t>(packet.size()));
    p.insert(p.end(), packet.begin(), packet.end());
    return p;
  };
  auto le32 = [](std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  std::vector<uint8_t> id = {1, 'v', 'o', 'r', 'b', 'i', 's'};
  le32(id, 0);           // version
  id.push_back(1);       // channels
  le32(id, 44100);       // rate
  le32(id, 0), le32(id, 128000), le32(id, 0);
  id.push_back(0xB8);    // block sizes 2^8 / 2^11
  id.push_back(1);       // framing
  bool allFailFast = true;
  for (uint32_t count : {0x20000001u, 0x7FFFFFFFu, 0x10000001u, 0xFFFFFFFFu}) {
    std::vector<uint8_t> cm = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    le32(cm, 4);
    cm.insert(cm.end(), {'t', 'e', 's', 't'});
    le32(cm, count);
    for (int k = 0; k < 6; ++k) le32(cm, 3), cm.insert(cm.end(), {'a', '=', 'b'});
    cm.push_back(1);
    std::vector<uint8_t> file = page(2, 0, id);
    std::vector<uint8_t> p2 = page(0, 1, cm);
    file.insert(file.end(), p2.begin(), p2.end());
    DecodedAudio out;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = decodeOggVorbis(file.data(), file.size(), out, &err);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    allFailFast = allFailFast && !ok && s < 2.0;
  }
  expect(allFailFast, "Ogg Vorbis: corrupt comment count fails fast (no overflow, no allocation storm)");
}

// Bug (app sweep: "Backup failed: cannot replace …"): every writer of a file
// used the same "<file>.tmp", so concurrent writers (two instances sharing the
// backup folder; the browser index saved from its worker and the UI thread)
// wrote into one temp file and renamed it away from under each other.
void concurrentAtomicWrites() {
  std::string dir = gl::test::hermeticTestEnv("qa") + "/atomic";
  std::filesystem::create_directories(dir);
  const std::string file = dir + "/shared.json";
  std::atomic<int> failures{0};
  std::vector<std::thread> th;
  for (int k = 0; k < 6; ++k)
    th.emplace_back([&, k] {
      const std::string body(4096 + static_cast<size_t>(k) * 100, static_cast<char>('a' + k));
      for (int i = 0; i < 150; ++i)
        if (!writeFileAtomic(file, body)) ++failures;
    });
  for (auto& t : th) t.join();
  std::string got;
  {
    std::ifstream in(file, std::ios::binary);
    got.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  bool whole = !got.empty() && got.size() == 4096 + static_cast<size_t>(got[0] - 'a') * 100 &&
               got.find_first_not_of(got[0]) == std::string::npos;
  size_t left = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) left += e.path().filename() != "shared.json";
  expect(failures == 0, "concurrent atomic writes of one file all succeed");
  expect(whole && left == 0, "the file holds one writer's complete text, no temp files left");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--types") {  // QA tooling: every built-in plugin type, one per line
    for (const auto& p : pluginCatalog()) std::printf("%s\n", p.type.c_str());
    return 0;
  }
  gl::test::hermeticTestEnv("qa");
  paramLayout();
  Song demo = makeDemoSong();
  roundTrip(demo, "demo song");
  mergedPatternStaysLoadable();
  removeChannelDropsReferences();
  jsonNumbers();
  decoderSweep();
  oggCommentCount();
  concurrentAtomicWrites();
  const int edits = std::getenv("GRIDLINE_QA_EDITS") ? std::atoi(std::getenv("GRIDLINE_QA_EDITS")) : 2000;
  undoStress(demo, "demo song", edits, 1);
  undoStress(Song{}, "empty song", edits / 4, 7);
  for (int i = 1; i < argc; ++i) {
    Song s;
    std::string err;
    if (!loadProject(argv[i], s, &err)) {
      std::printf("# cannot load %s: %s\n", argv[i], err.c_str());
      continue;
    }
    roundTrip(s, argv[i]);
    undoStress(s, argv[i], edits, 1000 + static_cast<uint64_t>(i));
  }
  return finish();
}
