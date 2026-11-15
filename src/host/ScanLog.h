// Gridline — the plugin scan log (K-267): every entry whose scan fails is written to
// <config>/plugin-scan.log (Windows: %APPDATA%\gridline\plugin-scan.log), whatever the debug log
// setting, with what the scan saw: how the scanner child ended, how long it ran, how many plugins
// it described, the most memory it held and its last steps. The plugin manager shows the file's
// path, so a user can send it with a report. Nothing else is written there.
#pragma once
#include <cstdint>
#include <string>
#include "PluginScan.h"

namespace gl::host {

struct ScanLogEntry {
  std::string time;    // scanLogTime()
  std::string format;  // "vst3"
  std::string entry;   // the plugin file or bundle
  std::string error;   // what the plugin manager shows
  ScanReport report;
  int attempt = 0;      // 1: the retry, alone after the others
  bool retried = false;  // this failure is tried once more, alone, after the other entries
};

// The text of one failed entry (a few lines, ending with a newline).
std::string formatScanLogEntry(const ScanLogEntry& e);
// The first line of a scan's part of the log.
std::string scanLogHeader(const std::string& time, int entries, int jobs);
// The outcome in words ("timed out", "crashed", ...).
const char* scanOutcomeName(ScanReport::Outcome o);
// Appends `text` to `file` (created with its folder when missing). A file already larger than
// `maxBytes` becomes <file>.old first (one older file is kept), so the log never grows without end.
bool appendScanLog(const std::string& file, const std::string& text, std::uintmax_t maxBytes = 1u << 20);
// The local time as "2026-11-15 14:03:07".
std::string scanLogTime();

}  // namespace gl::host
