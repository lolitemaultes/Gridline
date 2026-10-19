// Gridline — J-18 coverage sweep: the FLP event catalog and coverage tally,
// the newly decoded project events (main swing, creation date / time spent,
// channel switch, fixed pattern length), the fourth-wave plugin decoders and
// the new menu operations (markers, insert / delete space, pattern sorting,
// cut itself, swing mix, layers, unroute, submix, discard lengths, tap tempo).
// Synthetic files only — no FL Studio content.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MenuOpsExtra.h"
#include "core/MixerOps.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpCoverage.h"
#include "core/flp/FlpPresets.h"
#include "packs/modulation/modulation.h"

using namespace gl;

namespace {

struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void putF64(std::vector<uint8_t>& v, double x) {
  uint64_t u;
  std::memcpy(&u, &x, 8);
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(u >> (8 * i)));
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fwrite(bytes.data(), 1, bytes.size(), fp);
  std::fclose(fp);
  return true;
}

// ------------------------------------------------------------ event catalog
void testCatalog() {
  const auto& cat = flp::eventCatalog();
  bool sorted = true, notes = true;
  for (size_t i = 1; i < cat.size(); ++i) sorted = sorted && cat[i - 1].id < cat[i].id;
  for (const auto& e : cat) notes = notes && (e.status == flp::EventInfo::Decoded || std::strlen(e.note) > 0) && std::strlen(e.name) > 0;
  expect(sorted, "catalog: ids unique and in order");
  expect(notes, "catalog: every partial / ignored event says why");
  // Every id seen in FL Studio's shipped projects and presets (corpus histogram, J-18).
  const int seen[] = {0,   9,   10,  11,  17,  18,  19,  20,  21,  22,  23,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,
                      36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  64,  65,  67,  69,  70,
                      71,  72,  74,  75,  76,  80,  83,  85,  86,  89,  91,  92,  94,  95,  97,  98,  99,  100, 101, 103, 104, 128,
                      131, 132, 133, 135, 138, 139, 140, 142, 143, 144, 145, 146, 147, 148, 149, 150, 154, 155, 156, 157, 158, 159,
                      160, 161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201,
                      202, 203, 204, 205, 206, 207, 209, 210, 212, 213, 215, 216, 217, 218, 219, 220, 221, 223, 224, 225, 226, 227,
                      228, 229, 230, 231, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 251, 252, 253};
  bool all = true;
  for (int id : seen) all = all && flp::eventInfo(static_cast<uint8_t>(id)) != nullptr;
  expect(all, "catalog: every event id of the shipped corpus is listed");
  expect(flp::eventInfo(0)->status == flp::EventInfo::Decoded && flp::eventInfo(237)->status == flp::EventInfo::Decoded &&
             flp::eventInfo(164)->status == flp::EventInfo::Decoded && flp::eventInfo(11)->status == flp::EventInfo::Decoded,
         "catalog: the events decoded by the sweep are marked decoded");
  expect(flp::eventInfo(200)->status == flp::EventInfo::Ignored && flp::eventInfo(3) == nullptr, "catalog: licensee ignored; unknown id absent");
  expect(std::string(flp::statusName(flp::EventInfo::Partial)) == "partial", "catalog: status names");
}

// ------------------------------------------- newly decoded project events
std::vector<uint8_t> syntheticProject() {
  Builder B;
  B.text(199, "25.2.3.5164");
  B.d(156, 120000);
  B.b(11, 64);  // main swing: half
  std::vector<uint8_t> ts;
  putF64(ts, 45000.5);  // 2023-03-15 12:00 (days since 1899-12-30)
  putF64(ts, 0.25);     // six hours
  B.data(237, ts);
  B.w(65, 1);
  B.d(164, 96 * 4 * 3);  // fixed length: 3 bars at 96 PPQ, 4/4
  std::vector<uint8_t> note(24, 0);
  note[8] = 96, note[12] = 60, note[21] = 100, note[18] = 64, note[22] = 128, note[23] = 128, note[16] = 120, note[20] = 64;
  B.data(224, note);
  B.w(64, 0), B.b(0, 0), B.b(21, 0), B.text(192, "Off");  // switched off
  B.w(64, 1), B.b(0, 1), B.b(21, 0), B.text(192, "On");
  return B.file(96);
}

void testProjectEvents() {
  const std::string path = "test_coverage.flp";
  expect(writeFile(path, syntheticProject()), "write synthetic project");
  Song s;
  flp::Report rep;
  std::string err;
  bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  expect(ok, "synthetic project imports");
  expect(std::fabs(s.swing - 0.5f) < 1e-6f, "main swing 64/128 → 0.5");
  expect(s.info.created == static_cast<int64_t>((45000.5 - 25569.0) * 86400.0), "creation date → Unix seconds");
  expect(s.info.workTime == 6 * 3600, "time spent → seconds");
  expect(s.channels.size() == 2 && s.channels[0].muted && !s.channels[1].muted, "channel switch off → muted");
  expect(rep.counts["channels switched off (muted)"] == 1, "report counts switched-off channels");
  expect(!s.patterns.empty() && s.patterns[0].bars == 3, "fixed pattern length (164) sets the bars");
  // Project info survives save / load.
  Song back;
  expect(songFromJson(songToJson(s), back, &err) && back.info.created == s.info.created && back.info.workTime == s.info.workTime,
         "creation date and time spent round-trip through the project file");

  // Coverage tally over the same file.
  flp::CoverageTally t;
  flp::tallyFile(path, flp::FileKind::Project, t, "/nonexistent");
  expect(t.projects == 1 && t.events[0].projectCount == 2 && t.events[0].projectNonDefault == 1,
         "tally: event counts and non-default values");
  expect(t.events[164].projectFiles == 1 && t.events[237].projectCount == 1, "tally: per-id files");
  Json j;
  std::string js = flp::coverageJson(t);
  expect(jsonParse(js, j) && j.find("events") && j.find("decoders") && j.find("catalog"), "coverage JSON parses");
  std::remove(path.c_str());
  flp::CoverageTally bad;
  flp::tallyFile("/nonexistent/x.flp", flp::FileKind::Project, bad);
  expect(bad.unreadable == 1 && bad.projects == 0, "tally: unreadable files counted");
}

// ----------------------------------------------------- fourth-wave decoders
struct Decoded {
  bool ok = false;
  flp::PluginMapping m;
  flp::Report rep;
};
Decoded decode(const std::string& name, const std::vector<uint8_t>* state) {
  Builder B;
  B.text(201, name);
  if (state) B.data(213, *state);
  Decoded out;
  flp::File f;
  std::string err;
  if (!flp::readBytes(B.file(96), f, &err)) return out;
  Song song;
  flp::Context c(f, song, out.rep);
  const flp::PluginDecoder* dec = flp::findDecoder(name);
  out.m.flName = name;
  out.ok = dec && (*dec)(c, f.events, out.m);
  return out;
}
float param(const Decoded& d, int k) {
  return k >= 0 && static_cast<size_t>(k) < d.m.state.params.size() ? d.m.state.params[static_cast<size_t>(k)] : -999.f;
}

void testDecoders() {
  namespace md = packs::modulation;
  std::vector<uint8_t> one = {1};
  Decoded clock = decode("Fruity Big Clock", &one);
  expect(clock.ok && clock.m.glType == "gl.clock" && clock.m.exact, "Big Clock → Clock (colour scheme only)");
  expect(flp::defaultMapping("Fruity Big Clock") == "gl.clock", "default mapping of a fresh Big Clock");

  // Colour mapper: identity map (colours 8..15 have no Gridline target).
  std::vector<uint8_t> cm;
  put32(cm, 0);
  for (int k = 0; k < 16; ++k) put32(cm, static_cast<uint32_t>(k));
  for (int k = 0; k < 16; ++k) put32(cm, static_cast<uint32_t>(k));
  Decoded map = decode("VFX Color Mapper", &cm);
  expect(map.ok && map.m.glType == "gl.colormap" && !map.m.exact, "Color Mapper → Colour Router (approximate: ports above 8)");
  expect(param(map, md::CmMap + 0) == 1.f && param(map, md::CmMap + 7) == 8.f && param(map, md::CmMap + 12) == 8.f,
         "Color Mapper: port p → target p + 1 (clamped)");
  std::vector<uint8_t> cm0;
  put32(cm0, 0);
  for (int k = 0; k < 16; ++k) put32(cm0, 2);
  for (int k = 0; k < 16; ++k) put32(cm0, static_cast<uint32_t>(k));
  Decoded map0 = decode("VFX Color Mapper", &cm0);
  expect(map0.ok && map0.m.exact && param(map0, md::CmMap + 5) == 3.f, "Color Mapper: every colour to port 2 is exact");

  // Dashboard: three values (panel + two controls), two named controls.
  std::vector<uint8_t> db(28, 0);
  db[0] = 2;
  put32(db, 3);
  put32(db, 0), put32(db, 32768), put32(db, 65536);
  put32(db, 2);  // control count
  auto record = [&](const std::string& name) {
    std::vector<uint8_t> r(80, 0);
    r[0] = static_cast<uint8_t>(name.size());
    std::memcpy(&r[1], name.data(), name.size());
    r[48] = static_cast<uint8_t>(name.size());
    std::memcpy(&r[49], name.data(), name.size());
    db.insert(db.end(), r.begin(), r.end());
  };
  record("Cutoff"), record("Slider2");
  db.resize(db.size() + 16, 0);
  Decoded dash = decode("Dashboard", &db);
  expect(dash.ok && dash.m.glType == "gl.surface" && !dash.m.exact, "Dashboard → Control Surface");
  expect(std::fabs(param(dash, 0) - 0.5f) < 1e-6f && std::fabs(param(dash, 1) - 1.0f) < 1e-6f, "Dashboard: control values");
  expect(dash.m.state.data.find("Cutoff") != std::string::npos &&
             dash.m.state.data.find(std::to_string(md::SkSlider) + " Slider2") != std::string::npos,
         "Dashboard: control names and slider kind");

  // Soundfont Player: the .sf2 path (UTF-16) inside the state.
  std::vector<uint8_t> sf = {9, 0, 0, 0};
  for (char ch : std::string("/sounds/Grand Piano.sf2")) sf.push_back(static_cast<uint8_t>(ch)), sf.push_back(0);
  sf.push_back(0), sf.push_back(0), sf.push_back(7);
  Decoded sfp = decode("Soundfont Player", &sf);
  expect(sfp.ok && sfp.m.glType == "gl.multisampler" && sfp.m.state.data.find("sf2 0 0 - | /sounds/Grand Piano.sf2") != std::string::npos,
         "Soundfont Player → Multisampler with the SoundFont");
  std::vector<uint8_t> nosf = {1, 2, 3, 4};
  Decoded sfn = decode("Soundfont Player", &nosf);
  expect(sfn.ok && sfn.m.state.data.empty() && !sfn.rep.warnings.empty(), "Soundfont Player without a file: warning");

  // FL Studio Mobile rack: first module decides the effect.
  auto rack = [](std::vector<uint32_t> types) {
    std::vector<uint8_t> r = {'S', 'S', 'L', 'F', 0, 0, 0, 0};
    for (uint32_t t : types) {
      r.insert(r.end(), {'R', 'M', 'O', 'd'});
      put32(r, 8), put32(r, t), put32(r, 1);
    }
    return r;
  };
  auto chorus = rack({11, 35});
  Decoded mob = decode("FL Studio Mobile Rack FX", &chorus);
  expect(mob.ok && mob.m.glType == "gl.chorus" && !mob.rep.warnings.empty(), "Mobile rack: first module (chorus), extra modules warned");
  auto empty = rack({});
  Decoded mobE = decode("FL Studio Mobile Rack FX", &empty);
  expect(mobE.ok && mobE.m.glType == "gl.utility" && mobE.m.exact, "Mobile rack: empty rack passes through");
  auto unknown = rack({999});
  expect(!decode("FL Studio Mobile Rack FX", &unknown).ok, "Mobile rack: unknown module stays unmapped");

  // Phase inverter: State knob.
  std::vector<uint8_t> inv;
  put32(inv, 1), put32(inv, 1024);
  std::vector<uint8_t> thru;
  put32(thru, 1), put32(thru, 0);
  Decoded pi = decode("Fruity Phase Inverter", &inv), pt = decode("Fruity Phase Inverter", &thru);
  expect(pi.ok && pi.m.glType == "gl.utility" && param(pi, pp::UtlInvert) == 1.f && param(pt, pp::UtlInvert) == 0.f,
         "Phase Inverter → Utility polarity");

  // No shipped state: default instances of the equivalents, with a warning.
  const char* names[] = {"Wasp", "Wasp XT", "FL Slayer", "Fruity LSD", "Fruity Mute 2", "VFX Level Scaler", "VFX Sequencer"};
  const char* types[] = {"gl.analog", "gl.analog", "gl.string", "gl.midiout", "gl.mute", "gl.velscale", "gl.notesq"};
  bool allOk = true;
  for (size_t i = 0; i < 7; ++i) {
    Decoded x = decode(names[i], nullptr);
    allOk = allOk && x.ok && x.m.glType == types[i] && !x.m.exact && !x.rep.warnings.empty() && findPlugin(types[i]);
  }
  expect(allOk, "remaining stock plugins map to Gridline equivalents (defaults, flagged)");
  expect(flp::findDecoder("Fruity Peak Controller") != nullptr, "Peak Controller decoder registered");

  // An empty mixer preset (no plugins) is an empty effect chain.
  Builder E;
  E.text(199, "25.2.3.5164");
  E.data(236, std::vector<uint8_t>(12, 0));
  for (int k = 0; k < 10; ++k) E.w(98, static_cast<uint16_t>(k));
  E.data(225, std::vector<uint8_t>(12, 0));
  const std::string chainPath = "test_coverage_chain.fst";
  writeFile(chainPath, E.file(96));
  flp::PresetResult r;
  bool chainOk = flp::importFlPreset(chainPath, r, "/nonexistent");
  std::remove(chainPath.c_str());
  expect(chainOk && r.kind == flp::PresetResult::Chain && r.chain.slots.size() == 10 && r.chain.slots[0].empty() && r.exact,
         "empty mixer preset → empty effect chain");
}

// ------------------------------------------------------------ menu operations
struct Fixture {
  Document d{makeEmptySong()};
  ObjectId kick = kNoId, lead = kNoId, pat = kNoId;
  Tick bar = 0;
  Fixture() {
    kick = d.addChannel("Kick", "gl.kick");
    lead = d.addChannel("Lead", "gl.synth");
    pat = d.song().patterns[0].id;
    d.setNotes(pat, lead, {{0, 96, 60, 100}});
    bar = d.song().barTicks();
    d.addClip(pat, 0, 0, bar);          // [0, 1 bar)
    d.addClip(pat, 1, bar, 2 * bar);    // [1, 3 bars)
    d.addClip(pat, 2, 4 * bar, bar);    // [4, 5 bars)
    d.reset(d.song());
  }
};

void testMarkers() {
  Fixture f;
  Song base = f.d.song();
  expect(ops::nextMarkerName(f.d.song()) == "Marker 1", "first marker name");
  expect(ops::addMarker(f.d, 4 * f.bar, "Chorus") == 0, "add marker");
  expect(ops::addMarker(f.d, f.bar, "Verse") == 0 && f.d.song().markers[1].name == "Chorus", "markers stay sorted");
  int ts = ops::addMarker(f.d, 2 * f.bar, "3/4", 3, 4);
  expect(ts == 1 && f.d.song().markers[1].timeSigNum == 3 && f.d.song().markers[1].timeSigDen == 4, "time signature marker");
  expect(ops::nextMarker(f.d.song(), f.bar) == 1 && ops::previousMarker(f.d.song(), f.bar) == -1 && ops::previousMarker(f.d.song(), 5 * f.bar) == 2,
         "next / previous marker");
  expect(ops::renameMarker(f.d, 0, "Intro") && f.d.song().markers[0].name == "Intro", "rename marker");
  expect(!ops::renameMarker(f.d, 9, "x") && !ops::removeMarker(f.d, -1) && ops::addMarker(f.d, -5, "x") == -1, "invalid marker edits refused");
  expect(ops::removeMarkersIn(f.d, 0, 3 * f.bar) == 2 && f.d.song().markers.size() == 1, "delete markers in a range");
  expect(ops::removeMarker(f.d, 0) && f.d.song().markers.empty(), "delete marker");
  for (int i = 0; i < 6; ++i) f.d.undo();
  expect(f.d.song() == base, "marker edits undo one step each");
}

void testSpace() {
  Fixture f;
  Song base = f.d.song();
  ops::addMarker(f.d, 4 * f.bar, "Drop");
  f.d.reset(f.d.song());
  Song withMarker = f.d.song();
  expect(ops::insertSpace(f.d, f.bar, f.bar), "insert space");
  const auto& p = f.d.song().playlist;
  expect(p[0].start == 0 && p[1].start == 2 * f.bar && p[2].start == 5 * f.bar && f.d.song().markers[0].pos == 5 * f.bar,
         "insert space moves later clips and markers");
  f.d.undo();
  expect(f.d.song() == withMarker, "insert space undoes");
  // Delete [2, 4.5) bars: clip 2 [1,3) ends at 2, clip 3 [4,5) loses its head, marker at 4 is removed.
  expect(ops::deleteSpace(f.d, 2 * f.bar, 4 * f.bar + f.bar / 2), "delete space");
  const auto& q = f.d.song().playlist;
  expect(q.size() == 3 && q[1].start == f.bar && q[1].length == f.bar, "clip running into the range ends at its start");
  expect(q[2].start == 2 * f.bar && q[2].offset == f.bar / 2 && q[2].length == f.bar / 2, "clip starting inside loses its head");
  expect(f.d.song().markers.empty(), "markers inside the range are removed");
  f.d.undo();
  expect(f.d.song() == withMarker, "delete space undoes");
  // A range that holds a whole clip deletes it; a spanning clip gets shorter.
  expect(ops::deleteSpace(f.d, 4 * f.bar, 5 * f.bar) && f.d.song().playlist.size() == 2, "clip inside the range is deleted");
  f.d.undo();
  expect(ops::deleteSpace(f.d, f.bar + f.bar / 2, 2 * f.bar) && f.d.song().playlist[1].length == 2 * f.bar - f.bar / 2,
         "spanning clip gets shorter by the range");
  expect(!ops::deleteSpace(f.d, 3, 3) && !ops::insertSpace(f.d, 0, 0), "empty ranges refused");
  (void)base;
}

void testPatternsAndChannels() {
  Fixture f;
  ObjectId b = f.d.addPattern("beta"), a = f.d.addPattern("Alpha");
  f.d.reset(f.d.song());
  Song base = f.d.song();
  expect(ops::sortPatterns(f.d, ops::PatternSort::Name) && f.d.song().patterns[0].id == a && f.d.song().patterns[1].id == b,
         "sort patterns by name (case-insensitive)");
  f.d.undo();
  expect(f.d.song() == base, "sort patterns undoes");
  ops::setPatternColor(f.d, b, 0x000001);
  expect(ops::sortPatterns(f.d, ops::PatternSort::Color) && f.d.song().patterns[0].id == b, "sort patterns by colour");

  // Cut itself: each channel its own unused group.
  f.d.editChannel(f.kick, "cut", [](Channel& c) { c.cutGroup = 1, c.cutBy = 2; });
  expect(ops::cutItself(f.d, {f.kick, f.lead}), "cut itself");
  const Channel* k = f.d.song().channel(f.kick);
  const Channel* l = f.d.song().channel(f.lead);
  expect(k->cutGroup == k->cutBy && l->cutGroup == l->cutBy && k->cutGroup != l->cutGroup && k->cutGroup > 2 && l->cutGroup > 2,
         "cut itself: distinct new groups");
  int g = l->cutGroup;
  ops::cutItself(f.d, {f.lead});
  expect(f.d.song().channel(f.lead)->cutGroup == g, "cut itself is idempotent");

  expect(ops::setSwingMix(f.d, {f.kick}, 1.7f) && f.d.song().channel(f.kick)->swingMix == 1.0f, "swing mix (clamped)");

  // Layer: children in order, placed after the last one, never nests.
  ObjectId lay = ops::createLayer(f.d, {f.lead, f.kick});
  const Song& s = f.d.song();
  expect(lay && s.channels.size() == 3 && s.channels[2].id == lay && s.channel(lay)->layer.size() == 2 &&
             s.channel(lay)->plugin.type.empty(),
         "create layer with selected channels");
  expect(ops::createLayer(f.d, {lay}) == kNoId, "layers do not nest");
}

void testMixer() {
  Document d{makeEmptySong()};
  ObjectId a = d.addMixerTrack(), b = d.addMixerTrack(), c = d.addMixerTrack();
  mixops::route(d, {a}, {c}, mixops::RouteMode::Only);
  d.setSend(b, c, 1.0f);
  d.reset(d.song());
  Song base = d.song();
  expect(mixops::unrouteIncoming(d, c) == 2, "unroute both tracks routed here");
  const MixerTrack* ta = d.song().mixerTrack(a);
  const MixerTrack* tb = d.song().mixerTrack(b);
  expect(ta->sends.size() == 1 && ta->sends[0].to == d.song().masterId(), "a track left without a route goes to the master");
  bool bToC = false;
  for (const auto& snd : tb->sends) bToC = bToC || snd.to == c;
  expect(!bToC && !tb->sends.empty(), "other routes stay");
  d.undo();
  expect(d.song() == base, "unroute undoes");
  expect(mixops::unrouteIncoming(d, d.song().masterId()) == 0, "the master is never unrouted");

  ObjectId sub = mixops::createSubmix(d, {a, b}, "Submix");
  const Song& s = d.song();
  expect(sub && s.mixerIndex(sub) == s.mixerIndex(b) + 1 && s.mixerTrack(sub)->name == "Submix", "submix inserted after the sources");
  expect(s.mixerTrack(a)->sends.size() == 1 && s.mixerTrack(a)->sends[0].to == sub && s.mixerTrack(b)->sends.size() == 1 &&
             s.mixerTrack(b)->sends[0].to == sub,
         "sources route only to the submix");
  d.undo();
  expect(d.song() == base, "submix undoes in one step");
}

void testNotesAndTapper() {
  std::vector<Note> notes = {{0, 96, 60, 100}, {96, 300, 62, 100}, {400, 12, 64, 100}};
  ops::NoteSel sel = {false, true, false};
  ops::discardLengths(notes, sel, 24);
  expect(notes[0].length == 96 && notes[1].length == 24 && notes[2].length == 12, "discard lengths: selected notes only");
  ops::NoteSel none;
  ops::discardLengths(notes, none, 24);
  expect(notes[0].length == 24 && notes[2].length == 24, "discard lengths: all notes when none selected");

  expect(ops::followScroll(100, 300, 800) == 100, "auto scroll: position visible → no change");
  expect(std::fabs(ops::followScroll(100, 1000, 800) - (1000 - 32)) < 1e-3f && ops::followScroll(500, 100, 800) == 68.f,
         "auto scroll: next page starts just before the position");
  ops::TapTempo t;
  expect(t.tap(10.0) == 0, "one tap: no tempo yet");
  t.tap(10.5), t.tap(11.0);
  expect(std::fabs(t.tap(11.5) - 120.0) < 1e-9, "taps every 0.5 s → 120 BPM");
  expect(t.tap(20.0) == 0 && t.taps.size() == 1, "a long pause starts over");
  for (int i = 1; i <= 20; ++i) t.tap(20.0 + i * 0.25);
  expect(static_cast<int>(t.taps.size()) == t.window && std::fabs(t.bpm() - 240.0) < 1e-6, "window of recent taps");
}

}  // namespace

int main() {
  testCatalog();
  testProjectEvents();
  testDecoders();
  testMarkers();
  testSpace();
  testPatternsAndChannels();
  testMixer();
  testNotesAndTapper();
  return finish();
}
