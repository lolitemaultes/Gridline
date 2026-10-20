// Gridline — automation clips: curve shapes and evaluation, per-parameter
// playlist lanes, and the song's tempo map (tempo automation ⇒ tick ↔ time).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "Song.h"

namespace gl {

// Maps x in [0, 1] (position inside a segment) to [0, 1] (progress from the
// previous point's value to this point's value).
double autoShape(AutoShape shape, double tension, double x);
// Clip value (0..1) at content tick t; holds the first / last point outside.
double automationValue(const AutomationClip& a, double t);
// A bare point list at tick t: `fl` = FL's exact shape laws (J-41), else the
// earlier Gridline curves (AutomationClip::flCurves).
double pointsValue(const std::vector<AutoPoint>& pts, double t, bool fl);
// FL-law progress of the segment ending at point k (smooth uses its neighbours).
double flAutoShape(const std::vector<AutoPoint>& pts, size_t k, double x);
// Inserts a point keeping the list sorted (after points at the same tick); returns its index.
int insertAutoPoint(AutomationClip& a, const AutoPoint& p);
const char* autoShapeName(AutoShape s);

// The placed clips that drive one parameter. Overlaps are resolved when the
// lane is built (a later clip cuts off an earlier one), so the instances are
// disjoint and sorted.
struct AutomationLane {
  ObjectId owner = kNoId;
  uint32_t index = 0;
  struct Instance {
    Tick start = 0, end = 0, offset = 0;
    std::shared_ptr<const AutomationClip> clip;
  };
  std::vector<Instance> inst;
  // Normalized parameter value (clip value through the clip's min/max) at a
  // song tick. Before the first clip: its start value; after a clip: its end value.
  double valueAt(double tick) const;
  bool targets(ObjectId o, uint32_t i) const { return owner == o && index == i; }
};
// One lane per automated parameter. Muted clips, clips on muted tracks and
// clips whose automation has no points are skipped.
std::vector<AutomationLane> automationLanes(const Song& song);

// Tempo over the song as piecewise-constant segments (tempo automation is
// sampled on a fine tick grid). Converts between ticks and seconds.
class TempoMap {
 public:
  struct Seg {
    double tick = 0;  // segment start
    double bpm = 120;
    double sec = 0;   // seconds from tick 0 to the segment start
  };
  TempoMap() { setConstant(120.0, kDefaultPpq); }
  void setConstant(double bpm, int ppq);
  // Song tempo, overridden by tempo automation clips when there are any.
  void build(const Song& song);
  // Builds from explicit segments (start tick, bpm); the first must start at 0.
  void setSegments(const std::vector<std::pair<double, double>>& tickBpm, int ppq);

  bool constant() const { return segs_.size() <= 1; }
  int ppq() const { return ppq_; }
  double bpmAt(double tick) const { return segs_[segAt(tick)].bpm; }
  double secondsAt(double tick) const;
  double tickAtSeconds(double sec) const;
  // Engine helpers. Within one segment these use the same arithmetic as a
  // constant tempo (samples per tick = sr * 60 / (bpm * ppq)).
  double samplesBetween(double t0, double t1, double sampleRate) const;
  double tickAfterSamples(double t0, double samples, double sampleRate) const;
  // Changes whenever the map changes (the engine re-anchors its clock).
  uint64_t signature() const { return sig_; }
  const std::vector<Seg>& segments() const { return segs_; }

 private:
  size_t segAt(double tick) const;
  size_t segAtSec(double sec) const;
  void finish();
  int ppq_ = kDefaultPpq;
  std::vector<Seg> segs_;
  uint64_t sig_ = 0;
};

}  // namespace gl
