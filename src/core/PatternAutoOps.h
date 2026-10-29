// Gridline — J-173: moving automation clips from the playlist into patterns
// (each one undo step). The other way: ops::laneToAutomationClip (MenuParity.h).
#pragma once
#include "Document.h"
#include "Params.h"

namespace gl::patauto {

enum class MoveResult { Done, NoSource, NoPattern, TargetTaken, NoTarget };

// A playlist automation clip placement moved into a pattern: the content it
// plays becomes the pattern's clip for that control, at the place the
// placement sits over a playlist clip of the pattern (else at the pattern
// start); the placement goes, and the automation clip with it when nothing
// else places it. `keep`: a copy, the placement stays.
MoveResult placementIntoPattern(Document& doc, ObjectId placement, ObjectId pattern, bool keep);
// An automation clip's content (no placement) copied into a pattern at pattern tick `at`.
MoveResult automationIntoPattern(Document& doc, ObjectId automation, ObjectId pattern, Tick at);
// Where a song tick lands in a pattern placed under it (-1: no placement of
// the pattern covers the tick).
Tick patternTickAt(const Song& s, ObjectId pattern, Tick songTick);
// A short message for a result that did nothing ("" for Done).
const char* moveResultText(MoveResult r);

}  // namespace gl::patauto
