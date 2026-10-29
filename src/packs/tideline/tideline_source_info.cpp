// Gridline — "tideline" pack: trigger sources in the project (J-165), see
// tideline_source.h. The note source channel lives in PluginState::data as
// "channel=<id>"; audio sources are ordinary sidechain routes of the mixer.
#include <algorithm>
#include <cstdlib>
#include "../../core/MenuOps.h"
#include "../../core/Params.h"
#include "tideline.h"
#include "tideline_source.h"

namespace gl::packs::tideline {

namespace {

constexpr const char* kChannelKey = "channel=";

bool hasSend(const Song& s, ObjectId from, ObjectId to, bool* sidechain = nullptr) {
  const MixerTrack* t = s.mixerTrack(from);
  if (!t) return false;
  for (const Send& sd : t->sends)
    if (sd.to == to) {
      if (sidechain) *sidechain = sd.sidechain;
      return true;
    }
  return false;
}

bool isTideline(const Song& s, ObjectId owner, int slot) {
  const MixerTrack* t = s.mixerTrack(owner);
  return t && slot >= 0 && static_cast<size_t>(slot) < t->slots.size() && t->slots[static_cast<size_t>(slot)].plugin.type == "gl.tideline";
}

void setP(Document& d, ObjectId owner, int slot, int k, double v) { d.setParam(slotParam(owner, slot, k), v, false); }

}  // namespace

uint32_t noteSourceChannel(const std::string& data) {
  size_t p = 0;
  while (p < data.size()) {
    size_t e = data.find('\n', p);
    if (e == std::string::npos) e = data.size();
    if (data.compare(p, std::char_traits<char>::length(kChannelKey), kChannelKey) == 0) {
      const std::string v = data.substr(p + std::char_traits<char>::length(kChannelKey), e - p - std::char_traits<char>::length(kChannelKey));
      const unsigned long id = std::strtoul(v.c_str(), nullptr, 10);
      return id <= 0xFFFFFFFFul ? static_cast<uint32_t>(id) : 0;
    }
    p = e + 1;
  }
  return 0;
}

std::string withNoteSourceChannel(const std::string& data, uint32_t channel) {
  std::string out;
  size_t p = 0;
  while (p < data.size()) {  // keep every other line
    size_t e = data.find('\n', p);
    if (e == std::string::npos) e = data.size();
    if (e > p && data.compare(p, std::char_traits<char>::length(kChannelKey), kChannelKey) != 0) out += data.substr(p, e - p) + "\n";
    p = e + 1;
  }
  if (channel != 0) out += kChannelKey + std::to_string(channel) + "\n";
  return out;
}

std::vector<ObjectId> sidechainKeys(const Song& s, ObjectId track) {
  std::vector<ObjectId> r;
  for (const MixerTrack& m : s.mixer) {
    bool sc = false;
    if (m.id != track && hasSend(s, m.id, track, &sc) && sc) r.push_back(m.id);
  }
  return r;
}

ObjectId routeSourceTrack(const Song& s, ObjectId track, int scIn) {
  const int src = flRouteInput(s, s.mixerIndex(track), scIn);
  return src >= 0 && static_cast<size_t>(src) < s.mixer.size() ? s.mixer[static_cast<size_t>(src)].id : kNoId;
}

ObjectId channelTrack(const Song& s, ObjectId channel) {
  const Channel* c = s.channel(channel);
  if (!c) return kNoId;
  return s.mixerTrack(c->mixerTrack) ? c->mixerTrack : s.masterId();
}

bool channelKeysDirectly(const Song& s, ObjectId channel, ObjectId track) {
  const ObjectId t = channelTrack(s, channel);
  return t != kNoId && t != s.masterId() && t != track;
}

bool linkAudioTrack(Document& d, ObjectId owner, int slot, ObjectId from) {
  const Song& s = d.song();
  if (!isTideline(s, owner, slot) || from == owner || !s.mixerTrack(from)) return false;
  bool sc = false;
  const bool exists = hasSend(s, from, owner, &sc);
  if (!exists && !canRoute(s, from, owner)) return false;
  d.beginGroup("Tideline trigger from " + mixerTrackName(s, static_cast<size_t>(s.mixerIndex(from))));
  int scIn = -1;
  if (!exists) {
    if (!d.setSend(from, owner, 1.0f) || !d.setSendSidechain(from, owner, true)) {
      d.cancelGroup();
      return false;
    }
  } else if (!sc) {  // already mixed into this track: read that route (J-45 input route number)
    const std::vector<int> ins = flRouteInputs(d.song(), d.song().mixerIndex(owner));
    const auto it = std::find(ins.begin(), ins.end(), d.song().mixerIndex(from));
    scIn = it == ins.end() ? -1 : static_cast<int>(it - ins.begin());
  }
  setP(d, owner, slot, BwTrigger, TrigAudio);
  setP(d, owner, slot, BwScIn, scIn);
  d.endGroup();
  return true;
}

bool linkAudioChannel(Document& d, ObjectId owner, int slot, ObjectId channel) {
  const Song& s = d.song();
  const Channel* c = s.channel(channel);
  if (!c || !isTideline(s, owner, slot)) return false;
  d.beginGroup("Tideline trigger from " + c->name);
  ObjectId from = channelTrack(s, channel);
  if (!channelKeysDirectly(s, channel, owner)) {
    const std::vector<ObjectId> used = ops::routeToFreeMixerTracks(d, {channel});
    from = used.empty() ? kNoId : used.front();
  }
  if (from == kNoId || !linkAudioTrack(d, owner, slot, from)) {
    d.cancelGroup();
    return false;
  }
  d.endGroup();
  return true;
}

bool unlinkAudioTrack(Document& d, ObjectId owner, ObjectId from) {
  bool sc = false;
  if (!hasSend(d.song(), from, owner, &sc) || !sc) return false;
  return d.setSend(from, owner, 0.0f);
}

bool linkNoteChannel(Document& d, ObjectId owner, int slot, ObjectId channel) {
  const Song& s = d.song();
  const Channel* c = s.channel(channel);
  if (!c || !isTideline(s, owner, slot)) return false;
  return d.editMixerTrack(owner, "Tideline notes from " + c->name, [&](MixerTrack& t) {
    PluginState& p = t.slots[static_cast<size_t>(slot)].plugin;
    normalizePluginState(p);
    p.data = withNoteSourceChannel(p.data, channel);
    if (p.params.size() >= static_cast<size_t>(BwCount)) {
      p.params[BwTrigger] = TrigMidi;
      p.params[BwNoteSrc] = SrcChannel;
    }
  });
}

bool setNoteSource(Document& d, ObjectId owner, int slot, int src, int port) {
  if (!isTideline(d.song(), owner, slot) || src == SrcChannel) return false;
  return d.editMixerTrack(owner, src == SrcInput ? "Tideline notes from the MIDI input" : "Tideline notes from MIDI Out ports", [&](MixerTrack& t) {
    PluginState& p = t.slots[static_cast<size_t>(slot)].plugin;
    normalizePluginState(p);
    p.data = withNoteSourceChannel(p.data, 0);
    if (p.params.size() >= static_cast<size_t>(BwCount)) {
      p.params[BwTrigger] = TrigMidi;
      p.params[BwNoteSrc] = static_cast<float>(src == SrcInput ? SrcInput : SrcPorts);
      if (src != SrcInput) p.params[BwMidiPort] = static_cast<float>(std::clamp(port, -1, 255));
    }
  });
}

}  // namespace gl::packs::tideline
