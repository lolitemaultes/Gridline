// Gridline — plugin suites (J-131): membership and plugin identities.
#include "PluginSuite.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>

namespace gl::suite {

namespace {

// ---- suites: one line each (the bundle: cmake/GridlinePlugins.cmake gridline_suite()) ----
const std::vector<Suite> kSuites = {
    {"gridline", "Gridline Suite", "org.gridline.GridlineSuite"},
    {"mini", "Gridline Suite Mini", "org.gridline.GridlineSuiteMini"},
};

// ---- members of other suites than "gridline": one line per plugin ----
struct Assignment {
  const char* type;
  const char* suite;
};
const Assignment kAssignments[] = {
    // J-132: Gridline Suite Mini
    {"gl.minitrim", "mini"},
    {"gl.minibreadth", "mini"},
    {"gl.miniskew", "mini"},
    {"gl.miniweave", "mini"},
    {"gl.minibands", "mini"},
    {"gl.minisieve", "mini"},
    {"gl.minirungs", "mini"},
    {"gl.miniscorch", "mini"},
    {"gl.minivowel", "mini"},
    {"gl.minitines", "mini"},
    {"gl.minichime", "mini"},
    {"gl.minipress", "mini"},
    {"gl.miniceiling", "mini"},
    {"gl.minishear", "mini"},
    {"gl.minipack", "mini"},
    {"gl.minispan", "mini"},
    {"gl.minilatch", "mini"},
    {"gl.minipunch", "mini"},
    {"gl.miniburn", "mini"},
    {"gl.minibend", "mini"},
    {"gl.miniwarp", "mini"},
    {"gl.minicrumble", "mini"},
    {"gl.miniclang", "mini"},
    {"gl.minislide", "mini"},
    {"gl.miniinterval", "mini"},
    {"gl.minidouble", "mini"},
    {"gl.minicrowd", "mini"},
    {"gl.minijet", "mini"},
    {"gl.miniorbit", "mini"},
    {"gl.miniecho", "mini"},
    {"gl.minivolley", "mini"},
    {"gl.minivault", "mini"},
    {"gl.minirewind", "mini"},
    {"gl.minihalt", "mini"},
    {"gl.minipulse", "mini"},
    {nullptr, nullptr},
};

// ---- built-ins that only make sense inside Gridline ----
struct Exclusion {
  const char* type;
  const char* reason;
};
const Exclusion kExcluded[] = {
    {"gl.midiout", "sends MIDI to other Gridline channels"},
    {"gl.send", "routes audio to another Gridline mixer track"},
    {"gl.bussend", "routes audio to a Gridline audio bus"},
    {"gl.busreturn", "returns audio from a Gridline audio bus"},
    {"gl.splitter", "splits bands to other Gridline mixer tracks"},
    {"gl.flex", "stand-in for a third-party preset format in imported projects"},
};

// Effects with a sidechain input: the DSP reads the slot's sidechain buffer
// (SideIo::scL/scR: Compressor, Limiter, Vocoder, Ducker, Hall, Tideline) or
// has a side input route (PluginInfo::sideInParam). tests/test_suite.cpp
// checks the list against sideInParam.
const char* const kSidechain[] = {
    "gl.comp", "gl.limiter", "gl.vocoder", "gl.scduck", "gl.hall", "gl.tideline", "gl.transplit", "gl.hitloop",
    "gl.minipress", "gl.minilatch", "gl.miniclang",
};

// AU subtype overrides for a (future) hash collision: the type that came
// later gets a fixed code here; existing codes never change.
struct AuOverride {
  const char* type;
  const char* subtype;
};
const AuOverride kAuOverrides[] = {
    {nullptr, nullptr},
};

uint32_t code(const char* s) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}

// Host categories (VST3 sub-categories, CLAP features, AU tag) from the
// catalogue category, with a few types placed more precisely.
void categorize(Member& m) {
  struct Cat {
    const char* vst3;
    const char* clap;
    const char* au;
  };
  static const std::map<std::string, Cat> kByType = {
      {"gl.eq", {"Fx|EQ", "equalizer", "EQ"}},
      {"gl.geq", {"Fx|EQ", "equalizer", "EQ"}},
      {"gl.drawneq", {"Fx|EQ", "equalizer", "EQ"}},
      {"gl.bankeq", {"Fx|EQ", "equalizer", "EQ"}},
      {"gl.delay", {"Fx|Delay", "delay", "Delay"}},
      {"gl.multidelay", {"Fx|Delay", "delay", "Delay"}},
      {"gl.banddelay", {"Fx|Delay", "delay", "Delay"}},
      {"gl.hitloop", {"Fx|Delay", "glitch", "Delay"}},
      {"gl.limiter", {"Fx|Dynamics", "limiter", "Dynamics"}},
      {"gl.maximizer", {"Fx|Mastering", "limiter", "Dynamics"}},
      {"gl.master", {"Fx|Mastering", "mastering", "Dynamics"}},
      {"gl.gate", {"Fx|Dynamics", "gate", "Dynamics"}},
      {"gl.timegate", {"Fx|Modulation", "glitch", "Modulation"}},
      {"gl.pitch", {"Fx|Pitch Shift", "pitch-shifter", "Pitch"}},
      {"gl.pitcher", {"Fx|Pitch Shift", "pitch-correction", "Pitch"}},
      {"gl.vocoder", {"Fx|Modulation", "vocoder", "Modulation"}},
      {"gl.spread", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.stereo", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.stereomatrix", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.scratcher", {"Fx", "glitch", "Effects"}},
      {"gl.analyzer", {"Fx|Analyzer", "analyzer", "Analyzer"}},
      {"gl.tuner", {"Fx|Analyzer", "analyzer", "Analyzer"}},
      {"gl.freqshift", {"Fx|Modulation", "frequency-shifter", "Modulation"}},
      // J-132: Gridline Suite Mini (Trim keeps the utility default)
      {"gl.minibreadth", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.miniskew", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.miniweave", {"Fx|Spatial", "utility", "Imaging"}},
      {"gl.minibands", {"Fx|EQ", "equalizer", "EQ"}},
      {"gl.minisieve", {"Fx|Filter", "filter", "Filter"}},
      {"gl.minirungs", {"Fx|Filter", "filter", "Filter"}},
      {"gl.miniscorch", {"Fx|Filter", "filter", "Filter"}},
      {"gl.minivowel", {"Fx|Filter", "filter", "Filter"}},
      {"gl.minitines", {"Fx|Filter", "filter", "Filter"}},
      {"gl.minichime", {"Fx|Filter", "filter", "Filter"}},
      {"gl.minipress", {"Fx|Dynamics", "compressor", "Dynamics"}},
      {"gl.miniceiling", {"Fx|Dynamics", "limiter", "Dynamics"}},
      {"gl.minishear", {"Fx|Distortion", "distortion", "Distortion"}},
      {"gl.minipack", {"Fx|Dynamics", "compressor", "Dynamics"}},
      {"gl.minispan", {"Fx|Dynamics", "compressor", "Dynamics"}},
      {"gl.minilatch", {"Fx|Dynamics", "gate", "Dynamics"}},
      {"gl.minipunch", {"Fx|Dynamics", "transient-shaper", "Dynamics"}},
      {"gl.miniburn", {"Fx|Distortion", "distortion", "Distortion"}},
      {"gl.minibend", {"Fx|Distortion", "distortion", "Distortion"}},
      {"gl.miniwarp", {"Fx|Distortion", "distortion", "Distortion"}},
      {"gl.minicrumble", {"Fx|Distortion", "distortion", "Distortion"}},
      {"gl.miniclang", {"Fx|Modulation", "ring-modulator", "Modulation"}},
      {"gl.minislide", {"Fx|Modulation", "frequency-shifter", "Modulation"}},
      {"gl.miniinterval", {"Fx|Pitch Shift", "pitch-shifter", "Pitch"}},
      {"gl.minidouble", {"Fx|Modulation", "chorus", "Modulation"}},
      {"gl.minicrowd", {"Fx|Modulation", "chorus", "Modulation"}},
      {"gl.minijet", {"Fx|Modulation", "flanger", "Modulation"}},
      {"gl.miniorbit", {"Fx|Modulation", "phaser", "Modulation"}},
      {"gl.miniecho", {"Fx|Delay", "delay", "Delay"}},
      {"gl.minivolley", {"Fx|Delay", "delay", "Delay"}},
      {"gl.minivault", {"Fx|Reverb", "reverb", "Reverb"}},
      {"gl.minirewind", {"Fx|Delay", "glitch", "Delay"}},
      {"gl.minihalt", {"Fx", "glitch", "Effects"}},
      {"gl.minipulse", {"Fx|Modulation", "glitch", "Modulation"}},
  };
  static const std::map<std::string, Cat> kByCategory = {
      {"Drums", {"Instrument|Drum", "drum", "Drums"}},
      {"Sampler", {"Instrument|Sampler", "sampler", "Sampler"}},
      {"Synth", {"Instrument|Synth", "synthesizer", "Synthesizer"}},
      {"Dynamics", {"Fx|Dynamics", "compressor", "Dynamics"}},
      {"Filter", {"Fx|Filter", "filter", "Filter"}},
      {"Delay & reverb", {"Fx|Reverb", "reverb", "Reverb"}},
      {"Modulation", {"Fx|Modulation", "chorus", "Modulation"}},
      {"Distortion", {"Fx|Distortion", "distortion", "Distortion"}},
      {"Spectral", {"Fx|Modulation", "audio-effect", "Effects"}},
      {"Visual", {"Fx|Analyzer", "analyzer", "Analyzer"}},
      {"Multi FX", {"Fx", "multi-effects", "Effects"}},
  };
  Cat c = m.instrument ? Cat{"Instrument|Synth", "synthesizer", "Synthesizer"} : Cat{"Fx|Tools", "utility", "Utility"};
  if (auto it = kByCategory.find(m.info->category); it != kByCategory.end()) c = it->second;
  if (auto it = kByType.find(m.type); it != kByType.end()) c = it->second;
  m.vst3Category = c.vst3;
  m.clapFeatures = {m.instrument ? "instrument" : "audio-effect"};
  if (std::string(c.clap) != m.clapFeatures[0]) m.clapFeatures.push_back(c.clap);
  m.clapFeatures.push_back("stereo");
  m.auTag = c.au;
}

struct Tables {
  std::map<std::string, std::vector<Member>> bySuite;
  std::map<std::string, std::pair<std::string, size_t>> byType;  // type -> (suite, index)
};

const Tables& tables() {
  static Tables t;
  static std::once_flag once;
  std::call_once(once, [] {
    for (const Suite& s : kSuites) t.bySuite[s.id];
    for (const PluginInfo& info : pluginCatalog()) {
      if (!exclusionReason(info).empty()) continue;
      Member m;
      m.info = &info;
      m.type = info.type;
      m.name = info.name;
      m.suite = "gridline";
      for (const Assignment& a : kAssignments)
        if (a.type && info.type == a.type) m.suite = a.suite;
      if (!findSuite(m.suite)) continue;
      m.instrument = info.kind == PluginKind::Generator;
      m.midiInput = !m.instrument && info.midiInput;
      for (const char* s : kSidechain)
        if (info.type == s) m.sidechain = !m.instrument;
      const std::string stem = info.type.rfind("gl.", 0) == 0 ? info.type.substr(3) : info.type;
      m.clapId = "org.gridline.suite." + stem;
      const uint32_t h = fnv1a32(info.type);
      m.vst3Uid[0] = kVst3Uid0;
      m.vst3Uid[1] = kVst3Uid1;
      m.vst3Uid[2] = kVst3Uid2;
      m.vst3Uid[3] = h;
      m.auType = m.instrument ? code("aumu") : m.midiInput ? code("aumf") : code("aufx");
      static const char kAlnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
      char sub[5] = {static_cast<char>('a' + h % 26), kAlnum[(h / 26) % 36], kAlnum[(h / (26 * 36)) % 36], kAlnum[(h / (26 * 36 * 36)) % 36], 0};
      m.auSubtype = code(sub);
      for (const AuOverride& o : kAuOverrides)
        if (o.type && info.type == o.type) m.auSubtype = code(o.subtype);
      categorize(m);
      auto& list = t.bySuite[m.suite];
      t.byType[m.type] = {m.suite, list.size()};
      list.push_back(std::move(m));
    }
  });
  return t;
}

}  // namespace

const std::vector<Suite>& suites() { return kSuites; }

const Suite* findSuite(const std::string& id) {
  for (const Suite& s : kSuites)
    if (id == s.id) return &s;
  return nullptr;
}

const std::vector<Member>& members(const std::string& suiteId) {
  static const std::vector<Member> none;
  const auto& t = tables();
  auto it = t.bySuite.find(suiteId);
  return it == t.bySuite.end() ? none : it->second;
}

const Member* findMember(const std::string& type) {
  const auto& t = tables();
  auto it = t.byType.find(type);
  if (it == t.byType.end()) return nullptr;
  return &t.bySuite.at(it->second.first)[it->second.second];
}

const Member* byClapId(const std::string& id) {
  for (const auto& [suite, list] : tables().bySuite)
    for (const Member& m : list)
      if (m.clapId == id) return &m;
  return nullptr;
}

const Member* byVst3Uid(const uint32_t uid[4]) {
  for (const auto& [suite, list] : tables().bySuite)
    for (const Member& m : list)
      if (std::equal(uid, uid + 4, m.vst3Uid)) return &m;
  return nullptr;
}

const Member* byAu(uint32_t type, uint32_t subtype) {
  for (const auto& [suite, list] : tables().bySuite)
    for (const Member& m : list)
      if (m.auType == type && m.auSubtype == subtype) return &m;
  return nullptr;
}

std::string exclusionReason(const PluginInfo& info) {
  if (info.kind == PluginKind::NoteFx) return "note processor: runs on a Gridline channel's notes";
  if (!info.outputs.empty() || info.category == "Controller") return "controller: drives other Gridline parameters";
  if (isHostedType(info.type) || info.type.rfind("gl.", 0) != 0) return "not a Gridline plugin";
  for (const Exclusion& e : kExcluded)
    if (info.type == e.type) return e.reason;
  return {};
}

uint32_t fnv1a32(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

std::string fourcc(uint32_t c) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[static_cast<size_t>(i)] = static_cast<char>((c >> (24 - 8 * i)) & 0xFF);
  return s;
}

std::string vst3UidText(const uint32_t uid[4]) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08X-%08X-%08X-%08X", uid[0], uid[1], uid[2], uid[3]);
  return buf;
}

namespace {
std::string xmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}
}  // namespace

std::string auComponentsPlist(const std::string& suiteId, uint32_t version, const std::string& factory) {
  std::string out = "<key>AudioComponents</key><array>\n";
  for (const Member& m : members(suiteId)) {
    out += "<dict><key>type</key><string>" + fourcc(m.auType) + "</string><key>subtype</key><string>" + fourcc(m.auSubtype) +
           "</string><key>manufacturer</key><string>GrLn</string><key>name</key><string>Gridline: " + xmlEscape(m.name) +
           "</string><key>description</key><string>" + xmlEscape(m.name) + "</string><key>version</key><integer>" +
           std::to_string(version) + "</integer><key>factoryFunction</key><string>" + factory +
           "</string><key>tags</key><array><string>" + m.auTag + "</string></array></dict>\n";
  }
  out += "</array>\n";
  return out;
}

}  // namespace gl::suite
