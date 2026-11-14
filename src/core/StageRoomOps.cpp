// Gridline - Stage rooms. See StageRoomOps.h.
#include "StageRoomOps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include "I18n.h"
#include "Json.h"
#include "MixerOps.h"
#include "Params.h"
#include "PluginParams.h"
#include "Presets.h"
#include "Project.h"
#include "StageRooms.h"

namespace gl::roomops {

namespace fs = std::filesystem;

namespace {
constexpr Rgb kPalette[] = {0x73A6D8, 0xE07FA2, 0x6DBBA6, 0xE5B062, 0xA889E6, 0xE09466, 0x8FA0B2, 0xD9C45E};
constexpr const char* kTemplateFormat = "gridline-stage-room";
constexpr int kTemplateVersion = 1;

bool sendsInto(const MixerTrack& m, ObjectId to) {
  for (const auto& sd : m.sends)
    if (sd.to == to) return true;
  return false;
}
}  // namespace

// ---------------------------------------------------------------- reading ----
int roomCount(const Song& s) { return 1 + static_cast<int>(s.stageRooms.size()); }

const StageSettings& roomAt(const Song& s, int i) {
  if (i <= 0 || s.stageRooms.empty()) return s.stage;
  return s.stageRooms[static_cast<size_t>(std::min(i - 1, static_cast<int>(s.stageRooms.size()) - 1))];
}

StageSettings* roomAtMut(Song& s, int i) {
  if (i == 0) return &s.stage;
  if (i < 0 || i > static_cast<int>(s.stageRooms.size())) return nullptr;
  return &s.stageRooms[static_cast<size_t>(i - 1)];
}

ObjectId roomIdAt(const Song& s, int i) { return roomAt(s, i).id; }

int roomIndex(const Song& s, ObjectId id) {
  if (id == kNoId || id == s.stage.id) return 0;
  for (size_t k = 0; k < s.stageRooms.size(); ++k)
    if (s.stageRooms[k].id == id) return static_cast<int>(k) + 1;
  return -1;
}

int trackRoom(const Song& s, const MixerTrack& m) { return std::max(0, roomIndex(s, m.room)); }

int trackRoom(const Song& s, ObjectId track) {
  const MixerTrack* m = s.mixerTrack(track);
  return m ? trackRoom(s, *m) : 0;
}

int spaceRoom(const Song& s, ObjectId track) {
  if (track == kNoId) return -1;
  for (int i = 0; i < roomCount(s); ++i)
    if (roomAt(s, i).room == track) return i;
  return -1;
}

bool isSpaceTrack(const Song& s, ObjectId track) { return spaceRoom(s, track) >= 0; }

std::string roomName(const Song& s, int i) {
  const StageSettings& st = roomAt(s, i);
  if (!st.name.empty()) return st.name;
  char b[32];
  std::snprintf(b, sizeof b, i18n::tr("Room %d"), std::clamp(i, 0, roomCount(s) - 1) + 1);
  return b;
}

Rgb roomColor(const Song& s, int i) {
  const StageSettings& st = roomAt(s, i);
  if (st.color != 0) return st.color;
  return kPalette[static_cast<size_t>(std::max(0, i)) % (sizeof kPalette / sizeof kPalette[0])];
}

namespace {
// A room's space track name: one pattern, so the word order is the language's.
std::string spaceNameOf(const std::string& room) {
  std::vector<char> b(room.size() + 64);
  std::snprintf(b.data(), b.size(), i18n::tr("%s space"), room.c_str());
  return b.data();
}
}  // namespace

std::string spaceTrackName(const Song& s, int i) {
  if (roomCount(s) <= 1) return "Room";
  return spaceNameOf(roomName(s, i));
}

std::vector<ObjectId> roomTracks(const Song& s, int i) {
  std::vector<ObjectId> out;
  for (size_t k = 1; k < s.mixer.size(); ++k) {
    const MixerTrack& m = s.mixer[k];
    if (trackRoom(s, m) == i && !isSpaceTrack(s, m.id)) out.push_back(m.id);
  }
  return out;
}

bool trackInUse(const Song& s, const MixerTrack& m) {
  if (m.id == s.masterId() || isSpaceTrack(s, m.id)) return false;
  if (m.input >= 0 || !m.name.empty() || m.depth > 0 || m.pan != 0 || m.focus != 0) return true;
  for (const auto& sl : m.slots)
    if (!sl.plugin.type.empty()) return true;
  for (const auto& sd : m.sends)
    if (sd.to != s.masterId() && !sd.stageRoom) return true;
  for (const auto& c : s.channels)
    if (c.mixerTrack == m.id) return true;
  return false;
}

std::vector<ObjectId> roomUsedTracks(const Song& s, int i) {
  std::vector<ObjectId> out;
  for (size_t k = 1; k < s.mixer.size(); ++k)
    if (trackRoom(s, s.mixer[k]) == i && trackInUse(s, s.mixer[k])) out.push_back(s.mixer[k].id);
  return out;
}

DeleteBlock deleteBlock(const Song& s, int i, size_t* tracks) {
  if (tracks) *tracks = 0;
  const int n = roomCount(s);
  if (i < 0 || i >= n) return DeleteBlock::NoSuchRoom;
  if (n <= 1) return DeleteBlock::LastRoom;
  const size_t used = roomUsedTracks(s, i).size();
  if (tracks) *tracks = used;
  return used > 0 ? DeleteBlock::HasTracks : DeleteBlock::None;
}

std::vector<ObjectId> hearOnlyKeep(const Song& s, const std::vector<ObjectId>& rooms) {
  std::set<ObjectId> keep;
  std::vector<ObjectId> todo;
  std::vector<int> idx;
  for (ObjectId r : rooms)
    if (const int k = roomIndex(s, r); k >= 0) idx.push_back(k);
  if (idx.empty()) return {};
  for (const auto& m : s.mixer)
    if (m.id != s.masterId() && std::find(idx.begin(), idx.end(), trackRoom(s, m)) != idx.end()) keep.insert(m.id), todo.push_back(m.id);
  while (!todo.empty()) {  // and everything downstream: buses, their spaces, the master
    const MixerTrack* m = s.mixerTrack(todo.back());
    todo.pop_back();
    if (!m) continue;
    for (const auto& sd : m->sends)
      if (!sd.sidechain && keep.insert(sd.to).second) todo.push_back(sd.to);
  }
  keep.insert(s.masterId());
  return {keep.begin(), keep.end()};
}

bool hearOnly(Song& s, const std::vector<ObjectId>& rooms) {
  if (roomCount(s) < 2) return false;
  const std::vector<ObjectId> keep = hearOnlyKeep(s, rooms);
  if (keep.empty()) return false;
  for (auto& m : s.mixer)
    if (!std::binary_search(keep.begin(), keep.end(), m.id)) m.muted = true;
  return true;
}

std::vector<ObjectId> feedersOf(const Song& s, const std::vector<ObjectId>& tracks) {
  std::set<ObjectId> have(tracks.begin(), tracks.end()), found;
  std::vector<ObjectId> todo = tracks;
  while (!todo.empty()) {
    const ObjectId t = todo.back();
    todo.pop_back();
    for (size_t k = 1; k < s.mixer.size(); ++k) {
      const MixerTrack& m = s.mixer[k];
      if (have.count(m.id) || found.count(m.id) || isSpaceTrack(s, m.id)) continue;
      bool feeds = false;
      for (const auto& sd : m.sends) feeds = feeds || (sd.to == t && !sd.stageRoom);
      if (!feeds) continue;
      found.insert(m.id);
      todo.push_back(m.id);
    }
  }
  std::vector<ObjectId> out;
  for (size_t k = 1; k < s.mixer.size(); ++k)
    if (found.count(s.mixer[k].id)) out.push_back(s.mixer[k].id);
  return out;
}

namespace {
const std::vector<Clip>& clipsOf(const Song& s, int arr) {
  if (arr == s.currentArrangement || s.arrangements.empty() || arr < 0 || arr >= static_cast<int>(s.arrangements.size())) return s.playlist;
  return s.arrangements[static_cast<size_t>(arr)].playlist;
}

void addChannelTrack(const Song& s, ObjectId channel, std::set<ObjectId>& out) {
  const Channel* c = s.channel(channel);
  if (!c) return;
  if (c->mixerTrack != kNoId && c->mixerTrack != s.masterId()) out.insert(c->mixerTrack);
  for (ObjectId a : c->auxOuts)
    if (a != kNoId && a != s.masterId()) out.insert(a);
}

void addPatternTracks(const Song& s, const Pattern& p, std::set<ObjectId>& out) {
  for (const auto& [ch, ns] : p.notes)
    if (!ns.empty()) addChannelTrack(s, ch, out);
  for (const auto& [ch, st] : p.steps) {
    bool any = false;
    for (uint8_t v : st) any = any || v != 0;
    if (any) addChannelTrack(s, ch, out);
  }
}

// The tracks in `in` plus every track they send into (not the master or a space), mixer order.
std::vector<ObjectId> withDestinations(const Song& s, std::set<ObjectId> in) {
  std::vector<ObjectId> todo(in.begin(), in.end());
  while (!todo.empty()) {
    const MixerTrack* m = s.mixerTrack(todo.back());
    todo.pop_back();
    if (!m) continue;
    for (const auto& sd : m->sends)
      if (sd.to != s.masterId() && !sd.stageRoom && !isSpaceTrack(s, sd.to) && in.insert(sd.to).second) todo.push_back(sd.to);
  }
  std::vector<ObjectId> out;
  for (size_t k = 1; k < s.mixer.size(); ++k)
    if (in.count(s.mixer[k].id) && !isSpaceTrack(s, s.mixer[k].id)) out.push_back(s.mixer[k].id);
  return out;
}
}  // namespace

std::vector<ObjectId> tracksOfArrangement(const Song& s, int arr) {
  std::set<ObjectId> used;
  for (const Clip& c : clipsOf(s, arr)) {
    if (c.kind == ClipKind::Audio) addChannelTrack(s, c.source, used);
    else if (c.kind == ClipKind::Pattern)
      if (const Pattern* p = s.pattern(c.pattern)) addPatternTracks(s, *p, used);
  }
  return withDestinations(s, std::move(used));
}

std::vector<ObjectId> tracksOfPattern(const Song& s, ObjectId pattern) {
  std::set<ObjectId> used;
  if (const Pattern* p = s.pattern(pattern)) addPatternTracks(s, *p, used);
  return withDestinations(s, std::move(used));
}

std::vector<std::string> otherArrangementsUsing(const Song& s, ObjectId track, int arr) {
  std::vector<std::string> out;
  const int n = std::max(1, static_cast<int>(s.arrangements.size()));
  for (int k = 0; k < n; ++k) {
    if (k == arr) continue;
    const std::vector<ObjectId> t = tracksOfArrangement(s, k);
    if (std::find(t.begin(), t.end(), track) != t.end())
      out.push_back(s.arrangements.empty() ? std::string("Arrangement") : s.arrangements[static_cast<size_t>(k)].name);
  }
  return out;
}

// ---------------------------------------------------------------- templates ----
const std::vector<RoomTemplate>& factoryTemplates() {
  static const std::vector<RoomTemplate> list = [] {
    auto t = [](const char* name, const char* hint, int type, float strength, float width, float level) {
      RoomTemplate r;
      r.name = name;
      r.hint = hint;
      r.factory = true;
      r.roomType = type;
      r.depthStrength = strength;
      r.width = width;
      r.level = level;
      return r;
    };
    std::vector<RoomTemplate> v;
    v.push_back(t(i18n::N_("Bedroom demo"), i18n::N_("Close and dry, a little narrow"), rooms::kSmall, 0.0f, 0.85f, 0.72f));
    v.push_back(t(i18n::N_("Live band"), i18n::N_("A tight live room with natural depth"), rooms::kStudio, 0.5f, 1.0f, 0.8f));
    v.push_back(t(i18n::N_("Late club"), i18n::N_("Punchy and wide with a short dark tail"), 3 /* club */, 0.5f, 1.15f, 0.84f));
    v.push_back(t(i18n::N_("Concert hall"), i18n::N_("Warm and deep with a long even tail"), rooms::kHall, 1.0f, 1.0f, 0.8f));
    v.push_back(t(i18n::N_("Stone church"), i18n::N_("A big soft bloom, the back row far away"), 7 /* church */, 1.0f, 1.1f, 0.76f));
    v.push_back(t(i18n::N_("Open field"), i18n::N_("Distance with almost no tail"), rooms::kCount - 1 /* open air */, 1.0f, 1.2f, 0.6f));
    v[0].preDelay = false;
    return v;
  }();
  return list;
}

RoomTemplate templateOf(const Song& s, int i, const std::string& name) {
  const StageSettings& st = roomAt(s, i);
  RoomTemplate t;
  t.name = name;
  t.roomType = st.roomType;
  t.depthStrength = st.depthStrength;
  t.width = st.width;
  t.preDelay = st.preDelay;
  t.early = st.early;
  t.frontQuiet = st.frontQuiet;
  if (const MixerTrack* sp = mixops::stageRoom(s, i)) {
    t.level = sp->volume;
    t.spaceOn = !sp->muted;
    const int k = mixops::spaceSlot(*sp);
    if (k >= 0) {
      t.reverb = sp->slots[static_cast<size_t>(k)].plugin.params;
      t.size = mixops::roomSize(s, i);
    }
  }
  return t;
}

std::string templateFolder(const std::string& presetRoot) { return (fs::u8path(presetRoot) / "Stage rooms").u8string(); }

std::string templateToJsonText(const RoomTemplate& t) {
  Json j = Json::makeObject();
  auto b = [](bool v) {
    Json x;
    x.type = Json::Type::Bool;
    x.boolean = v;
    return x;
  };
  auto n = [](double v) { return Json::makeNumber(std::round(v * 1e6) / 1e6); };
  j.object["format"] = Json::makeString(kTemplateFormat);
  j.object["version"] = Json::makeNumber(kTemplateVersion);
  j.object["name"] = Json::makeString(t.name);
  j.object["space"] = Json::makeString(rooms::key(t.roomType));
  j.object["depthStrength"] = n(t.depthStrength);
  j.object["width"] = n(t.width);
  j.object["level"] = n(t.level);
  if (t.size >= 0) j.object["size"] = n(t.size);
  if (!t.reverb.empty()) {
    Json a = Json::makeArray();
    for (float v : t.reverb) a.array.push_back(n(v));
    j.object["reverb"] = std::move(a);
  }
  j.object["preDelay"] = b(t.preDelay);
  j.object["early"] = b(t.early);
  j.object["frontQuiet"] = b(t.frontQuiet);
  j.object["spaceOn"] = b(t.spaceOn);
  return jsonStringify(j);
}

bool templateFromJsonText(const std::string& text, RoomTemplate& out, std::string* err) {
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "not a JSON object";
    return false;
  }
  if (j.text("format") != kTemplateFormat) {
    if (err) *err = "not a stage room template";
    return false;
  }
  auto flagOf = [&j](const char* k, bool d) {
    const Json* v = j.find(k);
    return v && v->is(Json::Type::Bool) ? v->boolean : d;
  };
  auto clampNum = [&j](const char* k, double d, double lo, double hi) {
    const double v = j.num(k, d);
    return std::isfinite(v) ? std::clamp(v, lo, hi) : d;
  };
  RoomTemplate t;
  t.name = j.text("name");
  t.roomType = j.find("space") ? rooms::ofKey(j.text("space")) : 0;
  if (j.text("space") == "none") t.roomType = 0;
  t.depthStrength = static_cast<float>(clampNum("depthStrength", 0.5, 0, 1));
  t.width = static_cast<float>(clampNum("width", 1, 0, 1.5));
  t.level = static_cast<float>(clampNum("level", 0.8, 0, 1.25));
  t.size = j.find("size") ? clampNum("size", 0.5, 0, 1) : -1;
  if (const Json* r = j.find("reverb"); r && r->is(Json::Type::Array) && r->array.size() <= 256)
    for (const Json& v : r->array) t.reverb.push_back(v.is(Json::Type::Number) && std::isfinite(v.number) ? static_cast<float>(v.number) : 0.0f);
  t.preDelay = flagOf("preDelay", true);
  t.early = flagOf("early", true);
  t.frontQuiet = flagOf("frontQuiet", true);
  t.spaceOn = flagOf("spaceOn", true);
  out = std::move(t);
  return true;
}

std::vector<RoomTemplate> listUserTemplates(const std::string& presetRoot) {
  std::vector<RoomTemplate> out;
  if (presetRoot.empty()) return out;
  const fs::path root = fs::u8path(templateFolder(presetRoot));
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return out;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
    std::ifstream in(it->path(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    RoomTemplate t;
    if (!templateFromJsonText(ss.str(), t)) continue;
    t.name = it->path().stem().u8string();  // the file name is the template's name in the library
    const fs::path rel = it->path().parent_path().lexically_relative(root);
    t.folder = rel == "." ? std::string() : rel.generic_u8string();
    t.file = it->path().u8string();
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(), [](const RoomTemplate& a, const RoomTemplate& b) { return a.folder != b.folder ? a.folder < b.folder : a.name < b.name; });
  return out;
}

bool saveUserTemplate(const std::string& presetRoot, const RoomTemplate& t, const std::string& relName, std::string* path, std::string* err) {
  fs::path f = fs::u8path(templateFolder(presetRoot));
  std::string part;
  std::vector<std::string> parts;
  for (char c : (relName.empty() ? t.name : relName) + "/") {
    if (c == '/' || c == '\\') {
      if (!part.empty() && part != "." && part != "..") parts.push_back(safePresetName(part));
      part.clear();
    } else {
      part += c;
    }
  }
  if (parts.empty()) parts.push_back(safePresetName(t.name.empty() ? std::string("Room") : t.name));
  for (size_t i = 0; i + 1 < parts.size(); ++i) f /= fs::u8path(parts[i]);
  f /= fs::u8path(parts.back() + ".json");
  std::error_code ec;
  fs::create_directories(f.parent_path(), ec);
  RoomTemplate q = t;
  q.name = parts.back();
  if (!writeFileAtomic(f.u8string(), templateToJsonText(q), err)) return false;
  if (path) *path = f.u8string();
  return true;
}

bool deleteUserTemplate(const RoomTemplate& t) {
  if (t.factory || t.file.empty()) return false;
  std::error_code ec;
  return fs::remove(fs::u8path(t.file), ec);
}

// ---------------------------------------------------------------- edits ----
namespace {
// Gives the first room an id of its own (before a second room exists or the first changes place), and
// writes it on every track that stood in the first room without saying so. Inside a group.
void pinFirstRoom(Document& d) {
  if (d.song().stage.id == kNoId) {
    d.editSong("room", [](Song& s) { s.stage.id = s.allocateId(); });
  }
  const ObjectId first = d.song().stage.id;
  std::vector<ObjectId> loose;
  for (size_t k = 1; k < d.song().mixer.size(); ++k)
    if (d.song().mixer[k].room == kNoId) loose.push_back(d.song().mixer[k].id);
  for (ObjectId id : loose) d.editMixerTrack(id, "room", [first](MixerTrack& m) { m.room = first; });
}

// The space track's sound from a template (its level, on or off, size or whole reverb). Inside a group.
void applySpaceSound(Document& d, int room, const RoomTemplate& t) {
  const MixerTrack* sp = mixops::stageRoom(d.song(), room);
  if (!sp) return;
  const ObjectId id = sp->id;
  d.setParam(mixerParam(id, MixerParam::Volume), t.level, false);
  if (t.roomType != 0) d.setParam(mixerParam(id, MixerParam::Mute), t.spaceOn ? 0.0 : 1.0, false);
  const int k = mixops::spaceSlot(*sp);
  if (k < 0) return;
  if (!t.reverb.empty()) {
    const auto& cur = d.song().mixerTrack(id)->slots[static_cast<size_t>(k)].plugin.params;
    for (size_t p = 0; p < t.reverb.size() && p < cur.size(); ++p) d.setParam(slotParam(id, k, static_cast<int>(p)), t.reverb[p], false);
  } else if (t.size >= 0) {
    mixops::setRoomSize(d, t.size, false, room);
  }
}

// The settings of a template on a room's settings (not its identity or space track).
void copySettings(StageSettings& st, const RoomTemplate& t) {
  st.roomType = static_cast<uint8_t>(std::clamp(t.roomType, 0, rooms::kCount - 1));
  st.depthStrength = t.depthStrength;
  st.width = t.width;
  st.preDelay = t.preDelay;
  st.early = t.early;
  st.frontQuiet = t.frontQuiet;
}
}  // namespace

int addRoom(Document& d, int copyFrom, const RoomTemplate* tmpl, const std::string& name, Rgb color) {
  const int src = std::clamp(copyFrom, 0, roomCount(d.song()) - 1);
  const RoomTemplate t = tmpl ? *tmpl : templateOf(d.song(), src, "");
  d.endGesture();
  d.beginGroup("new room");
  const bool wasOne = roomCount(d.song()) == 1;
  pinFirstRoom(d);
  // The first room's space track said "Room" while it was the only one: it is now that room's space.
  if (wasOne)
    if (const MixerTrack* sp = mixops::stageRoom(d.song(), 0); sp && sp->name == "Room") {
      const std::string nm = spaceNameOf(roomName(d.song(), 0));  // named now, before the second room exists
      d.editMixerTrack(sp->id, "room", [&nm](MixerTrack& m) { m.name = nm; });
    }
  const int index = roomCount(d.song());
  d.editSong("new room", [&](Song& s) {
    StageSettings st;
    st.id = s.allocateId();
    st.name = name;
    st.color = color;
    copySettings(st, t);
    s.stageRooms.push_back(st);
  });
  if (t.roomType != 0) {
    mixops::ensureStageRoom(d, {}, "new room", index);
    applySpaceSound(d, index, t);
  }
  d.endGroup();
  d.endGesture();
  return index;
}

bool renameRoom(Document& d, int i, const std::string& name) {
  if (i < 0 || i >= roomCount(d.song()) || roomAt(d.song(), i).name == name) return false;
  d.endGesture();
  d.beginGroup("rename room");
  d.editSong("rename room", [i, &name](Song& s) {
    if (StageSettings* st = roomAtMut(s, i)) st->name = name;
  });
  if (roomCount(d.song()) > 1)
    if (const MixerTrack* sp = mixops::stageRoom(d.song(), i)) {
      const std::string nm = spaceTrackName(d.song(), i);
      d.editMixerTrack(sp->id, "rename room", [&nm](MixerTrack& m) { m.name = nm; });
    }
  d.endGroup();
  return true;
}

bool setRoomColor(Document& d, int i, Rgb color) {
  if (i < 0 || i >= roomCount(d.song()) || roomAt(d.song(), i).color == color) return false;
  d.endGesture();
  d.editSong("room colour", [i, color](Song& s) {
    if (StageSettings* st = roomAtMut(s, i)) st->color = color;
  });
  return true;
}

bool moveRoom(Document& d, int from, int to) {
  const int n = roomCount(d.song());
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
  d.endGesture();
  d.beginGroup("move room");
  if (from == 0 || to == 0) pinFirstRoom(d);  // the first room changes: every track says its room
  d.editSong("move room", [from, to](Song& s) {
    std::vector<StageSettings> all;
    all.push_back(s.stage);
    for (auto& r : s.stageRooms) all.push_back(r);
    StageSettings moved = all[static_cast<size_t>(from)];
    all.erase(all.begin() + from);
    all.insert(all.begin() + to, moved);
    s.stage = all.front();
    s.stageRooms.assign(all.begin() + 1, all.end());
  });
  d.endGroup();
  return true;
}

namespace {
// The track plays straight into the master (not only through a bus or a sidechain).
bool sendsToMaster(const Song& s, ObjectId id) {
  const MixerTrack* m = s.mixerTrack(id);
  if (!m) return false;
  for (const auto& sd : m->sends)
    if (sd.to == s.masterId() && !sd.sidechain && sd.level > 0.0f) return true;
  return false;
}
}  // namespace

int moveTracksToRoom(Document& d, const std::vector<ObjectId>& tracksIn, int to, bool feeders) {
  const Song& s0 = d.song();
  if (to < 0 || to >= roomCount(s0)) return 0;
  std::vector<ObjectId> tracks = tracksIn;
  if (feeders)
    for (ObjectId f : feedersOf(s0, tracksIn)) tracks.push_back(f);
  std::vector<ObjectId> moving;
  std::set<ObjectId> seen;
  for (ObjectId id : tracks) {
    const MixerTrack* m = s0.mixerTrack(id);
    if (!m || id == s0.masterId() || isSpaceTrack(s0, id) || !seen.insert(id).second) continue;
    if (trackRoom(s0, *m) == to) continue;
    moving.push_back(id);
  }
  if (moving.empty()) return 0;
  d.beginGroup("move to room");
  if (to == 0 && roomCount(d.song()) > 1) pinFirstRoom(d);
  const ObjectId rid = roomIdAt(d.song(), to);
  for (ObjectId id : moving) {
    const Song& s = d.song();
    const MixerTrack* m = s.mixerTrack(id);
    if (!m) continue;
    const int from = trackRoom(s, *m);
    // Its room send: into the new room's space, at the new space's level when it was at the old one's.
    const MixerTrack* oldSp = mixops::stageRoom(s, from);
    float level = -1;
    bool hadRoomSend = false;
    if (oldSp)
      for (const auto& sd : m->sends)
        if (sd.to == oldSp->id && sd.stageRoom) hadRoomSend = true, level = sd.level;
    const bool defaultLevel = hadRoomSend && std::fabs(level - mixops::roomSendLevel(s, from)) < 1e-6f;
    const ObjectId oldSpId = oldSp ? oldSp->id : kNoId;
    d.editMixerTrack(id, "move to room", [rid](MixerTrack& t) { t.room = rid; });
    if (hadRoomSend) {
      d.setSend(id, oldSpId, 0.0f);
      if (d.song().mixerTrack(id) && d.song().mixerTrack(id)->sends.empty()) d.setSend(id, d.song().masterId(), 1.0f);
      if (roomAt(d.song(), to).hasRoom()) {
        if (!mixops::stageRoom(d.song(), to)) mixops::ensureStageRoom(d, {}, "move to room", to);
        if (const MixerTrack* sp = mixops::stageRoom(d.song(), to)) {
          const ObjectId spId = sp->id;
          const float lv = defaultLevel ? mixops::roomSendLevel(d.song(), to) : level;
          if (mixops::routeProblem(d.song(), id, spId).empty() && d.setSend(id, spId, lv)) mixops::setSendRoom(d, id, spId, true);
        }
      }
    } else if (roomAt(d.song(), to).hasRoom() && sendsToMaster(d.song(), id)) {
      // No room send yet: the track takes its new room's sound, at the level a new track there gets.
      // Tracks that only feed a bus stay dry (the bus carries them into the room).
      if (!mixops::stageRoom(d.song(), to)) mixops::ensureStageRoom(d, {}, "move to room", to);
      mixops::addRoomSends(d, {id}, to);
    }
  }
  d.endGroup();
  return static_cast<int>(moving.size());
}

bool deleteRoom(Document& d, int i) {
  // Never a room with tracks (they are moved to another room first), never the last room.
  if (deleteBlock(d.song(), i) != DeleteBlock::None) return false;
  d.endGesture();
  d.beginGroup("delete room");
  pinFirstRoom(d);
  // Its unused inserts (no channel, effects, input, name, routing or place) go to the room before it
  // (the next one when it is the first): nothing is heard from them, so nothing else changes.
  const int dest = i > 0 ? i - 1 : 1;
  const ObjectId gone = roomIdAt(d.song(), i), keep = roomIdAt(d.song(), dest);
  // The room's space track goes with it.
  if (const MixerTrack* sp = mixops::stageRoom(d.song(), i)) d.removeMixerTrack(sp->id);
  d.editSong("delete room", [gone, keep](Song& s) {
    if (s.stage.id == gone) {
      s.stage = s.stageRooms.front();
      s.stageRooms.erase(s.stageRooms.begin());
    } else {
      s.stageRooms.erase(std::remove_if(s.stageRooms.begin(), s.stageRooms.end(), [gone](const StageSettings& r) { return r.id == gone; }),
                         s.stageRooms.end());
    }
    for (auto& m : s.mixer)
      if (m.room == gone) m.room = keep;
  });
  // One room left: it plays as a one-room project again ("Room" is its space's name).
  if (roomCount(d.song()) == 1)
    if (const MixerTrack* sp = mixops::stageRoom(d.song(), 0)) d.editMixerTrack(sp->id, "delete room", [](MixerTrack& m) { m.name = "Room"; });
  d.endGroup();
  return true;
}

bool applyTemplate(Document& d, int i, const RoomTemplate& t) {
  if (i < 0 || i >= roomCount(d.song())) return false;
  d.endGesture();
  d.beginGroup("room template");
  const std::vector<ObjectId> members = roomTracks(d.song(), i);
  std::vector<ObjectId> withSend;
  if (const MixerTrack* sp = mixops::stageRoom(d.song(), i))
    for (ObjectId id : members)
      if (const MixerTrack* m = d.song().mixerTrack(id); m && sendsInto(*m, sp->id)) withSend.push_back(id);
  mixops::setRoomType(d, t.roomType, mixops::stageRoom(d.song(), i) ? withSend : members, i);
  d.editSong("room template", [i, &t](Song& s) {
    if (StageSettings* st = roomAtMut(s, i)) {
      const ObjectId sp = st->room;
      copySettings(*st, t);
      st->room = sp;
    }
  });
  if (t.roomType != 0) applySpaceSound(d, i, t);
  d.endGroup();
  d.endGesture();
  return true;
}

}  // namespace gl::roomops
