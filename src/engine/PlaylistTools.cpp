// Gridline — playlist editing gestures (J-28). See PlaylistTools.h.
#include "PlaylistTools.h"
#include "ClipSource.h"
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
// audio (or its audioEnd), 0 when the sample is not available. K-85: inside
// the channel's start / end handles; `total` is where the audio ends (the end
// handle, in the clip's direction), `first` where it begins.
double sourceSeconds(const Song& s, const Clip& c, double* total = nullptr, double* first = nullptr) {
  const Channel* ch = s.channel(c.source);
  if (!ch) return 0;
  const ClipSource src = clipSource(*ch);
  if (!src.ok()) return 0;
  double lo = 0, hi = 0;
  clipLimits(src, clipPlaysReversed(*ch, c), lo, hi);
  if (total) *total = hi;
  if (first) *first = lo;
  double a = 0, b = 0;
  if (!clipWindow(src, clipPlaysReversed(*ch, c), c, a, b)) return 0;
  return std::max(0.0, b - a);
}

}  // namespace

double nearestZeroCrossing(const Song& s, const Clip& c, double sec, bool notAfter) {
  const Channel* ch = s.channel(c.source);
  SamplePtr smp = ch ? clipSource(*ch).sample : nullptr;  // K-85: the sound the clip plays
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

Clip makeClipLike(const Clip& brush, int track, Tick start) {
  Clip c = brush;
  c.id = kNoId;
  c.track = track;
  c.start = std::max<Tick>(0, start);
  c.muted = false;
  c.group = kNoId;
  c.takeGroup = kNoId;
  c.take = 0;
  c.launched = false;
  return c;
}

Clip makeBrushClip(const Song& s, const Source& src, const Clip* brush, int track, Tick start) {
  if (brush && sourceOf(*brush) == src && sourceValid(s, src)) return makeClipLike(*brush, track, start);
  return makeClip(s, src, track, start);
}

Tick brushLength(const Song& s, const Source& src, const Clip* brush) {
  if (brush && sourceOf(*brush) == src && sourceValid(s, src)) return std::max<Tick>(1, s.clipLength(*brush));
  return sourceLength(s, src);
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
    if (c.kind == ClipKind::Audio && !stretch && !c.loop) {  // K-114: a looping clip repeats its audio instead
      const Tick natural = std::max(audioClipNaturalLength(s, c), audioClipNaturalLength(s, fullWindow(c)));
      if (natural > 0) d = std::min<Tick>(d, std::max<Tick>(0, natural - len));
    }
  }
  std::vector<Clip> out = base;
  for (auto& c : out) {
    if (!sel.count(c.id)) continue;
    const Tick len = s.clipLength(c), nl = std::max<Tick>(1, len + d);
    if (c.kind == ClipKind::Audio && c.loop) {  // K-114: more or fewer passes; the loop itself stays (no stretch)
      c.length = nl;
      continue;
    }
    if (c.kind == ClipKind::Audio && stretch && len > 0) c.stretch = std::clamp(c.stretch * static_cast<double>(nl) / static_cast<double>(len), 0.0625, 16.0);
    c.length = nl;
    if (c.kind == ClipKind::Audio && stretch) {  // J-193: a stretched clip keeps its bars when the tempo changes
      TempoMap own;
      if (!tm) own.build(s);
      lockClipToTempo(s, c, tm ? *tm : own);
    }
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
    // J-193: a stretch locks every clip to the tempo (a resampling locked clip's pitch would follow its speed): all switch.
    if (c.kind != ClipKind::Audio || !sel.count(c.id)) continue;
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
    if (c.kind == ClipKind::Audio && c.loop) continue;  // K-114: the loop start follows the edge (wraps)
    if (c.kind == ClipKind::Audio) {
      if (stretch) continue;
      // Content may not start before the audio's beginning: the earliest
      // start whose source position is still >= the first audio (K-85: the
      // start handle; the mapping is monotonic).
      double first = 0;
      sourceSeconds(s, c, nullptr, &first);
      if (clipSourceAt(s, c, tm, static_cast<double>(c.start + d)) < first - 1e-9) {
        Tick lo = d, hi = 0;  // source(lo) < first <= source(hi)
        while (hi - lo > 1) {
          const Tick mid = lo + (hi - lo) / 2;
          (clipSourceAt(s, c, tm, static_cast<double>(c.start + mid)) < first - 1e-9 ? lo : hi) = mid;
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
    if (b.kind == ClipKind::Audio && b.loop) {
      const Tick n = audioLoopLength(s, b, tm);
      c.offset = n > 0 ? wrapLoopOffset(b.offset + d, n) : 0;
    } else if (b.kind == ClipKind::Audio) {
      if (stretch) {
        c.stretch = std::clamp(b.stretch * static_cast<double>(len - d) / static_cast<double>(std::max<Tick>(1, len)), 0.0625, 16.0);
        lockClipToTempo(s, c, tm);  // J-193
      } else {
        double first = 0;
        sourceSeconds(s, b, nullptr, &first);
        double src = std::max(first, clipSourceAt(s, b, tm, static_cast<double>(b.start + d)));
        if (zeroCross) src = nearestZeroCrossing(s, b, src);
        c.audioStart = src;
      }
    } else if (clipLoopLength(s, b) > 0) {
      c.offset = wrapClipOffset(s, b, b.offset + d);  // Q-1: the loop start follows the edge (K-140: within its own loop)
    } else {
      c.offset = std::max<Tick>(0, b.offset + d);
    }
  }
  return out;
}

Clip slipClip(const Song& s, const TempoMap& tm, const Clip& base, Tick d, bool zeroCross) {
  Clip c = base;
  if (d == 0) return c;
  if (base.kind == ClipKind::Audio && base.loop) {  // K-114: the loop's audio stays, where the passes start moves
    if (const Tick n = audioLoopLength(s, base, tm); n > 0) c.offset = wrapLoopOffset(base.offset - d, n);
    return c;
  }
  if (base.kind == ClipKind::Audio) {
    // Dragging right shows earlier audio: the source start moves back by the
    // source time the drag spans.
    const double from = clipSourceStart(s, base);
    const double span = std::fabs(clipSourceAt(s, base, tm, static_cast<double>(base.start + std::llabs(d))) - from);
    double total = 0, first = 0;
    sourceSeconds(s, base, &total, &first);
    double a = from + (d > 0 ? -span : span);
    a = std::max(first, a);  // K-85: never into audio the start / end handles cut away
    if (total > 0) a = std::min(a, std::max(first, total - 1e-3));
    if (zeroCross && a > first) a = nearestZeroCrossing(s, base, a);
    const double moved = a - from;
    c.audioStart = a;
    if (base.audioEnd > base.audioStart) c.audioEnd = std::max(a, base.audioEnd + moved);
    return c;
  }
  // Q-1: a looping pattern's content wraps (the loop start moves, the clip
  // stays); other content stops at its start and its end.
  if (clipLoopLength(s, base) > 0) {
    c.offset = wrapClipOffset(s, base, base.offset - d);
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
      if (!zeroCross || !right || right->kind != ClipKind::Audio || right->loop) continue;
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

namespace {
// A copy of `src` (a new id, appended) with `from`'s notes and steps moved to `to`.
ObjectId ownPatternCopy(Song& s, const Pattern& src, ObjectId from, ObjectId to) {
  if (static_cast<int>(s.patterns.size()) >= kMaxPatterns) return kNoId;
  Pattern p = src;
  p.id = s.allocateId();
  if (from != kNoId && to != kNoId && from != to) {
    if (auto it = p.notes.find(from); it != p.notes.end()) {
      std::vector<Note> moved = std::move(it->second);
      p.notes.erase(it);
      std::vector<Note>& dst = p.notes[to];
      dst.insert(dst.end(), moved.begin(), moved.end());
    }
    if (auto it = p.steps.find(from); it != p.steps.end()) {
      std::vector<uint8_t> moved = std::move(it->second);
      p.steps.erase(it);
      std::vector<uint8_t>& dst = p.steps[to];
      if (dst.size() < moved.size()) dst.resize(moved.size(), 0);
      for (size_t i = 0; i < moved.size(); ++i) {
        dst[i] = std::max(dst[i], moved[i]);
      }
    }
    if (auto it = p.stepProps.find(from); it != p.stepProps.end()) {
      if (!p.stepProps.count(to)) p.stepProps[to] = it->second;
      p.stepProps.erase(from);
    }
    if (auto it = p.stepLoop.find(from); it != p.stepLoop.end()) {
      if (!p.stepLoop.count(to)) p.stepLoop[to] = it->second;
      p.stepLoop.erase(from);
    }
  }
  s.patterns.push_back(std::move(p));
  return s.patterns.back().id;
}
}  // namespace

bool retargetPatternClip(Song& s, ObjectId clip, ObjectId from, ObjectId to) {
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Pattern || from == kNoId || to == kNoId || from == to) {
    return false;
  }
  const Pattern* p = s.pattern(c->pattern);
  if (!p || (!p->notes.count(from) && !p->steps.count(from))) {
    return false;
  }
  const ObjectId pid = c->pattern;
  bool shared = false;
  for (const Clip& k : s.playlist) {
    shared = shared || (k.id != clip && k.kind == ClipKind::Pattern && k.pattern == pid);
  }
  const Pattern src = *p;
  const ObjectId np = ownPatternCopy(s, src, from, to);
  if (np == kNoId) {
    return false;
  }
  if (shared) {
    for (Clip& k : s.playlist) {
      if (k.id == clip) {
        k.pattern = np;
      }
    }
    return true;
  }
  Pattern moved = std::move(s.patterns.back());  // the only region's own pattern: changed in place
  s.patterns.pop_back();
  moved.id = pid;
  for (Pattern& q : s.patterns) {
    if (q.id == pid) {
      q = std::move(moved);
      break;
    }
  }
  return true;
}

std::vector<ObjectId> pasteOwnCopies(Song& s, const std::vector<PasteItem>& items, bool noOverlap) {
  std::vector<Clip> copies;
  copies.reserve(items.size());
  for (const PasteItem& it : items) copies.push_back(it.clip);
  regroupCopies(s, copies);
  std::vector<ObjectId> out;
  for (size_t i = 0; i < copies.size(); ++i) {
    Clip c = copies[i];
    if (c.kind == ClipKind::Pattern) {
      const Pattern* p = s.pattern(c.pattern);
      if (!p) continue;
      const Pattern src = *p;
      if (const ObjectId np = ownPatternCopy(s, src, items[i].fromSound, items[i].toSound); np != kNoId) c.pattern = np;
    } else if (c.kind == ClipKind::Automation ? !s.automation(c.source) : !s.channel(c.source)) {
      continue;
    }
    c.id = s.allocateId();
    c.takeGroup = kNoId;
    s.playlist.push_back(c);
    out.push_back(c.id);
  }
  if (!noOverlap || out.empty()) return out;
  const std::set<ObjectId> placed(out.begin(), out.end());
  // K-112B (R12-01): the pasted regions keep their full length; what lies under them gives way.
  for (const ObjectId pid : out) {
    const Clip* pc = s.clip(pid);
    if (!pc) {
      continue;
    }
    const int track = pc->track;
    const bool autoKind = pc->kind == ClipKind::Automation;
    const Tick ps = pc->start, pe = pc->start + s.clipLength(*pc);
    std::set<ObjectId> drop;
    // (each step re-reads the clips: a trim or a split changes the playlist)
    for (size_t i = 0; i < s.playlist.size(); ++i) {
      const Clip o = s.playlist[i];
      if (placed.count(o.id) || drop.count(o.id) || o.track != track) {
        continue;
      }
      if ((o.kind == ClipKind::Automation) != autoKind) {
        continue;
      }
      const Tick os = o.start, oe = o.start + s.clipLength(o);
      if (oe <= ps || os >= pe) {
        continue;
      }
      if (os < ps && oe > pe) {
        sliceClips(s, {o.id}, pe);  // it runs on past the paste: the part after the paste stays as it was
        s.playlist = resizeEnds(s, s.playlist, {o.id}, ps - pe, false);  // ... and the part before ends where the paste starts
      } else if (os < ps) {
        s.playlist = resizeEnds(s, s.playlist, {o.id}, ps - oe, false);  // an earlier one ends where the pasted one starts
      } else if (oe > pe) {
        TempoMap tm;
        tm.build(s);
        s.playlist = trimStarts(s, tm, s.playlist, {o.id}, pe - os, false);  // a later one starts where the pasted one ends
      } else {
        drop.insert(o.id);  // wholly covered
      }
    }
    if (!drop.empty()) {
      s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return drop.count(c.id) > 0; }),
                       s.playlist.end());
    }
  }
  return out;
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
