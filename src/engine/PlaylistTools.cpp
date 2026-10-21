// Gridline — playlist editing gestures (J-28). See PlaylistTools.h.
#include "PlaylistTools.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include "ClipOps.h"
#include "../core/ClipLoop.h"
#include "../core/PluginParams.h"
#include "Sample.h"

namespace gl::pltools {

namespace {

// Seconds of source the clip has from its source start to the end of the
// audio (or its audioEnd), 0 when the sample is not available.
double sourceSeconds(const Song& s, const Clip& c, double* total = nullptr) {
  const Channel* ch = s.channel(c.source);
  if (!ch) return 0;
  SamplePtr smp = sampleCache().get(ch->plugin.data);
  if (!smp || smp->frames() < 2) return 0;
  if (total) *total = smp->seconds();
  const double end = c.audioEnd > c.audioStart ? std::min(c.audioEnd, smp->seconds()) : smp->seconds();
  return std::max(0.0, end - c.audioStart);
}

}  // namespace

double nearestZeroCrossing(const Song& s, const Clip& c, double sec, bool notAfter) {
  const Channel* ch = s.channel(c.source);
  SamplePtr smp = ch ? sampleCache().get(ch->plugin.data) : nullptr;
  if (!smp || smp->frames() < 2 || !(smp->sampleRate > 0)) return sec;
  const double sr = smp->sampleRate;
  const long n = static_cast<long>(smp->frames());
  const long at = std::clamp(static_cast<long>(std::floor(sec * sr + (notAfter ? 1e-9 : 0.5))), 0L, n - 1);
  const long reach = static_cast<long>(sr * kZeroCrossReach);
  auto v = [&](long i) { return smp->left[static_cast<size_t>(i)] + smp->right[static_cast<size_t>(i)]; };
  for (long k = 0; k <= reach; ++k)
    for (long i : {at - k, at + k}) {
      if (i <= 0 || i >= n || (notAfter && i > at)) continue;
      if (v(i) == 0.0f || (v(i - 1) < 0.0f) != (v(i) < 0.0f)) return static_cast<double>(i) / sr;
    }
  return sec;
}

bool sourceValid(const Song& s, const Source& src) {
  switch (src.kind) {
    case ClipKind::Audio: return s.channel(src.id) != nullptr;
    case ClipKind::Automation: return s.automation(src.id) != nullptr;
    default: return s.pattern(src.id) != nullptr;
  }
}

Source sourceOf(const Clip& c) { return {c.kind, c.kind == ClipKind::Pattern ? c.pattern : c.source}; }

Clip makeClip(const Song& s, const Source& src, int track, Tick start) {
  Clip c;
  c.kind = src.kind;
  c.track = track;
  c.start = std::max<Tick>(0, start);
  if (src.kind == ClipKind::Pattern) {
    c.pattern = src.id;
  } else {
    c.source = src.id;
  }
  if (src.kind == ClipKind::Audio) c.length = sourceLength(s, src);
  return c;
}

Tick sourceLength(const Song& s, const Source& src) {
  if (!sourceValid(s, src)) return s.beatTicks();
  Clip c;
  c.kind = src.kind;
  if (src.kind == ClipKind::Pattern) c.pattern = src.id;
  else c.source = src.id;
  if (src.kind == ClipKind::Audio) {
    const Tick n = audioClipNaturalLength(s, c);
    return n > 0 ? n : s.barTicks();
  }
  return std::max<Tick>(s.beatTicks(), s.clipLength(c));
}

std::vector<Clip> moveClips(const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick dt, int dtr, int trackCount,
                            const std::vector<char>& locked) {
  for (const auto& c : base)
    if (sel.count(c.id)) {
      dt = std::max<Tick>(dt, -c.start);
      dtr = std::clamp(dtr, -c.track, std::max(0, trackCount - 1 - c.track));
    }
  auto isLocked = [&](int t) { return t >= 0 && t < static_cast<int>(locked.size()) && locked[static_cast<size_t>(t)]; };
  for (const auto& c : base)
    if (sel.count(c.id) && (isLocked(c.track) || isLocked(c.track + dtr))) return base;
  std::vector<Clip> out = base;
  for (auto& c : out)
    if (sel.count(c.id)) {
      c.start += dt;
      c.track += dtr;
    }
  return out;
}

std::vector<Clip> resizeEnds(const Song& s, const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick d, bool stretch,
                             const TempoMap* tm, bool zeroCross) {
  // Every clip keeps at least one tick; audio clips without stretch stop at
  // the end of their audio (J-79: the sample's end; an end cut earlier, e.g.
  // on a zero crossing, opens up again when the clip grows past it).
  auto fullWindow = [](Clip c) {
    c.audioEnd = 0;
    return c;
  };
  for (const auto& c : base) {
    if (!sel.count(c.id)) continue;
    const Tick len = s.clipLength(c);
    d = std::max<Tick>(d, 1 - len);
    if (c.kind == ClipKind::Audio && !stretch) {
      const Tick natural = std::max(audioClipNaturalLength(s, c), audioClipNaturalLength(s, fullWindow(c)));
      if (natural > 0) d = std::min<Tick>(d, std::max<Tick>(0, natural - len));
    }
  }
  std::vector<Clip> out = base;
  for (auto& c : out) {
    if (!sel.count(c.id)) continue;
    const Tick len = s.clipLength(c), nl = std::max<Tick>(1, len + d);
    if (c.kind == ClipKind::Audio && stretch && len > 0) c.stretch = std::clamp(c.stretch * static_cast<double>(nl) / static_cast<double>(len), 0.0625, 16.0);
    c.length = nl;
    if (c.kind == ClipKind::Audio && !stretch && tm) {
      const double endSrc = clipSourceAt(s, c, *tm, static_cast<double>(c.start + nl));
      if (c.audioEnd > c.audioStart && endSrc > c.audioEnd + 1e-9) c.audioEnd = 0;  // grown past a cut end
      if (zeroCross) {
        double total = 0;
        sourceSeconds(s, fullWindow(c), &total);
        if (total > 0 && endSrc < total - 1e-6) {
          const double zc = nearestZeroCrossing(s, c, endSrc, true);
          if (zc > c.audioStart) c.audioEnd = zc;
        }
      }
    }
  }
  return out;
}

std::set<ObjectId> resamplingChannels(const Song& s, const std::vector<Clip>& clips, const std::set<ObjectId>& sel) {
  std::set<ObjectId> out;
  for (const auto& c : clips) {
    if (c.kind != ClipKind::Audio || !sel.count(c.id) || c.syncBpm > 0) continue;  // tempo-synced clips already keep the pitch
    const Channel* ch = s.channel(c.source);
    if (ch && ch->plugin.type == "gl.sampler" && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode) &&
        ch->plugin.params[pp::SmpStretchMode] < 0.5f)
      out.insert(ch->id);
  }
  return out;
}

void keepPitch(Song& s, const std::set<ObjectId>& channels) {
  for (ObjectId id : channels)
    if (Channel* ch = s.channel(id); ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode))
      ch->plugin.params[pp::SmpStretchMode] = 1.0f;
}

std::vector<Clip> trimStarts(const Song& s, const TempoMap& tm, const std::vector<Clip>& base, const std::set<ObjectId>& sel, Tick d,
                             bool stretch, bool zeroCross) {
  for (const auto& c : base) {
    if (!sel.count(c.id)) continue;
    const Tick len = s.clipLength(c);
    d = std::min<Tick>(d, len - 1);
    d = std::max<Tick>(d, -c.start);
    if (c.kind == ClipKind::Audio) {
      if (stretch) continue;
      // Content may not start before the audio's beginning: the earliest
      // start whose source position is still >= 0 (the mapping is monotonic).
      if (clipSourceAt(s, c, tm, static_cast<double>(c.start + d)) < -1e-9) {
        Tick lo = d, hi = 0;  // source(lo) < 0 <= source(hi)
        while (hi - lo > 1) {
          const Tick mid = lo + (hi - lo) / 2;
          (clipSourceAt(s, c, tm, static_cast<double>(c.start + mid)) < -1e-9 ? lo : hi) = mid;
        }
        d = hi;
      }
    } else if (clipLoopLength(s, c) <= 0) {
      d = std::max<Tick>(d, -c.offset);  // Q-1: a looping pattern clip may grow to the left (its content wraps)
    }
  }
  std::vector<Clip> out = base;
  for (auto& c : out) {
    if (!sel.count(c.id)) continue;
    const Clip b = c;
    const Tick len = s.clipLength(b);
    c.start = b.start + d;
    c.length = len - d;
    if (b.kind == ClipKind::Audio) {
      if (stretch) {
        c.stretch = std::clamp(b.stretch * static_cast<double>(len - d) / static_cast<double>(std::max<Tick>(1, len)), 0.0625, 16.0);
      } else {
        double src = std::max(0.0, clipSourceAt(s, b, tm, static_cast<double>(b.start + d)));
        if (zeroCross) src = nearestZeroCrossing(s, b, src);
        c.audioStart = src;
      }
    } else if (const Tick loop = clipLoopLength(s, b); loop > 0) {
      c.offset = wrapLoopOffset(b.offset + d, loop);  // Q-1: the loop start follows the edge
    } else {
      c.offset = std::max<Tick>(0, b.offset + d);
    }
  }
  return out;
}

Clip slipClip(const Song& s, const TempoMap& tm, const Clip& base, Tick d, bool zeroCross) {
  Clip c = base;
  if (d == 0) return c;
  if (base.kind == ClipKind::Audio) {
    // Dragging right shows earlier audio: the source start moves back by the
    // source time the drag spans.
    const double span = std::fabs(clipSourceAt(s, base, tm, static_cast<double>(base.start + std::llabs(d))) - base.audioStart);
    double total = 0;
    sourceSeconds(s, base, &total);
    double a = base.audioStart + (d > 0 ? -span : span);
    a = std::max(0.0, a);
    if (total > 0) a = std::min(a, std::max(0.0, total - 1e-3));
    if (zeroCross && a > 0) a = nearestZeroCrossing(s, base, a);
    const double moved = a - base.audioStart;
    c.audioStart = a;
    if (base.audioEnd > base.audioStart) c.audioEnd = std::max(a, base.audioEnd + moved);
    return c;
  }
  // Q-1: a looping pattern's content wraps (the loop start moves, the clip
  // stays); other content stops at its start and its end.
  if (const Tick loop = clipLoopLength(s, base); loop > 0) {
    c.offset = wrapLoopOffset(base.offset - d, loop);
    return c;
  }
  Tick hi = std::numeric_limits<Tick>::max() / 4;
  if (base.kind == ClipKind::Pattern)
    if (const Pattern* p = s.pattern(base.pattern)) hi = std::max<Tick>(0, s.patternContentLength(*p) - 1);
  c.offset = std::clamp<Tick>(base.offset - d, 0, std::max(hi, base.offset));
  return c;
}

std::vector<ObjectId> clipsAcross(const Song& s, Tick at, int t0, int t1) {
  if (t0 > t1) std::swap(t0, t1);
  std::vector<ObjectId> out;
  for (const auto& c : s.playlist)
    if (c.track >= t0 && c.track <= t1 && at > c.start && at < c.start + s.clipLength(c)) out.push_back(c.id);
  return out;
}

std::vector<ObjectId> sliceClips(Song& s, const std::vector<ObjectId>& ids, Tick at, bool zeroCross) {
  std::vector<ObjectId> out;
  for (ObjectId id : ids)
    if (ObjectId r = splitClipAt(s, id, at); r != kNoId) {
      out.push_back(r);
      Clip* right = s.clip(r);
      if (!zeroCross || !right || right->kind != ClipKind::Audio) continue;
      const double zc = nearestZeroCrossing(s, *right, right->audioStart, true);  // the left part ends on it
      if (Clip* left = s.clip(id); left && zc > left->audioStart) left->audioEnd = zc;
      if ((right = s.clip(r))) {
        if (right->audioEnd > 0 && right->audioEnd <= zc) continue;
        right->audioStart = zc;
      }
    }
  return out;
}

std::vector<Tick> paintSlots(Tick anchor, Tick to, Tick len) {
  std::vector<Tick> out;
  if (len <= 0) return out;
  const Tick k = (to - anchor) >= 0 ? (to - anchor) / len : -((anchor - to + len - 1) / len);
  const Tick lo = std::min<Tick>(0, k), hi = std::max<Tick>(0, k);
  for (Tick i = lo; i <= hi; ++i)
    if (anchor + i * len >= 0) out.push_back(anchor + i * len);
  return out;
}

bool occupied(const Song& s, int track, Tick start, Tick len) {
  for (const auto& c : s.playlist)
    if (c.track == track && start < c.start + s.clipLength(c) && start + len > c.start) return true;
  return false;
}

void regroupCopies(Song& s, std::vector<Clip>& copies) {
  std::map<ObjectId, ObjectId> fresh;
  for (Clip& c : copies) {
    if (c.group == kNoId) continue;
    auto [it, added] = fresh.emplace(c.group, kNoId);
    if (added) it->second = s.allocateId();
    c.group = it->second;
  }
}

std::vector<ObjectId> cloneClips(Song& s, const std::set<ObjectId>& sel) {
  std::vector<Clip> copies;
  for (const auto& c : s.playlist)
    if (sel.count(c.id)) copies.push_back(c);
  regroupCopies(s, copies);
  std::vector<ObjectId> out;
  for (Clip c : copies) {
    c.id = s.allocateId();
    c.takeGroup = kNoId;
    out.push_back(c.id);
    s.playlist.push_back(c);
  }
  return out;
}

ObjectId topClipAt(const Song& s, int track, Tick t) {
  ObjectId best = kNoId;
  Tick bestStart = -1;
  for (const auto& c : s.playlist)  // later start wins; equal starts: later in the playlist
    if (c.track == track && t >= c.start && t < c.start + s.clipLength(c) && c.start >= bestStart) best = c.id, bestStart = c.start;
  return best;
}

bool EraseSweep::covers(int track, Tick t) const {
  for (const Span& sp : spans)
    if (sp.track == track && t >= sp.start && t < sp.end) return true;
  return false;
}

void EraseSweep::add(const Song& s, const Clip& c) { spans.push_back({c.track, c.start, c.start + s.clipLength(c)}); }

ObjectId EraseSweep::pick(const Song& s, int track, Tick t, ObjectId hit) const {
  if (covers(track, t)) return kNoId;
  return hit != kNoId ? hit : topClipAt(s, track, t);
}

bool moveTrack(Song& s, int from, int to) {
  const int n = static_cast<int>(s.tracks.size());
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
  PlaylistTrack t = s.tracks[static_cast<size_t>(from)];
  s.tracks.erase(s.tracks.begin() + from);
  s.tracks.insert(s.tracks.begin() + to, t);
  for (auto& c : s.playlist) {
    if (c.track == from) c.track = to;
    else if (from < to && c.track > from && c.track <= to) --c.track;
    else if (from > to && c.track >= to && c.track < from) ++c.track;
  }
  return true;
}

}  // namespace gl::pltools
