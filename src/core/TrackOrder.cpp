// Gridline: putting mixer tracks and playlist tracks in a new order (see TrackOrder.h).
#include "TrackOrder.h"
#include <algorithm>
#include <cmath>
#include "Params.h"
#include "PluginInfo.h"

namespace gl::trackorder {

namespace {

constexpr const char* kMixerSendType = "gl.send";  // its parameter 0 names the target track

int paramInt(const PluginState& p, int k, int dflt) {
  return k >= 0 && static_cast<size_t>(k) < p.params.size() ? static_cast<int>(std::lround(p.params[static_cast<size_t>(k)])) : dflt;
}

// The state's send parameters hold route numbers of the hosting track (PluginInfo::routeModeParam).
bool routeNumbered(const PluginInfo& pi, const PluginState& p) {
  if (pi.routeModeParam == -1) return false;
  if (pi.routeModeParam == kRouteIndexAlways) return true;
  return paramInt(p, pi.routeModeParam, 0) >= 1;
}

// Where `target` (a mixer index of `s`) stands in `list` from position 1 on (0: not there).
int positionIn(const std::vector<int>& list, int target) {
  for (size_t r = 1; r < list.size(); ++r)
    if (list[r] == target) return static_cast<int>(r);
  return 0;
}

// The mixer index parameters of one plugin hosted on (or, for an instrument, routed through) the
// track that is at `oldHost` in `old` and at `newHost` in `now`. `to[old index]` = new index.
void remapPlugin(PluginState& p, const Song& old, const Song& now, int oldHost, int newHost, const std::vector<int>& to) {
  const PluginInfo* pi = findPlugin(p.type);
  if (!pi) return;
  auto set = [&](int k, int v) { p.params[static_cast<size_t>(k)] = static_cast<float>(v); };
  auto has = [&](int k) { return k >= 0 && static_cast<size_t>(k) < p.params.size(); };
  std::vector<int> sends = pi->sendParams;
  if (p.type == kMixerSendType) sends.push_back(0);
  if (!sends.empty() && oldHost >= 0 && newHost >= 0) {
    const bool routed = routeNumbered(*pi, p);
    const std::vector<int> outsOld = routed ? flRouteOutputs(old, oldHost) : std::vector<int>{};
    const std::vector<int> outsNew = routed ? flRouteOutputs(now, newHost) : std::vector<int>{};
    for (int k : sends) {
      if (!has(k)) continue;
      const int v = paramInt(p, k, -1);
      if (!routed) {  // a mixer index (-1 = none)
        if (v >= 0 && static_cast<size_t>(v) < to.size() && to[static_cast<size_t>(v)] != v) set(k, to[static_cast<size_t>(v)]);
        continue;
      }
      // A route number n of the host: its destinations in mixer order, so it follows the track it names.
      if (v <= 0 || outsOld.size() < 2) continue;
      const int target = outsOld[std::min(static_cast<size_t>(v), outsOld.size() - 1)];
      if (target == oldHost) continue;
      const int n = positionIn(outsNew, to[static_cast<size_t>(target)]);
      if (n > 0 && n != std::min(v, static_cast<int>(outsNew.size()) - 1)) set(k, n);
    }
  }
  if (has(pi->sideInParam) && oldHost >= 0 && newHost >= 0) {  // a route number among the host's sources
    const int v = paramInt(p, pi->sideInParam, -1);
    if (v >= 1) {
      const int src = flRouteInput(old, oldHost, v);
      if (src >= 0) {
        const int n = positionIn(flRouteInputs(now, newHost), to[static_cast<size_t>(src)]);
        if (n > 0 && n != v) set(pi->sideInParam, n);
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- mixer ----

bool reorderMixer(Song& s, const std::vector<ObjectId>& order) {
  const size_t n = s.mixer.size();
  if (order.size() != n || n == 0 || order[0] != s.masterId()) return false;
  std::vector<int> to(n, -1);  // to[old index] = new index
  for (size_t k = 0; k < n; ++k) {
    const int i = s.mixerIndex(order[k]);
    if (i < 0 || to[static_cast<size_t>(i)] >= 0) return false;
    to[static_cast<size_t>(i)] = static_cast<int>(k);
  }
  bool same = true;
  for (size_t i = 0; i < n; ++i) same = same && to[i] == static_cast<int>(i);
  if (same) return true;
  const Song old = s;
  std::vector<MixerTrack> mixer(n);
  for (size_t i = 0; i < n; ++i) mixer[static_cast<size_t>(to[i])] = std::move(s.mixer[i]);
  s.mixer = std::move(mixer);

  // Send levels addressed by the destination's index.
  auto param = [&](ObjectId owner, uint32_t& index) {
    if (index < kSendBase || index >= kSlotBase || !old.mixerTrack(owner)) return;
    const uint32_t d = index - kSendBase;
    if (d < n) index = kSendBase + static_cast<uint32_t>(to[d]);
  };
  for (auto& l : s.links) param(l.owner, l.index);
  for (auto& l : s.modLinks) param(l.owner, l.index);
  for (auto& a : s.automations) param(a.owner, a.index);
  for (auto& p : s.patterns)
    for (auto& a : p.automation) param(a.owner, a.index);

  // Plugin parameters that name tracks by index or by route number.
  for (size_t k = 0; k < n; ++k) {
    MixerTrack& m = s.mixer[k];
    const int oldIndex = old.mixerIndex(m.id);
    for (auto& slot : m.slots)
      if (!slot.plugin.empty()) remapPlugin(slot.plugin, old, s, oldIndex, static_cast<int>(k), to);
  }
  for (auto& c : s.channels) {  // instruments: their side input is a source of the channel's track
    const ObjectId host = s.mixerTrack(c.mixerTrack) ? c.mixerTrack : s.masterId();
    remapPlugin(c.plugin, old, s, old.mixerIndex(host), s.mixerIndex(host), to);
  }
  return true;
}

std::vector<ObjectId> mixerOrderAfterMove(const Song& s, const std::vector<ObjectId>& ids, int before) {
  const int n = static_cast<int>(s.mixer.size());
  if (n < 2) return {};
  std::set<ObjectId> moving;
  for (ObjectId id : ids)
    if (id != s.masterId() && s.mixerTrack(id)) moving.insert(id);
  if (moving.empty()) return {};
  before = std::clamp(before, 1, n);
  std::vector<ObjectId> block, order;
  for (const auto& m : s.mixer)
    if (moving.count(m.id)) block.push_back(m.id);
  for (int i = 0; i < n; ++i) {
    if (i == before) order.insert(order.end(), block.begin(), block.end());
    if (!moving.count(s.mixer[static_cast<size_t>(i)].id)) order.push_back(s.mixer[static_cast<size_t>(i)].id);
  }
  if (before == n) order.insert(order.end(), block.begin(), block.end());
  return order;
}

bool moveMixerTracks(Document& d, const std::vector<ObjectId>& ids, int before, int dock) {
  const Song& s = d.song();
  const std::vector<ObjectId> order = mixerOrderAfterMove(s, ids, before);
  if (order.empty()) return false;
  bool moves = false;
  for (size_t k = 0; k < order.size(); ++k) moves = moves || order[k] != s.mixer[k].id;
  bool docks = false;
  if (dock != kKeepDock)
    for (ObjectId id : ids)
      if (const MixerTrack* m = s.mixerTrack(id); m && id != s.masterId() && m->dock != dock) docks = true;
  if (!moves && !docks) return false;
  size_t count = 0;
  for (ObjectId id : ids) count += id != s.masterId() && s.mixerTrack(id) ? 1 : 0;
  d.editSong(count > 1 ? "move mixer tracks" : "move mixer track", [&](Song& x) {
    reorderMixer(x, order);
    if (dock != kKeepDock)
      for (ObjectId id : ids)
        if (MixerTrack* m = x.mixerTrack(id); m && id != x.masterId()) m->dock = dock;
  });
  return true;
}

bool stepMixerTracks(Document& d, const std::vector<ObjectId>& ids, int dir) {
  const Song& s = d.song();
  int first = -1, last = -1;
  for (ObjectId id : ids) {
    const int i = s.mixerIndex(id);
    if (i < 1) continue;
    first = first < 0 ? i : std::min(first, i);
    last = std::max(last, i);
  }
  if (first < 1 || dir == 0) return false;
  if (dir < 0) return first > 1 && moveMixerTracks(d, ids, first - 1);
  return last + 1 < static_cast<int>(s.mixer.size()) && moveMixerTracks(d, ids, last + 2);
}

// ------------------------------------------------------------- playlist ----

namespace {

// Track k is inside a folded group (not drawn): the outermost folded header above it, else -1.
int foldedHeaderOf(const Song& s, int k) {
  int found = -1;
  for (int h = k - 1; h >= 0; --h) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(h)];
    if (t.groupSize > 0 && t.folded && h + t.groupSize >= k) found = h;
  }
  return found;
}

}  // namespace

std::set<int> withGroups(const Song& s, const std::vector<int>& tracks) {
  const int n = static_cast<int>(s.tracks.size());
  std::set<int> out;
  std::vector<int> todo;
  for (int t : tracks)
    if (t >= 0 && t < n) todo.push_back(t);
  while (!todo.empty()) {
    const int t = todo.back();
    todo.pop_back();
    if (!out.insert(t).second) continue;
    const int size = s.tracks[static_cast<size_t>(t)].groupSize;
    for (int k = 1; k <= size && t + k < n; ++k) todo.push_back(t + k);
  }
  return out;
}

PlaylistMove playlistOrderAfterMove(const Song& s, const std::vector<int>& tracks, int before) {
  PlaylistMove m;
  const int n = static_cast<int>(s.tracks.size());
  m.moved = withGroups(s, tracks);
  if (m.moved.empty() || static_cast<int>(m.moved.size()) >= n) return {};
  before = std::clamp(before, 0, n);
  for (int i = 0; i < n; ++i) {
    if (i == before)
      for (int k : m.moved) m.order.push_back(k);
    if (!m.moved.count(i)) m.order.push_back(i);
  }
  if (before == n)
    for (int k : m.moved) m.order.push_back(k);
  bool same = true;
  for (int i = 0; i < n; ++i) same = same && m.order[static_cast<size_t>(i)] == i;
  if (same) return {};
  return m;
}

bool reorderPlaylist(Song& s, const PlaylistMove& m) {
  const size_t n = s.tracks.size();
  if (m.order.size() != n) return false;
  std::vector<int> to(n, -1);
  for (size_t k = 0; k < n; ++k) {
    const int i = m.order[k];
    if (i < 0 || static_cast<size_t>(i) >= n || to[static_cast<size_t>(i)] >= 0) return false;
    to[static_cast<size_t>(i)] = static_cast<int>(k);
  }
  // Groups: each header keeps the members that did not leave (all of them when it moved itself);
  // its span runs to the last of them, so tracks dropped among them join.
  std::vector<int> size(n, 0);
  for (size_t h = 0; h < n; ++h) {
    const int g = s.tracks[h].groupSize;
    if (g <= 0) continue;
    const bool headMoved = m.moved.count(static_cast<int>(h)) > 0;
    int last = -1;
    for (int k = 1; k <= g && h + static_cast<size_t>(k) < n; ++k) {
      const int member = static_cast<int>(h) + k;
      if (headMoved || !m.moved.count(member)) last = std::max(last, to[static_cast<size_t>(member)]);
    }
    if (last > to[h]) size[h] = last - to[h];
  }
  std::vector<PlaylistTrack> tracks(n);
  for (size_t i = 0; i < n; ++i) {
    PlaylistTrack& t = tracks[static_cast<size_t>(to[i])];
    t = std::move(s.tracks[i]);
    if (t.groupSize > 0 && size[i] == 0) t.folded = false;  // every member left: no longer a group
    t.groupSize = size[i];
  }
  s.tracks = std::move(tracks);
  for (auto& c : s.playlist)
    if (c.track >= 0 && static_cast<size_t>(c.track) < n) c.track = to[static_cast<size_t>(c.track)];
  return true;
}

bool movePlaylistTracks(Document& d, const std::vector<int>& tracks, int before, std::vector<int>* newIndexes) {
  const PlaylistMove m = playlistOrderAfterMove(d.song(), tracks, before);
  if (m.empty()) return false;
  std::set<int> asked(tracks.begin(), tracks.end());
  d.editSong(m.moved.size() > 1 ? "move playlist tracks" : "move playlist track", [&](Song& x) { reorderPlaylist(x, m); });
  if (newIndexes) {
    newIndexes->clear();
    for (size_t k = 0; k < m.order.size(); ++k)
      if (asked.count(m.order[k])) newIndexes->push_back(static_cast<int>(k));
  }
  return true;
}

bool stepPlaylistTracks(Document& d, const std::vector<int>& tracks, int dir, std::vector<int>* newIndexes) {
  const Song& s = d.song();
  const std::set<int> moving = withGroups(s, tracks);
  if (moving.empty() || dir == 0) return false;
  const int n = static_cast<int>(s.tracks.size());
  int before;
  if (dir < 0) {
    before = *moving.begin() - 1;
    if (before < 0) return false;
    if (const int h = foldedHeaderOf(s, before); h >= 0 && !moving.count(h)) before = h;  // past a folded group as a whole
  } else {
    const int next = *moving.rbegin() + 1;
    if (next >= n) return false;
    before = next + 1;
    if (s.tracks[static_cast<size_t>(next)].folded) before = *withGroups(s, {next}).rbegin() + 1;
  }
  return movePlaylistTracks(d, tracks, before, newIndexes);
}

}  // namespace gl::trackorder
