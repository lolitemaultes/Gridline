// Gridline - J-219: Track Receipt. Which parts of which original songs a
// project samples, so the user can fill in a sample license (for example a
// Tracklib license) and share the right splits when releasing.
//
// What counts is the time of the ORIGINAL that is used: a 2 s part looped for
// 4 minutes counts 2 s; chopped parts add up; the same section taken from
// several stems of one song counts once. One license per original song,
// chosen by length tier (up to 2 s, 15 s or 60 s; more than 60 s from one song
// cannot be licensed).
//
// This module is pure logic (no audio): file-name parsing, range arithmetic,
// the derived-file origin sidecar, the user's saved fields (Song::trackReceipt)
// and the report built from a measurement. The measurement itself (an offline
// render with the sample coverage tap, engine/SampleTap.h) is
// engine/TrackReceiptPass.h; the spreadsheet and PDF writers are
// core/TrackReceiptXml.cpp and core/TrackReceiptPdf.cpp.
//
// A helper, not legal advice: the category and fee come from the user's license and
// are typed in. The share follows from the category and tier (shareRate) unless typed.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Types.h"

namespace gl::receipt {

// ---- file names --------------------------------------------------------------
// Tracklib downloads are named
//   <Artist>-<Title>-<Stem>--<bpm>bpm-<Key>-[Clearance Required][ - Part_N].<ext>
// (the key may be empty). Artist and title may contain hyphens, so the split
// between them (first hyphen) is a best guess the user can correct.
struct NameInfo {
  bool tracklib = false;  // carries the "[Clearance Required]" tag
  std::string song;       // song identity: everything before the last "-<Stem>"
  std::string artist, title, stem, key;
  double bpm = 0;
  int part = 0;           // Part_N (0 = none)
  // A loop cut from the song ("<Stem>--<start>-<end>--<bpm>bpm..."): the
  // original's seconds it holds (loopEnd > loopStart); its uses map onto them.
  double loopStart = 0, loopEnd = 0;
  bool ambiguous = false;    // more than one hyphen before the stem: the artist / title split is a guess
  bool artistKnown = false;  // resplit() matched an artist seen on another file
};
NameInfo parseName(const std::string& pathOrName);
// A better guess for an ambiguous name: the split whose artist is already
// known (lower case) from another file of the project; else unchanged (first hyphen).
NameInfo resplit(NameInfo n, const std::set<std::string>& knownArtists);
std::string lowerText(std::string s);
// File name without folders ("a/b/c.wav" -> "c.wav").
std::string baseName(const std::string& path);

// ---- ranges (seconds) ----------------------------------------------------------
struct Range {
  double a = 0, b = 0;  // [a, b)
  bool operator==(const Range& o) const { return a == o.a && b == o.b; }
};
using Ranges = std::vector<Range>;
// Sorted, merged (ranges closer than `join` seconds become one).
Ranges unite(Ranges r, double join = 0.0005);
double total(const Ranges& r);
// "1:05.3" (minutes, seconds to 0.1 s).
std::string timeText(double seconds);
// "0:01.0-0:03.0, 0:10.5-0:12.0"; "" when empty.
std::string rangesText(const Ranges& r);
// Seconds as "12.3".
std::string secondsText(double seconds);
// Parses ranges typed as "m:ss.s-m:ss.s" or "s-s", separated by commas; false on a syntax error.
bool parseRanges(const std::string& text, Ranges& out);

// ---- tiers -------------------------------------------------------------------
inline constexpr double kMaxLicensable = 60.0;
// A voice's cue block (what it reads in its block before it jumps to where it plays: a deck
// cueing, a slice retrigger) is not counted when shorter than this (seconds). K-321: the tap
// flags cue blocks, so other short reads count (a stutter, grains far apart); uses made without
// the tap (Use::measured false) still leave out every read shorter than this.
inline constexpr double kMinHeard = 0.01;
// The tier to choose: 2, 15 or 60 (seconds); 0 when over 60 s (not licensable).
// Rounded to 0.1 s first, like the figures shown.
int tierFor(double seconds);
// The revenue share in percent of a license of category "A", "B" or "C" at tier 2, 15 or 60;
// -1 for anything else.
double shareRate(const std::string& category, int tier);

// ---- derived files: origin sidecar ("<file>.origin.json") -------------------------
// Written when Gridline makes a file from another one (an edit, a separated
// stem, a slice, a render), so uses of the new file map back to the original.
struct Origin {
  std::string source;      // the original file
  double start = 0;        // where the derived file's first second sits in the source (seconds)
  double end = -1;         // end of the source part (seconds); -1 = start + derived length x speed
  double speed = 1;        // source seconds per derived second
  bool reverse = false;    // the derived file plays the part backwards
  bool rangeKnown = true;  // false: made from the source, but which part is not known (renders)
  std::string kind;        // "edit", "stem", "slice", "render", ... (shown)
  std::vector<std::string> sources;  // renders: every file that went into it
};
std::string originPath(const std::string& file);
bool writeOrigin(const std::string& file, const Origin& o);
bool readOrigin(const std::string& file, Origin& o);

// ---- what the user typed (saved in Song::trackReceipt) ----------------------------
struct SongFields {
  std::string artist, title;   // empty = from the file name
  std::string category;        // "A", "B", "C" or ""
  std::string share;           // revenue share in percent, as typed ("10")
  std::string fee;             // license fee, as typed
  std::string tier;            // tier the user chose ("" = the suggested one)
  std::string holders;         // rights holders (master / publishing), free text
  // K-321: what the licence of this song is counted on: "" = what is read, "heard" = what is heard.
  std::string basis;
  bool operator==(const SongFields& o) const {
    return artist == o.artist && title == o.title && category == o.category && share == o.share && fee == o.fee && tier == o.tier &&
           holders == o.holders && basis == o.basis;
  }
};
struct FileFields {
  int ticked = -1;   // -1 automatic (Tracklib names), 0 no, 1 yes
  std::string song;  // song key this file belongs to ("" = automatic)
  // Parts of the file the user says are used ("0:01.0-0:03.0, 0:10-0:12"): replaces the
  // estimate of uses Gridline cannot measure (inside third-party plugins). "" = the estimate.
  std::string ranges;
  // K-321: a length the user says is used, when the parts are not known ("4.5"; "" = none), and
  // what the user entered per use the receipt cannot measure, by "<owner>:<slot>" (parts as in
  // `ranges`, or seconds). The per-file `ranges` above stays as it was.
  std::string seconds;
  std::map<std::string, std::string> uses;
  bool operator==(const FileFields& o) const {
    return ticked == o.ticked && song == o.song && ranges == o.ranges && seconds == o.seconds && uses == o.uses;
  }
};
struct Writer {
  std::string name, role, pro, ipi, share;  // share: percent of your part, as typed
  bool operator==(const Writer& o) const { return name == o.name && role == o.role && pro == o.pro && ipi == o.ipi && share == o.share; }
};
struct Release {
  std::string title, performer, licensee, isrc, date, distributor;  // date: YYYY-MM-DD
  bool operator==(const Release& o) const {
    return title == o.title && performer == o.performer && licensee == o.licensee && isrc == o.isrc && date == o.date &&
           distributor == o.distributor;
  }
};
struct Saved {
  std::map<std::string, SongFields> songs;  // by song key
  std::map<std::string, FileFields> files;  // by file path
  Release release;
  std::vector<Writer> writers;
  // K-322: the keys this version does not know (a newer Gridline's), kept so an edit here never
  // erases them: compact JSON of the saved object without the known keys ("" = none). The top
  // level, each song, each file and the release keep theirs (a writer's own unknown keys are not
  // kept: writers are a list that is edited by position).
  std::string unknown;
  // K-321: the project's own trail, by SHA-256 of the file's bytes (the key collab names its
  // copies by): the original file name of a file (Tracklib names survive a rename to the hash),
  // and the origin of a derived file (compact JSON as in the sidecar, sources as "sha256:<hex>"
  // where known), used when the sidecar is missing.
  std::map<std::string, std::string> names, origins;
  // K-321: the Heard settings ("" = default: the floor in dBFS as typed, "-60") and the kept
  // receipt versions (compact JSON each, the newest last).
  std::string heardFloor;
  std::vector<std::string> snapshots;
  bool operator==(const Saved& o) const {
    return songs == o.songs && files == o.files && release == o.release && writers == o.writers && unknown == o.unknown &&
           names == o.names && origins == o.origins && heardFloor == o.heardFloor && snapshots == o.snapshots;
  }
  bool empty() const;
};
std::string savedToJson(const Saved& s);  // compact; "" when empty
Saved savedFromJson(const std::string& json);
// (K-322: savedToJson(savedFromJson(j)) == j for any j savedToJson wrote, unknown keys included.)
// The fields of a file, matched by path, else (K-321) by "sha256:<hash>" when the hash is
// known, else by file name.
FileFields fileFields(const Saved& s, const std::string& path, const std::string& hash = {});

// ---- measurement (engine/TrackReceiptPass.h fills it) -------------------------------
struct Use {
  std::string file;        // path
  ObjectId owner = kNoId;  // channel / clip / mixer track
  std::string where;       // "Channel 3 'Chops' (Sampler)", "Playlist track 2, audio clip"
  int64_t plays = 0;       // starts (notes, clips)
  double bar0 = 0, bar1 = 0;  // first / last bar heard (1-based; 0 = unknown)
  Ranges ranges;           // file seconds read
  bool muted = false;      // not counted: muted or silent
  bool estimated = false;  // counted from the settings, not measured (whole file for analysers)
  std::string note;
  bool measured = false;   // K-321: ranges from the tap, cue blocks already left out (see kMinHeard)
  // K-321 (filled by later stages; inert until then):
  enum class Source { Measured, NotMeasured, EnteredRanges, EnteredSeconds };
  Source source = Source::Measured;
  double playSeconds = 0;  // song seconds it sounded while reading (every play counted)
  struct Detail {
    int pitched = 0;                       // pitch reports (0 = pitch not known)
    double semisMin = 0, semisMax = 0;     // pitch in semitones
    double speedMin = 0, speedMax = 0;     // source seconds per song second (0 = not known)
    double reversed = 0;                   // share of the reads played backwards (0..1)
    int chops = 0;                         // jumps inside a voice
    double avgRead = 0;                    // seconds per read between jumps
    std::vector<std::string> effects;      // effect categories on the way to the output
    int recognisable = -1;                 // 0..100 (-1 = not scored)
    std::vector<std::string> reasons;      // what the score took off
  };
  Detail detail;
};
struct FileIn {
  std::string path;
  double seconds = 0;  // decoded length (0 = unknown)
  bool exists = true;
  bool derived = false;       // an origin sidecar was found
  std::vector<Origin> chain;  // file -> its source -> ... (chain.back().source is the original)
  // K-321: SHA-256 of the file ("" = not hashed), and the trail by version 2 maps: every
  // origin on it, by file (empty when no file on the trail has maps: the chain above is used).
  std::string hash;
  std::map<std::string, std::string> trail;  // file -> its origin (compact JSON, ReceiptOrigin.h)
};
// The origin chain of `file` (an edit of a stem of X: [edit -> stem, stem -> X]).
// Empty when the file has no sidecar.
std::vector<Origin> originChain(const std::string& file);
// Maps file seconds through an origin chain onto the original; false when a
// step's range is not known. `fileSeconds` is the length of the first file.
bool mapThrough(const std::vector<Origin>& chain, double fileSeconds, Ranges& r);
struct Measurement {
  std::vector<FileIn> files;
  std::vector<Use> uses;
  std::vector<std::string> warnings;
  bool cancelled = false;
  std::string mode;  // "song", "patterns"
};

// ---- the report ------------------------------------------------------------------
struct FileRow {
  std::string path, name;
  NameInfo info;
  bool ticked = false, autoTicked = false;
  std::string song;          // song key ("" = none)
  double seconds = 0;
  bool exists = true;
  Ranges used;               // file seconds used (counted uses)
  double usedSeconds = 0;
  bool derived = false;      // has an origin sidecar
  Origin origin;
  std::string originNote;    // "derived from X" / "derived from X, range unknown"
  int part = 0;              // the song timeline this file maps onto
  bool unknownRange = false; // derived, part of the original not known
  bool estimated = false;    // some use is not measured (third-party plugin, analyser): whole file unless typed
  bool manual = false;       // the user typed the ranges (FileFields::ranges)
  std::vector<Use> uses, muted;
};
struct RangeFrom {
  Range r;                        // a merged range of the original
  std::vector<std::string> from;  // the uses it came from ("Playlist track 2 (Master)", ...)
  bool estimated = false;         // some of it is not measured (third-party plugin, analyser)
};
struct Timeline {
  int part = 0;        // Part_N; -1 = derived files of unknown range
  double length = 0;   // longest known file on it (seconds)
  Ranges used;         // seconds of the original used
  std::vector<RangeFrom> parts;  // `used` with where each range came from
  double seconds = 0;
  // K-321 (filled by later stages): what of `used` is heard, and lengths the user entered.
  Ranges heard;
  double heardSeconds = 0;
  bool heardKnown = false;
  double enteredSeconds = 0;
  bool operator==(const Timeline& o) const { return part == o.part && length == o.length && used == o.used && seconds == o.seconds; }
};
struct SongRow {
  std::string key, artist, title;
  std::string sourceName;          // an original file name of this song (to find it again on the store)
  bool checkName = false;          // the artist / title split is a guess: check it
  std::vector<std::string> stems;  // stems used (names)
  std::vector<std::string> files;  // paths of the files counted
  std::vector<Timeline> timelines;
  double seconds = 0;   // licensed amount: sum over timelines, to 0.1 s
  int tier = 0;         // suggested tier (2 / 15 / 60; 0 = over the limit)
  bool over = false;    // more than 60 s: not licensable
  SongFields fields;
  double share = -1;    // the share in force: fields.share, else autoShare (-1 = none yet)
  double autoShare = -1;  // the share the category and tier give (-1 = no category, nothing played or over 60 s)
  int64_t plays = 0;
  std::vector<std::string> warnings;
};
struct Report {
  std::string project;
  std::vector<FileRow> files;  // every audio file the project uses
  std::vector<SongRow> songs;  // ticked files grouped by original song
  Saved saved;
  double totalShare = 0;       // sum of the shares filled in
  bool sharesComplete = false; // every song has a share
  std::vector<std::string> warnings;
  std::string mode;
};
Report build(const Measurement& m, const Saved& saved, const std::string& projectName);

// Report dates: every 6 months from the release date (YYYY-MM-DD), `count` of them;
// empty when the date does not parse.
std::vector<std::string> reportDates(const std::string& releaseDate, int count);
// A share typed by the user ("10", "10 %", "12.5") as a number; -1 when empty or not a number.
double parseShare(const std::string& s);

// ---- outputs --------------------------------------------------------------------
// "<projectname>_trackreceipt.xml"
std::string receiptFileName(const std::string& projectName);
std::string resultsFileName(const std::string& projectName);  // "<projectname>_trackreceipt.pdf"
// SpreadsheetML 2003 (opens in Excel and LibreOffice): Licenses, Usage, Release, Splits.
std::string spreadsheetXml(const Report& r);
// The styled results document (PDF 1.4, vector only, standard fonts).
std::string resultsPdf(const Report& r);

}  // namespace gl::receipt
