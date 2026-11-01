// Gridline — J-217: audio being recorded into the playlist, drawn live.
//
// While a recording session runs, every take the recorder holds is shown on
// the playlist as the clip it will become (AudioRecord.h takePlacement /
// takeSpan: the same track, lane, start, length, name and colour as
// commitTakes() places), its waveform summarised incrementally as the
// recorder drains frames (PeakSummary: cost per frame proportional to the new
// frames only, memory bounded). When the takes are committed, each summary is
// handed to the committed clip's waveform (keyed by its file), so the clip
// draws the same pixels from the first frame, before its file has decoded,
// and never rescans an hour-long take. UI thread only.
#pragma once
#include <string>
#include <vector>
#include "../engine/AudioRecord.h"
#include "../engine/PeakSummary.h"
#include "../engine/Sample.h"

namespace gl {
class App;

namespace livetake {

struct View {
  size_t take = 0;            // index into the recorder's takes
  ObjectId mixer = kNoId;
  TakePlacement place;        // track, name, colour, muted (as committed)
  TakeSpan span;              // start / length in ticks, leading pad (as committed)
  int64_t fileFrames = 0;     // pad + frames recorded so far (the committed file's length)
  double sampleRate = 44100;
  bool done = false;          // the engine closed the take
  const PeakSummary* peaks = nullptr;
  // Drawn this frame (Playlist; dev trace): the clip rectangle, 0 when not drawn.
  bool drawn = false;
  float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

// Each frame of a session, after the recorder drained (AudioRecordUi).
void update(App& a);
// Forgets the live takes (no session, or the takes were committed).
void reset();
// Every take is closed and has been shown complete for a frame: committing now
// shows exactly what the last live frame showed.
bool settled();
std::vector<View>& views();
// The committed takes (filtered as commitTakes() received them) and its result:
// their summaries move to the committed clips' files.
void handoff(const std::vector<RecordedTake>& takes, const TakeCommitResult& r);

// A committed take's summary, while its file is not decoded yet (nullptr: none).
struct Handoff {
  PeakSummary peaks;
  int64_t frames = 0;
  double sampleRate = 44100;
};
const Handoff* pending(const std::string& path);
// The playlist's waveform summary of a decoded sample (cached per sample; a
// committed take's live summary is adopted instead of rescanning the file).
const PeakSummary& samplePeaks(const SamplePtr& s);

// Dev trace (--dev-input "livetakes"): one line per live take.
void devTrace(App& a, int frame);
// Dev check (--dev-input "clippeaks"): the committed audio clips' summaries as
// drawn and rescanned from their files.
void devClipPeaks(App& a, int frame);
// --dev-input "liverec <arm|select|start|stop|trace|clips> ..." (LiveTakes.cpp). True when handled.
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace livetake
}  // namespace gl
