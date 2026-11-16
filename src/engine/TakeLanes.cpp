// Gridline — take lanes and comping (K-123). See TakeLanes.h.
#include "TakeLanes.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include "../core/Automation.h"
#include "../packs/edison/edison.h"
#include "AudioRecord.h"
#include "ClipOps.h"
#include "ClipSource.h"

namespace fs = std::filesystem;

namespace gl::takes {

namespace ed = packs::edison;

namespace {

bool onTrack(const Clip& c, int track) { return c.takeGroup != kNoId && c.track == track; }

// The ranges where `take` of `group` has clips, in order.
std::vector<std::pair<Tick, Tick>> takeSpans(const Song& song, ObjectId group, int take) {
  std::vector<std::pair<Tick, Tick>> v;
  for (const Clip& c : song.playlist)
    if (c.takeGroup == group && c.take == take) v.emplace_back(c.start, c.start + song.clipLength(c));
  std::sort(v.begin(), v.end());
  return v;
}

// A plain audio clip whose source position can be followed linearly (what loop recording makes).
bool plainAudio(const Clip& c) { return c.kind == ClipKind::Audio && !c.loop && c.warp.empty() && !c.reverse; }

}  // namespace

int laneCount(const Song& song, int track) {
  int n = 0;
  for (const Clip& c : song.playlist)
    if (onTrack(c, track)) n = std::max(n, c.take + 1);
  return n;
}

bool lanesShown(const Song& song, int track) {
  return track >= 0 && static_cast<size_t>(track) < song.tracks.size() && song.tracks[static_cast<size_t>(track)].takeLanes &&
         laneCount(song, track) > 0;
}

std::vector<ObjectId> groupsOn(const Song& song, int track) {
  std::vector<ObjectId> v;
  for (const Clip& c : song.playlist)
    if (onTrack(c, track) && std::find(v.begin(), v.end(), c.takeGroup) == v.end()) v.push_back(c.takeGroup);
  return v;
}

int joinPieces(Song& song, ObjectId group) {
  if (group == kNoId) return 0;
  TempoMap map;
  map.build(song);
  int joins = 0;
  for (bool again = true; again;) {
    again = false;
    for (size_t i = 0; i < song.playlist.size() && !again; ++i) {
      const Clip& l = song.playlist[i];
      if (l.takeGroup != group || !plainAudio(l)) continue;
      const Tick end = l.start + song.clipLength(l);
      for (size_t j = 0; j < song.playlist.size(); ++j) {
        const Clip& r = song.playlist[j];
        if (j == i || r.takeGroup != group || r.take != l.take || r.track != l.track || r.start != end || r.muted != l.muted ||
            r.source != l.source || !plainAudio(r) || r.gain != l.gain || r.pitch != l.pitch || r.stretch != l.stretch ||
            r.syncBpm != l.syncBpm || r.speedPitch != l.speedPitch || r.rateFit != l.rateFit || r.tempoLock != l.tempoLock ||
            r.fadeIn > 0 || l.fadeOut > 0)
          continue;
        if (std::fabs(clipSourceAt(song, l, map, static_cast<double>(end)) - clipSourceStart(song, r)) > 1e-6) continue;
        Clip& lw = song.playlist[i];
        lw.length = end - lw.start + song.clipLength(r);
        lw.fadeOut = r.fadeOut, lw.fadeOutManual = r.fadeOutManual, lw.fadeOutShape = r.fadeOutShape, lw.fadeOutTension = r.fadeOutTension;
        lw.audioEnd = r.audioEnd;
        song.playlist.erase(song.playlist.begin() + static_cast<std::ptrdiff_t>(j));
        ++joins;
        again = true;
        break;
      }
    }
  }
  return joins;
}

bool compRange(Song& song, int track, int take, Tick a, Tick b) {
  if (b <= a || take < 0) return false;
  bool any = false;
  for (ObjectId g : groupsOn(song, track)) {
    for (const auto& [s0, s1] : takeSpans(song, g, take)) {
      const Tick x0 = std::max(a, s0), x1 = std::min(b, s1);
      if (x1 <= x0) continue;
      const std::vector<Clip> before = song.playlist;
      compTakes(song, g, x0, x1, take);
      joinPieces(song, g);
      any = any || song.playlist != before;
    }
  }
  return any;
}

int playingTake(const Song& song, int track, Tick at) {
  for (const Clip& c : song.playlist)
    if (onTrack(c, track) && !c.muted && at >= c.start && at < c.start + song.clipLength(c)) return c.take;
  return -1;
}

bool compPick(Song& song, int track, int take, Tick at) {
  // The section: the piece playing at `at`, else the take's own clip there.
  for (const Clip& c : song.playlist)
    if (onTrack(c, track) && !c.muted && at >= c.start && at < c.start + song.clipLength(c))
      return compRange(song, track, take, c.start, c.start + song.clipLength(c));
  for (const Clip& c : song.playlist)
    if (onTrack(c, track) && c.take == take && at >= c.start && at < c.start + song.clipLength(c))
      return compRange(song, track, take, c.start, c.start + song.clipLength(c));
  return false;
}

std::vector<char> takesInComp(const Song& song, int track) {
  std::vector<char> v(static_cast<size_t>(laneCount(song, track)), 0);
  for (const Clip& c : song.playlist)
    if (onTrack(c, track) && !c.muted && c.take >= 0 && static_cast<size_t>(c.take) < v.size()) v[static_cast<size_t>(c.take)] = 1;
  return v;
}

int removeUnused(Song& song, ObjectId group) {
  if (group == kNoId) return 0;
  const size_t n = song.playlist.size();
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.takeGroup == group && c.muted; }),
                      song.playlist.end());
  return static_cast<int>(n - song.playlist.size());
}

ObjectId flatten(Song& song, ObjectId group, const std::string& dir, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return kNoId;
  };
  if (group == kNoId) return fail("no takes");
  std::vector<Clip> pieces;
  for (const Clip& c : song.playlist)
    if (c.takeGroup == group && !c.muted && c.kind == ClipKind::Audio) pieces.push_back(c);
  if (pieces.empty()) return fail("nothing plays in the comp");
  std::sort(pieces.begin(), pieces.end(), [](const Clip& x, const Clip& y) { return x.start < y.start; });
  TempoMap map;
  map.build(song);
  Tick a = pieces.front().start, b = a;
  for (const Clip& c : pieces) b = std::max(b, c.start + song.clipLength(c));
  // The file's rate and channels: the takes'.
  double sr = 0;
  bool stereo = false;
  std::vector<ClipSource> src(pieces.size());
  for (size_t i = 0; i < pieces.size(); ++i) {
    double wa = 0, wb = 0;
    clipWindow(song, pieces[i], &src[i], wa, wb);
    if (!src[i].ok()) continue;
    if (sr <= 0) sr = src[i].sample->sampleRate;
    stereo = stereo || !src[i].sample->mono();
  }
  if (sr <= 0) return fail("the takes' audio files could not be read");
  const double t0 = map.secondsAt(static_cast<double>(a));
  const auto n = static_cast<int64_t>(std::llround((map.secondsAt(static_cast<double>(b)) - t0) * sr));
  if (n <= 0) return fail("the comp is empty");
  ed::Audio out = ed::makeAudio(sr, stereo ? 2 : 1, n);
  // Crossfade at a join between two pieces: each piece reads on into its take for
  // half of it, so the two overlap and their gains sum to one.
  const int64_t half = std::max<int64_t>(1, std::llround(0.005 * sr));
  for (size_t i = 0; i < pieces.size(); ++i) {
    const Clip& c = pieces[i];
    if (!src[i].ok()) continue;
    const Sample& smp = *src[i].sample;
    const Tick end = c.start + song.clipLength(c);
    const int64_t f0 = std::llround((map.secondsAt(static_cast<double>(c.start)) - t0) * sr);
    const int64_t f1 = std::llround((map.secondsAt(static_cast<double>(end)) - t0) * sr);
    if (f1 <= f0) continue;
    const double s0 = clipSourceAt(song, c, map, static_cast<double>(c.start));
    const double s1 = clipSourceAt(song, c, map, static_cast<double>(end));
    const double perFrame = (s1 - s0) / static_cast<double>(f1 - f0);  // source seconds per output frame
    const bool joinIn = c.start > a, joinOut = end < b;
    const int64_t r0 = joinIn ? std::max<int64_t>(0, f0 - half) : f0, r1 = joinOut ? std::min(n, f1 + half) : f1;
    const float g = c.gain;
    for (int64_t f = r0; f < r1; ++f) {
      double w = 1.0;
      if (joinIn && f < f0 + half) w = std::min(w, static_cast<double>(f - (f0 - half) + 1) / static_cast<double>(2 * half + 1));
      if (joinOut && f >= f1 - half) w = std::min(w, static_cast<double>((f1 + half) - f) / static_cast<double>(2 * half + 1));
      const double pos = (s0 + perFrame * static_cast<double>(f - f0)) * smp.sampleRate;
      if (pos < 0 || pos >= static_cast<double>(smp.frames() - 1)) continue;
      const auto k = static_cast<size_t>(pos);
      const float u = static_cast<float>(pos - static_cast<double>(k));
      const float l = smp.left[k] + (smp.left[k + 1] - smp.left[k]) * u;
      const float r = smp.right[k] + (smp.right[k + 1] - smp.right[k]) * u;
      out.L[static_cast<size_t>(f)] += static_cast<float>(w) * g * l;
      if (stereo) out.R[static_cast<size_t>(f)] += static_cast<float>(w) * g * r;
    }
  }
  if (!stereo) out.R = out.L;
  const Channel* ch0 = song.channel(pieces.front().source);
  const std::string base = ch0 ? ch0->name : std::string("Take");
  // "Vox take 2" -> "Vox comp".
  std::string name = base;
  if (const size_t k = name.rfind(" take "); k != std::string::npos) name.resize(k);
  name += " comp";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = ed::renderPath(dir, cleanFileName(name), ed::audioHash(out));
  std::string werr;
  if (!ed::writeWavFile(path, ed::makeContent(out), 32, &werr)) {
    if (err) *err = werr.empty() ? "could not write " + path : werr;
    return kNoId;
  }
  Channel ch;
  ch.id = song.allocateId();
  ch.name = name;
  ch.color = ch0 ? ch0->color : 0xC8724E;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = ch0 && song.mixerTrack(ch0->mixerTrack) ? ch0->mixerTrack : song.masterId();
  song.channels.push_back(ch);
  const int track = pieces.front().track;
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.takeGroup == group; }),
                      song.playlist.end());
  Clip c;
  c.id = song.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = track;
  c.start = a;
  c.length = b - a;
  song.playlist.push_back(c);
  return c.id;
}

bool spreadOverTracks(const Song& song, ObjectId group) {
  int t = -1;
  for (const Clip& c : song.playlist) {
    if (c.takeGroup != group || group == kNoId) continue;
    if (t >= 0 && c.track != t) return true;
    t = c.track;
  }
  return false;
}

int gatherIntoLanes(Song& song, ObjectId group) {
  if (group == kNoId) return 0;
  int first = -1;
  for (const Clip& c : song.playlist)
    if (c.takeGroup == group) first = first < 0 ? c.track : std::min(first, c.track);
  if (first < 0) return 0;
  int moved = 0;
  for (Clip& c : song.playlist)
    if (c.takeGroup == group && c.track != first) c.track = first, ++moved;
  if (static_cast<int>(song.tracks.size()) <= first) song.tracks.resize(static_cast<size_t>(first) + 1);
  song.tracks[static_cast<size_t>(first)].takeLanes = true;
  return moved;
}

}  // namespace gl::takes
