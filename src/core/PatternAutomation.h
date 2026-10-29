// Gridline — pattern automation (Pattern::automation): automation clips that
// live inside a pattern and follow it wherever it plays (J-173; plain lanes
// before that, see Song.h).
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Automation.h"
#include "Params.h"
#include "Song.h"

namespace gl {

// The clip's curve for playback (a shared copy: range, loop and curve law included).
std::shared_ptr<const AutomationClip> patternLaneCurve(const PatternAutomation& lane);
// Song mode: one lane per parameter automated by patterns placed in the
// playlist (muted clips / tracks and the live area are skipped). Each clip
// instance plays its pattern's curve from the clip's content offset.
std::vector<AutomationLane> patternAutomationLanes(const Song& song);
// Pattern mode / live launches: the lanes of one pattern as a single
// instance [0, length) starting at content tick `offset` (Q-1: `loop` > 0:
// the content repeats every `loop` ticks, one instance per pass).
std::vector<AutomationLane> singlePatternLanes(const Pattern& p, Tick length, Tick offset = 0, Tick loop = 0);
// Sets (or replaces) the points of the clip for a target; an empty point list
// removes the clip. A new clip is unnamed with range 0..1 and the earlier
// Gridline curves (the event lane, recording and imports make these).
void setPatternLane(Pattern& p, ObjectId owner, uint32_t index, std::vector<AutoPoint> points);
const PatternAutomation* findPatternLane(const Pattern& p, ObjectId owner, uint32_t index);
PatternAutomation* findPatternLane(Pattern& p, ObjectId owner, uint32_t index);

// ---- J-173: pattern automation clips -------------------------------------------
// A plain lane as projects before J-173 held it: unnamed, range 0..1, the earlier curves.
PatternAutomation patternLane(ObjectId owner, uint32_t index, std::vector<AutoPoint> points);
// The name shown for a pattern clip: its own, else its target's.
std::string patternAutoName(const Song& s, const PatternAutomation& c);
// Adds a clip (points sorted); false when the pattern already has one for the
// target or the clip has no target. `id` is cleared.
bool addPatternClip(Pattern& p, PatternAutomation c);
// Removes the clip of a target; false when there is none.
bool removePatternClip(Pattern& p, ObjectId owner, uint32_t index);
// A new pattern clip for a target holding `value` (clip units) over [0, length).
PatternAutomation newPatternClip(const Song& s, ParamId target, float value, Tick length);
// A playlist automation clip as a pattern clip: the content one placement
// plays (offset, length, loop unrolled; `placement` null: the content up to its
// last point, one loop cycle when it loops), moved to start at pattern tick `at`.
// Name, colour, range and curve law are kept.
PatternAutomation patternClipFromPlaylist(const Song& s, const AutomationClip& src, const Clip* placement, Tick at);
// A pattern clip as a playlist automation clip (id left kNoId, name resolved).
AutomationClip playlistClipFromPattern(const Song& s, const PatternAutomation& c);
// The clip's points as the parameter's normalized values (range applied: an
// affine map, so the curve is unchanged), the loop unrolled up to `until`
// (pattern ticks). What exporters write for formats without range or loops.
std::vector<AutoPoint> patternClipParamPoints(const PatternAutomation& c, Tick until);
// The same as held steps (formats whose events step, like FL's pattern events):
// segments that move with a curve are sampled every `step` ticks.
std::vector<AutoPoint> patternClipSteps(const PatternAutomation& c, Tick until, Tick step);
// Content end of the clip (its last point, or the loop length when longer).
Tick patternClipEnd(const PatternAutomation& c);

}  // namespace gl
