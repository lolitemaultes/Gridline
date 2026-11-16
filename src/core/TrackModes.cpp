// Gridline — playlist track modes (PL-08). See TrackModes.h.
#include "TrackModes.h"
#include <algorithm>
#include <functional>
#include <set>
#include "Document.h"
#include "PluginInfo.h"
#include "PluginParams.h"
#include "TrackOrder.h"

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

void addReturnSends(Song& s, ObjectId mixer);  // (checkpoint 52, below)
void freeGroupMixer(Song& s, ObjectId gid);    // (checkpoint 52, below)

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

std::vector<ObjectId> playedMixers(const Song& s, int track) {
  std::vector<ObjectId> out;
  if (const MixerTrack* m = linkedMixer(s, track)) return {m->id};
  auto take = [&](ObjectId channel) {
    const Channel* c = s.channel(channel);
    if (!c) return;
    const ObjectId m = c->mixerTrack != kNoId ? c->mixerTrack : s.masterId();
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  };
  for (const Clip& c : s.playlist) {
    if (c.track != track) continue;
    if (c.kind == ClipKind::Audio) {
      take(c.source);
    } else if (c.kind == ClipKind::Pattern) {
      const Pattern* p = s.pattern(c.pattern);
      if (!p) continue;
      for (const auto& [ch, v] : p->steps)
        if (std::any_of(v.begin(), v.end(), [](uint8_t x) { return x != 0; })) take(ch);
      for (const auto& [ch, v] : p->notes)
        if (!v.empty()) take(ch);
    }
  }
  return out;
}

ObjectId outputOf(const MixerTrack& m) {
  for (const Send& sd : m.sends)
    if (!sd.sidechain && !sd.stageRoom) return sd.to;
  return kNoId;
}

std::vector<size_t> returnTracks(const Song& s) {
  std::set<ObjectId> aux;
  for (const MixerTrack& m : s.mixer) {
    const ObjectId out = outputOf(m);
    for (const Send& sd : m.sends)
      if (!sd.sidechain && !sd.stageRoom && sd.to != out) aux.insert(sd.to);
  }
  std::vector<size_t> rows;
  for (size_t j = 1; j < s.mixer.size(); ++j)
    if (aux.count(s.mixer[j].id)) rows.push_back(j);
  return rows;
}

const MixerTrack* playedMixer(const Song& s, int track) {
  // An instrument on Main has no strip of its own (its fader would be Main's).
  if (const MixerTrack* m = linkedMixer(s, track)) return m->id == s.masterId() ? nullptr : m;
  const std::vector<ObjectId> v = playedMixers(s, track);
  if (v.size() == 1) return v[0] != s.masterId() ? s.mixerTrack(v[0]) : nullptr;
  if (v.empty()) return nullptr;
  // Several: the bus they all route into, when there is one (N45-05).
  ObjectId bus = kNoId;
  for (ObjectId id : v) {
    const MixerTrack* m = s.mixerTrack(id);
    const ObjectId out = m && id != s.masterId() ? outputOf(*m) : kNoId;
    if (out == kNoId || out == s.masterId() || (bus != kNoId && out != bus)) return nullptr;
    bus = out;
  }
  return s.mixerTrack(bus);
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

int usedTrackCount(const Song& s) {
  int last = -1;
  for (const Clip& c : s.playlist) last = std::max(last, c.track);
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t)
    if (s.tracks[static_cast<size_t>(t)].mode != TrackMode::Unassigned || !s.tracks[static_cast<size_t>(t)].name.empty()) last = std::max(last, t);
  // K-112c (round 42): never fewer than kShownTracks (as many as the song has): a new set shows that
  // many empty tracks with empty slots in both views, as the reference's new set starts with four.
  return std::min(std::max(last + 1, kShownTracks), static_cast<int>(s.tracks.size()));
}

int addUsedTrack(Song& s) {
  const int t = usedTrackCount(s);
  if (t >= kMaxPlaylistTracks) return -1;
  if (t >= static_cast<int>(s.tracks.size())) s.tracks.resize(static_cast<size_t>(t) + 1);
  s.tracks[static_cast<size_t>(t)].name = "Track " + std::to_string(t + 1);
  return t;
}

void makeClipsStarterTracks(Song& s) {
  struct Start {
    const char* name;
    Rgb color;
    bool audio;
  };
  const Start st[] = {{"1 MIDI", 0x4FA8B8, false}, {"2 MIDI", 0x58B394, false}, {"3 Audio", 0x86B04C, true}, {"4 Audio", 0xC8B23E, true}};
  makeRoom(s, 3);
  for (int i = 0; i < 4; ++i) {
    PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    if (t.mode != TrackMode::Unassigned || !t.name.empty()) continue;  // a track already in use stays as it is
    t.name = st[i].name;
    t.color = st[i].color;
    if (st[i].audio) makeAudioTrack(s, i, kNoId, nullptr);
  }
  // The returns, as the reference's new set: a reverb and a delay, every audio track sending to both
  // at zero (turn a send up to hear it).
  struct Ret {
    const char* name;
    Rgb color;
    const char* type;
    int dry, wet;
  };
  const Ret rets[] = {{"Reverb", 0xD98A78, "gl.reverb", pp::RvbDry, pp::RvbWet}, {"Delay", 0xCF86B0, "gl.delay", pp::DlyDry, pp::DlyWet}};
  for (const Ret& r : rets) {
    bool have = false;
    for (const MixerTrack& m : s.mixer) have = have || m.name == r.name;
    if (have) continue;
    const ObjectId id = freeMixer(s);
    MixerTrack* m = nullptr;
    for (MixerTrack& x : s.mixer)
      if (x.id == id) m = &x;
    if (!m) break;
    m->name = r.name;
    m->color = r.color;
    PluginState ps = defaultPluginState(r.type);
    if (static_cast<size_t>(r.dry) < ps.params.size()) ps.params[static_cast<size_t>(r.dry)] = 0.0f;
    if (static_cast<size_t>(r.wet) < ps.params.size()) ps.params[static_cast<size_t>(r.wet)] = 1.0f;
    m->slots[0] = EffectSlot{std::move(ps), true, 1.0f};
    for (int i = 0; i < 4; ++i)
      if (modeOf(s, i) == TrackMode::Audio)
        if (const MixerTrack* lm = linkedMixer(s, i))
          for (MixerTrack& from : s.mixer)
            if (from.id == lm->id) {
              Send sd;
              sd.to = id;
              sd.level = 0.0f;
              from.sends.push_back(sd);
            }
  }
}

ObjectId addReturnTrack(Song& s, const std::string& name) {
  const ObjectId id = freeMixer(s);
  MixerTrack* m = nullptr;
  for (MixerTrack& x : s.mixer)
    if (x.id == id) m = &x;
  if (!m) return kNoId;
  m->name = name;
  m->color = 0xC79A6A;
  std::set<ObjectId> from;  // every shown track's mixer track (its own, or its instrument's)
  const int n = usedTrackCount(s);
  for (int i = 0; i < n; ++i)
    if (const MixerTrack* pm = playedMixer(s, i); pm && pm->id != s.masterId() && pm->id != id) from.insert(pm->id);
  for (MixerTrack& x : s.mixer)
    if (from.count(x.id)) {
      Send sd;
      sd.to = id;
      sd.level = 0.0f;
      x.sends.push_back(sd);
    }
  return id;
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
  routeIntoItsGroup(s, track);  // checkpoint 52: a group's member plays into the group
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
  routeIntoItsGroup(s, track);  // checkpoint 52: an instrument given to a group's member plays into the group
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
  if (armed && m->recordSafe) return false;  // K-123: record safe
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

int addUsedTrack(Document& d, int count) {
  int t = -1;
  runEdit(d, count > 1 ? "new tracks" : "new track", [&](Song& s) {
    for (int i = 0; i < std::max(1, count); ++i) {
      const int k = addUsedTrack(s);
      if (k < 0) break;
      t = k;
    }
    return t >= 0;
  });
  return t;
}

namespace {
// N45-08: the track colours new Clips tracks take in turn (the starter tracks are the first four).
constexpr Rgb kTrackCycle[] = {0x4FA8B8, 0x58B394, 0x86B04C, 0xC8B23E, 0xDE9A45, 0xD97862, 0xD4729A, 0xAE82C8,
                               0x8589D4, 0x5E97D2, 0x6CB8D6, 0x9DC06A, 0xCDAA72, 0x9AA3AE};
constexpr int kTrackCycleN = static_cast<int>(sizeof kTrackCycle / sizeof kTrackCycle[0]);

// A name the Clips layout gave a track by its place ("3 Audio", "12 MIDI"): its number, else 0.
int placeNumber(const std::string& n, std::string* kind) {
  size_t i = 0;
  while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i;
  if (i == 0 || i > 3 || i >= n.size() || n[i] != ' ') return 0;
  const std::string rest = n.substr(i + 1);
  if (rest != "Audio" && rest != "MIDI" && rest != "Group") return 0;  // (checkpoint 51: and group tracks)
  if (kind) *kind = rest;
  return std::stoi(n.substr(0, i));
}

// A new empty track at `at`, the tracks from there moving down one (their clips with them, a group
// around it growing), as the arrangement's Insert track. The tracks named by their place are renamed
// for their new place, as the reference numbers its unnamed tracks. False when the list is full.
bool insertTrackAt(Song& s, int at) {
  if (at < 0 || at > static_cast<int>(s.tracks.size())) return false;
  if (static_cast<int>(s.tracks.size()) >= kMaxPlaylistTracks) {
    const int last = static_cast<int>(s.tracks.size()) - 1;
    const PlaylistTrack& lt = s.tracks.back();
    if (last < at || lt.mode != TrackMode::Unassigned || !lt.name.empty() || trackHasClips(s, last)) return false;
    s.tracks.pop_back();  // an unused track at the very end makes the room
  }
  for (int g = 0; g < at; ++g)
    if (const int gs = s.tracks[static_cast<size_t>(g)].groupSize; gs > 0 && at <= g + gs) ++s.tracks[static_cast<size_t>(g)].groupSize;
  s.tracks.insert(s.tracks.begin() + at, PlaylistTrack{});
  for (Clip& c : s.playlist)
    if (c.track >= at) ++c.track;
  for (int i = at + 1; i < static_cast<int>(s.tracks.size()); ++i) {
    std::string kind;
    PlaylistTrack& tr = s.tracks[static_cast<size_t>(i)];
    if (placeNumber(tr.name, &kind) != i) continue;
    const std::string was = tr.name;
    tr.name = std::to_string(i + 1) + " " + kind;
    // N47-09: its mixer track and instrument named after it follow (they are one thing in Clips; Studio,
    // Smooth and the routing lists show their names).
    for (MixerTrack& m : s.mixer)
      if (m.id == tr.mixer && m.name == was) m.name = tr.name;
    for (Channel& c : s.channels)
      if (c.id == tr.channel && c.name == was) c.name = tr.name;
  }
  return true;
}

int addNamedTrack(Document& d, bool audio, int after) {
  int t = -1;
  runEdit(d, audio ? "new audio track" : "new MIDI track", [&](Song& s) {
    const int used = usedTrackCount(s);
    // Checkpoint 52: a group selected: the new track goes after its tracks, in the group the group is in;
    // a group's track selected: it joins that group (its last track too).
    int home = after >= 0 && after < static_cast<int>(s.tracks.size()) ? groupTrackOf(s, after) : -1;
    if (after >= 0 && isGroupTrack(s, after)) after += s.tracks[static_cast<size_t>(after)].groupSize;
    if (after >= 0 && after < used - 1) {
      // N45-08: right after the selected track, as the reference.
      t = after + 1;
      if (!insertTrackAt(s, t)) return t = -1, false;
    } else {
      t = used;
      // A shown empty track (a new set's fourth, say) is taken first: it is the next free one in view.
      for (int k = 0; k < t; ++k)
        if (k >= static_cast<int>(s.tracks.size()) || (s.tracks[static_cast<size_t>(k)].mode == TrackMode::Unassigned && s.tracks[static_cast<size_t>(k)].name.empty() && !trackHasClips(s, k))) {
          t = k;
          break;
        }
      if (t >= kMaxPlaylistTracks) return t = -1, false;
      makeRoom(s, t);
    }
    if (t == after + 1)
      for (int g = home; g >= 0; g = groupTrackOf(s, g))
        if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; g + gs < t) gs = t - g;
    PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    tr.name = std::to_string(t + 1) + (audio ? " Audio" : " MIDI");
    // The next colour of the cycle (counted over the tracks in use, so every peer picks the same).
    tr.color = kTrackCycle[(usedTrackCount(s) - 1 + kTrackCycleN) % kTrackCycleN];
    if (audio && !makeAudioTrack(s, t, kNoId, nullptr)) return t = -1, false;
    if (audio) addReturnSends(s, s.tracks[static_cast<size_t>(t)].mixer);  // checkpoint 52 (N51-05): as every track of the set
    return true;
  });
  return t;
}
}  // namespace

bool makeNamedAudioTrack(Song& s, int t) {
  if (t < 0 || t >= kMaxPlaylistTracks) return false;
  makeRoom(s, t);
  PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
  if (tr.mode != TrackMode::Unassigned) return false;
  if (tr.name.empty() || tr.name == "Track " + std::to_string(t + 1)) tr.name = std::to_string(t + 1) + " Audio";
  if (tr.color == PlaylistTrack{}.color) tr.color = kTrackCycle[(usedTrackCount(s) - 1 + kTrackCycleN) % kTrackCycleN];
  if (!makeAudioTrack(s, t, kNoId, nullptr)) return false;
  addReturnSends(s, s.tracks[static_cast<size_t>(t)].mixer);
  return true;
}

int addAudioTrack(Document& d, int after) { return addNamedTrack(d, true, after); }
int addMidiTrack(Document& d, int after) { return addNamedTrack(d, false, after); }

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

// ---- K-112c (checkpoint 51): Clips group tracks ----
bool isGroupTrack(const Song& s, int track) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return false;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  if (t.groupSize <= 0 || t.mode != TrackMode::Audio) return false;
  // Checkpoint 52: automation clips (the group's own fader and devices) leave it a group.
  for (const auto& c : s.playlist)
    if (c.track == track && c.kind != ClipKind::Automation) return false;
  return true;
}

int groupTrackOf(const Song& s, int track) {
  for (int g = track - 1; g >= 0; --g)
    if (isGroupTrack(s, g) && track <= g + s.tracks[static_cast<size_t>(g)].groupSize) return g;
  return -1;
}

namespace {
// Checkpoint 52 (N51-05): the mixer track sends to every return track (at zero) it does not send to yet,
// as every track of a Clips set does.
void addReturnSends(Song& s, ObjectId mixer) {
  MixerTrack* m = s.mixerTrack(mixer);
  if (!m || mixer == s.masterId()) return;
  std::vector<ObjectId> rets;
  for (size_t r : returnTracks(s))
    if (r < s.mixer.size() && s.mixer[r].id != mixer) rets.push_back(s.mixer[r].id);
  for (ObjectId to : rets)
    if (std::none_of(m->sends.begin(), m->sends.end(), [to](const Send& x) { return x.to == to; })) m->sends.push_back(Send{to, 0.0f, false});
}

// The member's sound into the group: its mixer track's output (an instrument playing into Main first
// gets a free insert of its own, so the member keeps a fader of its own).
void routeIntoGroup(Song& s, int t, ObjectId group) {
  const ObjectId master = s.masterId();
  if (const Channel* lc = linkedChannel(s, t); lc && (lc->mixerTrack == master || !s.mixerTrack(lc->mixerTrack))) {
    ObjectId ins = freeMixer(s);
    if (ins == kNoId) ins = newInsert(s);
    if (ins == kNoId) return;
    Channel* c = s.channel(lc->id);
    c->mixerTrack = ins;
    if (MixerTrack* m = s.mixerTrack(ins)) {
      if (m->name.empty()) m->name = c->name;
      m->color = c->color;
    }
  }
  const MixerTrack* pm = playedMixer(s, t);
  if (!pm || pm->id == master || pm->id == group) return;
  const ObjectId mid = pm->id;
  {  // Checkpoint 52: a mixer track other tracks outside the group play through too stays where it plays
     // (rerouting it would pull their sound into the group: the showcase's Risers strip lost its fader).
    int gh = -1;
    for (int g = 0; g < static_cast<int>(s.tracks.size()) && gh < 0; ++g)
      if (s.tracks[static_cast<size_t>(g)].mixer == group && s.tracks[static_cast<size_t>(g)].groupSize > 0) gh = g;
    const int ge = gh >= 0 ? gh + s.tracks[static_cast<size_t>(gh)].groupSize : -1;
    const int used = std::min(usedTrackCount(s), static_cast<int>(s.tracks.size()));
    for (int u = 0; u < used; ++u) {
      if (u == t || (u > gh && u <= ge)) continue;
      const std::vector<ObjectId> v = playedMixers(s, u);
      const MixerTrack* up = playedMixer(s, u);
      if (std::find(v.begin(), v.end(), mid) != v.end() || (up && up->id == mid)) return;
    }
  }
  MixerTrack* m = s.mixerTrack(mid);
  bool set = false;
  for (Send& sd : m->sends)
    if (!sd.sidechain && !sd.stageRoom) {
      sd.to = group;
      set = true;
      break;
    }
  if (!set) m->sends.insert(m->sends.begin(), Send{group, 1.0f, false});
  // (a send to the group as well as the output would play it twice)
  for (size_t k = 1; k < m->sends.size();)
    if (m->sends[k].to == group && !m->sends[k].sidechain && !m->sends[k].stageRoom) m->sends.erase(m->sends.begin() + static_cast<long>(k));
    else ++k;
  addReturnSends(s, mid);
}

// The group track whose mixer track is `mixer` (-1: none).
int groupWithMixer(const Song& s, ObjectId mixer) {
  if (mixer == kNoId) return -1;
  for (int g = 0; g < static_cast<int>(s.tracks.size()); ++g)
    if (s.tracks[static_cast<size_t>(g)].mixer == mixer && isGroupTrack(s, g)) return g;
  return -1;
}
}  // namespace

int syncGroupRouting(Song& s) {
  int n = 0;
  const ObjectId master = s.masterId();
  const int used = std::min(usedTrackCount(s), static_cast<int>(s.tracks.size()));
  for (int t = 0; t < used; ++t) {
    const MixerTrack* pm = playedMixer(s, t);
    if (!pm || pm->id == master) continue;
    const int own = groupTrackOf(s, t);
    const ObjectId out = outputOf(*pm);
    const int into = groupWithMixer(s, out);
    if (into >= 0 && into != own) {  // plays into a group it is not in (N51-06): its own group, else Main
      MixerTrack* m = s.mixerTrack(pm->id);
      const ObjectId to = own >= 0 ? s.tracks[static_cast<size_t>(own)].mixer : master;
      for (Send& sd : m->sends)
        if (!sd.sidechain && !sd.stageRoom) {
          sd.to = to;
          break;
        }
      ++n;
    } else if (own >= 0 && out == master) {  // moved into a group: it plays into the group
      routeIntoItsGroup(s, t);
      ++n;
    }
  }
  return n;
}

void routeIntoItsGroup(Song& s, int track) {
  const int g = groupTrackOf(s, track);
  if (g < 0) return;
  const ObjectId gid = s.tracks[static_cast<size_t>(g)].mixer;
  if (s.mixerTrack(gid)) routeIntoGroup(s, track, gid);
}

namespace {
// The tracks named by their place take their place's number again (after a group moved them), their
// mixer track and instrument with them, as insertTrackAt's.
void renumberPlaces(Song& s) {
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    std::string kind;
    PlaylistTrack& tr = s.tracks[static_cast<size_t>(i)];
    const int n = placeNumber(tr.name, &kind);
    if (n <= 0 || n == i + 1) continue;
    const std::string was = tr.name;
    tr.name = std::to_string(i + 1) + " " + kind;
    const Channel* lc = s.channel(tr.channel);
    const ObjectId cm = lc ? lc->mixerTrack : kNoId;  // N51-11: an instrument's own mixer track too
    for (MixerTrack& m : s.mixer)
      if ((m.id == tr.mixer || (m.id == cm && cm != s.masterId())) && m.name == was) m.name = tr.name;
    for (Channel& c : s.channels)
      if (c.id == tr.channel && c.name == was) c.name = tr.name;
  }
}
}  // namespace

void renumberTrackNames(Song& s) { renumberPlaces(s); }

int groupTracks(Song& s, std::vector<int> tracks, std::string* why) {
  std::sort(tracks.begin(), tracks.end());
  tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
  const int used = usedTrackCount(s);
  if (tracks.empty()) return fail(why, "Select the tracks to group."), -1;
  for (int t : tracks) {
    if (t < 0 || t >= used || t >= static_cast<int>(s.tracks.size())) return fail(why, "No such track."), -1;
    if (s.tracks[static_cast<size_t>(t)].groupSize > 0 && !isGroupTrack(s, t)) return fail(why, "A track holding a playlist group cannot be grouped."), -1;
  }
  // Checkpoint 52: a group can hold groups, as the reference's. A selected group brings its tracks;
  // the outermost of the selection must sit in the same group (or in none): the new group goes there.
  const std::set<int> all = trackorder::withGroups(s, tracks);
  int parent = -2;
  for (int t : all) {
    const int p = groupTrackOf(s, t);
    if (p >= 0 && all.count(p)) continue;  // inside a selected group
    if (parent == -2) parent = p;
    else if (parent != p) return fail(why, "Tracks from different groups cannot be grouped together."), -1;
  }
  if (static_cast<int>(s.tracks.size()) >= kMaxPlaylistTracks) return fail(why, "There is no room for another track."), -1;
  const int at = *all.begin(), n = static_cast<int>(all.size());
  if (*all.rbegin() - at + 1 != n) {  // together first, at the first one's place
    const trackorder::PlaylistMove mv = trackorder::playlistOrderAfterMove(s, std::vector<int>(all.begin(), all.end()), at);
    if (mv.order.empty() || !trackorder::reorderPlaylist(s, mv)) return fail(why, "The tracks cannot be moved together."), -1;
  }
  const Rgb col = s.tracks[static_cast<size_t>(at)].color;
  if (!insertTrackAt(s, at)) return fail(why, "There is no room for another track."), -1;
  {
    PlaylistTrack& g = s.tracks[static_cast<size_t>(at)];
    g.name = std::to_string(at + 1) + " Group";
    g.color = col;
    g.groupSize = n;
  }
  if (!makeAudioTrack(s, at, kNoId, why)) return -1;
  const ObjectId gid = s.tracks[static_cast<size_t>(at)].mixer;
  if (MixerTrack* m = s.mixerTrack(gid)) {
    m->name = s.tracks[static_cast<size_t>(at)].name;
    m->color = col;
    std::vector<ObjectId> rets;
    for (size_t r : returnTracks(s))
      if (r < s.mixer.size() && s.mixer[r].id != gid) rets.push_back(s.mixer[r].id);
    for (ObjectId to : rets)  // the group's sends, at zero, as every track's
      if (std::none_of(m->sends.begin(), m->sends.end(), [to](const Send& x) { return x.to == to; })) m->sends.push_back(Send{to, 0.0f, false});
  }
  for (int t = at + 1; t <= at + n; ++t)
    if (groupTrackOf(s, t) == at) routeIntoGroup(s, t, gid);  // the new group's own tracks (a group inside plays on into its own)
  renumberPlaces(s);
  return at;
}

bool ungroupTracks(Song& s, int group, std::string* why) {
  if (!isGroupTrack(s, group)) return fail(why, "Select a group track to ungroup.");
  // What played into the group plays where the group played; the other sends to it go; the group's
  // own automation goes with it (S07: a later track reusing the mixer track must not inherit it).
  freeGroupMixer(s, s.tracks[static_cast<size_t>(group)].mixer);
  clearLinks(s, group);
  for (int g = 0; g < group; ++g)  // checkpoint 52: a group around it is one track shorter
    if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; gs > 0 && group <= g + gs) --gs;
  s.tracks.erase(s.tracks.begin() + group);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [group](const Clip& c) { return c.track == group; }), s.playlist.end());
  for (Clip& c : s.playlist)
    if (c.track > group) --c.track;
  renumberPlaces(s);
  return true;
}

namespace {
// The tracks with every group among them whole (a group brings its tracks), sorted.
std::vector<int> withWholeGroups(const Song& s, const std::vector<int>& tracks) {
  std::set<int> all;
  const int n = static_cast<int>(s.tracks.size());
  for (int t : tracks) {
    if (t < 0 || t >= n) continue;
    all.insert(t);
    if (isGroupTrack(s, t))
      for (int k = 1; k <= s.tracks[static_cast<size_t>(t)].groupSize && t + k < n; ++k) all.insert(t + k);
  }
  return std::vector<int>(all.begin(), all.end());
}

// The group's mixer track is free again; what still played into it plays where the group played.
void freeGroupMixer(Song& s, ObjectId gid) {
  MixerTrack* gm = s.mixerTrack(gid);
  if (!gm || gid == s.masterId()) return;
  ObjectId out = outputOf(*gm);
  if (out == kNoId || out == gid) out = s.masterId();
  for (MixerTrack& m : s.mixer) {
    if (m.id == gid) continue;
    for (Send& sd : m.sends)
      if (sd.to == gid && !sd.sidechain && !sd.stageRoom && outputOf(m) == gid) {
        sd.to = out;
        break;
      }
    m.sends.erase(std::remove_if(m.sends.begin(), m.sends.end(), [gid](const Send& x) { return x.to == gid; }), m.sends.end());
  }
  for (Channel& c : s.channels)
    if (c.mixerTrack == gid) c.mixerTrack = out;
  std::set<ObjectId> autos;
  for (const AutomationClip& ac : s.automations)
    if (ac.owner == gid) autos.insert(ac.id);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                  [&](const Clip& c) { return c.kind == ClipKind::Automation && autos.count(c.source) > 0; }),
                   s.playlist.end());
  s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(), [&](const AutomationClip& ac) { return autos.count(ac.id) > 0; }),
                      s.automations.end());
  MixerTrack fresh;
  fresh.id = gid;
  fresh.dock = gm->dock;
  fresh.sends.push_back({s.masterId(), 1.0f, false});
  *s.mixerTrack(gid) = fresh;
}
}  // namespace

int deleteTracks(Song& s, const std::vector<int>& tracks, std::string* why) {
  const std::vector<int> del = withWholeGroups(s, tracks);
  if (del.empty()) return fail(why, "Select the tracks to delete."), 0;
  if (static_cast<int>(del.size()) >= static_cast<int>(s.tracks.size())) return fail(why, "The last track cannot be deleted."), 0;
  for (auto it = del.rbegin(); it != del.rend(); ++it) {
    const int t = *it;
    if (isGroupTrack(s, t)) freeGroupMixer(s, s.tracks[static_cast<size_t>(t)].mixer);
    for (int g = 0; g < t; ++g)  // the groups around it are a track shorter
      if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; gs > 0 && t <= g + gs) --gs;
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [t](const Clip& c) { return c.track == t; }), s.playlist.end());
    for (Clip& c : s.playlist)
      if (c.track > t) --c.track;
    s.tracks.erase(s.tracks.begin() + t);
  }
  renumberPlaces(s);
  return static_cast<int>(del.size());
}

int duplicateTracks(Song& s, const std::vector<int>& tracks, std::string* why) {
  const std::vector<int> src = withWholeGroups(s, tracks);
  if (src.empty()) return fail(why, "Select the tracks to duplicate."), -1;
  const int first = src.front(), last = src.back(), n = static_cast<int>(src.size());
  if (last - first + 1 != n) return fail(why, "Duplicate tracks next to each other."), -1;
  if (static_cast<int>(s.tracks.size()) + n > kMaxPlaylistTracks) return fail(why, "There is no room for more tracks."), -1;
  const int home = groupTrackOf(s, first);  // the group the copies go in (the originals')
  std::vector<PlaylistTrack> copies;
  std::vector<int> sizes;
  for (int t : src) {
    PlaylistTrack k = s.tracks[static_cast<size_t>(t)];
    sizes.push_back(isGroupTrack(s, t) ? k.groupSize : 0);
    k.groupSize = 0;
    copies.push_back(k);
  }
  const int at = last + 1;
  for (int i = 0; i < n; ++i) {
    if (!insertTrackAt(s, at + i)) return fail(why, "There is no room for more tracks."), -1;
    PlaylistTrack& k = s.tracks[static_cast<size_t>(at + i)];
    k = copies[static_cast<size_t>(i)];
    const bool audio = k.mode == TrackMode::Audio;
    k.mode = TrackMode::Unassigned;  // a mixer track or channel belongs to one track (as Studio's clone)
    k.mixer = k.channel = kNoId;
    k.laneShown = k.laneSet = false;
    k.groupSize = audio ? -1 : 0;  // (marks the audio ones below)
  }
  for (int g = home; g >= 0; g = groupTrackOf(s, g))  // the copies stay in the originals' group
    if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; g + gs < at + n - 1) gs = at + n - 1 - g;
  std::vector<char> audio(static_cast<size_t>(n), 0);
  for (int i = 0; i < n; ++i) {
    PlaylistTrack& k = s.tracks[static_cast<size_t>(at + i)];
    audio[static_cast<size_t>(i)] = k.groupSize < 0;
    k.groupSize = sizes[static_cast<size_t>(i)];
  }
  // Outer tracks first, so each copy's group has its mixer track when the copy is routed into it.
  for (int i = 0; i < n; ++i) {
    if (!audio[static_cast<size_t>(i)]) continue;
    if (!makeAudioTrack(s, at + i, kNoId, why)) return -1;
    if (sizes[static_cast<size_t>(i)] > 0) {
      const ObjectId gid = s.tracks[static_cast<size_t>(at + i)].mixer;
      if (MixerTrack* m = s.mixerTrack(gid)) {
        const MixerTrack* om = s.mixerTrack(s.tracks[static_cast<size_t>(src[static_cast<size_t>(i)])].mixer);
        m->color = s.tracks[static_cast<size_t>(at + i)].color;
        if (om) m->volume = om->volume, m->pan = om->pan;
      }
      addReturnSends(s, gid);
    }
  }
  std::vector<Clip> clips;
  for (const Clip& c : s.playlist)
    if (c.track >= first && c.track <= last) {
      Clip k = c;
      k.track = c.track + n;
      k.takeGroup = kNoId, k.take = 0;
      clips.push_back(k);
    }
  for (Clip& k : clips) {
    k.id = s.allocateId();
    s.playlist.push_back(k);
  }
  renumberPlaces(s);
  return at;
}

int deleteTracks(Document& d, const std::vector<int>& tracks, std::string* why) {
  int out = 0;
  runEdit(d, "delete tracks", [&](Song& s) { return (out = deleteTracks(s, tracks, why)) > 0; });
  return out;
}

int duplicateTracks(Document& d, const std::vector<int>& tracks, std::string* why) {
  int out = -1;
  runEdit(d, "duplicate tracks", [&](Song& s) { return (out = duplicateTracks(s, tracks, why)) >= 0; });
  return out;
}

int syncGroupRouting(Document& d) {
  int out = 0;
  runEdit(d, "group routing", [&](Song& s) {
    auto names = [](const Song& x) {
      std::vector<std::string> v;
      for (const PlaylistTrack& t : x.tracks) v.push_back(t.name);
      return v;
    };
    const std::vector<std::string> before = names(s);
    out = syncGroupRouting(s);
    renumberPlaces(s);  // N51-11: a move renumbers the place-named tracks, as insert and delete do
    return out > 0 || names(s) != before;
  });
  return out;
}

int groupTracks(Document& d, const std::vector<int>& tracks, std::string* why) {
  int out = -1;
  runEdit(d, "group tracks", [&](Song& s) { return (out = groupTracks(s, tracks, why)) >= 0; });
  return out;
}

bool ungroupTracks(Document& d, int group, std::string* why) {
  return runEdit(d, "ungroup tracks", [&](Song& s) { return ungroupTracks(s, group, why); });
}

}  // namespace gl::trackmodes
