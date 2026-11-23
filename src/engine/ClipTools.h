// Gridline: Loupe's audio tools on playlist audio clips (the clip menus' "Audio tools" in every
// layout). The tool runs on the part of its file the clip plays (its start, end and slip, in the
// direction it plays) and the result is a new file in the project's edited-audio folder; the source
// file is never written. Only the clips asked for move to the new audio: a clip that shares its
// sample with others gets a channel of its own (the others keep the original), named
// "<name> (edited)", a clip alone on its channel keeps the channel and only the file changes. One Document edit for every clip, so undo
// brings the original back in one step and a collaboration session syncs it as one edit (the new
// files travel as other edited audio does).
//
// Tools that keep the length (all but the drum loop stretch and reverb) write the whole file with
// the clip's part processed, so the clip keeps every setting (its trim can still be opened out).
// The others write the processed part on its own, and the clip is fitted to it: its window is the
// new file and its length follows (a drum loop at a new tempo, reverb with its tail).
//
// Steps, so the UI can run the heavy part on a worker: target() on the UI thread, render() anywhere
// (reads the source, processes, writes), apply() inside Document::editSong.
#pragma once
#include <set>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "../packs/edison/edison_process.h"

namespace gl {
class Document;

namespace cliptools {

namespace ed = packs::edison;

// What a clip's tool works on.
struct Target {
  ObjectId clip = kNoId;
  ObjectId channel = kNoId;  // the channel the clip played when the target was made
  std::string file;          // the source file (the channel's sample)
  std::string name;          // the channel's name (the new file's name)
  int64_t a = 0, b = 0;      // the file frames the clip plays
  double rate = 0;           // the file's rate
  bool reversed = false;     // the clip plays them backwards (the window is turned round)
  ed::Range part;            // inside the window (frames, as heard); empty: all of it
};
// The clip's target, all of what it plays. False (with a reason) for a clip with no audio.
bool target(const Song& s, ObjectId clip, Target& t, std::string* why = nullptr);
// The same, limited to song ticks [t0, t1) of the clip (a selection in the clip editor).
bool target(const Song& s, ObjectId clip, double t0, double t1, Target& t, std::string* why = nullptr);

// The clip's window as it is heard (a reversed clip's turned round), read from the file. Null on failure.
ed::AudioPtr windowAudio(const Target& t, std::string* why = nullptr);

// A tool's settings for one clip of several, from the settings chosen on the clicked clip
// (`first`): the drum loop stretch keeps its tempo ratio, so each loop gets its own new length.
ed::ToolParams paramsFor(const ed::ToolParams& p, const Target& first, const Target& t);

struct Result {
  ObjectId clip = kNoId, channel = kNoId;
  ed::ToolKind kind = ed::ToolKind::Count;
  std::string path;          // the new file ("" on failure or when stopped)
  std::string error;
  bool part = false;         // the file holds the clip's part only (the tool changed its length)
  int64_t oldFrames = 0, newFrames = 0;  // the part's length before and after
  double rate = 0;
};
// Reads the source, runs the tool, writes the result into `dir` (named by its audio, so the same
// result is the same file). Any thread. A stopped run returns no path.
Result render(const Target& t, const ed::ToolParams& p, const std::string& dir, ed::Progress* prog = nullptr);

// Points the clip at a rendered result (inside Document::editSong). False when the clip is gone
// or plays another channel by now.
bool apply(Song& s, const Result& r);

// The name of a shared channel's copy that plays the new file: "<name> (edited)" in the interface
// language, never stacked ("Kick (edited)" stays "Kick (edited)").
std::string editedName(const std::string& name);
// The name without that ending ("Kick (edited)" is "Kick").
std::string uneditedName(const std::string& name);

// The undo label of a tool's run ("Clean up clip", "Reverse 3 clips").
std::string undoLabel(ed::ToolKind k, int clips);

// Everything at once, on the calling thread (tests, scripts): the clips of `clips` (audio clips;
// others are skipped), each with its own target (`t0` < `t1`: that range of ticks for every clip),
// rendered and applied in one undo step. Returns the clips changed; `why` the last problem.
int processClips(Document& d, const std::set<ObjectId>& clips, const ed::ToolParams& p, const std::string& dir, double t0 = 0,
                 double t1 = 0, std::string* why = nullptr);

}  // namespace cliptools
}  // namespace gl
