// Gridline: which window system the application asks SDL for (Linux, the BSDs).
//
// Hosted plugins draw into an X11 window handed to them by the host: IPlugView, CLAP and LV2
// editors on these systems embed in nothing else, and so do bridges such as yabridge. Gridline can
// only hand them one when its own windows are X11 windows, on an X server or on XWayland.
//
// Which driver SDL takes by itself depends on the SDL the system has. Classic SDL2 tries X11 first.
// SDL3 tries Wayland first on compositors with the frame pacing protocol it wants, and on systems
// whose "SDL2" is the sdl2-compat layer over SDL3 (Arch and its derivatives, recent Fedora) that
// decides for Gridline too; many desktops also set SDL_VIDEODRIVER=wayland for everything, for
// games. Then every plugin interface would fail to open, so Gridline asks for X11 whenever an X
// server answers. Wayland stays when there is none, or when GRIDLINE_VIDEO_DRIVER or the setting
// (Settings > Interface, "Window system", ui.windowSystem) asks for it (K-153); plugin interfaces
// then open in windows of their own on XWayland (app/PluginWindow.cpp SideWindowEditor).
#pragma once
#include <string>

namespace gl::videodriver {

struct Inputs {
  const char* gridlineDriver = nullptr;  // GRIDLINE_VIDEO_DRIVER: "x11", "wayland", "auto" or unset
  const char* setting = nullptr;         // ui.windowSystem: "auto" or "wayland" (K-153; the variable wins)
  const char* sdl2Driver = nullptr;      // SDL_VIDEODRIVER
  const char* sdl3Driver = nullptr;      // SDL_VIDEO_DRIVER
  bool xServerAnswers = false;
};

enum class Why {
  SdlDecides,    // nothing asked for: SDL's own order (or an X server was not needed)
  UserAsked,     // GRIDLINE_VIDEO_DRIVER
  SettingAsked,  // Settings > Interface, Window system: native Wayland (K-153)
  UserSdlOther,  // SDL's variable names a driver other than Wayland (x11, offscreen, dummy...)
  NoXServer,     // Wayland is all there is
  PluginsNeedX,  // an X server answers: plugin interfaces can open
};

struct Choice {
  std::string driver;  // what to ask SDL for; empty = leave it to SDL
  Why why = Why::SdlDecides;
};

// The first name of a comma separated driver list, in lower case.
inline std::string firstDriver(const char* list) {
  std::string out;
  for (const char* p = list; p && *p && *p != ','; ++p) {
    const char c = *p;
    out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  }
  return out;
}

inline Choice choose(const Inputs& in) {
  const std::string own = firstDriver(in.gridlineDriver);
  if (!own.empty() && own != "auto") return {own, Why::UserAsked};
  if (own.empty() && firstDriver(in.setting) == "wayland") return {"wayland", Why::SettingAsked};
  const std::string sdl3 = firstDriver(in.sdl3Driver);
  const std::string sdl = sdl3.empty() ? firstDriver(in.sdl2Driver) : sdl3;
  if (!sdl.empty() && sdl != "wayland") return {std::string(), Why::UserSdlOther};
  if (!in.xServerAnswers) return {std::string(), Why::NoXServer};
  return {"x11", Why::PluginsNeedX};
}

// Why a plugin's interface cannot open, for the message the user sees.
enum class Block {
  None,            // Gridline's windows are X11 windows (or another system's own)
  NoXServer,       // on Wayland because no X server answers
  WaylandByChoice  // on Wayland because GRIDLINE_VIDEO_DRIVER or the setting says so
};

// K-153: with an X server (XWayland) a blocked interface still opens, in a window of its own there;
// the message is for sessions without one.
inline Block interfaceBlock(bool hasNativeWindow, Why why) {
  if (hasNativeWindow) return Block::None;
  return why == Why::UserAsked || why == Why::SettingAsked ? Block::WaylandByChoice : Block::NoXServer;
}

inline const char* blockMessage(Block b) {
  switch (b) {
    case Block::None: return "";
    case Block::NoXServer: return "plugin interfaces need XWayland, and this session has none. Install or enable it, then restart Gridline";
    case Block::WaylandByChoice:
      return "plugin interfaces open on XWayland, and this session has none. Install or enable it, or set Window system to Automatic";
  }
  return "";
}

}  // namespace gl::videodriver
