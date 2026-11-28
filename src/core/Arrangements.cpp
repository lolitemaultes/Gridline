#include "Arrangements.h"
#include <algorithm>
#include <utility>

namespace gl {

int arrangementCount(const Song& s) { return std::max<int>(1, static_cast<int>(s.arrangements.size())); }

std::string arrangementName(const Song& s, int index) {
  if (index < 0 || index >= arrangementCount(s)) return "";
  if (s.arrangements.empty()) return "Arrangement";
  return s.arrangements[static_cast<size_t>(index)].name;
}

void ensureArrangementList(Song& s) {
  if (!s.arrangements.empty()) {
    s.currentArrangement = std::clamp(s.currentArrangement, 0, static_cast<int>(s.arrangements.size()) - 1);
    return;
  }
  s.arrangements.push_back(Arrangement{});
  s.arrangements[0].tracks.clear();
  s.currentArrangement = 0;
}

bool selectArrangement(Song& s, int index) {
  ensureArrangementList(s);
  if (index < 0 || index >= static_cast<int>(s.arrangements.size())) return false;
  if (index == s.currentArrangement) return true;
  Arrangement& cur = s.arrangements[static_cast<size_t>(s.currentArrangement)];
  cur.playlist = std::move(s.playlist);
  cur.tracks = std::move(s.tracks);
  cur.markers = std::move(s.markers);
  cur.live = s.live;
  Arrangement& next = s.arrangements[static_cast<size_t>(index)];
  s.playlist = std::move(next.playlist);
  s.tracks = std::move(next.tracks);
  s.markers = std::move(next.markers);
  s.live = next.live;
  next.playlist.clear();
  next.tracks.clear();
  next.markers.clear();
  next.live = LiveSettings{};
  if (s.tracks.empty()) s.tracks.resize(kDefaultPlaylistTracks);
  s.currentArrangement = index;
  return true;
}

std::string uniqueArrangementName(const Song& s, const std::string& base) {
  auto used = [&](const std::string& n) {
    for (int i = 0; i < arrangementCount(s); ++i)
      if (arrangementName(s, i) == n) return true;
    return false;
  };
  if (!used(base)) return base;
  for (int k = 2;; ++k) {
    std::string n = base + " " + std::to_string(k);
    if (!used(n)) return n;
  }
}

int addArrangement(Song& s, const std::string& name, bool copyCurrent) {
  ensureArrangementList(s);
  Arrangement a;
  a.name = name.empty() ? uniqueArrangementName(s, "Arrangement") : name;
  if (copyCurrent) {
    a.playlist = s.playlist;
    for (auto& c : a.playlist) c.id = s.allocateId();  // ids stay unique project-wide
    a.tracks = s.tracks;
    a.markers = s.markers;
    a.live = s.live;
  } else {
    a.tracks.resize(kDefaultPlaylistTracks);
  }
  s.arrangements.push_back(std::move(a));
  return static_cast<int>(s.arrangements.size()) - 1;
}

bool renameArrangement(Song& s, int index, const std::string& name) {
  ensureArrangementList(s);
  if (index < 0 || index >= static_cast<int>(s.arrangements.size()) || name.empty()) return false;
  s.arrangements[static_cast<size_t>(index)].name = name;
  return true;
}

bool removeArrangement(Song& s, int index) {
  ensureArrangementList(s);
  const int n = static_cast<int>(s.arrangements.size());
  if (n <= 1 || index < 0 || index >= n) return false;
  if (index == s.currentArrangement) selectArrangement(s, index > 0 ? index - 1 : index + 1);
  s.arrangements.erase(s.arrangements.begin() + index);
  if (s.currentArrangement > index) --s.currentArrangement;
  return true;
}

bool moveArrangement(Song& s, int from, int to) {
  ensureArrangementList(s);
  const int n = static_cast<int>(s.arrangements.size());
  if (from < 0 || from >= n || to < 0 || to >= n) return false;
  if (from == to) return true;
  Arrangement item = std::move(s.arrangements[static_cast<size_t>(from)]);
  s.arrangements.erase(s.arrangements.begin() + from);
  s.arrangements.insert(s.arrangements.begin() + to, std::move(item));
  if (s.currentArrangement == from) {
    s.currentArrangement = to;
  } else if (from < s.currentArrangement && to >= s.currentArrangement) {
    --s.currentArrangement;
  } else if (from > s.currentArrangement && to <= s.currentArrangement) {
    ++s.currentArrangement;
  }
  return true;
}

Song songWithArrangement(const Song& s, int index) {
  Song copy = s;
  if (index >= 0 && index < arrangementCount(copy)) selectArrangement(copy, index);
  return copy;
}

Tick arrangementLengthTicks(const Song& s, int index) {
  const int cur = s.arrangements.empty() ? 0 : s.currentArrangement;
  if (index < 0 || index >= arrangementCount(s)) return 0;
  if (s.arrangements.empty() || index == cur) return s.lengthTicks();
  Tick end = 0;
  for (const Clip& c : s.arrangements[static_cast<size_t>(index)].playlist) {
    end = std::max(end, c.start + s.clipLength(c));
  }
  return end;
}

void forEachArrangementClip(Song& s, const std::function<void(Clip&)>& fn) {
  for (auto& c : s.playlist) fn(c);
  for (size_t i = 0; i < s.arrangements.size(); ++i)
    if (static_cast<int>(i) != s.currentArrangement)
      for (auto& c : s.arrangements[i].playlist) fn(c);
}

void forEachArrangementClip(const Song& s, const std::function<void(const Clip&)>& fn) {
  for (const auto& c : s.playlist) fn(c);
  for (size_t i = 0; i < s.arrangements.size(); ++i)
    if (static_cast<int>(i) != s.currentArrangement)
      for (const auto& c : s.arrangements[i].playlist) fn(c);
}

int pruneParkedArrangements(Song& s) {
  int removed = 0;
  for (size_t i = 0; i < s.arrangements.size(); ++i) {
    if (static_cast<int>(i) == s.currentArrangement) continue;
    auto& pl = s.arrangements[i].playlist;
    auto dead = [&](const Clip& c) {
      switch (c.kind) {
        case ClipKind::Pattern: return s.pattern(c.pattern) == nullptr;
        case ClipKind::Audio: return s.channel(c.source) == nullptr;
        case ClipKind::Automation: return s.automation(c.source) == nullptr;
      }
      return false;
    };
    size_t before = pl.size();
    pl.erase(std::remove_if(pl.begin(), pl.end(), dead), pl.end());
    removed += static_cast<int>(before - pl.size());
  }
  return removed;
}

std::vector<int> liveClips(const Song& s, int track) {
  std::vector<int> out;
  for (size_t i = 0; i < s.playlist.size(); ++i)
    if (s.playlist[i].track == track && inLiveArea(s, s.playlist[i])) out.push_back(static_cast<int>(i));
  std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
    return s.playlist[static_cast<size_t>(a)].start < s.playlist[static_cast<size_t>(b)].start;
  });
  return out;
}

Tick launchGridTicks(const Song& s, LaunchGrid g, Tick clipLength) {
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  switch (g) {
    case LaunchGrid::Now: return 0;
    case LaunchGrid::Step: return std::max<Tick>(1, beat / 4);
    case LaunchGrid::HalfBeat: return std::max<Tick>(1, beat / 2);
    case LaunchGrid::Beat: return beat;
    case LaunchGrid::TwoBeats: return beat * 2;
    case LaunchGrid::FourBeats: return beat * 4;
    case LaunchGrid::ClipLength: return std::max<Tick>(1, clipLength);
    case LaunchGrid::Bar: return std::max<Tick>(1, s.barTicks());  // grid-1118
    case LaunchGrid::TwoBars: return std::max<Tick>(1, s.barTicks()) * 2;
    case LaunchGrid::FourBars: return std::max<Tick>(1, s.barTicks()) * 4;
    case LaunchGrid::EightBars: return std::max<Tick>(1, s.barTicks()) * 8;
    case LaunchGrid::ThirtySecond: return std::max<Tick>(1, beat / 8);  // grid2-1118
    case LaunchGrid::SixteenthT: return std::max<Tick>(1, beat / 6);
    case LaunchGrid::EighthT: return std::max<Tick>(1, beat / 3);
    case LaunchGrid::QuarterT: return std::max<Tick>(1, beat * 2 / 3);
    case LaunchGrid::HalfT: return std::max<Tick>(1, beat * 4 / 3);
    default: return beat * 4;
  }
}

const char* launchGridName(LaunchGrid g) {
  switch (g) {
    case LaunchGrid::Now: return "Immediately";
    case LaunchGrid::Step: return "Next step";
    case LaunchGrid::HalfBeat: return "Next half beat";
    case LaunchGrid::Beat: return "Next beat";
    case LaunchGrid::TwoBeats: return "Next 2 beats";
    case LaunchGrid::FourBeats: return "Next 4 beats";
    case LaunchGrid::ClipLength: return "Next clip length";
    case LaunchGrid::Bar: return "Next bar";
    case LaunchGrid::TwoBars: return "Next 2 bars";
    case LaunchGrid::FourBars: return "Next 4 bars";
    case LaunchGrid::EightBars: return "Next 8 bars";
    case LaunchGrid::ThirtySecond: return "Next 1/32";  // grid2-1118
    case LaunchGrid::SixteenthT: return "Next 1/16 triplet";
    case LaunchGrid::EighthT: return "Next 1/8 triplet";
    case LaunchGrid::QuarterT: return "Next 1/4 triplet";
    case LaunchGrid::HalfT: return "Next 1/2 triplet";
    default: return "?";
  }
}

const char* padModeName(PadMode m) {
  switch (m) {
    case PadMode::Trigger: return "Trigger";
    case PadMode::Gate: return "Gate (while held)";
    case PadMode::Toggle: return "Toggle";
    case PadMode::Repeat: return "Repeat (while held)";
    default: return "?";
  }
}

const char* followModeName(FollowMode m) {
  switch (m) {
    case FollowMode::Loop: return "Loop";
    case FollowMode::Once: return "Play once";
    case FollowMode::Next: return "Next clip";
    case FollowMode::NextHold: return "Next, hold last";
    case FollowMode::NextStop: return "Next, stop after last";
    case FollowMode::Random: return "Random clip";
    case FollowMode::Shuffle: return "Random other clip";
    default: return "?";
  }
}

}  // namespace gl
