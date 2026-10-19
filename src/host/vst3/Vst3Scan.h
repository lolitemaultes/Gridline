// Gridline — VST3 hosting: describing a bundle (scanner). The scan result
// types, the cache and the child-process scanner are shared by every format
// (host/PluginScan.h); the names below are kept for the VST3 code and tests.
#pragma once
#include <cstdint>
#include <string>
#include "../PluginScan.h"

namespace gl::vst3 {

using host::fromJson;
using host::ScanCache;
using host::ScannedClass;
using host::ScannedParam;
using host::ScanResult;
using host::selfExecutablePath;
using host::toJson;

// Loads the bundle in this process and describes every audio module class.
ScanResult scanBundleInProcess(const std::string& bundle);
// Same in a child process (see host::scanIsolated).
ScanResult scanBundleIsolated(const std::string& exe, const std::string& bundle, double timeoutSeconds = 60);
// Child-process entry point (see host::childMain).
int scanMain(int argc, char** argv);
int64_t bundleMtime(const std::string& bundle);
// `gridline vst3-smoke <bundle>` (= `plugin-smoke vst3 <bundle>`). Does not return.
[[noreturn]] void smokeMain(const std::string& bundle);

}  // namespace gl::vst3
