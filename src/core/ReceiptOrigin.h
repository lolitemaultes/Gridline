// Gridline - K-321: the origin of a derived file, version 2 ("<file>.origin.json").
//
// Version 1 (J-219) names one source and one linear part of it (start, end, speed, reverse), or
// "made from these files, part unknown" for renders. Version 2 adds, per source, the segments of
// the derived file's time that played which part of the source: [outA, outB) -> srcA..srcB in
// seconds (srcB < srcA: read backwards), optionally compacted as loops (`rep`). It also names the
// derived file's length and SHA-256, the source's SHA-256 and original file name (so the trail
// survives a move, a rename or a collab copy named by its hash), and the processing steps.
//
// A v2 sidecar always carries the exact v1 view that Gridline 0.7.7 reads: one linear source
// gives start/end/speed/reverse and rangeKnown:true; anything else gives rangeKnown:false with
// source = the first source and sources = all of them. A v1 sidecar is read into a v2 origin
// without maps, and tracing it uses the v1 math (mapThrough), so old sidecars give the same
// receipts as before.
//
// `trace()` follows the trail through a DAG (each source may itself be derived), to a depth of 8,
// stopping at cycles, and returns the source ranges per original file.
#pragma once
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Json.h"
#include "TrackReceipt.h"

namespace gl::receipt {

// One segment: output [outA, outB) of the derived file played source srcA -> srcB (seconds).
using Seg = std::array<double, 4>;

struct SourceMap {
  std::string source;      // the source file (absolute; "sha256:<hex>" in the project table)
  std::string sourceHash;  // SHA-256 of the source's bytes ("" = not known)
  std::string sourceName;  // the source's own file name when it was made (Tracklib names)
  double sourceLength = 0; // seconds (0 = not known)
  std::vector<Seg> segs;
  // Loops: segments [i0, i1) repeat every `period` seconds of output, `count` more times.
  struct Rep {
    int i0 = 0, i1 = 0;
    double period = 0;
    int count = 0;
    bool operator==(const Rep& o) const { return i0 == o.i0 && i1 == o.i1 && period == o.period && count == o.count; }
  };
  std::vector<Rep> rep;
  // An incomplete map (the segment log was full, or over the size cap): the merged source
  // ranges read, without where in the derived file. The whole file maps "somewhere in these".
  Ranges ranges;
  // Summaries for the transformation detail ([min, max]; empty = not known).
  std::vector<double> pitch, speed;
  int chops = -1;  // -1 = not known
  bool operator==(const SourceMap& o) const {
    return source == o.source && sourceHash == o.sourceHash && sourceName == o.sourceName && sourceLength == o.sourceLength &&
           segs == o.segs && rep == o.rep && ranges == o.ranges && pitch == o.pitch && speed == o.speed && chops == o.chops;
  }
};

struct OriginV2 {
  int version = 1;           // 1: read from a v1 sidecar (no maps); 2: written by 0.7.8 or later
  Origin v1;                 // the v1 fields (always written; filled from the maps by v1View)
  double length = 0;         // the derived file's length in seconds (0 = not known)
  std::string hash;          // SHA-256 of the derived file ("" = not known)
  bool complete = true;      // false: some map holds ranges only
  std::vector<SourceMap> maps;
  std::vector<std::string> ops;  // processing steps ("gain", "eq", ...)
  bool operator==(const OriginV2& o) const {
    return version == o.version && v1.source == o.v1.source && v1.start == o.v1.start && v1.end == o.v1.end && v1.speed == o.v1.speed &&
           v1.reverse == o.v1.reverse && v1.rangeKnown == o.v1.rangeKnown && v1.kind == o.v1.kind && v1.sources == o.v1.sources &&
           length == o.length && hash == o.hash && complete == o.complete && maps == o.maps && ops == o.ops;
  }
};

// The v1 view of the maps: what 0.7.7 reads. One source with one linear forward or backward
// segment starting at output 0 and covering the file: start/end/speed/reverse, rangeKnown. Any
// other shape: rangeKnown false, source = the first, sources = every source. Without maps: `o.v1`.
Origin v1View(const OriginV2& o);
// The segments of a map with its loops written out.
std::vector<Seg> expand(const SourceMap& m);
// Compacts repeating runs of segments into `rep` (the inverse of expand, within 1 microsecond).
void compactLoops(SourceMap& m);

// To and from JSON. Relative source paths are taken relative to `baseDir` (a package).
Json originToJson(const OriginV2& o);
bool originFromJson(const Json& j, OriginV2& o, const std::string& baseDir = {});

// Sidecars. writeOriginV2 writes the v1 view and, with maps, version 2; readOriginV2 reads both.
bool writeOriginV2(const std::string& file, const OriginV2& o);
bool readOriginV2(const std::string& file, OriginV2& o);

// Where to find the origin of a file: its sidecar, else the project table by the file's
// hash (sources named "sha256:<hex>" resolve to the project's file with that hash).
struct Lookup {
  std::function<bool(const std::string& file, OriginV2& o)> origin;  // null: sidecars only
  std::function<std::string(const std::string& hash)> fileOfHash;    // null: none
  bool find(const std::string& file, OriginV2& o) const;
  std::string resolve(const std::string& source) const;  // "sha256:<hex>" -> a path, else as is
};
Lookup sidecarLookup();

struct SourceTrace {
  Ranges ranges;          // seconds of the original read
  bool unknown = false;   // some of it is from a step whose range is not known
  int depth = 0;          // steps from the file to the original
};
// Maps `ranges` of `file` (seconds; the file is `fileSeconds` long, 0 = not known) through its
// origins onto every original it comes from. A file without an origin traces to itself. Stops at
// cycles and at a depth of 8 (and says so in `warnings`).
std::map<std::string, SourceTrace> trace(const std::string& file, const Ranges& ranges, double fileSeconds, const Lookup& lookup,
                                         std::vector<std::string>* warnings = nullptr);
// True when any file on the trail of `file` has a version 2 map (the receipt then traces it
// through `trace`; otherwise the 0.7.7 chain is kept exactly).
bool hasMaps(const std::string& file, const Lookup& lookup);

// The 0.7.7 chain of `file` (originChain) through `lookup`, sources resolved: the v1 view of
// each step. With sidecarLookup() it is originChain(file).
std::vector<Origin> chainOf(const std::string& file, const Lookup& lookup);
// Every origin on the trail of `file` by file, sources resolved (what FileIn::trail keeps).
std::map<std::string, OriginV2> trailOf(const std::string& file, const Lookup& lookup);
// A lookup over origins already read (FileIn::trail).
Lookup trailLookup(const std::map<std::string, OriginV2>& trail);

// ---- the project table (inside Song::trackReceipt, by SHA-256 of the file)
// An origin as the project keeps it: sources named by hash where known.
std::string tableOrigin(const OriginV2& o);
// Looks up the project's origin of a file with this hash.
bool tableFind(const Saved& s, const std::string& hash, OriginV2& o);
// What a receipt scan learnt, kept in the project's table: the Tracklib name of each hashed file
// and the origin of each derived one (sources that are project files named by hash). Entries of
// files the project no longer uses go. True when anything changed (the caller makes it one edit).
bool rememberOrigins(Saved& s, const Measurement& m);

}  // namespace gl::receipt
