// Gridline - J-191: stems into the playlist (see StemPlace.h).
#include "StemPlace.h"
#include <algorithm>
#include "StemModels.h"

namespace gl::stems {

int stemOrder(const std::string& s) {
  static const char* order[] = {"vocals", "drums", "bass", "other", "guitar", "piano"};
  for (int i = 0; i < 6; ++i)
    if (s == order[i]) return i;
  return 6;
}

Rgb stemColor(const std::string& s) {
  switch (stemOrder(s)) {
    case 0: return 0xD8607E;  // vocals
    case 1: return 0xD89A4E;  // drums
    case 2: return 0x4E86D8;  // bass
    case 3: return 0x5EB07A;  // other
    case 4: return 0xC9B54A;  // guitar
    case 5: return 0x9A78D0;  // piano
    default: return 0x8A94A6;
  }
}

std::vector<ObjectId> placeStems(Song& s, ObjectId clipId, const std::vector<StemFile>& stemsIn, const std::string& baseName,
                                 bool muteOriginal) {
  std::vector<ObjectId> made;
  const Clip* cp = s.clip(clipId);
  if (!cp || cp->kind != ClipKind::Audio || stemsIn.empty()) return made;
  const Clip orig = *cp;
  const Channel* chp = s.channel(orig.source);
  if (!chp) return made;
  const Channel origCh = *chp;
  std::vector<StemFile> stemsSorted = stemsIn;
  std::stable_sort(stemsSorted.begin(), stemsSorted.end(),
                   [](const StemFile& a, const StemFile& b) { return stemOrder(a.source) < stemOrder(b.source); });
  const int n = int(stemsSorted.size());
  // tracks: insert n right below the clip's track; when the playlist is full, use empty tracks further down
  std::vector<int> dest;
  if (int(s.tracks.size()) < orig.track + 1) s.tracks.resize(size_t(orig.track) + 1);
  if (int(s.tracks.size()) + n <= kMaxPlaylistTracks) {
    const int at = orig.track + 1;
    s.tracks.insert(s.tracks.begin() + at, size_t(n), PlaylistTrack{});
    for (auto& c : s.playlist)
      if (c.track >= at) c.track += n;
    for (int k = 0; k < n; ++k) dest.push_back(at + k);
  } else {
    for (int t = orig.track + 1; t < int(s.tracks.size()) && int(dest.size()) < n; ++t) {
      bool used = false;
      for (const auto& c : s.playlist) used = used || c.track == t;
      if (!used) dest.push_back(t);
    }
    while (int(dest.size()) < n) dest.push_back(orig.track);  // nowhere else: share the clip's track
  }
  for (int k = 0; k < n; ++k) {
    const StemFile& st = stemsSorted[size_t(k)];
    const std::string title = stemTitle(st.source);
    Channel ch = origCh;
    ch.id = s.allocateId();
    ch.name = baseName.empty() ? title : baseName + " - " + title;
    ch.color = stemColor(st.source);
    ch.plugin.data = st.path;
    ch.audioClip = true;
    ch.group = -1;
    s.channels.push_back(ch);
    Clip c = orig;
    c.id = s.allocateId();
    c.source = ch.id;
    c.track = dest[size_t(k)];
    c.takeGroup = kNoId;
    c.take = 0;
    c.group = kNoId;
    s.playlist.push_back(c);
    made.push_back(c.id);
    PlaylistTrack& tr = s.tracks[size_t(c.track)];
    if (tr.name.empty()) {
      tr.name = title;
      tr.color = stemColor(st.source);
    }
  }
  if (muteOriginal)
    if (Clip* o = s.clip(clipId)) o->muted = true;
  return made;
}

}  // namespace gl::stems
