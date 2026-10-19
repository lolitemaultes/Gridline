#include "MenuOpsExtra.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace gl::ops {

namespace {
void sortMarkers(Song& s) {
  std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
}
}  // namespace

// ---------------------------------------------------------- time markers ----

int addMarker(Document& d, Tick pos, const std::string& name, int num, int den) {
  if (pos < 0) return -1;
  Marker m;
  m.pos = pos;
  m.name = name;
  if (num > 0 && den > 0) m.timeSigNum = std::clamp(num, 1, 16), m.timeSigDen = std::clamp(den, 1, 16);
  int index = -1;
  d.editSong(m.timeSigNum ? "add time signature marker" : "add marker", [&](Song& s) {
    s.markers.push_back(m);
    sortMarkers(s);
    for (size_t i = s.markers.size(); i-- > 0;)
      if (s.markers[i] == m) {
        index = static_cast<int>(i);
        break;
      }
  });
  return index;
}

bool renameMarker(Document& d, int index, const std::string& name) {
  if (index < 0 || index >= static_cast<int>(d.song().markers.size())) return false;
  d.editSong("rename marker", [&](Song& s) { s.markers[static_cast<size_t>(index)].name = name; });
  return true;
}

bool removeMarker(Document& d, int index) {
  if (index < 0 || index >= static_cast<int>(d.song().markers.size())) return false;
  d.editSong("delete marker", [&](Song& s) { s.markers.erase(s.markers.begin() + index); });
  return true;
}

int removeMarkersIn(Document& d, Tick from, Tick to) {
  int n = 0;
  for (const auto& m : d.song().markers) n += m.pos >= from && m.pos < to;
  if (!n) return 0;
  d.editSong("delete markers", [&](Song& s) {
    s.markers.erase(std::remove_if(s.markers.begin(), s.markers.end(), [&](const Marker& m) { return m.pos >= from && m.pos < to; }),
                    s.markers.end());
  });
  return n;
}

int nextMarker(const Song& s, Tick pos) {
  for (size_t i = 0; i < s.markers.size(); ++i)
    if (s.markers[i].pos > pos) return static_cast<int>(i);
  return -1;
}

int previousMarker(const Song& s, Tick pos) {
  for (size_t i = s.markers.size(); i-- > 0;)
    if (s.markers[i].pos < pos) return static_cast<int>(i);
  return -1;
}

std::string nextMarkerName(const Song& s) {
  std::set<std::string> used;
  for (const auto& m : s.markers) used.insert(m.name);
  for (int k = 1;; ++k) {
    std::string n = "Marker " + std::to_string(k);
    if (!used.count(n)) return n;
  }
}

// ------------------------------------------------------- playlist space ----

bool insertSpace(Document& d, Tick at, Tick length) {
  if (at < 0 || length <= 0) return false;
  d.editSong("insert space", [&](Song& s) {
    for (auto& c : s.playlist)
      if (c.start >= at) c.start += length;
    for (auto& m : s.markers)
      if (m.pos >= at) m.pos += length;
    if (s.live.areaEnd > at) s.live.areaEnd += length;
  });
  return true;
}

bool deleteSpace(Document& d, Tick from, Tick to) {
  if (from < 0 || to <= from) return false;
  const Tick len = to - from;
  d.editSong("delete space", [&](Song& s) {
    std::vector<Clip> keep;
    keep.reserve(s.playlist.size());
    for (Clip c : s.playlist) {
      const Tick end = c.start + s.clipLength(c);
      if (c.start >= to) {  // after the range
        c.start -= len;
      } else if (end <= from) {
        // before the range: unchanged
      } else if (c.start < from) {  // runs into the range: ends at `from` (or spans it: loses the middle)
        c.length = end > to ? end - len - c.start : from - c.start;
      } else if (end > to && c.kind != ClipKind::Audio) {  // starts inside, runs past: loses its head
        c.offset += to - c.start;
        c.length = end - to;
        c.start = from;
      } else {
        continue;  // inside the range (or an audio clip starting inside): deleted
      }
      keep.push_back(c);
    }
    s.playlist = std::move(keep);
    s.markers.erase(std::remove_if(s.markers.begin(), s.markers.end(), [&](const Marker& m) { return m.pos >= from && m.pos < to; }),
                    s.markers.end());
    for (auto& m : s.markers)
      if (m.pos >= to) m.pos -= len;
    if (s.live.areaEnd >= to) s.live.areaEnd -= len;
    else if (s.live.areaEnd > from) s.live.areaEnd = from;
  });
  return true;
}

// ------------------------------------------------------------- patterns ----

bool sortPatterns(Document& d, PatternSort by) {
  if (d.song().patterns.size() < 2) return false;
  d.editSong(by == PatternSort::Name ? "sort patterns by name" : "sort patterns by colour", [by](Song& s) {
    auto lower = [](std::string n) {
      for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return n;
    };
    std::stable_sort(s.patterns.begin(), s.patterns.end(), [&](const Pattern& a, const Pattern& b) {
      if (by == PatternSort::Name) return lower(a.name) < lower(b.name);
      return a.color < b.color;
    });
  });
  return true;
}

// ------------------------------------------------------------- channels ----

bool cutItself(Document& d, const std::vector<ObjectId>& channels) {
  std::set<int> used;
  for (const auto& c : d.song().channels) used.insert(c.cutGroup), used.insert(c.cutBy);
  bool ok = false;
  d.beginGroup("cut itself");
  for (ObjectId id : channels) {
    const Channel* c = d.song().channel(id);
    if (!c) continue;
    int g = c->cutGroup > 0 && c->cutGroup == c->cutBy ? c->cutGroup : 0;  // already cuts itself
    for (int k = 1; !g && k <= kMaxCutGroups; ++k)
      if (!used.count(k)) g = k;
    if (!g) break;  // all groups taken
    used.insert(g);
    ok |= d.editChannel(id, "cut itself", [g](Channel& ch) { ch.cutGroup = ch.cutBy = g; });
  }
  d.endGroup();
  return ok;
}

bool setSwingMix(Document& d, const std::vector<ObjectId>& channels, float mix) {
  const float v = std::clamp(mix, 0.0f, 1.0f);
  bool ok = false;
  d.beginGroup("set swing mix");
  for (ObjectId id : channels) ok |= d.editChannel(id, "set swing mix", [v](Channel& ch) { ch.swingMix = v; });
  d.endGroup();
  return ok;
}

ObjectId createLayer(Document& d, const std::vector<ObjectId>& channels) {
  std::vector<ObjectId> kids;
  int last = -1;
  for (ObjectId id : channels) {
    const Channel* c = d.song().channel(id);
    if (!c || !c->layer.empty() || c->plugin.type.empty()) continue;  // no layers inside layers
    if (std::find(kids.begin(), kids.end(), id) != kids.end()) continue;
    kids.push_back(id);
    last = std::max(last, d.song().channelIndex(id));
  }
  if (kids.empty()) return kNoId;
  if (kids.size() > static_cast<size_t>(kMaxLayerChildren)) kids.resize(static_cast<size_t>(kMaxLayerChildren));
  ObjectId out = kNoId;
  d.editSong("create layer", [&](Song& s) {
    Channel l;
    l.id = s.allocateId();
    l.name = "Layer";
    l.plugin = PluginState{};
    l.mixerTrack = s.masterId();
    l.layer = kids;
    if (const Channel* first = s.channel(kids.front())) l.color = first->color;
    s.channels.insert(s.channels.begin() + (last + 1), l);
    out = l.id;
  });
  return out;
}

// ---------------------------------------------------------------- mixer ----

int unrouteInputs(Document& d, ObjectId track) {
  const Song& s0 = d.song();
  if (!s0.mixerTrack(track) || track == s0.masterId()) return 0;
  int n = 0;
  for (const auto& t : s0.mixer)
    for (const auto& snd : t.sends) n += snd.to == track;
  if (!n) return 0;
  int changed = 0;
  d.editSong("unroute tracks routed here", [&](Song& s) {
    const ObjectId m = s.masterId();
    for (auto& t : s.mixer) {
      const size_t before = t.sends.size();
      t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& x) { return x.to == track; }), t.sends.end());
      if (t.sends.size() == before) continue;
      ++changed;
      if (t.sends.empty() && t.id != m) t.sends.push_back({m, 1.0f, false});
    }
  });
  return changed;
}

ObjectId createSubmix(Document& d, const std::vector<ObjectId>& tracks) {
  const Song& s0 = d.song();
  std::vector<ObjectId> src;
  int last = -1;
  for (ObjectId t : tracks)
    if (s0.mixerTrack(t) && t != s0.masterId() && std::find(src.begin(), src.end(), t) == src.end()) {
      src.push_back(t);
      last = std::max(last, s0.mixerIndex(t));
    }
  if (src.empty() || static_cast<int>(s0.mixer.size()) > kMaxMixerInserts) return kNoId;
  d.beginGroup("create submix");
  ObjectId sub = insertMixerTrack(d, last + 1);
  if (sub) {
    d.editMixerTrack(sub, "name submix", [](MixerTrack& m) { m.name = "Submix"; });
    for (ObjectId t : src) routeOnlyTo(d, t, sub);
  }
  d.endGroup();
  return sub;
}

// ---------------------------------------------------------------- notes ----

void discardLengths(std::vector<Note>& notes, NoteSel& sel, Tick len) {
  if (len <= 0) return;
  sel.resize(notes.size(), false);
  const bool any = std::find(sel.begin(), sel.end(), true) != sel.end();
  for (size_t i = 0; i < notes.size(); ++i)
    if (!any || sel[i]) notes[i].length = len;
}

// ---------------------------------------------------------- auto scroll ----

float followScroll(float scrollX, float playPx, float viewW) {
  if (viewW <= 0) return scrollX;
  const float margin = viewW * 0.04f;
  if (playPx >= scrollX && playPx <= scrollX + viewW - margin) return scrollX;
  return std::max(0.0f, playPx - margin);
}

// ------------------------------------------------------------ tap tempo ----

double TapTempo::tap(double t) {
  if (!taps.empty() && (t - taps.back() > resetAfter || t <= taps.back())) taps.clear();
  taps.push_back(t);
  if (static_cast<int>(taps.size()) > window) taps.erase(taps.begin());
  return bpm();
}

double TapTempo::bpm() const {
  if (taps.size() < 2) return 0;
  const double span = taps.back() - taps.front();
  if (span <= 0) return 0;
  return std::clamp(60.0 * static_cast<double>(taps.size() - 1) / span, 10.0, 522.0);
}

}  // namespace gl::ops
