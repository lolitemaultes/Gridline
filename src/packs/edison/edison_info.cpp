// Gridline — "edison" pack: catalogue entry for the audio editor / recorder.
#include "../Packs.h"
#include "edison.h"

namespace gl::packs::edison {

namespace {

ParamDesc lin(const std::string& n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const std::string& n, double d) { return ParamDesc{n, "", 0, 1, d, ParamKind::Toggle, ParamScale::Linear, {}}; }
ParamDesc ch(const std::string& n, std::vector<std::string> c, double d) {
  double mx = static_cast<double>(c.size() - 1);
  return ParamDesc{n, "", 0, mx, d, ParamKind::Choice, ParamScale::Linear, std::move(c)};
}

}  // namespace

double recMaxSeconds(int choice) {
  static const double kSec[] = {0, 60, 180, 300, 600, 1800, 3600};
  return choice >= 0 && choice < 7 ? kSec[choice] : 0.0;
}

void appendInfos(std::vector<PluginInfo>& v) {
  PluginInfo p{kType, "Loupe", "Misc", PluginKind::Effect, {}, false, {}};
  p.params.resize(EdCount);
  p.params[EdRecMode] = ch("Record start", {"Now", "On signal", "While signal", "On playback"}, RecOnInput);
  p.params[EdRecMax] = ch("Record limit", {"Unlimited", "1 min", "3 min", "5 min", "10 min", "30 min", "60 min"}, 4);
  p.params[EdThreshold] = lin("Trigger level", "dB", -80, 0, -40);
  p.params[EdPreRoll] = lin("Pre-roll", "s", 0, 5, 0.5);
  p.params[EdMuteInput] = tog("Mute input", 0);
  p.params[EdAppend] = tog("Append takes", 0);
  p.params[EdLoop] = tog("Loop playback", 0);
  p.params[EdOutGain] = lin("Playback level", "dB", -24, 12, 0);
  p.params[EdFollowHost] = tog("Follow transport", 0);
  p.editorWidth = 1100;
  p.presets = {
      {"Standard", {}},
      {"Arm on signal", {{EdRecMode, static_cast<float>(RecOnInput)}, {EdPreRoll, 1.0f}, {EdThreshold, -45.f}}},
      {"Arm with transport", {{EdRecMode, static_cast<float>(RecOnPlay)}, {EdFollowHost, 1.f}, {EdPreRoll, 0.f}}},
      {"Input logger 3 min", {{EdRecMode, static_cast<float>(RecInput)}, {EdRecMax, 2.f}, {EdThreshold, -50.f}}},
      {"Transport logger 3 min", {{EdRecMode, static_cast<float>(RecOnPlay)}, {EdRecMax, 2.f}, {EdAppend, 1.f}}},
  };
  v.push_back(p);
}

}  // namespace gl::packs::edison
