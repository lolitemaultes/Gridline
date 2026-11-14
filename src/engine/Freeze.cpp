// Gridline - K-87: freezing and hibernating channels and mixer tracks (see Freeze.h).
#include "Freeze.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include "../core/Json.h"
#include "../core/MixerOps.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../core/Sha256.h"
#include "Export.h"
#include "NoteFx.h"
#include "Render.h"

namespace fs = std::filesystem;

namespace gl::freeze {

namespace {

constexpr const char* kSilent = "gl.sampler";  // what an unloaded channel holds: no sample, no sound

bool isSendEffect(const PluginState& p) {
  if (p.empty()) return false;
  if (p.type == "gl.send" || p.type == "gl.bussend" || p.type == "gl.busreturn") return true;
  const PluginInfo* pi = findPlugin(p.type);
  return pi && !pi->sendParams.empty();
}

// Mixer track a channel's main output plays into (the master for kNoId / a missing track).
ObjectId mainTarget(const Song& s, const Channel& c) { return s.mixerTrack(c.mixerTrack) ? c.mixerTrack : s.masterId(); }

bool hasAuxOuts(const Channel& c) {
  for (ObjectId t : c.auxOuts)
    if (t != kNoId) return true;
  return false;
}

// A channel's plugin MIDI output reaches another channel.
bool midiFeedsOthers(const Song& s, const Channel& c) {
  if (c.midiOutPort < 0) return false;
  for (const auto& o : s.channels)
    if (o.id != c.id && o.midiInPort == c.midiOutPort) return true;
  return false;
}

bool noteFxSendsElsewhere(const Channel& c) {
  for (const auto& nf : c.noteFx)
    for (ObjectId t : noteFxTargets(nf.data))
      if (t != c.id && t != kNoId) return true;
  return false;
}

// Some enabled link takes its value from this channel's instrument / one of this track's slots.
bool linkSource(const Song& s, ObjectId owner, bool mixer) {
  for (const auto& l : s.modLinks)
    if (l.enabled && l.sourceOwner == owner && (mixer ? l.sourceSlot >= 0 : l.sourceSlot < 0)) return true;
  return false;
}

// Mixer tracks a track's signal goes on to: its sends (sidechains too) and its send effects.
std::set<ObjectId> outputsOf(const Song& s, const MixerTrack& m) {
  std::set<ObjectId> out;
  for (const auto& sd : m.sends)
    if (s.mixerTrack(sd.to)) out.insert(sd.to);
  return out;
}

bool hasSendEffect(const MixerTrack& m) {
  for (const auto& sl : m.slots)
    if (isSendEffect(sl.plugin)) return true;
  return false;
}

PluginState silentInstrument() {
  PluginState p = defaultPluginState(kSilent);
  p.data.clear();
  p.ins.clear();
  return p;
}

// Channels whose notes reach `seed` (layer parents, note processors sending to it), and the
// sources of links that drive what is kept: all of them must keep playing in a render.
std::set<ObjectId> noteSources(const Song& s, std::set<ObjectId> keep) {
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& c : s.channels) {
      if (keep.count(c.id)) continue;
      bool feeds = false;
      for (ObjectId k : c.layer) feeds = feeds || keep.count(k);
      for (const auto& nf : c.noteFx)
        for (ObjectId t : noteFxTargets(nf.data)) feeds = feeds || keep.count(t);
      if (c.midiOutPort >= 0)
        for (const auto& o : s.channels) feeds = feeds || (keep.count(o.id) && o.midiInPort == c.midiOutPort);
      if (feeds) grew = keep.insert(c.id).second || grew;
    }
  }
  return keep;
}

// The parts of a render song that do not depend on what is kept go: other channels hold a silent
// instrument (their notes stay only when they cut a kept channel), other tracks lose their
// effects, automation of anything else goes.
void reduce(Song& x, const std::set<ObjectId>& keepChannels, const std::set<ObjectId>& keepMixers) {
  std::set<int> cutters;  // cut groups a kept channel listens to
  for (const auto& c : x.channels)
    if (keepChannels.count(c.id) && c.cutBy > 0) cutters.insert(c.cutBy);
  std::set<ObjectId> dropNotes, dropAudio;
  for (auto& c : x.channels) {
    c.freeze = {};
    c.hibernate = false;
    if (keepChannels.count(c.id)) continue;
    c.plugin = silentInstrument();
    c.noteFx.clear();
    c.layer.clear();
    c.muted = true;
    dropAudio.insert(c.id);
    if (!(c.cutGroup > 0 && cutters.count(c.cutGroup))) dropNotes.insert(c.id);
  }
  for (auto& p : x.patterns) {
    for (ObjectId id : dropNotes) {
      p.notes.erase(id);
      p.steps.erase(id);
      p.stepProps.erase(id);
      p.stepLoop.erase(id);
    }
  }
  const ObjectId master = x.masterId();
  for (auto& m : x.mixer) {
    m.freeze = {};
    m.hibernate = false;
    m.solo = m.soloAlone = false;
    m.armed = false;
    m.input = -1;
    if (m.id == master || keepMixers.count(m.id)) continue;
    m.slots.assign(kEffectSlots, EffectSlot{});
    m.muted = true;
  }
  auto keepOwner = [&](ObjectId o) { return o == kSongId || keepChannels.count(o) || keepMixers.count(o); };
  std::set<ObjectId> goneAutos;
  x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(),
                                     [&](const AutomationClip& a) {
                                       if (keepOwner(a.owner)) return false;
                                       goneAutos.insert(a.id);
                                       return true;
                                     }),
                      x.automations.end());
  x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                  [&](const Clip& c) {
                                    if (c.kind == ClipKind::Automation) return goneAutos.count(c.source) > 0;
                                    if (c.kind == ClipKind::Audio) return dropAudio.count(c.source) > 0;
                                    return false;
                                  }),
                   x.playlist.end());
  for (auto& p : x.patterns)
    p.automation.erase(std::remove_if(p.automation.begin(), p.automation.end(), [&](const PatternAutomation& a) { return !keepOwner(a.owner); }),
                       p.automation.end());
  x.modLinks.erase(std::remove_if(x.modLinks.begin(), x.modLinks.end(), [&](const ModLink& l) { return !keepOwner(l.owner) || l.owner == master; }),
                   x.modLinks.end());
  x.links.clear();  // hardware controls play no part in a render
}

void neutralStrip(MixerTrack& m) {
  const MixerTrack d;
  m.volume = d.volume;
  m.pan = 0;
  m.stereoSep = 0;
  m.depth = 0;
  m.focus = 0;
  m.invertPolarity = m.swapLR = false;
  m.eqGain = d.eqGain;
  m.muted = false;
}

}  // namespace

const FreezeRecord* record(const Song& s, Unit u) {
  if (u.mixer) {
    const MixerTrack* m = s.mixerTrack(u.id);
    return m ? &m->freeze : nullptr;
  }
  const Channel* c = s.channel(u.id);
  return c ? &c->freeze : nullptr;
}

bool frozen(const Song& s, Unit u) {
  const FreezeRecord* r = record(s, u);
  return r && r->frozen();
}

bool hibernating(const Song& s, Unit u) {
  if (u.mixer) {
    const MixerTrack* m = s.mixerTrack(u.id);
    return m && m->hibernate;
  }
  const Channel* c = s.channel(u.id);
  return c && c->hibernate;
}

std::string unitName(const Song& s, Unit u) {
  if (!u.mixer) {
    const Channel* c = s.channel(u.id);
    return c ? c->name : std::string();
  }
  const int i = s.mixerIndex(u.id);
  if (i < 0) return {};
  const MixerTrack& m = s.mixer[static_cast<size_t>(i)];
  if (!m.name.empty()) return m.name;
  return i == 0 ? std::string("Master") : "Insert " + std::to_string(i);
}

std::string whyNotFreeze(const Song& s, Unit u) {
  if (u.mixer) {
    const MixerTrack* m = s.mixerTrack(u.id);
    if (!m) return "The track is gone";
    if (u.id == s.masterId()) return "The master cannot be frozen";
    if (m->hasInput() || m->records()) return "It takes an audio input";
    if (m->disabled) return "The track is disabled";
    if (hasSendEffect(*m)) return "A send effect in its slots plays into other tracks";
    if (linkSource(s, m->id, true)) return "One of its effects drives a link";
    if (ObjectId t = coveringTrack(s, u); t != kNoId) return "It plays into a frozen track";
    for (const auto& c : s.channels)
      if (mainTarget(s, c) == u.id && hasAuxOuts(c)) return "\"" + c.name + "\" also plays into other tracks";
    if (renderEnd(s) <= 0) return "Nothing plays in the song";
    return {};
  }
  const Channel* c = s.channel(u.id);
  if (!c) return "The channel is gone";
  if (c->plugin.empty()) return "It has no instrument";
  if (c->plugin.type == "gl.midiout") return "It plays other devices";
  if (c->isLayer()) return "A layer plays other channels";
  if (midiFeedsOthers(s, *c)) return "Its MIDI output plays other channels";
  if (noteFxSendsElsewhere(*c)) return "A note effect sends notes to other channels";
  if (hasAuxOuts(*c)) return "It has extra outputs";
  if (linkSource(s, c->id, false)) return "It drives a link";
  if (ObjectId t = coveringTrack(s, u); t != kNoId) return "Its mixer track is frozen";
  if (renderEnd(s) <= 0) return "Nothing plays in the song";
  return {};
}

std::string whyNotHibernate(const Song& s, Unit u) {
  if (u.mixer) {
    const MixerTrack* m = s.mixerTrack(u.id);
    if (!m) return "The track is gone";
    if (u.id == s.masterId()) return "The master cannot hibernate";
    if (m->hasInput() || m->records()) return "It takes an audio input";
    if (hasSendEffect(*m)) return "A send effect in its slots plays into other tracks";
    if (linkSource(s, m->id, true)) return "One of its effects drives a link";
    return {};
  }
  const Channel* c = s.channel(u.id);
  if (!c) return "The channel is gone";
  if (c->plugin.empty()) return "It has no instrument";
  if (c->plugin.type == "gl.midiout") return "It plays other devices";
  if (midiFeedsOthers(s, *c)) return "Its MIDI output plays other channels";
  if (linkSource(s, c->id, false)) return "It drives a link";
  return {};
}

Cover covered(const Song& s, ObjectId track) {
  Cover cv;
  if (!s.mixerTrack(track) || track == s.masterId()) return cv;
  std::set<ObjectId> inside{track};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& m : s.mixer) {
      if (inside.count(m.id) || m.id == s.masterId()) continue;
      if (m.hasInput() || m.records() || hasSendEffect(m) || linkSource(s, m.id, true)) continue;
      const std::set<ObjectId> out = outputsOf(s, m);
      if (out.empty()) continue;  // plays nowhere: not part of this track's sound
      bool all = true;
      for (ObjectId o : out) all = all && inside.count(o);
      if (all) {
        inside.insert(m.id);
        cv.mixers.insert(m.id);
        grew = true;
      }
    }
  }
  for (const auto& c : s.channels) {
    if (!inside.count(mainTarget(s, c)) || c.isLayer() || c.plugin.type == "gl.midiout") continue;
    if (midiFeedsOthers(s, c) || noteFxSendsElsewhere(c) || linkSource(s, c.id, false)) continue;
    bool all = true;
    for (ObjectId t : c.auxOuts) all = all && (t == kNoId || inside.count(t));
    if (all) cv.channels.insert(c.id);
  }
  return cv;
}

ObjectId coveringTrack(const Song& s, Unit u) {
  for (const auto& m : s.mixer) {
    if (!m.freeze.frozen() || (u.mixer && m.id == u.id)) continue;
    const Cover cv = covered(s, m.id);
    if (u.mixer ? cv.mixers.count(u.id) > 0 : cv.channels.count(u.id) > 0) return m.id;
  }
  return kNoId;
}

Tick renderEnd(const Song& s) { return s.lengthTicks(); }

namespace {
// The unit's live strip: its automation and links act on the live strip, not in the render.
bool liveControl(Unit u, ObjectId owner, uint32_t index) {
  if (owner != u.id) return false;
  if (u.mixer) return index < kSlotBase;  // volume, pan, separation, mute, strip EQ, send levels
  return index == static_cast<uint32_t>(ChannelParam::Volume) || index == static_cast<uint32_t>(ChannelParam::Pan) ||
         index == static_cast<uint32_t>(ChannelParam::Mute);
}

void dropLiveControls(Song& x, Unit u) {
  std::set<ObjectId> gone;
  x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(),
                                     [&](const AutomationClip& a) {
                                       if (!liveControl(u, a.owner, a.index)) return false;
                                       gone.insert(a.id);
                                       return true;
                                     }),
                      x.automations.end());
  x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                  [&](const Clip& c) { return c.kind == ClipKind::Automation && gone.count(c.source) > 0; }),
                   x.playlist.end());
  for (auto& p : x.patterns)
    p.automation.erase(std::remove_if(p.automation.begin(), p.automation.end(),
                                      [&](const PatternAutomation& a) { return liveControl(u, a.owner, a.index); }),
                       p.automation.end());
  x.modLinks.erase(std::remove_if(x.modLinks.begin(), x.modLinks.end(), [&](const ModLink& l) { return liveControl(u, l.owner, l.index); }),
                   x.modLinks.end());
}
}  // namespace

Song renderSong(const Song& s, Unit u) {
  Song x = renderSongWithStrip(s, u);
  dropLiveControls(x, u);
  return x;
}

std::vector<ProgramBuilder::PreloadItem> wakeItems(const Song& s, Unit u) {
  std::vector<ProgramBuilder::PreloadItem> out;
  if (u.mixer) {
    if (const MixerTrack* m = s.mixerTrack(u.id))
      for (size_t k = 0; k < m->slots.size(); ++k)
        if (!m->slots[k].plugin.empty()) out.push_back({true, m->id, static_cast<int>(k), m->slots[k].plugin.type});
  } else if (const Channel* c = s.channel(u.id); c && !c->plugin.empty()) {
    out.push_back({false, c->id, -1, c->plugin.type});
  }
  return out;
}

Song renderSongWithStrip(const Song& s, Unit u) {
  const ObjectId master = s.masterId();
  if (u.mixer) {
    Song x = mixops::stemSong(s, u.id);
    std::set<ObjectId> keepMixers{u.id};
    for (const auto& m : x.mixer)
      if (m.id != master && !m.muted) keepMixers.insert(m.id);
    std::set<ObjectId> keepChannels;
    for (const auto& c : x.channels)
      if (!c.muted && keepMixers.count(mainTarget(x, c))) keepChannels.insert(c.id);
    keepChannels = noteSources(x, keepChannels);
    for (const auto& l : x.modLinks)
      if (keepChannels.count(l.owner) || keepMixers.count(l.owner)) {
        if (l.sourceSlot < 0) keepChannels.insert(l.sourceOwner);
        else keepMixers.insert(l.sourceOwner);
      }
    // stemSong mutes channels that play outside the stem; those kept for their notes stay muted.
    std::set<ObjectId> audible;
    for (const auto& c : x.channels)
      if (!c.muted) audible.insert(c.id);
    reduce(x, keepChannels, keepMixers);
    for (auto& c : x.channels)
      if (audible.count(c.id)) c.muted = false;
    for (auto& m : x.mixer)
      if (keepMixers.count(m.id) && m.id != master) m.muted = false;
    if (MixerTrack* f = x.mixerTrack(u.id)) {
      neutralStrip(*f);
      f->sends = {{master, 1.0f, false, false}};
      f->latencyOffset = 0;  // the manual latency stays live
    }
    if (!x.mixer.empty()) {
      MixerTrack& m = x.mixer[0];
      neutralStrip(m);
      m.slots.assign(kEffectSlots, EffectSlot{});
      m.disabled = false;
      m.latencyOffset = 0;
    }
    return x;
  }
  Song x = s;
  std::set<ObjectId> keep = noteSources(x, {u.id});
  std::set<ObjectId> keepMixers;
  for (const auto& l : x.modLinks)
    if (keep.count(l.owner)) {
      if (l.sourceSlot < 0) keep.insert(l.sourceOwner);
      else keepMixers.insert(l.sourceOwner);
    }
  reduce(x, keep, keepMixers);
  for (auto& c : x.channels) {
    if (c.id == u.id) {
      c.gain = 0.78f;  // unity on the channel fader law: volume and pan stay live
      c.pan = 0;
      c.muted = false;
      c.mixerTrack = master;
      c.auxOuts.clear();
    } else {
      c.muted = true;  // others play only for their notes (layers, note effects, links)
    }
  }
  for (auto& m : x.mixer) {
    if (m.id == master) {
      neutralStrip(m);
      m.slots.assign(kEffectSlots, EffectSlot{});
      m.disabled = false;
      m.latencyOffset = 0;
      m.sends.clear();
    } else {
      m.muted = true;
    }
  }
  return x;
}

std::string fingerprint(const Song& s, Unit u) {
  Song x = renderSong(s, u);
  {  // Silent parts (a placeholder channel without notes, a muted track without effects) and how
     // they are set play no part: an edit of an unrelated channel or track keeps the freeze.
    const ObjectId master = x.masterId();
    const PluginState silent = silentInstrument();
    auto hasNotes = [&](ObjectId id) {
      for (const auto& p : x.patterns)
        if (p.notes.count(id) || p.steps.count(id)) return true;
      return false;
    };
    x.channels.erase(std::remove_if(x.channels.begin(), x.channels.end(),
                                    [&](const Channel& c) { return c.id != u.id && c.muted && c.plugin == silent && !hasNotes(c.id); }),
                     x.channels.end());
    x.mixer.erase(std::remove_if(x.mixer.begin(), x.mixer.end(),
                                 [&](const MixerTrack& m) {
                                   if (m.id == master || m.id == u.id || !m.muted) return false;
                                   for (const auto& sl : m.slots)
                                     if (!sl.plugin.empty()) return false;
                                   return true;
                                 }),
                  x.mixer.end());
  }
  // Display state and bookkeeping play no part in the sound.
  x.info = SongInfo{};
  x.nextId = 0;
  x.arrangements.clear();
  x.currentArrangement = 0;
  x.channelGroups.clear();
  x.patternGroups.clear();
  x.trackReceipt.clear();
  x.flExportGridline.clear();
  x.flOrigin = FlOrigin{};
  x.groove.preset.clear();  // the swing preset's name (its timing and accents play)
  for (auto& c : x.channels) {
    c.name.clear();
    c.color = 0;
    c.icon = c.flIcon = 0;
    c.group = -1;
  }
  for (auto& p : x.patterns) {
    p.name.clear();
    p.color = 0;
  }
  for (auto& m : x.mixer) {
    m.name.clear();
    m.color = 0;
    m.icon = m.flIcon = 0;
    m.dock = 0;
    m.separator = m.locked = false;
    m.group.clear();
  }
  for (auto& t : x.tracks) {
    t.name.clear();
    t.color = 0;
    t.height = 1;
    t.folded = t.locked = false;
    t.icon = t.flIcon = 0;
  }
  for (auto& c : x.playlist) c.group = kNoId;
  for (auto& m : x.markers) m.name.clear();
  // File references by name: the same project on another computer (a shared project, an
  // unpacked zip) keeps its fingerprint.
  forEachRef(x, [](std::string& p, bool) { p = fs::path(p).filename().string(); });
  Json j = songToJson(x, false);
  // A third-party plugin's identity record lists the formats installed on this computer.
  std::function<void(Json&)> strip = [&](Json& v) {
    if (v.is(Json::Type::Object)) {
      v.object.erase("ident");
      for (auto& [k, c] : v.object) strip(c);
    } else if (v.is(Json::Type::Array)) {
      for (auto& c : v.array) strip(c);
    }
  };
  strip(j);
  const std::string text = (u.mixer ? "m:" : "c:") + jsonCompact(j);
  return sha256Hex(text.data(), text.size());
}

Plan makePlan(const Song& s, const std::set<Unit>& live, const std::set<Unit>& awake) {
  Plan p;
  for (const auto& m : s.mixer) {
    if (!m.freeze.frozen() || live.count(mixerUnit(m.id)) || m.id == s.masterId()) continue;
    p.mixerFiles[m.id] = m.freeze.render.data;
    p.unloadMixers.insert(m.id);
    const Cover cv = covered(s, m.id);
    p.unloadMixers.insert(cv.mixers.begin(), cv.mixers.end());
    p.unloadChannels.insert(cv.channels.begin(), cv.channels.end());
  }
  for (const auto& c : s.channels) {
    if (!c.freeze.frozen() || live.count(channelUnit(c.id)) || p.unloadChannels.count(c.id)) continue;
    p.channelFiles[c.id] = c.freeze.render.data;
    p.unloadChannels.insert(c.id);
  }
  for (const auto& c : s.channels)
    if (c.hibernate && !awake.count(channelUnit(c.id)) && !p.unloadChannels.count(c.id)) p.unloadChannels.insert(c.id);
  for (const auto& m : s.mixer)
    if (m.hibernate && m.id != s.masterId() && !awake.count(mixerUnit(m.id)) && !p.unloadMixers.count(m.id)) {
      p.unloadMixers.insert(m.id);
      p.asleepMixers.insert(m.id);
    }
  return p;
}

Song playView(const Song& s, const Plan& p) {
  Song x = s;
  if (p.empty()) return x;
  for (auto& c : x.channels)
    if (p.unloadChannels.count(c.id)) {
      c.plugin = silentInstrument();
      c.noteFx.clear();
    }
  for (auto& m : x.mixer) {
    if (p.unloadMixers.count(m.id)) m.slots.assign(kEffectSlots, EffectSlot{});
    if (p.asleepMixers.count(m.id)) m.muted = true;
    // A live track's send into a frozen track is in its render.
    if (!p.unloadMixers.count(m.id))
      m.sends.erase(std::remove_if(m.sends.begin(), m.sends.end(), [&](const Send& sd) { return p.mixerFiles.count(sd.to) > 0; }),
                    m.sends.end());
  }
  return x;
}

bool upToDate(const Song& s, Unit u) {
  const FreezeRecord* r = record(s, u);
  return r && r->frozen() && r->fingerprint == fingerprint(s, u);
}

Song exportView(const Song& s, FrozenAudio& out, std::set<Unit>* liveOut) {
  out = FrozenAudio{};
  std::set<Unit> live;
  auto consider = [&](Unit u, const FreezeRecord& r) {
    if (!r.frozen()) return;
    SamplePtr smp = upToDate(s, u) ? sampleCache().get(r.render.data) : nullptr;
    if (!smp || smp->frames() == 0) {
      live.insert(u);
      return;
    }
    (u.mixer ? out.mixers : out.channels)[u.id] = smp;
  };
  for (const auto& m : s.mixer) consider(mixerUnit(m.id), m.freeze);
  for (const auto& c : s.channels) consider(channelUnit(c.id), c.freeze);
  std::set<Unit> awake;  // a render wakes every hibernating unit
  for (const auto& c : s.channels)
    if (c.hibernate) awake.insert(channelUnit(c.id));
  for (const auto& m : s.mixer)
    if (m.hibernate) awake.insert(mixerUnit(m.id));
  const Plan p = makePlan(s, live, awake);
  for (auto it = out.channels.begin(); it != out.channels.end();)  // a channel inside a frozen track plays in its render
    it = p.channelFiles.count(it->first) ? std::next(it) : out.channels.erase(it);
  if (liveOut) *liveOut = live;
  return playView(s, p);
}

bool render(const Song& s, Unit u, int sampleRate, const std::string& dir, FreezeRecord& out, std::string* err,
            const RenderControl* ctl) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (std::string why = whyNotFreeze(s, u); !why.empty()) return fail(why);
  const std::string fp = fingerprint(s, u);
  std::string stem = safeFileName(unitName(s, u));
  if (stem.empty()) stem = u.mixer ? "Track" : "Channel";
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  // The name holds what the render is ("<name> <fingerprint>", " L<n>": its effects' latency in
  // samples) so the same sound is found again (undo, redo, a refreeze of the same content).
  const std::string base = stem + " " + fp.substr(0, 12);
  int latency = 0;
  fs::path file;
  for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().u8string();
    if (n.size() < base.size() + 4 || n.compare(0, base.size(), base) != 0 || n.compare(n.size() - 4, 4, ".wav") != 0) continue;
    const std::string rest = n.substr(base.size(), n.size() - 4 - base.size());
    if (rest.empty()) {
      file = it->path();
      break;
    }
    if (rest.size() > 2 && rest[0] == ' ' && rest[1] == 'L' && rest.find_first_not_of("0123456789", 2) == std::string::npos) {
      file = it->path();
      latency = std::atoi(rest.c_str() + 2);
      break;
    }
  }
  RenderJob job;
  job.sampleRate = sampleRate;
  job.scope = RenderScope::Range;
  job.start = 0;
  job.end = renderEnd(s);
  job.tail = TailMode::Leave;
  if (ctl) {
    job.progress = ctl->progress;
    job.cancel = ctl->cancel;
  }
  double seconds = 0;
  if (!file.empty() && fs::is_regular_file(file, ec)) {  // the same sound rendered before (undo, redo, refreeze)
    SamplePtr have = loadSample(file.u8string());
    if (have && static_cast<int>(have->sampleRate) == sampleRate) seconds = have->seconds();
  }
  if (seconds <= 0) {
    RenderResult r = renderAudio(renderSong(s, u), job);
    if (!r.error.empty()) return fail(r.error);
    if (r.cancelled) return fail("Cancelled");
    latency = 0;
    if (u.mixer)
      if (auto it = r.trackLatency.find(u.id); it != r.trackLatency.end()) latency = std::max(0, it->second);
    if (!file.empty()) fs::remove(file, ec);  // another rate
    file = fs::u8path(dir) / fs::u8path(base + (latency > 0 ? " L" + std::to_string(latency) : std::string()) + ".wav");
    const fs::path tmp = file.u8string() + ".part";
    std::string why;
    if (!writeWavFile(tmp.u8string(), r.audio, sampleRate, 32, {}, &why)) return fail(why.empty() ? "The file could not be written" : why);
    fs::rename(tmp, file, ec);
    if (ec) return fail("The file could not be written");
    seconds = static_cast<double>(r.audio.frames) / sampleRate;
  }
  out = FreezeRecord{};
  out.render.type = kSilent;
  out.render.data = file.u8string();
  out.fingerprint = fp;
  out.sampleRate = sampleRate;
  out.arrangement = s.currentArrangement;
  out.seconds = seconds;
  out.latency = latency;
  return true;
}

bool setRecord(Song& s, Unit u, const FreezeRecord& r) {
  if (!r.frozen()) return false;
  if (u.mixer) {
    MixerTrack* m = s.mixerTrack(u.id);
    if (!m) return false;
    m->freeze = r;
    m->hibernate = false;
    return true;
  }
  Channel* c = s.channel(u.id);
  if (!c) return false;
  c->freeze = r;
  c->hibernate = false;
  return true;
}

bool unfreeze(Song& s, Unit u) {
  FreezeRecord* r = nullptr;
  if (u.mixer) {
    if (MixerTrack* m = s.mixerTrack(u.id)) r = &m->freeze;
  } else if (Channel* c = s.channel(u.id)) {
    r = &c->freeze;
  }
  if (!r || !r->frozen()) return false;
  *r = FreezeRecord{};
  return true;
}

bool setHibernate(Song& s, Unit u, bool on) {
  if (on && !whyNotHibernate(s, u).empty()) return false;
  bool* h = nullptr;
  if (u.mixer) {
    if (MixerTrack* m = s.mixerTrack(u.id)) h = &m->hibernate;
  } else if (Channel* c = s.channel(u.id)) {
    h = &c->hibernate;
  }
  if (!h || *h == on) return false;
  *h = on;
  return true;
}

}  // namespace gl::freeze
