// Gridline — FLP import: FL's third-party plugin host ("Fruity Wrapper") →
// Gridline hosted plugins.
//
// The wrapper's state (event 213) is a u32 version followed by records
// {u32 id, u64 size, bytes}. Records used here (worked out from FL Studio's
// supplied projects and saved user projects; no Image-Line code is used):
//   50  plugin info: first u32 = kind (0 VST2 effect, 4 VST2 instrument,
//       7 VST3 effect, 8 VST3 instrument, 9 AU effect, 10 AU instrument)
//   51  VST2 unique id (u32 LE)
//   52  VST3 class id (16 bytes, COM GUID byte order) / AU: u32 LE
//       manufacturer, type, subtype
//   53  the plugin's state:
//       VST2: i32 -9, u8 version, i32 flag (-2: 8 padding bytes follow), u32
//             chunk size, chunk bytes; size 0: u32 count + f32 values
//       VST3: u32 1, records 1 (header), 3 (component state), 2 (controller
//             state), 4 (FL parameter index → ParamID table)
//       AU:   u32 0, u32 size, u32 0, ClassInfo property list
//   54 name, 55 path, 56 vendor (UTF-8)
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Flp.h"

namespace gl::flp {

struct WrapperInfo {
  enum Format { None, Vst2, Vst3, Au } format = None;
  bool instrument = false;
  std::string name, vendor, path;
  uint32_t vst2Id = 0;
  std::string vst3Uid;  // 32 hex digits, SDK order
  std::string auKey;    // "type/subtype/manufacturer"
  std::string data;     // the state as Gridline's PluginState::data ("" when none)
  std::vector<uint32_t> paramIds;  // VST3: FL parameter index → ParamID
  // Hosted type the instance was saved with ("vst2:…", "vst3:…", "au:…").
  std::string savedType() const;
  const char* formatName() const;
};

// Parses a wrapper state payload (event 213). False when it is not one (or
// an empty / unloaded wrapper).
bool parseWrapper(const uint8_t* p, size_t n, WrapperInfo& out);

// The installed hosted plugin for a wrapper instance: by id in its own
// format, else by name (+ vendor) in its own format, else by name in another
// format (the state cannot move between formats). Empty type: not installed.
struct WrapperMatch {
  std::string type;   // installed hosted type, or "" when none
  bool byId = false;
  bool stateKept = false;  // the saved state applies to `type`
};
WrapperMatch matchWrapper(const WrapperInfo& w);

// True when `vendor` names Image-Line. An empty vendor is not Image-Line.
bool isImageLineVendor(const std::string& vendor);
// True only for Image-Line's own plugins loaded through the wrapper (VST
// builds of FL plugins in older projects): the vendor says Image-Line, or no
// vendor was recorded and the plugin's file lives in an Image-Line folder or
// carries FL's "Fruity " name. Only these may become Gridline built-ins on
// import; every other plugin stays the real hosted plugin (or a placeholder).
bool isImageLinePlugin(const std::string& vendor, const std::string& name, const std::string& path);

void registerWrapperDecoder(std::map<std::string, PluginDecoder>& d);

}  // namespace gl::flp
