// Gridline - drum kits in the drumkit.xml / .h2drumkit format (core/HydrogenKit.h). Every kit here
// is made by the test itself from generated audio:
//   * the kit file of each version (current, 1.x with a component list, 0.9, the oldest), entities,
//     CDATA, comments, namespaces, damaged and foreign files;
//   * gzip and tar (stored deflate blocks written here, ustar prefixes, GNU and pax long names),
//     names that would leave the folder, cut short archives, the local cache copy;
//   * the kit on Abacus: velocity layers sample by sample (which layer answers a hit), round robin,
//     mute groups choking, stacked components, levels, pan, pitch, envelope, more than 32
//     instruments, missing samples and the notes of the report;
//   * the browser's file kinds and drop actions, the project's file references.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../../src/core/BrowserIndex.h"
#include "../../src/core/HydrogenKit.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/PluginParams.h"
#include "../../src/core/ProjectArchive.h"
#include "../../src/core/ProjectInterop.h"
#include "../../src/core/Zip.h"
#include "../../src/engine/Generators.h"
#include "../../src/engine/Slicing.h"
#include "../Check.h"

using namespace gl;
using namespace gl::pp;
namespace h2 = gl::hydrogen;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

fs::path scratch() {
  static fs::path p = [] {
    fs::path d = fs::temp_directory_path() / ("gl-h2-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
  }();
  return p;
}

// 16-bit mono WAV of `frames` frames at one level (a sign shows which layer played).
std::vector<uint8_t> wavBytes(int frames, float level) {
  std::vector<uint8_t> b;
  auto u32 = [&b](uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto u16 = [&b](uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
  };
  const uint32_t data = static_cast<uint32_t>(frames) * 2;
  b.insert(b.end(), {'R', 'I', 'F', 'F'});
  u32(36 + data);
  b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16), u16(1), u16(1), u32(44100), u32(88200), u16(2), u16(16);
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  u32(data);
  const int16_t v = static_cast<int16_t>(std::lround(std::clamp(level, -1.0f, 1.0f) * 32000.0f));
  for (int i = 0; i < frames; ++i) u16(static_cast<uint16_t>(v));
  return b;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void writeText(const fs::path& p, const std::string& s) { writeFile(p, std::vector<uint8_t>(s.begin(), s.end())); }

// ---------------------------------------------------------------- tar and gzip writers (test only)
void tarHeader(std::vector<uint8_t>& out, const std::string& name, uint64_t size, char type, const std::string& prefix = "") {
  uint8_t h[512] = {};
  std::memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
  std::snprintf(reinterpret_cast<char*>(h + 100), 8, "%07o", 0644);
  std::snprintf(reinterpret_cast<char*>(h + 108), 8, "%07o", 0);
  std::snprintf(reinterpret_cast<char*>(h + 116), 8, "%07o", 0);
  std::snprintf(reinterpret_cast<char*>(h + 124), 12, "%011llo", static_cast<unsigned long long>(size));
  std::snprintf(reinterpret_cast<char*>(h + 136), 12, "%011o", 0);
  h[156] = static_cast<uint8_t>(type);
  std::memcpy(h + 257, "ustar", 6);
  h[263] = '0', h[264] = '0';
  std::memcpy(h + 345, prefix.data(), std::min<size_t>(prefix.size(), 155));
  std::memset(h + 148, ' ', 8);
  unsigned sum = 0;
  for (uint8_t c : h) sum += c;
  std::snprintf(reinterpret_cast<char*>(h + 148), 8, "%06o", sum);
  out.insert(out.end(), h, h + 512);
}
void tarData(std::vector<uint8_t>& out, const std::vector<uint8_t>& d) {
  out.insert(out.end(), d.begin(), d.end());
  out.resize((out.size() + 511) / 512 * 512, 0);
}
struct TarFile {
  std::string name;
  std::vector<uint8_t> data;
};
std::vector<uint8_t> makeTar(const std::vector<TarFile>& files, bool gnuLong = false, bool pax = false) {
  std::vector<uint8_t> t;
  for (const auto& f : files) {
    if (f.name.size() > 100 && gnuLong) {
      std::vector<uint8_t> n(f.name.begin(), f.name.end());
      n.push_back(0);
      tarHeader(t, "././@LongLink", n.size(), 'L');
      tarData(t, n);
      tarHeader(t, f.name.substr(0, 99), f.data.size(), '0');
    } else if (f.name.size() > 100 && pax) {
      std::string rec = " path=" + f.name + "\n";
      std::string len = std::to_string(rec.size() + 2);
      if (len.size() + rec.size() != std::stoul(len)) len = std::to_string(rec.size() + len.size());
      rec = len + rec;
      tarHeader(t, "PaxHeader", rec.size(), 'x');
      tarData(t, std::vector<uint8_t>(rec.begin(), rec.end()));
      tarHeader(t, f.name.substr(0, 99), f.data.size(), '0');
    } else if (f.name.size() > 100) {
      const size_t cut = f.name.rfind('/', f.name.size() - 2);
      tarHeader(t, f.name.substr(cut + 1), f.data.size(), '0', f.name.substr(0, cut));
    } else {
      tarHeader(t, f.name, f.data.size(), f.name.back() == '/' ? '5' : '0');
    }
    tarData(t, f.data);
  }
  t.resize(t.size() + 1024, 0);
  return t;
}
// gzip with stored deflate blocks.
std::vector<uint8_t> gzipStored(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> o = {0x1F, 0x8B, 8, 8, 0, 0, 0, 0, 0, 3};
  for (char c : std::string("kit.tar")) o.push_back(static_cast<uint8_t>(c));
  o.push_back(0);
  size_t p = 0;
  do {
    const size_t n = std::min<size_t>(65535, in.size() - p);
    o.push_back(p + n >= in.size() ? 1 : 0);
    o.push_back(static_cast<uint8_t>(n)), o.push_back(static_cast<uint8_t>(n >> 8));
    o.push_back(static_cast<uint8_t>(~n)), o.push_back(static_cast<uint8_t>((~n) >> 8));
    o.insert(o.end(), in.begin() + static_cast<std::ptrdiff_t>(p), in.begin() + static_cast<std::ptrdiff_t>(p + n));
    p += n;
  } while (p < in.size());
  const uint32_t crc = crc32(in.data(), in.size()), sz = static_cast<uint32_t>(in.size());
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(sz >> (8 * i)));
  return o;
}

// ---------------------------------------------------------------- kit files
const char* kCurrentKit = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- made by the test -->
<drumkit_info xmlns="http://www.hydrogen-music.org/drumkit" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
 <formatVersion>2</formatVersion>
 <name>Test Kit &amp; Co</name>
 <userVersion>3</userVersion>
 <author>Gridline Tests</author>
 <info><![CDATA[<p>A <b>tiny</b> kit</p>]]></info>
 <license>CC BY-SA 4.0</license>
 <tags><tag>rock</tag><tag>acoustic</tag></tags>
 <image>kit.png</image>
 <imageLicense>CC0</imageLicense>
 <instrumentList>
  <instrument>
   <id>0</id><name>Kick</name><volume>0.9</volume><isMuted>false</isMuted><isSoloed>false</isSoloed>
   <pan>-0.25</pan><pitchOffset>-2</pitchOffset><randomPitchFactor>0</randomPitchFactor><gain>1</gain>
   <applyVelocity>true</applyVelocity><filterActive>false</filterActive><filterCutoff>1</filterCutoff><filterResonance>0</filterResonance>
   <Attack>441</Attack><Decay>4410</Decay><Sustain>0.5</Sustain><Release>2205</Release>
   <muteGroup>-1</muteGroup><midiOutChannel>-1</midiOutChannel><midiOutNote>36</midiOutNote><isStopNote>true</isStopNote>
   <isHihat>-1</isHihat><lower_cc>0</lower_cc><higher_cc>127</higher_cc>
   <instrumentComponent>
    <name>Main</name><gain>0.8</gain><isMuted>false</isMuted><isSoloed>false</isSoloed><sampleSelectionAlgo>VELOCITY</sampleSelectionAlgo>
    <layer><filename>soft.wav</filename><min>0</min><max>0.5</max><gain>1</gain><pitch>0</pitch><isMuted>false</isMuted><isSoloed>false</isSoloed></layer>
    <layer><filename>loud.wav</filename><min>0.5</min><max>1</max><gain>0.5</gain><pitch>1.5</pitch><isMuted>false</isMuted><isSoloed>false</isSoloed></layer>
   </instrumentComponent>
   <instrumentComponent>
    <name>Room</name><gain>1</gain><isMuted>true</isMuted><isSoloed>false</isSoloed>
    <layer><filename>room.wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch><isMuted>false</isMuted><isSoloed>false</isSoloed></layer>
   </instrumentComponent>
  </instrument>
  <instrument>
   <id>1</id><name>Snare</name><volume>1</volume><isMuted>false</isMuted><isSoloed>false</isSoloed>
   <pitchOffset>0</pitchOffset><randomPitchFactor>0.2</randomPitchFactor><gain>1.5</gain>
   <applyVelocity>false</applyVelocity><filterActive>true</filterActive><filterCutoff>0.4</filterCutoff><filterResonance>0.2</filterResonance>
   <Attack>0</Attack><Decay>0</Decay><Sustain>1</Sustain><Release>1000</Release>
   <muteGroup>3</muteGroup><isHihat>-1</isHihat><lower_cc>0</lower_cc><higher_cc>127</higher_cc>
   <instrumentComponent>
    <name>Main</name><gain>1</gain><isMuted>false</isMuted><isSoloed>false</isSoloed><sampleSelectionAlgo>ROUND_ROBIN</sampleSelectionAlgo>
    <layer><filename>soft.wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch><isMuted>false</isMuted><isSoloed>false</isSoloed></layer>
    <layer><filename>LOUD.WAV</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch><isMuted>false</isMuted><isSoloed>false</isSoloed></layer>
   </instrumentComponent>
  </instrument>
 </instrumentList>
</drumkit_info>
)";

// 1.x: a kit-wide component list, side pans, the choice of samples on the instrument.
const char* kOneKit = R"(<?xml version="1.0" encoding="UTF-8"?>
<drumkit_info xmlns="http://www.hydrogen-music.org/drumkit">
 <name>Old Kit</name><author>Someone</author><info>Info</info><license>GPL</license>
 <componentList>
  <drumkitComponent><id>0</id><name>Main</name><volume>0.5</volume></drumkitComponent>
  <drumkitComponent><id>1</id><name>Room</name><volume>1</volume></drumkitComponent>
 </componentList>
 <instrumentList>
  <instrument>
   <id>0</id><name>Hat closed</name><volume>1</volume><isMuted>false</isMuted>
   <pan_L>1</pan_L><pan_R>0.5</pan_R>
   <gain>1</gain><Attack>0</Attack><Decay>0</Decay><Sustain>1</Sustain><Release>1000</Release>
   <muteGroup>7</muteGroup><sampleSelectionAlgo>RANDOM</sampleSelectionAlgo>
   <instrumentComponent><component_id>0</component_id><gain>1</gain>
    <layer><filename>a.wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch></layer>
   </instrumentComponent>
   <instrumentComponent><component_id>1</component_id><gain>0.5</gain>
    <layer><filename>b.wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch></layer>
   </instrumentComponent>
  </instrument>
  <instrument>
   <id>1</id><name>Hat open</name><volume>1</volume><isMuted>true</isMuted>
   <pan_L>0.25</pan_L><pan_R>1</pan_R><muteGroup>7</muteGroup>
   <instrumentComponent><component_id>0</component_id><gain>1</gain>
    <layer><filename>c.wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch></layer>
   </instrumentComponent>
  </instrument>
 </instrumentList>
</drumkit_info>)";

// 0.9: layers inside the instrument; the oldest: one file named on it.
const char* kOldKit = R"(<drumkit_info>
 <name>Older</name><author>A</author><info/><license/>
 <instrumentList>
  <instrument><id>0</id><name>Tom</name><volume>1</volume>
   <layer><filename>t1.wav</filename><min>0</min><max>0.3</max><gain>1</gain><pitch>0</pitch></layer>
   <layer><filename>t2.wav</filename><min>0.3</min><max>0.7</max><gain>1</gain><pitch>0</pitch></layer>
   <layer><filename>t3.wav</filename><min>0.7</min><max>1</max><gain>1</gain><pitch>0</pitch></layer>
  </instrument>
  <instrument><id>1</id><name>Crash</name><volume>1</volume><filename>crash.wav</filename></instrument>
 </instrumentList>
</drumkit_info>)";

// ---------------------------------------------------------------- rendering
constexpr double kSr = 44100.0;
ProcessContext ctx() {
  ProcessContext c;
  c.sampleRate = kSr;
  return c;
}
struct Player {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
  uint32_t id = 1;
  explicit Player(const PluginState& s) : st(s) {
    prepareBuiltinAux(st, aux);
    g = createGenerator("gl.pads");
  }
  void hit(int note, float vel) { g->noteOn(id++, note, vel, 0); }
  std::vector<float> run(int frames) {
    std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
    for (int p = 0; p < frames; p += 128) g->process(st, aux, ctx(), l.data() + p, r.data() + p, std::min(128, frames - p));
    for (size_t i = 0; i < l.size(); ++i) l[i] += r[i];  // mono sum
    return l;
  }
};
double mean(const std::vector<float>& v, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < v.size(); ++i) s += v[i];
  return b > a ? s / static_cast<double>(b - a) : 0;
}

float param(const PluginState& s, int i) { return s.params[static_cast<size_t>(i)]; }

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--make-kit") {  // for the app test: <dir>/Test Kit/ and <dir>/Test Kit.h2drumkit
    const fs::path d = fs::u8path(argv[2]);
    std::vector<TarFile> files = {{"Test Kit/drumkit.xml", std::vector<uint8_t>(kCurrentKit, kCurrentKit + std::strlen(kCurrentKit))},
                                  {"Test Kit/soft.wav", wavBytes(22050, 0.5f)},
                                  {"Test Kit/loud.wav", wavBytes(22050, -0.5f)}};
    for (const auto& f : files) writeFile(d / fs::u8path(f.name), f.data);
    writeFile(d / "Test Kit.h2drumkit", gzipStored(makeTar(files)));
    return 0;
  }
  const fs::path root = scratch();
  // ---------------------------------------------------------------- the current format
  {
    h2::Kit k;
    std::string err;
    expect(h2::parseDrumkitXml(kCurrentKit, k, &err), "current: parses (" + err + ")");
    expect(k.name == "Test Kit & Co" && k.author == "Gridline Tests" && k.license == "CC BY-SA 4.0" && k.formatVersion == 2, "current: name, author, licence, version");
    expect(k.info == "<p>A <b>tiny</b> kit</p>", "current: CDATA info kept as written");
    expect(k.tags.size() == 2 && k.tags[1] == "acoustic" && k.image == "kit.png" && k.imageLicense == "CC0", "current: tags and picture");
    expect(k.instruments.size() == 2, "current: two instruments");
    const h2::Instrument& kick = k.instruments[0];
    expect(kick.name == "Kick" && std::fabs(kick.volume - 0.9f) < 1e-6f && std::fabs(kick.pan + 0.25f) < 1e-6f && kick.pitch == -2.0f, "current: kick volume, pan, pitch");
    expect(kick.attack == 441 && kick.decay == 4410 && kick.sustain == 0.5f && kick.release == 2205 && kick.stopNote, "current: envelope and stop note");
    expect(kick.components.size() == 2 && kick.components[0].layers.size() == 2 && kick.components[1].muted, "current: components and their layers");
    expect(kick.components[0].layers[1].file == "loud.wav" && kick.components[0].layers[1].pitch == 1.5f && kick.components[0].layers[1].gain == 0.5f,
           "current: layer file, pitch and gain");
    const h2::Instrument& sn = k.instruments[1];
    expect(sn.components[0].select == h2::Select::RoundRobin && sn.muteGroup == 3 && !sn.applyVelocity && sn.filterActive && sn.randomPitch > 0.1f,
           "current: round robin, mute group, velocity off, filter, random pitch");
  }
  // ---------------------------------------------------------------- 1.x and older
  {
    h2::Kit k;
    std::string err;
    expect(h2::parseDrumkitXml(kOneKit, k, &err) && k.kitComponents.size() == 2 && k.kitComponents[0].volume == 0.5f, "1.x: the kit's component list");
    expect(k.instruments.size() == 2 && k.instruments[0].components.size() == 2 && k.instruments[0].components[1].id == 1, "1.x: components point at the list");
    expect(k.instruments[0].components[0].select == h2::Select::Random && k.instruments[0].components[1].select == h2::Select::Random,
           "1.x: the instrument's sample choice reaches its components");
    expect(std::fabs(k.instruments[0].pan + 0.5f) < 1e-6f && std::fabs(k.instruments[1].pan - 0.75f) < 1e-6f, "1.x: side levels become a pan");
    expect(k.instruments[1].muted, "1.x: muted instrument");
    h2::Kit o;
    expect(h2::parseDrumkitXml(kOldKit, o, &err) && o.instruments.size() == 2, "0.9: parses");
    expect(o.instruments[0].components.size() == 1 && o.instruments[0].components[0].layers.size() == 3, "0.9: layers inside the instrument");
    expect(o.instruments[1].components[0].layers.size() == 1 && o.instruments[1].components[0].layers[0].file == "crash.wav", "oldest: one file on the instrument");
    expect(o.instruments[1].release == 1000 && o.instruments[1].muteGroup == -1 && o.instruments[1].pan == 0, "oldest: defaults");
  }
  // ---------------------------------------------------------------- damaged and foreign files
  {
    h2::Kit k;
    std::string err;
    expect(!h2::parseDrumkitXml("", k, &err), "empty text refused");
    expect(!h2::parseDrumkitXml("<song><name>x</name></song>", k, &err) && !err.empty(), "another XML document refused");
    expect(!h2::parseDrumkitXml("<drumkit_info><name>x</name><instrumentList><instrument><name>a", k, &err), "a cut short file refused");
    expect(!h2::parseDrumkitXml("<drumkit_info><name>x</name></drumkit_info>", k, &err), "a kit without instruments refused");
    std::string deep = "<drumkit_info>";
    for (int i = 0; i < 200; ++i) deep += "<a>";
    expect(!h2::parseDrumkitXml(deep, k, &err), "nesting too deep refused");
    expect(h2::parseDrumkitXml("\xEF\xBB\xBF<?xml version='1.0'?><!DOCTYPE x><kit:drumkit_info xmlns:kit='u'><kit:name>&#72;&#x69;</kit:name>"
                               "<kit:instrumentList><kit:instrument><kit:name>A</kit:name></kit:instrument></kit:instrumentList></kit:drumkit_info>",
                               k, &err) &&
               k.name == "Hi" && k.instruments.size() == 1,
           "byte order mark, doctype, prefixes and character references");
    // Every prefix of a valid file: never a crash, never a kit.
    const std::string full = kCurrentKit;
    int bad = 0;
    for (size_t n = 0; n + 20 < full.size(); n += 37) bad += h2::parseDrumkitXml(full.substr(0, n), k, &err) ? 1 : 0;
    expect(bad == 0, "cut short at any point: refused");
  }
  // ---------------------------------------------------------------- a kit folder on disk, rendered
  const fs::path kitDir = root / "Test Kit";
  writeText(kitDir / "drumkit.xml", kCurrentKit);
  writeFile(kitDir / "soft.wav", wavBytes(22050, 0.5f));
  writeFile(kitDir / "loud.wav", wavBytes(22050, -0.5f));
  {
    h2::Kit k;
    std::string err;
    expect(h2::isKitFolder(kitDir.u8string()) && !h2::isKitFolder(root.u8string()), "kit folder recognised");
    expect(h2::readKit(kitDir.u8string(), k, &err) && k.folder == kitDir.u8string(), "kit read from its folder");
    expect(h2::readKit((kitDir / "drumkit.xml").u8string(), k, &err), "kit read from its drumkit.xml");
    expect(h2::kitXmlIn(root.u8string()) == (kitDir / "drumkit.xml").u8string(), "the kit in the one folder inside");
    {
      std::error_code e2;  // (a file system that ignores case finds the name as written)
      expect(fs::equivalent(fs::u8path(h2::samplePath(k, "LOUD.WAV")), kitDir / "loud.wav", e2), "sample names match without regard to case");
    }
    expect(h2::samplePath(k, "/somewhere/else/soft.wav") == (kitDir / "soft.wav").u8string(), "an absolute path of another computer finds the kit's file");
    writeFile(kitDir / "old.flac", wavBytes(10, 0.1f));
    expect(h2::samplePath(k, "old.wav") == (kitDir / "old.flac").u8string(), "a renamed .flac is found");

    const h2::PadsImport p = h2::kitToPads(k);
    expect(p.channels.size() == 1 && p.names[0] == "Test Kit & Co" && p.instruments == 2, "one Abacus channel named after the kit");
    const PluginState& s = p.channels[0];
    expect(s.type == "gl.pads" && s.params.size() == static_cast<size_t>(PadsCount), "Abacus state of full size");
    expect(param(s, PadsBaseNote) == 36.0f && param(s, PadsVelocity) == 1.0f, "notes from 36, full velocity scaling");
    expect(padDataGet(s.data, "N1") == "Kick" && padDataGet(s.data, "N2") == "Snare", "pads named after the instruments");
    expect(padSamplePath(s.data, 0, 0) == (kitDir / "soft.wav").u8string() && padSamplePath(s.data, 0, 1) == (kitDir / "loud.wav").u8string(),
           "layer samples in order");
    expect(padSamplePath(s.data, 2, 0) == "none" && padSamplePath(s.data, 20, 0) == "none", "unused pads are empty (no built-in sounds)");
    expect(padLayerCount(s.data, 0) == 2, "the muted component's layer is left out");
    expect(param(s, padL(0, 0, LayVelLow)) == 0 && param(s, padL(0, 0, LayVelHigh)) == 63 && param(s, padL(0, 1, LayVelLow)) == 64 &&
               param(s, padL(0, 1, LayVelHigh)) == 127,
           "velocity ranges split where the layers touch");
    expect(std::fabs(param(s, padL(0, 0, LayLevel)) - 0.8f) < 1e-5f && std::fabs(param(s, padL(0, 1, LayLevel)) - 0.4f) < 1e-5f, "layer level = component gain x layer gain");
    expect(param(s, padL(0, 1, LayTune)) == 1.5f && param(s, padP(0, PadTune)) == -2.0f && std::fabs(param(s, padP(0, PadPan)) + 0.25f) < 1e-6f,
           "layer tune, pad tune, pan");
    expect(std::fabs(param(s, padP(0, PadLevel)) - 0.9f) < 1e-6f, "pad level = volume x gain");
    expect(std::fabs(param(s, padE(0, PadAttack)) - 0.01f) < 1e-5f && std::fabs(param(s, padE(0, PadDecay)) - 0.1f) < 1e-5f &&
               param(s, padE(0, PadSustain)) == 0.5f && std::fabs(param(s, padE(0, PadRelease)) - 0.05f) < 1e-5f && param(s, padE(0, PadHold)) == 0,
           "envelope in seconds");
    expect(param(s, padE(0, PadOneShot)) == 0 && param(s, padE(1, PadOneShot)) == 1, "stop note: one shot off");
    expect(param(s, padE(1, PadLayerMode)) == LayerCycle && param(s, padE(0, PadLayerMode)) == LayerStack, "round robin: Cycle, velocity: Stack");
    expect(param(s, padE(1, PadVelScale)) == 0 && param(s, padE(0, PadVelScale)) == 1, "velocity on and off");
    expect(param(s, padP(1, PadCut)) == 1 && param(s, padE(1, PadCutBy)) == 1 && param(s, padP(0, PadCut)) == 0, "mute group: a cut group");
    expect(param(s, padP(1, PadLevel)) == 1.0f, "gain above full set to full");
    auto hasNote = [&p](const char* start) {
      for (const auto& n : p.notes)
        if (std::strncmp(n.text, start, std::strlen(start)) == 0) return true;
      return false;
    };
    expect(hasNote("Levels above full") && hasNote("Their filter") && hasNote("Their random pitch") && !hasNote("Samples that are not there"),
           "report: clipped level, filter, random pitch; nothing missing");
    expect(h2::creditLines(k).size() == 3, "credits: author, licence, picture licence");
    {
      PluginState n = s;
      normalizePluginState(n);
      expect(n == s, "the state normalises as it is");
    }

    // Sound: soft hits play the positive sample, loud ones the negative one; levels follow velocity.
    {
      Player pl(s);
      pl.hit(36, 0.3f);
      auto a = pl.run(4096);
      Player p2(s);
      p2.hit(36, 0.9f);
      auto b = p2.run(4096);
      const double ma = mean(a, 2000, 4000), mb = mean(b, 2000, 4000);
      expect(ma > 0.01 && mb < -0.01, "sound: velocity 0.3 plays the soft layer, 0.9 the loud one");
      double fin = 0;
      for (float v : a) fin += std::isfinite(v) ? 0 : 1;
      expect(fin == 0, "sound: finite");
      expect(std::fabs(a[0]) < std::fabs(a[300]), "sound: the attack fades in");
    }
    // Round robin on the snare: hits alternate between its two layers.
    {
      Player pl(s);
      std::vector<double> signs;
      for (int i = 0; i < 4; ++i) {
        pl.hit(37, 1.0f);
        auto v = pl.run(2048);
        signs.push_back(mean(v, 1500, 2000));
      }
      // The snare is in a cut group: each hit cuts the previous one, so each block holds one layer.
      expect(signs[0] * signs[1] < 0 && signs[1] * signs[2] < 0 && signs[2] * signs[3] < 0, "round robin: hits alternate between the layers");
    }
  }
  // ---------------------------------------------------------------- mute groups choke
  {
    const fs::path d = root / "Choke";
    writeText(d / "drumkit.xml", kOneKit);
    writeFile(d / "a.wav", wavBytes(44100, 0.6f));
    writeFile(d / "b.wav", wavBytes(44100, 0.6f));
    writeFile(d / "c.wav", wavBytes(44100, -0.6f));
    h2::Kit k;
    std::string err;
    expect(h2::readKit(d.u8string(), k, &err), "choke kit read");
    PluginState s = h2::kitToPads(k).channels[0];
    expect(param(s, padP(0, PadCut)) == 1 && param(s, padP(1, PadCut)) == 1, "both hats in one cut group");
    expect(param(s, padE(1, PadMute)) == 1, "muted instrument: muted pad");
    // Two components stack: the closed hat plays a.wav and b.wav at once.
    expect(padLayerCount(s.data, 0) == 2 && std::fabs(param(s, padL(0, 0, LayLevel)) - 0.5f) < 1e-6f && std::fabs(param(s, padL(0, 1, LayLevel)) - 0.5f) < 1e-6f,
           "components: both layers, the kit's component volume applied");
    expect(param(s, padE(0, PadLayerMode)) == LayerRandom, "random: Random");
    s.params[static_cast<size_t>(padE(1, PadMute))] = 0;  // unmute the open hat for the choke
    s.params[static_cast<size_t>(padE(0, PadLayerMode))] = LayerStack;
    Player pl(s);
    pl.hit(37, 1.0f);  // open hat (negative)
    auto a = pl.run(4096);
    pl.hit(36, 1.0f);  // closed hat cuts it
    auto b = pl.run(4096);
    expect(mean(a, 3000, 4000) < -0.05, "choke: the open hat rings");
    expect(mean(b, 1000, 4000) > 0.05, "choke: the closed hat cuts the open one (only the positive samples are left)");
  }
  // ---------------------------------------------------------------- more than 32 instruments, missing samples
  {
    std::string xml = "<drumkit_info><name>Big</name><author>x</author><license>CC0</license><instrumentList>";
    for (int i = 0; i < 40; ++i)
      xml += "<instrument><id>" + std::to_string(i) + "</id><name>I" + std::to_string(i) + "</name><muteGroup>" + std::to_string(i % 10) +
             "</muteGroup><instrumentComponent><layer><filename>missing" + std::to_string(i) +
             ".wav</filename><min>0</min><max>1</max><gain>1</gain><pitch>0</pitch></layer></instrumentComponent></instrument>";
    xml += "</instrumentList></drumkit_info>";
    h2::Kit k;
    std::string err;
    expect(h2::parseDrumkitXml(xml, k, &err), "big kit parses");
    k.folder = (root / "nowhere").u8string();
    const h2::PadsImport p = h2::kitToPads(k);
    expect(p.channels.size() == 2 && p.names[1] == "Big 2", "40 instruments: two channels");
    expect(param(p.channels[1], PadsBaseNote) == 68.0f && padDataGet(p.channels[1].data, "N1") == "I32", "second channel continues at note 68");
    expect(p.missing.size() == 40, "missing samples listed");
    bool groupsNote = false, missingNote = false;
    for (const auto& n : p.notes) groupsNote |= std::string(n.text).find("eight mute groups") != std::string::npos, missingNote |= std::string(n.text).find("not there") != std::string::npos;
    expect(groupsNote && missingNote, "report: missing samples, more than eight mute groups");
  }
  // ---------------------------------------------------------------- velocity ranges of three layers
  {
    h2::Kit k;
    std::string err;
    h2::parseDrumkitXml(kOldKit, k, &err);
    const PluginState s = h2::kitToPads(k).channels[0];
    const int lo1 = static_cast<int>(param(s, padL(0, 1, LayVelLow))), hi0 = static_cast<int>(param(s, padL(0, 0, LayVelHigh)));
    const int lo2 = static_cast<int>(param(s, padL(0, 2, LayVelLow))), hi1 = static_cast<int>(param(s, padL(0, 1, LayVelHigh)));
    expect(hi0 + 1 == lo1 && hi1 + 1 == lo2 && param(s, padL(0, 2, LayVelHigh)) == 127 && param(s, padL(0, 0, LayVelLow)) == 0, "three layers: no gap, no overlap");
    expect(hi0 == 38 && hi1 == 88, "three layers: 0.3 and 0.7 of 127");
  }
  // ---------------------------------------------------------------- packed kits
  {
    std::vector<uint8_t> soft = wavBytes(100, 0.5f);
    std::vector<TarFile> files = {{"Test Kit/", {}},
                                  {"Test Kit/drumkit.xml", std::vector<uint8_t>(kCurrentKit, kCurrentKit + std::strlen(kCurrentKit))},
                                  {"Test Kit/soft.wav", soft},
                                  {"Test Kit/loud.wav", wavBytes(100, -0.5f)}};
    const std::vector<uint8_t> tar = makeTar(files), gz = gzipStored(tar);
    std::vector<uint8_t> back;
    std::string err;
    expect(h2::gunzip(gz, back, &err) && back == tar, "gzip: stored blocks read back");
    std::vector<uint8_t> broken = gz;
    broken[broken.size() - 12] ^= 0x55;
    expect(!h2::gunzip(broken, back, &err), "gzip: a damaged archive fails its checksum");
    std::vector<h2::TarEntry> entries;
    expect(h2::listTar(tar, entries, &err) && entries.size() == 4 && entries[0].dir && entries[2].name == "Test Kit/soft.wav" && entries[2].size == soft.size(),
           "tar: entries listed");
    const fs::path out = root / "unpacked";
    std::vector<std::string> written;
    expect(h2::unpackKit(gz, out.u8string(), written, &err) && written.size() == 3, "unpack: three files");
    h2::Kit k;
    expect(h2::readKit(out.u8string(), k, &err) && k.instruments.size() == 2, "unpack: the kit reads from the folder inside");
    written.clear();
    expect(h2::unpackKit(tar, (root / "plain").u8string(), written, &err) && written.size() == 3, "unpack: a plain tar too");
    // Long names three ways.
    const std::string longName = "Test Kit/" + std::string(120, 'x') + "/sample.wav";
    for (int mode = 0; mode < 3; ++mode) {
      const auto t = makeTar({{longName, soft}}, mode == 1, mode == 2);
      expect(h2::listTar(t, entries, &err) && entries.size() == 1 && entries[0].name == longName,
             std::string("tar: long names (") + (mode == 0 ? "ustar prefix" : mode == 1 ? "GNU" : "pax") + ")");
    }
    // Names leaving the folder refuse the whole archive.
    for (const char* evil : {"../evil.wav", "Kit/../../evil.wav", "/etc/evil.wav"}) {
      written.clear();
      const fs::path dest = root / "evil";
      expect(!h2::unpackKit(makeTar({{"Kit/ok.wav", soft}, {evil, soft}}), dest.u8string(), written, &err) && !fs::exists(dest / "Kit" / "ok.wav"),
             std::string("unpack refuses ") + evil);
    }
    std::vector<uint8_t> cut(tar.begin(), tar.begin() + 1100);
    expect(!h2::listTar(cut, entries, &err), "tar: cut short refused");
    std::vector<uint8_t> junk(2048, 0x41);
    expect(!h2::listTar(junk, entries, &err), "tar: not an archive refused");
    // The cache copy, as the app makes it.
    const fs::path file = root / "Test Kit.h2drumkit";
    writeFile(file, gz);
    archive::MakeRequest mr;
    mr.sourcePath = file.u8string();
    mr.cacheRoot = (root / "cache").u8string();
    mr.size = gz.size();
    int writes = 0;
    mr.write = [&](const std::string& folder, std::vector<std::string>& names, std::string* e) {
      ++writes;
      return h2::unpackKit(gz, folder, names, e);
    };
    archive::UnpackResult u1, u2;
    expect(archive::makeCopy(mr, u1, &err) && !u1.reused && h2::readKit(u1.folder, k, &err), "cache: the kit unpacks into the cache and reads");
    expect(archive::makeCopy(mr, u2, &err) && u2.reused && writes == 1, "cache: an unchanged kit finds its copy");
    expect(h2::isKitArchive(file.u8string()) && h2::isKitArchive("/a/B.H2DRUMKIT") && !h2::isKitArchive("/a/b.tar.gz"), "archive by name");
  }
  // ---------------------------------------------------------------- browser and project
  {
    using browse::DropAction;
    using browse::DropTarget;
    using browse::Kind;
    expect(browse::kindOf("/k/Rock.h2drumkit") == Kind::DrumKit && browse::kindOf("/k/Rock/drumkit.xml") == Kind::DrumKit &&
               browse::kindOf("/k/Rock/other.xml") == Kind::Other,
           "browser: kit files");
    Kind kk;
    expect(browse::kindFromName("kit", kk) && kk == Kind::DrumKit && browse::listable(Kind::DrumKit), "browser: search filter and listing");
    browse::DropItem it;
    it.kind = Kind::DrumKit;
    expect(browse::dropAction(it, DropTarget::Rack) == DropAction::LoadDrumKit && browse::dropAction(it, DropTarget::Workspace) == DropAction::LoadDrumKit &&
               browse::dropAction(it, DropTarget::PluginWindow, "gl.pads") == DropAction::LoadDrumKit &&
               browse::dropAction(it, DropTarget::PluginWindow, "gl.sampler") == DropAction::None &&
               browse::dropAction(it, DropTarget::MixerTrack) == DropAction::None,
           "browser: drop actions");
    // Every sample of the kit is a file reference of the project (save, collect, relink).
    h2::Kit k;
    std::string err;
    h2::readKit(kitDir.u8string(), k, &err);
    PluginState s = h2::kitToPads(k).channels[0];
    std::vector<std::string> refs;
    forEachStateFileRef(s, [&](std::string& p) { refs.push_back(p); });
    auto same = [](const std::string& a, const fs::path& b) {
      std::error_code e2;
      return fs::equivalent(fs::u8path(a), b, e2);
    };
    const auto soft = std::count_if(refs.begin(), refs.end(), [&](const std::string& r) { return same(r, kitDir / "soft.wav"); });
    const auto loud = std::count_if(refs.begin(), refs.end(), [&](const std::string& r) { return same(r, kitDir / "loud.wav"); });
    expect(refs.size() == 4 && soft == 2 && loud == 2, "project: every layer sample is a file reference (" + std::to_string(refs.size()) + ")");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  return finish();
}
