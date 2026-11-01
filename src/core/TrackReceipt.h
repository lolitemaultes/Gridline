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
// A helper, not legal advice: the category, rate and fee come from the
// user's license and are typed in, never computed.
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
// Measured reads shorter than this (seconds) are not counted (a voice's block before a jump).
inline constexpr double kMinHeard = 0.01;
// The tier to choose: 2, 15 or 60 (seconds); 0 when over 60 s (not licensable).
// Rounded to 0.1 s first, like the figures shown.
int tierFor(double seconds);

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
  bool operator==(const SongFields& o) const {
    return artist == o.artist && title == o.title && category == o.category && share == o.share && fee == o.fee && tier == o.tier &&
           holders == o.holders;
  }
};
struct FileFields {
  int ticked = -1;   // -1 automatic (Tracklib names), 0 no, 1 yes
  std::string song;  // song key this file belongs to ("" = automatic)
  // Parts of the file the user says are used ("0:01.0-0:03.0, 0:10-0:12"): replaces the
  // estimate of uses Gridline cannot measure (inside third-party plugins). "" = the estimate.
  std::string ranges;
  bool operator==(const FileFields& o) const { return ticked == o.ticked && song == o.song && ranges == o.ranges; }
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
  bool operator==(const Saved& o) const { return songs == o.songs && files == o.files && release == o.release && writers == o.writers; }
  bool empty() const;
};
std::string savedToJson(const Saved& s);  // compact; "" when empty
Saved savedFromJson(const std::string& json);
// The fields of a file, matched by path, else by file name.
FileFields fileFields(const Saved& s, const std::string& path);

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
};
struct FileIn {
  std::string path;
  double seconds = 0;  // decoded length (0 = unknown)
  bool exists = true;
  bool derived = false;       // an origin sidecar was found
  std::vector<Origin> chain;  // file -> its source -> ... (chain.back().source is the original)
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
  double share = -1;    // fields.share as a number (-1 = not filled in)
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
