// Gridline — tool windows of the piano roll and the channel rack (J-11):
// quantizer, chopper, strummer, arpeggiator, grace notes, articulation,
// randomizer, LFO, level scaling, key limit, riff generator, note properties,
// advanced step fill and step randomizer. Each works on the piano roll's
// target (current pattern, selected channel; the selected notes or all of
// them) with a live preview: changes apply as the controls move and the
// whole session is one undo step; Cancel restores the notes.
#pragma once
#include <string>
#include "../core/Types.h"

namespace gl {

class App;

enum class NoteTool { None = 0, Quantize, Chop, Strum, Arpeggiate, Flam, Articulate, Randomize, Lfo, ScaleLevels, Limit, Riff,
                      Properties, Count };
const char* noteToolTitle(NoteTool t);
void openNoteTool(App& a, NoteTool t);
bool noteToolOpen();

enum class StepTool { None = 0, Fill, Randomize };
void openStepTool(App& a, StepTool t, ObjectId channel);

// Draws the open tool window (per frame, from the piano roll / channel rack).
void drawEditTools(App& a);
// Development (--tool name): opens a tool window by name for screenshots.
void devOpenTool(App& a, const std::string& name);

}  // namespace gl
