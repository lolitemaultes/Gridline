#include "LiveRender.h"
#include <algorithm>
#include <vector>
#include "../core/Arrangements.h"

namespace gl {

Tick prepareOfflineLive(Song& s) {
  const Tick zone = s.live.areaEnd;
  if (zone <= 0) return 0;
  // Arranged content after the live area (automation alone does not count).
  Tick arrangedEnd = 0;
  bool arranged = false;
  for (const Clip& c : s.playlist)
    if (c.start >= zone && c.kind != ClipKind::Automation && !c.muted) {
      arranged = true;
      arrangedEnd = std::max(arrangedEnd, c.start + s.clipLength(c));
    }
  // Launched clips: at most one per track (the first by position).
  std::vector<int> launched;  // playlist indices
  for (size_t i = 0; i < s.playlist.size(); ++i) {
    const Clip& c = s.playlist[i];
    if (!c.launched || c.start >= zone || s.clipLength(c) <= 0) continue;
    bool taken = false;
    for (int& j : launched) {
      const Clip& o = s.playlist[static_cast<size_t>(j)];
      if (o.track != c.track) continue;
      taken = true;
      if (c.start < o.start) j = static_cast<int>(i);
    }
    if (!taken) launched.push_back(static_cast<int>(i));
  }
  if (!arranged && launched.empty()) {
    s.live.areaEnd = 0;  // nothing to launch: the live area plays as arranged
    return 0;
  }
  const Tick end = arranged ? arrangedEnd : zone + zone;
  std::vector<Clip> add;
  for (int li : launched) {
    const Clip first = s.playlist[static_cast<size_t>(li)];
    const std::vector<int> row = liveClips(s, first.track);  // by position
    const FollowMode follow =
        first.track >= 0 && static_cast<size_t>(first.track) < s.tracks.size() ? s.tracks[static_cast<size_t>(first.track)].follow : FollowMode::Loop;
    size_t at = 0;
    while (at < row.size() && row[at] != li) ++at;
    Tick t = zone;
    int idx = li;
    for (int guard = 0; t < end && guard < 100000; ++guard) {
      const Clip& src = s.playlist[static_cast<size_t>(idx)];
      const Tick len = s.clipLength(src);
      if (len <= 0) break;
      Clip k = src;
      k.id = kNoId;  // ids assigned below
      k.start = t;
      k.length = std::min(len, end - t);
      k.launched = false;
      k.muted = false;
      add.push_back(k);
      t += len;
      // What plays next on the track.
      if (follow == FollowMode::Once) break;
      if (follow == FollowMode::Loop || row.size() < 2) continue;
      if (at + 1 < row.size()) {
        ++at;
      } else if (follow == FollowMode::NextStop) {
        break;
      } else if (follow != FollowMode::NextHold) {
        at = 0;  // Next / Random / Shuffle: cycle through the track's clips in order (deterministic)
      }
      idx = row[at];
    }
  }
  // The live area itself does not play: its clips go, so the engine sees a
  // plain arrangement (a live area left in place would also keep its tracks
  // under live control).
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.start < zone; }), s.playlist.end());
  s.live.areaEnd = 0;
  for (Clip& k : add) {
    k.id = s.allocateId();
    s.playlist.push_back(k);
  }
  return zone;
}

}  // namespace gl
