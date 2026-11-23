// Gridline — the guide to the built-in instruments (GitHub #73): for every instrument, what kind of
// sound engine it is and what it is good for, in one short line each. The instrument menus and the
// plugin picker show the line beside each instrument and link to the full page
// (docs/instruments.md), which lists the same instruments in the same order with a picture each.
// A test keeps the three in step: every instrument the pickers offer has an entry here and a
// section in the page.
#pragma once
#include <string>
#include <vector>

namespace gl::guide {

struct InstrumentEntry {
  const char* type;     // "gl.*"
  const char* group;    // the page's section ("Flagships", "Subtractive and virtual analog", ...)
  const char* engine;   // the kind of sound engine, a few words
  const char* goodFor;  // what it is good for, a few words
};

// Every entry, in the page's order: the flagships first (Plasma, Seltmann, Platter, Tideline,
// Chater, Loupe; the last three are effects that sit beside the instruments), then the rest by
// kind of engine.
const std::vector<InstrumentEntry>& instruments();
// The entry of `type`, or nullptr.
const InstrumentEntry* find(const std::string& type);
// "<engine>. Good for <goodFor>." (translated), or "" for a type without an entry.
std::string shortLine(const std::string& type);
// The page on the web (opened by the pickers' guide links).
const char* pageUrl();

}  // namespace gl::guide
