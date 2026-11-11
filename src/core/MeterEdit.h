// Gridline — editing the song's meter changes and patterns' own meters (one undo step each).
// A meter change is a marker with a time signature (Song::markers); see core/Meter.h.
#pragma once
#include <string>
#include "Document.h"
#include "Meter.h"

namespace gl::meter {

// Index of the meter marker in effect at t (-1: the song's meter at the start).
int changeAt(const Song& s, Tick t);
// Index of the meter marker exactly at t (-1: none).
int changeExactlyAt(const Song& s, Tick t);
// Number of meter changes in the current arrangement.
int changeCount(const Song& s);
// The meter in effect at t.
void meterAt(const Song& s, Tick t, int& num, int& den);

// Adds a meter change at the bar line at or before `pos` (`free`: exactly at pos). A meter marker
// already there is changed instead. Returns its marker index (-1 if nothing changed).
int add(Document& d, Tick pos, int num, int den, bool free = false);
// Changes a meter marker's meter. False if `index` is not a meter marker.
bool set(Document& d, int index, int num, int den);
// Moves a meter change to the bar line nearest `pos` in the map without it (`free`: exactly to pos).
// Returns the marker's new index (-1 if not moved).
int move(Document& d, int index, Tick pos, bool free = false);
// Removes a meter change: a marker that also has a name, action or key keeps those and loses only
// its meter.
bool remove(Document& d, int index);
// Removes meter changes that share a tick with a later one (the later one wins). Returns how many.
int removeDuplicates(Document& d);
// Sets whether patterns made in this meter section get its meter.
bool setForPatterns(Document& d, int index, bool on);
// The meter a new pattern made at `pos` gets: the section's meter when it differs from the song's
// meter at the start and its change says so, else 0/0 (follows the song). Also returns the steps
// per bar that keep a step a sixteenth (`steps`, 16 when it follows the song).
void newPatternMeter(const Song& s, Tick pos, int& num, int& den, int& steps);

// A pattern's own meter (num 0 = follows the song). Its bars stay, so its length follows; a
// pattern without steps also gets the steps per bar that keep a step a sixteenth.
bool setPatternMeter(Document& d, ObjectId pattern, int num, int den);
// Gives a pattern the meter of the section at `at`. With `keepLength` its tick length stays when it
// is a whole number of the new bars (returns false and changes nothing when it is not).
bool matchPatternMeter(Document& d, ObjectId pattern, Tick at, bool keepLength = true);

// Text for a meter: "6/8".
std::string text(int num, int den);

}  // namespace gl::meter
