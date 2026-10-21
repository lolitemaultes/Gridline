// Gridline — FL Studio project (.flp) export (J-34).
//
// Writes a Gridline song as an FL Studio project in FL 25's layout (event
// order and record sizes of the projects FL 25.2 / 26.1 save): project
// settings, channels (sampler channels with their sample settings, plugin
// channels with their FL plugin state, layers, audio-clip and automation
// channels), patterns (notes, pattern automation), the mixer (inserts,
// routing, slots with their FL plugin states, levels, strip EQ), the
// playlist arrangements (tracks, pattern / audio / automation clips,
// markers), automation targets, controller and MIDI remote links.
//
// Songs imported from FL keep the original events of every object (Song::
// flOrigin, FlpOrigin.h). An object that is unchanged since the import is
// written back from them byte for byte; a changed object is written from
// Gridline's values, with the original events as the template for settings
// Gridline does not model. Plugins go through the encoders (FlpEncode.h).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../Song.h"
#include "FlpWriter.h"

namespace gl::flp {

struct ExportOptions {
  bool useOrigin = true;     // false: ignore Song::flOrigin (tests the encoders)
  std::string factoryData;   // folder %FLStudioFactoryData% stands for (sample paths)
  std::string projectDir;    // folder of the .flp (relative sample paths)
  std::string version = "25.2.5.5319";  // saved-by version written to event 199
  // J-80: Gridline-only instruments are rendered to samples in this folder
  // (FlpStandIn.h). exportFlpFile fills it with "<folder of the .flp>/<its
  // name>" when empty and renderInstruments is on; empty in exportFlp: those
  // instruments stay placeholders.
  std::string sampleDir;
  bool renderInstruments = true;
  std::function<void(double)> progress;           // 0..1 (rendering)
  const std::atomic<bool>* cancel = nullptr;      // stops the export (exportFlp returns false)
};

struct ExportReport {
  std::vector<std::string> warnings;
  std::map<std::string, int> counts;  // "channels", "plugins exact", …
  struct Plugin {
    std::string where;    // "channel 'Kick'", "insert 3 slot 2"
    std::string glType;
    std::string flName;   // FL plugin written ("" = none)
    std::string quality;  // verbatim / exact / approx / defaults / placeholder
    std::string note;
  };
  std::vector<Plugin> plugins;
  std::vector<std::string> files;  // J-80: samples rendered for the project
  bool cancelled = false;
  void warn(const std::string& w) { warnings.push_back(w); }
};

bool exportFlp(const Song& song, Bytes& out, ExportReport& rep, const ExportOptions& opt);
// J-80: a song whose stand-ins applyStandIns (FlpStandIn.h) already made.
struct StandInResult;
bool exportFlpWithStandIns(const Song& work, const StandInResult& standIns, Bytes& out, ExportReport& rep, const ExportOptions& opt);
// The folder rendered samples go to for an .flp: "<its folder>/<its name>".
std::string defaultSampleDir(const std::string& flpPath);
bool exportFlpFile(const Song& song, const std::string& path, ExportReport& rep, const ExportOptions& opt, std::string* err);
std::string reportText(const ExportReport& rep);
// J-80: one line for the notification ("Saved x.flp: 6 Gridline instruments
// rendered to samples, 2 effects mapped").
std::string exportSummary(const ExportReport& rep, const std::string& fileName);

// Round trip of one FL project: import → export → import, then the two
// Gridline songs compared field by field, and the exported event stream
// compared with the original event by event.
struct RoundTrip {
  Bytes exported;
  ExportReport report;
  // Gridline song differences (path → "a | b"); empty = identical.
  std::vector<std::string> songDiffs;
  std::map<std::string, int> songDiffCounts;  // by category (first path element)
  // Event-stream comparison: per event id, original count / exported count /
  // events whose payload differs (aligned by section).
  struct EventDiff {
    int original = 0, exported = 0, same = 0, changed = 0;
  };
  std::map<int, EventDiff> events;
  int originalEvents = 0, exportedEvents = 0, identicalEvents = 0;
  bool bytesIdentical = false;
  Song original, reimported;  // the two Gridline songs compared
  int plugins = 0, pluginsVerbatim = 0, pluginsExact = 0, pluginsApprox = 0, pluginsDefaults = 0, pluginsNone = 0;
};
bool roundTrip(const std::string& flpPath, const ExportOptions& opt, RoundTrip& out, std::string* err);
std::string roundTripText(const RoundTrip& rt);
std::string roundTripJson(const RoundTrip& rt);
// Field-by-field comparison of two songs (paths like "channels[3].plugin.params[12]").
std::vector<std::string> compareSongs(const Song& a, const Song& b, bool ignoreOrigin = true);

}  // namespace gl::flp
