// Gridline — FL Studio zipped loop package (J-52): one .zip holding an FL
// Studio project (.flp, written by the FLP exporter) and the files it uses,
// laid out so FL finds them when it opens the package: the .flp at the root
// of the archive and every file beside it (unique names), with the sample
// paths inside the .flp (event 196) rewritten to those names — relative to
// the project's folder, so they resolve wherever the archive is unpacked.
//
// The .flp is otherwise byte for byte the one File > Export > FL Studio
// project writes. Paths that FL keeps inside a plugin's own state are left as
// they are: the files sit beside the project, where FL and Gridline look for
// a missing sample by its name.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "../Song.h"
#include "FlpExport.h"

namespace gl::flp {

struct PackageOptions {
  ExportOptions flp;               // the FLP exporter's options (factoryData, projectDir, …)
  bool includeFactorySamples = false;  // files under factoryData (every FL install has them)
};

struct PackageReport {
  ExportReport flp;
  std::string projectEntry;                  // "<name>.flp"
  std::map<std::string, std::string> files;  // source file → archive entry
  std::vector<std::string> missing;          // referenced files that do not exist
  int rewritten = 0;                         // sample path events pointed at the archive
};

// The archive entries of the files a song uses: source path → unique name
// beside the project (case-insensitive, never "<project>.flp").
std::map<std::string, std::string> packageEntries(const Song& song, const std::string& projectEntry, const PackageOptions& opt,
                                                  std::vector<std::string>* missing = nullptr);
// Rewrites the sample path events (196) of an .flp image whose resolved path
// (against factoryData / projectDir) is a key of `entries` to the entry name.
// Every other byte is kept. Returns how many events changed (-1: not an FLP).
int rewriteSamplePaths(Bytes& flp, const std::map<std::string, std::string>& entries, const std::string& factoryData,
                       const std::string& projectDir);

// Writes the package. `name` = the project's file name without extension.
bool exportFlpZip(const Song& song, const std::string& zipPath, const std::string& name, const PackageOptions& opt,
                  PackageReport& rep, std::string* err);

}  // namespace gl::flp
