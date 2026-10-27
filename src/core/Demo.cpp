#include "Demo.h"

#include <cstdio>

#include "Log.h"
#include "Project.h"

namespace gl {

bool loadDemoTrack(Song& out, std::string* err) {
  const std::string_view text = demoSongJson();
  Json j;
  size_t pos = 0;
  if (!jsonParse(std::string(text), j, &pos)) {
    if (err) *err = "demo song: JSON syntax error at byte " + std::to_string(pos);
    return false;
  }
  return songFromJson(j, out, err);
}

Song makeDemoTrack() {
  Song s;
  std::string err;
  if (loadDemoTrack(s, &err)) return s;
  // Never expected (the demo_song test loads it): a new empty song rather than nothing.
  std::fprintf(stderr, "[demo] the built-in demo song did not load: %s\n", err.c_str());
  logging::error("app", "The built-in demo song did not load: " + err);
  return makeEmptySong();
}

}  // namespace gl
