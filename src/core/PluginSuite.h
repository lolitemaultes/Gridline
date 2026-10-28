// Gridline — plugin suites (J-131): Gridline's own instruments and effects
// built as standalone plugins (VST3, CLAP and, on macOS, Audio Unit) for any
// host. A suite is one bundle per format ("Gridline Suite.vst3", ".clap",
// ".component") holding one plugin class per member.
//
// Membership: every built-in instrument and effect belongs to the suite
// "gridline" ("Gridline Suite") unless it is excluded (it only makes sense
// inside Gridline: routing, controllers of other Gridline parameters, note
// processors, stand-ins) or assigned to another suite. Adding a plugin to
// another suite is one line in kAssignments (PluginSuite.cpp); a new suite is
// one line in kSuites plus one gridline_suite(...) line in
// cmake/GridlinePlugins.cmake.
//
// Plugin identities never change: they come from the type alone ("gl.<name>"),
// never from the suite, so a type may move between suites and hosts still find
// it. Scheme (docs/gridline-suite.md):
//   CLAP id     org.gridline.suite.<name>
//   VST3 class  47524944 4C494E45 53554954 <FNV-1a 32 of the type>   ("GRIDLINESUIT" + hash)
//   AU          manufacturer GrLn; type aumu (instrument), aufx (effect), aumf
//               (effect that takes MIDI); subtype [a-z][a-z0-9]{3} from the
//               same hash (Gridline's own units are GrIn / GrFx: upper case).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "PluginInfo.h"

namespace gl::suite {

struct Suite {
  const char* id;      // "gridline"
  const char* name;    // "Gridline Suite": bundle file name and host folder name
  const char* bundleId;  // macOS bundle identifier stem ("org.gridline.GridlineSuite")
};

// Every suite (the bundles the build makes).
const std::vector<Suite>& suites();
const Suite* findSuite(const std::string& id);

struct Member {
  const PluginInfo* info = nullptr;  // catalogue entry (never freed)
  std::string type;                  // gl.<name>
  std::string name;                  // display name in hosts
  std::string suite;                 // suite id
  bool instrument = false;           // generator (notes in) / effect (audio in)
  bool sidechain = false;            // effect with a sidechain input bus
  bool midiInput = false;            // effect that also takes notes (PluginInfo::midiInput)
  std::string clapId;
  uint32_t vst3Uid[4] = {0, 0, 0, 0};
  uint32_t auType = 0, auSubtype = 0;  // four-character codes
  std::string vst3Category;            // VST3 sub-categories ("Fx|Dynamics")
  std::vector<std::string> clapFeatures;
  std::string auTag;                   // AudioComponents "tags" entry
};

inline constexpr uint32_t kAuManufacturer = 0x47724C6E;  // 'GrLn'
inline constexpr uint32_t kVst3Uid0 = 0x47524944, kVst3Uid1 = 0x4C494E45, kVst3Uid2 = 0x53554954;

// Members of a suite in catalogue order (empty for an unknown suite).
const std::vector<Member>& members(const std::string& suiteId);
// Same, for a literal id (the plugin entries' GRIDLINE_SUITE): no temporary string at the call site,
// so a reference to the result never looks like it is bound to one.
const std::vector<Member>& members(const char* suiteId);
// The member for a type in whatever suite holds it; nullptr when excluded / unknown.
const Member* findMember(const std::string& type);
// Lookups by plugin identity (nullptr when none).
const Member* byClapId(const std::string& id);
const Member* byVst3Uid(const uint32_t uid[4]);
const Member* byAu(uint32_t type, uint32_t subtype);
// Why a built-in plugin is not in any suite ("" when it is in one).
std::string exclusionReason(const PluginInfo& info);

uint32_t fnv1a32(const std::string& s);
std::string fourcc(uint32_t code);  // 'aumu' -> "aumu"
// "47524944-4C494E45-53554954-1A2B3C4D"
std::string vst3UidText(const uint32_t uid[4]);

// The suite's Audio Unit components as an Info.plist "AudioComponents" array
// (keys and values only, no plist header); version as 0xMMMMmmbb.
std::string auComponentsPlist(const std::string& suiteId, uint32_t version, const std::string& factory);

}  // namespace gl::suite
