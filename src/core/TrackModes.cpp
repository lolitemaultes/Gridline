// Gridline — playlist track modes (PL-08). See TrackModes.h.
#include "TrackModes.h"
#include <algorithm>
#include <functional>
#include "Document.h"
#include "PluginInfo.h"

namespace gl::trackmodes {

namespace {

bool validTrack(const Song& s, int track) { return track >= 0 && track < static_cast<int>(s.tracks.size()); }

// Every track list of the song: the current arrangement's and the stored ones.
template <class SongT, class Fn>
void forEachTrackList(SongT& s, Fn fn) {
  fn(s.tracks, true);
  for (size_t i = 0; i < s.arrangements.size(); ++i)
    if (static_cast<int>(i) != s.currentArrangement) fn(s.arrangements[i].tracks, false);
}

bool fail(std::string* why, const char* text) {
  if (why) *why = text;
  return false;
}

bool trackHasClips(const Song& s, int track) {
  for (const auto& c : s.playlist)
    if (c.track == track) return true;
  return false;
}

bool insertIsDefault(const Song& s, const MixerTrack& m) {
  if (m.id == s.masterId() || !m.name.empty() || m.armed || m.input >= 0) return false;
  for (const auto& slot : m.slots)
    if (!slot.plugin.empty()) return false;
  return true;
}

// Channels playing into a mixer track. Audio clip channels (the sources of an
// audio track's recordings) count only with `clips`.
bool channelUsesMixer(const Song& s, ObjectId mixer, ObjectId exceptChannel = kNoId, bool clips = true) {
  for (const auto& c : s.channels) {
    if (c.id == exceptChannel || (!clips && c.audioClip)) continue;
    if (!clips && !c.audioClip) {  // a sampler that only plays clips counts as a clip source too
      bool onlyClips = false;
      for (const auto& cl : s.playlist) onlyClips = onlyClips || (cl.kind == ClipKind::Audio && cl.source == c.id);
      if (onlyClips) continue;
    }
    if (c.mixerTrack == mixer) return true;
    for (ObjectId a : c.auxOuts)
      if (a == mixer) return true;
  }
  return false;
}

bool sentInto(const Song& s, ObjectId mixer) {
  for (const auto& t : s.mixer)
    for (const auto& snd : t.sends)
      if (snd.to == mixer) return true;
  return false;
}

// Grows the track list so `track` exists.
void makeRoom(Song& s, int track) {
  if (track >= 0 && track < kMaxPlaylistTracks && static_cast<int>(s.tracks.size()) <= track) s.tracks.resize(static_cast<size_t>(track) + 1);
}

// Drops the track's links (disarming an audio track's mixer track).
void clearLinks(Song& s, int track) {
  PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  if (t.mode == TrackMode::Audio)
    if (MixerTrack* m = s.mixerTrack(t.mixer)) m->armed = false;
  t.mode = TrackMode::Unassigned;
  t.mixer = kNoId;
  t.channel = kNoId;
}

ObjectId newInsert(Song& s) {
  if (static_cast<int>(s.mixer.size()) > kMaxMixerInserts) return kNoId;
  ensureMixer(s, 0);
  MixerTrack t;
  t.id = s.allocateId();
  t.sends.push_back({s.masterId(), 1.0f, false});
  s.mixer.push_back(t);
  return t.id;
}

bool runEdit(Document& d, const char* label, const std::function<bool(Song&)>& fn) {
  Song probe = d.song();  // dry run: no undo step for an edit that cannot happen
  if (!fn(probe)) return false;
  d.editSong(label, [&](Song& s) { fn(s); });
  return true;
}

}  // namespace

const char* modeName(TrackMode m) {
  switch (m) {
    case TrackMode::Audio: return "Audio track";
    case TrackMode::Instrument: return "Instrument track";
    default: return "Ordinary track";
  }
}

TrackMode modeOf(const Song& s, int track) {
  if (!validTrack(s, track)) return TrackMode::Unassigned;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  if (t.mode == TrackMode::Audio && s.mixerTrack(t.mixer)) return TrackMode::Audio;
  if (t.mode == TrackMode::Instrument && s.channel(t.channel)) return TrackMode::Instrument;
  return TrackMode::Unassigned;
}

const MixerTrack* linkedMixer(const Song& s, int track) {
  switch (modeOf(s, track)) {
    case TrackMode::Audio: return s.mixerTrack(s.tracks[static_cast<size_t>(track)].mixer);
    case TrackMode::Instrument: {
      const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
      if (const MixerTrack* m = s.mixerTrack(t.mixer)) return m;
      const Channel* c = s.channel(t.channel);
      return c ? s.mixerTrack(c->mixerTrack != kNoId ? c->mixerTrack : s.masterId()) : nullptr;
    }
    default: return nullptr;
  }
}

const Channel* linkedChannel(const Song& s, int track) {
  return modeOf(s, track) == TrackMode::Instrument ? s.channel(s.tracks[static_cast<size_t>(track)].channel) : nullptr;
}

int audioTrackOf(const Song& s, ObjectId mixer) {
  if (mixer == kNoId) return -1;
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i)
    if (s.tracks[static_cast<size_t>(i)].mode == TrackMode::Audio && s.tracks[static_cast<size_t>(i)].mixer == mixer) return i;
  return -1;
}

int instrumentTrackOf(const Song& s, ObjectId channel) {
  if (channel == kNoId) return -1;
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i)
    if (s.tracks[static_cast<size_t>(i)].mode == TrackMode::Instrument && s.tracks[static_cast<size_t>(i)].channel == channel) return i;
  return -1;
}

bool mixerLinked(const Song& s, ObjectId mixer, int exceptTrack) {
  bool found = false;
  forEachTrackList(s, [&](const std::vector<PlaylistTrack>& v, bool current) {
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i].mode != TrackMode::Unassigned && v[i].mixer == mixer && !(current && static_cast<int>(i) == exceptTrack)) found = true;
  });
  return found;
}

bool channelLinked(const Song& s, ObjectId channel, int exceptTrack) {
  bool found = false;
  forEachTrackList(s, [&](const std::vector<PlaylistTrack>& v, bool current) {
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i].mode == TrackMode::Instrument && v[i].channel == channel && !(current && static_cast<int>(i) == exceptTrack)) found = true;
  });
  return found;
}

bool mixerLinkable(const Song& s, ObjectId mixer, std::string* why, int exceptTrack) {
  if (!s.mixerTrack(mixer)) return fail(why, "That mixer track does not exist.");
  if (mixer == s.masterId()) return fail(why, "The master track cannot be an audio track's mixer track.");
  if (mixerLinked(s, mixer, exceptTrack)) return fail(why, "That mixer track already belongs to another audio or instrument track.");
  if (channelUsesMixer(s, mixer, kNoId, false)) return fail(why, "Channels play into that mixer track, so it cannot belong to an audio track.");
  return true;
}

bool channelLinkable(const Song& s, ObjectId channel, std::string* why, int exceptTrack) {
  const Channel* c = s.channel(channel);
  if (!c) return fail(why, "That channel does not exist.");
  if (c->audioClip) return fail(why, "Audio clip channels cannot be instrument tracks.");
  if (c->isLayer()) return fail(why, "Layer channels cannot be instrument tracks.");
  if (!isHostedType(c->plugin.type)) {
    const PluginInfo* info = findPlugin(c->plugin.type);
    if (!info || info->kind != PluginKind::Generator) return fail(why, "Only instruments can be instrument tracks.");
  }
  if (channelLinked(s, channel, exceptTrack)) return fail(why, "That channel already has an instrument track in this project.");
  return true;
}

ObjectId freeMixer(const Song& s) {
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    const MixerTrack& m = s.mixer[i];
    if (insertIsDefault(s, m) && !channelUsesMixer(s, m.id) && !sentInto(s, m.id) && !mixerLinked(s, m.id)) return m.id;
  }
  return kNoId;
}

int freeTrack(const Song& s, int from) {
  for (int i = std::max(0, from); i < kMaxPlaylistTracks; ++i) {
    if (i >= static_cast<int>(s.tracks.size())) return i;
    if (s.tracks[static_cast<size_t>(i)].mode == TrackMode::Unassigned && !trackHasClips(s, i)) return i;
  }
  return -1;
}

bool clipAllowed(const Song& s, ClipKind kind, int track) {
  return kind == ClipKind::Pattern || modeOf(s, track) != TrackMode::Instrument;
}

int recordTrack(const Song& s, ObjectId mixer) {
  const int t = audioTrackOf(s, mixer);
  return t >= 0 && modeOf(s, t) == TrackMode::Audio ? t : -1;
}

std::vector<int> armedTracks(const Song& s) {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i)
    if (modeOf(s, i) == TrackMode::Audio)
      if (const MixerTrack* m = linkedMixer(s, i); m && m->armed) out.push_back(i);
  return out;
}

bool makeAudioTrack(Song& s, int track, ObjectId mixer, std::string* why) {
  if (track < 0 || track >= kMaxPlaylistTracks) return fail(why, "No such playlist track.");
  makeRoom(s, track);
  const PlaylistTrack& cur = s.tracks[static_cast<size_t>(track)];
  if (mixer == kNoId && cur.mode == TrackMode::Audio && s.mixerTrack(cur.mixer)) return true;  // already one
  if (cur.mode == TrackMode::Instrument && s.channel(cur.channel) && trackHasClips(s, track))
    return fail(why, "An instrument track with clips cannot become an audio track.");
  if (mixer == kNoId) {
    mixer = freeMixer(s);
    if (mixer == kNoId) mixer = newInsert(s);
    if (mixer == kNoId) return fail(why, "There is no free mixer track for a new audio track.");
  } else if (!mixerLinkable(s, mixer, why, track)) {
    return false;
  }
  clearLinks(s, track);
  PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  t.mode = TrackMode::Audio;
  t.mixer = mixer;
  MixerTrack& m = *s.mixerTrack(mixer);
  const bool trackColoured = t.color != PlaylistTrack{}.color, mixerColoured = m.color != MixerTrack{}.color;
  if (m.name.empty() && !t.name.empty()) m.name = t.name;
  if (t.name.empty()) t.name = m.name.empty() ? "Audio " + std::to_string(s.mixerIndex(mixer)) : m.name;
  if (trackColoured && !mixerColoured) m.color = t.color;
  else if (!trackColoured && mixerColoured) t.color = m.color;
  return true;
}

bool makeInstrumentTrack(Song& s, int track, ObjectId channel, std::string* why) {
  if (track < 0 || track >= kMaxPlaylistTracks) return fail(why, "No such playlist track.");
  if (!channelLinkable(s, channel, why, track)) return false;
  makeRoom(s, track);
  for (const auto& c : s.playlist)
    if (c.track == track && c.kind != ClipKind::Pattern) return fail(why, "Instrument tracks take pattern clips only: move the audio and automation clips first.");
  Channel& ch = *s.channel(channel);
  // The channel plays into a mixer track of its own.
  ObjectId mix = ch.mixerTrack;
  const bool own = mix != kNoId && mix != s.masterId() && s.mixerTrack(mix) && !channelUsesMixer(s, mix, channel) && !mixerLinked(s, mix, track);
  if (!own) {
    mix = freeMixer(s);
    if (mix == kNoId) mix = newInsert(s);
    if (mix == kNoId) return fail(why, "There is no free mixer track for the instrument.");
    MixerTrack& m = *s.mixerTrack(mix);
    m.name = ch.name;
    m.color = ch.color;
    ch.mixerTrack = mix;
  }
  clearLinks(s, track);
  PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  t.mode = TrackMode::Instrument;
  t.channel = channel;
  t.mixer = kNoId;  // follows the channel's routing
  if (t.name.empty()) t.name = ch.name;
  if (t.color == PlaylistTrack{}.color) t.color = ch.color;
  return true;
}

bool unassign(Song& s, int track) {
  if (!validTrack(s, track)) return false;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  if (t.mode == TrackMode::Unassigned && t.mixer == kNoId && t.channel == kNoId) return false;
  clearLinks(s, track);
  return true;
}

bool setArmed(Song& s, int track, bool armed) {
  if (modeOf(s, track) != TrackMode::Audio) return false;
  MixerTrack* m = s.mixerTrack(s.tracks[static_cast<size_t>(track)].mixer);
  if (m->armed == armed) return false;
  m->armed = armed;
  if (armed && m->input < 0 && m->recPoint == 0) {
    m->input = 0;
    m->inputStereo = true;
  }
  return true;
}

bool setInput(Song& s, int track, int input, bool stereo) {
  if (modeOf(s, track) != TrackMode::Audio) return false;
  MixerTrack* m = s.mixerTrack(s.tracks[static_cast<size_t>(track)].mixer);
  input = std::clamp(input, -1, kMaxInputChannels - 1);
  if (m->input == input && (input < 0 || m->inputStereo == stereo)) return false;
  m->input = input;
  m->inputStereo = stereo;
  if (input < 0 && m->recPoint == 0) m->armed = false;  // nothing left to record
  return true;
}

bool setMonitor(Song& s, int track, InputMonitor mon) {
  if (modeOf(s, track) != TrackMode::Audio) return false;
  MixerTrack* m = s.mixerTrack(s.tracks[static_cast<size_t>(track)].mixer);
  const uint8_t v = static_cast<uint8_t>(mon);
  if (m->monitor == v) return false;
  m->monitor = v;
  return true;
}

int assignMixerToNewTrack(Song& s, ObjectId mixer, std::string* why) {
  if (const int t = audioTrackOf(s, mixer); t >= 0) return t;
  if (!mixerLinkable(s, mixer, why)) return -1;
  const int track = freeTrack(s);
  if (track < 0) return fail(why, "There is no free playlist track."), -1;
  return makeAudioTrack(s, track, mixer, why) ? track : -1;
}

bool unassignMixer(Song& s, ObjectId mixer) {
  bool any = false;
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    if (t.mode == TrackMode::Audio && t.mixer == mixer) any = unassign(s, i) || any;
  }
  return any;
}

int assignChannelToNewTrack(Song& s, ObjectId channel, std::string* why) {
  if (const int t = instrumentTrackOf(s, channel); t >= 0) return t;
  if (!channelLinkable(s, channel, why)) return -1;
  const int track = freeTrack(s);
  if (track < 0) return fail(why, "There is no free playlist track."), -1;
  return makeInstrumentTrack(s, track, channel, why) ? track : -1;
}

bool unassignChannel(Song& s, ObjectId channel) {
  bool any = false;
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    if (t.mode == TrackMode::Instrument && t.channel == channel) any = unassign(s, i) || any;
  }
  return any;
}

int placeAudioClip(Song& s, int track, ObjectId channel) {
  track = std::clamp(track, 0, kMaxPlaylistTracks - 1);
  if (!clipAllowed(s, ClipKind::Audio, track)) {
    int k = freeTrack(s, track + 1);
    while (k >= 0 && !clipAllowed(s, ClipKind::Audio, k)) k = freeTrack(s, k + 1);
    track = k >= 0 ? k : track;
    makeRoom(s, track);
  }
  if (modeOf(s, track) == TrackMode::Audio)
    if (Channel* c = s.channel(channel)) c->mixerTrack = s.tracks[static_cast<size_t>(track)].mixer;
  return track;
}

int dropStaleLinks(Song& s) {
  int n = 0;
  forEachTrackList(s, [&](std::vector<PlaylistTrack>& v, bool) {
    for (auto& t : v) {
      const bool mixerOk = t.mixer == kNoId || s.mixerTrack(t.mixer);
      const bool chanOk = t.channel == kNoId || s.channel(t.channel);
      const bool modeOk = t.mode == TrackMode::Unassigned || (t.mode == TrackMode::Audio && t.mixer != kNoId && mixerOk) ||
                          (t.mode == TrackMode::Instrument && t.channel != kNoId && chanOk);
      if (mixerOk && chanOk && modeOk) continue;
      t.mode = TrackMode::Unassigned;
      t.mixer = kNoId;
      t.channel = kNoId;
      ++n;
    }
  });
  return n;
}

// ---- Document wrappers ----

bool makeAudioTrack(Document& d, int track, ObjectId mixer, std::string* why) {
  return runEdit(d, "make audio track", [&](Song& s) { return makeAudioTrack(s, track, mixer, why); });
}

bool makeInstrumentTrack(Document& d, int track, ObjectId channel, std::string* why) {
  return runEdit(d, "make instrument track", [&](Song& s) { return makeInstrumentTrack(s, track, channel, why); });
}

ObjectId newInstrumentTrack(Document& d, int track, const std::string& pluginType, std::string* why) {
  const PluginInfo* info = findPlugin(pluginType);
  if (!isHostedType(pluginType) && (!info || info->kind != PluginKind::Generator)) return fail(why, "Only instruments can be instrument tracks."), kNoId;
  if (track < 0 || track >= kMaxPlaylistTracks) return fail(why, "No such playlist track."), kNoId;
  for (const auto& c : d.song().playlist)
    if (c.track == track && c.kind != ClipKind::Pattern)
      return fail(why, "Instrument tracks take pattern clips only: move the audio and automation clips first."), kNoId;
  d.beginGroup("new instrument track");
  const ObjectId ch = d.addChannel(info ? info->name : pluginType, pluginType);
  const bool ok = ch != kNoId && makeInstrumentTrack(d, track, ch, why);
  d.endGroup();
  if (!ok && ch != kNoId) d.undo();
  return ok ? ch : kNoId;
}

bool unassign(Document& d, int track) {
  return runEdit(d, "clear track mode", [&](Song& s) { return unassign(s, track); });
}

bool setArmed(Document& d, int track, bool armed) {
  return runEdit(d, armed ? "arm audio track" : "disarm audio track", [&](Song& s) { return setArmed(s, track, armed); });
}

bool setInput(Document& d, int track, int input, bool stereo) {
  return runEdit(d, "audio track input", [&](Song& s) { return setInput(s, track, input, stereo); });
}

bool setMonitor(Document& d, int track, InputMonitor m) {
  return runEdit(d, "audio track monitoring", [&](Song& s) { return setMonitor(s, track, m); });
}

int assignMixerToNewTrack(Document& d, ObjectId mixer, std::string* why) {
  int out = -1;
  runEdit(d, "assign to audio track", [&](Song& s) { return (out = assignMixerToNewTrack(s, mixer, why)) >= 0; });
  return out;
}

bool unassignMixer(Document& d, ObjectId mixer) {
  return runEdit(d, "unassign audio track", [&](Song& s) { return unassignMixer(s, mixer); });
}

int assignChannelToNewTrack(Document& d, ObjectId channel, std::string* why) {
  int out = -1;
  runEdit(d, "assign to instrument track", [&](Song& s) { return (out = assignChannelToNewTrack(s, channel, why)) >= 0; });
  return out;
}

bool unassignChannel(Document& d, ObjectId channel) {
  return runEdit(d, "unassign instrument track", [&](Song& s) { return unassignChannel(s, channel); });
}

}  // namespace gl::trackmodes
