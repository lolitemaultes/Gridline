// Gridline — K-89: DAWproject import and export. A song through a .dawproject file and back
// keeps its tempo, meter, channels, instruments, levels, routing, effects, notes (with their
// per-note expression), patterns, audio clips, automation and markers; what cannot be carried is
// reported. A hand-written file in another app's style imports too.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/DawProject.h"
#include "core/NoteBend.h"
#include "core/Params.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "core/XmlDom.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
  static const fs::path d = [] {
    fs::path p = fs::temp_directory_path() / "gridline_test_k89";
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
  }();
  return d;
}

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

const Channel* byName(const Song& s, const std::string& n) {
  for (const auto& c : s.channels)
    if (c.name == n) return &c;
  return nullptr;
}

void testXml() {
  xml::Node r;
  std::string err;
  expect(xml::parse("<?xml version=\"1.0\"?>\n<!-- c --><A x=\"1 &amp; 2\" y='q'><B/><C>t &lt;x&gt; &#233;</C><![CDATA[<raw>]]></A>", r, &err), "xml: parses");
  expect(r.name == "A" && r.get("x") == "1 & 2" && r.get("y") == "q" && r.children.size() == 2, "xml: attributes and children");
  expect(r.child("C") && r.child("C")->text == "t <x> \xC3\xA9" && r.text == "<raw>", "xml: entities, numeric references and CDATA");
  xml::Node back;
  expect(xml::parse(xml::write(r), back) && back.get("x") == "1 & 2" && back.child("C")->text == r.child("C")->text, "xml: writes what it reads");
  expect(!xml::parse("<A><B></A>", back, &err) && !err.empty(), "xml: a mismatched tag is an error");
  expect(xml::numText(0.25) == "0.25" && xml::numText(3) == "3", "xml: numbers");
}

Song sample(std::string& wavPath) {
  Song s = makeEmptySong();
  s.tempo = 128;
  s.timeSigNum = 3;
  s.timeSigDen = 4;
  s.info.title = "Round trip";
  s.info.author = "Tester";
  // An insert with a reverb that sends into a return.
  MixerTrack bus;
  bus.id = s.allocateId();
  bus.name = "Bus";
  bus.volume = 0.6f;
  bus.pan = -0.5f;
  bus.slots[0].plugin = defaultPluginState("gl.reverb");
  MixerTrack ret;
  ret.id = s.allocateId();
  ret.name = "Return";
  ret.sends.push_back({s.masterId(), 1.0f, false});
  bus.sends.push_back({s.masterId(), 1.0f, false});
  bus.sends.push_back({ret.id, 0.4f, false});
  s.mixer.push_back(bus);
  s.mixer.push_back(ret);
  Channel lead;
  lead.id = s.allocateId();
  lead.name = "Lead";
  lead.plugin = defaultPluginState("gl.synth");
  lead.plugin.params[0] = 1.0f;  // (a value every kind of parameter takes)
  lead.gain = 0.5f;
  lead.pan = 0.25f;
  lead.mixerTrack = bus.id;
  s.channels.push_back(lead);
  Channel drum;
  drum.id = s.allocateId();
  drum.name = "Kick";
  drum.plugin = defaultPluginState("gl.kick");
  drum.mixerTrack = s.masterId();
  s.channels.push_back(drum);
  // Audio.
  wavPath = (dir() / "loop.wav").string();
  AudioBuffer b(2, 44100);
  for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = static_cast<float>(0.3 * std::sin(i * 0.05));
  writeWav(wavPath, b, 44100, 16);
  Channel au;
  au.id = s.allocateId();
  au.name = "Loop";
  au.plugin = defaultPluginState("gl.sampler");
  au.plugin.data = wavPath;
  au.mixerTrack = s.masterId();
  s.channels.push_back(au);
  // A pattern: notes for the lead, steps for the kick.
  Pattern& p = s.patterns[0];
  p.name = "Verse";
  Note a(0, 96, 60, 100), c(96, 192, 67, 64);
  a.pressure = {BendPoint(0, 0.2f), BendPoint(48, 0.9f)};
  c.bend = {BendPoint(0, 0), BendPoint(96, 2.0f)};
  c.timbre = {BendPoint(0, 0.25f)};
  c.slide = true;  // reported
  p.notes[lead.id] = {a, c};
  p.steps[drum.id] = std::vector<uint8_t>(16, 0);
  p.steps[drum.id][0] = p.steps[drum.id][4] = 100;
  // Placed twice, and the audio once.
  const Tick bar = s.barTicks();
  for (int k = 0; k < 2; ++k) {
    Clip x;
    x.id = s.allocateId();
    x.pattern = p.id;
    x.track = 0;
    x.start = k * 4 * bar;
    x.length = bar;
    s.playlist.push_back(x);
  }
  Clip ac;
  ac.id = s.allocateId();
  ac.kind = ClipKind::Audio;
  ac.source = au.id;
  ac.track = 2;
  ac.start = 2 * bar;
  ac.length = 192;
  ac.audioStart = 0.25;
  s.playlist.push_back(ac);
  // Volume automation of the lead, and tempo automation.
  AutomationClip va;
  va.id = s.allocateId();
  va.owner = lead.id;
  va.index = static_cast<uint32_t>(ChannelParam::Volume);
  const auto vd = describeParam(s, {lead.id, va.index});
  AutoPoint p0, p1;
  p0.pos = 0;
  p0.value = static_cast<float>(vd->toNormalized(0.2));
  p1.pos = 384;
  p1.value = static_cast<float>(vd->toNormalized(0.8));
  va.points = {p0, p1};
  s.automations.push_back(va);
  Clip vc;
  vc.id = s.allocateId();
  vc.kind = ClipKind::Automation;
  vc.source = va.id;
  vc.track = 5;
  vc.start = 96;
  vc.length = 400;
  s.playlist.push_back(vc);
  AutomationClip ta;
  ta.id = s.allocateId();
  ta.owner = kSongId;
  ta.index = static_cast<uint32_t>(SongParam::Tempo);
  const auto td = describeParam(s, songParam(SongParam::Tempo));
  AutoPoint t0, t1;
  t0.value = static_cast<float>(td->toNormalized(128));
  t1.pos = 960;
  t1.value = static_cast<float>(td->toNormalized(140));
  ta.points = {t0, t1};
  s.automations.push_back(ta);
  Clip tc;
  tc.id = s.allocateId();
  tc.kind = ClipKind::Automation;
  tc.source = ta.id;
  tc.track = 6;
  tc.start = 0;
  tc.length = 961;
  s.playlist.push_back(tc);
  Marker m;
  m.pos = 4 * bar;
  m.name = "Chorus";
  s.markers.push_back(m);
  return s;
}

void testRoundTrip() {
  std::string wav;
  const Song s = sample(wav);
  const std::string file = (dir() / "song.dawproject").string();
  dawproject::Report out;
  std::string err;
  expect(dawproject::exportFile(s, file, &out, &err), ("export: " + err).c_str());
  std::printf("# export report:\n%s", out.text().c_str());
  bool slideLine = false, stepLine = false;
  for (const auto& l : out.lines) slideLine = slideLine || l.find("slide") != std::string::npos, stepLine = stepLine || l.find("steps") != std::string::npos;
  expect(slideLine && stepLine, "export report: the slide and the steps are named");
  Song b;
  dawproject::Report in;
  expect(dawproject::importFile(file, (dir() / "audio").string(), b, &in, &err), ("import: " + err).c_str());
  std::printf("# import report:\n%s", in.text().c_str());
  expect(near(b.tempo, 128) && b.timeSigNum == 3 && b.timeSigDen == 4, "round trip: tempo and meter");
  expect(b.info.title == "Round trip" && b.info.author == "Tester", "round trip: title and artist");
  expect(b.channels.size() == 3, "round trip: three channels");
  const Channel* lead = byName(b, "Lead");
  const Channel* kick = byName(b, "Kick");
  const Channel* loop = byName(b, "Loop");
  expect(lead && kick && loop, "round trip: the channels by name");
  if (!lead || !kick || !loop) return;
  std::printf("# lead %s %f, kick %s\n", lead->plugin.type.c_str(), lead->plugin.params.empty() ? -1.0 : lead->plugin.params[0], kick->plugin.type.c_str());
  expect(lead->plugin.type == "gl.synth" && near(lead->plugin.params[0], 1.0f) && kick->plugin.type == "gl.kick", "round trip: instruments and their states");
  expect(near(lead->gain, 0.5) && near(lead->pan, 0.25), "round trip: channel volume and pan");
  const MixerTrack* bus = b.mixerTrack(lead->mixerTrack);
  expect(bus && bus->name == "Bus" && near(bus->volume, 0.6) && near(bus->pan, -0.5) && bus->slots[0].plugin.type == "gl.reverb",
         "round trip: the insert with its level, pan and effect");
  bool toMaster = false, toReturn = false;
  if (bus)
    for (const auto& sd : bus->sends) {
      if (sd.to == b.masterId() && near(sd.level, 1)) toMaster = true;
      const MixerTrack* r = b.mixerTrack(sd.to);
      if (r && r->name == "Return" && near(sd.level, 0.4)) toReturn = true;
    }
  expect(toMaster && toReturn, "round trip: routing and the send");
  expect(loop->plugin.type == "gl.sampler" && fs::exists(loop->plugin.data) && fs::file_size(loop->plugin.data) == fs::file_size(wav),
         "round trip: the audio file comes out of the archive");
  // Patterns: the two placements share one pattern with both channels' notes.
  int patClips = 0, audioClips = 0, autoClips = 0;
  ObjectId pat = kNoId;
  bool shared = true;
  for (const auto& c : b.playlist) {
    if (c.kind == ClipKind::Pattern) {
      ++patClips;
      if (pat != kNoId && c.pattern != pat) shared = false;
      pat = c.pattern;
    }
    if (c.kind == ClipKind::Audio) {
      ++audioClips;
      expect(c.start == 2 * b.barTicks() && c.length == 192 && near(c.audioStart, 0.25) && c.source == loop->id, "round trip: the audio clip");
    }
    if (c.kind == ClipKind::Automation) ++autoClips;
  }
  expect(patClips == 2 && shared, "round trip: two placements of one pattern");
  expect(audioClips == 1 && autoClips == 2, "round trip: the audio and automation clips");
  const Pattern* p = b.pattern(pat);
  expect(p && p->name == "Verse", "round trip: the pattern's name");
  if (p) {
    const auto& ln = p->notes.at(lead->id);
    expect(ln.size() == 2 && ln[0].start == 0 && ln[0].length == 96 && ln[1].pitch == 67 && ln[1].velocity == 64, "round trip: notes");
    expect(ln.size() == 2 && near(bend::value(ln[0].pressure, 48), 0.9, 0.01) && near(bend::value(ln[1].bend, 96), 2.0, 0.01) &&
               near(bend::value(ln[1].timbre, 10), 0.25, 0.01),
           "round trip: per-note pressure, pitch and timbre");
    expect(p->notes.count(kick->id) && p->notes.at(kick->id).size() == 2, "round trip: the kick's steps as notes");
  }
  // Automation.
  bool vol = false, tempo = false;
  for (const auto& a : b.automations) {
    const auto d = describeParam(b, {a.owner, a.index});
    if (a.owner == lead->id && a.index == static_cast<uint32_t>(ChannelParam::Volume) && a.points.size() == 2)
      vol = near(d->fromNormalized(a.points[0].value), 0.2, 0.01) && near(d->fromNormalized(a.points[1].value), 0.8, 0.01) && a.points[1].pos == 384;
    if (a.owner == kSongId && a.points.size() == 2) tempo = near(d->fromNormalized(a.points[1].value), 140, 0.5);
  }
  expect(vol, "round trip: volume automation");
  expect(tempo, "round trip: tempo automation");
  expect(b.markers.size() == 1 && b.markers[0].name == "Chorus" && b.markers[0].pos == 4 * b.barTicks(), "round trip: the marker");
  // A second round changes nothing more.
  const std::string file2 = (dir() / "song2.dawproject").string();
  expect(dawproject::exportFile(b, file2, nullptr, &err), "second export");
  Song c;
  expect(dawproject::importFile(file2, (dir() / "audio2").string(), c, nullptr, &err), "second import");
  expect(c.channels.size() == b.channels.size() && c.playlist.size() == b.playlist.size() && c.patterns.size() == b.patterns.size(), "second round: stable");
}

void testForeign() {
  // Another app's style: plugins by id, a decibel volume, a group track, notes straight in a lane.
  const char* text = R"(<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Other" version="5"/>
  <Transport><Tempo unit="bpm" value="97.5" id="t"/><TimeSignature numerator="6" denominator="8" id="ts"/></Transport>
  <Structure>
    <Track contentType="tracks" id="g" name="Group">
      <Channel role="regular" id="gc" destination="m"><Volume unit="decibel" value="-6" id="gv"/></Channel>
      <Track contentType="notes" id="k" name="Keys" color="#336699">
        <Channel role="regular" id="kc" destination="gc">
          <Devices><Vst3Plugin deviceID="ABCDEF0123456789ABCDEF0123456789" deviceName="Some Piano" deviceRole="instrument" id="d1" name="Piano"/></Devices>
          <Pan unit="normalized" value="0.75" id="kp"/>
        </Channel>
      </Track>
    </Track>
    <Channel role="master" id="m"/>
  </Structure>
  <Arrangement id="a">
    <Lanes timeUnit="beats" id="l">
      <Lanes track="k" id="lk">
        <Clips id="cl">
          <Clip time="4" duration="4" playStart="0" name="Riff">
            <Notes id="n"><Note time="0" duration="1" key="64" vel="0.5" channel="0"/><Note time="2" duration="0.5" key="67" vel="1"/></Notes>
          </Clip>
        </Clips>
        <Points id="pp" unit="normalized"><Target parameter="kp"/><RealPoint time="0" value="0.5" interpolation="linear"/><RealPoint time="4" value="1" interpolation="linear"/></Points>
      </Lanes>
    </Lanes>
  </Arrangement>
  <Scenes><Scene id="s1"/></Scenes>
</Project>)";
  xml::Node pr;
  expect(xml::parse(text, pr), "foreign: parses");
  Song s;
  dawproject::Report rep;
  std::string err;
  expect(dawproject::fromParts(pr, nullptr, {}, s, &rep, &err), ("foreign: imports " + err).c_str());
  std::printf("# foreign report:\n%s", rep.text().c_str());
  expect(near(s.tempo, 97.5) && s.timeSigNum == 6 && s.timeSigDen == 8, "foreign: tempo and meter");
  const Channel* keys = byName(s, "Keys");
  expect(keys && keys->plugin.type == "vst3:ABCDEF0123456789ABCDEF0123456789" && keys->color == 0x336699 && near(keys->pan, 0.5),
         "foreign: the plugin by its id, colour and pan");
  const MixerTrack* g = keys ? s.mixerTrack(keys->mixerTrack) : nullptr;
  expect(g && g->name == "Group" && near(g->volume, std::pow(10.0, -6 / 20.0)), "foreign: the group as an insert, its decibels as a level");
  int clips = 0;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern) {
      ++clips;
      const Pattern* p = s.pattern(c.pattern);
      expect(c.start == 4 * s.ppq && c.length == 4 * s.ppq && p && p->name == "Riff" && keys && p->notes.at(keys->id).size() == 2 &&
                 p->notes.at(keys->id)[1].start == 2 * s.ppq && p->notes.at(keys->id)[0].velocity == 64,
             "foreign: the clip and its notes");
    }
  expect(clips == 1, "foreign: one clip");
  bool pan = false;
  for (const auto& a : s.automations) pan = pan || (keys && a.owner == keys->id && a.index == static_cast<uint32_t>(ChannelParam::Pan) && a.points.size() == 2);
  expect(pan, "foreign: pan automation");
  bool scenes = false;
  for (const auto& l : rep.lines) scenes = scenes || l.find("scenes") != std::string::npos;
  expect(scenes, "foreign: the report names what was left out");
  xml::Node bad;
  bad.name = "Song";
  expect(!dawproject::fromParts(bad, nullptr, {}, s, nullptr, &err) && !err.empty(), "not a project: an error");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k89_dawproject");
  testXml();
  testRoundTrip();
  testForeign();
  return finish();
}
