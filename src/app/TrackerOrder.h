// Gridline - K-142 (0.7.8): the Tracker's order list (trk::orderList), worked out once per change
// of the song instead of several times a frame. orderList compares every slice of the song with
// every clip (a large project's 2000 clips: about 9 ms a frame and 35000 allocations, idle).
// The list is the one trk::orderList gives; it is made again when the song changes (any edit,
// undo, redo or another project: Document::revision and projectEpoch).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "../core/Document.h"
#include "../core/TrackerGrid.h"

namespace gl::trackerorder {

using List = std::shared_ptr<const std::vector<trk::OrderEntry>>;

// The order list of `s`: cached when `s` is the song of `doc`; any other song (a copy being
// edited) is worked out as before. A list held stays valid (unchanged) after later edits.
inline List ofDoc(const Document& doc, const Song& s) {
  if (&s != &doc.song()) return std::make_shared<const std::vector<trk::OrderEntry>>(trk::orderList(s));
  static List cached;
  static const Document* owner = nullptr;
  static uint64_t rev = 0, epoch = 0;
  if (!cached || owner != &doc || rev != doc.revision() || epoch != doc.projectEpoch()) {
    cached = std::make_shared<const std::vector<trk::OrderEntry>>(trk::orderList(s));
    owner = &doc, rev = doc.revision(), epoch = doc.projectEpoch();
  }
  return cached;
}
// (App: anything with a `doc`; a template so this header needs no App.h)
template <class AppT>
List of(const AppT& a, const Song& s) {
  return ofDoc(a.doc, s);
}
template <class AppT>
List of(const AppT& a) {
  return ofDoc(a.doc, a.doc.song());
}

}  // namespace gl::trackerorder
