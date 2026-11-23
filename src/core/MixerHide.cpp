// Gridline — hiding, narrowing and folding mixer strips (GitHub #58). See MixerHide.h.
#include "MixerHide.h"
#include <algorithm>
#include <functional>
#include <set>

namespace gl::mixhide {

namespace {

// Each changed track edited in one undo step (tracks left as they are make no edit).
bool editAll(Document& d, const std::vector<ObjectId>& tracks, const std::string& label, const std::function<void(MixerTrack&)>& fn) {
  bool any = false;
  d.beginGroup(label);
  for (ObjectId id : tracks) {
    const MixerTrack* t = d.song().mixerTrack(id);
    if (!t) continue;
    MixerTrack copy = *t;
    fn(copy);
    if (copy == *t) continue;
    any |= d.editMixerTrack(id, label, fn);
  }
  d.endGroup();
  return any;
}

std::vector<ObjectId> inserts(const Song& s, const std::vector<ObjectId>& tracks) {
  std::vector<ObjectId> out;
  for (ObjectId id : tracks)
    if (s.mixerIndex(id) > 0) out.push_back(id);
  return out;
}

std::vector<ObjectId> members(const Song& s, const std::string& group) {
  std::vector<ObjectId> out;
  if (group.empty()) return out;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (s.mixer[i].group == group) out.push_back(s.mixer[i].id);
  return out;
}

}  // namespace

bool groupFolded(const Song& s, const std::string& group) {
  if (group.empty()) return false;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (s.mixer[i].group == group && s.mixer[i].groupFolded) return true;
  return false;
}

bool trackHidden(const Song& s, size_t i) {
  if (i == 0 || i >= s.mixer.size()) return false;
  const MixerTrack& t = s.mixer[i];
  return t.hidden || groupFolded(s, t.group);
}

std::vector<Entry> arrange(const Song& s, const std::vector<size_t>& order) {
  std::vector<Entry> out;
  std::set<std::string> folded, placed;
  for (const std::string& g : foldedGroups(s)) folded.insert(g);
  for (size_t i : order) {
    if (i >= s.mixer.size()) continue;
    const MixerTrack& t = s.mixer[i];
    if (i == 0) {
      out.push_back(Entry{i, Show::Strip, {}, 0});
      continue;
    }
    if (!t.group.empty() && folded.count(t.group)) {
      if (!placed.insert(t.group).second) continue;
      int n = 0;
      for (size_t k = 1; k < s.mixer.size(); ++k) n += s.mixer[k].group == t.group;
      out.push_back(Entry{i, Show::Group, t.group, n});
      continue;
    }
    if (t.hidden) continue;
    out.push_back(Entry{i, t.narrow ? Show::Narrow : Show::Strip, {}, 0});
  }
  return out;
}

int hiddenCount(const Song& s) {
  int n = 0;
  for (size_t i = 1; i < s.mixer.size(); ++i) n += trackHidden(s, i);
  return n;
}

std::vector<ObjectId> hiddenTracks(const Song& s) {
  std::vector<ObjectId> out;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (s.mixer[i].hidden) out.push_back(s.mixer[i].id);
  return out;
}

std::vector<std::string> foldedGroups(const Song& s) {
  std::vector<std::string> out;
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    const MixerTrack& t = s.mixer[i];
    if (!t.group.empty() && t.groupFolded && std::find(out.begin(), out.end(), t.group) == out.end()) out.push_back(t.group);
  }
  return out;
}

bool setHidden(Document& d, const std::vector<ObjectId>& tracks, bool hidden) {
  return editAll(d, inserts(d.song(), tracks), hidden ? "hide mixer tracks" : "show mixer tracks", [hidden](MixerTrack& m) { m.hidden = hidden; });
}

bool setNarrow(Document& d, const std::vector<ObjectId>& tracks, bool narrow) {
  return editAll(d, inserts(d.song(), tracks), narrow ? "narrow mixer strips" : "full mixer strips", [narrow](MixerTrack& m) { m.narrow = narrow; });
}

bool setGroupFolded(Document& d, const std::string& group, bool folded) {
  return editAll(d, members(d.song(), group), folded ? "collapse mixer group" : "expand mixer group",
                 [folded](MixerTrack& m) { m.groupFolded = folded; });
}

bool showAll(Document& d) {
  std::vector<ObjectId> ids;
  for (size_t i = 1; i < d.song().mixer.size(); ++i) ids.push_back(d.song().mixer[i].id);
  return editAll(d, ids, "show all mixer tracks", [](MixerTrack& m) {
    m.hidden = false;
    m.groupFolded = false;
  });
}

bool groupMuted(const Song& s, const std::string& group) {
  bool any = false;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (s.mixer[i].group == group && !group.empty()) {
      if (!s.mixer[i].muted) return false;
      any = true;
    }
  return any;
}

bool groupSoloed(const Song& s, const std::string& group) {
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (!group.empty() && s.mixer[i].group == group && s.mixer[i].solo) return true;
  return false;
}

bool setGroupMute(Document& d, const std::string& group, bool muted) {
  return editAll(d, members(d.song(), group), muted ? "mute mixer group" : "unmute mixer group", [muted](MixerTrack& m) { m.muted = muted; });
}

bool setGroupSolo(Document& d, const std::string& group, bool solo) {
  return editAll(d, members(d.song(), group), solo ? "solo mixer group" : "unsolo mixer group", [solo](MixerTrack& m) { m.solo = solo; });
}

std::vector<int> surfaceTracks(const Song& s) {
  std::vector<int> out;
  for (size_t i = 1; i < s.mixer.size(); ++i)
    if (!trackHidden(s, i)) out.push_back(static_cast<int>(i));
  return out;
}

}  // namespace gl::mixhide
