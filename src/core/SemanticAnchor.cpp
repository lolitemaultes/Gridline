// Gridline - semantic anchors (K-112). See SemanticAnchor.h.
#include "SemanticAnchor.h"
#include <cstdlib>
#include "Song.h"

namespace gl::anchor {

namespace {
const char* const kNames[] = {"none", "channel", "mixer", "pattern", "clip", "track", "marker"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Kind::Count), "a name per kind");

// The cursor anchor kinds (app/CursorAnchor.h canchor::Kind; CollabUi checks they match).
enum CursorKind : int {
  CWindow = 0, CItem = 1, CPlaylistTrack = 2, CPlaylistHeader = 3, CPlaylistRuler = 4, CPianoGrid = 5, CPianoKeys = 6,
  CPianoRuler = 7, CPianoVelocity = 8, CRackRow = 9, CMixerStrip = 10, CStageTrack = 11
};
}  // namespace

const char* kindName(Kind k) {
  const size_t i = static_cast<size_t>(k);
  return i < static_cast<size_t>(Kind::Count) ? kNames[i] : "none";
}

Kind kindByName(const std::string& name) {
  for (size_t i = 0; i < static_cast<size_t>(Kind::Count); ++i)
    if (name == kNames[i]) return static_cast<Kind>(i);
  return Kind::Count;
}

std::string toText(const Target& t) {
  if (t.kind == Kind::None || t.kind >= Kind::Count) return "none";
  std::string s = std::string(kindName(t.kind)) + ":" + std::to_string(t.id);
  if (t.sub) s += "/" + std::to_string(t.sub);
  return s;
}

bool fromText(const std::string& text, Target& out) {
  out = Target();
  if (text == "none") return true;
  const size_t colon = text.find(':');
  if (colon == std::string::npos) return false;
  const Kind k = kindByName(text.substr(0, colon));
  if (k == Kind::Count || k == Kind::None) return false;
  const std::string rest = text.substr(colon + 1);
  if (rest.empty() || rest[0] < '0' || rest[0] > '9') return false;
  char* end = nullptr;
  const unsigned long long id = std::strtoull(rest.c_str(), &end, 10);  // (64-bit: a long is 32 bits on Windows)
  unsigned long long sub = 0;
  if (end && *end == '/') {
    const char* s = end + 1;
    if (*s < '0' || *s > '9') return false;
    sub = std::strtoull(s, &end, 10);
  }
  if (!end || *end != 0 || id > 0xFFFFFFFFull || sub > 0xFFFFFFFFull) return false;
  out.kind = k;
  out.id = static_cast<uint32_t>(id);
  out.sub = static_cast<uint32_t>(sub);
  return true;
}

Target fromCursor(int cursorKind, uint32_t a, uint32_t b, uint32_t plugin, int pluginSlot) {
  Target t;
  switch (cursorKind) {
    case CPlaylistTrack:
    case CPlaylistHeader: t = {Kind::PlaylistTrack, a, 0}; break;
    case CPianoGrid:
    case CPianoKeys:
    case CPianoRuler:
    case CPianoVelocity: t = {Kind::Pattern, a, b}; break;
    case CRackRow: t = {Kind::Channel, a, 0}; break;
    case CMixerStrip:
    case CStageTrack: t = {Kind::MixerTrack, a, 0}; break;
    default: break;
  }
  // In a plugin editor: the plugin's owner (the editor's window is not the same in every layout).
  if (t.kind == Kind::None && plugin != 0)
    t = pluginSlot < 0 ? Target{Kind::Channel, plugin, 0} : Target{Kind::MixerTrack, plugin, static_cast<uint32_t>(pluginSlot + 1)};
  return t;
}

bool exists(const Song& s, const Target& t) {
  switch (t.kind) {
    case Kind::Channel: return s.channel(t.id) != nullptr;
    case Kind::MixerTrack: return s.mixerTrack(t.id) != nullptr;
    case Kind::Pattern: return s.pattern(t.id) != nullptr;
    case Kind::Clip:
      for (const Clip& c : s.playlist)
        if (c.id == t.id) return true;
      return false;
    case Kind::PlaylistTrack: return t.id < s.tracks.size();
    case Kind::Marker:
      for (const Marker& m : s.markers)
        if (m.pos == static_cast<Tick>(t.id)) return true;
      return false;
    default: return false;
  }
}

std::string describe(const Song& s, const Target& t) {
  switch (t.kind) {
    case Kind::Channel:
      if (const Channel* c = s.channel(t.id)) return c->name;
      return {};
    case Kind::MixerTrack: {
      const int i = s.mixerIndex(t.id);
      return i < 0 ? std::string() : i == 0 ? std::string("Master") : mixerTrackName(s, static_cast<size_t>(i));
    }
    case Kind::Pattern:
      if (const Pattern* p = s.pattern(t.id)) return p->name;
      return {};
    case Kind::Clip:
      for (const Clip& c : s.playlist)
        if (c.id == t.id) {
          const Pattern* p = c.kind == ClipKind::Pattern ? s.pattern(c.pattern) : nullptr;
          return p ? p->name : std::string("Clip");
        }
      return {};
    case Kind::PlaylistTrack:
      if (t.id < s.tracks.size()) return s.tracks[t.id].name.empty() ? "Track " + std::to_string(t.id + 1) : s.tracks[t.id].name;
      return {};
    case Kind::Marker:
      for (const Marker& m : s.markers)
        if (m.pos == static_cast<Tick>(t.id)) return m.name;
      return {};
    default: return {};
  }
}

}  // namespace gl::anchor
