// Gridline — pattern automation (Pattern::automation) for playback: lanes that
// follow a pattern wherever it plays.
#pragma once
#include <memory>
#include <vector>
#include "Automation.h"
#include "Song.h"

namespace gl {

// The lane's curve as an automation clip (range 0..1, so its value is the
// parameter's normalized value).
std::shared_ptr<const AutomationClip> patternLaneCurve(const PatternAutomation& lane);
// Song mode: one lane per parameter automated by patterns placed in the
// playlist (muted clips / tracks and the live area are skipped). Each clip
// instance plays its pattern's curve from the clip's content offset.
std::vector<AutomationLane> patternAutomationLanes(const Song& song);
// Pattern mode / live launches: the lanes of one pattern as a single
// instance [0, length) starting at content tick `offset`.
std::vector<AutomationLane> singlePatternLanes(const Pattern& p, Tick length, Tick offset = 0);
// Sets (or replaces) the lane for a target; an empty point list removes it.
void setPatternLane(Pattern& p, ObjectId owner, uint32_t index, std::vector<AutoPoint> points);
const PatternAutomation* findPatternLane(const Pattern& p, ObjectId owner, uint32_t index);

}  // namespace gl
