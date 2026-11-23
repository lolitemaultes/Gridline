// Gridline: finding the silence in audio, and taking it out (K-253).
//
// One engine for both silence tools, wherever they show (the playlist's audio clips, Loupe,
// Kerf, the Sampler and every channel that plays a sample):
//   Trim silence at the ends   the audio starts at its first sound and ends after its last one;
//   Remove silence             every silence (a pause longer than the shortest gap) goes too, the
//                              parts that are left join up with short fades (in the playlist the
//                              clip is split there, and Close the gaps moves the parts together).
// Both find sound the same way and keep the same margin around it, so a trim is exactly what a
// remove keeps at the two ends.
//
// How sound is found: the audio's level in 10 ms windows (RMS, the louder channel) against a
// threshold, the noise floor. Automatic, the threshold sits just above the audio's own quiet
// level (its 10th percentile window, never closer than 18 dB under its median window) and
// between -80 and -30 dBFS. Parts of sound shorter than 15 ms are clicks and count as silence.
// Keep breaths on, a quiet short sound between two silences (a breath, a page turn) stays; off,
// a sound under 1 s that never comes within `breathDb` of the audio's loud level (its 95th
// percentile window) counts as silence. Pauses shorter than `minGapMs` stay (they belong to the
// phrasing), and `marginMs` of audio stays before and after every part.
#pragma once
#include <cstdint>
#include <vector>
#include "../core/Song.h"
#include "../packs/edison/edison.h"

namespace gl::silence {

struct Options {
  bool autoThreshold = true;  // the threshold from the audio's own noise floor
  double thresholdDb = -50;   // with autoThreshold off: quieter than this is silence (dBFS)
  double minGapMs = 300;      // shorter pauses stay
  double marginMs = 25;       // audio kept before and after each part with sound
  double fadeMs = 5;          // fades at every cut (none at the file's own ends)
  bool keepBreaths = true;    // quiet short sounds between silences stay
  double breathDb = 24;       // keepBreaths off: a short sound this far under the loud level is a breath
  bool ripple = false;        // playlist Remove: the parts move together (and the clips after them on the track)
  bool operator==(const Options& o) const {
    return autoThreshold == o.autoThreshold && thresholdDb == o.thresholdDb && minGapMs == o.minGapMs && marginMs == o.marginMs &&
           fadeMs == o.fadeMs && keepBreaths == o.keepBreaths && breathDb == o.breathDb && ripple == o.ripple;
  }
};

struct Span {
  int64_t a = 0, b = 0;  // frames [a, b)
  int64_t len() const { return b - a; }
  bool operator==(const Span& o) const { return a == o.a && b == o.b; }
};

// The level the threshold sits at (dBFS) for this audio: the Options' own or the automatic one.
double thresholdFor(const float* L, const float* R, int64_t n, double rate, const Options& o);
// The parts with sound (margins added, short pauses closed up), sorted, apart; empty when all of
// it is silence. L and R may be the same channel.
std::vector<Span> soundSpans(const float* L, const float* R, int64_t n, double rate, const Options& o);
// What a trim keeps: from the first part's start to the last part's end (empty when silent).
Span trimSpan(const float* L, const float* R, int64_t n, double rate, const Options& o);
std::vector<Span> soundSpans(const packs::edison::Audio& a, const Options& o);
Span trimSpan(const packs::edison::Audio& a, const Options& o);
// Where frame `f` of the audio lands after only `keep` stays (a frame in a removed part lands
// where the next kept part starts; past the last one, at the end).
int64_t mapFrame(const std::vector<Span>& keep, int64_t f);
// Frames that stay after only `keep` stays.
int64_t keptFrames(const std::vector<Span>& keep);

// ---- audio (Loupe, instruments): new versions, markers and regions moved with the audio ----
// The ends trimmed (fades at a cut inside sound). Unchanged when there is nothing to trim; `kept`
// receives what stayed (empty: the audio was all silence and nothing is left).
packs::edison::Content trimEnds(const packs::edison::Content& c, const Options& o, Span* kept = nullptr);
// Every silence removed, the parts joined with `fadeMs` fades. `keep` receives the parts kept.
packs::edison::Content removeSilence(const packs::edison::Content& c, const Options& o, std::vector<Span>* keep = nullptr);

// ---- playlist audio clips (song edits: wrap in Document::editSong for one undo step) ----
// Why a clip cannot be edited ("" when it can): not an audio clip, a looping clip, no audio yet.
const char* clipProblem(const Song& song, const Clip& c);
// The clip's start and end moved in to its first and last sound (the clip is cut there, so its
// audio plays where it played). False when there is nothing to trim (or the clip is all silence:
// then it is left as it is).
bool trimClip(Song& song, ObjectId clip, const Options& o);
// The clip cut at its silences and the silent parts taken out, a fade at every cut; with
// o.ripple the parts move left to close the gaps and every later clip on its track moves left by
// the time taken out. Returns the parts left (the clip's own id first), empty when unchanged.
std::vector<ObjectId> removeClipSilence(Song& song, ObjectId clip, const Options& o);

// ---- a channel's sample (the Sampler, Kerf and every instrument that plays a file) ----
// True when the channel plays a sample file these tools can work on.
bool channelHasSample(const Channel& ch);
// The channel's sample with its ends trimmed (`remove` false) or every silence removed, written
// as a new file into `dir`; the channel plays it (the rest of its data, such as Kerf's slice
// names, kept) and its positions move with the audio: the Sampler's start, end and loop points,
// Kerf's slice markers and ends. Returns the new file, "" when nothing changed (`why` says why).
std::string applyToChannel(Song& song, ObjectId channel, bool remove, const Options& o, const std::string& dir, std::string* why = nullptr);

}  // namespace gl::silence
