// Gridline — J-211: the track icon set shared by mixer tracks, playlist
// tracks and channels. Each icon is a Gridline original (assets/icons/ui/
// track-<svg>.svg); ids are stable (saved in projects: 0 = none, 1..16 are the
// first set). Each icon also carries the number FL Studio's icon list uses for
// the same meaning, so FL projects open with their icons and export with them.
#pragma once

namespace gl::trackicon {

enum Category {
  Drums, Percussion, Keys, Synths, Strings, Wind, Vocals, Samples, Notation, Effects, Mixing, Surround, Gear, Symbols,
  Numbers, Faces, CategoryCount
};

struct Info {
  int id;                // Gridline icon id (MixerTrack / PlaylistTrack / Channel ::icon)
  const char* svg;       // SVG file stem (assets/icons/ui)
  const char* name;      // English name (translated where shown)
  Category category;
  int fl;                // FL Studio icon number (0 = none)
  const char* keywords;  // extra search words
};

// Icons in picker order (by category); `count` receives their number.
const Info* all(int* count);
// The icon `id`, or nullptr (0 / unknown).
const Info* info(int id);
// Highest icon id.
int maxId();
// Category name (English, marked for translation).
const char* categoryName(int category);

// FL interop. The Gridline icon showing FL's icon `flNumber` (0 = none or no
// matching icon). toFl: the FL number to write for Gridline icon `id` (0 = none).
int fromFl(int flNumber);
int toFl(int id);
// The FL number to export for an item with Gridline icon `icon` and kept FL
// number `flIcon`: the kept number while it still shows that icon or no icon is set (exact round
// trip, also for FL numbers Gridline has no icon for), else the icon's own.
int exportFl(int icon, int flIcon);

}  // namespace gl::trackicon
