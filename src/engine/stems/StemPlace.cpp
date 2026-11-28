// Gridline - J-191: stems into the playlist (see StemPlace.h).
#include "StemPlace.h"
#include <algorithm>
#include <cmath>
#include "../../core/Automation.h"
#include "../../core/ClipRate.h"
#include "../../core/PluginInfo.h"
#include "../../core/PluginParams.h"
#include "../../core/TimeWarp.h"
#include "../ClipOps.h"
#include "../ClipSource.h"
#include "../Sample.h"
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

namespace {

std::vector<StemFile> sortedStems(const std::vector<StemFile>& in) {
  std::vector<StemFile> v = in;
  std::stable_sort(v.begin(), v.end(), [](const StemFile& a, const StemFile& b) { return stemOrder(a.source) < stemOrder(b.source); });
  return v;
}

// n tracks for the stems: inserted right below `track`; when the playlist is full, empty tracks
// further down (else the clip's own track).
std::vector<int> stemTracks(Song& s, int track, int n) {
  std::vector<int> dest;
  if (int(s.tracks.size()) < track + 1) s.tracks.resize(size_t(track) + 1);
  if (int(s.tracks.size()) + n <= kMaxPlaylistTracks) {
    const int at = track + 1;
    s.tracks.insert(s.tracks.begin() + at, size_t(n), PlaylistTrack{});
    for (auto& c : s.playlist)
      if (c.track >= at) c.track += n;
    for (int k = 0; k < n; ++k) dest.push_back(at + k);
  } else {
    for (int t = track + 1; t < int(s.tracks.size()) && int(dest.size()) < n; ++t) {
      bool used = false;
      for (const auto& c : s.playlist) used = used || c.track == t;
      if (!used) dest.push_back(t);
    }
    while (int(dest.size()) < n) dest.push_back(track);  // nowhere else: share the clip's track
  }
  return dest;
}

// One stem: its channel (a copy of `origCh` playing `st`) and its clip (`clip` on track `track`).
ObjectId addStem(Song& s, const Channel& origCh, Clip clip, const StemFile& st, const std::string& baseName, int track) {
  const std::string title = stemTitle(st.source);
  Channel ch = origCh;
  ch.id = s.allocateId();
  ch.name = baseName.empty() ? title : baseName + " - " + title;
  ch.color = stemColor(st.source);
  ch.plugin.data = st.path;
  ch.audioClip = true;
  ch.group = -1;
  s.channels.push_back(ch);
  clip.id = s.allocateId();
  clip.source = ch.id;
  clip.track = track;
  clip.takeGroup = kNoId;
  clip.take = 0;
  clip.group = kNoId;
  s.playlist.push_back(clip);
  PlaylistTrack& tr = s.tracks[size_t(track)];
  if (tr.name.empty()) {
    tr.name = title;
    tr.color = stemColor(st.source);
  }
  return clip.id;
}

void eraseClip(Song& s, ObjectId id) {
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [id](const Clip& c) { return c.id == id; }), s.playlist.end());
}

// The piece of clip `id` (in the song) in ticks [t0, t1), cut as the split tool cuts; the clip
// itself keeps the piece and the rest is removed. Returns the piece's id.
ObjectId keepPiece(Song& s, ObjectId id, Tick t0, Tick t1) {
  if (const Clip* c = s.clip(id); c && t1 < c->start + s.clipLength(*c)) {
    const ObjectId right = splitClipAt(s, id, t1);
    if (right != kNoId) eraseClip(s, right);
  }
  if (const Clip* c = s.clip(id); c && t0 > c->start) {
    const ObjectId right = splitClipAt(s, id, t0);
    if (right != kNoId) {
      eraseClip(s, id);
      return right;
    }
  }
  return id;
}

// Clip `id` cut at t0 and t1 where they fall inside it: the id of its piece in [t0, t1).
ObjectId cutOut(Song& s, ObjectId id, Tick t0, Tick t1) {
  ObjectId mid = id;
  if (const Clip* c = s.clip(id); c && t0 > c->start)
    if (const ObjectId r = splitClipAt(s, id, t0); r != kNoId) mid = r;
  if (const Clip* c = s.clip(mid); c && t1 < c->start + s.clipLength(*c)) splitClipAt(s, mid, t1);
  return mid;
}

// The stem channel plays its file as it is: the stem already holds the channel's sample shaping.
void plainSampleChannel(Channel& ch, const StemPart& part) {
  if (ch.plugin.type != "gl.sampler") return;
  const PluginInfo* info = findPlugin(ch.plugin.type);
  auto& v = ch.plugin.params;
  auto reset = [&](int k) {
    if (k >= 0 && size_t(k) < v.size()) v[size_t(k)] = info && size_t(k) < info->params.size() ? info->params[size_t(k)].def : 0.0f;
  };
  // the sampler's own loop points (fractions of the last frame) stay on the same audio
  const double last = double(std::max<int64_t>(1, part.frames - 1)), newLast = double(std::max<int64_t>(1, part.to - part.from - 1));
  for (int k : {int(pp::SmpLoopStart), int(pp::SmpLoopEnd)})
    if (size_t(k) < v.size() && part.frames > 1)
      v[size_t(k)] = float(std::clamp((double(v[size_t(k)]) * last - double(part.from)) / newLast, 0.0, 1.0));
  using namespace pp;
  for (int k : {int(SmpStart), int(SmpEnd), int(SmpNormalize), int(SmpPolarity), int(SmpRemoveDc), int(SmpSwapStereo), int(SmpFadeIn),
                int(SmpFadeOut), int(SmpPreRampIn), int(SmpPreRampOut), int(SmpPreBoost), int(SmpPreClip), int(SmpPreTilt),
                int(SmpPreCutoff), int(SmpPreReso), int(SmpPreRingMix), int(SmpPreRingFreq), int(SmpPreStereoDelay), int(SmpPreWarp),
                int(SmpPreReverbMix), int(SmpPreReverbType), int(SmpPreFadeStereo)})
    reset(k);
}

// Source second `v` of the file moved onto the stem file, which starts `frames` frames later: the
// result lands on the same place between two frames as `v` did (v·rate − frames, to the last bit
// where doubles allow), so playback starts its reads, and the stretch its stream, on the same
// frames of the same audio. (v − frames/rate rounds differently: a start on a frame would land a
// hair before it, and the stretch engines would begin one frame earlier, on a different grid.)
double movedSeconds(double v, int64_t frames, double rate) {
  const double want = v * rate - double(frames);
  double r = want / rate;
  for (int i = 0; i < 4 && r * rate != want; ++i) r = std::nextafter(r, r * rate < want ? HUGE_VAL : -HUGE_VAL);
  return r;
}

// Warp markers moved onto the stem file: markers at the part's edges (where the warp passes them)
// keep the mapping inside it exact; markers outside it go.
std::vector<WarpMarker> warpOnPart(const std::vector<WarpMarker>& m, const StemPart& part) {
  if (m.empty() || warpIsIdentity(m)) return m;
  const double a = part.startSeconds(), b = part.endSeconds();
  const std::vector<WarpPoint> w = warpPoints(m, false, 0.0);
  auto moved = [&](double at) { return movedSeconds(at, part.from, part.rate); };
  uint32_t next = 1;
  for (const WarpMarker& k : m) next = std::max(next, k.id + 1);
  std::vector<WarpMarker> out;
  out.push_back({next++, 0.0, warpTimeAt(w, a)});
  for (const WarpMarker& k : m)
    if (k.at > a + 1e-9 && k.at < b - 1e-9) out.push_back({k.id, moved(k.at), k.to});
  out.push_back({next, moved(b), warpTimeAt(w, b)});
  return out;
}

}  // namespace

std::vector<ObjectId> placeStems(Song& s, ObjectId clipId, const std::vector<StemFile>& stemsIn, const std::string& baseName,
                                 bool muteOriginal) {
  std::vector<ObjectId> made;
  const Clip* cp = s.clip(clipId);
  if (!cp || cp->kind != ClipKind::Audio || stemsIn.empty()) return made;
  const Clip orig = *cp;
  const Channel* chp = s.channel(orig.source);
  if (!chp) return made;
  const Channel origCh = *chp;
  const std::vector<StemFile> stemsSorted = sortedStems(stemsIn);
  const int n = int(stemsSorted.size());
  const std::vector<int> dest = stemTracks(s, orig.track, n);
  for (int k = 0; k < n; ++k) made.push_back(addStem(s, origCh, orig, stemsSorted[size_t(k)], baseName, dest[size_t(k)]));
  if (muteOriginal)
    if (Clip* o = s.clip(clipId)) o->muted = true;
  return made;
}

int64_t stemHandleFrames(const Song& s, const Channel& ch, const Clip& c, double rate) {
  const ClipRate r = clipRate(ch, c);
  const std::vector<WarpPoint> w = clipWarp(s, c);
  const bool stretched = !w.empty() || !r.resample || c.syncBpm > 0 || std::fabs(r.pitch - r.speed) > 1e-9;
  if (!stretched) return kStemHandleFrames;
  // the fastest the clip reads its source: its speed, through the warp's fastest section, or what the tempo makes it
  double speed = r.speed;
  if (c.syncBpm > 0) {
    TempoMap tm;
    tm.build(s);
    const double songSecPerTick = tm.secondsAt(double(c.start) + 1.0) - tm.secondsAt(double(c.start));
    if (songSecPerTick > 0) speed = 60.0 / (c.syncBpm * double(s.ppq)) / r.timeFactor / songSecPerTick;
  }
  double warpMax = 1;
  for (size_t i = 1; i < w.size(); ++i) {
    const double dw = w[i].w - w[i - 1].w, ds = w[i].s - w[i - 1].s;
    if (dw > 1e-9) warpMax = std::max(warpMax, std::fabs(ds / dw));
  }
  speed = std::clamp(std::fabs(speed) * warpMax, 1.0, 16.0);
  return std::max<int64_t>(kStemHandleFrames, int64_t(std::ceil(kStemStretchHandleSeconds * speed * rate)));
}

bool clipStemPart(const Song& s, ObjectId id, Tick t0, Tick t1, StemPart& out) {
  const Clip* c = s.clip(id);
  const Channel* ch = c && c->kind == ClipKind::Audio ? s.channel(c->source) : nullptr;
  if (!ch) return false;
  const ClipSource src = clipSource(*ch);
  if (!src.ok()) return false;
  const Tick cs = c->start, ce = c->start + s.clipLength(*c);
  const Tick a = t1 > t0 ? std::max(cs, t0) : cs, b = t1 > t0 ? std::min(ce, t1) : ce;
  if (b <= a) return false;
  TempoMap tm;
  tm.build(s);
  // the source seconds (the clip's own direction) the part hears, pass by pass
  double s0 = 0, s1 = 0;
  bool any = false;
  for (const AudibleSpan& sp : clipAudibleSpans(s, *c, tm)) {
    const double u0 = std::max(double(a), sp.t0), u1 = std::min(double(b), sp.t1);
    if (!(u1 > u0)) continue;
    const double a0 = u0 <= sp.t0 ? sp.s0 : std::clamp(clipSourceAt(s, sp.pass, tm, u0), sp.s0, sp.s1);
    const double a1 = u1 >= sp.t1 ? sp.s1 : std::clamp(clipSourceAt(s, sp.pass, tm, u1), sp.s0, sp.s1);
    s0 = any ? std::min(s0, std::min(a0, a1)) : std::min(a0, a1);
    s1 = any ? std::max(s1, std::max(a0, a1)) : std::max(a0, a1);
    any = true;
  }
  if (!any) return false;
  const double rate = src.sample->sampleRate;
  const int64_t n = int64_t(src.sample->frames());
  const bool rev = clipPlaysReversed(*ch, *c);
  // frame positions in the file's direction (a reversed clip reads frame (n - 1) - position)
  double f0 = s0 * rate, f1 = s1 * rate;
  if (rev) {
    const double last = double(n - 1);
    f0 = last - s1 * rate, f1 = last - s0 * rate;
  }
  out = StemPart{};
  out.clip = id;
  out.snapshot = *c;
  out.t0 = a, out.t1 = b;
  out.frames = n;
  out.rate = rate;
  out.reversed = rev;
  out.wholeClip = a <= cs && b >= ce;
  // whole frames outwards, and the frame after the last one, which playback interpolates towards
  out.heardFrom = std::clamp<int64_t>(int64_t(std::floor(std::min(f0, f1) + 1e-6)), 0, n);
  out.heardTo = std::clamp<int64_t>(int64_t(std::ceil(std::max(f0, f1) - 1e-6)) + 1, 0, n);
  out.handle = stemHandleFrames(s, *ch, *c, rate);
  out.from = std::max<int64_t>(0, out.heardFrom - out.handle);
  out.to = std::min<int64_t>(n, out.heardTo + out.handle);
  out.sound = src.sample;
  return out.heardTo > out.heardFrom;
}

std::vector<ObjectId> placeStems(Song& s, const StemPart& part, const std::vector<StemFile>& stemsIn, const std::string& baseName,
                                 bool muteOriginal) {
  std::vector<ObjectId> made;
  const Clip* cp = s.clip(part.clip);
  if (!cp || cp->kind != ClipKind::Audio || stemsIn.empty() || part.to <= part.from || !(part.rate > 0)) return made;
  const Channel* chp = s.channel(cp->source);
  if (!chp) return made;
  const Channel origCh = *chp;
  // the clip as the part was taken from it, where it is now
  const Tick delta = cp->start - part.snapshot.start;
  Clip base = part.snapshot;
  base.start = cp->start;
  base.track = cp->track;
  base.muted = cp->muted;
  const int track = cp->track;
  const Tick t0 = part.t0 + delta, t1 = part.t1 + delta;
  // its piece in [t0, t1), cut by the split tool on a copy
  base.id = s.allocateId();
  s.playlist.push_back(base);
  const ObjectId pieceId = keepPiece(s, base.id, t0, t1);
  Clip piece = *s.clip(pieceId);
  eraseClip(s, pieceId);
  TempoMap tm;
  tm.build(s);
  if (piece.loop) {  // a piece that does not reach its loop point plays one plain pass
    const std::vector<Clip> passes = audioLoopPasses(s, piece, tm);
    if (passes.size() == 1) piece = passes[0];
  }
  double wa = 0, wb = 0;
  if (!clipWindow(s, piece, nullptr, wa, wb)) return made;
  // the window moves onto the stem file: by the frames cut away before the part, in the clip's direction
  const int64_t shift = part.reversed ? part.frames - part.to : part.from;
  piece.audioStart = std::max(0.0, movedSeconds(wa, shift, part.rate));
  piece.audioEnd = std::max(piece.audioStart + 1e-9, movedSeconds(wb, shift, part.rate));
  piece.warp = warpOnPart(piece.warp, part);
  Channel stemCh = origCh;
  plainSampleChannel(stemCh, part);
  const std::vector<StemFile> stemsSorted = sortedStems(stemsIn);
  const int n = int(stemsSorted.size());
  const std::vector<int> dest = stemTracks(s, track, n);
  for (int k = 0; k < n; ++k) made.push_back(addStem(s, stemCh, piece, stemsSorted[size_t(k)], baseName, dest[size_t(k)]));
  if (muteOriginal)
    if (Clip* m = s.clip(cutOut(s, part.clip, t0, t1))) m->muted = true;
  return made;
}

}  // namespace gl::stems
