// Gridline — J-82: clip source usage and deletion (see ClipSources.h).
#include "ClipSources.h"
#include <algorithm>
#include <set>

namespace gl::ops {

namespace {

bool clipOf(const Clip& c, ClipKind kind, ObjectId source) {
  if (kind == ClipKind::Pattern) return c.kind == ClipKind::Pattern && c.pattern == source;
  return c.kind == kind && c.source == source;
}

}  // namespace

bool channelHasPatternData(const Song& s, ObjectId channel) {
  for (const auto& p : s.patterns) {
    if (auto it = p.notes.find(channel); it != p.notes.end() && !it->second.empty()) return true;
    if (auto it = p.steps.find(channel); it != p.steps.end())
      for (uint8_t v : it->second)
        if (v) return true;
  }
  return false;
}

bool isAudioClipSource(const Channel& ch) {
  return ch.plugin.type == "gl.sampler" && !ch.plugin.data.empty();
}

int clipSourceUses(const Song& s, ClipKind kind, ObjectId source) {
  int n = 0;
  for (const auto& c : s.playlist) n += clipOf(c, kind, source) ? 1 : 0;
  for (int i = 0; i < static_cast<int>(s.arrangements.size()); ++i) {
    if (i == s.currentArrangement) continue;  // its content is `playlist`
    for (const auto& c : s.arrangements[static_cast<size_t>(i)].playlist) n += clipOf(c, kind, source) ? 1 : 0;
  }
  return n;
}

std::vector<ObjectId> unusedClipSources(const Song& s, ClipKind kind) {
  std::vector<ObjectId> out;
  if (kind == ClipKind::Pattern) {
    for (const auto& p : s.patterns)
      if (!clipSourceUses(s, kind, p.id)) out.push_back(p.id);
  } else if (kind == ClipKind::Audio) {
    for (const auto& ch : s.channels)
      if (isAudioClipSource(ch) && !clipSourceUses(s, kind, ch.id) && !channelHasPatternData(s, ch.id)) out.push_back(ch.id);
  } else {
    for (const auto& a : s.automations)
      if (!clipSourceUses(s, kind, a.id)) out.push_back(a.id);
  }
  return out;
}

int deleteClipSources(Document& d, ClipKind kind, const std::vector<ObjectId>& sources) {
  int n = 0;
  d.beginGroup("delete clip sources");
  std::set<ObjectId> seen;
  for (ObjectId id : sources) {
    if (!seen.insert(id).second) continue;
    if (kind == ClipKind::Pattern) {
      if (d.song().patterns.size() > 1 && d.removePattern(id)) ++n;
    } else if (kind == ClipKind::Audio) {
      if (d.removeChannel(id)) ++n;
    } else if (d.song().automation(id)) {
      d.editSong("delete automation clip", [id](Song& s) {
        s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(), [id](const AutomationClip& a) { return a.id == id; }),
                            s.automations.end());
        auto drop = [id](std::vector<Clip>& v) {
          v.erase(std::remove_if(v.begin(), v.end(), [id](const Clip& c) { return clipOf(c, ClipKind::Automation, id); }), v.end());
        };
        drop(s.playlist);
        for (auto& a : s.arrangements) drop(a.playlist);
      });
      ++n;
    }
  }
  d.endGroup();
  return n;
}

}  // namespace gl::ops
