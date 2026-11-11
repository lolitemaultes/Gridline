// Gridline: which window system the application asks SDL for.
#include "../src/core/VideoDriverChoice.h"
#include "Check.h"

using namespace gl::videodriver;

namespace {
Choice pick(const char* own, const char* sdl2, const char* sdl3, bool x) {
  Inputs in;
  in.gridlineDriver = own;
  in.sdl2Driver = sdl2;
  in.sdl3Driver = sdl3;
  in.xServerAnswers = x;
  return choose(in);
}
}  // namespace

int main() {
  // A Wayland desktop with XWayland, SDL left to itself (what sdl2-compat over SDL3 turns into Wayland).
  expect(pick(nullptr, nullptr, nullptr, true).driver == "x11", "X server answers, nothing set: X11");
  expect(pick(nullptr, nullptr, nullptr, true).why == Why::PluginsNeedX, "and the reason is the plugin interfaces");
  // A session that sets the variable for every program, for games.
  expect(pick(nullptr, "wayland", nullptr, true).driver == "x11", "SDL_VIDEODRIVER=wayland in the session: still X11 when an X server answers");
  expect(pick(nullptr, nullptr, "wayland", true).driver == "x11", "SDL_VIDEO_DRIVER=wayland: the same");
  expect(pick(nullptr, "wayland,x11", nullptr, true).driver == "x11", "a list that starts with Wayland: the same");
  expect(pick(nullptr, "WAYLAND", nullptr, true).driver == "x11", "any case");
  // Wayland only.
  expect(pick(nullptr, nullptr, nullptr, false).driver.empty(), "no X server: left to SDL");
  expect(pick(nullptr, nullptr, nullptr, false).why == Why::NoXServer, "and the reason says so");
  expect(pick(nullptr, "wayland", nullptr, false).driver.empty(), "no X server, SDL_VIDEODRIVER=wayland: left alone");
  // Other requests stay as they are: test runs and headless use.
  expect(pick(nullptr, "x11", nullptr, true).driver.empty(), "SDL_VIDEODRIVER=x11: left alone");
  expect(pick(nullptr, "dummy", nullptr, true).driver.empty(), "SDL_VIDEODRIVER=dummy: left alone");
  expect(pick(nullptr, "offscreen", nullptr, false).why == Why::UserSdlOther, "SDL_VIDEODRIVER=offscreen: left alone");
  expect(pick(nullptr, "x11", "wayland", true).driver == "x11", "SDL3's name counts first");
  // Gridline's own setting.
  expect(pick("wayland", nullptr, nullptr, true).driver == "wayland", "GRIDLINE_VIDEO_DRIVER=wayland keeps Wayland with an X server around");
  expect(pick("wayland", nullptr, nullptr, true).why == Why::UserAsked, "and the reason is the setting");
  expect(pick("x11", "wayland", nullptr, false).driver == "x11", "GRIDLINE_VIDEO_DRIVER=x11 beats everything");
  expect(pick("auto", "wayland", nullptr, true).driver == "x11", "GRIDLINE_VIDEO_DRIVER=auto is the default behaviour");
  expect(pick("", nullptr, nullptr, true).driver == "x11", "an empty setting is no setting");
  // K-153: Settings > Interface, Window system: native Wayland.
  {
    Inputs in;
    in.setting = "wayland";
    in.xServerAnswers = true;
    expect(choose(in).driver == "wayland" && choose(in).why == Why::SettingAsked, "the setting keeps Wayland with XWayland around");
    in.sdl2Driver = "x11";
    expect(choose(in).driver == "wayland", "the setting beats the session's SDL variable");
    in.gridlineDriver = "x11";
    expect(choose(in).driver == "x11" && choose(in).why == Why::UserAsked, "GRIDLINE_VIDEO_DRIVER beats the setting");
    in.gridlineDriver = "auto";
    expect(choose(in).driver.empty() && choose(in).why == Why::UserSdlOther, "GRIDLINE_VIDEO_DRIVER=auto: the session decides, not the setting");
    in.gridlineDriver = nullptr, in.sdl2Driver = nullptr, in.setting = "auto";
    expect(choose(in).driver == "x11", "setting Automatic: X11 when an X server answers");
    in.setting = "something";
    expect(choose(in).driver == "x11", "an unknown setting is Automatic");
    expect(interfaceBlock(false, Why::SettingAsked) == Block::WaylandByChoice, "no window, by the Wayland setting");
  }
  // The message for a refused interface.
  expect(interfaceBlock(true, Why::PluginsNeedX) == Block::None, "an X11 window: nothing in the way");
  expect(interfaceBlock(true, Why::SdlDecides) == Block::None, "SDL's own X11 window: nothing in the way");
  expect(interfaceBlock(false, Why::NoXServer) == Block::NoXServer, "no window, no X server");
  expect(interfaceBlock(false, Why::UserAsked) == Block::WaylandByChoice, "no window, by the setting");
  expect(*blockMessage(Block::None) == 0 && *blockMessage(Block::NoXServer) != 0 && *blockMessage(Block::WaylandByChoice) != 0, "the messages");
  expect(firstDriver("x11,wayland") == "x11" && firstDriver(nullptr).empty() && firstDriver("").empty(), "first name of a list");
  return finish();
}
