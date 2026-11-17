// Gridline — disk recording of mixer inputs into the playlist (J-8).
//
// The audio thread captures every armed mixer track's input into that
// track's capture ring and reports take boundaries (AudioEngine::RecEvent):
// a take starts where recording starts (or the punch-in point) and ends at
// stop, the punch-out point or a loop wrap. Positions are already latency
// compensated. AudioRecorder (UI thread) drains rings and events into takes;
// commitTakes() writes them as WAV files and places them on the playlist as
// audio clips (loop passes become take lanes on the track, or stacked takes on
// the tracks below, earlier passes muted). compTakes() picks the take heard in a time range.
#pragma once
#include <functional>
#include <memory>
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
  // K-123 (0.7.7): punches. Whether the take starts / ends at a punch (RecEvent::punch), its frames
  // in the track's capture-while-armed ring, the punch points' song positions, and the frames added
  // before and after them for the crossfades (AudioRecorder::setPunchPads).
  bool punchIn = false, punchOut = false;
  size_t armStart = 0, armEnd = 0;
  double punchInTick = -1, punchOutTick = -1;
  int64_t padBefore = 0, padAfter = 0;
  bool padInDone = false, padOutDone = false;
};

class ArmedRing;
// K-123 (0.7.7): the capture-while-armed ring of every armed track in a program.
using ArmRings = std::vector<std::pair<ObjectId, std::shared_ptr<ArmedRing>>>;
ArmRings armRingsOf(const Program& p);

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
  // K-123 (0.7.7) quick punch: punches further on in the song belong to the same pass (they are
  // pieces of one take, not loop passes); a punch at or before the last one starts the next pass.
  void setQuickPunch(bool on) { quickPunch_ = on; }
  // K-123 (0.7.7) punch crossfades: a take that starts at a punch gets, as it starts, the audio from
  // half the crossfade before the punch in (reaching back to a whole tick) out of its track's
  // capture-while-armed ring, where the transport played on into the punch (not after a loop
  // wrap); a take that ends at a punch gets half the crossfade after the punch out the same way
  // (open() waits for it). The punch points' positions are kept on the take. fadeSeconds 0: none.
  void setPunchPads(ArmRings rings, double fadeSeconds, double sampleRate);

 private:
  std::vector<RecordedTake> takes_;
  struct Open {
    ObjectId mixer;
    size_t take;
    size_t pos;  // next ring frame to read
  };
  std::vector<Open> open_;
  bool quickPunch_ = false;
  ArmRings armRings_;
  double padHalf_ = 0;  // frames: half the punch crossfade
  void padIn(RecordedTake& t);
  bool padOut(RecordedTake& t, bool last);  // true once done
  std::vector<float> bufL_, bufR_;
  std::vector<uint8_t> bufP_;
  void readInto(const packs::edison::CaptureRing& ring, Open& o, size_t until);
};

struct TakeCommitOptions {
  std::string dir;             // "<project>/Recorded"
  int bits = 32;               // 32 = float, 24 / 16 = PCM
  double sampleRate = 44100;
  bool muteEarlierTakes = true;
  // K-123: loop passes of one track land on that track as take lanes (engine/TakeLanes.h);
  // off, they stack on the tracks below (the earlier layout).
  bool lanes = true;
  // J-31 naming: clips (and empty playlist tracks) are named after the
  // recorded insert, or after its device input ("In 1 - In 2").
  enum class Naming : uint8_t { Insert = 0, Input };
  Naming naming = Naming::Insert;
  bool nameTracks = true;  // name the playlist track after the take when it has no name
  // K-123 (0.7.7): punch crossfade (seconds; 0 = off). A take that starts or ends at a punch
  // (padded by padPunchTakes) replaces the audio clips already on its playlist track over its span,
  // and at each punch the take and that audio crossfade: the take fades in where the old part fades
  // out, over the crossfade around the punch point, on whole ticks. Clip fades set by hand.
  double punchFade = 0;
};

// Recovery (capture while armed): the newest stretch the transport played, at least `minSeconds`
// long, of every ring, as takes placed where they were played (one per track). Empty when none.
std::vector<RecordedTake> recoverLastPass(const ArmRings& rings, double sampleRate, double minSeconds = 0.5);

struct TakeCommitResult {
  std::vector<ObjectId> clips;      // new audio clips, in take order
  std::vector<size_t> takeIndex;    // J-217: the take (index into `takes`) of each clip
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
// J-217: where and how a take lands on the playlist, shared by commitTakes()
// and the playlist's live drawing of takes still recording (so the growing
// clip is the clip that will be placed). `takes`: the session's takes so far;
// takes without frames are ignored, as commitTakes() does.
struct TakePlacement {
  int track = 0;          // playlist track
  bool grouped = false;   // the mixer track recorded several passes (a take group)
  int take = 0;           // take number within the group
  bool muted = false;     // an earlier pass (with muteEarlierTakes)
  bool lane = false;      // K-123: a pass in the track's take lanes (grouped, TakeCommitOptions::lanes)
  bool stereo = true;     // the clip's file is stereo
  std::string name;       // clip / channel name
  Rgb color = 0xC8724E;   // channel colour
};
TakePlacement takePlacement(const Song& song, const std::vector<RecordedTake>& takes, size_t i, const TakeCommitOptions& o);
// The clip of a take of `frames` frames starting at song tick `startTick`: it
// starts on the tick at or before the take (the gap is `pad` frames of leading
// silence in its file) and lasts `length` ticks (whole ticks covering the file).
struct TakeSpan {
  Tick start = 0, length = 1;
  int64_t pad = 0;
};
TakeSpan takeSpan(const TempoMap& map, double startTick, int64_t frames, double sampleRate);

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
