// Gridline — playlist track modes (PL-08). See TrackModes.h.
#include "TrackModes.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include "AutomationEdit.h"
#include "Document.h"
#include "Meter.h"
#include "Arrangements.h"
#include "Params.h"
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
void releaseMixer(Song& s, ObjectId gid, std::set<int>* emptied);  // (checkpoint 53, below)
void eraseTracks(Song& s, const std::set<int>& gone);
std::set<int> emptyLanes(const Song& s, const std::set<int>& emptied);

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
  // K-142 (0.7.8): a channel or a pattern met again adds nothing new (its mixer track is already in
  // the list, in the place it first came), so each is looked at once: a large project's track
  // repeats a few patterns hundreds of times (the Clips grid asks this for every strip, every frame).
  // (small lists, looked through and kept from call to call: no allocation per call, every frame)
  static thread_local std::vector<ObjectId> seenChannels, seenPatterns;
  seenChannels.clear(), seenPatterns.clear();
  auto once = [](std::vector<ObjectId>& seen, ObjectId id) {
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) return false;
    seen.push_back(id);
    return true;
  };
  auto take = [&](ObjectId channel) {
    if (!once(seenChannels, channel)) return;
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
      if (!once(seenPatterns, c.pattern)) continue;
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
    // Every plain send after the output is an aux send.
    bool pastOut = false;
    for (const Send& sd : m.sends) {
      if (sd.sidechain || sd.stageRoom) continue;
      if (pastOut) aux.insert(sd.to);
      pastOut = true;
    }
  }
  // Checkpoint 57 (N56-01): a send Clips keeps aside while the track plays into that return.
  for (const KeptSend& k : s.keptSends) {
    if (s.mixerTrack(k.mixer)) {
      aux.insert(k.to);
    }
  }
  std::vector<size_t> rows;
  for (size_t j = 1; j < s.mixer.size(); ++j)
    if (aux.count(s.mixer[j].id)) rows.push_back(j);
  return rows;
}

int auxSendIndex(const MixerTrack& m, ObjectId to) {
  bool pastOut = false;
  for (size_t i = 0; i < m.sends.size(); ++i) {
    const Send& sd = m.sends[i];
    if (sd.sidechain || sd.stageRoom) continue;
    if (pastOut && sd.to == to) return static_cast<int>(i);
    pastOut = true;
  }
  return -1;
}

const KeptSend* keptSend(const Song& s, ObjectId from, ObjectId to) {
  for (const KeptSend& k : s.keptSends) {
    if (k.mixer == from && k.to == to) {
      return &k;
    }
  }
  return nullptr;
}

bool setOutput(Song& s, ObjectId from, ObjectId to) {
  MixerTrack* m = s.mixerTrack(from);
  if (!m || from == s.masterId() || from == to || !canRoute(s, from, to) || outputOf(*m) == to) return false;
  // Checkpoint 57 (N56-01): first the send kept aside while the output played into the old return goes
  // back where it was, with its level (unless the track has a send there again).
  const ObjectId old = outputOf(*m);
  for (size_t i = 0; i < s.keptSends.size(); ++i) {
    const KeptSend k = s.keptSends[i];
    if (k.mixer != from || k.to != old) {
      continue;
    }
    s.keptSends.erase(s.keptSends.begin() + static_cast<std::ptrdiff_t>(i));
    if (old != kNoId && auxSendIndex(*m, old) < 0 && s.mixerTrack(old)) {
      Send sd{old, k.level, false};
      sd.preFader = k.preFader;
      const size_t at = std::min(static_cast<size_t>(std::max(k.index, 1)), m->sends.size());
      m->sends.insert(m->sends.begin() + static_cast<std::ptrdiff_t>(at), sd);
    }
    break;
  }
  bool moved = false;
  for (Send& sd : m->sends) {
    if (!sd.sidechain && !sd.stageRoom) {
      sd.to = to;  // the output (its level and pre/post as they were)
      moved = true;
      break;
    }
  }
  if (!moved) {
    m->sends.insert(m->sends.begin(), Send{to, 1.0f, false});
  }
  // Then an aux send to the new output steps aside: it would play the track into the return twice. Its
  // level is kept (Song::keptSends), as the reference greys the send out.
  if (const int k = auxSendIndex(*m, to); k >= 0) {
    const Send sd = m->sends[static_cast<size_t>(k)];
    KeptSend ks;
    ks.mixer = from, ks.to = to, ks.level = sd.level, ks.preFader = sd.preFader, ks.index = k;
    for (size_t i = 0; i < s.keptSends.size(); ++i) {
      if (s.keptSends[i].mixer == from && s.keptSends[i].to == to) {
        s.keptSends.erase(s.keptSends.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    s.keptSends.push_back(ks);
    m->sends.erase(m->sends.begin() + k);
  }
  return true;
}

namespace {
// Checkpoint 58 (N57-01): the entry no longer matches the song: its track or return is gone, or the
// track's output is not that return any more (another layout, a collaborator or a file moved it).
bool keptStale(const Song& s, const KeptSend& k) {
  const MixerTrack* m = s.mixerTrack(k.mixer);
  return !m || !s.mixerTrack(k.to) || outputOf(*m) != k.to;
}
}  // namespace

bool keptSendsStale(const Song& s) {
  for (const KeptSend& k : s.keptSends) {
    if (keptStale(s, k)) {
      return true;
    }
  }
  return false;
}

bool restoreKeptSends(Song& s) {
  bool changed = false;
  for (size_t i = 0; i < s.keptSends.size();) {
    const KeptSend k = s.keptSends[i];
    if (!keptStale(s, k)) {
      ++i;
      continue;
    }
    s.keptSends.erase(s.keptSends.begin() + static_cast<std::ptrdiff_t>(i));
    changed = true;
    MixerTrack* m = s.mixerTrack(k.mixer);
    if (!m || k.mixer == s.masterId() || !s.mixerTrack(k.to) || outputOf(*m) == kNoId || auxSendIndex(*m, k.to) >= 0 ||
        !canRoute(s, k.mixer, k.to)) {
      continue;  // nothing to put back (or the track has a send there again: that one wins)
    }
    size_t outAt = 0;
    while (outAt < m->sends.size() && (m->sends[outAt].sidechain || m->sends[outAt].stageRoom)) {
      ++outAt;
    }
    // Back at its place, but always after the output (it is an aux send, never the output).
    const size_t at = std::min(std::max(static_cast<size_t>(std::max(k.index, 0)), outAt + 1), m->sends.size());
    Send sd{k.to, k.level, false};
    sd.preFader = k.preFader;
    m->sends.insert(m->sends.begin() + static_cast<std::ptrdiff_t>(at), sd);
  }
  return changed;
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
  // N52-05: only while no track is in use: tracks deleted down to two show two (as the reference),
  // with no grey "Track N" columns padding the grid.
  return std::min(last >= 0 ? last + 1 : kShownTracks, static_cast<int>(s.tracks.size()));
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

int routeClipSources(Song& s, int track) {
  const MixerTrack* m = modeOf(s, track) == TrackMode::Audio ? linkedMixer(s, track) : nullptr;
  if (!m) return 0;
  const ObjectId mix = m->id;
  std::set<ObjectId> mine, shared;
  for (const Clip& c : s.playlist)
    if (c.kind == ClipKind::Audio) (c.track == track ? mine : shared).insert(c.source);
  int n = 0;
  for (ObjectId id : mine)
    if (Channel* ch = s.channel(id); ch && !shared.count(id) && ch->mixerTrack != mix) {
      ch->mixerTrack = mix;
      ++n;
    }
  return n;
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

// N52-11: the same for an instrument track named after its instrument ("1 Analog Synth"): its number.
int placeNumberOf(const Song& s, const PlaylistTrack& t, std::string* kind) {
  if (const int n = placeNumber(t.name, kind)) return n;
  if (t.mode != TrackMode::Instrument) return 0;
  const Channel* c = s.channel(t.channel);
  const PluginInfo* info = c ? findPlugin(c->plugin.type) : nullptr;
  if (!info) return 0;
  const std::string& n = t.name;
  size_t i = 0;
  while (i < n.size() && n[i] >= '0' && n[i] <= '9') ++i;
  if (i == 0 || i > 3 || i >= n.size() || n[i] != ' ') {
    return 0;
  }
  // K-314 B-06: or after the channel ("19 A1"), as Clips names a track it makes for an instrument that came
  // without one (a collaborator's, or this computer's added by a path that made no track)
  const std::string rest = n.substr(i + 1);
  const int num = std::stoi(n.substr(0, i));
  if (rest != info->name && (rest != c->name || num > static_cast<int>(s.tracks.size()))) {
    return 0;  // ("808 Kick" is a name)
  }
  if (kind) {
    *kind = rest;
  }
  return num;
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
  // N47-09: their mixer tracks and instruments named after them follow (they are one thing in Clips;
  // Studio, Smooth and the routing lists show their names). Checkpoint 53: an instrument's own mixer
  // track too. Checkpoint 54: by the numbers Clips shows (lanes and gaps take none).
  renumberTrackNames(s);
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
    renumberTrackNames(s);  // checkpoint 54: its number as Clips counts (lanes and gaps take none)
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
  // N52-08: an empty MIDI track an audio file landed on is an audio track now, named as one.
  std::string kind;
  if (tr.name.empty() || tr.name == "Track " + std::to_string(t + 1) || (placeNumber(tr.name, &kind) > 0 && kind == "MIDI"))
    tr.name = std::to_string(t + 1) + " Audio";
  if (tr.color == PlaylistTrack{}.color) tr.color = kTrackCycle[(usedTrackCount(s) - 1 + kTrackCycleN) % kTrackCycleN];
  if (!makeAudioTrack(s, t, kNoId, nullptr)) return false;
  const ObjectId mix = s.tracks[static_cast<size_t>(t)].mixer;
  addReturnSends(s, mix);
  // N52-02: its clips play through it (their sources were on Main): its fader, devices and sends.
  routeClipSources(s, t);
  renumberTrackNames(s);  // checkpoint 54: its number as Clips counts (lanes and gaps take none)
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
  {  // N52-04: a dry run first, so a failure leaves no step to take back (this may run inside a caller's step)
    Song probe = d.song();
    Channel c;
    c.id = probe.allocateId();
    c.name = info ? info->name : pluginType;
    c.plugin.type = pluginType;
    c.mixerTrack = probe.masterId();
    probe.channels.push_back(c);
    if (!makeInstrumentTrack(probe, track, c.id, why)) return kNoId;
  }
  d.beginGroup("new instrument track");
  const ObjectId ch = d.addChannel(info ? info->name : pluginType, pluginType);
  const bool ok = ch != kNoId && makeInstrumentTrack(d, track, ch, why);
  d.endGroup();
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
  // Checkpoint 54 (N53-03, N53-04): the number Clips shows the track by: lanes and empty gap tracks
  // take none, so "3 Audio" follows "2 MIDI" whatever lies between.
  const std::vector<int> num = trackNumbers(s, static_cast<int>(s.tracks.size()));
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    std::string kind;
    PlaylistTrack& tr = s.tracks[static_cast<size_t>(i)];
    const int n = placeNumberOf(s, tr, &kind), want = num[static_cast<size_t>(i)];
    if (n <= 0 || want <= 0 || n == want) continue;
    const std::string was = tr.name;
    tr.name = std::to_string(want) + " " + kind;
    followTrackName(s, i, was);  // N51-11: its mixer track and instrument (and the instrument's own mixer track)
  }
  followAutomationNames(s);  // checkpoint 55 (N54-03): a copy's automation and lanes take the copy's name
}
}  // namespace

void renumberTrackNames(Song& s) { renumberPlaces(s); }

bool trackNumbersStale(const Song& s) {
  const std::vector<int> num = trackNumbers(s, static_cast<int>(s.tracks.size()));
  for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i) {
    const int n = placeNumberOf(s, s.tracks[static_cast<size_t>(i)], nullptr), want = num[static_cast<size_t>(i)];
    if (n > 0 && want > 0 && n != want) {
      return true;
    }
  }
  return false;
}

int followTrackName(Song& s, int track, const std::string& was) {
  if (!validTrack(s, track) || was.empty()) return 0;
  const PlaylistTrack& tr = s.tracks[static_cast<size_t>(track)];
  if (tr.name == was) return 0;
  const std::string now = tr.name;
  int n = 0;
  ObjectId mix = tr.mode == TrackMode::Audio ? tr.mixer : kNoId;
  if (tr.mode == TrackMode::Instrument)
    if (Channel* c = s.channel(tr.channel)) {
      if (c->name == was) c->name = tr.name, ++n;
      if (c->mixerTrack != s.masterId()) mix = c->mixerTrack;
    }
  if (MixerTrack* m = s.mixerTrack(mix); m && mix != s.masterId() && m->name == was) m->name = tr.name, ++n;
  // Checkpoint 55 (N54-03): the takes recorded on the track ("3 Audio rec", "3 Audio take 2"), named
  // after it when they were made, take its new name when only this track's clips play them.
  for (Channel& c : s.channels) {
    if (c.name.size() <= was.size() || c.name.compare(0, was.size(), was) != 0) {
      continue;
    }
    const std::string rest = c.name.substr(was.size());
    if (rest != " rec" && rest.rfind(" take ", 0) != 0) {
      continue;
    }
    bool here = false, elsewhere = false;
    for (const Clip& k : s.playlist) {
      if (k.kind == ClipKind::Audio && k.source == c.id) {
        (k.track == track ? here : elsewhere) = true;
      }
    }
    if (here && !elsewhere) {
      c.name = now + rest;
      ++n;
    }
  }
  n += followAutomationNames(s);
  return n;
}

int followAutomationNames(Song& s) {
  int n = 0;
  for (AutomationClip& ac : s.automations) {
    const std::string now = paramDisplayName(s, ParamId{ac.owner, ac.index});
    if (now == ac.name || now == "?") {
      continue;
    }
    // Only a name made from the control's ("<owner> › <control>"): the same control part, another owner.
    const size_t kw = ac.name.find(" \xE2\x80\xBA "), kn = now.find(" \xE2\x80\xBA ");
    if (kw == std::string::npos || kn == std::string::npos || ac.name.compare(kw, std::string::npos, now, kn, std::string::npos) != 0) {
      continue;
    }
    const std::string was = ac.name;
    ac.name = now;
    ++n;
    std::set<int> lanes;
    for (const Clip& c : s.playlist) {
      if (c.kind == ClipKind::Automation && c.source == ac.id) {
        lanes.insert(c.track);
      }
    }
    for (int t : lanes) {
      if (validTrack(s, t) && s.tracks[static_cast<size_t>(t)].name == was && isAutomationLane(s, t)) {
        s.tracks[static_cast<size_t>(t)].name = now;
        ++n;
      }
    }
  }
  return n;
}

int shownNumber(const Song& s, int t) {
  if (t < 0) {
    return 0;
  }
  const std::vector<int> nums = trackNumbers(s, t + 1);
  return t < static_cast<int>(nums.size()) && nums[static_cast<size_t>(t)] > 0 ? nums[static_cast<size_t>(t)] : t + 1;
}

std::vector<ObjectId> loopingShortSlotClips(const Song& s) {
  std::vector<ObjectId> v;
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  for (const Clip& c : s.playlist) {
    if (c.kind == ClipKind::Audio && inLiveArea(s, c) && !c.launchOnce && c.syncBpm <= 0 && c.warp.empty() && !c.tempoLock && c.length > 0 &&
        c.length < beat) {
      v.push_back(c.id);
    }
  }
  return v;
}

bool renameTrack(Document& d, int track, const std::string& name, const std::function<void(PlaylistTrack&)>& extra) {
  const Song& cur = d.song();
  if (!validTrack(cur, track)) return false;
  const std::string was = cur.tracks[static_cast<size_t>(track)].name;
  Song probe = cur;
  probe.tracks[static_cast<size_t>(track)].name = name;
  if (followTrackName(probe, track, was) == 0)
    return d.editPlaylistTrack(track, "rename playlist track", [&](PlaylistTrack& x) {
      x.name = name;
      if (extra) extra(x);
    });
  d.editSong("rename playlist track", [&](Song& s) {
    PlaylistTrack& x = s.tracks[static_cast<size_t>(track)];
    x.name = name;
    if (extra) extra(x);
    followTrackName(s, track, was);
  });
  return true;
}

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
  // N52-10: a lane left empty by that (its "<group> › Volume" track) goes with the group.
  std::set<int> emptied;
  releaseMixer(s, s.tracks[static_cast<size_t>(group)].mixer, &emptied);
  clearLinks(s, group);
  s.tracks[static_cast<size_t>(group)].groupSize = 0;  // (its members stay: only the group track goes)
  std::set<int> gone = emptyLanes(s, emptied);
  gone.insert(group);
  eraseTracks(s, gone);
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

// The mixer track is free again; what still played into it plays where it played (its output); sends to
// it go; its own automation goes (data and clips). `emptied` gets the tracks that held that automation.
void releaseMixer(Song& s, ObjectId gid, std::set<int>* emptied = nullptr) {
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
  for (const Clip& c : s.playlist)
    if (emptied && c.kind == ClipKind::Automation && autos.count(c.source)) emptied->insert(c.track);
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

// True when a clip of an arrangement other than the current one uses the channel / pattern.
bool usedElsewhere(const Song& s, ObjectId channel, ObjectId pattern) {
  for (size_t i = 0; i < s.arrangements.size(); ++i) {
    if (static_cast<int>(i) == s.currentArrangement) continue;
    for (const Clip& c : s.arrangements[i].playlist)
      if ((channel != kNoId && c.kind == ClipKind::Audio && c.source == channel) || (pattern != kNoId && c.kind == ClipKind::Pattern && c.pattern == pattern))
        return true;
  }
  return false;
}

// N52-05: a channel goes with everything that names it (as Document::removeChannel, on the song).
void eraseChannel(Song& s, ObjectId id) {
  const int idx = s.channelIndex(id);
  if (idx < 0) return;
  s.channels.erase(s.channels.begin() + idx);
  for (Pattern& p : s.patterns) {
    p.steps.erase(id);
    p.notes.erase(id);
    p.stepProps.erase(id);
    p.stepLoop.erase(id);
    p.lanes.erase(id);
    p.automation.erase(std::remove_if(p.automation.begin(), p.automation.end(), [id](const PatternAutomation& l) { return l.owner == id; }), p.automation.end());
  }
  s.modLinks.erase(std::remove_if(s.modLinks.begin(), s.modLinks.end(), [id](const ModLink& l) { return l.owner == id || (l.sourceOwner == id && l.sourceSlot < 0); }),
                   s.modLinks.end());
  s.links.erase(std::remove_if(s.links.begin(), s.links.end(), [id](const ControllerLink& l) { return l.owner == id && l.action == RemoteAction::None; }),
                s.links.end());
  for (Channel& ch : s.channels) {
    ch.layer.erase(std::remove(ch.layer.begin(), ch.layer.end(), id), ch.layer.end());
    ch.layerZones.erase(std::remove_if(ch.layerZones.begin(), ch.layerZones.end(), [id](const LayerZone& z) { return z.child == id; }), ch.layerZones.end());
  }
  std::set<ObjectId> autos;
  for (const AutomationClip& ac : s.automations)
    if (ac.owner == id) autos.insert(ac.id);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                  [&](const Clip& c) { return (c.kind == ClipKind::Audio && c.source == id) || (c.kind == ClipKind::Automation && autos.count(c.source) > 0); }),
                   s.playlist.end());
  s.automations.erase(std::remove_if(s.automations.begin(), s.automations.end(), [&](const AutomationClip& ac) { return autos.count(ac.id) > 0; }),
                      s.automations.end());
}

// Erases tracks (indexes in the current list): their clips go, the groups around them shrink.
void eraseTracks(Song& s, const std::set<int>& gone) {
  for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
    const int t = *it;
    if (t < 0 || t >= static_cast<int>(s.tracks.size())) continue;
    for (int g = 0; g < t; ++g)  // the groups around it are a track shorter
      if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; gs > 0 && t <= g + gs) --gs;
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [t](const Clip& c) { return c.track == t; }), s.playlist.end());
    for (Clip& c : s.playlist)
      if (c.track > t) --c.track;
    s.tracks.erase(s.tracks.begin() + t);
  }
}

// N52-10: the tracks that held only the automation just removed (a lane of a group's bus): nothing
// else is on them and they are no group, audio or instrument track, so they go too.
std::set<int> emptyLanes(const Song& s, const std::set<int>& emptied) {
  std::set<int> out;
  for (int t : emptied)
    if (t >= 0 && t < static_cast<int>(s.tracks.size()) && s.tracks[static_cast<size_t>(t)].mode == TrackMode::Unassigned &&
        s.tracks[static_cast<size_t>(t)].groupSize <= 0 && !trackHasClips(s, t))
      out.insert(t);
  return out;
}
}  // namespace

int deleteTracks(Song& s, const std::vector<int>& tracks, std::string* why) {
  const std::vector<int> del = withWholeGroups(s, tracks);
  if (del.empty()) return fail(why, "Select the tracks to delete."), 0;
  if (static_cast<int>(del.size()) >= static_cast<int>(s.tracks.size())) return fail(why, "The last track cannot be deleted."), 0;
  std::set<int> gone(del.begin(), del.end());
  const ObjectId master = s.masterId();
  // N52-05: what the tracks own goes with them, decided before any of them goes (a group is still a
  // group while its bus is freed): their mixer tracks (a group's bus, an audio track's, an instrument's
  // own), their instruments, and the channels and patterns only their clips play.
  std::vector<ObjectId> mixers;
  std::set<ObjectId> chans, pats;
  auto takeMixer = [&](ObjectId m) {
    if (m != kNoId && m != master && s.mixerTrack(m) && std::find(mixers.begin(), mixers.end(), m) == mixers.end()) mixers.push_back(m);
  };
  for (int t : del) {
    if (modeOf(s, t) == TrackMode::Audio) takeMixer(s.tracks[static_cast<size_t>(t)].mixer);
    if (const Channel* c = linkedChannel(s, t)) {
      chans.insert(c->id);
      if (!channelUsesMixer(s, c->mixerTrack, c->id) && !mixerLinked(s, c->mixerTrack)) takeMixer(c->mixerTrack);
    }
  }
  for (const Clip& c : s.playlist) {
    if (!gone.count(c.track)) continue;
    if (c.kind == ClipKind::Audio) chans.insert(c.source);
    if (c.kind == ClipKind::Pattern) pats.insert(c.pattern);
  }
  for (const Clip& c : s.playlist) {  // kept while a clip on another track plays them
    if (gone.count(c.track)) continue;
    if (c.kind == ClipKind::Audio) chans.erase(c.source);
    if (c.kind == ClipKind::Pattern) {
      pats.erase(c.pattern);
      if (const Pattern* p = s.pattern(c.pattern)) {
        for (const auto& kv : p->notes)
          if (!kv.second.empty()) chans.erase(kv.first);
        for (const auto& kv : p->steps) chans.erase(kv.first);
      }
    }
  }
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t)
    if (!gone.count(t) && s.tracks[static_cast<size_t>(t)].mode == TrackMode::Instrument) chans.erase(s.tracks[static_cast<size_t>(t)].channel);
  for (auto it = chans.begin(); it != chans.end();) it = usedElsewhere(s, *it, kNoId) || !s.channel(*it) ? chans.erase(it) : std::next(it);
  for (auto it = pats.begin(); it != pats.end();) it = usedElsewhere(s, kNoId, *it) ? pats.erase(it) : std::next(it);
  std::set<int> emptied;
  for (ObjectId m : mixers) releaseMixer(s, m, &emptied);
  for (ObjectId c : chans) eraseChannel(s, c);
  for (int t : emptyLanes(s, emptied)) gone.insert(t);
  const int count = static_cast<int>(gone.size());
  eraseTracks(s, gone);
  for (ObjectId pid : pats) {
    if (const int pi = s.patternIndex(pid); pi >= 0) s.patterns.erase(s.patterns.begin() + pi);
    for (PatternGroup& g : s.patternGroups) g.patterns.erase(std::remove(g.patterns.begin(), g.patterns.end(), pid), g.patterns.end());
  }
  s.patternGroups.erase(std::remove_if(s.patternGroups.begin(), s.patternGroups.end(), [](const PatternGroup& g) { return g.patterns.empty(); }),
                        s.patternGroups.end());
  renumberPlaces(s);
  return count;
}

namespace {
// N52-01: the copy's mixer track takes the original's devices, strip and sends (its name, colour and
// id stay; it is not armed). Sends are remapped to the copies afterwards.
void copyMixerContent(Song& s, ObjectId from, ObjectId to) {
  const MixerTrack* f = s.mixerTrack(from);
  MixerTrack* t = s.mixerTrack(to);
  if (!f || !t || from == to) return;
  MixerTrack k = *f;
  k.id = to;
  k.name = t->name;
  k.color = t->color;
  k.dock = t->dock;
  k.armed = false;
  k.solo = k.soloAlone = false;
  k.freeze = FreezeRecord{};
  k.hibernate = false;
  *t = k;
}

// A copy of a channel right after it (a new id; its mixer track `mixer`), named `name` when given.
ObjectId cloneChannel(Song& s, ObjectId from, ObjectId mixer, const std::string& name) {
  const int idx = s.channelIndex(from);
  if (idx < 0) return kNoId;
  Channel c = s.channels[static_cast<size_t>(idx)];
  c.id = s.allocateId();
  c.mixerTrack = mixer;
  c.freeze = FreezeRecord{};
  if (!name.empty()) c.name = name;
  s.channels.insert(s.channels.begin() + idx + 1, c);
  return c.id;
}
}  // namespace

bool isAutomationLane(const Song& s, int track) {
  if (!validTrack(s, track)) return false;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
  if (t.mode != TrackMode::Unassigned || t.groupSize > 0) return false;
  bool any = false;
  for (const Clip& c : s.playlist)
    if (c.track == track) {
      if (c.kind != ClipKind::Automation) return false;
      any = true;
    }
  return any;
}

std::vector<char> automationLanes(const Song& s, int count) {
  const int n = std::max(0, std::min(count, static_cast<int>(s.tracks.size())));
  std::vector<char> v(static_cast<size_t>(n), 0);  // 0 none yet, 1 automation only, 2 something else
  for (const Clip& c : s.playlist)
    if (c.track >= 0 && c.track < n) {
      char& x = v[static_cast<size_t>(c.track)];
      x = c.kind == ClipKind::Automation && x != 2 ? 1 : 2;
    }
  for (int t = 0; t < n; ++t) {
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    v[static_cast<size_t>(t)] = v[static_cast<size_t>(t)] == 1 && tr.mode == TrackMode::Unassigned && tr.groupSize <= 0;
  }
  return v;
}

int controlTrack(const Song& s, ObjectId owner) {
  if (owner == kNoId) return -1;
  const int used = usedTrackCount(s);
  for (int t = 0; t < used; ++t) {
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    if (tr.channel == owner && tr.mode == TrackMode::Instrument) return t;
    if (const MixerTrack* pm = playedMixer(s, t); pm && pm->id == owner && pm->id != s.masterId()) return t;
  }
  return -1;
}

int laneOwner(const Song& s, int lane) {
  if (!isAutomationLane(s, lane)) return -1;
  for (const Clip& c : s.playlist)
    if (c.track == lane && c.kind == ClipKind::Automation)
      if (const AutomationClip* ac = s.automation(c.source)) {
        const int t = controlTrack(s, ac->owner);
        if (t >= 0 && t != lane) return t;
      }
  return -1;
}

namespace {
// The lanes of `t` in the block of lanes starting at `from` (lanes of other tracks there end it).
void lanesFrom(const Song& s, int t, int from, std::set<int>& out) {
  const int used = usedTrackCount(s);
  for (int k = from; k < used && isAutomationLane(s, k) && laneOwner(s, k) == t; ++k) out.insert(k);
}
}  // namespace

std::vector<int> withLanes(const Song& s, const std::vector<int>& tracks) {
  std::set<int> out(tracks.begin(), tracks.end());
  for (int t : tracks) {
    if (!validTrack(s, t)) continue;
    lanesFrom(s, t, t + 1, out);
    if (isGroupTrack(s, t)) lanesFrom(s, t, t + s.tracks[static_cast<size_t>(t)].groupSize + 1, out);  // (a lane made before checkpoint 54)
  }
  return std::vector<int>(out.begin(), out.end());
}

int pastLanes(const Song& s, int before) {
  const int used = usedTrackCount(s);
  while (before > 0 && before < used && isAutomationLane(s, before) && laneOwner(s, before) >= 0 && laneOwner(s, before) < before) ++before;
  return before;
}

std::vector<char> gapTracks(const Song& s, int count) {
  const int n = std::max(0, std::min(count, static_cast<int>(s.tracks.size())));
  std::vector<char> v(static_cast<size_t>(n), 0);
  std::vector<char> clips(static_cast<size_t>(n), 0);
  for (const Clip& c : s.playlist)
    if (c.track >= 0 && c.track < n) clips[static_cast<size_t>(c.track)] = 1;
  auto empty = [&](int t) {
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    return tr.mode == TrackMode::Unassigned && tr.name.empty() && tr.groupSize <= 0 && !clips[static_cast<size_t>(t)];
  };
  bool later = false;  // a track in use after this one
  for (int t = n - 1; t >= 0; --t) {
    if (!empty(t)) later = true;
    else if (later) v[static_cast<size_t>(t)] = 1;
  }
  return v;
}

std::vector<int> trackNumbers(const Song& s, int count) {
  const int n = std::max(0, std::min(count, static_cast<int>(s.tracks.size())));
  const std::vector<char> lanes = automationLanes(s, n), gaps = gapTracks(s, n);
  std::vector<int> v(static_cast<size_t>(n), 0);
  int k = 0;
  // K-314 B-03: a track shown on this computer only is numbered after the shared ones, so every
  // collaborator numbers the shared tracks alike.
  for (int pass = 0; pass < 2; ++pass) {
    for (int t = 0; t < n; ++t) {
      if (s.tracks[static_cast<size_t>(t)].shownHere != (pass == 1)) {
        continue;
      }
      if (!lanes[static_cast<size_t>(t)] && !gaps[static_cast<size_t>(t)]) {
        v[static_cast<size_t>(t)] = ++k;
      }
    }
  }
  return v;
}

void addReturnSendsTo(Song& s, ObjectId mixer) { addReturnSends(s, mixer); }

bool missingReturnSends(const Song& s, ObjectId mixer) {
  const MixerTrack* m = s.mixerTrack(mixer);
  if (!m || mixer == s.masterId()) return false;
  for (size_t r : returnTracks(s))
    if (r < s.mixer.size() && s.mixer[r].id != mixer &&
        std::none_of(m->sends.begin(), m->sends.end(), [&](const Send& x) { return x.to == s.mixer[r].id; }))
      return true;
  return false;
}

int addAutomationLane(Song& s, ObjectId owner, const std::string& name) {
  const int used = usedTrackCount(s);
  const int under = controlTrack(s, owner);  // the track the control belongs to
  int at = used;
  std::vector<int> grow;  // N53-03: groups ending with `under`: the lane under it joins them
  if (under >= 0) {
    at = under + 1;  // checkpoint 54: right under its track (a group's under its title, inside the group)
    while (at < used && isAutomationLane(s, at) && laneOwner(s, at) == under) ++at;  // after the track's other lanes
    for (int g = 0; g < under; ++g)
      if (const int gs = s.tracks[static_cast<size_t>(g)].groupSize; gs > 0 && isGroupTrack(s, g) && at == g + gs + 1) grow.push_back(g);
  }
  if (!insertTrackAt(s, at)) return -1;
  for (int g : grow) ++s.tracks[static_cast<size_t>(g)].groupSize;
  s.tracks[static_cast<size_t>(at)].name = name;
  if (under >= 0) s.tracks[static_cast<size_t>(at)].color = s.tracks[static_cast<size_t>(under)].color;
  return at;
}

int duplicateTracks(Song& s, const std::vector<int>& tracks, std::string* why, bool withContent) {
  // N53-03: a track's lanes are copied with it (K-351: not without the content, where they would be empty)
  const std::vector<int> src = withWholeGroups(s, withContent ? withLanes(s, tracks) : tracks);
  if (src.empty()) return fail(why, "Select the tracks to duplicate."), -1;
  const int first = src.front(), last = src.back(), n = static_cast<int>(src.size());
  if (last - first + 1 != n) return fail(why, "Duplicate tracks next to each other."), -1;
  if (static_cast<int>(s.tracks.size()) + n > kMaxPlaylistTracks) return fail(why, "There is no room for more tracks."), -1;
  const ObjectId master = s.masterId();
  const int home = groupTrackOf(s, first);  // the group the copies go in (the originals')
  // N52-01: the copy is a track of its own, as the reference's: its own mixer track (devices, fader,
  // pan and sends copied), its own instrument, its own clip sources and clips; nothing plays through
  // the original any more. Each original's mixer track and channel, and its mode:
  std::vector<PlaylistTrack> copies;
  std::vector<int> sizes;
  std::vector<TrackMode> modes;
  std::vector<ObjectId> origMix, origChan;
  for (int t : src) {
    PlaylistTrack k = s.tracks[static_cast<size_t>(t)];
    sizes.push_back(isGroupTrack(s, t) ? k.groupSize : 0);
    modes.push_back(modeOf(s, t));
    const Channel* lc = linkedChannel(s, t);
    origChan.push_back(lc ? lc->id : kNoId);
    ObjectId om = kNoId;
    if (modes.back() == TrackMode::Audio) om = k.mixer;
    else if (lc && lc->mixerTrack != master && s.mixerTrack(lc->mixerTrack) && !channelUsesMixer(s, lc->mixerTrack, lc->id)) om = lc->mixerTrack;
    origMix.push_back(om);
    k.groupSize = 0;
    copies.push_back(k);
  }
  int at = last + 1;
  if (!withContent) {  // K-351: the copies go after the originals' lanes, which stay under their tracks
    const std::vector<int> full = withWholeGroups(s, withLanes(s, tracks));
    if (!full.empty() && full.back() - full.front() + 1 == static_cast<int>(full.size())) at = std::max(at, full.back() + 1);
  }
  for (int i = 0; i < n; ++i) {
    if (!insertTrackAt(s, at + i)) return fail(why, "There is no room for more tracks."), -1;
    PlaylistTrack& k = s.tracks[static_cast<size_t>(at + i)];
    k = copies[static_cast<size_t>(i)];
    k.mode = TrackMode::Unassigned;  // linked below to mixer tracks and channels of its own
    k.mixer = k.channel = kNoId;
    k.laneShown = k.laneSet = false;
  }
  for (int g = home; g >= 0; g = groupTrackOf(s, g))  // the copies stay in the originals' group
    if (int& gs = s.tracks[static_cast<size_t>(g)].groupSize; g + gs < at + n - 1) gs = at + n - 1 - g;
  for (int i = 0; i < n; ++i) s.tracks[static_cast<size_t>(at + i)].groupSize = sizes[static_cast<size_t>(i)];
  std::map<ObjectId, ObjectId> idMap;  // an original's mixer track / channel -> the copy's
  std::vector<ObjectId> newMixers;
  // Outer tracks first, so each copy's group has its mixer track when the copy is routed into it.
  for (int i = 0; i < n; ++i) {
    const size_t u = static_cast<size_t>(i);
    const int t = at + i;
    if (modes[u] == TrackMode::Audio) {
      if (!makeAudioTrack(s, t, kNoId, why)) return -1;
      const ObjectId nm = s.tracks[static_cast<size_t>(t)].mixer;
      copyMixerContent(s, origMix[u], nm);
      idMap[origMix[u]] = nm;
      newMixers.push_back(nm);
    } else if (modes[u] == TrackMode::Instrument && s.channel(origChan[u])) {
      ObjectId nm = master;
      if (origMix[u] != kNoId) {
        nm = freeMixer(s);
        if (nm == kNoId) nm = newInsert(s);
        if (nm == kNoId) return fail(why, "There is no free mixer track for the copy."), -1;
        if (MixerTrack* m = s.mixerTrack(nm)) m->name = s.tracks[static_cast<size_t>(t)].name, m->color = s.tracks[static_cast<size_t>(t)].color;
        copyMixerContent(s, origMix[u], nm);
        idMap[origMix[u]] = nm;
        newMixers.push_back(nm);
      }
      const ObjectId nc = cloneChannel(s, origChan[u], nm, s.tracks[static_cast<size_t>(t)].name);
      idMap[origChan[u]] = nc;
      PlaylistTrack& k = s.tracks[static_cast<size_t>(t)];
      k.mode = TrackMode::Instrument;
      k.channel = nc;
      k.mixer = kNoId;
    }
  }
  // The copies' sends: to a copied group (or return) the copy's, else where the original's went.
  for (ObjectId id : newMixers)
    if (MixerTrack* m = s.mixerTrack(id)) {
      for (Send& sd : m->sends)
        if (auto it = idMap.find(sd.to); it != idMap.end()) sd.to = it->second;
      m->sends.erase(std::remove_if(m->sends.begin(), m->sends.end(), [id](const Send& x) { return x.to == id; }), m->sends.end());
      if (m->sends.empty() || outputOf(*m) == kNoId) m->sends.insert(m->sends.begin(), Send{master, 1.0f, false});
    }
  // The clips: an audio clip plays a copy of its source (through the copy's mixer track), a MIDI clip
  // a copy of its pattern (the notes on the copy's instrument), automation of a copied control the copy's.
  std::map<ObjectId, ObjectId> patMap, autoMap;
  std::vector<Clip> clips;
  for (const Clip& c : s.playlist)
    if (withContent && c.track >= first && c.track <= last) {
      Clip k = c;
      k.track = c.track + n;
      k.takeGroup = kNoId, k.take = 0;
      clips.push_back(k);
    }
  for (Clip& k : clips) {
    const size_t u = static_cast<size_t>(k.track - at);
    if (k.kind == ClipKind::Audio && s.channel(k.source)) {
      auto it = idMap.find(k.source);
      if (it == idMap.end()) {
        const Channel* oc = s.channel(k.source);
        ObjectId mix = oc->mixerTrack;
        if (modes[u] == TrackMode::Audio) mix = s.tracks[static_cast<size_t>(k.track)].mixer;  // (N52-02: also a source that played to Main)
        else if (auto mt = idMap.find(mix); mt != idMap.end()) mix = mt->second;
        it = idMap.emplace(k.source, cloneChannel(s, k.source, mix, "")).first;
      }
      if (it->second != kNoId) k.source = it->second;
    } else if (k.kind == ClipKind::Pattern && s.pattern(k.pattern)) {
      auto it = patMap.find(k.pattern);
      if (it == patMap.end()) {
        Pattern p = *s.pattern(k.pattern);
        p.id = s.allocateId();
        auto remap = [&](auto& mp) {
          std::remove_reference_t<decltype(mp)> out;
          for (auto& kv : mp) {
            auto m = idMap.find(kv.first);
            out[m != idMap.end() ? m->second : kv.first] = kv.second;
          }
          mp = std::move(out);
        };
        remap(p.steps);
        remap(p.notes);
        remap(p.stepProps);
        remap(p.stepLoop);
        remap(p.lanes);
        for (PatternAutomation& pa : p.automation)
          if (auto m = idMap.find(pa.owner); m != idMap.end()) pa.owner = m->second;
        s.patterns.push_back(p);
        it = patMap.emplace(k.pattern, p.id).first;
      }
      k.pattern = it->second;
    } else if (k.kind == ClipKind::Automation) {
      const AutomationClip* ac = s.automation(k.source);
      if (ac && idMap.count(ac->owner)) {
        auto it = autoMap.find(k.source);
        if (it == autoMap.end()) {
          AutomationClip na = *ac;
          na.id = s.allocateId();
          na.owner = idMap[ac->owner];
          s.automations.push_back(na);
          it = autoMap.emplace(k.source, na.id).first;
        }
        k.source = it->second;
      }
    }
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

int duplicateTracks(Document& d, const std::vector<int>& tracks, std::string* why, bool withContent) {
  int out = -1;
  runEdit(d, withContent ? "duplicate tracks" : "duplicate tracks without notes",
          [&](Song& s) { return (out = duplicateTracks(s, tracks, why, withContent)) >= 0; });
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

ObjectId recordArrangementAutomation(Song& s, ObjectId owner, uint32_t index, const std::string& name, std::vector<AutoPoint> pts,
                                     double tolerance) {
  if (pts.empty()) return kNoId;
  const Tick areaEnd = std::max<Tick>(0, s.live.areaEnd);
  std::sort(pts.begin(), pts.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  for (AutoPoint& p : pts) p.pos = std::max(p.pos, areaEnd);  // (never into the clip grid's storage)
  const Tick from = pts.front().pos, to = pts.back().pos;
  // The Arrangement clip of this control the take starts in: the take joins it.
  for (const Clip& c : s.playlist) {
    if (c.kind != ClipKind::Automation || c.muted || c.start < areaEnd) continue;
    const AutomationClip* ac = s.automation(c.source);
    if (!ac || ac->owner != owner || ac->index != index) continue;
    if (from < c.start || from >= c.start + s.clipLength(c)) continue;
    const Tick shift = c.offset - c.start;
    const double r = ac->max - ac->min;
    autoedit::Points rec;
    for (AutoPoint p : pts) {
      p.pos += shift;
      p.value = static_cast<float>(std::fabs(r) < 1e-9 ? 0.0 : std::clamp((p.value - ac->min) / r, 0.0, 1.0));
      rec.push_back(p);
    }
    const ObjectId id = ac->id, cid = c.id;
    AutomationClip* w = s.automation(id);
    // Checkpoint 60 (N59-02): the clip's curve before the take stays as it was up to the tick before
    // the take's first move (a hold point), instead of ramping from its last point into the take.
    Tick mergeFrom = from + shift;
    if (!w->points.empty() && w->points.front().pos < mergeFrom && mergeFrom > 0) {
      rec.insert(rec.begin(), AutoPoint{mergeFrom - 1, static_cast<float>(autoedit::valueAt(w->points, static_cast<double>(mergeFrom - 1))), 0, AutoShape::Hold});
      mergeFrom -= 1;
    }
    autoedit::mergeRecorded(w->points, rec, mergeFrom, to + shift, tolerance);
    // A take running past the clip's end makes the clip longer (up to the take's end, on a beat).
    if (Clip* k = s.clip(cid); k && to >= k->start + s.clipLength(*k)) {
      const MeterMap mm(s);
      const Tick end = mm.beatStartAt(to) == to ? to + std::max<Tick>(1, s.beatTicks()) : mm.nextBeat(to);
      k->length = std::max(s.clipLength(*k), end - k->start);
    }
    return id;
  }
  // A new clip from the beat of the take's first point, to the beat after its last.
  const MeterMap mm(s);
  const Tick start = std::max(areaEnd, mm.beatStartAt(from));
  const Tick end = std::max(mm.nextBeat(start), mm.beatStartAt(to) == to ? to + std::max<Tick>(1, s.beatTicks()) : mm.nextBeat(to));
  autoedit::Points rec;
  for (AutoPoint p : pts) {
    p.pos -= start;
    rec.push_back(p);
  }
  autoedit::Points points;
  autoedit::mergeRecorded(points, rec, from - start, to - start, tolerance);
  // The control's lane: one already holding this control's automation, else a new one under its track.
  int lane = -1;
  for (const Clip& c : s.playlist) {
    if (c.kind != ClipKind::Automation || c.start < areaEnd || !isAutomationLane(s, c.track)) continue;
    if (const AutomationClip* ac = s.automation(c.source); ac && ac->owner == owner && ac->index == index) {
      bool busy = false;
      for (const Clip& o : s.playlist) busy = busy || (o.track == c.track && o.start < end && o.start + s.clipLength(o) > start);
      if (!busy) {
        lane = c.track;
        break;
      }
    }
  }
  if (lane < 0) lane = addAutomationLane(s, owner, name);
  if (lane < 0) return kNoId;
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.name = name;
  ac.owner = owner;
  ac.index = index;
  ac.points = std::move(points);
  Clip k;
  k.id = s.allocateId();
  k.kind = ClipKind::Automation;
  k.source = ac.id;
  k.track = lane;
  k.start = start;
  k.length = end - start;
  const ObjectId id = ac.id;
  s.automations.push_back(std::move(ac));
  s.playlist.push_back(k);
  return id;
}

namespace {
void trimScenes(Song& s) {
  while (!s.live.scenes.empty() && s.live.scenes.back().empty()) {
    s.live.scenes.pop_back();
  }
}
// grid2-1118: the song-time marks after the live area move with the arrangement (the loop brace and the
// Project End), so the arrangement keeps its loop when a scene is inserted or deleted.
void shiftAfterArea(Song& s, Tick from, Tick shift) {
  if (shift == 0) {
    return;
  }
  if (s.cycleEnd > s.cycleStart && s.cycleStart >= from) {
    s.cycleStart = std::max<Tick>(0, s.cycleStart + shift);
    s.cycleEnd = std::max<Tick>(0, s.cycleEnd + shift);
  }
  if (s.projectEnd > 0 && s.projectEnd >= from) {
    s.projectEnd = std::max<Tick>(0, s.projectEnd + shift);
  }
}
// grid2-1118: the tracks' removed stop buttons stay with their scenes: from scene `k` on they move by `d`
// (a deleted scene's go with it).
void shiftNoStop(Song& s, int k, int d, int erase = -1) {
  for (PlaylistTrack& t : s.tracks) {
    std::vector<int> v;
    for (int x : t.noStop) {
      if (x == erase) {
        continue;
      }
      v.push_back(x >= k ? x + d : x);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    t.noStop = std::move(v);
  }
}
}  // namespace

Tick sceneTicksOf(const Song& s) {
  if (s.live.sceneLen > 0) {
    return s.live.sceneLen;
  }
  return std::max<Tick>(1, s.barTicks()) * kSceneBars;
}

int sceneOf(const Song& s, Tick start) { return start < 0 ? -1 : static_cast<int>(start / sceneTicksOf(s)); }

int sceneCount(const Song& s) {
  const Tick st = sceneTicksOf(s);
  return s.live.areaEnd > 0 ? static_cast<int>((s.live.areaEnd + st - 1) / st) : 0;
}

bool usesGridV10(const Song& s) {
  auto uses = [](const std::vector<Clip>& pl, const std::vector<PlaylistTrack>& tr, const LiveSettings& lv) {
    if (lv.areaEnd > 0 || lv.sceneLen > 0) {
      return true;
    }
    for (const SceneInfo& sc : lv.scenes) {
      if (sc.tempo > 0 || sc.sigNum > 0 || !sc.follow.isDefault()) {
        return true;
      }
    }
    for (const PlaylistTrack& t : tr) {
      if (!t.noStop.empty() || static_cast<int>(t.launch) >= kLaunchGridsBasic || static_cast<int>(t.syncStart) >= kLaunchGridsBasic) {
        return true;
      }
    }
    for (const Clip& c : pl) {
      if (!c.launchSet.isDefault()) {
        return true;
      }
    }
    return false;
  };
  if (uses(s.playlist, s.tracks, s.live)) {
    return true;
  }
  for (const Arrangement& a : s.arrangements) {
    if (uses(a.playlist, a.tracks, a.live)) {
      return true;
    }
  }
  return false;
}

int splitSharedSlots(Song& s) {
  if (s.live.areaEnd <= 0 || s.playlist.empty()) {
    return 0;
  }
  const Tick st = sceneTicksOf(s);
  int moved = 0;
  for (int k = 0; static_cast<Tick>(k) * st < s.live.areaEnd; ++k) {
    const Tick a0 = static_cast<Tick>(k) * st, a1 = a0 + st;
    // Per track, the clips starting in this scene (playlist indices), in time order (the first launched).
    std::map<int, std::vector<size_t>> per;
    for (size_t i = 0; i < s.playlist.size(); ++i) {
      const Clip& c = s.playlist[i];
      if (c.start >= a0 && c.start < a1 && c.start < s.live.areaEnd) {
        per[c.track].push_back(i);
      }
    }
    size_t extra = 0;
    for (auto& [t, v] : per) {
      std::stable_sort(v.begin(), v.end(), [&](size_t x, size_t y) {
        const Clip& cx = s.playlist[x];
        const Clip& cy = s.playlist[y];
        return cx.start != cy.start ? cx.start < cy.start : cx.id < cy.id;
      });
      extra = std::max(extra, v.size() - 1);
    }
    if (extra == 0) {
      continue;
    }
    std::vector<std::pair<ObjectId, int>> moves;  // (clip, rows down)
    for (const auto& [t, v] : per) {
      for (size_t j = 1; j < v.size(); ++j) {
        moves.emplace_back(s.playlist[v[j]].id, static_cast<int>(j));
      }
    }
    for (size_t j = 0; j < extra; ++j) {
      insertScene(s, k, st);  // an empty scene under scene k (each further one goes under it)
    }
    for (const auto& [id, rows] : moves) {
      if (Clip* c = s.clip(id)) {
        c->start += static_cast<Tick>(rows) * st;  // into its own slot, at the same place in the scene
        ++moved;
      }
    }
    k += static_cast<int>(extra);
  }
  return moved;
}

bool hasStopButton(const Song& s, int track, int scene) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) {
    return true;
  }
  const std::vector<int>& v = s.tracks[static_cast<size_t>(track)].noStop;
  return !std::binary_search(v.begin(), v.end(), scene);
}

void setStopButton(Song& s, int track, int scene, bool on) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size()) || scene < 0) {
    return;
  }
  std::vector<int>& v = s.tracks[static_cast<size_t>(track)].noStop;
  const auto it = std::lower_bound(v.begin(), v.end(), scene);
  const bool has = it != v.end() && *it == scene;
  if (on && has) {
    v.erase(it);
  } else if (!on && !has) {
    v.insert(it, scene);
  }
}

namespace {
// The number before " BPM" / "BPM" in a scene name (0: none).
double tempoInName(const std::string& n) {
  std::string low;
  for (char c : n) {
    low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  for (size_t p = low.find("bpm"); p != std::string::npos; p = low.find("bpm", p + 1)) {
    size_t e = p;
    while (e > 0 && low[e - 1] == ' ') {
      --e;
    }
    size_t b = e;
    while (b > 0 && ((low[b - 1] >= '0' && low[b - 1] <= '9') || low[b - 1] == '.')) {
      --b;
    }
    if (b == e) {
      continue;
    }
    const double v = std::atof(low.substr(b, e - b).c_str());
    if (v >= 10 && v <= 999) {
      return v;
    }
  }
  return 0;
}
// "3/4" in a scene name (a numerator 1..32 and a power-of-two denominator), as a word of its own.
bool signatureInName(const std::string& n, int& num, int& den) {
  for (size_t p = n.find('/'); p != std::string::npos; p = n.find('/', p + 1)) {
    size_t b = p, e = p + 1;
    while (b > 0 && n[b - 1] >= '0' && n[b - 1] <= '9') {
      --b;
    }
    while (e < n.size() && n[e] >= '0' && n[e] <= '9') {
      ++e;
    }
    if (b == p || e == p + 1 || (b > 0 && n[b - 1] != ' ') || (e < n.size() && n[e] != ' ')) {
      continue;
    }
    const int a = std::atoi(n.substr(b, p - b).c_str()), d = std::atoi(n.substr(p + 1, e - p - 1).c_str());
    if (a >= 1 && a <= 32 && (d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32)) {
      num = a, den = d;
      return true;
    }
  }
  return false;
}
}  // namespace

double sceneTempo(const SceneInfo& sc) { return sc.tempo > 0 ? sc.tempo : tempoInName(sc.name); }

bool sceneSignature(const SceneInfo& sc, int& num, int& den) {
  if (sc.sigNum > 0 && sc.sigDen > 0) {
    num = sc.sigNum, den = sc.sigDen;
    return true;
  }
  return signatureInName(sc.name, num, den);
}

void setSceneSettings(Song& s, int k, const SceneInfo& info) {
  if (k < 0) {
    return;
  }
  if (static_cast<int>(s.live.scenes.size()) <= k) {
    s.live.scenes.resize(static_cast<size_t>(k) + 1);
  }
  SceneInfo& sc = s.live.scenes[static_cast<size_t>(k)];
  sc.tempo = info.tempo;
  sc.sigNum = info.sigNum;
  sc.sigDen = info.sigDen;
  sc.follow = info.follow;
  trimScenes(s);
}

void insertScene(Song& s, int k, Tick len) {
  if (k < -1 || len <= 0) {
    return;
  }
  const Tick from = s.live.areaEnd, at = static_cast<Tick>(k + 1) * len;
  const Tick newEnd = std::max(from, at) + len, shift = newEnd - from;
  for (Clip& c : s.playlist) {
    if (c.start >= from) {
      c.start += shift;
    } else if (c.start >= at) {
      c.start += len;
    }
  }
  for (Marker& m : s.markers) {
    if (m.pos >= from) {
      m.pos += shift;
    }
  }
  shiftAfterArea(s, from, shift);  // grid2-1118
  shiftNoStop(s, k + 1, 1);
  s.live.areaEnd = newEnd;
  if (static_cast<int>(s.live.scenes.size()) > k + 1) {
    s.live.scenes.insert(s.live.scenes.begin() + (k + 1), SceneInfo{});
  }
  trimScenes(s);
}

bool deleteScene(Song& s, int k, Tick len) {
  const Tick from = s.live.areaEnd, a0 = static_cast<Tick>(k) * len, a1 = a0 + len;
  if (k < 0 || len <= 0 || a0 >= from) {
    return false;
  }
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.start >= a0 && c.start < a1 && c.start < from; }),
                   s.playlist.end());
  const Tick newEnd = std::max<Tick>(a0, from - len), shift = from - newEnd;
  for (Clip& c : s.playlist) {
    if (c.start >= from) {
      c.start -= shift;
    } else if (c.start >= a1) {
      c.start -= len;
    }
  }
  for (Marker& m : s.markers) {
    if (m.pos >= from) {
      m.pos -= shift;
    }
  }
  shiftAfterArea(s, from, -shift);  // grid2-1118
  shiftNoStop(s, k + 1, -1, k);
  s.live.areaEnd = newEnd;
  if (static_cast<int>(s.live.scenes.size()) > k) {
    s.live.scenes.erase(s.live.scenes.begin() + k);
  }
  trimScenes(s);
  return true;
}

bool duplicateScene(Song& s, int k, Tick len) {
  const Tick from = s.live.areaEnd, a0 = static_cast<Tick>(k) * len, a1 = a0 + len;
  if (k < 0 || len <= 0) {
    return false;
  }
  if (a0 >= from) {
    // Checkpoint 60 (N59-06): a scene past the stored ones holds no clips: its copy is its name and
    // colour in a new row under it (the later rows' names move down); the clip storage stays as it is.
    if (k >= static_cast<int>(s.live.scenes.size())) {
      return false;  // (an unnamed empty row with nothing after it: the copy is the same empty rows)
    }
    const SceneInfo info = s.live.scenes[static_cast<size_t>(k)];
    s.live.scenes.insert(s.live.scenes.begin() + (k + 1), info);
    trimScenes(s);
    return true;
  }
  std::vector<Clip> copies;
  for (const Clip& c : s.playlist) {
    if (c.start >= a0 && c.start < a1 && c.start < from) {
      copies.push_back(c);
    }
  }
  const SceneInfo info = k < static_cast<int>(s.live.scenes.size()) ? s.live.scenes[static_cast<size_t>(k)] : SceneInfo{};
  insertScene(s, k, len);
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {  // grid2-1118: the copy's stop buttons as the scene's
    if (!hasStopButton(s, t, k)) {
      setStopButton(s, t, k + 1, false);
    }
  }
  for (Clip c : copies) {
    c.id = s.allocateId();
    c.start += len;
    c.takeGroup = kNoId, c.take = 0;
    s.playlist.push_back(c);
  }
  if (!info.empty()) {
    if (static_cast<int>(s.live.scenes.size()) <= k + 1) {
      s.live.scenes.resize(static_cast<size_t>(k) + 2);
    }
    s.live.scenes[static_cast<size_t>(k) + 1] = info;
  }
  trimScenes(s);
  return true;
}

void setSceneName(Song& s, int k, const std::string& name) {
  if (k < 0) {
    return;
  }
  if (static_cast<int>(s.live.scenes.size()) <= k) {
    s.live.scenes.resize(static_cast<size_t>(k) + 1);
  }
  s.live.scenes[static_cast<size_t>(k)].name = name;
  trimScenes(s);
}

void setSceneColor(Song& s, int k, Rgb color) {
  if (k < 0) {
    return;
  }
  if (static_cast<int>(s.live.scenes.size()) <= k) {
    s.live.scenes.resize(static_cast<size_t>(k) + 1);
  }
  s.live.scenes[static_cast<size_t>(k)].color = color & 0xFFFFFFu;
  trimScenes(s);
}

const SceneInfo* sceneInfo(const Song& s, int k) {
  if (k < 0 || k >= static_cast<int>(s.live.scenes.size()) || s.live.scenes[static_cast<size_t>(k)].empty()) {
    return nullptr;
  }
  return &s.live.scenes[static_cast<size_t>(k)];
}

}  // namespace gl::trackmodes
