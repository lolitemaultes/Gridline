// Gridline — disk recording of mixer inputs into the playlist (J-8).
//
// The audio thread captures every armed mixer track's input into that
// track's capture ring and reports take boundaries (AudioEngine::RecEvent):
// a take starts where recording starts (or the punch-in point) and ends at
// stop, the punch-out point or a loop wrap. Positions are already latency
// compensated. AudioRecorder (UI thread) drains rings and events into takes;
// commitTakes() writes them as WAV files and places them on the playlist as
// audio clips (loop passes become stacked takes on the tracks below, earlier
// passes muted). compTakes() picks the take heard in a time range.
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "AudioEngine.h"

namespace gl::packs::edison {
class CaptureRing;
}

namespace gl {

struct RecordedTake {
  ObjectId mixer = kNoId;  // armed mixer track
  double startTick = 0;    // song position of the first frame (fractional)
  int pass = 0;            // 0 = first pass of this track in the session
  bool done = false;       // the audio thread closed the take
  std::vector<float> L, R;
  int64_t frames() const { return static_cast<int64_t>(L.size()); }
};

class AudioRecorder {
 public:
  using Rings = std::vector<std::pair<ObjectId, const packs::edison::CaptureRing*>>;
  void begin();  // forgets previous takes
  // Drains take boundaries and captured frames. `rings`: the capture ring of
  // every armed mixer track (ringsOf).
  void pump(AudioEngine& engine, const Rings& rings);
  // True while some take is still open (the engine has not closed it).
  bool open() const;
  std::vector<RecordedTake>& takes() { return takes_; }
  const std::vector<RecordedTake>& takes() const { return takes_; }
  std::vector<RecordedTake> release();  // moves every take out
  double seconds(double sampleRate) const;  // longest take

 private:
  std::vector<RecordedTake> takes_;
  struct Open {
    ObjectId mixer;
    size_t take;
    size_t pos;  // next ring frame to read
  };
  std::vector<Open> open_;
  std::vector<float> bufL_, bufR_;
  std::vector<uint8_t> bufP_;
  void readInto(const packs::edison::CaptureRing& ring, Open& o, size_t until);
};

struct TakeCommitOptions {
  std::string dir;             // "<project>/Recorded"
  int bits = 32;               // 32 = float, 24 / 16 = PCM
  double sampleRate = 44100;
  bool muteEarlierTakes = true;
  // J-31 naming: clips (and empty playlist tracks) are named after the
  // recorded insert, or after its device input ("In 1 - In 2").
  enum class Naming : uint8_t { Insert = 0, Input };
  Naming naming = Naming::Insert;
  bool nameTracks = true;  // name the playlist track after the take when it has no name
};

struct TakeCommitResult {
  std::vector<ObjectId> clips;      // new audio clips, in take order
  std::vector<ObjectId> channels;   // their channels
  std::vector<std::string> files;   // written WAV files
  std::string error;
};

// J-31: silent takes. Peak of a take (linear) and removal of the takes whose
// peak stays below `thresholdDb` (dBFS). Returns how many were removed.
float takePeak(const RecordedTake& t);
int dropSilentTakes(std::vector<RecordedTake>& takes, float thresholdDb);
// Name a take gets: the insert's name or its input's name (TakeCommitOptions::Naming).
std::string takeBaseName(const Song& song, ObjectId mixer, TakeCommitOptions::Naming naming);

// Capture rings of the armed tracks in a program.
AudioRecorder::Rings ringsOf(const Program& p);

// Playlist track recorded audio of a mixer track goes to (insert N → track N).
int recordTrackFor(const Song& song, ObjectId mixer);
// Writes the takes and places them: each take becomes a sampler channel
// (routed to the recorded mixer track) and an audio clip at its position.
// Several passes of one track form a take group on consecutive tracks.
TakeCommitResult commitTakes(Song& song, const std::vector<RecordedTake>& takes, const TakeCommitOptions& o);

// Comping: within [a, b), the clips of `takeGroup` whose take is `take` play
// and the others are muted. Clips crossing a or b are split there first.
bool compTakes(Song& song, ObjectId takeGroup, Tick a, Tick b, int take);
// Take numbers present in a group (sorted).
std::vector<int> takesInGroup(const Song& song, ObjectId takeGroup);

}  // namespace gl
