// Gridline - K-123 (4.5): instrument channels armed for note recording. See MidiArm.h.
#include "MidiArm.h"
#include <algorithm>
#include "../core/I18n.h"
#include "../core/MidiRecord.h"
#include "App.h"
#include "LiveLook.h"
#include "LogicPlugins.h"
#include "Theme.h"

namespace gl::midiarm {

using theme::pal;

bool applies(const App& a) { return !livelook::active(a); }

bool armed(const App& a, ObjectId channel) { return channel != kNoId && a.playlist.midiArm.count(channel) > 0; }

void setArmed(App& a, ObjectId channel, bool on) {
  if (channel == kNoId || !a.doc.song().channel(channel)) return;
  if (on) {
    a.playlist.midiArm.insert(channel);
  } else {
    a.playlist.midiArm.erase(channel);
  }
}

void toggle(App& a, ObjectId channel, bool exclusive) {
  const bool on = !armed(a, channel);
  if (exclusive) a.playlist.midiArm.clear();
  setArmed(a, channel, exclusive ? true : on);
}

std::vector<ObjectId> armedChannels(const App& a) {
  std::vector<ObjectId> out;
  if (a.playlist.midiArm.empty()) return out;
  for (const Channel& c : a.doc.song().channels) {
    if (a.playlist.midiArm.count(c.id)) out.push_back(c.id);
  }
  return out;
}

ObjectId liveChannel(const App& a) {
  if (logicplug::active(a)) return logicplug::liveChannel(a);  // R10-03: Smooth's armed / monitored track
  if (!applies(a)) return a.selChannel;
  return armedLiveChannel(armedChannels(a), a.selChannel);
}

std::vector<ObjectId> liveChannels(const App& a) {
  if (logicplug::active(a)) return logicplug::liveChannels(a);
  std::vector<ObjectId> out{liveChannel(a)};
  if (!applies(a)) return out;
  for (ObjectId id : armedChannels(a)) {
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

bool records(const App& a, ObjectId channel) {
  if (logicplug::active(a)) return logicplug::recordsChannel(a, channel);
  if (!applies(a)) return true;
  return armedRecords(armedChannels(a), channel);
}

bool layering(const App& a) { return logicplug::active(a) || (applies(a) && !armedChannels(a).empty()); }

bool layerNote(App& a, ObjectId channel, int pitch, int velocity, bool on, double tick) {
  if (logicplug::active(a)) return logicplug::layerNote(a, channel, pitch, velocity, on, tick);
  if (!applies(a) || !a.recorder.active() || !records(a, channel)) return false;
  if (on) {
    a.recorder.noteOn(a.doc, channel, pitch, velocity, tick);
  } else {
    a.recorder.noteOff(a.doc, channel, pitch, tick);
  }
  return true;
}

void drawDot(ImDrawList* dl, ImVec2 c, float r, bool isArmed, bool hot, float alpha) {
  const auto& P = pal();
  if (alpha <= 0.01f && !isArmed) return;
  if (isArmed) {
    dl->AddCircleFilled(c, r, hot ? theme::mix(P.danger, P.paper, 0.2f) : P.danger, 16);
    return;
  }
  dl->AddCircle(c, r - 0.5f, theme::withAlpha(hot ? P.danger : P.textDim, alpha), 16, 1.3f);
  if (hot) dl->AddCircleFilled(c, r * 0.45f, theme::withAlpha(P.danger, alpha), 12);
}

const char* dotHint(bool isArmed) {
  return isArmed ? i18n::N_("Armed: the notes you play go to this instrument and record there \xC2\xB7 click: disarm \xC2\xB7 Alt+click: arm only this one")
                 : i18n::N_("Arm for recording notes: what you play goes to this instrument and records there \xC2\xB7 Alt+click: arm only this one");
}

}  // namespace gl::midiarm
