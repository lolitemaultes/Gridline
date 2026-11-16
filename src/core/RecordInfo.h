// Gridline — K-123: what the Recording card and the Record window say (pure, tested).
#pragma once
#include <string>

namespace gl::recinfo {

// The card's mode line: "punch, pass 2", "loop, pass 3", "pass 1" (recording) or "punch",
// "loop", "" (ready).
std::string modeLine(bool recording, bool punch, bool loop, int pass);
// Minutes of recording left on `freeBytes` for `tracks` armed stereo tracks at `sampleRate`,
// `bytesPerSample` bytes per sample (4 for 32-bit float, 3 for 24-bit).
double minutesLeft(double freeBytes, double sampleRate, int tracks, int bytesPerSample);
// "3 h 11 min", "42 min", "under a minute".
std::string durationText(double minutes);
// A recording's length: "00:07.4", "12:03.0", "1:02:03.5".
std::string clock(double seconds);

// Where the record controls show (setting record.form): 0 the card at the bottom right, 1 a strip
// under the top bar, 2 the card, and the Record window stays open while recording.
enum Form { kCard = 0, kStrip = 1, kKeepWindow = 2 };
struct View {
  bool card = false, strip = false, pill = false, window = false;
};
// What shows this frame: `open` (record is on and the card was opened), `hidden` (folded to the
// REC pill with H), `windowOpen` (the Record window), `recording`, `started` (recording began this
// frame). With the card or the strip, the Record window folds away when a take starts; with
// kKeepWindow it stays, and the card steps aside while it shows.
View view(int form, bool open, bool hidden, bool windowOpen, bool recording, bool started);

}  // namespace gl::recinfo
