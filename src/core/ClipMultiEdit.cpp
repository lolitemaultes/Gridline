// Gridline: J-142, relative edits of one clip parameter across a selection (ClipMultiEdit.h).
#include "ClipMultiEdit.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "Automation.h"
#include "ClipGain.h"
#include "Fades.h"

namespace gl::clipmulti {

namespace {

constexpr double kPitchLimit = 4800.0;  // cents, the project's range

bool isFade(Param p) { return p == Param::FadeIn || p == Param::FadeOut; }
bool isCurve(Param p) { return p == Param::FadeInCurve || p == Param::FadeOutCurve; }
bool inEdge(Param p) { return p == Param::FadeIn || p == Param::FadeInCurve; }

// Seconds of playback the clip spans.
double durationOf(const Song& s, const TempoMap& tm, const Clip& c) {
  const double a = tm.secondsAt(static_cast<double>(c.start));
  return std::max(0.0, tm.secondsAt(static_cast<double>(c.start + s.clipLength(c))) - a);
}

// The partner whose opposite edge fades over exactly the same span (a tied
// crossfade: one bend steers both, the partner mirrored).
ObjectId tiedPartner(const Song& s, const TempoMap& tm, const EdgeFade& e, bool in) {
  if (!e.crossfade || e.partner == kNoId) return kNoId;
  const Clip* p = s.clip(e.partner);
  if (!p) return kNoId;
  const ClipFades pf = resolveFades(s, tm, *p);
  const EdgeFade& o = in ? pf.out : pf.in;
  return o.len > 0 && std::fabs(o.len - e.len) < 1e-6 ? e.partner : kNoId;
}

}  // namespace

bool hasParam(const Clip& c, Param) { return c.kind == ClipKind::Audio; }

std::vector<ObjectId> targets(const Song& s, const std::set<ObjectId>& selected, ObjectId clicked, Param p) {
  std::vector<ObjectId> out;
  const Clip* c = s.clip(clicked);
  if (!c || !hasParam(*c, p)) return out;
  if (selected.count(clicked) && selected.size() > 1) {
    for (const Clip& k : s.playlist)  // playlist order: stable, independent of the set's order
      if (selected.count(k.id) && hasParam(k, p)) out.push_back(k.id);
  } else {
    out.push_back(clicked);
  }
  return out;
}

bool isMulti(const Song& s, const std::set<ObjectId>& selected, ObjectId clicked, Param p) {
  return targets(s, selected, clicked, p).size() > 1;
}

const Entry* Snapshot::find(ObjectId id) const {
  for (const Entry& e : clips)
    if (e.id == id) return &e;
  return nullptr;
}

double Snapshot::minDelta() const {
  double d = 0;
  bool any = false;
  for (const Entry& e : clips) d = any ? std::min(d, e.lo - e.base) : e.lo - e.base, any = true;
  return d;
}

double Snapshot::maxDelta() const {
  double d = 0;
  bool any = false;
  for (const Entry& e : clips) d = any ? std::max(d, e.hi - e.base) : e.hi - e.base, any = true;
  return d;
}

double valueOf(const Song& s, const Clip& c, Param p) {
  switch (p) {
    case Param::GainDb: return std::max(clipgain::kMinDb - 0.1, clipgain::toDb(c.gain));
    case Param::Pitch: return static_cast<double>(c.pitch);
    case Param::FadeIn:
    case Param::FadeOut: {
      TempoMap tm;
      tm.build(s);
      const ClipFades f = resolveFades(s, tm, c);
      return p == Param::FadeIn ? f.in.len : f.out.len;
    }
    case Param::FadeInCurve: return static_cast<double>(fadeTensionToUi(c.fadeInTension));
    case Param::FadeOutCurve: return static_cast<double>(fadeTensionToUi(c.fadeOutTension));
  }
  return 0;
}

void store(Clip& c, Param p, double v) {
  switch (p) {
    case Param::GainDb: c.gain = clipgain::fromDb(v); break;
    case Param::Pitch: c.pitch = static_cast<float>(v); break;
    case Param::FadeIn: c.fadeIn = std::max(0.0, v), c.fadeInManual = true; break;
    case Param::FadeOut: c.fadeOut = std::max(0.0, v), c.fadeOutManual = true; break;
    case Param::FadeInCurve: c.fadeInTension = fadeTensionFromUi(static_cast<float>(v)); break;
    case Param::FadeOutCurve: c.fadeOutTension = fadeTensionFromUi(static_cast<float>(v)); break;
  }
}

double defaultOf(Param) { return 0.0; }  // 0 dB, no pitch shift, no fade, plain curve

Snapshot capture(const Song& s, const std::vector<ObjectId>& ids, Param p, ObjectId anchor) {
  Snapshot snap;
  snap.param = p, snap.anchor = anchor;
  TempoMap tm;
  if (isFade(p) || isCurve(p)) tm.build(s);
  for (ObjectId id : ids) {
    const Clip* c = s.clip(id);
    if (!c || !hasParam(*c, p)) continue;
    Entry e;
    e.id = id;
    switch (p) {
      case Param::GainDb:
        e.base = valueOf(s, *c, p), e.lo = clipgain::kMinDb - 0.1, e.hi = clipgain::kMaxDb;
        break;
      case Param::Pitch:
        e.base = static_cast<double>(c->pitch), e.lo = -kPitchLimit, e.hi = kPitchLimit;
        break;
      case Param::FadeIn:
      case Param::FadeOut: {
        const ClipFades f = resolveFades(s, tm, *c);
        const bool in = p == Param::FadeIn;
        e.base = in ? f.in.len : f.out.len;
        e.lo = 0, e.hi = std::max(0.0, durationOf(s, tm, *c) - (in ? f.out.len : f.in.len));
        e.base = std::clamp(e.base, e.lo, e.hi);
        break;
      }
      case Param::FadeInCurve:
      case Param::FadeOutCurve: {
        const bool in = inEdge(p);
        e.base = valueOf(s, *c, p), e.lo = -1, e.hi = 1;
        const ClipFades f = resolveFades(s, tm, *c);
        e.mirror = tiedPartner(s, tm, in ? f.in : f.out, in);
        break;
      }
    }
    snap.clips.push_back(e);
  }
  return snap;
}

namespace {

// J-196: playlist positions by id, built once per edit (an edit of many
// clips used to scan the whole playlist for each of them).
class Index {
 public:
  explicit Index(std::vector<Clip>& clips) : clips_(clips) {
    pos_.reserve(clips.size());
    for (size_t i = 0; i < clips.size(); ++i) pos_.emplace(clips[i].id, i);
  }
  Clip* find(ObjectId id) {
    auto it = pos_.find(id);
    return it != pos_.end() ? &clips_[it->second] : nullptr;
  }

 private:
  std::vector<Clip>& clips_;
  std::unordered_map<ObjectId, size_t> pos_;
};

void writeEntry(Index& clips, const Snapshot& snap, const Entry& e, double v) {
  const bool curve = isCurve(snap.param);
  if (Clip* x = clips.find(e.id)) store(*x, snap.param, v);
  if (curve && e.mirror != kNoId) {
    // The tied partner's opposite edge mirrors the bend (FL: the pair keeps its
    // level). That edge is never this parameter, so a selected partner's own
    // entry does not collide with it.
    const float t = -fadeTensionFromUi(static_cast<float>(v));
    if (Clip* x = clips.find(e.mirror)) (snap.param == Param::FadeInCurve ? x->fadeOutTension : x->fadeInTension) = t;
  }
}

}  // namespace

void applyOffset(std::vector<Clip>& clips, const Snapshot& snap, double delta, double curveDetent) {
  if (!std::isfinite(delta)) return;
  Index index(clips);
  for (const Entry& e : snap.clips) {
    double v = std::clamp(e.base + delta, e.lo, e.hi);
    if (isCurve(snap.param) && e.id == snap.anchor && std::fabs(v) < curveDetent) v = 0;
    writeEntry(index, snap, e, v);
  }
}

void applyValue(std::vector<Clip>& clips, const Snapshot& snap, double value) {
  if (!std::isfinite(value)) return;
  Index index(clips);
  for (const Entry& e : snap.clips) writeEntry(index, snap, e, std::clamp(value, e.lo, e.hi));
}

bool offset(Document& doc, const std::string& label, const Snapshot& snap, double delta, uint64_t mergeKey, double curveDetent) {
  if (snap.clips.empty()) return false;
  return doc.editClips(label, [&](std::vector<Clip>& v) { applyOffset(v, snap, delta, curveDetent); }, mergeKey);
}

bool setAll(Document& doc, const std::string& label, const Snapshot& snap, double value, uint64_t mergeKey) {
  if (snap.clips.empty()) return false;
  return doc.editClips(label, [&](std::vector<Clip>& v) { applyValue(v, snap, value); }, mergeKey);
}

}  // namespace gl::clipmulti
