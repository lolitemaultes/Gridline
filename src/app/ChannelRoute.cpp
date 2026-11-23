// Gridline — J-195: route a channel to a free mixer track (see ChannelRoute.h).
#include "ChannelRoute.h"
#include <algorithm>
#include <string>
#include "../core/I18n.h"
#include "../core/MenuOps.h"
#include "../../third_party/imgui/imgui.h"
#include "App.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::chanroute {

namespace {

std::string trackLabel(const Song& s, ObjectId track) {
  const int i = s.mixerIndex(track);
  return i < 0 ? std::string() : std::to_string(i) + "  " + mixerTrackName(s, static_cast<size_t>(i));
}

}  // namespace

std::vector<ObjectId> routeToFree(App& a, const std::vector<ObjectId>& channels) {
  ops::FreeRouteReport rep;
  const std::vector<ObjectId> used = ops::routeToFreeMixerTracks(a.doc, channels, &rep, true);
  const Song& s = a.doc.song();
  if (!used.empty()) a.selMixer = used.back();
  if (used.empty() && rep.full > 0) {
    a.notify("No free mixer track left: every insert is in use and the mixer has its maximum number of tracks", true);
  } else if (used.size() == 1 && channels.size() == 1) {
    if (rep.kept > 0)
      a.notify("Already on its own mixer track: " + trackLabel(s, used[0]));
    else if (rep.created > 0)
      a.notify("Routed to a new mixer track: " + trackLabel(s, used[0]));
    else
      a.notify("Routed to a free mixer track: " + trackLabel(s, used[0]));
  } else if (!used.empty()) {
    a.notify("Routed to free mixer tracks: " + std::to_string(used.size()));
    if (rep.full > 0) a.notify("Some channels stayed where they were: the mixer has its maximum number of tracks", true);
  }
  return used;
}

ObjectId routeToFree(App& a, ObjectId channel) {
  const std::vector<ObjectId> t = routeToFree(a, std::vector<ObjectId>{channel});
  return t.empty() ? kNoId : t.front();
}

const char* hint(const App& a, ObjectId channel) {
  if (ops::channelOwnsMixerTrack(a.doc.song(), channel))
    return i18n::tr("This channel already has a mixer track of its own: click to name the track after the channel");
  return i18n::tr("Route to a free mixer track: the next unused one (a new one when none is left), named after this channel");
}

bool button(const App& a, ObjectId channel, float size) {
  return ui::IconButton("##routefree", ui::Icon::RouteFree, false, size, hint(a, channel));
}

bool menuItem(const char* shortcut) { return ImGui::MenuItem(i18n::tr("Route to a free mixer track"), shortcut); }

bool listItem() { return ImGui::Selectable(i18n::tr("Route to a free mixer track")); }

std::string boxText(const App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  const int mi = c ? std::max(0, s.mixerIndex(c->mixerTrack)) : 0;
  return "\xE2\x86\x92 " + (mi == 0 ? std::string(i18n::tr("Master")) : std::to_string(mi) + "  " + mixerTrackName(s, static_cast<size_t>(mi)));
}

bool box(App& a, ObjectId channel, const char* id, float width, const char* dev) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  if (!c) return false;
  const int mi = std::max(0, s.mixerIndex(c->mixerTrack));
  bool free = false;
  const bool open = ui::BeginCombo(id, boxText(a, channel).c_str(), width);
  if (dev) devNoteRect(dev);
  if (open) {
    free = listItem();
    if (dev) devNoteRect((std::string(dev) + "list").c_str());
    ImGui::Separator();
    for (size_t i = 0; i < s.mixer.size(); ++i) {
      const std::string l = (i == 0 ? std::string(i18n::tr("Master")) : std::to_string(i) + "  " + mixerTrackName(s, i)) + "##" + std::to_string(i);
      if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == mi)) a.doc.routeChannel(channel, s.mixer[i].id);
      if (dev && i < 4) devNoteRect((std::string(dev) + "track" + std::to_string(i)).c_str());
    }
    ImGui::EndCombo();
  }
  ui::Hint("Mixer track this channel plays through");
  return free;
}

}  // namespace gl::chanroute
