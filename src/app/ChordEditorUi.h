// Gridline — chord progression editor window (J-31), attached to the piano
// roll: build a progression chord by chord (key and scale, degrees,
// inversions, voicing, bass, extensions, rhythm and strum, length per chord),
// hear it, and write it into the pattern (one undo step per session).
#pragma once

namespace gl {

class App;

namespace j31 {

void drawChordEditor(App& a);
// MIDI notes played while the editor learns a chord (App::playedNoteOn / Off).
void chordEditorNote(App& a, int pitch, bool on);

}  // namespace j31
}  // namespace gl
