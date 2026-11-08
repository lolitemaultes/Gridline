// Gridline: which installed plugin is the same plugin as a saved one, across formats (K-78).
//
// A project saved on one computer names each third-party plugin by its hosted type ("au:aufx/DLSF/TLDN",
// "vst3:<class id>", "vst2:<id>", "clap:<id>", "lv2:<uri>"). Another computer may have the same plugin
// only in another format. An Identity is what a project keeps of a plugin so it can be found again:
// the saved type, every other type the same plugin had where it was saved (aliases), its name, vendor,
// kind, main input / output channels and its parameter names.
//
// The matcher scores every registered class against an Identity:
//   - ids that are known to belong together: the Steinberg VST2-compatible VST3 class id (it holds the
//     VST2 id and the first letters of the name), the class ids a JUCE build derives from the same
//     manufacturer and plugin codes as its Audio Unit, the aliases recorded when the project was saved;
//   - the name and the vendor, normalised (case, punctuation, format tags such as "(VST3)", an Audio
//     Unit's "Vendor: Name");
//   - instrument vs effect must agree, a different vendor never matches, the main outputs and the
//     parameter names must not contradict it.
// The score gives a confidence. Only Medium and above load on their own (and only when one product is
// that good a match); Low ones are offered to the user, never loaded silently.
//
// Pure functions over HostedClass lists: tests/test_cross_format.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../core/Json.h"

namespace gl::host {

struct HostedClass;

namespace ident {

enum class Confidence { None = 0, Low = 1, Medium = 2, High = 3, Exact = 4 };
const char* confidenceName(Confidence c);  // "none", "low", "medium", "high", "exact"
Confidence confidenceOf(const std::string& name);

struct Identity {
  std::string type;  // the hosted type the plugin was saved as
  std::string name, vendor, version;
  int instrument = -1;            // 1 instrument, 0 effect, -1 not known
  int inputs = -1, outputs = -1;  // main bus channels (-1 not known)
  std::vector<std::string> aliases;     // other types of the same plugin where it was saved
  std::vector<std::string> paramNames;  // the saved type's parameters, in PluginState order
  std::vector<int> paramSteps;          // their discrete step counts (0 = continuous / toggle); may be empty
  bool empty() const { return type.empty() && name.empty(); }
};
// The identity of a registered class (aliases not filled in).
Identity identityOf(const HostedClass& c);
Json toJson(const Identity& id);
bool fromJson(const Json& j, Identity& out);

// ---- ids --------------------------------------------------------------------------------------
// Four-character code of an Audio Unit key part ("DLSF", or "0x444C5346" for codes that are not text).
bool parseFourCC(const std::string& s, uint32_t& out);
// Parts of "au:<type>/<subtype>/<manufacturer>" (with or without the "au:" prefix).
bool parseAuKey(const std::string& key, uint32_t& type, uint32_t& subtype, uint32_t& manufacturer);
// The Steinberg VST2-compatible VST3 class id of a VST2 plugin: "VST" ("565354"), the VST2 id, then
// the first nine characters of the plugin name in lower case (zero padded), as 32 hex digits.
std::string vst2CompatibleVst3Uid(uint32_t vst2Id, const std::string& vst2Name);
// The VST2 id inside a VST2-compatible VST3 class id; false for other class ids.
bool vst2IdOfVst3Uid(const std::string& uid, uint32_t& vst2Id);
// The class id a JUCE build gives its VST3 component from its manufacturer and plugin codes (the Audio
// Unit's manufacturer and subtype). Also the reverse.
std::string juceVst3Uid(uint32_t manufacturer, uint32_t plugin);
bool juceCodesOfVst3Uid(const std::string& uid, uint32_t& manufacturer, uint32_t& plugin);

// How strongly two hosted types are known to be one plugin from their ids alone (0: nothing known,
// 100: certain). `nameA` / `nameB` are the plugins' names (the VST2-compatible id holds name letters).
int idRelation(const std::string& typeA, const std::string& nameA, const std::string& typeB, const std::string& nameB, std::string* why = nullptr);

// ---- the matcher ------------------------------------------------------------------------------
struct Candidate {
  std::shared_ptr<const HostedClass> cls;
  int score = 0;
  Confidence confidence = Confidence::None;
  int paramsMatched = 0, paramsTotal = 0;  // saved parameter names found in this class
  bool exactName = false;                  // the saved name names this very build ("Synth (CLAP)")
  std::vector<std::string> reasons;        // English, for the load report and the resolve window
};
// Every class that may be the saved plugin, best first: higher confidence, then a saved name with a
// format tag naming this very build, then the format order (`formatOrder`, most preferred first), then the score. The saved type itself is never a candidate.
// Classes whose type `skip` returns true for are left out (hidden or switched-off formats).
std::vector<Candidate> rank(const Identity& saved, const std::vector<std::shared_ptr<const HostedClass>>& classes,
                            const std::vector<std::string>& formatOrder, bool (*skip)(const HostedClass&) = nullptr);
// The candidate that may load on its own: the best one when it is Medium or better and no other
// product (another name / vendor) is as good. nullptr otherwise.
const Candidate* autoPick(const std::vector<Candidate>& ranked);

// Parameter correspondence: for each saved parameter (by name, the n-th of a repeated name matching
// the n-th) the index of the target class's parameter, or -1. Without saved names, by position when
// both lists are the same length.
struct ParamMap {
  std::vector<int> target;  // per saved parameter
  int matched = 0;
  bool byPosition = false;
};
ParamMap mapParams(const std::vector<std::string>& savedNames, size_t savedCount, const HostedClass& target);
// Name key used to match parameters: lower-case letters and digits.
std::string paramKey(const std::string& title);

}  // namespace ident
}  // namespace gl::host
