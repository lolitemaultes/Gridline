// Gridline - MCP tools: helpers shared by the tool files (McpTools.cpp, McpMore.cpp). Not an API.
#pragma once
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include "Mcp.h"
#include "Params.h"
#include "PluginInfo.h"

namespace gl::mcp::detail {

inline constexpr size_t kMaxNameLen = 256;
inline constexpr size_t kMaxChannels = 999, kMaxClips = 100000, kMaxAutomation = 10000, kMaxMarkers = 10000;

// Common argument descriptions.
inline constexpr const char* kChannelD = "Channel number in the channel rack (1 = first)";
inline constexpr const char* kPatternD = "Pattern number (1 = first)";
inline constexpr const char* kMixerD = "Mixer track: 0 = master, 1.. = inserts";
inline constexpr const char* kSlotD = "Effect slot of the mixer track (1..10)";
inline constexpr const char* kParamD =
    "Parameter: a name (\"volume\", \"pan\", \"mute\", \"pitch\" for channels; \"volume\", \"pan\", \"stereo\", \"mute\", "
    "\"eqLowGain\" ... for mixer tracks; \"mix\", \"enabled\" for effect slots; \"tempo\", \"swing\", \"masterPitch\" for the song), "
    "a plugin parameter number (\"1\" = first, see get_parameters) or a plugin parameter's name";

// ---- schema
Json prop(const char* type, const std::string& desc);
Json intP(const std::string& desc, double lo, double hi);
Json numP(const std::string& desc, double lo = NAN, double hi = NAN);
Json strP(const std::string& desc, size_t maxLen = kMaxNameLen);
Json boolP(const std::string& desc);
Json enumP(const std::string& desc, std::vector<std::string> values);
Json intEnumP(const std::string& desc, std::vector<int> values);
Json arrP(const std::string& desc, Json items, size_t maxItems);
Json objSchema(std::vector<std::pair<std::string, Json>> props, std::vector<std::string> required = {});

// ---- arguments
bool has(const Json& a, const char* k);
long long argInt(const Json& a, const char* k, long long d = 0);
double argNum(const Json& a, const char* k, double d = 0);
std::string argStr(const Json& a, const char* k, const std::string& d = "");
bool argBool(const Json& a, const char* k, bool d = false);
ToolResult fail(const std::string& msg);
ToolResult ok(Json data, std::string summary);

// ---- conversions
Tick toTicks(const Song& s, double beats);
double toBeats(const Song& s, Tick t);
double round4(double v);
Json beatsJ(const Song& s, Tick t);
std::string hexColor(Rgb c);
bool parseColor(std::string s, Rgb& out);
std::string key(std::string s);  // case / separator-insensitive name
std::string clipName(std::string s);
const char* kindName(PluginKind k);
const char* clipKindName(ClipKind k);
std::string pluginName(const std::string& type);

// ---- lookups (numbers as the tools take them)
const Channel* channelAt(const Song& s, long long n, std::string& err);
const Pattern* patternAt(const Song& s, long long n, std::string& err);
const MixerTrack* mixerAt(const Song& s, long long n, std::string& err);
int channelNumber(const Song& s, ObjectId id);
int patternNumber(const Song& s, ObjectId id);
const PluginInfo* pluginOfKind(const std::string& type, PluginKind kind, std::string& err);
bool presetByName(const Environment& env, const std::string& type, const std::string& name, PluginState& out, std::string& err);

// ---- parameters
struct ParamTarget {
  enum Kind { Song, Channel, Mixer, Slot } kind = Song;
  ObjectId owner = kNoId;
  int slot = -1;
};
bool targetOf(const Song& s, const Json& a, ParamTarget& t, std::string& err);
const std::vector<std::pair<const char*, MixerParam>>& mixerParamNames();
std::vector<std::pair<std::string, ParamId>> paramsOf(const Song& s, const ParamTarget& t);
bool resolveParam(const Song& s, const Json& a, ParamId& id, std::string& err);
Json paramJson(const Song& s, const std::string& apiName, ParamId id);

// ---- read-outs
Json channelJson(const Song& s, size_t i);
Json patternJson(const Song& s, size_t i);
bool mixerUsed(const Song& s, size_t i);
Json mixerJson(const Song& s, size_t i);
Json markerJson(const Song& s, size_t i);
double songSeconds(const Song& s);
Json noteJson(const Song& s, const Note& n);

// ---- the tools of McpMore.cpp (generic object access, batches, discovery, the app's commands)
void addMoreTools(std::vector<Tool>& v);

}  // namespace gl::mcp::detail
